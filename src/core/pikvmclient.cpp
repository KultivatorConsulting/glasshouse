#include "pikvmclient.h"
#include "logging.h"

#include <QAbstractSocket>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMessageAuthenticationCode>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSslError>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWebSocket>

#include <algorithm>

namespace glasshouse {

namespace {

constexpr int kPingIntervalMs       = 15'000;
constexpr int kInitialBackoffMs     = 1'000;
constexpr int kMaxBackoffMs         = 30'000;

// RFC 4648 base32 decode. Skips `=` padding and whitespace. Returns empty on
// any invalid character.
QByteArray base32Decode(const QString& s) {
    QByteArray out;
    quint32 buf = 0;
    int bits = 0;
    for (QChar ch : s) {
        if (ch == QLatin1Char('=') || ch.isSpace()) continue;
        const QChar u = ch.toUpper();
        int v = -1;
        if (u >= QLatin1Char('A') && u <= QLatin1Char('Z')) v = u.unicode() - 'A';
        else if (u >= QLatin1Char('2') && u <= QLatin1Char('7')) v = u.unicode() - '2' + 26;
        else return {};
        buf = (buf << 5) | static_cast<quint32>(v);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out.append(static_cast<char>((buf >> bits) & 0xffu));
        }
    }
    return out;
}

// RFC 6238 TOTP, 6-digit, SHA-1, 30 s step.
QString generateTotp(const QString& base32Secret) {
    const QByteArray key = base32Decode(base32Secret);
    if (key.isEmpty()) return {};

    quint64 counter = static_cast<quint64>(QDateTime::currentSecsSinceEpoch()) / 30;
    QByteArray counterBytes(8, '\0');
    for (int i = 7; i >= 0; --i) {
        counterBytes[i] = static_cast<char>(counter & 0xffu);
        counter >>= 8;
    }

    QMessageAuthenticationCode mac(QCryptographicHash::Sha1);
    mac.setKey(key);
    mac.addData(counterBytes);
    const QByteArray digest = mac.result();
    if (digest.size() < 20) return {};

    const int offset = digest.at(digest.size() - 1) & 0x0f;
    const quint32 code = (
        (static_cast<quint32>(static_cast<quint8>(digest[offset    ])) & 0x7fu) << 24 |
         static_cast<quint32>(static_cast<quint8>(digest[offset + 1]))         << 16 |
         static_cast<quint32>(static_cast<quint8>(digest[offset + 2]))         <<  8 |
         static_cast<quint32>(static_cast<quint8>(digest[offset + 3]))
    ) % 1'000'000u;

    return QString::asprintf("%06u", code);
}

HidState parseHidState(const QJsonObject& ev) {
    HidState s;
    s.online = ev.value(QStringLiteral("online")).toBool();

    const auto mouse = ev.value(QStringLiteral("mouse")).toObject();
    s.mouse_online   = mouse.value(QStringLiteral("online")).toBool();
    s.mouse_absolute = mouse.value(QStringLiteral("absolute")).toBool();

    const auto outputs = mouse.value(QStringLiteral("outputs")).toObject();
    s.mouse_outputs_active = outputs.value(QStringLiteral("active")).toString();

    const auto kbd = ev.value(QStringLiteral("keyboard")).toObject();
    s.keyboard_online = kbd.value(QStringLiteral("online")).toBool();
    return s;
}

}  // namespace

PiKvmClient::PiKvmClient(Options opts, QObject* parent)
    : QObject(parent), m_opts(std::move(opts)) {
    m_nam = std::make_unique<QNetworkAccessManager>(this);
    m_nam->setCookieJar(new QNetworkCookieJar(m_nam.get()));

    m_pingTimer = std::make_unique<QTimer>(this);
    m_pingTimer->setInterval(kPingIntervalMs);
    connect(m_pingTimer.get(), &QTimer::timeout, this, &PiKvmClient::onPingTick);

    m_reconnectTimer = std::make_unique<QTimer>(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer.get(), &QTimer::timeout,
            this, &PiKvmClient::attemptReconnect);
}

PiKvmClient::~PiKvmClient() = default;

void PiKvmClient::start() {
    m_stopped = false;
    authenticate();
}

void PiKvmClient::stop() {
    m_stopped = true;
    m_pingTimer->stop();
    m_reconnectTimer->stop();
    if (m_ws) m_ws->close();
}

bool PiKvmClient::isConnected() const {
    return m_ws && m_ws->state() == QAbstractSocket::ConnectedState;
}

QString PiKvmClient::currentPassword() const {
    if (m_opts.totp_secret.isEmpty()) return m_opts.password;
    const QString code = generateTotp(m_opts.totp_secret);
    if (code.isEmpty()) {
        qCWarning(lcPikvm) << m_opts.host
            << "TOTP generation failed (bad base32?); sending password without it.";
        return m_opts.password;
    }
    return m_opts.password + code;
}

void PiKvmClient::authenticate() {
    qCInfo(lcPikvm) << m_opts.host << "authenticating as" << m_opts.user;

    QUrl url(QStringLiteral("https://%1/api/auth/login").arg(m_opts.host));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));

    QUrlQuery form;
    form.addQueryItem(QStringLiteral("user"),   m_opts.user);
    form.addQueryItem(QStringLiteral("passwd"), currentPassword());

    QNetworkReply* reply = m_nam->post(
        req, form.toString(QUrl::FullyEncoded).toUtf8());

    if (m_opts.insecure_tls) {
        connect(reply, &QNetworkReply::sslErrors,
                reply, qOverload<>(&QNetworkReply::ignoreSslErrors));
    }
    connect(reply, &QNetworkReply::finished,
            this,  &PiKvmClient::onLoginFinished);
}

void PiKvmClient::onLoginFinished() {
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    reply->deleteLater();

    if (m_stopped) return;

    if (reply->error() != QNetworkReply::NoError) {
        emit errorOccurred(QStringLiteral("login failed: %1").arg(reply->errorString()));
        scheduleReconnect();
        return;
    }

    // Extract the auth_token cookie so we can present it on the WS upgrade;
    // QWebSocket does not share a cookie jar with QNetworkAccessManager.
    const auto cookies = m_nam->cookieJar()->cookiesForUrl(
        QUrl(QStringLiteral("https://%1/").arg(m_opts.host)));
    m_authCookie.clear();
    for (const auto& c : cookies) {
        if (c.name() == "auth_token") {
            m_authCookie = QString::fromLatin1(
                c.toRawForm(QNetworkCookie::NameAndValueOnly));
            break;
        }
    }
    if (m_authCookie.isEmpty()) {
        emit errorOccurred(QStringLiteral("login: no auth_token cookie set"));
        scheduleReconnect();
        return;
    }

    emit authenticated();
    openWebSocket();
}

void PiKvmClient::openWebSocket() {
    // Detach the slots from any previous instance *before* it gets
    // destroyed by the assignment below. QWebSocket emits disconnected
    // from its close path during destruction; without this disconnect
    // that final signal would route into onWsDisconnected and trigger a
    // spurious scheduleReconnect() — which on top of an already-running
    // re-auth cascade produces a doubled "authenticating" / pipeline
    // start per external start() call.
    if (m_ws) m_ws->disconnect(this);
    m_ws = std::make_unique<QWebSocket>();

    connect(m_ws.get(), &QWebSocket::connected,
            this,       &PiKvmClient::onWsConnected);
    connect(m_ws.get(), &QWebSocket::disconnected,
            this,       &PiKvmClient::onWsDisconnected);
    connect(m_ws.get(), &QWebSocket::textMessageReceived,
            this,       &PiKvmClient::onWsTextMessage);
    connect(m_ws.get(), &QWebSocket::binaryMessageReceived,
            this,       &PiKvmClient::onWsBinaryMessage);

    if (m_opts.insecure_tls) {
        connect(m_ws.get(), &QWebSocket::sslErrors,
                m_ws.get(), qOverload<>(&QWebSocket::ignoreSslErrors));
    }

    QNetworkRequest req(QUrl(
        QStringLiteral("wss://%1/api/ws?stream=1").arg(m_opts.host)));
    req.setRawHeader(QByteArrayLiteral("Cookie"), m_authCookie.toLatin1());

    m_initialStateSeen = false;
    m_ws->open(req);
}

void PiKvmClient::onWsConnected() {
    qCInfo(lcWs) << m_opts.host << "WS connected";
    m_reconnectBackoffMs = kInitialBackoffMs;
    m_pingTimer->start();
    emit connected();
}

void PiKvmClient::onWsDisconnected() {
    const QString reason = m_ws
        ? QStringLiteral("closeCode=%1 error=%2")
              .arg(static_cast<int>(m_ws->closeCode()))
              .arg(m_ws->errorString())
        : QStringLiteral("WS closed");
    qCInfo(lcWs) << m_opts.host << "WS disconnected:" << reason;
    m_pingTimer->stop();
    emit disconnected(reason);
    if (!m_stopped) scheduleReconnect();
}

void PiKvmClient::onWsTextMessage(const QString& msg) {
    qCDebug(lcWs) << m_opts.host << "RX text" << msg.size() << "chars:"
                  << msg.left(200);

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(lcWs) << m_opts.host << "malformed state event:" << err.errorString();
        return;
    }
    const QJsonObject obj = doc.object();
    const QString     type = obj.value(QStringLiteral("event_type")).toString();
    const QJsonObject ev   = obj.value(QStringLiteral("event")).toObject();

    if (type == QLatin1String("pong")) {
        return;
    }

    if (type == QLatin1String("hid_state")) {
        m_hid = parseHidState(ev);
        if (m_initialStateSeen) emit hidStateChanged(m_hid);
    }

    // `loop` is the terminator of the initial state burst — every subsystem
    // snapshot precedes it. Verified empirically against kvmd 3.106 and 3.199
    // (see DESIGN.md §10.4).
    if (type == QLatin1String("loop") && !m_initialStateSeen) {
        m_initialStateSeen = true;
        emit initialStateReceived(m_hid);
    }

    emit rawStateEvent(type, ev);
}

void PiKvmClient::onWsBinaryMessage(const QByteArray& msg) {
    // kvmd sends state as TEXT frames; any binary traffic is unexpected.
    qCWarning(lcWs) << m_opts.host << "unexpected binary frame," << msg.size()
                    << "bytes, first:" << msg.left(32).toHex(' ');
}

void PiKvmClient::onPingTick() {
    if (!isConnected()) return;
    // kvmd-level keepalive: round-trips through the aiohttp JSON handler, proves
    // end-to-end liveness beyond the WS frame layer. Server replies with
    // {"event_type":"pong","event":{}}.
    m_ws->sendTextMessage(QStringLiteral(R"({"event_type":"ping","event":{}})"));
}

void PiKvmClient::scheduleReconnect() {
    if (m_stopped) return;
    qCInfo(lcPikvm) << m_opts.host
                    << "reconnect in" << m_reconnectBackoffMs << "ms";
    m_reconnectTimer->start(m_reconnectBackoffMs);
    m_reconnectBackoffMs = std::min(m_reconnectBackoffMs * 2, kMaxBackoffMs);
}

void PiKvmClient::attemptReconnect() {
    if (m_stopped) return;
    if (m_ws) m_ws.reset();
    authenticate();
}

void PiKvmClient::sendWsJson(const QString& type, const QJsonObject& event) {
    if (!isConnected()) {
        // Debug, not warning: a WS outage drops every coalesced mouse-move
        // (~120/s) and key here — the disconnect itself is already logged
        // once via onWsDisconnected, so per-event drops would just flood.
        qCDebug(lcHid) << m_opts.host << "drop" << type << "— WS not connected";
        return;
    }
    QJsonObject frame;
    frame.insert(QStringLiteral("event_type"), type);
    frame.insert(QStringLiteral("event"),      event);
    m_ws->sendTextMessage(QString::fromUtf8(
        QJsonDocument(frame).toJson(QJsonDocument::Compact)));
}

void PiKvmClient::sendMouseMove(int to_x, int to_y) {
    QJsonObject to;
    to.insert(QStringLiteral("x"), std::clamp(to_x, -32768, 32767));
    to.insert(QStringLiteral("y"), std::clamp(to_y, -32768, 32767));
    QJsonObject event;
    event.insert(QStringLiteral("to"), to);
    sendWsJson(QStringLiteral("mouse_move"), event);
}

void PiKvmClient::sendMouseButton(MouseButton button, bool pressed) {
    QJsonObject event;
    event.insert(QStringLiteral("button"), mouseButtonToWire(button));
    event.insert(QStringLiteral("state"),  pressed);
    sendWsJson(QStringLiteral("mouse_button"), event);
}

void PiKvmClient::sendMouseWheel(int delta_x, int delta_y) {
    QJsonObject delta;
    delta.insert(QStringLiteral("x"), std::clamp(delta_x, -127, 127));
    delta.insert(QStringLiteral("y"), std::clamp(delta_y, -127, 127));
    QJsonObject event;
    event.insert(QStringLiteral("delta"), delta);
    sendWsJson(QStringLiteral("mouse_wheel"), event);
}

void PiKvmClient::sendKey(const QString& key, bool pressed, bool finish) {
    QJsonObject event;
    event.insert(QStringLiteral("key"),   key);
    event.insert(QStringLiteral("state"), pressed);
    if (finish) event.insert(QStringLiteral("finish"), true);
    sendWsJson(QStringLiteral("key"), event);
}

void PiKvmClient::sendShortcut(const QStringList& keys) {
    // No WS `shortcut` event type exists on the server. The reference UI
    // synthesises shortcuts by pressing keys in order and releasing in reverse.
    for (const auto& k : keys) sendKey(k, true);
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) sendKey(*it, false);
}

namespace {

// All MSD endpoints share the same auth + insecure-tls handling. This
// helper wires that up around an arbitrary POST.
QNetworkReply* postEmpty(QNetworkAccessManager* nam, const QUrl& url,
                         bool insecureTls) {
    QNetworkRequest req(url);
    QNetworkReply* reply = nam->post(req, QByteArray());
    if (insecureTls) {
        QObject::connect(reply, &QNetworkReply::sslErrors,
                         reply, qOverload<>(&QNetworkReply::ignoreSslErrors));
    }
    return reply;
}

}  // namespace

void PiKvmClient::msdSetConnected(bool connected) {
    if (m_authCookie.isEmpty()) return;
    QUrl url(QStringLiteral("https://%1/api/msd/set_connected").arg(m_opts.host));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("connected"), connected ? "1" : "0");
    url.setQuery(q);
    QNetworkReply* reply = postEmpty(m_nam.get(), url, m_opts.insecure_tls);
    connect(reply, &QNetworkReply::finished, reply, [reply, this, connected]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(lcPikvm) << m_opts.host << "msd set_connected="
                               << connected << "failed:" << reply->errorString();
        }
    });
}

void PiKvmClient::msdSetParams(const QString& image, bool cdrom, bool rw) {
    if (m_authCookie.isEmpty()) return;
    QUrl url(QStringLiteral("https://%1/api/msd/set_params").arg(m_opts.host));
    QUrlQuery q;
    if (!image.isEmpty()) q.addQueryItem(QStringLiteral("image"), image);
    q.addQueryItem(QStringLiteral("cdrom"), cdrom ? "1" : "0");
    q.addQueryItem(QStringLiteral("rw"),    rw    ? "1" : "0");
    url.setQuery(q);
    QNetworkReply* reply = postEmpty(m_nam.get(), url, m_opts.insecure_tls);
    connect(reply, &QNetworkReply::finished, reply, [reply, this, image]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(lcPikvm) << m_opts.host << "msd set_params image=" << image
                               << "failed:" << reply->errorString();
        }
    });
}

void PiKvmClient::msdRemove(const QString& image) {
    if (m_authCookie.isEmpty() || image.isEmpty()) return;
    QUrl url(QStringLiteral("https://%1/api/msd/remove").arg(m_opts.host));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("image"), image);
    url.setQuery(q);
    QNetworkReply* reply = postEmpty(m_nam.get(), url, m_opts.insecure_tls);
    connect(reply, &QNetworkReply::finished, reply, [reply, this, image]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(lcPikvm) << m_opts.host << "msd remove image=" << image
                               << "failed:" << reply->errorString();
        }
    });
}

void PiKvmClient::msdUpload(const QString& imageName, QIODevice* file) {
    if (m_authCookie.isEmpty() || !file) {
        emit msdUploadFinished(false, QStringLiteral("not authenticated or no file"));
        return;
    }
    if (!file->isOpen() && !file->open(QIODevice::ReadOnly)) {
        emit msdUploadFinished(false,
            QStringLiteral("cannot open upload source: %1").arg(file->errorString()));
        return;
    }

    QUrl url(QStringLiteral("https://%1/api/msd/write").arg(m_opts.host));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("image"), imageName);
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/octet-stream"));
    // QNAM streams the QIODevice — no full-buffer materialisation, so a
    // multi-GB ISO works without spiking memory.
    QNetworkReply* reply = m_nam->post(req, file);
    if (m_opts.insecure_tls) {
        connect(reply, &QNetworkReply::sslErrors,
                reply, qOverload<>(&QNetworkReply::ignoreSslErrors));
    }
    connect(reply, &QNetworkReply::uploadProgress, this,
            &PiKvmClient::msdUploadProgress);
    connect(reply, &QNetworkReply::finished, reply, [reply, this, imageName]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(lcPikvm) << m_opts.host << "msd upload" << imageName
                               << "failed:" << reply->errorString();
            emit msdUploadFinished(false, reply->errorString());
        } else {
            qCInfo(lcPikvm) << m_opts.host << "msd upload" << imageName << "OK";
            emit msdUploadFinished(true, QString());
        }
    });
}

void PiKvmClient::atxClick(const QString& button) {
    if (m_authCookie.isEmpty()) {
        qCWarning(lcPikvm) << m_opts.host
                           << "atxClick before authenticated — dropped";
        return;
    }

    QUrl url(QStringLiteral("https://%1/api/atx/click_button").arg(m_opts.host));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("button"), button);
    url.setQuery(q);

    QNetworkRequest req(url);
    QNetworkReply* reply = m_nam->post(req, QByteArray());
    if (m_opts.insecure_tls) {
        connect(reply, &QNetworkReply::sslErrors,
                reply, qOverload<>(&QNetworkReply::ignoreSslErrors));
    }
    const QString btn = button;
    connect(reply, &QNetworkReply::finished, reply, [reply, this, btn]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(lcPikvm) << m_opts.host << "atx click_button=" << btn
                               << "failed:" << reply->errorString();
        } else {
            qCInfo(lcPikvm) << m_opts.host << "atx click_button=" << btn << "OK";
        }
    });
}

void PiKvmClient::pasteText(const QString& text, bool slow, int delayMs) {
    if (text.isEmpty()) return;

    // The server accepts up to `limit` chars per request (default 1024).
    // For longer pastes we split — each chunk is its own POST so the
    // server can keep its rate-limiter happy.
    constexpr qsizetype kChunk = 1024;
    const QByteArray utf8 = text.toUtf8();
    qsizetype offset = 0;
    while (offset < utf8.size()) {
        // Safe split: don't bisect a multi-byte UTF-8 sequence.
        qsizetype take = std::min(kChunk, utf8.size() - offset);
        while (take > 1 && offset + take < utf8.size()
                && (static_cast<unsigned char>(utf8[offset + take]) & 0xC0) == 0x80) {
            // continuation byte; back off until we hit a leading byte
            --take;
        }

        QUrl url(QStringLiteral("https://%1/api/hid/print").arg(m_opts.host));
        QUrlQuery q;
        if (slow) q.addQueryItem(QStringLiteral("slow"), QStringLiteral("1"));
        if (delayMs > 0) {
            q.addQueryItem(QStringLiteral("delay"),
                           QString::number(delayMs / 1000.0, 'f', 3));
        }
        // Server cap is 1024 by default; we chunk client-side already,
        // but pass `limit` explicitly so the server returns a clear
        // error if a future build lowers that default.
        q.addQueryItem(QStringLiteral("limit"), QString::number(kChunk));
        url.setQuery(q);

        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("text/plain; charset=utf-8"));

        QNetworkReply* reply = m_nam->post(req, utf8.mid(offset, take));
        if (m_opts.insecure_tls) {
            connect(reply, &QNetworkReply::sslErrors,
                    reply, qOverload<>(&QNetworkReply::ignoreSslErrors));
        }
        connect(reply, &QNetworkReply::finished, reply, [reply, this]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                qCWarning(lcHid) << m_opts.host << "pasteText failed:"
                                 << reply->errorString();
            }
        });

        offset += take;
    }
}

}  // namespace glasshouse
