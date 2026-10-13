// HARNESS.md §5 describes four stages; this covers the one that runs on
// every decoded frame. Unlike the MCP input tools — which hidReady() gates
// behind a connected PiKVM, so no test can reach them — the detector takes
// images in and produces events out, which makes all of its behaviour
// (edge triggering, cooldown, pointer suppression, the volatile budget)
// reachable from a unit test with synthetic frames.
//
// Note: no R"(...)" raw string literals in this file. moc emits an EMPTY
// .moc when it meets one in a file with a Q_OBJECT class, and the only
// symptom is 'undefined reference to vtable' at link time.

#include "watcher.h"

#include <QImage>
#include <QElapsedTimer>
#include <QPainter>
#include <QTest>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <cstring>

using namespace glasshouse;

namespace {

constexpr int kW = 640;
constexpr int kH = 480;

// A frame of flat grey, so any region of it reads as quiet.
QImage flatFrame(int grey = 40) {
    QImage img(kW, kH, QImage::Format_RGB888);
    img.fill(QColor(grey, grey, grey));
    return img;
}

// The same frame with `rect` painted a different shade — a change confined
// to one region, which is what every watcher test needs.
QImage framePatched(const QRect& rect, int grey, int base = 40) {
    QImage img = flatFrame(base);
    QPainter p(&img);
    p.fillRect(rect, QColor(grey, grey, grey));
    p.end();
    return img;
}

// A QImage wrapped as the packed-BGRA QVideoFrame that Glasshouse's own
// pipelines actually produce — both of them end in
// `videoconvert ! video/x-raw,format=BGRA`. QImage::Format_ARGB32 is a
// word value, so on little-endian its bytes are already B,G,R,A.
QVideoFrame bgraFrame(const QImage& src) {
    const QImage img = src.convertToFormat(QImage::Format_ARGB32);
    QVideoFrame f(QVideoFrameFormat(img.size(),
                                    QVideoFrameFormat::Format_BGRA8888));
    if (!f.map(QVideoFrame::WriteOnly)) return QVideoFrame();
    for (int y = 0; y < img.height(); ++y) {
        std::memcpy(f.bits(0) + qsizetype(y) * f.bytesPerLine(0),
                    img.constScanLine(y), size_t(img.width()) * 4);
    }
    f.unmap();
    return f;
}

// Tight budgets so eviction is reachable without allocating 96 MB.
WatcherLimits smallLimits() {
    WatcherLimits l;
    l.defaultThreshold  = 8;
    l.defaultCooldownMs = 0;      // create() replaces 0 with this, so set it
    l.defaultSettleMs   = 50;
    l.volatileBytesPerWatcher = 256 * 1024;
    l.volatilePoolBytes       = 512 * 1024;
    l.minVolatileDepth        = 8;
    return l;
}

// Registers a watcher on a registry that has already seen a frame, so the
// rect can be validated. Returns the id; 0 means create() refused.
int add(WatcherRegistry* reg, const QRect& rect, QString* error = nullptr,
        WatcherTrigger trigger = WatcherTrigger::Change,
        bool wantVolatile = false, int cooldownMs = 400) {
    WatcherDef d;
    d.screen         = 0;
    d.rect           = rect;
    d.intent         = QStringLiteral("a test watcher");
    d.trigger        = trigger;
    d.threshold      = 8;
    d.cooldownMs     = cooldownMs;
    d.settleMs       = 50;
    d.volatileWanted = wantVolatile;
    QString err;
    const int id = reg->create(d, &err);
    if (error) *error = err;
    return id;
}

}  // namespace

class TestWatcher : public QObject {
    Q_OBJECT
private slots:
    void aBgraVideoFrameIsReadInPlace();
    void aVideoFrameAndAnImageAgreeOnTheDelta();
    void createRejectsAnEmptyIntent();
    void createRejectsARectOutsideTheFrame();
    void createRejectsPastTheWatcherLimit();

    void aQuietRegionNeverFires();
    void aChangeFiresOnceOnTheRisingEdge();
    void aChangeOutsideTheRectIsIgnored();
    void theCooldownSuppressesARapidSecondFire();
    void aNeverQuietRegionFiresAtTheCooldownRate();
    void thePointerInsideTheRegionSuppresses();
    void settleFiresOnlyOnceTheRegionGoesQuiet();

    void volatileIsRefusedAboveTheAreaCeiling();
    void theVolatileRingEvictsOldestOverTheCap();
    void aNonVolatileWatcherKeepsNoHistory();

    void thePollCursorReturnsOnlyNewEvents();
    void deletingAWatcherDropsItsEvents();

    void aLargeRectStillDetectsChange();
    void theFilmstripTilesEveryFrame();
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void TestWatcher::createRejectsAnEmptyIntent() {
    WatcherRegistry reg;
    reg.setFrameSize(0, QSize(kW, kH));

    WatcherDef d;
    d.screen = 0;
    d.rect   = QRect(10, 10, 40, 40);
    QString err;
    QCOMPARE(reg.create(d, &err), 0);
    QVERIFY2(err.contains(QStringLiteral("intent")), qPrintable(err));
}

void TestWatcher::createRejectsARectOutsideTheFrame() {
    WatcherRegistry reg;
    reg.setFrameSize(0, QSize(kW, kH));
    QString err;
    QCOMPARE(add(&reg, QRect(kW - 10, 10, 100, 40), &err), 0);
    QVERIFY2(err.contains(QStringLiteral("not inside")), qPrintable(err));

    // And a rect on a screen nothing has streamed yet, which is the common
    // real case: a watcher created before the video is up.
    WatcherRegistry fresh;
    QCOMPARE(add(&fresh, QRect(10, 10, 40, 40), &err), 0);
    QVERIFY2(err.contains(QStringLiteral("no frames yet")), qPrintable(err));
}

void TestWatcher::createRejectsPastTheWatcherLimit() {
    WatcherRegistry reg;
    WatcherLimits l = smallLimits();
    l.maxWatchers = 2;
    reg.setLimits(l);
    reg.setFrameSize(0, QSize(kW, kH));

    QVERIFY(add(&reg, QRect(0, 0, 20, 20)) > 0);
    QVERIFY(add(&reg, QRect(30, 0, 20, 20)) > 0);
    QString err;
    QCOMPARE(add(&reg, QRect(60, 0, 20, 20), &err), 0);
    QVERIFY2(err.contains(QStringLiteral("limit")), qPrintable(err));
    QCOMPARE(reg.count(), 2);
}

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

void TestWatcher::aQuietRegionNeverFires() {
    WatcherRegistry reg;
    reg.setLimits(smallLimits());
    reg.ingestImage(0, flatFrame());
    const int id = add(&reg, QRect(100, 100, 80, 40));
    QVERIFY(id > 0);

    for (int i = 0; i < 20; ++i) reg.ingestImage(0, flatFrame());
    QCOMPARE(reg.stats(id).fires, 0);

    qint64 next = 0;
    QVERIFY(reg.eventsSince(0, &next).isEmpty());
}

void TestWatcher::aChangeFiresOnceOnTheRisingEdge() {
    WatcherRegistry reg;
    reg.setLimits(smallLimits());
    reg.ingestImage(0, flatFrame());
    const QRect roi(100, 100, 80, 40);
    const int id = add(&reg, roi);                 // 400 ms cooldown
    QVERIFY(id > 0);

    reg.ingestImage(0, flatFrame());              // baseline
    QCOMPARE(reg.stats(id).fires, 0);

    // The badge appears, and stays. One rising edge, so exactly one fire
    // however many frames it remains on screen — this is the whole point
    // of edge triggering, and the thing a naive "differs from baseline"
    // detector gets wrong by firing on every frame for as long as the
    // badge is visible.
    for (int i = 0; i < 10; ++i) {
        reg.ingestImage(0, framePatched(roi, 220));
    }
    QCOMPARE(reg.stats(id).fires, 1);

    // It vanishing is a change too, so it is a second event — but not
    // inside the cooldown, which is what stops a flicker becoming a
    // stream of them.
    reg.ingestImage(0, flatFrame());
    reg.ingestImage(0, framePatched(roi, 220));
    reg.ingestImage(0, flatFrame());
    QCOMPARE(reg.stats(id).fires, 1);

    QTest::qWait(420);
    reg.ingestImage(0, framePatched(roi, 220));
    QCOMPARE(reg.stats(id).fires, 2);
}

void TestWatcher::aChangeOutsideTheRectIsIgnored() {
    WatcherRegistry reg;
    reg.setLimits(smallLimits());
    reg.ingestImage(0, flatFrame());
    const int id = add(&reg, QRect(0, 0, 60, 60));
    QVERIFY(id > 0);
    reg.ingestImage(0, flatFrame());

    // A change on the far side of the frame. The rectangle is the whole
    // point: a watcher on the tray must not fire because a window moved.
    for (int i = 0; i < 5; ++i) {
        reg.ingestImage(0, framePatched(QRect(400, 300, 200, 100), 240));
    }
    QCOMPARE(reg.stats(id).fires, 0);
}

void TestWatcher::theCooldownSuppressesARapidSecondFire() {
    WatcherRegistry reg;
    reg.setLimits(smallLimits());
    reg.ingestImage(0, flatFrame());
    const QRect roi(50, 50, 60, 60);
    const int id = add(&reg, roi, nullptr, WatcherTrigger::Change, false,
                       /*cooldownMs=*/500);
    QVERIFY(id > 0);
    reg.ingestImage(0, flatFrame());

    reg.ingestImage(0, framePatched(roi, 200));
    QCOMPARE(reg.stats(id).fires, 1);

    // Flicker it hard inside the cooldown window: every one of these is a
    // genuine rising edge, and none of them may fire.
    for (int i = 0; i < 6; ++i) {
        reg.ingestImage(0, flatFrame());
        reg.ingestImage(0, framePatched(roi, 200));
    }
    QCOMPARE(reg.stats(id).fires, 1);

    // Past the cooldown the next edge gets through.
    QTest::qWait(520);
    reg.ingestImage(0, flatFrame());
    QCOMPARE(reg.stats(id).fires, 2);
}

void TestWatcher::aNeverQuietRegionFiresAtTheCooldownRate() {
    // The gap the first draft of the detector had: re-arming only on a
    // quiet frame meant a region that never goes quiet — a progress bar,
    // a clock, a video — fired exactly once and then stayed silent for
    // the rest of the session. The cooldown has to re-arm it too.
    WatcherRegistry reg;
    reg.setLimits(smallLimits());
    reg.ingestImage(0, flatFrame());
    const QRect roi(0, 0, 80, 80);
    const int id = add(&reg, roi, nullptr, WatcherTrigger::Change, false,
                       /*cooldownMs=*/100);
    QVERIFY(id > 0);
    reg.ingestImage(0, flatFrame());

    // Every frame differs from the last, for about four cooldowns.
    QElapsedTimer clock;
    clock.start();
    int shade = 0;
    while (clock.elapsed() < 420) {
        shade = (shade + 60) % 240;
        reg.ingestImage(0, framePatched(roi, 20 + shade));
        QTest::qWait(10);
    }

    const int fires = reg.stats(id).fires;
    QVERIFY2(fires >= 3, qPrintable(QStringLiteral("fired %1 times in ~4 "
                                                   "cooldowns").arg(fires)));
    // And rate limited rather than firing on every one of the ~42 frames.
    QVERIFY2(fires <= 8, qPrintable(QStringLiteral("fired %1 times, the "
                                                   "cooldown is not holding")
                                        .arg(fires)));
}

void TestWatcher::thePointerInsideTheRegionSuppresses() {
    WatcherRegistry reg;
    reg.setLimits(smallLimits());
    reg.ingestImage(0, flatFrame());
    const QRect roi(200, 200, 100, 60);
    const int id = add(&reg, roi);
    QVERIFY(id > 0);
    reg.ingestImage(0, flatFrame());

    // The target's cursor is captured over HDMI, so a pointer crossing the
    // region looks exactly like the event. Glasshouse knows where it put
    // the pointer, so it can rule that out.
    reg.setPointer(0, roi.center());
    for (int i = 0; i < 5; ++i) {
        reg.ingestImage(0, framePatched(roi, 220));
        reg.ingestImage(0, flatFrame());
    }
    QCOMPARE(reg.stats(id).fires, 0);
    QVERIFY(reg.stats(id).suppressed);

    // Pointer moves away, and the region starts reporting again.
    reg.clearPointer(0);
    reg.ingestImage(0, framePatched(roi, 220));
    QCOMPARE(reg.stats(id).fires, 1);
    QVERIFY(!reg.stats(id).suppressed);
}

void TestWatcher::settleFiresOnlyOnceTheRegionGoesQuiet() {
    WatcherRegistry reg;
    reg.setLimits(smallLimits());
    reg.ingestImage(0, flatFrame());
    const QRect roi(0, 0, 120, 120);
    const int id = add(&reg, roi, nullptr, WatcherTrigger::Settle);
    QVERIFY(id > 0);
    reg.ingestImage(0, flatFrame());

    // While it is churning it must not fire — that is the whole difference
    // from Change, and the reason it is a per-watcher property.
    for (int i = 0; i < 8; ++i) {
        reg.ingestImage(0, framePatched(roi, 60 + i * 20));
    }
    QCOMPARE(reg.stats(id).fires, 0);

    // Now hold it still. Quiet frames alone are not enough; settleMs has
    // to have elapsed since the last change.
    const QImage held = framePatched(roi, 200);
    reg.ingestImage(0, held);
    reg.ingestImage(0, held);
    QCOMPARE(reg.stats(id).fires, 0);

    QTest::qWait(60);
    reg.ingestImage(0, held);
    QCOMPARE(reg.stats(id).fires, 1);

    // And it does not re-fire while it stays quiet.
    for (int i = 0; i < 5; ++i) reg.ingestImage(0, held);
    QCOMPARE(reg.stats(id).fires, 1);
}

// ---------------------------------------------------------------------------
// Volatile history
// ---------------------------------------------------------------------------

void TestWatcher::volatileIsRefusedAboveTheAreaCeiling() {
    WatcherRegistry reg;
    WatcherLimits l = smallLimits();
    reg.setLimits(l);
    reg.ingestImage(0, flatFrame());

    // 256 KB / 8 frames / 3 bytes = ~10922 px, so 100x100 fits and
    // 300x300 does not.
    const qint64 ceiling = l.volatileAreaCeiling();
    QVERIFY(ceiling > 100 * 100);
    QVERIFY(ceiling < 300 * 300);

    const int small = add(&reg, QRect(0, 0, 100, 100), nullptr,
                          WatcherTrigger::Change, /*wantVolatile=*/true);
    QVERIFY(small > 0);
    WatcherDef d;
    QVERIFY(reg.get(small, &d));
    QVERIFY(d.volatileWanted);
    QVERIFY(d.volatileGranted);

    const int big = add(&reg, QRect(0, 0, 300, 300), nullptr,
                        WatcherTrigger::Change, /*wantVolatile=*/true);
    // Refused rather than silently degraded: the watcher still exists and
    // still watches, it just keeps no history.
    QVERIFY(big > 0);
    QVERIFY(reg.get(big, &d));
    QVERIFY(d.volatileWanted);
    QVERIFY(!d.volatileGranted);
}

void TestWatcher::theVolatileRingEvictsOldestOverTheCap() {
    WatcherRegistry reg;
    WatcherLimits l = smallLimits();
    reg.setLimits(l);
    reg.ingestImage(0, flatFrame());

    const QRect roi(0, 0, 100, 100);           // 30 KB a frame as RGB888
    const int id = add(&reg, roi, nullptr, WatcherTrigger::Change,
                       /*wantVolatile=*/true, /*cooldownMs=*/50);
    QVERIFY(id > 0);
    reg.ingestImage(0, flatFrame());

    // Churn it so every frame is worth retaining, well past the 256 KB cap.
    for (int i = 0; i < 60; ++i) {
        reg.ingestImage(0, framePatched(roi, 30 + (i % 8) * 25));
    }

    const WatcherStats s = reg.stats(id);
    QVERIFY(s.ringFrames > 1);
    QVERIFY2(s.ringBytes <= l.volatileBytesPerWatcher,
             qPrintable(QStringLiteral("ring is %1 bytes, cap is %2")
                            .arg(s.ringBytes)
                            .arg(l.volatileBytesPerWatcher)));

    // What survives is the newest, because "what just happened" is the
    // question the ring exists to answer.
    const auto kept = reg.frames(id, 0, 0, 0);
    QCOMPARE(kept.size(), s.ringFrames);
    for (int i = 1; i < kept.size(); ++i) {
        QVERIFY(kept[i].wallMs >= kept[i - 1].wallMs);
    }
}

void TestWatcher::aNonVolatileWatcherKeepsNoHistory() {
    WatcherRegistry reg;
    reg.setLimits(smallLimits());
    reg.ingestImage(0, flatFrame());
    const QRect roi(0, 0, 60, 60);
    const int id = add(&reg, roi, nullptr, WatcherTrigger::Change, false, 50);
    QVERIFY(id > 0);
    reg.ingestImage(0, flatFrame());

    for (int i = 0; i < 20; ++i) {
        reg.ingestImage(0, framePatched(roi, 30 + (i % 6) * 30));
    }
    QCOMPARE(reg.stats(id).ringFrames, 0);
    QCOMPARE(reg.stats(id).ringBytes, qint64(0));
    QVERIFY(reg.frames(id, 0, 0, 0).isEmpty());

    // It still fired, and the event still carries a thumbnail — for a
    // watcher with no ring that crop is the only imagery there is.
    QVERIFY(reg.stats(id).fires > 0);
    qint64 next = 0;
    const auto evs = reg.eventsSince(0, &next);
    QVERIFY(!evs.isEmpty());
    QVERIFY(!evs.first().crop.isNull());
}

// ---------------------------------------------------------------------------
// Polling
// ---------------------------------------------------------------------------

void TestWatcher::thePollCursorReturnsOnlyNewEvents() {
    WatcherRegistry reg;
    reg.setLimits(smallLimits());
    reg.ingestImage(0, flatFrame());
    const QRect roi(10, 10, 50, 50);
    const int id = add(&reg, roi, nullptr, WatcherTrigger::Change, false,
                       /*cooldownMs=*/120);
    QVERIFY(id > 0);
    reg.ingestImage(0, flatFrame());

    reg.ingestImage(0, framePatched(roi, 200));
    QCOMPARE(reg.stats(id).fires, 1);
    qint64 cursor = 0;
    auto first = reg.eventsSince(0, &cursor);
    QCOMPARE(first.size(), 1);
    QCOMPARE(first.first().id, id);
    QVERIFY(cursor > 0);
    QCOMPARE(first.first().intent, QStringLiteral("a test watcher"));
    QCOMPARE(first.first().rect, roi);

    // Polling again with the returned cursor yields nothing — the contract
    // HARNESS.md §9.1 rests on, since a consumer polls every 10-30 s.
    qint64 again = 0;
    QVERIFY(reg.eventsSince(cursor, &again).isEmpty());
    QCOMPARE(again, cursor);

    QTest::qWait(150);
    reg.ingestImage(0, flatFrame());
    const auto second = reg.eventsSince(cursor, &again);
    QCOMPARE(second.size(), 1);
    QVERIFY(second.first().seq > cursor);

    // A fresh consumer passing 0 still sees the whole buffer.
    qint64 ignored = 0;
    QCOMPARE(reg.eventsSince(0, &ignored).size(), 2);
}

void TestWatcher::deletingAWatcherDropsItsEvents() {
    WatcherRegistry reg;
    reg.setLimits(smallLimits());
    reg.ingestImage(0, flatFrame());
    const QRect roi(10, 10, 50, 50);
    const int id = add(&reg, roi);
    reg.ingestImage(0, flatFrame());
    reg.ingestImage(0, framePatched(roi, 200));

    qint64 next = 0;
    QCOMPARE(reg.eventsSince(0, &next).size(), 1);

    // §11: a watcher's retained screen content goes when the watcher does.
    QVERIFY(reg.remove(id));
    QVERIFY(!reg.remove(id));
    QVERIFY(reg.eventsSince(0, &next).isEmpty());
    QCOMPARE(reg.count(), 0);
}

// ---------------------------------------------------------------------------
// Sampling and presentation
// ---------------------------------------------------------------------------

void TestWatcher::aLargeRectStillDetectsChange() {
    // A full-frame rectangle is sampled with a stride so it costs about
    // what a small one does. Subsampling must not cost it the detection.
    WatcherRegistry reg;
    reg.setLimits(smallLimits());
    reg.ingestImage(0, flatFrame());
    const QRect roi(0, 0, kW, kH);
    const int id = add(&reg, roi);
    QVERIFY(id > 0);
    reg.ingestImage(0, flatFrame());

    reg.ingestImage(0, framePatched(roi, 180));
    QCOMPARE(reg.stats(id).fires, 1);
}

void TestWatcher::theFilmstripTilesEveryFrame() {
    QList<WatcherFrame> frames;
    for (int i = 0; i < 7; ++i) {
        WatcherFrame f;
        f.wallMs = 1760000000000ll + i * 100;
        f.delta  = i * 1.5;
        QImage c(40, 20, QImage::Format_RGB888);
        c.fill(QColor(10 * i, 20, 30));
        f.crop = c;
        frames.append(f);
    }

    const QImage strip = WatcherRegistry::filmstrip(frames, 3, 0);
    QVERIFY(!strip.isNull());
    // 3 columns x 3 rows of 40x20 cells, plus padding and a label strip
    // under each one, so it must be wider than one cell and taller than
    // three bare crops.
    QVERIFY(strip.width()  >= 3 * 40);
    QVERIFY(strip.height() >= 3 * 20);

    // maxWidth narrows the grid rather than scaling the crops, so the
    // text stays readable.
    const QImage narrow = WatcherRegistry::filmstrip(frames, 3, 100);
    QVERIFY(!narrow.isNull());
    QVERIFY(narrow.width() <= 110);
    QVERIFY(narrow.height() > strip.height());

    QVERIFY(WatcherRegistry::filmstrip({}, 3, 0).isNull());
}

void TestWatcher::aBgraVideoFrameIsReadInPlace() {
    // The regression this pins: ingestFrame's format switch once listed
    // only planar YUV, so the packed BGRA our pipelines emit fell through
    // to a whole-frame toImage() plus a whole-frame greyscale conversion
    // on every frame — about +46% of a core for one small watcher, with
    // nothing in the log to say so. Every other test here drives
    // ingestImage, which is why it went unnoticed.
    WatcherRegistry reg;
    reg.setLimits(smallLimits());

    const QRect roi(100, 100, 80, 60);
    reg.ingestFrame(0, bgraFrame(flatFrame()));
    const int id = add(&reg, roi);
    QVERIFY(id > 0);

    reg.ingestFrame(0, bgraFrame(flatFrame()));           // baseline
    reg.ingestFrame(0, bgraFrame(framePatched(roi, 220)));

    qint64 cursor = 0;
    QCOMPARE(reg.eventsSince(0, &cursor).size(), 1);
    QCOMPARE(reg.slowPathFrames(), qint64(0));
}

void TestWatcher::aVideoFrameAndAnImageAgreeOnTheDelta() {
    // The in-place reader and the conversion fallback must measure the
    // same change, or a threshold tuned on one path is wrong on the other.
    const QRect roi(100, 100, 80, 60);

    WatcherRegistry viaFrame;
    viaFrame.setLimits(smallLimits());
    viaFrame.ingestFrame(0, bgraFrame(flatFrame()));
    const int a = add(&viaFrame, roi);
    viaFrame.ingestFrame(0, bgraFrame(flatFrame()));      // baseline
    viaFrame.ingestFrame(0, bgraFrame(framePatched(roi, 120)));

    WatcherRegistry viaImage;
    viaImage.setLimits(smallLimits());
    viaImage.ingestImage(0, flatFrame());
    const int b = add(&viaImage, roi);
    viaImage.ingestImage(0, flatFrame());                 // baseline
    viaImage.ingestImage(0, framePatched(roi, 120));

    QVERIFY(a > 0 && b > 0);
    qint64 c1 = 0, c2 = 0;
    const auto ef = viaFrame.eventsSince(0, &c1);
    const auto ei = viaImage.eventsSince(0, &c2);
    QCOMPARE(ef.size(), 1);
    QCOMPARE(ei.size(), 1);
    // Rounding differs (BT.601 integer weights vs Qt's greyscale), so
    // allow a point of slack rather than demanding bit equality.
    QVERIFY2(qAbs(ef.first().delta - ei.first().delta) < 1.0,
             qPrintable(QStringLiteral("frame %1 vs image %2")
                            .arg(ef.first().delta).arg(ei.first().delta)));
}

QTEST_MAIN(TestWatcher)
#include "test_watcher.moc"
