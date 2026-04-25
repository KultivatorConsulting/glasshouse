// Probe driver for JanusClient: auths against the PiKVM, opens /janus/ws,
// runs info+create+attach+watch, prints the SDP offer, exits. No GStreamer,
// no video decode — this is the minimum round-trip that proves the cookie,
// the subprotocol, and the plugin name against a stock PiKVM.

#include "config.h"
#include "janusclient.h"
#include "pikvmclient.h"
#include "secrets.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

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

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("glasshouse-janusprobe"));
    QCoreApplication::setOrganizationName(QStringLiteral("glasshouse"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Janus signalling probe: auth, open /janus/ws, info → create → "
        "attach → watch, print the SDP offer and exit."));
    parser.addHelpOption();

    QCommandLineOption configOpt(
        {QStringLiteral("c"), QStringLiteral("config")},
        QStringLiteral("Path to YAML config."),
        QStringLiteral("path"), defaultConfigPath());
    QCommandLineOption hostOpt(
        {QStringLiteral("H"), QStringLiteral("host")},
        QStringLiteral("PiKVM to probe (must be in config). "
                       "Defaults to hid_master."),
        QStringLiteral("ip"));
    QCommandLineOption pluginOpt(
        {QStringLiteral("p"), QStringLiteral("plugin")},
        QStringLiteral("Janus plugin name to attach."),
        QStringLiteral("name"),
        QStringLiteral("janus.plugin.ustreamer"));
    QCommandLineOption timeoutOpt(
        {QStringLiteral("t"), QStringLiteral("timeout")},
        QStringLiteral("Fail after N seconds if no SDP offer arrives."),
        QStringLiteral("secs"),
        QStringLiteral("15"));
    parser.addOption(configOpt);
    parser.addOption(hostOpt);
    parser.addOption(pluginOpt);
    parser.addOption(timeoutOpt);
    parser.process(app);

    const auto cfgResult = loadConfig(parser.value(configOpt));
    if (!cfgResult.ok()) {
        errln(QStringLiteral("config load failed:"));
        for (const auto& e : cfgResult.errors) errln(QStringLiteral("  - ") + e);
        return 1;
    }
    const Config cfg = *cfgResult.config;

    QString targetHost = parser.value(hostOpt);
    if (targetHost.isEmpty()) targetHost = cfg.hid_master;

    const auto authIt = cfg.auth.constFind(targetHost);
    if (authIt == cfg.auth.constEnd()) {
        errln(QStringLiteral("no auth entry for %1").arg(targetHost));
        return 2;
    }
    const AuthSpec& a = *authIt;

    const auto password = SecretResolver::resolve(a.password_ref);
    if (!password) {
        errln(QStringLiteral("cannot resolve password_ref for %1 (%2)")
                  .arg(targetHost, a.password_ref));
        return 2;
    }
    QString totp;
    if (!a.totp_secret_ref.isEmpty()) {
        const auto t = SecretResolver::resolve(a.totp_secret_ref);
        if (!t) {
            errln(QStringLiteral("cannot resolve totp_secret_ref for %1")
                      .arg(targetHost));
            return 2;
        }
        totp = *t;
    }

    PiKvmClient::Options pkOpts;
    pkOpts.host         = targetHost;
    pkOpts.user         = a.user;
    pkOpts.password     = *password;
    pkOpts.totp_secret  = totp;
    pkOpts.insecure_tls = a.insecure_tls;
    auto* pk = new PiKvmClient(pkOpts, &app);

    const QString pluginName = parser.value(pluginOpt);
    JanusClient* jc = nullptr;

    QObject::connect(pk, &PiKvmClient::authenticated, &app,
        [pk, &jc, &app, targetHost, pluginName]() {
            JanusClient::Options jOpts;
            jOpts.host             = targetHost;
            jOpts.authCookieHeader = pk->authCookieHeader();
            jOpts.insecure_tls     = pk->insecureTls();
            jOpts.pluginName       = pluginName;
            jc = new JanusClient(jOpts, &app);

            QObject::connect(jc, &JanusClient::sessionReady, &app,
                [](qint64 s, qint64 h) {
                    QTextStream(stdout)
                        << QStringLiteral("session=%1 handle=%2\n")
                               .arg(s).arg(h);
                });
            QObject::connect(jc, &JanusClient::sdpOfferReceived, &app,
                [&app, jc](const QString& sdp) {
                    QTextStream out(stdout);
                    out << "---- SDP offer ----\n"
                        << sdp
                        << "---- end ----\n";
                    out.flush();
                    // The probe never answers, so the plugin will drop us a
                    // moment later. Detach sessionFailed so that expected
                    // teardown doesn't mask the success exit.
                    QObject::disconnect(jc, &JanusClient::sessionFailed,
                                        nullptr, nullptr);
                    QTimer::singleShot(250, &app, [&app] { app.exit(0); });
                });
            QObject::connect(jc, &JanusClient::sessionFailed, &app,
                [&app](const QString& reason) {
                    errln(QStringLiteral("probe failed: %1").arg(reason));
                    app.exit(4);
                });

            jc->open();
        });

    QObject::connect(pk, &PiKvmClient::errorOccurred, &app,
        [&app](const QString& msg) {
            errln(QStringLiteral("auth error: %1").arg(msg));
            app.exit(3);
        });

    const int timeoutSecs = parser.value(timeoutOpt).toInt();
    QTimer::singleShot(timeoutSecs * 1000, &app, [&app] {
        errln(QStringLiteral("timeout: no SDP offer received"));
        app.exit(5);
    });

    pk->start();
    const int rc = app.exec();
    if (jc) jc->close();
    pk->stop();
    return rc;
}
