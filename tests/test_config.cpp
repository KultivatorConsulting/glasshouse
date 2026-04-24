// DESIGN.md §8.1 requires the loader to reject five specific failure modes.
// Each test here exercises one of them, plus a happy-path sanity check.

#include "config.h"

#include <QTest>

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

QTEST_APPLESS_MAIN(TestConfig)
#include "test_config.moc"
