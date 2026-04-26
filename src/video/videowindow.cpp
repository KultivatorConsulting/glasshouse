#include "videowindow.h"
#include "coord_transform.h"
#include "keymap.h"
#include "logging.h"

#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
#include <QShortcut>
#include <QStatusBar>
#include <QVideoSink>
#include <QVideoWidget>
#include <QWheelEvent>

#include <optional>

namespace glasshouse {

namespace {

std::optional<MouseButton> qtToMouseButton(Qt::MouseButton qt) {
    switch (qt) {
        case Qt::LeftButton:    return MouseButton::Left;
        case Qt::RightButton:   return MouseButton::Right;
        case Qt::MiddleButton:  return MouseButton::Middle;
        case Qt::BackButton:    return MouseButton::Back;
        case Qt::ForwardButton: return MouseButton::Forward;
        default:                return std::nullopt;
    }
}

// Decode the first chord of a QKeySequence back to `Qt::Key | modifiers`,
// which is the shape we match against QKeyEvent fields on every keypress.
int encodeHotkey(const QString& s) {
    if (s.isEmpty()) return 0;
    const QKeySequence seq(s, QKeySequence::PortableText);
    if (seq.isEmpty()) return 0;
    return seq[0].toCombined();
}

}  // namespace

VideoWindow::VideoWindow(QWidget* parent) : QMainWindow(parent) {
    m_video = new QVideoWidget(this);
    setCentralWidget(m_video);
    setWindowTitle(QStringLiteral("Glasshouse Viewer"));
    resize(1280, 720);

    m_statusLabel  = new QLabel(QStringLiteral("(connecting)"), this);
    m_decoderLabel = new QLabel(QStringLiteral("decoder: -"),   this);
    m_latencyLabel = new QLabel(QStringLiteral("latency: -"),   this);

    auto* bar = statusBar();
    bar->addWidget(m_statusLabel, 1);
    bar->addPermanentWidget(m_decoderLabel);
    bar->addPermanentWidget(m_latencyLabel);

    // Pre-capture: mouse events arrive on the video widget; we peek at them
    // via an event filter to detect the click-to-capture trigger, but
    // otherwise leave them alone (so the window chrome still works).
    m_video->installEventFilter(this);
    m_video->setMouseTracking(true);
}

VideoWindow::~VideoWindow() = default;

QVideoSink* VideoWindow::videoSink() const {
    return m_video->videoSink();
}

void VideoWindow::setCaptureContext(const QRect& targetMonitor,
                                    const QSize& logicalDesktop,
                                    const QString& releaseHotkey,
                                    const QString& fullscreenHotkey) {
    m_targetMonitor    = targetMonitor;
    m_logicalDesktop   = logicalDesktop;
    m_releaseHotkey    = encodeHotkey(releaseHotkey);
    m_fullscreenHotkey = encodeHotkey(fullscreenHotkey);
    if (m_releaseHotkey == 0 && !releaseHotkey.isEmpty()) {
        qCWarning(lcHid) << "unparseable release_hotkey:" << releaseHotkey;
    }
    if (m_fullscreenHotkey == 0 && !fullscreenHotkey.isEmpty()) {
        qCWarning(lcHid) << "unparseable fullscreen_hotkey:" << fullscreenHotkey;
    }

    // Pre-capture path: while no widget has Qt's keyboard grab, the
    // QShortcut machinery handles the hotkey. While captured, grabKeyboard
    // bypasses QShortcut and the eventFilter check in handleKey takes
    // over — so the same hotkey value drives both routes.
    if (m_fullscreenHotkey != 0) {
        auto* sc = new QShortcut(QKeySequence(fullscreenHotkey), this);
        sc->setContext(Qt::WindowShortcut);
        connect(sc, &QShortcut::activated, this, &VideoWindow::toggleFullscreen);
    }
}

void VideoWindow::toggleFullscreen() {
    if (isFullScreen()) {
        showNormal();
        statusBar()->show();
    } else {
        statusBar()->hide();
        showFullScreen();
    }
}

void VideoWindow::setSiblings(const QList<QPointer<VideoWindow>>& siblings) {
    m_siblings = siblings;
}

ApiCoord VideoWindow::apiCoordForCursor(const QPoint& globalPos) const {
    // Prefer whichever sibling currently contains the cursor — that's
    // the window whose CoordTransform should drive the target's HID
    // for this event. Fall back to self if the cursor is outside every
    // sibling (the cursor must be over *some* window, since grabMouse
    // routed the event here, but it could be over our own window or a
    // gap; transformFor clamps cleanly in either case).
    for (const auto& sib : m_siblings) {
        if (!sib || sib.data() == this) continue;
        if (sib->frameGeometry().contains(globalPos)) {
            return sib->transformFor(globalPos);
        }
    }
    return transformFor(globalPos);
}

ApiCoord VideoWindow::transformFor(const QPoint& globalPos) const {
    // Letterbox rect needs to be in window-frame coords (same space as
    // frameGeometry). m_video->geometry() is in QMainWindow content
    // coords — i.e. relative to the central widget area, not the frame
    // — so map the video widget's top-left through globals to compute
    // its offset inside the frame. Without this, every transform is
    // off by the title-bar (and any menu/toolbar) height.
    const QPoint videoGlobalTL = m_video->mapToGlobal(QPoint(0, 0));
    const QRect  frame         = frameGeometry();
    const QRect  letterboxFrameLocal(
        videoGlobalTL.x() - frame.x(),
        videoGlobalTL.y() - frame.y(),
        m_video->width(),
        m_video->height());
    return transformToApi({
        globalPos,
        frame,
        letterboxFrameLocal,
        m_targetMonitor,
        m_logicalDesktop,
    });
}

void VideoWindow::setConnectionStatus(const QString& text) {
    m_statusLabel->setText(text);
}
void VideoWindow::setDecoderLabel(const QString& decoder) {
    m_decoderLabel->setText(QStringLiteral("decoder: %1").arg(decoder));
}
void VideoWindow::setLatencyLabel(const QString& latency) {
    m_latencyLabel->setText(QStringLiteral("latency: %1").arg(latency));
}

// ---------------------------------------------------------------------------

void VideoWindow::startCapture() {
    if (m_captured) return;
    m_captured = true;

    // Capture is session-wide: this window becomes the "holder" that
    // owns the Qt grabs, but mouse events fall through transformFor()
    // on whichever sibling the cursor is currently over. The holder is
    // just the anchor — there's no per-window capture state to enforce
    // across siblings.

    // App-wide blank cursor so the cursor stays invisible while it's
    // moved across sibling windows. Native window decorations remain
    // window-manager-drawn; that mismatch is acceptable in practice
    // because the user types into the target, not the title bar.
    QApplication::setOverrideCursor(QCursor(Qt::BlankCursor));

    // grabMouse so events keep arriving at this widget even when the
    // cursor is physically over a sibling window's video area.
    // grabKeyboard so the user can type without having to keep focus
    // pinned. Qt auto-releases prior grabs when a new widget grabs, so
    // a different window starting capture later does the right thing.
    m_video->grabMouse();
    m_video->grabKeyboard();

    qCInfo(lcHid) << "capture started";
    emit captureStateChanged(true);
}

void VideoWindow::stopCapture() {
    if (!m_captured) return;
    m_captured = false;

    // Best-effort release of any modifiers we may have left pressed on the
    // target when the user hit the hotkey mid-chord. No-op on the server
    // side if the modifier wasn't actually down.
    for (const auto& mod : {
            QStringLiteral("ShiftLeft"),  QStringLiteral("ShiftRight"),
            QStringLiteral("ControlLeft"),QStringLiteral("ControlRight"),
            QStringLiteral("AltLeft"),    QStringLiteral("AltRight"),
            QStringLiteral("MetaLeft"),   QStringLiteral("MetaRight"),
         }) {
        emit keyEvent(mod, /*pressed=*/false);
    }

    m_video->releaseKeyboard();
    m_video->releaseMouse();
    QApplication::restoreOverrideCursor();

    qCInfo(lcHid) << "capture released";
    emit captureStateChanged(false);
}

void VideoWindow::closeEvent(QCloseEvent* ev) {
    if (m_captured) stopCapture();
    QMainWindow::closeEvent(ev);
}

// ---------------------------------------------------------------------------

bool VideoWindow::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
        case QEvent::MouseButtonPress:
            return handleMouseButton(static_cast<QMouseEvent*>(event), true);
        case QEvent::MouseButtonRelease:
            return handleMouseButton(static_cast<QMouseEvent*>(event), false);
        case QEvent::MouseMove:
            return handleMouseMove(static_cast<QMouseEvent*>(event));
        case QEvent::Wheel:
            return handleWheel(static_cast<QWheelEvent*>(event));
        case QEvent::KeyPress:
            return handleKey(static_cast<QKeyEvent*>(event), true);
        case QEvent::KeyRelease:
            return handleKey(static_cast<QKeyEvent*>(event), false);
        default:
            break;
    }
    return QMainWindow::eventFilter(watched, event);
}

bool VideoWindow::handleMouseButton(QMouseEvent* ev, bool pressed) {
    // Pre-capture: clicking inside the video widget starts capture and
    // forwards the same click so the target sees a natural "click where I
    // pointed" event at that position.
    if (!m_captured) {
        if (pressed && ev->button() == Qt::LeftButton
                && m_video->rect().contains(ev->position().toPoint())) {
            startCapture();
            // fall through so the click is also forwarded below
        } else {
            return false;  // let the widget handle it (window drag etc.)
        }
    }

    const auto btn = qtToMouseButton(ev->button());
    if (!btn) return true;  // unknown button — swallow rather than confuse

    const auto api = apiCoordForCursor(ev->globalPosition().toPoint());
    emit mouseMoved(api.x, api.y);
    emit mouseButton(*btn, pressed);
    return true;
}

bool VideoWindow::handleMouseMove(QMouseEvent* ev) {
    if (!m_captured) return false;
    const auto api = apiCoordForCursor(ev->globalPosition().toPoint());
    emit mouseMoved(api.x, api.y);
    return true;
}

bool VideoWindow::handleWheel(QWheelEvent* ev) {
    if (!m_captured) return false;
    // PiKVM accepts signed-int wheel deltas and clamps internally. QWheel's
    // angleDelta is in 1/8-degrees; standard mouse notches = 120. Divide by
    // 120 so one notch becomes ±1 on the wire.
    const QPoint ad = ev->angleDelta() / 120;
    if (!ad.isNull()) emit mouseWheel(ad.x(), ad.y());
    return true;
}

bool VideoWindow::handleKey(QKeyEvent* ev, bool pressed) {
    // Fullscreen toggle: works whether captured or not. While captured,
    // grabKeyboard routes the press here and bypasses the QShortcut
    // installed in setCaptureContext, so we re-check the combo here.
    // Pressed-only — the matching release is harmless to drop.
    if (pressed && m_fullscreenHotkey != 0) {
        const int combo = int(ev->modifiers() & Qt::KeyboardModifierMask) | ev->key();
        if (combo == m_fullscreenHotkey) {
            toggleFullscreen();
            return true;
        }
    }

    if (!m_captured) return false;

    // Release hotkey on key-down: stop capture and swallow the event. The
    // matching key-release is ignored (we're no longer captured).
    if (pressed && m_releaseHotkey != 0) {
        const int combo = int(ev->modifiers() & Qt::KeyboardModifierMask) | ev->key();
        if (combo == m_releaseHotkey) {
            stopCapture();
            return true;
        }
    }

    // Auto-repeat produces a spam of press events with no intervening
    // release. The target's HID plugin autorepeats on its own end based on
    // our held-down state, so we drop Qt's synthetic repeats.
    if (ev->isAutoRepeat()) return true;

    const QString wire = keyEventToWire(*ev);
    if (wire.isEmpty()) {
        qCDebug(lcHid) << "unmapped key" << Qt::hex << ev->key()
                       << "nativeVirtualKey=" << ev->nativeVirtualKey();
        return true;
    }
    emit keyEvent(wire, pressed);
    return true;
}

}  // namespace glasshouse
