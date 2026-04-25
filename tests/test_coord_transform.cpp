// CoordTransform exercises DESIGN.md §5.2 math. The golden values are taken
// from the empirical verification table in §10.1 — a 3840×1080 target
// desktop composed of two 1920×1080 monitors, with the cursor driven into
// known positions and the wire values read back from `evtest`.
//
// The tests here drive the transform with a synthetic 1:1 local window
// whose geometry matches the target monitor exactly. That strips the
// window-mapping and letterbox steps so we exercise the API-coord math
// in isolation — the same math that has to line up with hardware.

#include "coord_transform.h"

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QTest>

using namespace glasshouse;

namespace {

// 3840×1080 combined desktop, two 1920×1080 monitors side by side (the
// exact layout from §10.1).
constexpr QSize kDesktop{3840, 1080};
const QRect    kMonitor1{ QPoint(0,    0), QSize(1920, 1080) };
const QRect    kMonitor2{ QPoint(1920, 0), QSize(1920, 1080) };

// Build an input where the window rect matches the monitor rect 1:1, so
// local (x,y) inside the window maps straight to target-pixel (x+origin, y).
CoordTransformInput oneToOne(const QRect& monitor, int localX, int localY) {
    return CoordTransformInput{
        /*localCursor=*/   QPoint(localX, localY),
        /*windowRect=*/    QRect(QPoint(0, 0), monitor.size()),
        /*letterboxRect=*/ QRect(QPoint(0, 0), monitor.size()),
        /*monitorRect=*/   monitor,
        /*logicalDesktop=*/kDesktop,
    };
}

}  // namespace

class TestCoordTransform : public QObject {
    Q_OBJECT
private slots:
    void centerOfCombinedDesktop();
    void topLeftOfMonitorOne();
    void bottomRightOfCombinedDesktop();
    void centerOfMonitorTwo();
    void clampsOutsideLetterboxToEdge();
    void letterboxOutputsEdgeWhenCursorLeftOfVideo();
    void windowOffsetDoesNotAffectResult();
    void rightEdgeVerticalCenter();
    void leftEdgeVerticalCenter();
};

void TestCoordTransform::centerOfCombinedDesktop() {
    // Center of monitor 1's right edge = center of combined desktop.
    const auto out = transformToApi(oneToOne(kMonitor1, 1920, 540));
    QCOMPARE(out.x, 0);
    QCOMPARE(out.y, 0);
}

void TestCoordTransform::topLeftOfMonitorOne() {
    const auto out = transformToApi(oneToOne(kMonitor1, 0, 0));
    QCOMPARE(out.x, -32768);
    QCOMPARE(out.y, -32768);
}

void TestCoordTransform::bottomRightOfCombinedDesktop() {
    // Bottom-right corner of monitor 2 = bottom-right of combined desktop.
    const auto out = transformToApi(oneToOne(kMonitor2, 1920, 1080));
    QCOMPARE(out.x, 32767);
    QCOMPARE(out.y, 32767);
}

void TestCoordTransform::centerOfMonitorTwo() {
    // Middle of monitor 2, vertically middle. §10.1 says (16384, 16384)
    // API coords lands at target pixel (24575, 24575). By symmetry, the
    // inverse direction: target pixel (2880, 540) → API (16383, 0).
    const auto out = transformToApi(oneToOne(kMonitor2, 960, 540));
    QCOMPARE(out.x, 16383);
    QCOMPARE(out.y, 0);
}

void TestCoordTransform::rightEdgeVerticalCenter() {
    // §10.1: API (32767, 0) corresponds to the right edge, vertical center.
    const auto out = transformToApi(oneToOne(kMonitor2, 1920, 540));
    QCOMPARE(out.x, 32767);
    QCOMPARE(out.y, 0);
}

void TestCoordTransform::leftEdgeVerticalCenter() {
    // §10.1: API (-32768, 0) = left edge, vertical center.
    const auto out = transformToApi(oneToOne(kMonitor1, 0, 540));
    QCOMPARE(out.x, -32768);
    QCOMPARE(out.y, 0);
}

void TestCoordTransform::clampsOutsideLetterboxToEdge() {
    // Cursor 100 px left of the letterbox → treated as sitting on its
    // left edge; API coord is the monitor's left edge, not clamped to the
    // desktop edge.
    CoordTransformInput in = oneToOne(kMonitor2, -100, 540);
    const auto out = transformToApi(in);
    QCOMPARE(out.x, 0);    // left edge of monitor 2 = center of desktop
    QCOMPARE(out.y, 0);
}

void TestCoordTransform::letterboxOutputsEdgeWhenCursorLeftOfVideo() {
    // Asymmetric letterbox: video occupies only the right 1000 px of a
    // 1920 px wide window. Cursor at window-local x=100 is 100 px into
    // the *window* but 820 px *left* of the letterbox, so it's clamped
    // to the letterbox's left edge — which maps to the monitor's left
    // edge in target space.
    CoordTransformInput in{
        /*localCursor=*/   QPoint(100, 540),
        /*windowRect=*/    QRect(0, 0, 1920, 1080),
        /*letterboxRect=*/ QRect(920, 0, 1000, 1080),
        /*monitorRect=*/   kMonitor2,
        /*logicalDesktop=*/kDesktop,
    };
    const auto out = transformToApi(in);
    QCOMPARE(out.x, 0);          // left edge of monitor 2
    QCOMPARE(out.y, 0);
}

void TestCoordTransform::windowOffsetDoesNotAffectResult() {
    // Moving the window around the local desktop must not change the
    // target coord for a given *window-relative* cursor position. Drive
    // the same window-relative (960, 540) via two different window
    // origins; the result must be identical.
    const auto a = transformToApi({
        QPoint(960,        540),
        QRect(0,   0, 1920, 1080),
        QRect(0,   0, 1920, 1080),
        kMonitor1,
        kDesktop,
    });
    const auto b = transformToApi({
        QPoint(500 + 960, 300 + 540),
        QRect(500, 300, 1920, 1080),
        QRect(0,   0,   1920, 1080),
        kMonitor1,
        kDesktop,
    });
    QCOMPARE(a.x, b.x);
    QCOMPARE(a.y, b.y);
}

QTEST_APPLESS_MAIN(TestCoordTransform)
#include "test_coord_transform.moc"
