#include "mcpserver.h"

#include "coord_transform.h"
#include "keymap.h"
#include "logging.h"
#include "mcphttpserver.h"
#include "pikvmclient.h"
#include "videowindow.h"

#include <QBuffer>
#include <QImage>
#include <QJsonDocument>
#include <QJsonValue>
#include <QPointF>
#include <QPointer>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>
#include <algorithm>

namespace glasshouse {

namespace {

// Protocol revisions we understand. The spec says to echo the client's
// requested version when we support it, so this is a set rather than a
// single constant; an unknown request gets our preferred one and the
// client decides whether to proceed.
const QStringList kSupportedProtocols = {
    QStringLiteral("2025-11-25"),
    QStringLiteral("2025-06-18"),
    QStringLiteral("2025-03-26"),
    QStringLiteral("2024-11-05"),
};
const QString kPreferredProtocol = QStringLiteral("2025-06-18");

// Screenshots are base64 in a JSON body, so a native 1920x1080 PNG costs
// a few MB per call. Default to something a client can read comfortably
// and let a caller ask for native with max_width: 0 when it needs to read
// small text — the same trade-off region extraction makes by hand.
constexpr int kDefaultMaxWidth = 1280;

// After commanding a pointer move, the target's compositor has to draw the
// cursor and the capture pipeline has to carry the frame. Borrowed from
// the PiKVM MCP server's calibration loop, which waits 300ms between a
// move and the screenshot it measures; without it a follow-up screenshot
// shows the cursor at its previous position.
constexpr int kDefaultSettleMs = 300;

QJsonObject textContent(const QString& text) {
    return QJsonObject{
        {"content", QJsonArray{QJsonObject{{"type", "text"},
                                           {"text", text}}}}};
}

int intArg(const QJsonObject& o, const char* key, int fallback) {
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? static_cast<int>(v.toDouble()) : fallback;
}

}  // namespace

McpServer::McpServer(QObject* parent)
    : QObject(parent), m_http(std::make_unique<McpHttpServer>()) {
    m_http->setHandler([this](const QJsonObject& req,
                              McpHttpServer::Responder respond) {
        handleRequest(req, std::move(respond));
    });
}

McpServer::~McpServer() = default;

void McpServer::setSession(const McpSessionView& view) { m_session = view; }

bool McpServer::start(const QString& bind, quint16 port, const QString& token,
                      QString* error) {
    m_http->setToken(token);
    if (!m_http->listen(bind, port, error)) return false;
    qCInfo(lcMcp).nospace() << "MCP server listening on " << bind << ':'
                            << m_http->port()
                            << (token.isEmpty() ? " (no token)" : " (token set)");
    return true;
}

void McpServer::stop()            { m_http->close(); }
bool McpServer::isRunning() const { return m_http->isListening(); }

// ---------------------------------------------------------------------------
// JSON-RPC / MCP envelope
// ---------------------------------------------------------------------------

void McpServer::handleRequest(const QJsonObject& req,
                              std::function<void(const QJsonObject&)> respond) {
    const QString method = req.value(QStringLiteral("method")).toString();
    const QJsonValue id  = req.value(QStringLiteral("id"));

    // No id means a notification: acknowledge, say nothing. `initialized`
    // is the one every client sends and the one we must not answer.
    if (id.isUndefined() || id.isNull()) {
        qCDebug(lcMcp) << "notification" << method;
        respond(QJsonObject{});
        return;
    }

    const auto ok = [id, &respond](const QJsonObject& result) {
        respond(QJsonObject{{"jsonrpc", "2.0"}, {"id", id},
                            {"result", result}});
    };
    const auto fail = [id, &respond](int code, const QString& message) {
        respond(QJsonObject{{"jsonrpc", "2.0"}, {"id", id},
                            {"error", QJsonObject{{"code", code},
                                                  {"message", message}}}});
    };

    if (method == QLatin1String("initialize")) {
        const QString want = req.value(QStringLiteral("params")).toObject()
                                .value(QStringLiteral("protocolVersion"))
                                .toString();
        const QString use = kSupportedProtocols.contains(want)
                              ? want : kPreferredProtocol;
        if (use != want) {
            qCInfo(lcMcp) << "client asked for protocol" << want
                          << "- answering with" << use;
        }
        ok(QJsonObject{
            {"protocolVersion", use},
            // Tools only. No resources, no prompts, and no `listChanged`
            // subscription: HARNESS.md §9.1 settles on the client polling.
            {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
            {"serverInfo", QJsonObject{
                {"name", "glasshouse"},
                {"version", QStringLiteral(GLASSHOUSE_VERSION)}}},
        });
        return;
    }

    if (method == QLatin1String("ping")) { ok(QJsonObject{}); return; }

    if (method == QLatin1String("tools/list")) {
        ok(QJsonObject{{"tools", toolSchemas()}});
        return;
    }

    if (method == QLatin1String("tools/call")) {
        const QJsonObject params = req.value(QStringLiteral("params")).toObject();
        const QString name = params.value(QStringLiteral("name")).toString();
        const QJsonObject args =
            params.value(QStringLiteral("arguments")).toObject();
        qCDebug(lcMcp) << "tools/call" << name;
        callTool(name, args, [ok, fail, name](const QJsonObject& result,
                                              const QString& error) {
            if (!error.isEmpty()) {
                // A tool that failed for a reason the caller can act on is
                // reported as tool content with isError, not as a protocol
                // error — the model can read the former and retry.
                QJsonObject r = textContent(error);
                r.insert(QStringLiteral("isError"), true);
                ok(r);
                return;
            }
            ok(result);
        });
        return;
    }

    fail(-32601, QStringLiteral("unknown method: %1").arg(method));
}

void McpServer::callTool(const QString& name, const QJsonObject& args,
                         Done done) {
    if      (name == QLatin1String("glasshouse_session"))      toolSession(args, std::move(done));
    else if (name == QLatin1String("glasshouse_screenshot"))   toolScreenshot(args, std::move(done));
    else if (name == QLatin1String("glasshouse_type"))         toolType(args, std::move(done));
    else if (name == QLatin1String("glasshouse_keys"))         toolKeys(args, std::move(done));
    else if (name == QLatin1String("glasshouse_mouse_move"))   toolMouseMove(args, std::move(done));
    else if (name == QLatin1String("glasshouse_mouse_click"))  toolMouseClick(args, std::move(done));
    else if (name == QLatin1String("glasshouse_mouse_scroll")) toolMouseScroll(args, std::move(done));
    else done({}, QStringLiteral("unknown tool: %1").arg(name));
}

// ---------------------------------------------------------------------------
// Shared argument handling
// ---------------------------------------------------------------------------

const McpScreen* McpServer::screenFor(const QJsonObject& args,
                                      QString* error) const {
    if (m_session.screens.isEmpty()) {
        *error = QStringLiteral("no screens in the session");
        return nullptr;
    }
    const QJsonValue v = args.value(QStringLiteral("screen"));
    if (v.isUndefined() || v.isNull()) {
        if (m_session.screens.size() == 1) return &m_session.screens.first();
        *error = QStringLiteral("`screen` is required: the session has %1 "
                                "screens (0..%2). Call glasshouse_session "
                                "to see them.")
                     .arg(m_session.screens.size())
                     .arg(m_session.screens.size() - 1);
        return nullptr;
    }
    const int idx = static_cast<int>(v.toDouble(-1));
    for (const auto& s : m_session.screens) {
        if (s.index == idx) return &s;
    }
    *error = QStringLiteral("no screen with index %1; the session has 0..%2")
                 .arg(idx).arg(m_session.screens.size() - 1);
    return nullptr;
}

bool McpServer::pointFor(const QJsonObject& args, QPointF* outDesktopPx,
                         QString* how, QString* error) const {
    const bool hasDesktop = args.contains(QStringLiteral("desktop_x"))
                         && args.contains(QStringLiteral("desktop_y"));
    const bool hasLocal   = args.contains(QStringLiteral("x"))
                         && args.contains(QStringLiteral("y"));

    if (hasDesktop) {
        *outDesktopPx = QPointF(args.value(QStringLiteral("desktop_x")).toDouble(),
                                args.value(QStringLiteral("desktop_y")).toDouble());
        *how = QStringLiteral("desktop pixel (%1,%2)")
                   .arg(outDesktopPx->x()).arg(outDesktopPx->y());
        return true;
    }
    if (!hasLocal) {
        *error = QStringLiteral("give either x/y (pixels within a screen, "
                                "with `screen`) or desktop_x/desktop_y "
                                "(pixels across the whole target desktop)");
        return false;
    }
    const McpScreen* s = screenFor(args, error);
    if (!s) return false;

    const double lx = args.value(QStringLiteral("x")).toDouble();
    const double ly = args.value(QStringLiteral("y")).toDouble();
    *outDesktopPx = QPointF(s->monitorRect.x() + lx, s->monitorRect.y() + ly);
    *how = QStringLiteral("screen %1 pixel (%2,%3) = desktop (%4,%5)")
               .arg(s->index).arg(lx).arg(ly)
               .arg(outDesktopPx->x()).arg(outDesktopPx->y());
    return true;
}

bool McpServer::hidReady(QString* error) const {
    if (!m_session.hid) {
        *error = QStringLiteral("no HID master is connected, so keyboard and "
                                "mouse cannot be routed. Check that the "
                                "configured hid_master window is running.");
        return false;
    }
    if (!m_session.hid->isConnected()) {
        *error = QStringLiteral("the HID master (%1) is not connected")
                     .arg(m_session.hidHost);
        return false;
    }
    const HidState hid = m_session.hid->lastHidState();
    if (!hid.online) {
        *error = QStringLiteral("the HID master (%1) reports the target's HID "
                                "offline — the USB cable to the target may be "
                                "out, or the target is off")
                     .arg(m_session.hidHost);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

void McpServer::toolSession(const QJsonObject&, Done done) {
    QJsonArray screens;
    for (const auto& s : m_session.screens) {
        QSize capture;
        if (s.window) {
            if (QVideoSink* sink = s.window->videoSink()) {
                capture = sink->videoFrame().size();
            }
        }
        QJsonObject o{
            {"screen",      s.index},
            {"monitor_id",  s.monitorId},
            {"pikvm",       s.host},
            {"hid_master",  s.hidMaster},
            {"connected",   s.client && s.client->isConnected()},
            {"desktop_rect", QJsonObject{{"x", s.monitorRect.x()},
                                         {"y", s.monitorRect.y()},
                                         {"w", s.monitorRect.width()},
                                         {"h", s.monitorRect.height()}}},
        };
        if (capture.isValid() && !capture.isEmpty()) {
            o.insert(QStringLiteral("capture_size"),
                     QJsonObject{{"w", capture.width()},
                                 {"h", capture.height()}});
        }
        screens.append(o);
    }

    const QJsonObject payload{
        {"logical_desktop", QJsonObject{
            {"w", m_session.logicalDesktop.width()},
            {"h", m_session.logicalDesktop.height()}}},
        {"hid_master", m_session.hidHost},
        {"screens", screens},
        {"coordinates",
         QStringLiteral("Screen-relative: pass `screen` plus x/y in that "
                        "screen's own pixels. Desktop-absolute: pass "
                        "desktop_x/desktop_y, origin at the top-left of the "
                        "combined desktop. Both map exactly — the layout is "
                        "configured, so no calibration is involved.")},
    };
    done(textContent(QString::fromUtf8(
             QJsonDocument(payload).toJson(QJsonDocument::Indented))),
         {});
}

void McpServer::toolScreenshot(const QJsonObject& args, Done done) {
    QString error;
    const McpScreen* s = screenFor(args, &error);
    if (!s) { done({}, error); return; }
    if (!s->window) {
        done({}, QStringLiteral("screen %1 has no window").arg(s->index));
        return;
    }
    QVideoSink* sink = s->window->videoSink();
    const QImage full = sink ? sink->videoFrame().toImage() : QImage();
    if (full.isNull()) {
        done({}, QStringLiteral("screen %1 (%2) has no video frame yet — the "
                                "stream may still be connecting")
                     .arg(s->index).arg(s->host));
        return;
    }

    const int maxW = intArg(args, "max_width", kDefaultMaxWidth);
    QImage out = full;
    if (maxW > 0 && full.width() > maxW) {
        out = full.scaledToWidth(maxW, Qt::SmoothTransformation);
    }

    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    if (!out.save(&buf, "PNG")) {
        done({}, QStringLiteral("failed to encode the frame as PNG"));
        return;
    }
    buf.close();

    const QJsonObject result{
        {"content", QJsonArray{
            QJsonObject{{"type", "image"},
                        {"mimeType", "image/png"},
                        {"data", QString::fromLatin1(png.toBase64())}},
            QJsonObject{{"type", "text"},
                        {"text", QStringLiteral(
                             "screen %1 (%2), %3x%4 captured at %5x%6")
                             .arg(s->index).arg(s->host)
                             .arg(out.width()).arg(out.height())
                             .arg(full.width()).arg(full.height())}},
        }}};
    done(result, {});
}

void McpServer::toolType(const QJsonObject& args, Done done) {
    QString error;
    if (!hidReady(&error)) { done({}, error); return; }

    const QString raw = args.value(QStringLiteral("text")).toString();
    if (raw.isEmpty()) {
        done({}, QStringLiteral("`text` is empty, nothing to type"));
        return;
    }
    if (m_session.hid->pasteBusy()) {
        done({}, QStringLiteral("a paste is already in flight; two at once "
                                "would interleave and scramble both"));
        return;
    }

    // Same sanitiser the clipboard hotkey uses: kvmd's server-side keymap
    // silently drops what it cannot map, so fold the common offenders and
    // report what still went missing rather than letting it vanish.
    int dropped = 0;
    const QString text = sanitiseForPrint(raw, &dropped);
    if (text.isEmpty()) {
        done({}, QStringLiteral("after sanitising, there was nothing typeable "
                                "left (%1 characters the target keymap cannot "
                                "produce)").arg(dropped));
        return;
    }

    const bool slow = args.value(QStringLiteral("slow")).toBool(false);
    const int delay = intArg(args, "delay_ms", 0);

    // One shot: disconnect after the first completion so a later paste
    // from the hotkey path cannot fire this tool's responder.
    auto* ctx = new QObject(this);
    QPointer<McpServer> self(this);
    const int total = static_cast<int>(text.size());
    connect(m_session.hid, &PiKvmClient::pasteFinished, ctx,
            [this, ctx, done, total, dropped, slow](bool ok, const QString& why) {
                ctx->deleteLater();
                if (!ok) {
                    done({}, QStringLiteral("typing failed: %1").arg(why));
                    return;
                }
                QString msg = QStringLiteral("typed %1 characters").arg(total);
                if (slow)      msg += QStringLiteral(" (slow mode)");
                if (dropped)   msg += QStringLiteral("; dropped %1 character(s) "
                                                     "the target keymap cannot "
                                                     "produce").arg(dropped);
                emit toolActivity(QStringLiteral("MCP typed %1 chars").arg(total));
                done(textContent(msg), {});
            });

    m_session.hid->pasteText(text, slow, delay);
}

void McpServer::toolKeys(const QJsonObject& args, Done done) {
    QString error;
    if (!hidReady(&error)) { done({}, error); return; }

    QStringList keys;
    for (const QJsonValue& v : args.value(QStringLiteral("keys")).toArray()) {
        const QString k = v.toString().trimmed();
        if (!k.isEmpty()) keys.append(k);
    }
    if (keys.isEmpty()) {
        done({}, QStringLiteral("`keys` must hold at least one MDN "
                                "KeyboardEvent.code name, e.g. "
                                "[\"ControlLeft\",\"KeyC\"] or [\"Enter\"]"));
        return;
    }

    // Pressed in order then released in reverse, which is what makes a
    // list a chord rather than a sequence of taps.
    m_session.hid->sendShortcut(keys);
    emit toolActivity(QStringLiteral("MCP sent %1").arg(keys.join('+')));
    done(textContent(QStringLiteral("sent %1").arg(keys.join(QLatin1Char('+')))),
         {});
}

void McpServer::toolMouseMove(const QJsonObject& args, Done done) {
    QString error;
    if (!hidReady(&error)) { done({}, error); return; }

    QPointF desktopPx;
    QString how;
    if (!pointFor(args, &desktopPx, &how, &error)) { done({}, error); return; }

    const ApiCoord api = desktopPixelToApi(desktopPx, m_session.logicalDesktop);
    m_session.hid->sendMouseMove(api.x, api.y);
    emit toolActivity(QStringLiteral("MCP moved pointer"));

    const int settle = intArg(args, "settle_ms", 0);
    const QString msg = QStringLiteral("moved pointer to %1, wire (%2,%3)")
                            .arg(how).arg(api.x).arg(api.y);
    if (settle <= 0) { done(textContent(msg), {}); return; }

    // Settling matters when the caller is about to screenshot: the cursor
    // has to be drawn and the frame has to travel. Without the wait the
    // next capture shows the pointer where it used to be.
    QTimer::singleShot(std::clamp(settle, 0, 5000), this,
                       [done, msg, settle]() {
        done(textContent(msg + QStringLiteral(", settled for %1ms")
                                   .arg(settle)), {});
    });
}

void McpServer::toolMouseClick(const QJsonObject& args, Done done) {
    QString error;
    if (!hidReady(&error)) { done({}, error); return; }

    const QString name = args.value(QStringLiteral("button"))
                             .toString(QStringLiteral("left")).toLower();
    MouseButton button{};
    if      (name == QLatin1String("left"))    button = MouseButton::Left;
    else if (name == QLatin1String("right"))   button = MouseButton::Right;
    else if (name == QLatin1String("middle"))  button = MouseButton::Middle;
    else if (name == QLatin1String("back"))    button = MouseButton::Back;
    else if (name == QLatin1String("forward")) button = MouseButton::Forward;
    else {
        done({}, QStringLiteral("unknown button '%1'; use left, right, "
                                "middle, back or forward").arg(name));
        return;
    }

    QString where;
    const bool wantsMove = args.contains(QStringLiteral("x"))
                        || args.contains(QStringLiteral("desktop_x"));
    if (wantsMove) {
        QPointF desktopPx;
        if (!pointFor(args, &desktopPx, &where, &error)) {
            done({}, error);
            return;
        }
        const ApiCoord api =
            desktopPixelToApi(desktopPx, m_session.logicalDesktop);
        m_session.hid->sendMouseMove(api.x, api.y);
    }

    const int clicks = std::clamp(intArg(args, "count", 1), 1, 3);
    for (int i = 0; i < clicks; ++i) {
        m_session.hid->sendMouseButton(button, true);
        m_session.hid->sendMouseButton(button, false);
    }

    emit toolActivity(QStringLiteral("MCP clicked %1").arg(name));
    QString msg = QStringLiteral("%1 %2 click(s)").arg(clicks).arg(name);
    if (!where.isEmpty()) msg += QStringLiteral(" at %1").arg(where);
    done(textContent(msg), {});
}

void McpServer::toolMouseScroll(const QJsonObject& args, Done done) {
    QString error;
    if (!hidReady(&error)) { done({}, error); return; }

    const int dx = intArg(args, "dx", 0);
    const int dy = intArg(args, "dy", 0);
    if (dx == 0 && dy == 0) {
        done({}, QStringLiteral("give a non-zero dx or dy; positive dy "
                                "scrolls up, negative scrolls down"));
        return;
    }
    m_session.hid->sendMouseWheel(dx, dy);
    emit toolActivity(QStringLiteral("MCP scrolled"));
    done(textContent(QStringLiteral("scrolled dx=%1 dy=%2").arg(dx).arg(dy)),
         {});
}

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

QJsonArray McpServer::toolSchemas() const {
    const auto obj = [](QJsonObject props, QStringList required = {}) {
        QJsonObject schema{{"type", "object"}, {"properties", props}};
        if (!required.isEmpty()) {
            QJsonArray r;
            for (const auto& s : required) r.append(s);
            schema.insert(QStringLiteral("required"), r);
        }
        return schema;
    };
    const auto num = [](const char* desc) {
        return QJsonObject{{"type", "number"}, {"description", desc}};
    };
    const auto integer = [](const char* desc) {
        return QJsonObject{{"type", "integer"}, {"description", desc}};
    };

    const QJsonObject screenProp = integer(
        "0-based screen index from glasshouse_session. Optional when the "
        "session has exactly one screen.");

    QJsonArray tools;

    tools.append(QJsonObject{
        {"name", "glasshouse_session"},
        {"description",
         "Describe the session: every screen with its index, the PiKVM "
         "serving it and its rectangle within the combined target desktop, "
         "plus which PiKVM carries keyboard and mouse. Call this first — "
         "the other tools take the screen indexes it reports."},
        {"inputSchema", obj({})}});

    tools.append(QJsonObject{
        {"name", "glasshouse_screenshot"},
        {"description",
         "Capture the current video frame of one screen as a PNG. The frame "
         "comes from the live stream, so it is whatever the target is "
         "showing right now."},
        {"inputSchema", obj({
            {"screen", screenProp},
            {"max_width", integer(
                 "Downscale to at most this width. Defaults to 1280, which "
                 "keeps the reply small; pass 0 for the native capture "
                 "resolution when you need to read small text.")},
        })}});

    tools.append(QJsonObject{
        {"name", "glasshouse_type"},
        {"description",
         "Type text on the target as keystrokes, through the HID master. "
         "Goes through the PiKVM's server-side keymap, so characters it "
         "cannot produce are reported as dropped rather than silently lost. "
         "Text only — this is a keyboard, not a clipboard."},
        {"inputSchema", obj({
            {"text", QJsonObject{{"type", "string"},
                                 {"description", "The text to type."}}},
            {"slow", QJsonObject{{"type", "boolean"},
                                 {"description",
                                  "Add an inter-key delay for inputs that "
                                  "drop fast keystrokes, such as a BIOS "
                                  "prompt."}}},
            {"delay_ms", integer("Inter-key delay in ms when slow is set, "
                                 "0-5000.")},
        }, {QStringLiteral("text")})}});

    tools.append(QJsonObject{
        {"name", "glasshouse_keys"},
        {"description",
         "Press a key or a chord on the target. Keys are pressed in the "
         "order given and released in reverse, so [\"ControlLeft\",\"KeyC\"] "
         "is Ctrl+C rather than two separate taps. Names are MDN "
         "KeyboardEvent.code strings: KeyA, Enter, Tab, Escape, F5, "
         "ArrowLeft, ControlLeft, AltLeft, ShiftLeft, MetaLeft, Delete."},
        {"inputSchema", obj({
            {"keys", QJsonObject{
                {"type", "array"},
                {"items", QJsonObject{{"type", "string"}}},
                {"description", "One key for a single press, several for a "
                                "chord."}}},
        }, {QStringLiteral("keys")})}});

    const QJsonObject coordProps{
        {"screen", screenProp},
        {"x", num("Pixels from the left of that screen.")},
        {"y", num("Pixels from the top of that screen.")},
        {"desktop_x", num("Alternative to screen+x: pixels from the left of "
                          "the whole combined desktop.")},
        {"desktop_y", num("Alternative to screen+y: pixels from the top of "
                          "the whole combined desktop.")},
    };

    QJsonObject moveProps = coordProps;
    moveProps.insert(QStringLiteral("settle_ms"), integer(
        "Wait this long before replying, so a following screenshot shows the "
        "cursor in its new position rather than its old one. 300 is a good "
        "starting point; 0 (the default) returns immediately."));

    tools.append(QJsonObject{
        {"name", "glasshouse_mouse_move"},
        {"description",
         "Move the target's pointer. Coordinates are exact: Glasshouse is "
         "configured with the monitor layout and verifies it against the "
         "streamer, so a pixel maps straight through to absolute HID "
         "coordinates with no calibration step. Give either screen+x/y or "
         "desktop_x/desktop_y."},
        {"inputSchema", obj(moveProps)}});

    QJsonObject clickProps = coordProps;
    clickProps.insert(QStringLiteral("button"), QJsonObject{
        {"type", "string"},
        {"enum", QJsonArray{"left", "right", "middle", "back", "forward"}},
        {"description", "Defaults to left."}});
    clickProps.insert(QStringLiteral("count"), integer(
        "1 for a single click, 2 for a double click. Max 3."));

    tools.append(QJsonObject{
        {"name", "glasshouse_mouse_click"},
        {"description",
         "Click a mouse button on the target, optionally moving there first "
         "by passing coordinates. Without coordinates it clicks wherever "
         "the pointer already is."},
        {"inputSchema", obj(clickProps)}});

    tools.append(QJsonObject{
        {"name", "glasshouse_mouse_scroll"},
        {"description", "Scroll the target's wheel. Positive dy scrolls up, "
                        "negative scrolls down."},
        {"inputSchema", obj({
            {"dx", integer("Horizontal wheel delta.")},
            {"dy", integer("Vertical wheel delta.")},
        })}});

    return tools;
}

}  // namespace glasshouse
