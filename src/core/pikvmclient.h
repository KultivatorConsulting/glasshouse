#pragma once

#include "pikvmevents.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>

class QNetworkAccessManager;
class QTimer;
class QWebSocket;

namespace glasshouse {

// One client per PiKVM. Owns the HTTPS auth session, the `/api/ws?stream=0`
// state WebSocket, the kvmd-level ping/pong keepalive, reconnect with
// exponential backoff, and the outgoing HID event senders.
//
// All networking is asynchronous and driven by the Qt event loop on the thread
// that owns this QObject. No internal threads. If Phase 4 wants one thread per
// PiKVM, call `moveToThread()` before calling `start()`.
class PiKvmClient : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString host;             // IP or hostname
        QString user;
        QString password;         // already-resolved plaintext
        QString totp_secret;      // base32 TOTP seed; empty = no 2FA
        bool    insecure_tls = false;
    };

    explicit PiKvmClient(Options opts, QObject* parent = nullptr);
    ~PiKvmClient() override;

    QString host() const { return m_opts.host; }

    // Begin auth + WS flow. Idempotent and safe to call again after a stop().
    void start();
    // Close the WS, cancel reconnect attempts. Signals for pending network
    // completions are still delivered but will no-op.
    void stop();

    bool     isConnected() const;
    HidState lastHidState() const { return m_hid; }

    // Full Cookie-header value (`auth_token=...`) for reuse on sibling
    // sockets (e.g. Janus `/janus/ws`). Empty until authenticated() fires.
    QString  authCookieHeader() const { return m_authCookie; }
    bool     insecureTls() const { return m_opts.insecure_tls; }

    // HID senders. All ride on the state WebSocket (same connection used for
    // incoming events). If the WS is not open, the event is dropped with a
    // warning log — there is no REST fallback in Phase 1.
    void sendMouseMove(int to_x, int to_y);
    void sendMouseButton(MouseButton button, bool pressed);
    void sendMouseWheel(int delta_x, int delta_y);
    void sendKey(const QString& key, bool pressed, bool finish = false);
    // The WS has no `shortcut` event type — synthesised by pressing keys in
    // order, then releasing them in reverse. Matches what the PiKVM web UI does.
    void sendShortcut(const QStringList& keys);

    // Server-side type-text-as-keystrokes via `POST /api/hid/print`.
    // The endpoint takes a UTF-8 body and types it on the target's HID,
    // applying its configured server-side keymap (default `en_US`).
    // `slow=true` adds an inter-key delay (`delayMs`, 0–5000) for picky
    // BIOS prompts. Server caps the body at ~1024 chars by default; we
    // chunk above that. Auth re-uses the cookie jar from the login
    // session, so this only works after authenticated() has fired.
    void pasteText(const QString& text, bool slow = false, int delayMs = 0);

    // ATX power affordances: `POST /api/atx/click_button?button=<X>`
    // where X is one of "power" (short press), "power_long" (force-off
    // long press), or "reset" (hardware reset). Fire-and-forget; the
    // server has its own debouncing. Auth uses the existing cookie jar.
    void atxClick(const QString& button);

signals:
    void authenticated();
    void connected();
    // Fires once per connection, on first `hid` state event. There is no
    // discrete end-of-burst marker on the server (verified 2026-04-25 against
    // kvmd HEAD — see DESIGN.md §10.4), so this is the earliest point at which
    // HID state is known.
    void initialStateReceived(const glasshouse::HidState& hid);
    void hidStateChanged(const glasshouse::HidState& hid);
    // Every state event (including `hid`); useful for debugging.
    void rawStateEvent(const QString& type, const QJsonObject& event);
    void disconnected(const QString& reason);
    void errorOccurred(const QString& msg);

private slots:
    void onLoginFinished();
    void onWsConnected();
    void onWsDisconnected();
    void onWsTextMessage(const QString& msg);
    void onWsBinaryMessage(const QByteArray& msg);
    void onPingTick();
    void attemptReconnect();

private:
    void authenticate();
    void openWebSocket();
    void sendWsJson(const QString& type, const QJsonObject& event);
    void scheduleReconnect();
    QString currentPassword() const;   // password + TOTP if configured

    Options                                 m_opts;
    std::unique_ptr<QNetworkAccessManager>  m_nam;
    std::unique_ptr<QWebSocket>             m_ws;
    std::unique_ptr<QTimer>                 m_pingTimer;
    std::unique_ptr<QTimer>                 m_reconnectTimer;
    HidState                                m_hid;
    QString                                 m_authCookie;      // "auth_token=..."
    bool                                    m_stopped = false;
    bool                                    m_initialStateSeen = false;
    int                                     m_reconnectBackoffMs = 1000;
};

}  // namespace glasshouse
