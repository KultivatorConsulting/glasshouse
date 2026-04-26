#pragma once

#include <QString>
#include <optional>

namespace glasshouse {

// Resolves `secret://<name>` references to plaintext secrets.
//
// Lookup order on resolve():
//   1. Environment variable `GLASSHOUSE_SECRET_<NAME>` — uppercased,
//      non-alphanumerics replaced with `_`.
//        secret://pikvm-1-passwd  →  GLASSHOUSE_SECRET_PIKVM_1_PASSWD
//      Environment wins because shell-driven workflows (CI, ad-hoc
//      ssh sessions) routinely override config files.
//   2. `secrets.yaml` file under $XDG_CONFIG_HOME/glasshouse/ (default
//      ~/.config/glasshouse/secrets.yaml). YAML map of name → plaintext,
//      where the name matches the part after `secret://`. The file is
//      parsed once per process and cached.
//
// A `ref` that does not start with `secret://` is returned verbatim
// with a warning (handy for local debugging).
class SecretResolver {
public:
    static std::optional<QString> resolve(const QString& ref);

    // Test/diagnostic hook: compute the env-var name for a ref without
    // reading it. Empty if `ref` isn't a well-formed `secret://<name>`.
    static QString envKeyForRef(const QString& ref);

    // Test hook: drop the cached secrets-file map so the next resolve()
    // re-reads from disk. Production callers don't need this.
    static void resetCacheForTesting();

    // Test hook: override the secrets.yaml path. Empty resets to the
    // default `$XDG_CONFIG_HOME/glasshouse/secrets.yaml`.
    static void setSecretsFilePathForTesting(const QString& path);
};

}  // namespace glasshouse
