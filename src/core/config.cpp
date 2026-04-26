#include "config.h"
#include "logging.h"

#include <QFile>
#include <QSet>
#include <yaml-cpp/yaml.h>

namespace glasshouse {

namespace {

QString yamlString(const YAML::Node& node, const char* key) {
    if (!node[key]) return {};
    try {
        return QString::fromStdString(node[key].as<std::string>());
    } catch (const YAML::Exception&) {
        return {};
    }
}

template <typename T>
T yamlScalarOr(const YAML::Node& node, const char* key, T fallback) {
    if (!node[key]) return fallback;
    try { return node[key].as<T>(); } catch (...) { return fallback; }
}

}  // namespace

QStringList validateConfig(const Config& cfg) {
    QStringList errors;

    // 1. monitors count matches windows count
    if (cfg.monitors.size() != cfg.windows.size()) {
        errors.append(QStringLiteral(
            "target.monitors has %1 entries but windows has %2 — they must match")
            .arg(cfg.monitors.size()).arg(cfg.windows.size()));
    }

    // Index monitor IDs.
    QSet<int> monitorIds;
    for (const auto& m : cfg.monitors) monitorIds.insert(m.id);

    // 2. every window.target_monitor references an existing monitor
    for (int i = 0; i < cfg.windows.size(); ++i) {
        if (!monitorIds.contains(cfg.windows[i].target_monitor)) {
            errors.append(QStringLiteral(
                "windows[%1].target_monitor=%2 does not reference any "
                "target.monitors[].id")
                .arg(i).arg(cfg.windows[i].target_monitor));
        }
    }

    // 3. hid_master matches some monitor's pikvm
    bool hidMasterFound = false;
    for (const auto& m : cfg.monitors) {
        if (m.pikvm == cfg.hid_master) { hidMasterFound = true; break; }
    }
    if (!hidMasterFound) {
        errors.append(QStringLiteral(
            "hid_master='%1' does not match any target.monitors[].pikvm")
            .arg(cfg.hid_master));
    }

    // 4. monitor rects within logical_desktop
    const QRect desktop(QPoint(0, 0), cfg.logical_desktop);
    for (int i = 0; i < cfg.monitors.size(); ++i) {
        const auto& m = cfg.monitors[i];
        const QRect r(m.origin, m.size);
        if (!desktop.contains(r)) {
            errors.append(QStringLiteral(
                "target.monitors[%1] rect (x=%2 y=%3 w=%4 h=%5) extends outside "
                "logical_desktop (%6x%7)")
                .arg(i)
                .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height())
                .arg(desktop.width()).arg(desktop.height()));
        }
    }

    // 5. monitor rects must not overlap
    for (int i = 0; i < cfg.monitors.size(); ++i) {
        const QRect a(cfg.monitors[i].origin, cfg.monitors[i].size);
        for (int j = i + 1; j < cfg.monitors.size(); ++j) {
            const QRect b(cfg.monitors[j].origin, cfg.monitors[j].size);
            if (a.intersects(b)) {
                errors.append(QStringLiteral(
                    "target.monitors[%1] and target.monitors[%2] overlap")
                    .arg(i).arg(j));
            }
        }
    }

    return errors;
}

ConfigResult loadConfig(const QString& path) {
    ConfigResult result;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.errors.append(QStringLiteral("cannot open %1: %2")
                                 .arg(path, f.errorString()));
        return result;
    }
    const std::string yaml = f.readAll().toStdString();

    YAML::Node root;
    try {
        root = YAML::Load(yaml);
    } catch (const YAML::ParserException& e) {
        result.errors.append(QStringLiteral("YAML parse error: %1")
                                 .arg(QString::fromStdString(e.what())));
        return result;
    }

    Config cfg;

    // target.logical_desktop
    if (const auto t = root["target"]; t && t["logical_desktop"]) {
        const auto ld = t["logical_desktop"];
        cfg.logical_desktop = QSize(
            yamlScalarOr<int>(ld, "width",  0),
            yamlScalarOr<int>(ld, "height", 0));
    } else {
        result.errors.append(QStringLiteral("target.logical_desktop is required"));
    }

    // target.monitors
    if (const auto t = root["target"]; t && t["monitors"]) {
        for (const auto& n : t["monitors"]) {
            MonitorRect m;
            m.id = yamlScalarOr<int>(n, "id", 0);
            if (n["origin"] && n["origin"].IsSequence() && n["origin"].size() == 2) {
                m.origin = QPoint(n["origin"][0].as<int>(), n["origin"][1].as<int>());
            }
            if (n["size"] && n["size"].IsSequence() && n["size"].size() == 2) {
                m.size = QSize(n["size"][0].as<int>(), n["size"][1].as<int>());
            }
            m.pikvm = yamlString(n, "pikvm");
            cfg.monitors.append(m);
        }
    } else {
        result.errors.append(QStringLiteral("target.monitors is required"));
    }

    // hid_master
    cfg.hid_master = yamlString(root, "hid_master");
    if (cfg.hid_master.isEmpty()) {
        result.errors.append(QStringLiteral("hid_master is required"));
    }

    // auth (map of pikvm -> AuthSpec)
    if (const auto a = root["auth"]; a) {
        for (auto it = a.begin(); it != a.end(); ++it) {
            const QString host = QString::fromStdString(it->first.as<std::string>());
            const YAML::Node spec = it->second;
            AuthSpec as;
            as.user            = yamlString(spec, "user");
            as.password_ref    = yamlString(spec, "password_ref");
            as.totp_secret_ref = yamlString(spec, "totp_secret_ref");
            as.insecure_tls    = yamlScalarOr<bool>(spec, "insecure_tls", false);
            const QString tx   = yamlString(spec, "transport");
            if (tx.isEmpty() || tx == QLatin1String("janus")) {
                as.transport = VideoTransport::Janus;
            } else if (tx == QLatin1String("mjpeg")) {
                as.transport = VideoTransport::Mjpeg;
            } else {
                result.errors.append(QStringLiteral(
                    "auth['%1'].transport='%2' is not 'janus' or 'mjpeg'")
                    .arg(host, tx));
            }
            cfg.auth.insert(host, as);
        }
    }

    // release_hotkey
    cfg.release_hotkey    = yamlString(root, "release_hotkey");
    cfg.fullscreen_hotkey = yamlString(root, "fullscreen_hotkey");

    // video
    if (const auto v = root["video"]; v) {
        cfg.video.prefer_hw_decode = yamlScalarOr<bool>(v, "prefer_hw_decode", true);
        cfg.video.target_fps       = yamlScalarOr<int> (v, "target_fps",       60);
    }

    // windows
    if (const auto ws = root["windows"]; ws) {
        for (const auto& n : ws) {
            WindowSpec w;
            w.target_monitor = yamlScalarOr<int>(n, "target_monitor", 0);
            if (const auto g = n["geometry"]; g) {
                w.geometry = QRect(
                    yamlScalarOr<int>(g, "x", 0),
                    yamlScalarOr<int>(g, "y", 0),
                    yamlScalarOr<int>(g, "w", 0),
                    yamlScalarOr<int>(g, "h", 0));
            }
            cfg.windows.append(w);
        }
    }

    // If we had parse-time errors, don't bother with structural validation.
    if (!result.errors.isEmpty()) return result;

    const QStringList structural = validateConfig(cfg);
    if (!structural.isEmpty()) {
        result.errors = structural;
        return result;
    }

    result.config = cfg;
    return result;
}

}  // namespace glasshouse
