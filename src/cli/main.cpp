// Phase 1 CLI harness: loads a glasshouse YAML config, opens a PiKvmClient
// against each configured PiKVM (or the one named by --host), streams state
// events to stdout, and optionally fires the §10.1 5-point coord sequence on
// the HID master as an end-to-end smoke test.

#include "config.h"
#include "logging.h"
#include "pikvmclient.h"
#include "secrets.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QList>
#include <QPair>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#include <csignal>

using namespace glasshouse;

namespace {

// Matches DESIGN.md §10.1: center, four corners, back to center. Visible on
// the target as a clockwise sweep from top-left.
const QList<QPair<int,int>> kTestSequence = {
    { 0,      0      },
    { -32768, -32768 },
    { 32767,  -32768 },
    { 32767,  32767  },
    { -32768, 32767  },
    { 0,      0      },
};

QString defaultConfigPath() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
         + QStringLiteral("/glasshouse/config.yaml");
}

// Writes to stdout with explicit flush so `glasshouse-cli | tee` behaves
// predictably when output is redirected.
void sayln(const QString& line) {
    QTextStream out(stdout);
    out << line << '\n';
    out.flush();
}

void errln(const QString& line) {
    QTextStream err(stderr);
    err << line << '\n';
    err.flush();
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("glasshouse-cli"));
    QCoreApplication::setOrganizationName(QStringLiteral("glasshouse"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Phase 1 PiKVM client harness: proves auth + state WS + "
                       "HID send end-to-end. Does not render video."));
    parser.addHelpOption();

    QCommandLineOption configOpt(
        {QStringLiteral("c"), QStringLiteral("config")},
        QStringLiteral("Path to YAML config (default: $XDG_CONFIG_HOME/glasshouse/config.yaml)."),
        QStringLiteral("path"), defaultConfigPath());
    QCommandLineOption hostOpt(
        {QStringLiteral("H"), QStringLiteral("host")},
        QStringLiteral("Only connect to the named PiKVM (must be in config)."),
        QStringLiteral("ip"));
    QCommandLineOption testSeqOpt(
        {QStringLiteral("send-test-sequence")},
        QStringLiteral("On the HID master only: after the WS is ready, send the 5-point "
                       "coord sequence from DESIGN.md §10.1 and exit."));
    parser.addOption(configOpt);
    parser.addOption(hostOpt);
    parser.addOption(testSeqOpt);
    parser.process(app);

    const QString configPath  = parser.value(configOpt);
    const QString hostFilter  = parser.value(hostOpt);
    const bool    sendTestSeq = parser.isSet(testSeqOpt);

    // --- load + validate config -------------------------------------------
    const auto result = loadConfig(configPath);
    if (!result.ok()) {
        errln(QStringLiteral("config load failed (%1):").arg(configPath));
        for (const auto& e : result.errors) errln(QStringLiteral("  - ") + e);
        return 1;
    }
    const Config cfg = *result.config;

    sayln(QStringLiteral("Loaded %1 (%2 monitor(s), hid_master=%3)")
              .arg(configPath)
              .arg(cfg.monitors.size())
              .arg(cfg.hid_master));

    // --- instantiate clients ----------------------------------------------
    QList<PiKvmClient*> clients;
    PiKvmClient*        master = nullptr;

    for (const auto& m : cfg.monitors) {
        if (!hostFilter.isEmpty() && m.pikvm != hostFilter) continue;

        const auto authIt = cfg.auth.constFind(m.pikvm);
        if (authIt == cfg.auth.constEnd()) {
            errln(QStringLiteral("no auth entry for %1; skipping").arg(m.pikvm));
            continue;
        }
        const AuthSpec& a = *authIt;

        const auto password = SecretResolver::resolve(a.password_ref);
        if (!password) {
            errln(QStringLiteral("cannot resolve password_ref for %1 (%2)")
                      .arg(m.pikvm, a.password_ref));
            return 2;
        }

        QString totp;
        if (!a.totp_secret_ref.isEmpty()) {
            const auto t = SecretResolver::resolve(a.totp_secret_ref);
            if (!t) {
                errln(QStringLiteral("cannot resolve totp_secret_ref for %1").arg(m.pikvm));
                return 2;
            }
            totp = *t;
        }

        PiKvmClient::Options opts;
        opts.host         = m.pikvm;
        opts.user         = a.user;
        opts.password     = *password;
        opts.totp_secret  = totp;
        opts.insecure_tls = a.insecure_tls;

        auto* client = new PiKvmClient(opts, &app);
        client->setObjectName(m.pikvm);
        clients.append(client);
        if (m.pikvm == cfg.hid_master) master = client;

        const QString host = m.pikvm;  // captured by value into lambdas

        QObject::connect(client, &PiKvmClient::authenticated, client, [host]() {
            sayln(QStringLiteral("[%1] authenticated").arg(host));
        });
        QObject::connect(client, &PiKvmClient::connected, client, [host]() {
            sayln(QStringLiteral("[%1] WS connected").arg(host));
        });
        QObject::connect(client, &PiKvmClient::initialStateReceived, client,
            [host](const HidState& h) {
                sayln(QStringLiteral(
                    "[%1] initial state — hid.online=%2 mouse.absolute=%3 "
                    "mouse.online=%4 mouse.outputs.active=%5 keyboard.online=%6")
                    .arg(host)
                    .arg(h.online)
                    .arg(h.mouse_absolute)
                    .arg(h.mouse_online)
                    .arg(h.mouse_outputs_active)
                    .arg(h.keyboard_online));
            });
        QObject::connect(client, &PiKvmClient::hidStateChanged, client,
            [host](const HidState& h) {
                sayln(QStringLiteral(
                    "[%1] hid state delta — online=%2 mouse.abs=%3 mouse.on=%4 kbd.on=%5")
                    .arg(host)
                    .arg(h.online).arg(h.mouse_absolute)
                    .arg(h.mouse_online).arg(h.keyboard_online));
            });
        QObject::connect(client, &PiKvmClient::rawStateEvent, client,
            [host](const QString& type, const QJsonObject&) {
                sayln(QStringLiteral("[%1] state event: %2").arg(host, type));
            });
        QObject::connect(client, &PiKvmClient::disconnected, client,
            [host](const QString& why) {
                sayln(QStringLiteral("[%1] WS disconnected: %2").arg(host, why));
            });
        QObject::connect(client, &PiKvmClient::errorOccurred, client,
            [host](const QString& msg) {
                errln(QStringLiteral("[%1] error: %2").arg(host, msg));
            });

        client->start();
    }

    if (clients.isEmpty()) {
        errln(QStringLiteral("no PiKVMs to connect to (check --host filter)"));
        return 3;
    }

    // --- optional test sequence on the HID master -------------------------
    if (sendTestSeq) {
        if (!master) {
            errln(QStringLiteral(
                "--send-test-sequence set but HID master '%1' is not in the active "
                "client list (filtered out by --host?)").arg(cfg.hid_master));
            return 4;
        }

        QObject::connect(master, &PiKvmClient::initialStateReceived, master,
            [master](const HidState& h) {
                if (!h.mouse_absolute) {
                    errln(QStringLiteral(
                        "[%1] HID master is not in absolute mode; cannot run "
                        "test sequence. Fix via:\n"
                        "    curl -k -X POST -u USER:PASS "
                        "'https://%1/api/hid/set_params?mouse_output=usb'")
                        .arg(master->host()));
                    QCoreApplication::exit(5);
                    return;
                }

                auto* timer = new QTimer(master);
                timer->setInterval(1000);
                timer->setProperty("step", 0);
                QObject::connect(timer, &QTimer::timeout, master, [master, timer]() {
                    const int i = timer->property("step").toInt();
                    if (i >= kTestSequence.size()) {
                        timer->stop();
                        timer->deleteLater();
                        QTimer::singleShot(500, QCoreApplication::instance(),
                                           &QCoreApplication::quit);
                        return;
                    }
                    const auto [x, y] = kTestSequence[i];
                    sayln(QStringLiteral("[%1] mouse_move to (%2, %3)")
                              .arg(master->host()).arg(x).arg(y));
                    master->sendMouseMove(x, y);
                    timer->setProperty("step", i + 1);
                });
                timer->start();
            });
    }

    // SIGINT -> graceful exit. QCoreApplication::quit() dispatches via the
    // event loop and is safe enough for a CLI harness.
    std::signal(SIGINT,  [](int) { QCoreApplication::quit(); });
    std::signal(SIGTERM, [](int) { QCoreApplication::quit(); });

    const int rc = app.exec();
    for (auto* c : clients) c->stop();
    return rc;
}
