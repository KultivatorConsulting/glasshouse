#include "mcpserver.h"

#include "coord_transform.h"
#include "keymap.h"
#include "logging.h"
#include "mcphttpserver.h"
#include "pikvmclient.h"
#include "videowindow.h"
#include "watcher.h"

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
#include <QDateTime>

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

    // `respond` is captured by value, not by reference, and that is load
    // bearing: a tool may answer long after this function has returned —
    // toolMouseMove's settle timer and toolType's pasteFinished both do.
    // A reference would dangle into a dead stack frame, and invoking the
    // destroyed std::function throws bad_function_call with nothing to
    // catch it, which aborts the viewer.
    const auto ok = [id, respond](const QJsonObject& result) {
        respond(QJsonObject{{"jsonrpc", "2.0"}, {"id", id},
                            {"result", result}});
    };
    const auto fail = [id, respond](int code, const QString& message) {
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
    else if (name == QLatin1String("glasshouse_watcher_create")) toolWatcherCreate(args, std::move(done));
    else if (name == QLatin1String("glasshouse_watcher_list"))   toolWatcherList(args, std::move(done));
    else if (name == QLatin1String("glasshouse_watcher_poll"))   toolWatcherPoll(args, std::move(done));
    else if (name == QLatin1String("glasshouse_watcher_frames")) toolWatcherFrames(args, std::move(done));
    else if (name == QLatin1String("glasshouse_watcher_delete")) toolWatcherDelete(args, std::move(done));
    else if (name == QLatin1String("glasshouse_watcher_verdict")) toolWatcherVerdict(args, std::move(done));
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

// ---------------------------------------------------------------------------
// Watchers (HARNESS.md §5, §9.1)
// ---------------------------------------------------------------------------

WatcherRegistry* McpServer::watchersOrError(QString* error) const {
    if (!m_session.watchers) {
        *error = QStringLiteral("watchers are disabled — set "
                                "`watchers.enabled: true` in config.yaml");
        return nullptr;
    }
    return m_session.watchers;
}

namespace {

QString isoOf(qint64 ms) {
    if (ms <= 0) return {};
    return QDateTime::fromMSecsSinceEpoch(ms).toString(Qt::ISODateWithMs);
}

QJsonObject pngContent(const QImage& img) {
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    if (!img.save(&buf, "PNG")) return {};
    buf.close();
    return QJsonObject{{"type", "image"},
                       {"mimeType", "image/png"},
                       {"data", QString::fromLatin1(png.toBase64())}};
}

QJsonObject watcherJson(const WatcherDef& d, const WatcherStats& st) {
    QJsonObject o{
        {"id", d.id},
        {"screen", d.screen},
        {"rect", QJsonObject{{"x", d.rect.x()}, {"y", d.rect.y()},
                             {"w", d.rect.width()}, {"h", d.rect.height()}}},
        {"intent", d.intent},
        {"origin", watcherOriginName(d.origin)},
        {"trigger", watcherTriggerName(d.trigger)},
        {"threshold", d.threshold},
        {"cooldown_ms", d.cooldownMs},
        {"fires", st.fires},
        {"last_delta", st.lastDelta},
        {"history_frames", st.ringFrames},
        {"history_bytes", double(st.ringBytes)},
    };
    if (d.trigger == WatcherTrigger::Settle) {
        o.insert(QStringLiteral("settle_ms"), d.settleMs);
    }
    if (!st.lastFireMs) {
        o.insert(QStringLiteral("last_fire"), QJsonValue::Null);
    } else {
        o.insert(QStringLiteral("last_fire"), isoOf(st.lastFireMs));
    }
    o.insert(QStringLiteral("keep_history"), d.volatileGranted);
    if (d.volatileWanted && !d.volatileGranted) {
        o.insert(QStringLiteral("history_refused"),
                 QStringLiteral("the rectangle is too large to buffer"));
    }
    if (st.suppressed) {
        o.insert(QStringLiteral("suppressed"),
                 QStringLiteral("the target's pointer is in this region, so "
                                "detection is paused"));
    }
    return o;
}

}  // namespace

void McpServer::toolWatcherCreate(const QJsonObject& args, Done done) {
    QString error;
    WatcherRegistry* reg = watchersOrError(&error);
    if (!reg) { done({}, error); return; }
    const McpScreen* sc = screenFor(args, &error);
    if (!sc) { done({}, error); return; }

    WatcherDef d;
    d.screen = sc->index;
    d.origin = WatcherOrigin::Described;
    d.rect   = QRect(intArg(args, "x", 0), intArg(args, "y", 0),
                     intArg(args, "w", 0), intArg(args, "h", 0));
    d.intent = args.value(QStringLiteral("intent")).toString().trimmed();
    if (const QString t = args.value(QStringLiteral("trigger")).toString();
        !t.isEmpty()) {
        if (!parseWatcherTrigger(t, &d.trigger, &error)) {
            done({}, error);
            return;
        }
    }
    d.threshold      = intArg(args, "threshold", 0);
    d.cooldownMs     = intArg(args, "cooldown_ms", 0);
    d.settleMs       = intArg(args, "settle_ms", 0);
    d.notify         = args.value(QStringLiteral("notify")).toBool();
    d.volatileWanted = args.value(QStringLiteral("keep_history")).toBool();

    const int id = reg->create(d, &error);
    if (id == 0) { done({}, error); return; }

    WatcherDef stored;
    reg->get(id, &stored);
    QJsonObject payload = watcherJson(stored, reg->stats(id));
    payload.insert(QStringLiteral("next_step"),
                   QStringLiteral("Call glasshouse_watcher_poll to collect "
                                  "events; nothing is pushed to you."));
    if (stored.volatileWanted && !stored.volatileGranted) {
        payload.insert(
            QStringLiteral("note"),
            QStringLiteral("History was refused because the rectangle covers "
                           "more than %1 pixels. The watcher works; it just "
                           "keeps no history. A tighter rectangle gets it.")
                .arg(reg->limits().volatileAreaCeiling()));
    }

    // §11: a watcher is a standing read of the target's screen, so the user
    // should be able to see one appear rather than discovering it later.
    emit toolActivity(QStringLiteral("MCP: watching %1x%2 on screen %3 — %4")
                          .arg(stored.rect.width()).arg(stored.rect.height())
                          .arg(stored.screen).arg(stored.intent));

    done(textContent(QString::fromUtf8(
             QJsonDocument(payload).toJson(QJsonDocument::Indented))), {});
}

void McpServer::toolWatcherList(const QJsonObject&, Done done) {
    QString error;
    WatcherRegistry* reg = watchersOrError(&error);
    if (!reg) { done({}, error); return; }

    QJsonArray arr;
    for (const auto& d : reg->list()) {
        arr.append(watcherJson(d, reg->stats(d.id)));
    }
    const QJsonObject payload{
        {"watchers", arr},
        {"cursor", double(reg->cursor())},
        {"limit", reg->limits().maxWatchers},
    };
    done(textContent(QString::fromUtf8(
             QJsonDocument(payload).toJson(QJsonDocument::Indented))), {});
}

void McpServer::toolWatcherPoll(const QJsonObject& args, Done done) {
    QString error;
    WatcherRegistry* reg = watchersOrError(&error);
    if (!reg) { done({}, error); return; }

    const qint64 cursor = qint64(
        args.value(QStringLiteral("cursor")).toDouble(0));
    qint64 next = 0;
    const auto events = reg->eventsSince(cursor, &next);

    const bool wantImages = args.value(QStringLiteral("include_images")).toBool();
    const int  maxImages  = std::max(0, intArg(args, "max_images", 4));

    QJsonArray arr;
    QJsonArray content;
    int attached = 0;
    for (const auto& ev : events) {
        QJsonObject o{
            {"watcher", ev.id},
            {"seq", double(ev.seq)},
            {"at", isoOf(ev.wallMs)},
            {"intent", ev.intent},
            {"screen", ev.screen},
            {"delta", ev.delta},
            {"rect", QJsonObject{{"x", ev.rect.x()}, {"y", ev.rect.y()},
                                 {"w", ev.rect.width()},
                                 {"h", ev.rect.height()}}},
            {"transience", watcherTransienceName(ev.transience)},
            {"verdict", watcherVerdictName(ev.verdict)},
        };
        if (!ev.verdictNote.isEmpty()) {
            o.insert(QStringLiteral("verdict_note"), ev.verdictNote);
        }
        if (wantImages && attached < maxImages && !ev.crop.isNull()) {
            const QJsonObject img = pngContent(ev.crop);
            if (!img.isEmpty()) {
                content.append(img);
                o.insert(QStringLiteral("image_index"), attached);
                attached++;
            }
        }
        arr.append(o);
    }

    QJsonObject payload{
        {"events", arr},
        {"next_cursor", double(next)},
        {"count", arr.size()},
    };
    if (attached > 0) {
        payload.insert(QStringLiteral("images"),
                       QStringLiteral("%1 attached, in the order the events "
                                      "are listed; `image_index` on an event "
                                      "says which is which")
                           .arg(attached));
    }
    if (arr.isEmpty()) {
        payload.insert(QStringLiteral("note"),
                       QStringLiteral("Nothing since that cursor. Poll again "
                                      "with next_cursor."));
    }

    // Text first so a client that reads one block gets the structure, not
    // an unlabelled picture.
    QJsonArray out;
    out.append(QJsonObject{{"type", "text"},
                           {"text", QString::fromUtf8(
                                QJsonDocument(payload)
                                    .toJson(QJsonDocument::Indented))}});
    for (const auto& c : content) out.append(c);
    done(QJsonObject{{"content", out}}, {});
}

void McpServer::toolWatcherFrames(const QJsonObject& args, Done done) {
    QString error;
    WatcherRegistry* reg = watchersOrError(&error);
    if (!reg) { done({}, error); return; }

    const int id = intArg(args, "id", 0);
    WatcherDef d;
    if (!reg->get(id, &d)) {
        done({}, QStringLiteral("no watcher with id %1").arg(id));
        return;
    }
    if (!d.volatileGranted) {
        done({}, QStringLiteral("watcher %1 keeps no history — it was "
                                "created without keep_history, or the "
                                "rectangle was too large for it")
                     .arg(id));
        return;
    }

    const QString mode = args.value(QStringLiteral("mode")).toString()
                             .trimmed().toLower();
    const bool stills = mode == QLatin1String("stills");
    const int lastSeconds = std::max(1, intArg(args, "last_seconds", 60));
    const int limit = std::max(1, intArg(args, "limit", stills ? 4 : 12));

    const qint64 from = QDateTime::currentMSecsSinceEpoch() -
                        qint64(lastSeconds) * 1000;
    const auto frames = reg->frames(id, from, 0, limit);
    if (frames.isEmpty()) {
        done(textContent(QStringLiteral(
                 "watcher %1 has no retained frames in the last %2s. It "
                 "retains on change, so a region that has been quiet holds "
                 "only its baseline.").arg(id).arg(lastSeconds)), {});
        return;
    }

    QJsonArray content;
    QJsonArray listed;
    for (const auto& f : frames) {
        listed.append(QJsonObject{
            {"at", isoOf(f.wallMs)},
            {"delta", f.delta},
            {"event", double(f.eventSeq)},
            {"transience", watcherTransienceName(f.transience)},
            {"verdict", watcherVerdictName(f.verdict)}});
    }

    if (stills) {
        for (const auto& f : frames) {
            const QJsonObject img = pngContent(f.crop);
            if (!img.isEmpty()) content.append(img);
        }
    } else {
        const int cols = std::max(1, intArg(args, "cols", 4));
        const QImage strip = WatcherRegistry::filmstrip(frames, cols, 0);
        if (strip.isNull()) {
            done({}, QStringLiteral("failed to build a filmstrip"));
            return;
        }
        const QJsonObject img = pngContent(strip);
        if (img.isEmpty()) {
            done({}, QStringLiteral("failed to encode the filmstrip"));
            return;
        }
        content.append(img);
    }

    const QJsonObject payload{
        {"watcher", id},
        {"intent", d.intent},
        {"mode", stills ? QStringLiteral("stills")
                        : QStringLiteral("filmstrip")},
        {"frames", listed},
        {"window_seconds", lastSeconds},
        {"note", stills
                     ? QStringLiteral("Full-resolution crops, oldest first.")
                     : QStringLiteral("One image, tiled oldest-first with "
                                      "each frame's wall-clock time beneath "
                                      "it. Ask for mode=stills to read one "
                                      "moment at full resolution.")},
    };

    QJsonArray out;
    out.append(QJsonObject{{"type", "text"},
                           {"text", QString::fromUtf8(
                                QJsonDocument(payload)
                                    .toJson(QJsonDocument::Indented))}});
    for (const auto& c : content) out.append(c);
    done(QJsonObject{{"content", out}}, {});
}

void McpServer::toolWatcherDelete(const QJsonObject& args, Done done) {
    QString error;
    WatcherRegistry* reg = watchersOrError(&error);
    if (!reg) { done({}, error); return; }

    if (args.value(QStringLiteral("all")).toBool()) {
        const int n = reg->count();
        reg->clear();
        emit toolActivity(QStringLiteral("MCP: removed %1 watcher(s)").arg(n));
        done(textContent(QStringLiteral("removed %1 watcher(s)").arg(n)), {});
        return;
    }

    const int id = intArg(args, "id", 0);
    if (!reg->remove(id)) {
        done({}, QStringLiteral("no watcher with id %1").arg(id));
        return;
    }
    emit toolActivity(QStringLiteral("MCP: watcher %1 removed").arg(id));
    done(textContent(QStringLiteral("watcher %1 removed").arg(id)), {});
}

void McpServer::toolWatcherVerdict(const QJsonObject& args, Done done) {
    QString error;
    WatcherRegistry* reg = watchersOrError(&error);
    if (!reg) { done({}, error); return; }

    WatcherVerdict v = WatcherVerdict::Unjudged;
    if (!parseWatcherVerdict(args.value(QStringLiteral("verdict")).toString(),
                             &v, &error)) {
        done({}, error);
        return;
    }
    if (v == WatcherVerdict::Unjudged) {
        done({}, QStringLiteral("verdict is required: relevant or spurious"));
        return;
    }
    const QString note = args.value(QStringLiteral("note")).toString();

    QList<qint64> seqs;
    const QJsonValue sv = args.value(QStringLiteral("seq"));
    if (sv.isArray()) {
        for (const auto& e : sv.toArray()) seqs.append(qint64(e.toDouble()));
    } else if (sv.isDouble()) {
        seqs.append(qint64(sv.toDouble()));
    }
    if (seqs.isEmpty()) {
        done({}, QStringLiteral("seq is required: one event seq, or an array "
                                "of them, as glasshouse_watcher_poll reports"));
        return;
    }

    qint64 freed = 0;
    QList<qint64> unknown;
    int judged = 0;
    for (qint64 seq : seqs) {
        const qint64 f = reg->judge(seq, v, note);
        if (f < 0) { unknown.append(seq); continue; }
        freed += f;
        judged++;
    }

    QString msg = QStringLiteral("judged %1 event(s) %2")
                      .arg(judged)
                      .arg(watcherVerdictName(v));
    if (freed > 0) {
        msg += QStringLiteral(", freeing %1 KB of history").arg(freed / 1024);
    }
    if (!unknown.isEmpty()) {
        QStringList u;
        for (qint64 q : unknown) u << QString::number(q);
        msg += QStringLiteral(". No such event: %1 — the event log keeps the "
                              "last 500 firings, so an old seq may have "
                              "aged out").arg(u.join(QStringLiteral(", ")));
    }
    emit toolActivity(QStringLiteral("MCP: %1").arg(msg));
    done(textContent(msg), {});
}

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

    // ----------------------------------------------------------------- watchers
    const QJsonObject watcherIdProp =
        integer("Watcher id, as glasshouse_watcher_create or "
                "glasshouse_watcher_list reports it.");

    tools.append(QJsonObject{
        {"name", "glasshouse_watcher_create"},
        {"description",
         "Watch a rectangle of one screen and record an event whenever it "
         "changes. Detection is local and runs on every decoded frame, so "
         "it catches things that are gone in a second — a notification "
         "toast, a badge appearing on a taskbar icon, a dialog. Nothing is "
         "pushed anywhere: collect events with glasshouse_watcher_poll. "
         "`intent` is required and is not decoration — the rectangle says "
         "where to look and nothing about what matters there, and whoever "
         "reads the event needs the sentence to judge it. Take the "
         "rectangle from a glasshouse_screenshot at max_width 0, whose "
         "pixels are the frame's own."},
        {"inputSchema", obj({
            {"screen", screenProp},
            {"x", integer("Left edge in the screen's own frame pixels.")},
            {"y", integer("Top edge in the screen's own frame pixels.")},
            {"w", integer("Width in frame pixels.")},
            {"h", integer("Height in frame pixels.")},
            {"intent", QJsonObject{{"type", "string"},
                {"description",
                 "What to care about here, in words, e.g. \"a bullet with a "
                 "number over the Chrome icon means new mail\". Carried on "
                 "every event this watcher produces."}}},
            {"trigger", QJsonObject{{"type", "string"},
                {"enum", QJsonArray{"change", "settle"}},
                {"description",
                 "change (default) fires the instant the region changes, "
                 "which is what a transient needs. settle fires once a "
                 "region that was changing has gone quiet, which is what "
                 "\"the page finished loading\" needs."}}},
            {"threshold", integer(
                "Mean absolute luma change, 1-255, that counts as a change. "
                "Default 12. Lower is more sensitive; being trigger-happy "
                "is fine, since whoever reads the event decides whether it "
                "mattered.")},
            {"cooldown_ms", integer(
                "Floor on how often this watcher can fire. Default 3000. "
                "Also the rate at which a region that never goes quiet — a "
                "clock, a progress bar — reports.")},
            {"settle_ms", integer(
                "trigger=settle only: how long the region must be quiet "
                "before it counts as settled. Default 600.")},
            {"keep_history", QJsonObject{{"type", "boolean"},
                {"description",
                 "Keep a rolling buffer of recent crops so you can look "
                 "back through a change rather than only at the frame that "
                 "tripped it — detection is always slightly late, and the "
                 "most useful frame is often the one before. Refused for a "
                 "large rectangle rather than silently degraded; the reply "
                 "says which you got. Fetch with glasshouse_watcher_frames."}}},
            {"notify", QJsonObject{{"type", "boolean"},
                {"description", "Ask the viewer to surface the firing to "
                                "the user as well as recording it."}}},
        }, {"intent"})}});

    tools.append(QJsonObject{
        {"name", "glasshouse_watcher_list"},
        {"description", "Every registered watcher, with how often it has "
                        "fired, how much history it is holding and whether "
                        "the target's pointer is currently sitting in it "
                        "(which suppresses detection)."},
        {"inputSchema", obj({})}});

    tools.append(QJsonObject{
        {"name", "glasshouse_watcher_poll"},
        {"description",
         "Collect watcher events recorded since `cursor`, and get the "
         "cursor to pass next time. This is the only way events are "
         "delivered — the viewer never pushes — so poll it on whatever "
         "interval suits; every 10-30 s is ample, because the history "
         "buffer is what stops anything being missed, not the poll rate. "
         "Pass 0 (or omit) the first time to drain whatever is buffered. "
         "Each event carries a `transience`: `reverted` means the region "
         "went back to its pre-event content, so something appeared and "
         "then left — the shape of a notification; `persisted` means it is "
         "still on screen; `pending` means the verdict is not in yet, and "
         "polling again from a cursor just below that seq will pick it up "
         "once it is. Judge events with glasshouse_watcher_verdict so the "
         "spurious ones stop eating the history budget."},
        {"inputSchema", obj({
            {"cursor", integer("The next_cursor from your previous poll. "
                               "Omit or 0 to get everything buffered.")},
            {"include_images", QJsonObject{{"type", "boolean"},
                {"description", "Attach a thumbnail of what tripped each "
                                "event. Off by default to keep polling "
                                "cheap."}}},
            {"max_images", integer("Cap on attached thumbnails. Default 4.")},
        })}});

    tools.append(QJsonObject{
        {"name", "glasshouse_watcher_frames"},
        {"description",
         "Read a watcher's retained history (keep_history watchers only). "
         "In filmstrip mode, many moments are tiled into one image with "
         "each one's wall-clock time under it, so a model limited to a few "
         "images per request can still see a whole transition; in stills "
         "mode each frame comes back at full resolution, which is what you "
         "want once the filmstrip has told you which moment to read."},
        {"inputSchema", obj({
            {"id", watcherIdProp},
            {"last_seconds", integer("Look back this many seconds from now. "
                                     "Default 60.")},
            {"mode", QJsonObject{{"type", "string"},
                {"enum", QJsonArray{"filmstrip", "stills"}},
                {"description", "filmstrip (default) tiles the frames into "
                                "one image; stills returns them separately."}}},
            {"limit", integer("Most frames to return. Default 12 for a "
                              "filmstrip, 4 for stills.")},
            {"cols", integer("filmstrip only: tiles per row. Default 4.")},
        }, {"id"})}});

    tools.append(QJsonObject{
        {"name", "glasshouse_watcher_verdict"},
        {"description",
         "Record what an event actually turned out to be, so the viewer can "
         "act on it. The local detector is deliberately trigger-happy — it "
         "compares pixels and cannot tell a notification from a tab switch "
         "— and this is the other half of that bargain. A `spurious` "
         "verdict drops that event's retained frames immediately and hands "
         "the budget back, which is how a busy watcher's history is kept "
         "worth reading; `relevant` protects them from eviction instead. "
         "Judge on the event's `transience` and its crop, not on delta "
         "alone: `reverted` means the region went back to what it was, so "
         "something appeared and left, which is the shape of a "
         "notification; `persisted` means it is still on screen, which "
         "covers both a tab switch and a dialog waiting to be answered — "
         "so persisted is never on its own a reason to call something "
         "spurious."},
        {"inputSchema", obj({
            {"seq", QJsonObject{{"type", "array"},
                {"items", QJsonObject{{"type", "integer"}}},
                {"description",
                 "Event seq numbers from glasshouse_watcher_poll. A single "
                 "integer is accepted too. Judging several at once is the "
                 "normal case."}}},
            {"verdict", QJsonObject{{"type", "string"},
                {"enum", QJsonArray{"relevant", "spurious"}},
                {"description",
                 "relevant keeps the event's frames; spurious drops them."}}},
            {"note", QJsonObject{{"type", "string"},
                {"description",
                 "Optional one line on why, carried on the event for "
                 "whoever reads it next."}}},
        })}});

    tools.append(QJsonObject{
        {"name", "glasshouse_watcher_delete"},
        {"description", "Remove a watcher and drop its retained history."},
        {"inputSchema", obj({
            {"id", watcherIdProp},
            {"all", QJsonObject{{"type", "boolean"},
                {"description", "Remove every watcher instead of one."}}},
        })}});

    return tools;
}

}  // namespace glasshouse
