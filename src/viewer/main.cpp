// PiKVM viewer: loads YAML config, brings up one VideoWindow per
// configured window, each with its own auth / Janus / decode pipeline,
// and routes click-to-capture input through the configured HID master.

#include "config.h"
#include "input_router.h"
#include "janusclient.h"
#include "logging.h"
#include "mjpegpipeline.h"
#include "pikvmclient.h"
#include "secrets.h"
#include "specialkeysdialog.h"
#include "videopipeline.h"
#include "videowindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

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
};

// Look up the MonitorRect for a given window spec.
const MonitorRect* findMonitor(const Config& cfg, int targetMonitorId) {
    for (const auto& m : cfg.monitors) {
        if (m.id == targetMonitorId) return &m;
    }
    return nullptr;
}

// Resolve secrets for a PiKVM's auth entry. Returns std::nullopt and
// writes diagnostic to stderr on failure.
std::optional<std::pair<QString, QString>>
resolveSecrets(const QString& host, const AuthSpec& a) {
    const auto password = SecretResolver::resolve(a.password_ref);
    if (!password) {
        errln(QStringLiteral("cannot resolve password_ref for %1 (%2)")
                  .arg(host, a.password_ref));
        return std::nullopt;
    }
    QString totp;
    if (!a.totp_secret_ref.isEmpty()) {
        const auto t = SecretResolver::resolve(a.totp_secret_ref);
        if (!t) {
            errln(QStringLiteral("cannot resolve totp_secret_ref for %1")
                      .arg(host));
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
    parser.addOption(configOpt);
    parser.addOption(onlyOpt);
    parser.process(app);

    const auto result = loadConfig(parser.value(configOpt));
    if (!result.ok()) {
        errln(QStringLiteral("config load failed:"));
        for (const auto& e : result.errors) errln(QStringLiteral("  - ") + e);
        return 1;
    }
    const Config cfg = *result.config;

    QString releaseHotkey = cfg.release_hotkey;
    if (releaseHotkey.isEmpty()) {
        releaseHotkey = QStringLiteral("Ctrl+Alt+Shift+Escape");
    }
    QString fullscreenHotkey = cfg.fullscreen_hotkey;
    if (fullscreenHotkey.isEmpty()) {
        fullscreenHotkey = QStringLiteral("F11");
    }
    QString specialKeysHotkey = cfg.special_keys_hotkey;
    if (specialKeysHotkey.isEmpty()) {
        specialKeysHotkey = QStringLiteral("Ctrl+Alt+K");
    }
    const QString onlyHost = parser.value(onlyOpt);

    // ------------------------------------------------------------------
    // Build one instance per configured window (or just the filtered one).
    // ------------------------------------------------------------------
    QList<Instance> instances;

    for (const auto& w : cfg.windows) {
        const MonitorRect* monitor = findMonitor(cfg, w.target_monitor);
        if (!monitor) {
            errln(QStringLiteral("windows[].target_monitor=%1 not found")
                      .arg(w.target_monitor));
            return 2;
        }
        if (!onlyHost.isEmpty() && monitor->pikvm != onlyHost) continue;

        const auto authIt = cfg.auth.constFind(monitor->pikvm);
        if (authIt == cfg.auth.constEnd()) {
            errln(QStringLiteral("no auth entry for %1").arg(monitor->pikvm));
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
                                       releaseHotkey, fullscreenHotkey,
                                       specialKeysHotkey);
        inst.window->setPersistenceHost(inst.host);

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
        errln(QStringLiteral("no windows to bring up "
                             "(check config.windows / --only filter)"));
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
        errln(QStringLiteral(
            "hid_master=%1 not in the running window set "
            "(--only filter excluded it?)").arg(cfg.hid_master));
        return 2;
    }
    auto* router = new InputRouter(master->pk, &app);

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

    // ------------------------------------------------------------------
    // Per-instance signal wiring (auth → pipeline → Janus → router).
    // ------------------------------------------------------------------
    for (int i = 0; i < instances.size(); ++i) {
        Instance& inst = instances[i];

        QObject::connect(inst.pk, &PiKvmClient::authenticated, &app,
            [&inst, &app]() {
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
                            // Re-auth after a small breather. The auth
                            // handler tears down stale state and rebuilds.
                            // PiKvmClient's own backoff handles network
                            // bumps if /api/auth/login itself is down.
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
                        inst.pk->insecureTls());
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

        // Capture is session-wide; whichever window the user clicked
        // becomes the holder. The holder shows "capture ON"; siblings
        // keep their normal status. No cross-window release needed —
        // there's only one Qt grab in flight at a time.
        QObject::connect(inst.window, &VideoWindow::captureStateChanged, inst.window,
            [&inst](bool on) {
                inst.window->setConnectionStatus(on
                    ? QStringLiteral("%1: capture ON — release hotkey to stop")
                          .arg(inst.host)
                    : QStringLiteral("%1: capture OFF").arg(inst.host));
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
    return rc;
}
