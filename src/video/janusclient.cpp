#include "janusclient.h"
#include "logging.h"

#include <QAbstractSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>

namespace glasshouse {

namespace {
// Janus defaults to a 60 s session-inactivity timeout. 30 s keepalive keeps
// a comfortable margin and matches what the PiKVM web UI ships.
constexpr int kKeepaliveMs       = 30'000;
constexpr int kInitialBackoffMs  = 1'000;
constexpr int kMaxBackoffMs      = 30'000;
// Total attempts (including the first) before we escalate via
// sessionFailed. With the backoff schedule above, that's ~1+2+4+8 = 15 s
// worst-case before main.cpp gets to re-authenticate and rebuild.
constexpr int kMaxAttempts       = 4;
}  // namespace

JanusClient::JanusClient(Options opts, QObject* parent)
    : QObject(parent), m_opts(std::move(opts)) {
    m_keepalive = std::make_unique<QTimer>(this);
    m_keepalive->setInterval(kKeepaliveMs);
    connect(m_keepalive.get(), &QTimer::timeout,
            this, &JanusClient::onKeepaliveTick);

    m_reconnectTimer = std::make_unique<QTimer>(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer.get(), &QTimer::timeout,
            this, &JanusClient::attemptReconnect);
}

JanusClient::~JanusClient() = default;

void JanusClient::open() {
    m_stopRequested      = false;
    m_attempt            = 0;
    m_reconnectBackoffMs = kInitialBackoffMs;
    resetPerAttemptState();
    openWebSocket();
}

void JanusClient::close() {
    m_stopRequested = true;
    m_keepalive->stop();
    m_reconnectTimer->stop();
    if (m_ws) m_ws->close();
}

void JanusClient::openWebSocket() {
    m_ws = std::make_unique<QWebSocket>();

    connect(m_ws.get(), &QWebSocket::connected,
            this,       &JanusClient::onWsConnected);
    connect(m_ws.get(), &QWebSocket::disconnected,
            this,       &JanusClient::onWsDisconnected);
    connect(m_ws.get(), &QWebSocket::textMessageReceived,
            this,       &JanusClient::onWsTextMessage);

    if (m_opts.insecure_tls) {
        connect(m_ws.get(), &QWebSocket::sslErrors,
                m_ws.get(), qOverload<>(&QWebSocket::ignoreSslErrors));
    }

    QNetworkRequest req(QUrl(
        QStringLiteral("wss://%1/janus/ws").arg(m_opts.host)));
    req.setRawHeader(QByteArrayLiteral("Cookie"),
                     m_opts.authCookieHeader.toLatin1());

    QWebSocketHandshakeOptions hs;
    hs.setSubprotocols({QStringLiteral("janus-protocol")});

    qCInfo(lcJanus) << m_opts.host << "opening Janus WS"
                    << "(attempt" << (m_attempt + 1) << "of" << kMaxAttempts << ")";
    m_ws->open(req, hs);
}

void JanusClient::resetPerAttemptState() {
    m_sessionId = 0;
    m_handleId  = 0;
    m_txnTag.clear();
    m_nextTxn   = 1;
}

void JanusClient::onWsConnected() {
    qCInfo(lcJanus) << m_opts.host << "Janus WS connected, subprotocol="
                    << m_ws->subprotocol();
    emit wsConnected();
    sendInfo();
}

void JanusClient::onWsDisconnected() {
    m_keepalive->stop();
    const QString reason = m_ws
        ? QStringLiteral("closeCode=%1 error=%2")
              .arg(static_cast<int>(m_ws->closeCode()))
              .arg(m_ws->errorString())
        : QStringLiteral("Janus WS closed");
    qCInfo(lcJanus) << m_opts.host << "Janus WS disconnected:" << reason;
    transientFail(reason);
}

void JanusClient::onWsTextMessage(const QString& msg) {
    qCDebug(lcJanus) << m_opts.host << "RX" << msg.size() << "chars:"
                     << msg.left(400);

    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(msg.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(lcJanus) << m_opts.host
                           << "malformed janus frame:" << err.errorString();
        return;
    }
    const auto obj  = doc.object();
    const auto kind = obj.value(QStringLiteral("janus")).toString();
    const auto txn  = obj.value(QStringLiteral("transaction")).toString();
    const auto tag  = m_txnTag.take(txn);

    if (kind == QLatin1String("server_info")) {
        handleServerInfo(obj);
    } else if (kind == QLatin1String("success")) {
        handleSuccess(obj, tag);
    } else if (kind == QLatin1String("ack")) {
        qCDebug(lcJanus) << m_opts.host << "ack for"
                         << (tag.isEmpty() ? txn : tag);
    } else if (kind == QLatin1String("event")) {
        handleEvent(obj);
    } else if (kind == QLatin1String("error")) {
        handleError(obj, tag);
    } else if (kind == QLatin1String("keepalive")) {
        // Server-side keepalive; no action.
    } else if (kind == QLatin1String("hangup")
            || kind == QLatin1String("detached")
            || kind == QLatin1String("timeout")) {
        qCInfo(lcJanus) << m_opts.host << "Janus session ended:" << kind;
        transientFail(kind);
    } else {
        qCDebug(lcJanus) << m_opts.host << "unhandled janus kind:" << kind;
    }
}

void JanusClient::onKeepaliveTick() {
    if (!m_ws || m_sessionId == 0
            || m_ws->state() != QAbstractSocket::ConnectedState) return;
    QJsonObject req;
    req.insert(QStringLiteral("janus"),       QStringLiteral("keepalive"));
    req.insert(QStringLiteral("session_id"),  m_sessionId);
    req.insert(QStringLiteral("transaction"),
               nextTxn(QStringLiteral("keepalive")));
    sendJanus(req);
}

QString JanusClient::nextTxn(const QString& tag) {
    const auto id = QStringLiteral("t%1").arg(m_nextTxn++);
    m_txnTag.insert(id, tag);
    return id;
}

void JanusClient::sendJanus(QJsonObject obj) {
    if (!m_ws || m_ws->state() != QAbstractSocket::ConnectedState) {
        qCWarning(lcJanus) << m_opts.host << "drop frame — WS not open";
        return;
    }
    m_ws->sendTextMessage(QString::fromUtf8(
        QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

void JanusClient::sendInfo() {
    QJsonObject req;
    req.insert(QStringLiteral("janus"),       QStringLiteral("info"));
    req.insert(QStringLiteral("transaction"), nextTxn(QStringLiteral("info")));
    sendJanus(req);
}

void JanusClient::sendCreate() {
    QJsonObject req;
    req.insert(QStringLiteral("janus"),       QStringLiteral("create"));
    req.insert(QStringLiteral("transaction"), nextTxn(QStringLiteral("create")));
    sendJanus(req);
}

void JanusClient::sendAttach() {
    QJsonObject req;
    req.insert(QStringLiteral("janus"),       QStringLiteral("attach"));
    req.insert(QStringLiteral("plugin"),      m_opts.pluginName);
    req.insert(QStringLiteral("session_id"),  m_sessionId);
    req.insert(QStringLiteral("transaction"), nextTxn(QStringLiteral("attach")));
    sendJanus(req);
}

void JanusClient::sendWatch() {
    QJsonObject body;
    body.insert(QStringLiteral("request"), QStringLiteral("watch"));
    QJsonObject req;
    req.insert(QStringLiteral("janus"),       QStringLiteral("message"));
    req.insert(QStringLiteral("body"),        body);
    req.insert(QStringLiteral("session_id"),  m_sessionId);
    req.insert(QStringLiteral("handle_id"),   m_handleId);
    req.insert(QStringLiteral("transaction"), nextTxn(QStringLiteral("watch")));
    sendJanus(req);
}

void JanusClient::sendAnswer(const QString& sdp) {
    if (m_sessionId == 0 || m_handleId == 0) {
        qCWarning(lcJanus) << m_opts.host
                           << "sendAnswer before session ready — dropped";
        return;
    }
    QJsonObject body;
    body.insert(QStringLiteral("request"), QStringLiteral("start"));
    QJsonObject jsep;
    jsep.insert(QStringLiteral("type"), QStringLiteral("answer"));
    jsep.insert(QStringLiteral("sdp"),  sdp);
    QJsonObject req;
    req.insert(QStringLiteral("janus"),       QStringLiteral("message"));
    req.insert(QStringLiteral("body"),        body);
    req.insert(QStringLiteral("jsep"),        jsep);
    req.insert(QStringLiteral("session_id"),  m_sessionId);
    req.insert(QStringLiteral("handle_id"),   m_handleId);
    req.insert(QStringLiteral("transaction"), nextTxn(QStringLiteral("start")));
    qCInfo(lcJanus) << m_opts.host << "sending SDP answer," << sdp.size() << "bytes";
    sendJanus(req);
}

void JanusClient::handleServerInfo(const QJsonObject& obj) {
    const auto plugins = obj.value(QStringLiteral("plugins")).toObject();
    qCInfo(lcJanus) << m_opts.host
                    << "Janus" << obj.value(QStringLiteral("name")).toString()
                    << "version"
                    << obj.value(QStringLiteral("version_string")).toString()
                    << "— plugins:" << plugins.keys();
    // Proceed regardless. If the configured pluginName isn't in the list,
    // `attach` will fail and handleError surfaces a clean message alongside
    // the plugin registry we just logged — enough to adjust --plugin.
    sendCreate();
}

void JanusClient::handleSuccess(const QJsonObject& obj, const QString& tag) {
    const auto data = obj.value(QStringLiteral("data")).toObject();
    const qint64 id = static_cast<qint64>(
        data.value(QStringLiteral("id")).toDouble());

    if (tag == QLatin1String("create")) {
        m_sessionId = id;
        qCInfo(lcJanus) << m_opts.host << "session id=" << m_sessionId;
        m_keepalive->start();
        sendAttach();
    } else if (tag == QLatin1String("attach")) {
        m_handleId = id;
        qCInfo(lcJanus) << m_opts.host
                        << "attached plugin=" << m_opts.pluginName
                        << "handle=" << m_handleId;
        emit sessionReady(m_sessionId, m_handleId);
        sendWatch();
    } else {
        qCDebug(lcJanus) << m_opts.host << "success (" << tag << ") id=" << id;
    }
}

void JanusClient::handleEvent(const QJsonObject& obj) {
    const auto plugindata = obj.value(QStringLiteral("plugindata")).toObject();
    if (!plugindata.isEmpty()) {
        qCDebug(lcJanus) << m_opts.host << "plugin event:"
                         << QJsonDocument(plugindata)
                                .toJson(QJsonDocument::Compact);
    }

    const auto jsep = obj.value(QStringLiteral("jsep")).toObject();
    if (jsep.isEmpty()) return;

    const auto type = jsep.value(QStringLiteral("type")).toString();
    const auto sdp  = jsep.value(QStringLiteral("sdp")).toString();
    if (type != QLatin1String("offer")) {
        qCDebug(lcJanus) << m_opts.host << "non-offer jsep type:" << type;
        return;
    }
    qCInfo(lcJanus) << m_opts.host << "SDP offer received,"
                    << sdp.size() << "bytes";
    // Signalling reached its happy state. From here, any drop is a
    // "stable session interrupted" rather than a startup failure, so
    // reset the retry budget — the next reconnect cycle starts fresh.
    m_attempt            = 0;
    m_reconnectBackoffMs = kInitialBackoffMs;
    emit sdpOfferReceived(sdp);
}

void JanusClient::handleError(const QJsonObject& obj, const QString& tag) {
    const auto err = obj.value(QStringLiteral("error")).toObject();
    const QString reason = QStringLiteral("Janus error %1 during %2: %3")
        .arg(err.value(QStringLiteral("code")).toInt())
        .arg(tag.isEmpty() ? QStringLiteral("?") : tag)
        .arg(err.value(QStringLiteral("reason")).toString());
    qCWarning(lcJanus) << m_opts.host << reason;
    transientFail(reason);
}

void JanusClient::transientFail(const QString& reason) {
    if (m_stopRequested) return;
    m_keepalive->stop();
    if (m_ws) {
        // Disconnect signal slots so the impending close doesn't
        // recursively trigger this path again.
        m_ws->disconnect(this);
        m_ws->close();
    }

    if (m_attempt + 1 >= kMaxAttempts) {
        terminalFail(reason);
        return;
    }
    scheduleReconnect();
}

void JanusClient::terminalFail(const QString& reason) {
    qCWarning(lcJanus) << m_opts.host
                       << "giving up Janus connection after"
                       << (m_attempt + 1) << "attempts:" << reason;
    emit sessionFailed(reason);
}

void JanusClient::scheduleReconnect() {
    qCInfo(lcJanus) << m_opts.host << "reconnect in"
                    << m_reconnectBackoffMs << "ms";
    m_reconnectTimer->start(m_reconnectBackoffMs);
    m_reconnectBackoffMs = std::min(m_reconnectBackoffMs * 2, kMaxBackoffMs);
}

void JanusClient::attemptReconnect() {
    if (m_stopRequested) return;
    ++m_attempt;
    resetPerAttemptState();
    openWebSocket();
}

}  // namespace glasshouse
