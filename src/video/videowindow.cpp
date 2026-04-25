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
                                    const QString& releaseHotkey) {
    m_targetMonitor   = targetMonitor;
    m_logicalDesktop  = logicalDesktop;
    m_releaseHotkey   = encodeHotkey(releaseHotkey);
    if (m_releaseHotkey == 0 && !releaseHotkey.isEmpty()) {
        qCWarning(lcHid) << "unparseable release_hotkey:" << releaseHotkey;
    }
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

    // App-wide blank cursor. setOverrideCursor also hides the cursor when it
    // briefly leaves the window (e.g. we never installed a compositor pointer
    // constraint). restoreOverrideCursor undoes it on stopCapture.
    QApplication::setOverrideCursor(QCursor(Qt::BlankCursor));

    // Route all mouse events to the video widget even when the cursor is
    // physically outside it; route keyboard there too.
    m_video->grabMouse();
    m_video->grabKeyboard();

    // Catch keyboard globally — otherwise modifiers pressed while focus is
    // on the status bar (or nowhere) get missed.
    QApplication::instance()->installEventFilter(this);

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

    QApplication::instance()->removeEventFilter(this);
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

    // Forward the cursor position that accompanies this click, so the
    // target cursor is at the click point before the button event fires.
    const auto api = transformToApi({
        ev->globalPosition().toPoint(),
        frameGeometry(),
        m_video->geometry(),
        m_targetMonitor,
        m_logicalDesktop,
    });
    emit mouseMoved(api.x, api.y);
    emit mouseButton(*btn, pressed);
    return true;
}

bool VideoWindow::handleMouseMove(QMouseEvent* ev) {
    if (!m_captured) return false;
    const auto api = transformToApi({
        ev->globalPosition().toPoint(),
        frameGeometry(),
        m_video->geometry(),
        m_targetMonitor,
        m_logicalDesktop,
    });
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
