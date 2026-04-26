#include "secrets.h"
#include "logging.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStandardPaths>
#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <mutex>

namespace glasshouse {

namespace {

constexpr auto kScheme = "secret://";

QString defaultSecretsPath() {
    // Honours XDG_CONFIG_HOME via QStandardPaths, falls through to
    // ~/.config/glasshouse on a normal Linux setup.
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
         + QStringLiteral("/glasshouse/secrets.yaml");
}

// Process-wide cache for the parsed secrets file. Loaded lazily on the
// first lookup that needs it; reset by tests via resetCacheForTesting().
struct SecretsCache {
    std::once_flag      once;
    QHash<QString, QString> map;   // name → plaintext
    bool                attempted = false;
    QString             pathOverride;  // empty = use defaultSecretsPath()
};

SecretsCache& cache() {
    static SecretsCache c;
    return c;
}

void loadSecretsFileLocked(SecretsCache& c) {
    c.attempted = true;
    c.map.clear();

    const QString path = c.pathOverride.isEmpty()
                       ? defaultSecretsPath()
                       : c.pathOverride;
    QFileInfo info(path);
    if (!info.exists()) {
        qCDebug(lcConfig) << "no secrets.yaml at" << path;
        return;
    }

    // Best-effort permissions check. World/group readable on a file
    // containing PiKVM passwords is a foot-gun; warn loudly but don't
    // refuse — owner/group conventions vary on shared workstations.
    const auto perms = info.permissions();
    if (perms & (QFile::ReadGroup | QFile::ReadOther
              |  QFile::WriteGroup | QFile::WriteOther
              |  QFile::ExeGroup   | QFile::ExeOther)) {
        qCWarning(lcConfig) << "secrets file" << path
                            << "is readable beyond owner — chmod 0600 recommended";
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCWarning(lcConfig) << "cannot open secrets file" << path << ":"
                            << f.errorString();
        return;
    }

    YAML::Node root;
    try {
        root = YAML::Load(f.readAll().toStdString());
    } catch (const YAML::Exception& e) {
        qCWarning(lcConfig) << "secrets.yaml parse error:"
                            << QString::fromStdString(e.what());
        return;
    }
    if (!root || !root.IsMap()) {
        qCWarning(lcConfig) << "secrets.yaml must be a top-level map of "
                               "name: plaintext";
        return;
    }

    int n = 0;
    for (const auto& kv : root) {
        try {
            const QString k = QString::fromStdString(kv.first.as<std::string>());
            const QString v = QString::fromStdString(kv.second.as<std::string>());
            if (!k.isEmpty()) { c.map.insert(k, v); ++n; }
        } catch (const YAML::Exception&) {
            // Skip non-string entries silently; YAML allows nested maps
            // and we don't want a noisy log line per oddball entry.
        }
    }
    qCInfo(lcConfig) << "loaded" << n << "secrets from" << path;
}

}  // namespace

QString SecretResolver::envKeyForRef(const QString& ref) {
    if (!ref.startsWith(QLatin1String(kScheme))) return {};
    const QString name = ref.mid(static_cast<int>(qstrlen(kScheme)));
    if (name.isEmpty()) return {};

    QString envKey = QStringLiteral("GLASSHOUSE_SECRET_");
    envKey.reserve(envKey.size() + name.size());
    for (QChar c : name) {
        envKey.append(c.isLetterOrNumber() ? c.toUpper() : QChar('_'));
    }
    return envKey;
}

std::optional<QString> SecretResolver::resolve(const QString& ref) {
    if (!ref.startsWith(QLatin1String(kScheme))) {
        qCWarning(lcConfig) << "secret ref is not a secret:// URI; "
                               "using verbatim:" << ref;
        return ref;
    }

    // Layer 1: environment variable.
    const QString envKey = envKeyForRef(ref);
    if (envKey.isEmpty()) {
        qCWarning(lcConfig) << "malformed secret:// ref:" << ref;
        return std::nullopt;
    }
    if (const QByteArray raw = qgetenv(envKey.toLocal8Bit().constData());
            !raw.isEmpty()) {
        return QString::fromUtf8(raw);
    }

    // Layer 2: secrets.yaml. Loaded once per process.
    auto& c = cache();
    std::call_once(c.once, [&c]() { loadSecretsFileLocked(c); });
    const QString name = ref.mid(static_cast<int>(qstrlen(kScheme)));
    if (auto it = c.map.constFind(name); it != c.map.constEnd()) {
        return it.value();
    }

    qCWarning(lcConfig) << "secret ref" << ref
                        << "not found in env (" << envKey
                        << ") or secrets.yaml";
    return std::nullopt;
}

void SecretResolver::resetCacheForTesting() {
    auto& c = cache();
    // std::once_flag isn't resettable, so we work around by replacing
    // the cache wholesale on the next lookup. Easiest portable trick:
    // clear the map and the pathOverride, set attempted=false, and live
    // with the once_flag staying tripped. Tests that need a re-read can
    // call setSecretsFilePathForTesting() which forces an explicit
    // re-load on the next resolve().
    c.map.clear();
    c.attempted = false;
    c.pathOverride.clear();
}

void SecretResolver::setSecretsFilePathForTesting(const QString& path) {
    auto& c = cache();
    c.pathOverride = path;
    c.map.clear();
    c.attempted = false;
    // Force a re-read on the next resolve() by bypassing once_flag —
    // load synchronously here.
    loadSecretsFileLocked(c);
}

}  // namespace glasshouse
