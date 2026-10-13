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

QString ocrProviderName(OcrProvider p) {
    switch (p) {
        case OcrProvider::None:    return QStringLiteral("none");
        case OcrProvider::Pikvm:   return QStringLiteral("pikvm");
        case OcrProvider::Command: return QStringLiteral("command");
        case OcrProvider::OpenAi:  return QStringLiteral("openai");
        case OcrProvider::Tui:     return QStringLiteral("tui");
    }
    return QStringLiteral("none");
}

bool parseOcrProvider(const QString& name, OcrProvider* out, QString* error) {
    const QString n = name.trimmed().toLower();
    if      (n == QLatin1String("none"))    *out = OcrProvider::None;
    else if (n == QLatin1String("pikvm"))   *out = OcrProvider::Pikvm;
    else if (n == QLatin1String("command")) *out = OcrProvider::Command;
    else if (n == QLatin1String("openai"))  *out = OcrProvider::OpenAi;
    else if (n == QLatin1String("tui"))     *out = OcrProvider::Tui;
    else {
        if (error) {
            *error = QStringLiteral(
                "ocr.provider '%1' is not one of: none, pikvm, command, "
                "openai, tui").arg(name);
        }
        return false;
    }
    return true;
}

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

    // 6. the chosen OCR provider has what it needs. Checked here rather
    // than at use so a typo in `command` is a startup error, not a
    // surprise the first time someone drags a rectangle.
    switch (cfg.ocr.provider) {
        case OcrProvider::Command:
            if (cfg.ocr.command.isEmpty()) {
                errors.append(QStringLiteral(
                    "ocr.provider is 'command' but ocr.command is empty — "
                    "give the argv to run, e.g. [tesseract, '-', '-']"));
            }
            break;
        case OcrProvider::OpenAi:
        case OcrProvider::Tui:
            if (cfg.ocr.api_key.isEmpty()) {
                errors.append(QStringLiteral(
                    "ocr.provider is '%1' but ocr.api_key is unset")
                    .arg(ocrProviderName(cfg.ocr.provider)));
            }
            break;
        case OcrProvider::Pikvm:
            if (cfg.ocr.langs.isEmpty()) {
                errors.append(QStringLiteral(
                    "ocr.langs is empty; the PiKVM needs at least one "
                    "language (stock firmware has 'eng')"));
            }
            break;
        case OcrProvider::None:
            break;
    }

    // 4b. watcher budgets. min_history_frames divides the per-watcher byte
    // budget to derive the area ceiling, so a zero here is a crash rather
    // than a bad setting.
    if (cfg.watchers.enabled) {
        if (cfg.watchers.max < 1) {
            errors.append(QStringLiteral(
                "watchers.max is %1; it must be at least 1 (or set "
                "watchers.enabled: false)").arg(cfg.watchers.max));
        }
        if (cfg.watchers.threshold < 1 || cfg.watchers.threshold > 255) {
            errors.append(QStringLiteral(
                "watchers.threshold is %1; it is a mean absolute luma "
                "difference and must be 1-255").arg(cfg.watchers.threshold));
        }
        if (cfg.watchers.revert_window_ms < 0) {
            errors << QStringLiteral(
                "watchers.revert_window_ms is %1; it cannot be negative. 0 "
                "resolves every event as persisted on the next frame, which "
                "turns the signal off without turning the watcher off.")
                .arg(cfg.watchers.revert_window_ms);
        }
        if (cfg.watchers.min_history_frames < 1) {
            errors.append(QStringLiteral(
                "watchers.min_history_frames is %1; it must be at least 1 "
                "— it divides history_bytes_per_watcher to decide how large "
                "a region may keep history")
                .arg(cfg.watchers.min_history_frames));
        }
        if (cfg.watchers.history_bytes_per_watcher < 1) {
            errors.append(QStringLiteral(
                "watchers.history_bytes_per_watcher is %1; it must be "
                "positive").arg(cfg.watchers.history_bytes_per_watcher));
        }
        if (cfg.watchers.history_pool_bytes <
            cfg.watchers.history_bytes_per_watcher) {
            errors.append(QStringLiteral(
                "watchers.history_pool_bytes (%1) is below "
                "history_bytes_per_watcher (%2), so no watcher could ever "
                "fill its own budget")
                .arg(cfg.watchers.history_pool_bytes)
                .arg(cfg.watchers.history_bytes_per_watcher));
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

    // hotkeys
    cfg.release_hotkey      = yamlString(root, "release_hotkey");
    cfg.fullscreen_hotkey   = yamlString(root, "fullscreen_hotkey");
    cfg.special_keys_hotkey = yamlString(root, "special_keys_hotkey");
    cfg.paste_hotkey        = yamlString(root, "paste_hotkey");
    cfg.region_hotkey       = yamlString(root, "region_hotkey");
    cfg.watch_hotkey        = yamlString(root, "watch_hotkey");

    // shortcuts list — optional. Each entry: {label: "...", keys: [...]}.
    if (const auto sc = root["shortcuts"]; sc && sc.IsSequence()) {
        for (const auto& n : sc) {
            ShortcutSpec s;
            s.label = QString::fromStdString(yamlScalarOr<std::string>(n, "label", ""));
            if (const auto k = n["keys"]; k && k.IsSequence()) {
                for (const auto& kn : k) {
                    s.keys.append(QString::fromStdString(kn.as<std::string>()));
                }
            }
            if (s.label.isEmpty() || s.keys.isEmpty()) {
                result.errors.append(QStringLiteral(
                    "shortcuts[] entry must have non-empty label and non-empty keys"));
                continue;
            }
            cfg.shortcuts.append(s);
        }
    }

    // video
    if (const auto v = root["video"]; v) {
        cfg.video.prefer_hw_decode  = yamlScalarOr<bool>(v, "prefer_hw_decode",  true);
        cfg.video.target_fps        = yamlScalarOr<int> (v, "target_fps",        60);
        cfg.video.webrtc_latency_ms = yamlScalarOr<int> (v, "webrtc_latency_ms", 100);
        cfg.video.cursor_marker       = QString::fromStdString(
            yamlScalarOr<std::string>(v, "cursor_marker",       "ring"));
        cfg.video.cursor_marker_color = QString::fromStdString(
            yamlScalarOr<std::string>(v, "cursor_marker_color", "#FFA000"));
        cfg.video.cursor_marker_size  = yamlScalarOr<int>(v, "cursor_marker_size", 24);
    }

    // ocr — all optional; defaults live in the struct so an absent
    // block still yields a working Tui configuration bar the API key.
    if (const auto o = root["ocr"]; o) {
        const OcrSpec d;
        if (const QString name = yamlString(o, "provider"); !name.isEmpty()) {
            QString why;
            if (parseOcrProvider(name, &cfg.ocr.provider, &why)) {
                cfg.ocr.provider_explicit = true;
            } else {
                result.errors.append(why);
            }
        }
        if (const auto l = o["langs"]; l && l.IsSequence()) {
            cfg.ocr.langs.clear();
            for (const auto& n : l) {
                cfg.ocr.langs.append(
                    QString::fromStdString(n.as<std::string>("")));
            }
        }
        if (const auto c = o["command"]; c && c.IsSequence()) {
            for (const auto& n : c) {
                cfg.ocr.command.append(
                    QString::fromStdString(n.as<std::string>("")));
            }
        }
        cfg.ocr.endpoint = yamlString(o, "endpoint");
        cfg.ocr.api_key  = yamlString(o, "api_key");
        cfg.ocr.model    = QString::fromStdString(yamlScalarOr<std::string>(
            o, "model", d.model.toStdString()));
        cfg.ocr.reasoning_effort = QString::fromStdString(
            yamlScalarOr<std::string>(o, "reasoning_effort",
                                      d.reasoning_effort.toStdString()));
        cfg.ocr.max_tokens = yamlScalarOr<int>(o, "max_tokens", d.max_tokens);
        cfg.ocr.timeout_ms = yamlScalarOr<int>(o, "timeout_ms", d.timeout_ms);
        cfg.ocr.prompt     = QString::fromStdString(yamlScalarOr<std::string>(
            o, "prompt", d.prompt.toStdString()));
    }

    // Provider defaulting, after parsing so an explicit `provider:` always
    // wins. A config written before providers existed set `api_key` and
    // meant Tui, because Tui's endpoint was the only default there was;
    // anything else means the PiKVM's own OCR, which costs nothing to try.
    if (!cfg.ocr.provider_explicit) {
        cfg.ocr.provider = cfg.ocr.api_key.isEmpty() ? OcrProvider::Pikvm
                                                     : OcrProvider::Tui;
    }
    if (cfg.ocr.endpoint.isEmpty()) {
        if (cfg.ocr.provider == OcrProvider::Tui) {
            cfg.ocr.endpoint = QString::fromUtf8(OcrSpec::kTuiEndpoint);
        } else if (cfg.ocr.provider == OcrProvider::OpenAi) {
            cfg.ocr.endpoint = QString::fromUtf8(OcrSpec::kOpenAiEndpoint);
        }
    }

    // mcp — all optional; absent block leaves the server disabled.
    if (const auto m = root["mcp"]; m) {
        const McpSpec d;
        cfg.mcp.enabled = yamlScalarOr<bool>(m, "enabled", d.enabled);
        cfg.mcp.bind    = QString::fromStdString(yamlScalarOr<std::string>(
            m, "bind", d.bind.toStdString()));
        cfg.mcp.port    = yamlScalarOr<int>(m, "port", d.port);
        cfg.mcp.token   = yamlString(m, "token");
    }

    // watchers — all optional; the defaults are HARNESS.md §5.4 and §10.
    if (const auto w = root["watchers"]; w) {
        const WatchSpec d;
        cfg.watchers.enabled     = yamlScalarOr<bool>(w, "enabled", d.enabled);
        cfg.watchers.max         = yamlScalarOr<int>(w, "max", d.max);
        cfg.watchers.threshold   = yamlScalarOr<int>(w, "threshold",
                                                     d.threshold);
        cfg.watchers.cooldown_ms = yamlScalarOr<int>(w, "cooldown_ms",
                                                     d.cooldown_ms);
        cfg.watchers.settle_ms   = yamlScalarOr<int>(w, "settle_ms",
                                                     d.settle_ms);
        cfg.watchers.history_bytes_per_watcher = yamlScalarOr<int>(
            w, "history_bytes_per_watcher", d.history_bytes_per_watcher);
        cfg.watchers.history_pool_bytes = yamlScalarOr<int>(
            w, "history_pool_bytes", d.history_pool_bytes);
        cfg.watchers.min_history_frames = yamlScalarOr<int>(
            w, "min_history_frames", d.min_history_frames);
        cfg.watchers.revert_window_ms = yamlScalarOr<int>(
            w, "revert_window_ms", d.revert_window_ms);
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
