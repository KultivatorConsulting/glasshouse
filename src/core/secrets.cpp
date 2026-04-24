#include "secrets.h"
#include "logging.h"

#include <QByteArray>
#include <cstdlib>

namespace glasshouse {

namespace {
constexpr auto kScheme = "secret://";
}

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
        qCWarning(lcConfig) << "secret ref is not a secret:// URI; using verbatim:" << ref;
        return ref;
    }
    const QString envKey = envKeyForRef(ref);
    if (envKey.isEmpty()) {
        qCWarning(lcConfig) << "malformed secret:// ref:" << ref;
        return std::nullopt;
    }
    const QByteArray raw = qgetenv(envKey.toLocal8Bit().constData());
    if (raw.isEmpty()) {
        qCWarning(lcConfig) << "env var not set for secret ref:" << ref
                            << "(expected" << envKey << ")";
        return std::nullopt;
    }
    return QString::fromUtf8(raw);
}

}  // namespace glasshouse
