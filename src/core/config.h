#pragma once

#include <QList>
#include <QMap>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <optional>

namespace glasshouse {

struct MonitorRect {
    int     id = 0;
    QPoint  origin;
    QSize   size;
    QString pikvm;  // IP or hostname — also the key used in auth map
};

struct WindowSpec {
    int   target_monitor = 0;
    QRect geometry;
};

enum class VideoTransport {
    Janus,   // PiKVM 4 / firmware with H.264 over WebRTC at /janus/ws
    Mjpeg,   // PiKVM 3 / older firmware: MJPEG via GET /streamer/stream
};

struct AuthSpec {
    QString        user;
    QString        password_ref;
    QString        totp_secret_ref;   // may be empty
    bool           insecure_tls = false;
    VideoTransport transport    = VideoTransport::Janus;
};

struct VideoSpec {
    bool prefer_hw_decode = true;
    int  target_fps = 60;
    // webrtcbin jitterbuffer dwell (ms) for the Janus/H.264 path. Lower =
    // less glass-to-glass latency, less jitter tolerance. 100 suits the LAN
    // a PiKVM lives on (webrtcbin's own default is 200). No effect on MJPEG.
    int  webrtc_latency_ms = 100;
    // Capture cursor marker — a local, zero-lag pointer drawn while captured
    // that the guest cursor converges to (PiKVM absolute mode). style =
    // "ring" | "crosshair" | "hidden"; color = "#RRGGBB"; size = ring outer
    // diameter (px).
    QString cursor_marker       = QStringLiteral("ring");
    QString cursor_marker_color = QStringLiteral("#FFA000");
    int     cursor_marker_size  = 24;
};

// Custom chord entry for the special-keys palette / future macro-pad
// daemon. `keys` is a list of MDN KeyboardEvent.code names (the same
// `web_name` strings PiKVM's send_shortcut endpoint expects).
struct ShortcutSpec {
    QString     label;   // shown on the dialog button
    QStringList keys;    // e.g. ["ControlLeft", "AltLeft", "Delete"]
};

struct Config {
    QSize                  logical_desktop;
    QList<MonitorRect>     monitors;
    QString                hid_master;
    QMap<QString, AuthSpec> auth;           // keyed by MonitorRect::pikvm
    QString                release_hotkey;
    QString                fullscreen_hotkey;
    QString                special_keys_hotkey;
    QList<ShortcutSpec>    shortcuts;       // optional, populates the palette
    VideoSpec              video;
    QList<WindowSpec>      windows;
};

struct ConfigResult {
    std::optional<Config> config;
    QStringList           errors;
    bool ok() const { return config.has_value() && errors.isEmpty(); }
};

// Parse YAML + run DESIGN.md §8.1 structural validation. On failure, `config`
// is nullopt and `errors` is non-empty.
ConfigResult loadConfig(const QString& path);

// DESIGN.md §8.1 structural validation, pure. Exposed for tests.
QStringList validateConfig(const Config& cfg);

}  // namespace glasshouse
