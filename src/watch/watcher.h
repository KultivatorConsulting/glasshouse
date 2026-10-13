#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QString>

#include <functional>

class QVideoFrame;

namespace glasshouse {

// What makes a watcher fire. HARNESS.md §5.3: a notification toast and
// "the page finished loading" want opposite capture rules, so this is a
// per-watcher property rather than a global policy.
enum class WatcherTrigger {
    Change,   // fire the instant the region changes — transients
    Settle,   // fire once a changing region has gone quiet — steady state
};

QString watcherTriggerName(WatcherTrigger t);
bool    parseWatcherTrigger(const QString& name, WatcherTrigger* out,
                            QString* error);

// How the rectangle was obtained. HARNESS.md §5.5: dragging is a peer of
// describing, not a fallback. It matters beyond bookkeeping because the
// two recover from drift differently — a described rect can replay its
// sentence, a dragged one can only template-match its reference patch.
enum class WatcherOrigin { Manual, Described };

QString watcherOriginName(WatcherOrigin o);

// One registered watcher, as the caller asked for it.
struct WatcherDef {
    int            id     = 0;      // assigned by the registry
    int            screen = 0;      // 0-based session screen index
    QRect          rect;            // frame pixels (HARNESS.md §5.1)
    QString        intent;          // why the user cares; step 5 needs it
    WatcherOrigin  origin  = WatcherOrigin::Manual;
    WatcherTrigger trigger = WatcherTrigger::Change;

    // Mean absolute luma delta against the previous frame, 1..255, above
    // which the region counts as changing. Low numbers are deliberately
    // fine: HARNESS.md §5 wants the local detector trigger-happy, because
    // whatever consumes the event is what decides if it mattered.
    int threshold  = 0;
    int cooldownMs = 0;             // floor on how often this can fire
    int settleMs   = 0;             // Settle: quiet for this long = fired
    bool notify    = false;         // ask the host to raise a notification

    // What the caller wanted, and what the budget actually allowed.
    // HARNESS.md §5.4: above the ceiling, volatile is refused rather than
    // silently degraded, and the user is told which of the two they got.
    bool volatileWanted  = false;
    bool volatileGranted = false;
};

// A firing. `crop` is a thumbnail of what tripped it — full-resolution
// history comes from the ring, via WatcherRegistry::frames().
struct WatcherEvent {
    int     id    = 0;
    qint64  seq   = 0;        // monotonic across all watchers; the poll cursor
    qint64  wallMs = 0;       // wall clock, because "at 14:32" is the point
    double  delta = 0;        // the metric that tripped it
    QString intent;
    int     screen = 0;
    QRect   rect;
    QImage  crop;
};

// One retained frame in a volatile watcher's ring.
struct WatcherFrame {
    qint64 wallMs = 0;
    double delta  = 0;
    QImage crop;              // RGB888, native crop resolution
};

// Budgets. Defaults are HARNESS.md §5.4 and §10: ten watchers at the
// ceiling, 16 MB of history each, 96 MB across all of them, and a minimum
// useful depth of 30 frames — which is what sets the area ceiling for a
// volatile region rather than it being a second number to keep in step.
struct WatcherLimits {
    int    maxWatchers       = 10;
    int    defaultThreshold  = 12;
    int    defaultCooldownMs = 3000;
    int    defaultSettleMs   = 600;
    qint64 volatileBytesPerWatcher = 16ll * 1024 * 1024;
    qint64 volatilePoolBytes       = 96ll * 1024 * 1024;
    int    minVolatileDepth        = 30;

    // Pixels a volatile region may cover, derived from the above.
    qint64 volatileAreaCeiling() const {
        return volatileBytesPerWatcher / minVolatileDepth / 3;   // RGB888
    }
};

struct WatcherStats {
    int    id = 0;
    int    fires = 0;
    qint64 lastFireMs = 0;
    double lastDelta = 0;
    int    ringFrames = 0;
    qint64 ringBytes = 0;
    bool   suppressed = false;      // pointer is sitting in the region
};

// The registry and the detector, in one object on the GUI thread.
//
// Detection is local, free and runs on every decoded frame rather than on
// a timer (HARNESS.md §5.3): a toast lives a few seconds, so a ten-second
// poll would miss most of them. The metric is the mean absolute luma delta
// against the previous frame over the watcher's rectangle, sampled with a
// stride so a careless full-screen rectangle cannot cost more than a small
// one. Luma only, and against the previous frame rather than a fixed
// baseline — which is what makes it edge-triggered for free: a badge
// appearing is a brief spike, and a badge that stays put goes quiet again.
//
// Nothing is pushed anywhere. Events queue until something collects them
// with eventsSince(), which is HARNESS.md §9.1's whole argument: no
// subscriber registry, no outbound connection, no "what if the consumer is
// down", and any harness works because polling needs no protocol feature.
class WatcherRegistry : public QObject {
    Q_OBJECT
public:
    explicit WatcherRegistry(QObject* parent = nullptr);

    void setLimits(const WatcherLimits& limits) { m_limits = limits; }
    const WatcherLimits& limits() const { return m_limits; }

    // Tells the registry how big each screen's frames are, so a rectangle
    // can be validated at create time rather than silently never matching.
    void setFrameSize(int screen, const QSize& size);
    QSize frameSize(int screen) const;

    // Registers `def`, filling in defaults for anything left at 0 and
    // deciding whether volatile history fits the budget. Returns the new
    // id, or 0 with `error` set. `def.volatileGranted` on the stored copy
    // says what the caller actually got.
    int create(const WatcherDef& def, QString* error);

    bool remove(int id);
    void clear();

    QList<WatcherDef> list() const;
    bool              get(int id, WatcherDef* out) const;
    WatcherStats      stats(int id) const;
    int               count() const { return m_order.size(); }

    // Events with seq > cursor, oldest first, plus the cursor to pass next
    // time. A fresh consumer passes 0 and gets whatever is still buffered.
    QList<WatcherEvent> eventsSince(qint64 cursor, qint64* nextCursor) const;
    qint64              cursor() const { return m_seq; }

    // Test seam: frames that took the whole-frame conversion rather than
    // being read in place. Both paths produce identical events, so this is
    // the only way a test can tell them apart — and a dead in-place branch
    // shipped once already because nothing could see which one ran.
    qint64              slowPathFrames() const { return m_slowPathFrames; }

    // Retained frames for one watcher, within [fromMs, toMs] when either is
    // non-zero, newest-last. `limit` 0 means all of them.
    QList<WatcherFrame> frames(int id, qint64 fromMs, qint64 toMs,
                               int limit) const;

    // Frame input. Both paths end up in the same detector; the QVideoFrame
    // one is the production path and reads the decoder's luma plane in
    // place where it can, the QImage one is for tests and for frame formats
    // with no plane we can sample directly.
    void ingestFrame(int screen, const QVideoFrame& frame);
    void ingestImage(int screen, const QImage& image);

    // Where the target's pointer is, in frame pixels. HARNESS.md §5.2: the
    // PiKVM captures the target's cursor over HDMI, so the pointer crossing
    // a region trips it — and Glasshouse knows where it put the pointer, so
    // this defence is free. An invalid point means "no idea", which
    // suppresses nothing.
    void setPointer(int screen, const QPoint& framePt);
    void clearPointer(int screen);

    // Tiles retained crops into one image with each one's wall-clock time
    // drawn beneath it (HARNESS.md §5.4): a model that accepts four images
    // per request can read dozens of moments this way. Returns a null image
    // when there is nothing to tile.
    static QImage filmstrip(const QList<WatcherFrame>& frames, int maxCols,
                            int maxWidth);

signals:
    // A watcher fired. The viewer uses this to surface the event locally;
    // delivery to a consumer is still a poll away.
    void fired(const glasshouse::WatcherEvent& event);

private:
    // A frame's pixels, flattened enough to read luma out of without
    // caring whether the decoder handed us a YUV plane or an RGB buffer.
    struct PixelView {
        const uchar* bits  = nullptr;
        qsizetype bytesPerLine = 0;
        int  bytesPerPixel = 0;
        int  width = 0, height = 0;
        bool isLuma = true;      // `bits` is a luma/Y plane
        int  lumaOffset = 0;     // byte within the pixel (1 for 10/16-bit Y)
        int  rOffset = 0, gOffset = 1, bOffset = 2;

        bool valid() const {
            return bits && bytesPerLine > 0 && bytesPerPixel > 0;
        }
        inline int luma(int x, int y) const {
            const uchar* p = bits + qsizetype(y) * bytesPerLine +
                             qsizetype(x) * bytesPerPixel;
            if (isLuma) return p[lumaOffset];
            return (p[rOffset] * 77 + p[gOffset] * 151 + p[bOffset] * 28) >> 8;
        }
    };

    struct State {
        WatcherDef def;
        QByteArray prevLuma;        // samples from the previous frame
        int        stride = 1;      // sampling stride, both axes
        bool       armed  = true;   // rising-edge gate
        bool       changing = false;  // Settle: has been moving
        qint64     lastFireMs   = 0;
        qint64     lastChangeMs = 0;
        double     lastDelta = 0;
        int        fires = 0;
        bool       suppressed = false;
        QList<WatcherFrame> ring;
        qint64     ringBytes = 0;
    };

    // Runs every registered watcher for `screen` against one frame.
    // `view` is already mapped; `colour` lazily produces the full frame as
    // an image, because only a retained or firing frame needs one.
    void detect(int screen, const PixelView& view,
                const std::function<QImage()>& colour);

    // Samples the rect's luma into `out` at `stride`, returning the number
    // of samples written.
    static int sampleLuma(const PixelView& view, const QRect& rect, int stride,
                          QByteArray* out);

    // Stride that keeps a rectangle under kMaxSamples, so a full-screen
    // watcher costs about the same per frame as a tray-badge one.
    static int strideFor(const QRect& rect);

    void retain(State& st, const QImage& crop, qint64 wallMs, double delta);
    void evictGlobal();
    qint64 poolBytes() const;

    static constexpr int kMaxSamples   = 1 << 16;  // 65536 per watcher/frame
    static constexpr int kEventCap     = 500;
    static constexpr int kEventThumbW  = 640;

    WatcherLimits         m_limits;
    QHash<int, State>     m_watchers;
    QList<int>            m_order;        // creation order, for list()
    QHash<int, QSize>     m_frameSizes;
    QHash<int, QPoint>    m_pointers;
    QSet<int>             m_pathLogged;   // one line per screen, not per frame
    qint64                m_slowPathFrames = 0;
    QList<WatcherEvent>   m_events;
    qint64                m_seq     = 0;
    int                   m_nextId  = 1;
};

}  // namespace glasshouse
