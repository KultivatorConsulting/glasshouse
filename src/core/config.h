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

// Which backend turns a region of the target's screen into text.
//
// Four of them, because they differ in what they cost the user, not
// just in implementation: `pikvm` needs nothing installed anywhere and never
// leaves the LAN; `command` needs a host tool but no account; `openai`
// and `tui` need an API key and send the image to a third party. A
// public clone should work out of the box, which is why `pikvm` is the
// fallback rather than `none`.
enum class OcrProvider {
    None,     // explicitly off; the region hotkey selects but reports nothing
    Pikvm,    // the PiKVM's own Tesseract, via /api/streamer/snapshot?ocr=1
    Command,  // any host tool: PNG on stdin, text on stdout
    OpenAi,   // /v1/chat/completions with Authorization: Bearer
    Tui,      // same shape, Kultivator's endpoint, X-API-Key
};

QString   ocrProviderName(OcrProvider p);
// Returns false and sets `error` for an unrecognised name.
bool      parseOcrProvider(const QString& name, OcrProvider* out,
                           QString* error);

// Region capture → text → clipboard. See DESIGN.md §10.10. `api_key`
// takes a `secret://<name>` reference resolved through SecretResolver,
// so the key itself never appears in config.yaml.
struct OcrSpec {
    // Unset in YAML means "work it out": `tui` when an api_key is set
    // (what every config written before providers existed meant), and
    // `pikvm` otherwise, since that needs no configuration at all.
    OcrProvider provider      = OcrProvider::Pikvm;
    bool        provider_explicit = false;

    // --- provider: pikvm -------------------------------------------
    // Languages to pass as ocr_langs. The PiKVM reports what it has at
    // /api/streamer/ocr; stock firmware ships `eng` and `osd`.
    QStringList langs = {QStringLiteral("eng")};

    // --- provider: command -----------------------------------------
    // argv, run with the PNG on stdin and the text read from stdout.
    // Empty means unconfigured, which validation rejects.
    QStringList command;

    // --- provider: openai | tui ------------------------------------
    QString endpoint;        // defaulted per provider when left unset
    QString api_key;         // secret:// ref
    QString model            = QStringLiteral("auto");
    // Reasoning is pure cost for transcription, and it eats the token
    // budget — Tui answers 200 with empty content when it runs out.
    QString reasoning_effort = QStringLiteral("none");
    int     max_tokens       = 8000;
    // Default asks for a verbatim transcription, because the result goes
    // straight to the clipboard and a description isn't pasteable. Set
    // this to ask for something else (a summary, a description, JSON).
    QString prompt = QStringLiteral(
        "Transcribe every piece of text visible in this image exactly as it appears, preserving reading order and line breaks. Output only the transcribed text: no commentary, no headings, no code fences. If the image contains no text, output nothing.");

    // --- all providers ---------------------------------------------
    // Generous on purpose: PiKVM-side OCR of a 890x320 region measured
    // 11 s on a PiKVM 3, and nginx in front of kvmd gives up at 60.
    int timeout_ms = 60000;

    static constexpr auto kTuiEndpoint =
        "https://chat.kultivator.net.nz/v1/chat/completions";
    static constexpr auto kOpenAiEndpoint =
        "https://api.openai.com/v1/chat/completions";
};

// Local MCP server (DESIGN.md §10.11): exposes the running session — screen
// indexes, the combined desktop, HID-master routing — to any MCP client.
//
// Off by default and loopback-only when on. This is a remote-control
// surface for the target machine: anything that can reach it can type on
// and click the target, so it is opt-in and it does not listen beyond the
// host without being told to. Set `token` to require a bearer header.
struct McpSpec {
    bool    enabled = false;
    QString bind    = QStringLiteral("127.0.0.1");
    int     port    = 8765;
    QString token;   // optional; secret:// ref resolved like a password
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
    QString                paste_hotkey;
    QString                region_hotkey;
    QList<ShortcutSpec>    shortcuts;       // optional, populates the palette
    VideoSpec              video;
    OcrSpec                ocr;
    McpSpec                mcp;
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
