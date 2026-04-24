#pragma once

#include <QString>
#include <optional>

namespace glasshouse {

// Resolves `secret://<name>` references to plaintext secrets. Phase 1
// implementation: env-var lookup. The name is upper-cased and every
// non-alphanumeric character is replaced with `_`, then prefixed with
// `GLASSHOUSE_SECRET_`.
//
//   secret://pikvm-1-passwd  ->  GLASSHOUSE_SECRET_PIKVM_1_PASSWD
class SecretResolver {
public:
    // Returns the resolved plaintext secret, or std::nullopt if the ref is
    // malformed or the env variable is unset. A `ref` that does not start
    // with `secret://` is returned verbatim (handy for local debugging) with
    // a warning logged.
    static std::optional<QString> resolve(const QString& ref);

    // Exposed for tests: compute the env-var name for a ref without reading it.
    // Returns empty string if ref is not a well-formed `secret://<name>` URI.
    static QString envKeyForRef(const QString& ref);
};

}  // namespace glasshouse
