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

    void letterboxMatchedAspectFillsWidget();
    void letterboxTallerWidgetGetsHorizontalBars();
    void letterboxWiderWidgetGetsVerticalBars();
    void letterboxFallsBackToWidgetWhenVideoSizeUnknown();
    void letterboxIsCenteredInsideWidget();
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

// ----------------------------------------------------------------------
// computeLetterbox: Phase 5 — figure out the rendered video rect inside
// a Qt::KeepAspectRatio widget so we can use it (instead of the whole
// widget) as the CoordTransform letterbox.

void TestCoordTransform::letterboxMatchedAspectFillsWidget() {
    // 16:9 video into a 16:9 widget — no bars.
    const QRect r = computeLetterbox(QSize(1280, 720), QSize(1920, 1080));
    QCOMPARE(r, QRect(0, 0, 1280, 720));
}

void TestCoordTransform::letterboxTallerWidgetGetsHorizontalBars() {
    // 16:9 video into a 16:10 widget — bars at top and bottom.
    // 1280/1920 = 0.6667 ; 800/1080 = 0.7407
    // pick min → 0.6667 → rendered 1280x720, centered: y = (800-720)/2 = 40
    const QRect r = computeLetterbox(QSize(1280, 800), QSize(1920, 1080));
    QCOMPARE(r, QRect(0, 40, 1280, 720));
}

void TestCoordTransform::letterboxWiderWidgetGetsVerticalBars() {
    // 16:9 video into a wider-than-16:9 widget — pillarbox left/right.
    // 1600/1920 = 0.8333 ; 720/1080 = 0.6667
    // pick min → 0.6667 → rendered 1280x720, x = (1600-1280)/2 = 160
    const QRect r = computeLetterbox(QSize(1600, 720), QSize(1920, 1080));
    QCOMPARE(r, QRect(160, 0, 1280, 720));
}

void TestCoordTransform::letterboxFallsBackToWidgetWhenVideoSizeUnknown() {
    // Pre-first-frame: videoSize is invalid. We want the cursor math to
    // keep working — fall back to using the whole widget as the letterbox.
    QCOMPARE(computeLetterbox(QSize(1280, 720), QSize()),
             QRect(0, 0, 1280, 720));
    QCOMPARE(computeLetterbox(QSize(1280, 720), QSize(0, 1080)),
             QRect(0, 0, 1280, 720));
}

void TestCoordTransform::letterboxIsCenteredInsideWidget() {
    // Sanity check: letterbox rect is symmetric inside the widget.
    const QRect r = computeLetterbox(QSize(1000, 800), QSize(1920, 1080));
    const int leftPad  = r.x();
    const int rightPad = 1000 - r.x() - r.width();
    const int topPad   = r.y();
    const int botPad   = 800 - r.y() - r.height();
    // Off-by-one tolerance for integer rounding when the unused
    // dimension's pad is odd.
    QVERIFY(qAbs(leftPad - rightPad) <= 1);
    QVERIFY(qAbs(topPad  - botPad)   <= 1);
}

QTEST_APPLESS_MAIN(TestCoordTransform)
#include "test_coord_transform.moc"
