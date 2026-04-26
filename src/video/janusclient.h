#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <memory>

class QTimer;
class QWebSocket;

namespace glasshouse {

// Janus gateway signalling client for stock PiKVM.
//
// Endpoint: `wss://<host>/janus/ws`, WebSocket subprotocol `janus-protocol`,
// auth via the kvmd `auth_token` cookie (same cookie `PiKvmClient` uses on
// the state WS). The endpoint fronts the Janus WebRTC gateway that pipes
// H.264 out of ustreamer — the only H.264 path stock PiKVM firmware
// exposes.
//
// Probe slice: drives the signalling handshake up to the first SDP offer
// and surfaces it via `sdpOfferReceived`, but does NOT answer. A following
// slice will plumb the offer into GStreamer `webrtcbin`, generate the
// answer, and trickle ICE both directions.
//
//     info → create → attach(<plugin>) → message{request:"watch"}
//       → (async event with jsep.type="offer")  ← probe stops here
class JanusClient : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString host;              // IP or hostname, no scheme
        QString authCookieHeader;  // "auth_token=..." — exactly what the
                                   // kvmd cookie jar stored; value is sent
                                   // verbatim as the `Cookie` header.
        // Plugin name observed via `info` on a stock PiKVM is
        // `janus.plugin.ustreamer`. Overridable because older/variant
        // images may ship `janus.plugin.streaming` instead — the probe
        // exposes this on the command line.
        QString pluginName = QStringLiteral("janus.plugin.ustreamer");
        bool    insecure_tls = false;
    };

    explicit JanusClient(Options opts, QObject* parent = nullptr);
    ~JanusClient() override;

    void open();
    void close();

    qint64 sessionId() const { return m_sessionId; }
    qint64 handleId()  const { return m_handleId;  }

public slots:
    // Send the SDP answer back to Janus in response to the offer that
    // triggered `sdpOfferReceived`. Wraps the SDP in a Janus `message` frame
    // with body `{"request":"start"}` and `jsep.type="answer"` — matches what
    // the ustreamer plugin expects to finish the negotiation.
    void sendAnswer(const QString& sdp);

signals:
    void wsConnected();
    void sessionReady(qint64 sessionId, qint64 handleId);
    void sdpOfferReceived(QString sdp);
    // Terminal: WS drop, Janus error frame, plugin not found, bad auth, etc.
    // After this fires the client will not drive further state; caller must
    // call close() and (optionally) build a new instance to retry.
    void sessionFailed(QString reason);

private slots:
    void onWsConnected();
    void onWsDisconnected();
    void onWsTextMessage(const QString& msg);
    void onKeepaliveTick();
    void attemptReconnect();

private:
    QString nextTxn(const QString& tag);
    void    sendJanus(QJsonObject obj);
    void    sendInfo();
    void    sendCreate();
    void    sendAttach();
    void    sendWatch();

    void    handleServerInfo(const QJsonObject& obj);
    void    handleSuccess(const QJsonObject& obj, const QString& tag);
    void    handleEvent(const QJsonObject& obj);
    void    handleError(const QJsonObject& obj, const QString& tag);

    // Reliability path. Any in-flight signalling failure (WS drop,
    // Janus error frame, etc.) routes through `transientFail`, which
    // either schedules another open() or, after the retry budget is
    // exhausted, escalates via `terminalFail` → `sessionFailed`.
    void    transientFail(const QString& reason);
    void    terminalFail(const QString& reason);
    void    scheduleReconnect();
    void    resetPerAttemptState();
    void    openWebSocket();

    Options                      m_opts;
    std::unique_ptr<QWebSocket>  m_ws;
    std::unique_ptr<QTimer>      m_keepalive;
    std::unique_ptr<QTimer>      m_reconnectTimer;

    quint64                      m_nextTxn = 1;
    // transaction id -> logical tag ("info","create","attach","watch",
    // "keepalive"). Drained on each response so the map stays bounded.
    QHash<QString, QString>      m_txnTag;

    qint64                       m_sessionId = 0;
    qint64                       m_handleId  = 0;

    bool                         m_stopRequested = false;
    int                          m_attempt        = 0;
    int                          m_reconnectBackoffMs = 1000;
};

}  // namespace glasshouse
