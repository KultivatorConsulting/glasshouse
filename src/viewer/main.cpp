// PiKVM viewer: loads YAML config, brings up one VideoWindow per
// configured window, each with its own auth / Janus / decode pipeline,
// and routes click-to-capture input through the configured HID master.

#include "config.h"
#include "input_router.h"
#include "janusclient.h"
#include "keymap.h"
#include "logging.h"
#include "mjpegpipeline.h"
#include "msddialog.h"
#include "pikvmclient.h"
#include "secrets.h"
#include "specialkeysdialog.h"
#include "tuiclient.h"
#include "videopipeline.h"
#include "videowindow.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCommandLineParser>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QIcon>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QVideoSink>

#include <memory>
#include <optional>

using namespace glasshouse;

namespace {

QString defaultConfigPath() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
         + QStringLiteral("/glasshouse/config.yaml");
}

void errln(const QString& s) {
    QTextStream err(stderr);
    err << s << '\n';
    err.flush();
}

void outln(const QString& s) {
    QTextStream out(stdout);
    out << s << '\n';
    out.flush();
}

// Startup-failure announcer. errln so the message lands on stderr for
// terminal launches, plus a modal QMessageBox so a desktop launch
// (KDE menu, .desktop double-click) actually surfaces the reason —
// without it the process disappears with no UI feedback at all.
void fatal(const QString& title, const QString& body) {
    errln(body);
    QMessageBox::critical(nullptr, title, body);
}

// File-logging glue. Active only when --log-file is passed; otherwise
// Qt's default handler writes to stderr unmodified.
QFile*           g_logFile        = nullptr;
QTextStream*     g_logStream      = nullptr;
QtMessageHandler g_originalLogger = nullptr;

void glasshouseMessageHandler(QtMsgType type,
                              const QMessageLogContext& ctx,
                              const QString& msg) {
    // Always let Qt's default handler render to stderr first — it
    // honours QT_LOGGING_RULES the way users expect, and we don't want
    // to reimplement that filtering here.
    if (g_originalLogger) g_originalLogger(type, ctx, msg);

    if (!g_logStream) return;

    const char* levelTag = "?";
    switch (type) {
        case QtDebugMsg:    levelTag = "DEBUG"; break;
        case QtInfoMsg:     levelTag = "INFO";  break;
        case QtWarningMsg:  levelTag = "WARN";  break;
        case QtCriticalMsg: levelTag = "ERROR"; break;
        case QtFatalMsg:    levelTag = "FATAL"; break;
    }
    const QString line = QStringLiteral("%1 %2 %3 %4")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
             QString::fromLatin1(levelTag),
             QString::fromLatin1(ctx.category ? ctx.category : "default"),
             msg);
    *g_logStream << line << '\n';
    g_logStream->flush();
}

// One PiKVM = one VideoWindow + its auth/signalling/decode chain. All
// QObjects share `&app` as the parent so they're destroyed when the
// QApplication tears down. The cleanup at the bottom runs explicit
// close()/stop() in dependency order before the destructors fire.
//
// Exactly one of (webrtc + jc) or (mjpeg) is populated, gated on
// AuthSpec.transport.
struct Instance {
    QString          host;
    QRect            monitorRect;
    VideoTransport   transport = VideoTransport::Janus;

    VideoWindow*     window    = nullptr;
    PiKvmClient*     pk        = nullptr;

    // Janus path
    VideoPipeline*   webrtc    = nullptr;
    JanusClient*     jc        = nullptr;   // assigned on authenticated()

    // MJPEG path
    MjpegPipeline*   mjpeg     = nullptr;

    QElapsedTimer*   wallClock = nullptr;

    // Last delivered-frame count sampled by the stats poll timer, used to
    // derive effective render FPS from the per-second delta.
    quint64          lastDelivered = 0;

    // Set when an error has scheduled a re-auth + rebuild via QTimer;
    // cleared in the authenticated handler once the rebuild has run.
    // Without this, a single failed pipeline-start that fires multiple
    // GStreamer errors (e.g. 502 → internal-data-stream-error → EOS)
    // queues N concurrent re-auths, each of which rebuilds the
    // pipeline and produces another N errors — exponential storm.
    bool             reconnectScheduled = false;
};

// Look up the MonitorRect for a given window spec.
const MonitorRect* findMonitor(const Config& cfg, int targetMonitorId) {
    for (const auto& m : cfg.monitors) {
        if (m.id == targetMonitorId) return &m;
    }
    return nullptr;
}

// Resolve secrets for a PiKVM's auth entry. Returns std::nullopt and
// surfaces a diagnostic via fatal() on failure (stderr + QMessageBox).
std::optional<std::pair<QString, QString>>
resolveSecrets(const QString& host, const AuthSpec& a) {
    const auto password = SecretResolver::resolve(a.password_ref);
    if (!password) {
        fatal(QStringLiteral("Glasshouse — secret missing"),
              QStringLiteral("Cannot resolve password_ref for %1 (%2).\n\n"
                             "Set it in either:\n"
                             "  • environment variable %3, or\n"
                             "  • ~/.config/glasshouse/secrets.yaml "
                             "(map of name: plaintext, chmod 0600)")
                  .arg(host, a.password_ref,
                       SecretResolver::envKeyForRef(a.password_ref)));
        return std::nullopt;
    }
    QString totp;
    if (!a.totp_secret_ref.isEmpty()) {
        const auto t = SecretResolver::resolve(a.totp_secret_ref);
        if (!t) {
            fatal(QStringLiteral("Glasshouse — secret missing"),
                  QStringLiteral("Cannot resolve totp_secret_ref for %1 (%2).")
                      .arg(host, a.totp_secret_ref));
            return std::nullopt;
        }
        totp = *t;
    }
    return std::make_pair(*password, totp);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("glasshouse-viewer"));
    QCoreApplication::setOrganizationName(QStringLiteral("glasshouse"));
    QCoreApplication::setApplicationVersion(
        QStringLiteral(GLASSHOUSE_VERSION));

    // Pick up the MDI greenhouse glyph the .deb installs into
    // /usr/share/icons/hicolor/scalable/apps/. fromTheme() returns a
    // null QIcon if the theme path isn't populated (dev builds without
    // the .deb installed) — setWindowIcon on a null icon is a no-op,
    // so we don't bother gating the call.
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("glasshouse-viewer")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "PiKVM viewer: spawns one window per configured PiKVM, "
        "routes input via the configured HID master."));
    parser.addHelpOption();

    QCommandLineOption configOpt(
        {QStringLiteral("c"), QStringLiteral("config")},
        QStringLiteral("Path to YAML config."),
        QStringLiteral("path"), defaultConfigPath());
    QCommandLineOption onlyOpt(
        {QStringLiteral("H"), QStringLiteral("only")},
        QStringLiteral("Bring up only the window for this PiKVM host. "
                       "Default: all configured windows."),
        QStringLiteral("ip"));
    QCommandLineOption logFileOpt(
        {QStringLiteral("L"), QStringLiteral("log-file")},
        QStringLiteral("Append all log messages to FILE in addition to stderr. "
                       "No rotation; pair with logrotate(8) for long sessions."),
        QStringLiteral("path"));
    QCommandLineOption verboseOpt(
        {QStringLiteral("v"), QStringLiteral("verbose")},
        QStringLiteral("Enable debug logging for all glasshouse.* categories "
                       "(same as QT_LOGGING_RULES='glasshouse.*.debug=true')."));
    // Long form only: addVersionOption() would also claim `-v`, which
    // --verbose owns, and Qt warns on every startup when it collides.
    // Prints setApplicationVersion() — the string CI compiles in from the
    // same value the .deb carries, since the two have disagreed before.
    QCommandLineOption versionOpt(
        QStringList{QStringLiteral("version")},
        QStringLiteral("Display the version and exit."));
    parser.addOption(configOpt);
    parser.addOption(onlyOpt);
    parser.addOption(logFileOpt);
    parser.addOption(verboseOpt);
    parser.addOption(versionOpt);
    parser.process(app);

    if (parser.isSet(versionOpt)) {
        outln(QStringLiteral("%1 %2")
                  .arg(QCoreApplication::applicationName(),
                       QCoreApplication::applicationVersion()));
        return 0;
    }

    // Wire file logging early so config-load failures land in the file too.
    if (const QString logFilePath = parser.value(logFileOpt);
            !logFilePath.isEmpty()) {
        g_logFile = new QFile(logFilePath);
        if (g_logFile->open(QIODevice::WriteOnly | QIODevice::Append)) {
            g_logStream      = new QTextStream(g_logFile);
            g_originalLogger = qInstallMessageHandler(glasshouseMessageHandler);
        } else {
            errln(QStringLiteral("cannot open log file %1: %2")
                      .arg(logFilePath, g_logFile->errorString()));
            delete g_logFile;
            g_logFile = nullptr;
        }
    }

    // --verbose: enable debug for our categories without needing the env var.
    // (Categories default to Info — see logging.cpp.) An explicit
    // QT_LOGGING_RULES still layers on top for finer per-category control.
    if (parser.isSet(verboseOpt)) {
        QLoggingCategory::setFilterRules(QStringLiteral("glasshouse.*.debug=true"));
    }

    const auto result = loadConfig(parser.value(configOpt));
    if (!result.ok()) {
        QStringList lines{QStringLiteral("Config load failed (%1):")
                              .arg(parser.value(configOpt))};
        for (const auto& e : result.errors) lines << QStringLiteral("  • ") + e;
        fatal(QStringLiteral("Glasshouse — config error"), lines.join('\n'));
        return 1;
    }
    const Config cfg = *result.config;

    QString releaseHotkey = cfg.release_hotkey;
    if (releaseHotkey.isEmpty()) {
        releaseHotkey = QStringLiteral("Ctrl+Alt+Shift+Backspace");
    }
    QString fullscreenHotkey = cfg.fullscreen_hotkey;
    if (fullscreenHotkey.isEmpty()) {
        fullscreenHotkey = QStringLiteral("F11");
    }
    QString specialKeysHotkey = cfg.special_keys_hotkey;
    if (specialKeysHotkey.isEmpty()) {
        specialKeysHotkey = QStringLiteral("Ctrl+Alt+K");
    }
    QString pasteHotkey = cfg.paste_hotkey;
    if (pasteHotkey.isEmpty()) {
        pasteHotkey = QStringLiteral("Ctrl+Alt+Shift+V");
    }
    QString regionHotkey = cfg.region_hotkey;
    if (regionHotkey.isEmpty()) {
        regionHotkey = QStringLiteral("Ctrl+Alt+Shift+C");
    }
    const CaptureHotkeys hotkeys{releaseHotkey, fullscreenHotkey,
                                 specialKeysHotkey, pasteHotkey,
                                 regionHotkey};
    const QString onlyHost = parser.value(onlyOpt);

    // ------------------------------------------------------------------
    // Build one instance per configured window (or just the filtered one).
    // ------------------------------------------------------------------
    QList<Instance> instances;

    for (const auto& w : cfg.windows) {
        const MonitorRect* monitor = findMonitor(cfg, w.target_monitor);
        if (!monitor) {
            fatal(QStringLiteral("Glasshouse — config error"),
                  QStringLiteral("windows[].target_monitor=%1 does not match "
                                 "any target.monitors[].id")
                      .arg(w.target_monitor));
            return 2;
        }
        if (!onlyHost.isEmpty() && monitor->pikvm != onlyHost) continue;

        const auto authIt = cfg.auth.constFind(monitor->pikvm);
        if (authIt == cfg.auth.constEnd()) {
            fatal(QStringLiteral("Glasshouse — config error"),
                  QStringLiteral("No auth entry for %1.\n\n"
                                 "Add an `auth.\"%1\"` block to your config.")
                      .arg(monitor->pikvm));
            return 2;
        }
        const AuthSpec& a = *authIt;

        const auto secrets = resolveSecrets(monitor->pikvm, a);
        if (!secrets) return 2;

        Instance inst;
        inst.host        = monitor->pikvm;
        inst.monitorRect = QRect(monitor->origin, monitor->size);
        inst.transport   = a.transport;

        inst.window = new VideoWindow();
        inst.window->setWindowTitle(QStringLiteral("Glasshouse Viewer — %1")
                                        .arg(inst.host));
        inst.window->setConnectionStatus(QStringLiteral("%1: authenticating…")
                                             .arg(inst.host));
        inst.window->setCaptureContext(inst.monitorRect, cfg.logical_desktop,
                                       hotkeys);
        inst.window->setPersistenceHost(inst.host);
        inst.window->setCursorMarker(cfg.video.cursor_marker,
                                     cfg.video.cursor_marker_color,
                                     cfg.video.cursor_marker_size);

        // Geometry precedence: explicit YAML override > last saved
        // QSettings slot > Qt's default 1280x720 (constructor).
        if (w.geometry.width() > 0 && w.geometry.height() > 0) {
            inst.window->setGeometry(w.geometry);
        } else {
            QSettings s;
            const QByteArray saved = s.value(
                QStringLiteral("windows/%1/geometry").arg(inst.host))
                .toByteArray();
            if (!saved.isEmpty()) inst.window->restoreGeometry(saved);
        }

        if (inst.transport == VideoTransport::Janus) {
            inst.webrtc = new VideoPipeline(inst.window->videoSink(), &app);
            inst.webrtc->setJitterLatencyMs(cfg.video.webrtc_latency_ms);
            // Loud warning at WARN level so it shows up in default
            // launches (no QT_LOGGING_RULES needed). The leak is well
            // outside our pipeline boundary; see DESIGN.md §10.5.
            qCWarning(lcVideo).nospace()
                << inst.host << ": transport=janus has a known long-running "
                << "memory leak inside GStreamer's webrtcbin (~8 MB/s/stream). "
                << "Process RSS will grow unbounded; expect to restart the "
                << "viewer periodically, or switch this entry to "
                << "transport: mjpeg. See DESIGN.md §10.5.";
        } else {
            inst.mjpeg  = new MjpegPipeline(inst.window->videoSink(), &app);
        }
        inst.wallClock = new QElapsedTimer();

        PiKvmClient::Options pkOpts;
        pkOpts.host         = inst.host;
        pkOpts.user         = a.user;
        pkOpts.password     = secrets->first;
        pkOpts.totp_secret  = secrets->second;
        pkOpts.insecure_tls = a.insecure_tls;
        inst.pk = new PiKvmClient(pkOpts, &app);

        instances.append(inst);
    }

    if (instances.isEmpty()) {
        fatal(QStringLiteral("Glasshouse — nothing to show"),
              QStringLiteral("No windows to bring up. "
                             "Check config.windows[] and the --only filter."));
        return 1;
    }

    // ------------------------------------------------------------------
    // Identify the HID master instance and stand up the InputRouter.
    // ------------------------------------------------------------------
    Instance* master = nullptr;
    for (auto& inst : instances) {
        if (inst.host == cfg.hid_master) { master = &inst; break; }
    }
    if (!master) {
        fatal(QStringLiteral("Glasshouse — config error"),
              QStringLiteral("hid_master=%1 is not in the running window set. "
                             "(Did the --only filter exclude it?)")
                  .arg(cfg.hid_master));
        return 2;
    }
    auto* router = new InputRouter(master->pk, &app);

    // Region capture → Tui → clipboard. One client for every window. The
    // API key is a `secret://` reference resolved here, so it never has to
    // sit in config.yaml.
    TuiClient::Options tuiOpts;
    tuiOpts.endpoint        = cfg.ocr.endpoint;
    tuiOpts.model           = cfg.ocr.model;
    tuiOpts.reasoningEffort = cfg.ocr.reasoning_effort;
    tuiOpts.maxTokens       = cfg.ocr.max_tokens;
    tuiOpts.timeoutMs       = cfg.ocr.timeout_ms;
    if (cfg.ocr.api_key.isEmpty()) {
        qCInfo(lcOcr) << "ocr.api_key unset; region capture will select but"
                      << "not extract";
    } else if (const auto key = SecretResolver::resolve(cfg.ocr.api_key)) {
        tuiOpts.apiKey = *key;
    } else {
        qCWarning(lcOcr) << "ocr.api_key could not be resolved:"
                         << cfg.ocr.api_key;
    }
    auto* tui = new TuiClient(tuiOpts, &app);

    // Both of these report back on the window that asked, which is the
    // one the pointer was over — not necessarily the one with focus.
    auto regionRequester = std::make_shared<QPointer<VideoWindow>>();
    QObject::connect(tui, &TuiClient::finished, &app,
        [regionRequester](bool ok, const QString& text, const QString& error) {
            VideoWindow* w = *regionRequester;
            if (w) w->clearBusy();
            if (!ok) {
                if (w) w->flashStatus(
                    error == QStringLiteral("cancelled")
                        ? QStringLiteral("extraction cancelled")
                        : QStringLiteral("extract failed: %1").arg(error),
                    8000);
                return;
            }
            if (QClipboard* cb = QGuiApplication::clipboard()) {
                cb->setText(text);
            }
            if (w) w->flashStatus(
                QStringLiteral("copied %1 chars to clipboard").arg(text.size()));
        });

    // Clipboard paste runs on the HID master but is triggered from, and
    // reported on, whichever window had focus — so remember who asked.
    auto pasteRequester = std::make_shared<QPointer<VideoWindow>>();

    QObject::connect(master->pk, &PiKvmClient::pasteProgress, &app,
        [pasteRequester](int sent, int total) {
            // Single-chunk pastes complete before this is worth drawing;
            // the interesting case is a long one the user may want to stop.
            if (total <= 1024 || !*pasteRequester) return;
            (*pasteRequester)->flashStatus(
                QStringLiteral("pasting %1/%2 chars — hotkey aborts")
                    .arg(sent).arg(total), 30000);
        });
    QObject::connect(master->pk, &PiKvmClient::pasteFinished, &app,
        [pasteRequester](bool ok, const QString& reason) {
            if (!*pasteRequester) return;
            (*pasteRequester)->flashStatus(
                ok ? QStringLiteral("paste done")
                   : QStringLiteral("paste: %1").arg(reason));
        });

    // One special-keys palette shared across all windows. Toggle is
    // per-window (any window's hotkey shows/hides the same dialog).
    // Outputs route through the same InputRouter so chord and
    // type-text traffic land on the HID master regardless of which
    // window had focus when the user opened it.
    auto* specialKeys = new SpecialKeysDialog(nullptr);
    specialKeys->setCustomShortcuts(cfg.shortcuts);
    QObject::connect(specialKeys, &SpecialKeysDialog::shortcut,
                     router, &InputRouter::routeShortcut);
    QObject::connect(specialKeys, &SpecialKeysDialog::typeText,
                     router, &InputRouter::routeTypeText);

    // One MSD dialog tied to the HID master (the PiKVM whose mass
    // storage hardware is wired to the target — same box for the same
    // target in our supported topology). Toggled from any window's
    // Target → Mass Storage… menu item.
    auto* msdDialog = new MsdDialog(master->pk, nullptr);

    // ------------------------------------------------------------------
    // Per-instance signal wiring (auth → pipeline → Janus → router).
    // ------------------------------------------------------------------
    for (int i = 0; i < instances.size(); ++i) {
        Instance& inst = instances[i];

        QObject::connect(inst.pk, &PiKvmClient::authenticated, &app,
            [&inst, &app, &cfg]() {
                // Tear down stale Janus / pipeline state on re-auth.
                // The handler is idempotent so the same wiring works
                // whether this is the first login or a recovery cycle
                // triggered by Phase 6's sessionFailed / errorOccurred
                // path. The pipeline OBJECT persists; only its internal
                // GStreamer state resets via stop() → start().
                if (inst.jc) {
                    inst.jc->close();
                    inst.jc->deleteLater();
                    inst.jc = nullptr;
                }
                if (inst.webrtc) inst.webrtc->stop();
                if (inst.mjpeg)  inst.mjpeg->stop();

                // We made it through to a fresh auth — clear the
                // reconnect-scheduled gate so future errors can queue
                // a new cycle. (No-op on the very first authenticated
                // event, since the gate started at false.)
                inst.reconnectScheduled = false;

                inst.window->setConnectionStatus(
                    QStringLiteral("%1: authenticated, starting pipeline")
                        .arg(inst.host));

                if (inst.transport == VideoTransport::Janus) {
                    const QString err = inst.webrtc->start();
                    if (!err.isEmpty()) {
                        errln(err);
                        app.exit(3);
                        return;
                    }
                    inst.window->setDecoderLabel(inst.webrtc->activeDecoder());

                    JanusClient::Options jOpts;
                    jOpts.host             = inst.host;
                    jOpts.authCookieHeader = inst.pk->authCookieHeader();
                    jOpts.insecure_tls     = inst.pk->insecureTls();
                    inst.jc = new JanusClient(jOpts, &app);

                    QObject::connect(inst.jc, &JanusClient::wsConnected, inst.window,
                        [&inst]() {
                            inst.window->setConnectionStatus(
                                QStringLiteral("%1: Janus WS connected")
                                    .arg(inst.host));
                        });
                    QObject::connect(inst.jc, &JanusClient::sessionReady, inst.window,
                        [&inst](qint64 s, qint64 h) {
                            inst.window->setConnectionStatus(
                                QStringLiteral("%1: Janus session=%2 handle=%3")
                                    .arg(inst.host).arg(s).arg(h));
                        });
                    QObject::connect(inst.jc, &JanusClient::sdpOfferReceived,
                                     inst.webrtc, &VideoPipeline::handleRemoteOffer);
                    QObject::connect(inst.jc, &JanusClient::sdpOfferReceived, inst.window,
                        [&inst](const QString&) {
                            inst.window->setConnectionStatus(
                                QStringLiteral("%1: SDP offer received, answering…")
                                    .arg(inst.host));
                            inst.wallClock->restart();
                        });
                    QObject::connect(inst.webrtc, &VideoPipeline::answerReady,
                                     inst.jc, &JanusClient::sendAnswer);
                    QObject::connect(inst.jc, &JanusClient::sessionFailed, &app,
                        [&inst, &app](const QString& reason) {
                            inst.window->setConnectionStatus(
                                QStringLiteral("%1: Janus session failed (%2) "
                                               "— reconnecting…")
                                    .arg(inst.host, reason));
                            // Dedupe: only one re-auth in flight per
                            // instance. Cleared in the authenticated
                            // handler when rebuild is done.
                            if (inst.reconnectScheduled) return;
                            inst.reconnectScheduled = true;
                            QTimer::singleShot(2000, &app, [&inst]() {
                                inst.pk->start();
                            });
                        });

                    inst.jc->open();
                } else {
                    // MJPEG: no signalling, just an HTTP GET that streams
                    // multipart JPEG. Auth via the cookie kvmd issued at
                    // login. Wall-clock for first-frame latency starts
                    // from this point (analogous to "SDP offer" on the
                    // janus path).
                    inst.wallClock->restart();
                    const QString err = inst.mjpeg->start(
                        inst.host,
                        inst.pk->authCookieHeader(),
                        inst.pk->insecureTls(),
                        cfg.video.target_fps);
                    if (!err.isEmpty()) {
                        errln(err);
                        app.exit(3);
                        return;
                    }
                    inst.window->setDecoderLabel(inst.mjpeg->activeDecoder());
                    inst.window->setConnectionStatus(
                        QStringLiteral("%1: MJPEG stream open").arg(inst.host));
                }
            });

        QObject::connect(inst.pk, &PiKvmClient::errorOccurred, inst.window,
            [&inst](const QString& msg) {
                inst.window->setConnectionStatus(
                    QStringLiteral("%1: auth error: %2").arg(inst.host, msg));
            });

        // Wire firstFrame / error to whichever pipeline this instance owns.
        // Both classes expose the same two signals with identical semantics;
        // the indirection lives only here at startup. Errors trigger the
        // same re-auth path as a Janus terminal failure: tear down stale
        // state via the auth handler and rebuild from a fresh login.
        auto wireFrameSignals = [&inst, &app](auto* pipe, const char* originLabel) {
            QObject::connect(pipe, &std::remove_pointer_t<decltype(pipe)>::firstFrameRendered,
                inst.window, [&inst, originLabel]() {
                    const qint64 ms = inst.wallClock->isValid()
                        ? inst.wallClock->elapsed() : -1;
                    inst.window->setLatencyLabel(
                        ms >= 0
                            ? QStringLiteral("first frame @ %1 ms from %2")
                                  .arg(ms).arg(QString::fromLatin1(originLabel))
                            : QStringLiteral("first frame"));
                });
            QObject::connect(pipe, &std::remove_pointer_t<decltype(pipe)>::errorOccurred,
                &app, [&inst, &app](const QString& msg) {
                    inst.window->setConnectionStatus(
                        QStringLiteral("%1: pipeline error: %2 — reconnecting…")
                            .arg(inst.host, msg));
                    // GStreamer can fire 3+ errors per failed pipeline
                    // (transport error → internal-stream-error → EOS-no-pads).
                    // Without this gate each becomes its own queued
                    // re-auth and the rebuilds compound exponentially.
                    if (inst.reconnectScheduled) return;
                    inst.reconnectScheduled = true;
                    QTimer::singleShot(2000, &app, [&inst]() {
                        inst.pk->start();
                    });
                });
        };
        if (inst.transport == VideoTransport::Janus) {
            wireFrameSignals(inst.webrtc, "SDP offer");
        } else {
            wireFrameSignals(inst.mjpeg, "stream open");
        }

        // Input fan-in: every window's HID signals route through the
        // master client. Per DESIGN.md §5.3, the captured window's
        // CoordTransform applies (already done inside VideoWindow), but
        // the wire output goes via the HID master.
        QObject::connect(inst.window, &VideoWindow::mouseMoved,
                         router, &InputRouter::routeMouseMove);
        QObject::connect(inst.window, &VideoWindow::mouseButton,
                         router, &InputRouter::routeMouseButton);
        QObject::connect(inst.window, &VideoWindow::mouseWheel,
                         router, &InputRouter::routeMouseWheel);
        QObject::connect(inst.window, &VideoWindow::keyEvent,
                         router, &InputRouter::routeKey);

        // Region capture: the window hands back a crop of its frozen
        // frame; we encode and send it. PNG rather than JPEG so text
        // edges stay crisp for the model, and a 1920x1080 frame is 2.07 MP
        // — just under Tui's ~2.1 MP downscale threshold, so a full-frame
        // selection survives at native resolution.
        QObject::connect(inst.window, &VideoWindow::regionCaptured, inst.window,
                         [&inst, tui, &cfg, regionRequester](const QImage& image) {
            if (!tui->configured()) {
                inst.window->flashStatus(QStringLiteral(
                    "region capture: set ocr.api_key to extract text"), 8000);
                return;
            }
            QByteArray png;
            QBuffer buf(&png);
            buf.open(QIODevice::WriteOnly);
            if (!image.save(&buf, "PNG")) {
                qCWarning(lcOcr) << "region capture: PNG encode failed for"
                                 << image.size();
                inst.window->flashStatus(
                    QStringLiteral("region capture: could not encode image"));
                return;
            }
            buf.close();

            *regionRequester = inst.window;
            inst.window->setBusy(QStringLiteral("extracting %1×%2")
                                     .arg(image.width()).arg(image.height()));
            tui->extractText(png, cfg.ocr.prompt);
        });

        // A second region hotkey press while the spinner is up abandons
        // the request rather than queueing another.
        QObject::connect(inst.window, &VideoWindow::busyCancelRequested,
                         tui, &TuiClient::cancel);

        // Paste hotkey: type the host clipboard onto the target via the
        // HID master's server-side keymap. Text only. Pressing the hotkey
        // again while a long paste is still running aborts the rest — the
        // abort lives here rather than behind a confirmation dialog on the
        // way in, because a modal dialog raised while the video widget
        // holds grabMouse/grabKeyboard cannot be clicked or dismissed.
        QObject::connect(inst.window, &VideoWindow::pasteClipboardRequested,
                         inst.window, [&inst, master, router, pasteRequester]() {
            *pasteRequester = inst.window;

            if (master->pk->pasteBusy()) {
                master->pk->cancelPaste();
                return;
            }

            const QClipboard* cb = QGuiApplication::clipboard();
            const QMimeData*  md = cb ? cb->mimeData() : nullptr;
            if (!md || !md->hasText()) {
                qCInfo(lcHid) << "paste: clipboard holds no text";
                inst.window->flashStatus(
                    QStringLiteral("paste: clipboard holds no text"));
                return;
            }

            int dropped = 0;
            const QString text = sanitiseForPrint(cb->text(), &dropped);
            if (text.isEmpty()) {
                qCWarning(lcHid) << "paste: nothing in the clipboard is"
                                 << "typeable on the target keymap";
                inst.window->flashStatus(
                    QStringLiteral("paste: nothing typeable in clipboard"));
                return;
            }

            inst.window->flashStatus(
                dropped == 0
                    ? QStringLiteral("pasting %1 chars…").arg(text.size())
                    : QStringLiteral("pasting %1 chars (%2 unsupported "
                                     "dropped)…").arg(text.size()).arg(dropped));

            // The modifier key-ups VideoWindow just sent went out on the
            // state WebSocket; this paste goes over HTTPS. Two sockets, no
            // ordering guarantee between them — and losing that race means
            // kvmd types the whole clipboard while it still believes
            // Ctrl+Alt+Shift are held. A short gap is cheap insurance.
            QTimer::singleShot(50, router, [router, text]() {
                router->routeTypeText(text, /*slow=*/false, /*delayMs=*/0);
            });
        });

        // Capture is session-wide; whichever window the user clicked
        // becomes the holder, but the badge follows the cursor so it marks
        // the window actually being worked in. No cross-window release
        // needed — there's only one Qt grab in flight at a time.
        QObject::connect(inst.window, &VideoWindow::captureStatusChanged, inst.window,
            [&inst](bool sessionActive, bool focused) {
                if (!sessionActive) {
                    inst.window->setConnectionStatus(
                        QStringLiteral("%1: capture OFF").arg(inst.host));
                } else if (focused) {
                    inst.window->setConnectionStatus(
                        QStringLiteral("%1: capture ON — release hotkey to stop")
                            .arg(inst.host));
                } else {
                    inst.window->setConnectionStatus(
                        QStringLiteral("%1: capture ON — cursor elsewhere")
                            .arg(inst.host));
                }
            });

        // Special-keys palette: hotkey on any window toggles the same
        // shared dialog.
        QObject::connect(inst.window, &VideoWindow::showSpecialKeysRequested,
                         specialKeys, &SpecialKeysDialog::toggle);

        // Target menu's ATX actions route through the same HID-master
        // PiKvmClient as everything else. The user has confirmed in the
        // VideoWindow's QMessageBox before this fires.
        QObject::connect(inst.window, &VideoWindow::atxClickRequested,
                         router, &InputRouter::routeAtxClick);

        // Target → Mass Storage… opens (or hides) the shared MsdDialog.
        QObject::connect(inst.window, &VideoWindow::showMsdRequested,
                         msdDialog, &MsdDialog::toggle);
    }

    // Tell every window about every other window so the holder's
    // sibling-aware transform lookup works. Cheap O(N²) loop — N is
    // tiny (usually 2 or 3).
    {
        QList<QPointer<VideoWindow>> all;
        for (auto& inst : instances) all.append(QPointer<VideoWindow>(inst.window));
        for (auto& inst : instances) inst.window->setSiblings(all);
    }

    // ------------------------------------------------------------------
    // Target-layout drift watch (DESIGN §10.8).
    //
    // The coord transform is purely proportional: a monitor's share of the
    // logical desktop is all that reaches the wire, so the target's real
    // pixel resolution never enters the maths. That means every monitor
    // changing resolution by the same factor is harmless — but one monitor
    // changing while another doesn't shifts the shares, and the cursor then
    // lands on the wrong part of the target desktop.
    //
    // Each PiKVM's capture resolution is the decoded video size, so we can
    // compare the real per-monitor scale factors against the config and say
    // which case we're in. What we *can't* see is the target's arrangement
    // (which monitor is left of which) — PiKVM has no visibility into that,
    // so a reordering only shows up as the mapping being wrong.
    // ------------------------------------------------------------------
    {
        auto lastReport = std::make_shared<QString>();
        auto recheck = [&instances, lastReport]() {
            struct Entry { QString host; QSize cfg; QSize actual; };
            QList<Entry> entries;
            for (auto& inst : instances) {
                QSize actual;
                if (auto* sink = inst.window->videoSink()) actual = sink->videoSize();
                entries.append({inst.host, inst.monitorRect.size(), actual});
            }

            // Scale factors per monitor; mapping stays correct as long as
            // every monitor shares the same width scale and the same height
            // scale (see the block comment above).
            bool allKnown = true, uniform = true, exact = true;
            double kx0 = 0.0, ky0 = 0.0;
            for (const auto& e : entries) {
                if (e.actual.isEmpty() || e.cfg.isEmpty()) { allKnown = false; continue; }
                const double kx = double(e.actual.width())  / double(e.cfg.width());
                const double ky = double(e.actual.height()) / double(e.cfg.height());
                if (kx0 == 0.0) { kx0 = kx; ky0 = ky; }
                if (qAbs(kx - kx0) > 0.01 || qAbs(ky - ky0) > 0.01) uniform = false;
                if (e.actual != e.cfg) exact = false;
            }

            for (int i = 0; i < entries.size() && i < instances.size(); ++i) {
                const Entry& e = entries[i];
                // Terse on the happy path — the status bar is tight on a
                // half-width window — and loud only when it matters.
                QString text;
                if (e.actual.isEmpty()) {
                    text = QStringLiteral("—");
                } else if (e.actual == e.cfg) {
                    text = QStringLiteral("%1×%2")
                               .arg(e.actual.width()).arg(e.actual.height());
                } else {
                    text = QStringLiteral("%1%2×%3 ≠ config %4×%5")
                               .arg(allKnown && uniform ? QString()
                                                        : QStringLiteral("⚠ "))
                               .arg(e.actual.width()).arg(e.actual.height())
                               .arg(e.cfg.width()).arg(e.cfg.height());
                }
                instances[i].window->setResolutionLabel(text);
            }

            if (!allKnown || exact) return;  // nothing to say yet, or all matches

            QStringList detail;
            for (const auto& e : entries) {
                detail << QStringLiteral("%1 %2×%3 (config %4×%5)")
                              .arg(e.host)
                              .arg(e.actual.width()).arg(e.actual.height())
                              .arg(e.cfg.width()).arg(e.cfg.height());
            }
            const QString report = detail.join(QStringLiteral("; "));
            if (report == *lastReport) return;   // only log on change
            *lastReport = report;

            if (uniform) {
                qCInfo(lcVideo).nospace()
                    << "target capture resolution differs from config, but every "
                    << "monitor scaled by the same factor, so the mouse mapping "
                    << "is unaffected: " << report;
            } else {
                qCWarning(lcVideo).nospace()
                    << "TARGET LAYOUT MISMATCH — the monitors no longer have the "
                    << "proportions target.monitors describes, so mouse "
                    << "coordinates will land on the wrong part of the target "
                    << "desktop: " << report
                    << ". Fix the target's display settings, or update "
                    << "target.monitors / target.logical_desktop to match. "
                    << "(A left/right reordering on the target looks the same "
                    << "from here and can't be detected — check that too.)";
            }
        };

        for (auto& inst : instances) {
            if (auto* sink = inst.window->videoSink()) {
                QObject::connect(sink, &QVideoSink::videoSizeChanged, &app, recheck);
            }
        }
        recheck();
    }

    // ------------------------------------------------------------------
    // Steady-state video telemetry (DESIGN §10.2). Poll each pipeline's
    // delivered / coalesced frame counters once a second; show effective
    // render FPS + the GUI-thread coalesced-drop count in the status bar,
    // and log at debug. This surfaces throughput and frame-shedding — the
    // behaviour the buffering fix bounds. True glass-to-glass latency still
    // needs the manual clock-on-screen procedure documented in §10.2.
    // ------------------------------------------------------------------
    {
        auto statsClock = std::make_shared<QElapsedTimer>();
        statsClock->start();
        auto* statsTimer = new QTimer(&app);
        statsTimer->setInterval(1000);
        QObject::connect(statsTimer, &QTimer::timeout, &app,
            [&instances, statsClock]() {
                const double secs = statsClock->restart() / 1000.0;
                for (auto& inst : instances) {
                    const bool janus = (inst.transport == VideoTransport::Janus);
                    const quint64 delivered = janus
                        ? (inst.webrtc ? inst.webrtc->framesDelivered() : 0)
                        : (inst.mjpeg  ? inst.mjpeg->framesDelivered()  : 0);
                    const quint64 coalesced = janus
                        ? (inst.webrtc ? inst.webrtc->framesCoalesced() : 0)
                        : (inst.mjpeg  ? inst.mjpeg->framesCoalesced()  : 0);
                    quint64 base = inst.lastDelivered;
                    if (delivered < base) base = 0;  // pipeline rebuilt (reconnect)
                    const double fps = secs > 0.0
                        ? double(delivered - base) / secs : 0.0;
                    inst.lastDelivered = delivered;
                    inst.window->setVideoStats(
                        QStringLiteral("%1 fps · %2 coalesced")
                            .arg(fps, 0, 'f', 1).arg(coalesced));
                    qCDebug(lcVideo).nospace()
                        << inst.host << " video: " << fps
                        << " fps, coalesced=" << coalesced;
                }
            });
        statsTimer->start();
    }

    // ------------------------------------------------------------------
    // Show all windows, kick off auth on every PiKVM.
    // ------------------------------------------------------------------
    for (auto& inst : instances) inst.window->show();
    for (auto& inst : instances) inst.pk->start();

    const int rc = app.exec();

    // Orderly teardown: close Janus first (sends WS close), then stop
    // pipelines, then stop PiKvmClients. Anything still alive is freed
    // by QApplication's destructor via the &app parent.
    for (auto& inst : instances) {
        if (inst.jc) inst.jc->close();
    }
    for (auto& inst : instances) {
        if (inst.webrtc) inst.webrtc->stop();
        if (inst.mjpeg)  inst.mjpeg->stop();
    }
    for (auto& inst : instances) {
        if (inst.pk) inst.pk->stop();
        delete inst.wallClock;
    }
    delete specialKeys;
    delete msdDialog;

    // Tear down file logging cleanly so the last few log lines hit disk.
    if (g_logStream) {
        if (g_originalLogger) qInstallMessageHandler(g_originalLogger);
        g_logStream->flush();
        delete g_logStream;
        g_logStream = nullptr;
    }
    if (g_logFile) {
        g_logFile->close();
        delete g_logFile;
        g_logFile = nullptr;
    }
    return rc;
}
