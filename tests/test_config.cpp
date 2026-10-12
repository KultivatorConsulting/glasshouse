// DESIGN.md §8.1 requires the loader to reject five specific failure modes.
// Each test here exercises one of them, plus a happy-path sanity check.

#include "config.h"

#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>

using namespace glasshouse;

namespace {

// Baseline 2-monitor config that validates cleanly. Tests mutate one field at
// a time to provoke each §8.1 failure.
Config makeValid() {
    Config c;
    c.logical_desktop = QSize(3840, 1080);
    c.monitors = {
        {1, QPoint(0,    0), QSize(1920, 1080), QStringLiteral("192.168.1.71")},
        {2, QPoint(1920, 0), QSize(1920, 1080), QStringLiteral("192.168.1.72")},
    };
    c.hid_master = QStringLiteral("192.168.1.71");
    c.windows = {
        {1, QRect(0,   0, 960, 540)},
        {2, QRect(960, 0, 960, 540)},
    };
    return c;
}

bool anyErrorContains(const QStringList& errs, const QString& needle) {
    for (const auto& e : errs) if (e.contains(needle)) return true;
    return false;
}

// Minimum that validates, so a test can bolt one `ocr:` block on and get
// a parsed Config back. Written as a concatenated literal rather than a
// raw string: moc emits an EMPTY .moc for this file when it meets an
// R"(...)" literal, and the only symptom is 'undefined reference to
// vtable for TestConfig' at link time.
// Provider defaulting happens in loadConfig, not validateConfig, so it
// can only be exercised through a real file.
constexpr auto kMinimalYaml =
    "target:\n"
    "  logical_desktop: { width: 1920, height: 1080 }\n"
    "  monitors:\n"
    "    - { id: 1, origin: [0, 0], size: [1920, 1080], pikvm: 10.0.0.1 }\n"
    "hid_master: 10.0.0.1\n"
    "auth:\n"
    "  10.0.0.1: { user: admin, password_ref: \"secret://pw\" }\n"
    "windows:\n"
    "  - { target_monitor: 1 }\n";

// Writes kMinimalYaml plus `extra`, loads it, and hands back the result.
// `dir` must outlive the call.
ConfigResult loadWith(QTemporaryDir& dir, const QString& extra) {
    const QString path = dir.filePath(QStringLiteral("c.yaml"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    QTextStream(&f) << QString::fromUtf8(kMinimalYaml) << "\n" << extra << "\n";
    f.close();
    return loadConfig(path);
}

}  // namespace

class TestConfig : public QObject {
    Q_OBJECT
private slots:
    void validConfigAccepts();
    void monitorWindowCountMismatchRejects();
    void unknownWindowTargetMonitorRejects();
    void hidMasterNotInMonitorsRejects();
    void monitorOutsideDesktopRejects();
    void overlappingMonitorsRejects();

    void ocrProviderNamesParse();
    void ocrCommandProviderNeedsCommand();
    void ocrModelProvidersNeedApiKey();
    void ocrPikvmProviderNeedsALanguage();
    void ocrDefaultsToPikvmWhenNothingIsSet();
    void ocrDefaultsToTuiWhenOnlyAnApiKeyIsSet();
    void ocrExplicitProviderBeatsTheApiKeyHeuristic();
    void ocrEndpointDefaultsPerProvider();
    void ocrRejectsAnUnknownProvider();
};

void TestConfig::validConfigAccepts() {
    QCOMPARE(validateConfig(makeValid()), QStringList{});
}

void TestConfig::monitorWindowCountMismatchRejects() {
    Config c = makeValid();
    c.windows.removeLast();
    const auto errs = validateConfig(c);
    QVERIFY(!errs.isEmpty());
    QVERIFY(anyErrorContains(errs, QStringLiteral("target.monitors has")));
}

void TestConfig::unknownWindowTargetMonitorRejects() {
    Config c = makeValid();
    c.windows[0].target_monitor = 99;
    const auto errs = validateConfig(c);
    QVERIFY(anyErrorContains(errs, QStringLiteral("target_monitor=99")));
}

void TestConfig::hidMasterNotInMonitorsRejects() {
    Config c = makeValid();
    c.hid_master = QStringLiteral("10.0.0.1");
    const auto errs = validateConfig(c);
    QVERIFY(anyErrorContains(errs, QStringLiteral("hid_master")));
}

void TestConfig::monitorOutsideDesktopRejects() {
    Config c = makeValid();
    // Shift monitor 2 so it extends past the right edge of logical_desktop.
    c.monitors[1].origin = QPoint(2000, 0);
    const auto errs = validateConfig(c);
    QVERIFY(anyErrorContains(errs, QStringLiteral("extends outside logical_desktop")));
}

void TestConfig::overlappingMonitorsRejects() {
    Config c = makeValid();
    c.monitors[1].origin = QPoint(500, 0);
    const auto errs = validateConfig(c);
    QVERIFY(anyErrorContains(errs, QStringLiteral("overlap")));
}

// ---------------------------------------------------------------------------
// OCR provider selection. The defaulting rules carry a compatibility
// promise — a config written before providers existed must keep working
// — so they are pinned here rather than left to inspection.

void TestConfig::ocrProviderNamesParse() {
    const QList<QPair<QString, OcrProvider>> cases{
        {QStringLiteral("none"),    OcrProvider::None},
        {QStringLiteral("pikvm"),   OcrProvider::Pikvm},
        {QStringLiteral("command"), OcrProvider::Command},
        {QStringLiteral("openai"),  OcrProvider::OpenAi},
        {QStringLiteral("tui"),     OcrProvider::Tui},
        // Case and padding are a user's business, not a parse failure.
        {QStringLiteral("  PiKVM "), OcrProvider::Pikvm},
    };
    for (const auto& [name, want] : cases) {
        OcrProvider got{};
        QString err;
        QVERIFY2(parseOcrProvider(name, &got, &err), qPrintable(err));
        QCOMPARE(static_cast<int>(got), static_cast<int>(want));
        QCOMPARE(ocrProviderName(want), name.trimmed().toLower());
    }

    OcrProvider got{};
    QString err;
    QVERIFY(!parseOcrProvider(QStringLiteral("tesseract"), &got, &err));
    QVERIFY(err.contains(QStringLiteral("not one of")));
}

void TestConfig::ocrCommandProviderNeedsCommand() {
    Config c = makeValid();
    c.ocr.provider = OcrProvider::Command;
    QVERIFY(anyErrorContains(validateConfig(c),
                             QStringLiteral("ocr.command is empty")));

    c.ocr.command = {QStringLiteral("tesseract"), QStringLiteral("-"),
                     QStringLiteral("-")};
    QCOMPARE(validateConfig(c), QStringList{});
}

void TestConfig::ocrModelProvidersNeedApiKey() {
    for (auto p : {OcrProvider::OpenAi, OcrProvider::Tui}) {
        Config c = makeValid();
        c.ocr.provider = p;
        QVERIFY(anyErrorContains(validateConfig(c),
                                 QStringLiteral("ocr.api_key is unset")));
        c.ocr.api_key = QStringLiteral("secret://k");
        QCOMPARE(validateConfig(c), QStringList{});
    }
}

void TestConfig::ocrPikvmProviderNeedsALanguage() {
    Config c = makeValid();
    c.ocr.provider = OcrProvider::Pikvm;
    QCOMPARE(validateConfig(c), QStringList{});   // defaults to eng
    c.ocr.langs.clear();
    QVERIFY(anyErrorContains(validateConfig(c),
                             QStringLiteral("ocr.langs is empty")));
}

void TestConfig::ocrDefaultsToPikvmWhenNothingIsSet() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto r = loadWith(dir, QString());
    QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(QStringLiteral("; "))));
    QCOMPARE(static_cast<int>(r.config->ocr.provider),
             static_cast<int>(OcrProvider::Pikvm));
}

void TestConfig::ocrDefaultsToTuiWhenOnlyAnApiKeyIsSet() {
    // The compatibility case: every config written before providers
    // existed looks exactly like this, and meant Tui.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto r = loadWith(dir,
        QStringLiteral("ocr:\n  api_key: \"secret://tui\"\n"));
    QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(QStringLiteral("; "))));
    QCOMPARE(static_cast<int>(r.config->ocr.provider),
             static_cast<int>(OcrProvider::Tui));
    QVERIFY(r.config->ocr.endpoint.contains(QStringLiteral("kultivator")));
}

void TestConfig::ocrExplicitProviderBeatsTheApiKeyHeuristic() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto r = loadWith(dir, QStringLiteral(
        "ocr:\n  provider: pikvm\n  api_key: \"secret://tui\"\n"));
    QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(QStringLiteral("; "))));
    QCOMPARE(static_cast<int>(r.config->ocr.provider),
             static_cast<int>(OcrProvider::Pikvm));
}

void TestConfig::ocrEndpointDefaultsPerProvider() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const auto openai = loadWith(dir, QStringLiteral(
        "ocr:\n  provider: openai\n  api_key: \"secret://k\"\n"));
    QVERIFY(openai.errors.isEmpty());
    QCOMPARE(openai.config->ocr.endpoint,
             QString::fromUtf8(OcrSpec::kOpenAiEndpoint));

    // An explicit endpoint is left alone — that is how a local Ollama or
    // vLLM gets used through the openai provider.
    const auto local = loadWith(dir, QStringLiteral(
        "ocr:\n  provider: openai\n  api_key: \"secret://k\"\n"
        "  endpoint: \"http://localhost:11434/v1/chat/completions\"\n"));
    QVERIFY(local.errors.isEmpty());
    QCOMPARE(local.config->ocr.endpoint,
             QStringLiteral("http://localhost:11434/v1/chat/completions"));
}

void TestConfig::ocrRejectsAnUnknownProvider() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto r = loadWith(dir, QStringLiteral("ocr:\n  provider: magic\n"));
    QVERIFY(anyErrorContains(r.errors, QStringLiteral("not one of")));
}

QTEST_APPLESS_MAIN(TestConfig)
#include "test_config.moc"
