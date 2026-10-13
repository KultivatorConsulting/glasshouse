#include "watcher.h"

#include "logging.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QPainter>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <algorithm>
#include <cmath>

namespace glasshouse {

namespace {

// Pointer suppression is generous on purpose: a cursor's hotspot is not
// its drawn extent, and a hover highlight reaches past the glyph.
constexpr int kPointerMarginPx = 24;

// Below this a retained frame is not worth keeping. HARNESS.md §5.4 wants
// retention driven by the same metric that drives the trigger, so a quiet
// region costs one frame for as long as it stays quiet.
int retainThresholdFor(int fireThreshold) {
    return std::max(1, fireThreshold / 4);
}

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

QString stampOf(qint64 wallMs) {
    return QDateTime::fromMSecsSinceEpoch(wallMs).toString(
        QStringLiteral("HH:mm:ss.zzz"));
}


// One line per screen, the first time a frame arrives for it. Which branch
// ingestFrame takes is a 2x difference in viewer CPU, and it used to be
// invisible from the outside — the slow path logged nothing at all.
void logPixelPath(QSet<int>& logged, int screen,
                  QVideoFrameFormat::PixelFormat fmt, bool inPlace) {
    if (logged.contains(screen)) return;
    logged.insert(screen);
    qCInfo(lcWatch).nospace()
        << "watcher: screen " << screen << " frames are "
        << QVideoFrameFormat::pixelFormatToString(fmt)
        << (inPlace ? " - sampled in place"
                    : " - no in-place reader for this format, converting the "
                      "whole frame every frame (expect noticeably more CPU)");
}

}  // namespace

QString watcherTriggerName(WatcherTrigger t) {
    switch (t) {
        case WatcherTrigger::Change: return QStringLiteral("change");
        case WatcherTrigger::Settle: return QStringLiteral("settle");
    }
    return QStringLiteral("change");
}

bool parseWatcherTrigger(const QString& name, WatcherTrigger* out,
                         QString* error) {
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("change")) { *out = WatcherTrigger::Change; return true; }
    if (n == QLatin1String("settle")) { *out = WatcherTrigger::Settle; return true; }
    if (error) {
        *error = QStringLiteral("trigger '%1' is not one of: change, settle")
                     .arg(name);
    }
    return false;
}

QString watcherOriginName(WatcherOrigin o) {
    return o == WatcherOrigin::Manual ? QStringLiteral("manual")
                                      : QStringLiteral("described");
}

WatcherRegistry::WatcherRegistry(QObject* parent) : QObject(parent) {}

void WatcherRegistry::setFrameSize(int screen, const QSize& size) {
    if (size.isEmpty()) return;
    m_frameSizes.insert(screen, size);
}

QSize WatcherRegistry::frameSize(int screen) const {
    return m_frameSizes.value(screen);
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

int WatcherRegistry::create(const WatcherDef& in, QString* error) {
    const auto fail = [error](const QString& msg) {
        if (error) *error = msg;
        return 0;
    };

    if (m_order.size() >= m_limits.maxWatchers) {
        return fail(QStringLiteral("the watcher limit of %1 is reached — "
                                   "delete one first")
                        .arg(m_limits.maxWatchers));
    }
    if (!in.rect.isValid() || in.rect.isEmpty()) {
        return fail(QStringLiteral("the rectangle is empty"));
    }
    if (in.intent.trimmed().isEmpty()) {
        // HARNESS.md §5.5: a rectangle says where to look and nothing about
        // what to care about, so the sentence is not optional even when the
        // rectangle was dragged rather than described.
        return fail(QStringLiteral("`intent` is required: the rectangle says "
                                   "where to look, not what matters about it"));
    }

    const QSize fs = m_frameSizes.value(in.screen);
    if (fs.isEmpty()) {
        return fail(QStringLiteral("screen %1 has no frames yet — the stream "
                                   "may still be connecting")
                        .arg(in.screen));
    }
    if (!QRect(QPoint(0, 0), fs).contains(in.rect)) {
        return fail(QStringLiteral("the rectangle %1x%2+%3+%4 is not inside "
                                   "screen %5's %6x%7 frame")
                        .arg(in.rect.width()).arg(in.rect.height())
                        .arg(in.rect.x()).arg(in.rect.y())
                        .arg(in.screen).arg(fs.width()).arg(fs.height()));
    }

    State st;
    st.def = in;
    st.def.id = m_nextId++;
    if (st.def.threshold <= 0)  st.def.threshold  = m_limits.defaultThreshold;
    if (st.def.cooldownMs <= 0) st.def.cooldownMs = m_limits.defaultCooldownMs;
    if (st.def.settleMs <= 0)   st.def.settleMs   = m_limits.defaultSettleMs;
    st.def.threshold = std::clamp(st.def.threshold, 1, 255);
    st.stride = strideFor(st.def.rect);

    // HARNESS.md §5.4: a large volatile region is the footgun — full-screen
    // at frame rate would exhaust the whole pool in under two seconds — so
    // above the ceiling history is refused and the watcher still works.
    st.def.volatileGranted = false;
    if (st.def.volatileWanted) {
        const qint64 area = qint64(st.def.rect.width()) * st.def.rect.height();
        if (area <= m_limits.volatileAreaCeiling()) {
            st.def.volatileGranted = true;
        } else {
            qCInfo(lcWatch).nospace()
                << "watcher " << st.def.id << ": volatile refused, "
                << st.def.rect.width() << "x" << st.def.rect.height()
                << " is " << area << " px against a ceiling of "
                << m_limits.volatileAreaCeiling();
        }
    }

    const int id = st.def.id;
    m_watchers.insert(id, st);
    m_order.append(id);

    qCInfo(lcWatch).nospace()
        << "watcher " << id << " on screen " << st.def.screen << " "
        << st.def.rect.width() << "x" << st.def.rect.height() << "+"
        << st.def.rect.x() << "+" << st.def.rect.y()
        << " trigger=" << watcherTriggerName(st.def.trigger)
        << " threshold=" << st.def.threshold
        << " cooldown=" << st.def.cooldownMs << "ms"
        << " stride=" << st.stride
        << (st.def.volatileGranted ? " volatile" : "")
        << " origin=" << watcherOriginName(st.def.origin)
        << ": " << st.def.intent;
    return id;
}

bool WatcherRegistry::remove(int id) {
    if (!m_watchers.contains(id)) return false;
    m_watchers.remove(id);
    m_order.removeAll(id);
    // §11: a volatile buffer is retained screen content, so it goes when
    // the watcher does rather than lingering until something reuses it.
    for (int i = m_events.size() - 1; i >= 0; --i) {
        if (m_events[i].id == id) m_events.removeAt(i);
    }
    qCInfo(lcWatch) << "watcher" << id << "removed";
    return true;
}

void WatcherRegistry::clear() {
    m_watchers.clear();
    m_order.clear();
    m_events.clear();
}

QList<WatcherDef> WatcherRegistry::list() const {
    QList<WatcherDef> out;
    out.reserve(m_order.size());
    for (int id : m_order) {
        const auto it = m_watchers.find(id);
        if (it != m_watchers.end()) out.append(it->def);
    }
    return out;
}

bool WatcherRegistry::get(int id, WatcherDef* out) const {
    const auto it = m_watchers.find(id);
    if (it == m_watchers.end()) return false;
    if (out) *out = it->def;
    return true;
}

WatcherStats WatcherRegistry::stats(int id) const {
    WatcherStats s;
    const auto it = m_watchers.find(id);
    if (it == m_watchers.end()) return s;
    s.id         = id;
    s.fires      = it->fires;
    s.lastFireMs = it->lastFireMs;
    s.lastDelta  = it->lastDelta;
    s.ringFrames = it->ring.size();
    s.ringBytes  = it->ringBytes;
    s.suppressed = it->suppressed;
    return s;
}

QList<WatcherEvent> WatcherRegistry::eventsSince(qint64 cursor,
                                                 qint64* nextCursor) const {
    QList<WatcherEvent> out;
    for (const auto& e : m_events) {
        if (e.seq > cursor) out.append(e);
    }
    if (nextCursor) *nextCursor = m_seq;
    return out;
}

QList<WatcherFrame> WatcherRegistry::frames(int id, qint64 fromMs, qint64 toMs,
                                            int limit) const {
    QList<WatcherFrame> out;
    const auto it = m_watchers.find(id);
    if (it == m_watchers.end()) return out;
    for (const auto& f : it->ring) {
        if (fromMs > 0 && f.wallMs < fromMs) continue;
        if (toMs   > 0 && f.wallMs > toMs)   continue;
        out.append(f);
    }
    // Keep the newest when there are more than asked for: the question is
    // almost always "what just happened".
    if (limit > 0 && out.size() > limit) out = out.mid(out.size() - limit);
    return out;
}

void WatcherRegistry::setPointer(int screen, const QPoint& framePt) {
    m_pointers.insert(screen, framePt);
}

void WatcherRegistry::clearPointer(int screen) { m_pointers.remove(screen); }

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

int WatcherRegistry::strideFor(const QRect& rect) {
    const qint64 area = qint64(rect.width()) * rect.height();
    if (area <= kMaxSamples) return 1;
    // ceil(sqrt(area / kMaxSamples)) — one stride for both axes keeps the
    // sampled grid square, which matters for a thin region.
    const double s = std::sqrt(double(area) / double(kMaxSamples));
    return std::max(1, int(std::ceil(s)));
}

int WatcherRegistry::sampleLuma(const PixelView& view, const QRect& rect,
                                int stride, QByteArray* out) {
    const int x0 = std::max(0, rect.x());
    const int y0 = std::max(0, rect.y());
    const int x1 = std::min(view.width,  rect.x() + rect.width());
    const int y1 = std::min(view.height, rect.y() + rect.height());
    if (x1 <= x0 || y1 <= y0) { out->clear(); return 0; }

    const int cols = (x1 - x0 + stride - 1) / stride;
    const int rows = (y1 - y0 + stride - 1) / stride;
    out->resize(qsizetype(cols) * rows);
    uchar* dst = reinterpret_cast<uchar*>(out->data());

    int n = 0;
    for (int y = y0; y < y1; y += stride) {
        for (int x = x0; x < x1; x += stride) {
            dst[n++] = static_cast<uchar>(view.luma(x, y));
        }
    }
    return n;
}

void WatcherRegistry::ingestImage(int screen, const QImage& image) {
    if (image.isNull()) return;
    setFrameSize(screen, image.size());
    if (m_watchers.isEmpty()) return;

    // Grayscale8 is a luma plane whatever the source format was, which
    // means one conversion covers every QImage the caller might hand us.
    const QImage gray = image.format() == QImage::Format_Grayscale8
                            ? image
                            : image.convertToFormat(QImage::Format_Grayscale8);
    if (gray.isNull()) return;

    PixelView view;
    view.bits          = gray.constBits();
    view.bytesPerLine  = gray.bytesPerLine();
    view.bytesPerPixel = 1;
    view.width         = gray.width();
    view.height        = gray.height();
    view.isLuma        = true;

    detect(screen, view, [image]() { return image; });
}

void WatcherRegistry::ingestFrame(int screen, const QVideoFrame& frame) {
    if (!frame.isValid()) return;
    setFrameSize(screen, frame.size());
    if (m_watchers.isEmpty()) return;

    bool haveWatcher = false;
    for (int id : m_order) {
        const auto it = m_watchers.find(id);
        if (it != m_watchers.end() && it->def.screen == screen) {
            haveWatcher = true;
            break;
        }
    }
    if (!haveWatcher) return;

    // Which bytes to read, and how. Glasshouse's own pipelines decide this,
    // not the decoder: mjpegpipeline and videopipeline both put
    // `videoconvert ! video/x-raw,format=BGRA` in front of their appsink,
    // so what arrives here is packed BGRA. The first version of this switch
    // handled only planar YUV, on the assumption that a decoder hands back
    // NV12 or I420 — so in practice every frame fell through to the
    // whole-frame conversion below, measured at about +46% of a core for a
    // single 42x22 watcher. Everything we can read in place is listed.
    PixelView view;
    bool inPlace = true;
    switch (frame.pixelFormat()) {
        // Packed 32-bit. The offsets are byte positions within a pixel,
        // which is what these names mean in Qt Multimedia — unlike QImage,
        // whose ARGB32 names a word and so reverses on little-endian.
        case QVideoFrameFormat::Format_BGRA8888:
        case QVideoFrameFormat::Format_BGRA8888_Premultiplied:
        case QVideoFrameFormat::Format_BGRX8888:
            view.bytesPerPixel = 4; view.isLuma = false;
            view.bOffset = 0; view.gOffset = 1; view.rOffset = 2;
            break;
        case QVideoFrameFormat::Format_RGBA8888:
        case QVideoFrameFormat::Format_RGBX8888:
            view.bytesPerPixel = 4; view.isLuma = false;
            view.rOffset = 0; view.gOffset = 1; view.bOffset = 2;
            break;
        case QVideoFrameFormat::Format_ARGB8888:
        case QVideoFrameFormat::Format_ARGB8888_Premultiplied:
        case QVideoFrameFormat::Format_XRGB8888:
            view.bytesPerPixel = 4; view.isLuma = false;
            view.rOffset = 1; view.gOffset = 2; view.bOffset = 3;
            break;
        case QVideoFrameFormat::Format_ABGR8888:
        case QVideoFrameFormat::Format_XBGR8888:
            view.bytesPerPixel = 4; view.isLuma = false;
            view.bOffset = 1; view.gOffset = 2; view.rOffset = 3;
            break;

        // Planar and semi-planar YUV: plane 0 is the luma we want.
        case QVideoFrameFormat::Format_NV12:
        case QVideoFrameFormat::Format_NV21:
        case QVideoFrameFormat::Format_IMC1:
        case QVideoFrameFormat::Format_IMC2:
        case QVideoFrameFormat::Format_IMC3:
        case QVideoFrameFormat::Format_IMC4:
        case QVideoFrameFormat::Format_YUV420P:
        case QVideoFrameFormat::Format_YUV422P:
        case QVideoFrameFormat::Format_YV12:
        case QVideoFrameFormat::Format_Y8:
            view.bytesPerPixel = 1; view.lumaOffset = 0;
            break;
        case QVideoFrameFormat::Format_P010:
        case QVideoFrameFormat::Format_P016:
        case QVideoFrameFormat::Format_Y16:
        case QVideoFrameFormat::Format_YUV420P10:
            view.bytesPerPixel = 2; view.lumaOffset = 1;  // LE: the high byte
            break;

        // Packed YUV 4:2:2 and 4:4:4 — luma is every second or fourth byte.
        case QVideoFrameFormat::Format_YUYV:
            view.bytesPerPixel = 2; view.lumaOffset = 0;
            break;
        case QVideoFrameFormat::Format_UYVY:
            view.bytesPerPixel = 2; view.lumaOffset = 1;
            break;
        case QVideoFrameFormat::Format_AYUV:
        case QVideoFrameFormat::Format_AYUV_Premultiplied:
            view.bytesPerPixel = 4; view.lumaOffset = 1;
            break;

        default:
            inPlace = false;
            break;
    }

    if (inPlace) {
        QVideoFrame f = frame;   // map() is non-const
        if (f.map(QVideoFrame::ReadOnly)) {
            view.bits         = f.bits(0);
            view.bytesPerLine = f.bytesPerLine(0);
            view.width        = f.width();
            view.height       = f.height();

            if (view.valid()) {
                logPixelPath(m_pathLogged, screen, frame.pixelFormat(), true);
                // The colour closure runs only for a frame worth keeping,
                // so a quiet region never pays for a conversion. It cannot
                // read the mapped frame, so it unmaps first via a copy.
                QVideoFrame copy = frame;
                detect(screen, view, [copy]() mutable {
                    return copy.toImage();
                });
                f.unmap();
                return;
            }
            f.unmap();
        }
    }

    ++m_slowPathFrames;
    logPixelPath(m_pathLogged, screen, frame.pixelFormat(), false);

    QVideoFrame copy = frame;
    const QImage img = copy.toImage();
    if (img.isNull()) return;
    ingestImage(screen, img);
}

void WatcherRegistry::detect(int screen, const PixelView& view,
                             const std::function<QImage()>& colour) {
    const qint64 now = nowMs();
    QImage       full;          // produced at most once per frame
    bool         haveFull = false;
    const auto   fullFrame = [&]() -> const QImage& {
        if (!haveFull) { full = colour(); haveFull = true; }
        return full;
    };

    for (int id : m_order) {
        auto it = m_watchers.find(id);
        if (it == m_watchers.end()) continue;
        State& st = *it;
        if (st.def.screen != screen) continue;

        QByteArray cur;
        const int n = sampleLuma(view, st.def.rect, st.stride, &cur);
        if (n <= 0) continue;

        if (st.prevLuma.size() != cur.size()) {
            // First frame, or the frame geometry changed under us. Either
            // way there is nothing to compare against yet.
            st.prevLuma = cur;
            continue;
        }

        const uchar* a = reinterpret_cast<const uchar*>(st.prevLuma.constData());
        const uchar* b = reinterpret_cast<const uchar*>(cur.constData());
        qint64 sum = 0;
        for (int i = 0; i < n; ++i) {
            sum += std::abs(int(a[i]) - int(b[i]));
        }
        const double delta = double(sum) / double(n);
        st.prevLuma = cur;
        st.lastDelta = delta;

        // HARNESS.md §5.2: the target's own cursor is captured over HDMI,
        // so the pointer crossing the region is the single noisiest source
        // of false positives — and it is the one we can rule out for free.
        st.suppressed = false;
        const auto ptr = m_pointers.constFind(screen);
        if (ptr != m_pointers.constEnd()) {
            const QRect guard =
                st.def.rect.adjusted(-kPointerMarginPx, -kPointerMarginPx,
                                     kPointerMarginPx, kPointerMarginPx);
            if (guard.contains(*ptr)) st.suppressed = true;
        }

        const bool changing = delta >= st.def.threshold;
        if (changing) st.lastChangeMs = now;

        if (st.def.volatileGranted) {
            const bool worth = delta >= retainThresholdFor(st.def.threshold);
            if (st.ring.isEmpty() || worth) {
                const QImage& f = fullFrame();
                if (!f.isNull()) {
                    retain(st, f.copy(st.def.rect), now, delta);
                }
            }
        }

        if (st.suppressed) {
            // Keep the baseline moving, but neither fire nor arm: a cursor
            // leaving the region must not read as the event.
            st.armed = false;
            continue;
        }

        bool fire = false;
        if (st.def.trigger == WatcherTrigger::Change) {
            // Two ways to re-arm. A quiet frame is the normal one, and it
            // is what makes a toast appearing and a toast vanishing two
            // separate events. The cooldown elapsing is the other, and it
            // is load bearing: a region that never goes quiet — a progress
            // bar, a clock, a video — would otherwise fire exactly once
            // and then stay silent for the rest of the session. With this,
            // a sustained change reports at the cooldown's rate, which is
            // the job the cooldown is named for.
            const bool cooledDown = st.lastFireMs == 0 ||
                                    now - st.lastFireMs >= st.def.cooldownMs;
            if (!changing || cooledDown) st.armed = true;
            if (changing && st.armed)    fire = true;
        } else {
            // Settle: the region has to have been moving, then go quiet for
            // settleMs. "The page finished loading" is the motivating case.
            if (changing) {
                st.changing = true;
                st.armed = true;
            } else if (st.changing && st.armed &&
                       now - st.lastChangeMs >= st.def.settleMs) {
                fire = true;
                st.changing = false;
            }
        }

        if (!fire) continue;
        if (st.lastFireMs > 0 && now - st.lastFireMs < st.def.cooldownMs) {
            // Within the cooldown. Disarm so the next rising edge after it
            // expires still fires, rather than needing a fresh transition.
            st.armed = false;
            continue;
        }

        st.armed = false;
        st.lastFireMs = now;
        st.fires++;

        WatcherEvent ev;
        ev.id     = st.def.id;
        ev.seq    = ++m_seq;
        ev.wallMs = now;
        ev.delta  = delta;
        ev.intent = st.def.intent;
        ev.screen = st.def.screen;
        ev.rect   = st.def.rect;

        // A thumbnail of what tripped it. The full-resolution history is in
        // the ring for a volatile watcher; for a plain one this is all the
        // imagery there is, so it is worth the one conversion.
        const QImage& f = fullFrame();
        if (!f.isNull()) {
            QImage crop = f.copy(st.def.rect);
            if (crop.width() > kEventThumbW) {
                crop = crop.scaledToWidth(kEventThumbW,
                                          Qt::SmoothTransformation);
            }
            ev.crop = crop;
        }

        m_events.append(ev);
        while (m_events.size() > kEventCap) m_events.removeFirst();

        if (st.def.trigger == WatcherTrigger::Change) {
            qCInfo(lcWatch).nospace()
                << "watcher " << st.def.id << " fired (change: delta "
                << delta << " >= " << st.def.threshold << "): "
                << st.def.intent;
        } else {
            qCInfo(lcWatch).nospace()
                << "watcher " << st.def.id << " fired (settled: quiet for "
                << (now - st.lastChangeMs) << "ms >= " << st.def.settleMs
                << "ms): " << st.def.intent;
        }
        emit fired(ev);
    }
}

// ---------------------------------------------------------------------------
// Volatile history
// ---------------------------------------------------------------------------

void WatcherRegistry::retain(State& st, const QImage& crop, qint64 wallMs,
                             double delta) {
    if (crop.isNull()) return;

    WatcherFrame f;
    f.wallMs = wallMs;
    f.delta  = delta;
    // RGB888 rather than ARGB32: a quarter less memory for a crop that has
    // no alpha to carry, and it is the figure §5.4's sizing assumes.
    f.crop   = crop.format() == QImage::Format_RGB888
                   ? crop
                   : crop.convertToFormat(QImage::Format_RGB888);
    if (f.crop.isNull()) return;

    st.ring.append(f);
    st.ringBytes += f.crop.sizeInBytes();

    while (st.ring.size() > 1 &&
           st.ringBytes > m_limits.volatileBytesPerWatcher) {
        st.ringBytes -= st.ring.first().crop.sizeInBytes();
        st.ring.removeFirst();
    }
    if (poolBytes() > m_limits.volatilePoolBytes) evictGlobal();
}

qint64 WatcherRegistry::poolBytes() const {
    qint64 total = 0;
    for (const auto& st : m_watchers) total += st.ringBytes;
    return total;
}

void WatcherRegistry::evictGlobal() {
    // Oldest frame first, wherever it lives, until the pool fits. Every
    // watcher keeps at least its most recent frame, so a busy region can
    // never starve a quiet one of the thing it is actually watching.
    while (poolBytes() > m_limits.volatilePoolBytes) {
        int    victim = 0;
        qint64 oldest = 0;
        for (auto it = m_watchers.begin(); it != m_watchers.end(); ++it) {
            if (it->ring.size() <= 1) continue;
            const qint64 t = it->ring.first().wallMs;
            if (victim == 0 || t < oldest) { victim = it.key(); oldest = t; }
        }
        if (victim == 0) break;    // nothing left that is safe to drop
        State& st = m_watchers[victim];
        st.ringBytes -= st.ring.first().crop.sizeInBytes();
        st.ring.removeFirst();
    }
}

// ---------------------------------------------------------------------------
// Filmstrip
// ---------------------------------------------------------------------------

QImage WatcherRegistry::filmstrip(const QList<WatcherFrame>& frames,
                                  int maxCols, int maxWidth) {
    if (frames.isEmpty()) return {};

    int cellW = 0, cellH = 0;
    for (const auto& f : frames) {
        cellW = std::max(cellW, f.crop.width());
        cellH = std::max(cellH, f.crop.height());
    }
    if (cellW <= 0 || cellH <= 0) return {};

    constexpr int kPad   = 6;
    constexpr int kLabel = 16;

    int cols = std::max(1, maxCols > 0 ? maxCols : 3);
    cols = std::min<int>(cols, frames.size());
    if (maxWidth > 0) {
        const int fits = std::max(1, (maxWidth - kPad) / (cellW + kPad));
        cols = std::min(cols, fits);
    }
    const int rows = (frames.size() + cols - 1) / cols;

    QImage out(cols * (cellW + kPad) + kPad,
               rows * (cellH + kLabel + kPad) + kPad,
               QImage::Format_RGB888);
    if (out.isNull()) return {};
    out.fill(QColor(24, 24, 24));

    QPainter p(&out);
    QFont font = p.font();
    font.setPixelSize(11);
    p.setFont(font);

    for (int i = 0; i < frames.size(); ++i) {
        const int cx = kPad + (i % cols) * (cellW + kPad);
        const int cy = kPad + (i / cols) * (cellH + kLabel + kPad);
        p.drawImage(QPoint(cx, cy), frames[i].crop);
        p.setPen(QColor(200, 200, 200));
        p.drawText(QRect(cx, cy + cellH, cellW, kLabel),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("%1  Δ%2")
                       .arg(stampOf(frames[i].wallMs))
                       .arg(frames[i].delta, 0, 'f', 1));
    }
    p.end();
    return out;
}

}  // namespace glasshouse
