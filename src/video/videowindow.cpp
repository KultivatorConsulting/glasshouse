#include "videowindow.h"
#include "coord_transform.h"
#include "keymap.h"
#include "logging.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QColor>
#include <QCursor>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QSettings>
#include <QShortcut>
#include <QStatusBar>
#include <QTimer>
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

// Build a ring-marker pixmap: a haloed coloured ring with a centre dot. The
// dark halo keeps it visible on any background. `pulseT` in [0,1] adds an
// expanding, fading outer ring for the click ripple; pulseT < 0 draws no
// pulse. The canvas leaves room around the ring for the pulse to grow into,
// and the hotspot is the canvas centre so the marker point never shifts.
QPixmap drawMarkerPixmap(int size, const QColor& color, double pulseT) {
    const int ring   = qBound(10, size, 64);
    const int canvas = ring * 2 + 8;
    QPixmap pm(canvas, canvas);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPointF ctr(canvas / 2.0, canvas / 2.0);
    auto stroke = [&](double dia, const QColor& col, double w) {
        QPen pen(col); pen.setWidthF(w);
        p.setPen(pen); p.setBrush(Qt::NoBrush);
        p.drawEllipse(ctr, dia / 2.0, dia / 2.0);
    };
    stroke(ring + 1.0, QColor(0, 0, 0, 150), 3.0);   // dark halo
    stroke(ring,       color,                2.0);   // coloured ring
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 150)); p.drawEllipse(ctr, 2.4, 2.4);
    p.setBrush(color);                p.drawEllipse(ctr, 1.4, 1.4);  // centre dot
    if (pulseT >= 0.0) {
        const double dia = ring + ring * 0.9 * pulseT;
        QColor pc = color; pc.setAlpha(int(170.0 * (1.0 - pulseT)));
        stroke(dia, pc, 2.5);
    }
    p.end();
    return pm;
}
QCursor cursorFromPixmap(const QPixmap& pm) {
    return QCursor(pm, pm.width() / 2, pm.height() / 2);  // hotspot = centre
}

}  // namespace

VideoWindow::VideoWindow(QWidget* parent) : QMainWindow(parent) {
    m_video = new QVideoWidget(this);
    // Explicit even though it's the default — the CoordTransform letterbox
    // math in transformFor assumes Qt::KeepAspectRatio. If this ever
    // changes (e.g. someone wants stretched fill), the math has to be
    // updated alongside.
    m_video->setAspectRatioMode(Qt::KeepAspectRatio);
    setCentralWidget(m_video);
    setWindowTitle(QStringLiteral("Glasshouse Viewer"));
    resize(1280, 720);

    m_statusLabel  = new QLabel(QStringLiteral("(connecting)"), this);
    m_decoderLabel = new QLabel(QStringLiteral("decoder: -"),   this);
    m_latencyLabel = new QLabel(QStringLiteral("latency: -"),   this);
    m_statsLabel   = new QLabel(QStringLiteral("video: -"),     this);

    auto* bar = statusBar();
    bar->addWidget(m_statusLabel, 1);
    bar->addPermanentWidget(m_decoderLabel);
    bar->addPermanentWidget(m_latencyLabel);
    bar->addPermanentWidget(m_statsLabel);

    // Pre-capture: mouse events arrive on the video widget; we peek at them
    // via an event filter to detect the click-to-capture trigger, but
    // otherwise leave them alone (so the window chrome still works).
    m_video->installEventFilter(this);
    m_video->setMouseTracking(true);

    // ~120 Hz coalescer for mouse moves. Always running — handleMouseMove
    // sets `m_mousePending` only while captured, so the timer is a no-op
    // outside capture.
    m_mouseFlushTimer = new QTimer(this);
    m_mouseFlushTimer->setInterval(8);  // 8 ms ≈ 120 Hz
    connect(m_mouseFlushTimer, &QTimer::timeout, this,
            &VideoWindow::flushMousePending);
    m_mouseFlushTimer->start();

    // Click-pulse animation timer (drives stepPulse). A default ring marker is
    // built now; main.cpp overrides it via setCursorMarker() once config loads.
    m_pulseTimer = new QTimer(this);
    m_pulseTimer->setInterval(30);  // ~5 frames ≈ 150 ms ripple
    connect(m_pulseTimer, &QTimer::timeout, this, &VideoWindow::stepPulse);
    setCursorMarker(QStringLiteral("ring"), QStringLiteral("#FFA000"), 24);

    buildMenuBar();
}

void VideoWindow::buildMenuBar() {
    // ---- File ----
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    auto* quitAct = fileMenu->addAction(QStringLiteral("&Quit"));
    quitAct->setShortcut(QKeySequence::Quit);  // Ctrl+Q on Linux
    connect(quitAct, &QAction::triggered, qApp, &QCoreApplication::quit);

    // ---- View ----
    auto* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    auto* fsAct = viewMenu->addAction(QStringLiteral("Toggle &Fullscreen"));
    // Shortcut text on the menu item is informational — actual toggling
    // also happens via the QShortcut / eventFilter set up in
    // setCaptureContext, so users can hit F11 from any focus state.
    fsAct->setShortcut(QKeySequence(QStringLiteral("F11")));
    connect(fsAct, &QAction::triggered, this, &VideoWindow::toggleFullscreen);
    auto* skAct = viewMenu->addAction(QStringLiteral("Show Special &Keys…"));
    skAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+K")));
    connect(skAct, &QAction::triggered,
            this, &VideoWindow::showSpecialKeysRequested);

    // ---- Target (ATX) ----
    // Each is gated by a confirmation dialog so a stray menu click
    // doesn't reboot a production target.
    auto* targetMenu = menuBar()->addMenu(QStringLiteral("&Target"));
    auto confirmAndEmit = [this](const QString& question, const QString& btn) {
        if (QMessageBox::question(
                this, QStringLiteral("Confirm"), question,
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) == QMessageBox::Yes) {
            emit atxClickRequested(btn);
        }
    };
    auto* powerAct = targetMenu->addAction(QStringLiteral("&Power (short press)"));
    connect(powerAct, &QAction::triggered, this, [confirmAndEmit]() {
        confirmAndEmit(QStringLiteral("Send a short power-button press to the target?"),
                       QStringLiteral("power"));
    });
    auto* powerLongAct = targetMenu->addAction(
        QStringLiteral("Power (&long press — force off)"));
    connect(powerLongAct, &QAction::triggered, this, [confirmAndEmit]() {
        confirmAndEmit(QStringLiteral("Force the target off (long power-button press)?"),
                       QStringLiteral("power_long"));
    });
    auto* resetAct = targetMenu->addAction(QStringLiteral("&Reset"));
    connect(resetAct, &QAction::triggered, this, [confirmAndEmit]() {
        confirmAndEmit(QStringLiteral("Hardware-reset the target?"),
                       QStringLiteral("reset"));
    });

    targetMenu->addSeparator();
    auto* msdAct = targetMenu->addAction(QStringLiteral("&Mass Storage…"));
    connect(msdAct, &QAction::triggered, this, &VideoWindow::showMsdRequested);

    // ---- Help ----
    auto* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    auto* aboutAct = helpMenu->addAction(QStringLiteral("&About Glasshouse Viewer"));
    connect(aboutAct, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, QStringLiteral("About Glasshouse Viewer"),
            QStringLiteral(
                "<b>Glasshouse Viewer</b> %1<br><br>"
                "Native Linux PiKVM client.<br><br>"
                "<a href=\"https://github.com/kultivator-consulting/glasshouse\">"
                "github.com/kultivator-consulting/glasshouse</a>")
                .arg(QApplication::applicationVersion()));
    });
}

VideoWindow::~VideoWindow() = default;

QVideoSink* VideoWindow::videoSink() const {
    return m_video->videoSink();
}

void VideoWindow::setCaptureContext(const QRect& targetMonitor,
                                    const QSize& logicalDesktop,
                                    const QString& releaseHotkey,
                                    const QString& fullscreenHotkey,
                                    const QString& specialKeysHotkey) {
    m_targetMonitor     = targetMonitor;
    m_logicalDesktop    = logicalDesktop;
    m_releaseHotkey     = encodeHotkey(releaseHotkey);
    m_fullscreenHotkey  = encodeHotkey(fullscreenHotkey);
    m_specialKeysHotkey = encodeHotkey(specialKeysHotkey);
    if (m_releaseHotkey == 0 && !releaseHotkey.isEmpty()) {
        qCWarning(lcHid) << "unparseable release_hotkey:" << releaseHotkey;
    }
    if (m_fullscreenHotkey == 0 && !fullscreenHotkey.isEmpty()) {
        qCWarning(lcHid) << "unparseable fullscreen_hotkey:" << fullscreenHotkey;
    }
    if (m_specialKeysHotkey == 0 && !specialKeysHotkey.isEmpty()) {
        qCWarning(lcHid) << "unparseable special_keys_hotkey:" << specialKeysHotkey;
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
    if (m_specialKeysHotkey != 0) {
        auto* sc = new QShortcut(QKeySequence(specialKeysHotkey), this);
        sc->setContext(Qt::WindowShortcut);
        connect(sc, &QShortcut::activated,
                this, &VideoWindow::showSpecialKeysRequested);
    }
}

void VideoWindow::toggleFullscreen() {
    if (isFullScreen()) {
        showNormal();
        statusBar()->show();
        menuBar()->show();
    } else {
        statusBar()->hide();
        menuBar()->hide();
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
    // Where the video actually paints inside the QVideoWidget — accounts
    // for letterbox / pillarbox bars when the widget AR doesn't match
    // the source AR. Falls back to the full widget rect before the first
    // frame arrives (videoSize is invalid).
    const QSize videoSize     = m_video->videoSink()
                                    ? m_video->videoSink()->videoSize()
                                    : QSize();
    const QRect inWidget      = computeLetterbox(m_video->size(), videoSize);

    // Letterbox rect needs to be in window-frame coords (same space as
    // frameGeometry). m_video->geometry() is in QMainWindow content
    // coords — relative to the central-widget area, not the frame — so
    // map the video widget's top-left through globals to compute its
    // offset inside the frame, then add the rendered-rect offset inside
    // the widget on top.
    const QPoint widgetGlobalTL = m_video->mapToGlobal(QPoint(0, 0));
    const QRect  frame          = frameGeometry();
    const QPoint widgetFrameTL  = widgetGlobalTL - frame.topLeft();
    const QRect  letterboxFrameLocal(widgetFrameTL + inWidget.topLeft(),
                                     inWidget.size());

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
void VideoWindow::setVideoStats(const QString& stats) {
    m_statsLabel->setText(QStringLiteral("video: %1").arg(stats));
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

    // Local cursor marker: instead of hiding the pointer, show a zero-lag
    // marker (a ring by default) at the true pointer position. Because PiKVM
    // is absolute-positioned and the video is a 1:1 letterboxed view, that's
    // exactly where the guest cursor lands — the marker leads, the laggy guest
    // cursor trails and converges, masking perceived mouse latency.
    // Style/colour/size come from setCursorMarker(). (Assumes absolute mode.)
    QApplication::setOverrideCursor(m_markerCursor);

    // grabMouse so events keep arriving at this widget even when the
    // cursor is physically over a sibling window's video area.
    // grabKeyboard so the user can type without having to keep focus
    // pinned. Qt auto-releases prior grabs when a new widget grabs, so
    // a different window starting capture later does the right thing.
    m_video->grabMouse();
    m_video->grabKeyboard();

    // Pen the pointer inside the session windows so it can't drift onto the
    // local desktop / another monitor and misroute input. Server-side X11
    // barriers — no warp, no grab feedback, so it can't stall the event loop.
    m_confiner.confineToBounds(sessionWindowRects());

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
    m_confiner.release();
    if (m_pulseTimer) m_pulseTimer->stop();
    m_pulseStep = 0;
    QApplication::restoreOverrideCursor();

    qCInfo(lcHid) << "capture released";
    emit captureStateChanged(false);
}

void VideoWindow::closeEvent(QCloseEvent* ev) {
    if (m_captured) stopCapture();
    if (!m_persistHost.isEmpty()) {
        // saveGeometry packages position, size, and the maximized /
        // fullscreen flag in a single QByteArray; restoreGeometry
        // unpacks all three on next launch. Per-host key avoids two
        // PiKVM windows clobbering each other's slots.
        QSettings s;
        s.setValue(QStringLiteral("windows/%1/geometry").arg(m_persistHost),
                   saveGeometry());
    }
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

    // Force the coalescer to flush at the click point so the target sees
    // the cursor at the click coords *before* the button event arrives.
    const auto api = apiCoordForCursor(ev->globalPosition().toPoint());
    m_pendingMouseX = api.x;
    m_pendingMouseY = api.y;
    m_mousePending  = true;
    flushMousePending();

    // Click ripple: pulse the marker on press (ring style only).
    if (pressed && !m_pulseCursors.isEmpty()) {
        m_pulseStep = 0;
        if (!m_pulseTimer->isActive()) m_pulseTimer->start();
    }
    emit mouseButton(*btn, pressed);
    return true;
}

QList<QRect> VideoWindow::sessionWindowRects() const {
    QList<QRect> rects;
    for (const auto& sib : m_siblings) {
        if (sib) rects.append(sib->frameGeometry());
    }
    if (rects.isEmpty()) rects.append(frameGeometry());
    return rects;
}

bool VideoWindow::handleMouseMove(QMouseEvent* ev) {
    if (!m_captured) return false;
    const auto api  = apiCoordForCursor(ev->globalPosition().toPoint());
    m_pendingMouseX = api.x;
    m_pendingMouseY = api.y;
    m_mousePending  = true;
    return true;
}

void VideoWindow::flushMousePending() {
    if (!m_mousePending) return;
    m_mousePending = false;
    emit mouseMoved(m_pendingMouseX, m_pendingMouseY);
}

void VideoWindow::setCursorMarker(const QString& style, const QString& color,
                                  int size) {
    m_markerStyle = style.trimmed().toLower();
    m_pulseCursors.clear();
    if (m_markerStyle == QLatin1String("hidden")) {
        m_markerCursor = QCursor(Qt::BlankCursor);
    } else if (m_markerStyle == QLatin1String("crosshair")) {
        m_markerCursor = QCursor(Qt::CrossCursor);
    } else {
        m_markerStyle = QStringLiteral("ring");
        QColor c(color);
        if (!c.isValid()) c = QColor(QStringLiteral("#FFA000"));
        m_markerCursor = cursorFromPixmap(drawMarkerPixmap(size, c, -1.0));
        for (double t : {0.0, 0.22, 0.45, 0.7, 1.0})
            m_pulseCursors.append(cursorFromPixmap(drawMarkerPixmap(size, c, t)));
    }
    // Apply immediately if a capture is already in progress.
    if (m_captured) QApplication::changeOverrideCursor(m_markerCursor);
}

void VideoWindow::stepPulse() {
    if (!m_captured || m_pulseStep >= m_pulseCursors.size()) {
        m_pulseTimer->stop();
        if (m_captured) QApplication::changeOverrideCursor(m_markerCursor);
        m_pulseStep = 0;
        return;
    }
    QApplication::changeOverrideCursor(m_pulseCursors[m_pulseStep]);
    ++m_pulseStep;
}

bool VideoWindow::handleWheel(QWheelEvent* ev) {
    if (!m_captured) return false;
    // PiKVM accepts signed-int wheel deltas and clamps internally. QWheel's
    // angleDelta is in 1/8-degrees; one notch = 120. Stepped mice deliver
    // ±120 per notch in a single event, but high-resolution wheels and
    // Wayland touchpad scrolls deliver many small sub-notch events
    // (e.g. ±16 each). Accumulate so sub-notch deltas combine into
    // whole-notch ±1 values on the wire.
    m_wheelAccum += ev->angleDelta();
    const int notchX = m_wheelAccum.x() / 120;
    const int notchY = m_wheelAccum.y() / 120;
    if (notchX != 0 || notchY != 0) {
        m_wheelAccum -= QPoint(notchX * 120, notchY * 120);
        emit mouseWheel(notchX, notchY);
    }
    return true;
}

bool VideoWindow::handleKey(QKeyEvent* ev, bool pressed) {
    // App-side hotkeys (fullscreen / special-keys palette): work
    // whether captured or not. While captured, grabKeyboard routes the
    // press here and bypasses the QShortcut installed in
    // setCaptureContext, so we re-check the combo here. Pressed-only —
    // the matching release is harmless to drop.
    if (pressed) {
        const int combo = int(ev->modifiers() & Qt::KeyboardModifierMask) | ev->key();
        if (m_fullscreenHotkey != 0 && combo == m_fullscreenHotkey) {
            toggleFullscreen();
            return true;
        }
        if (m_specialKeysHotkey != 0 && combo == m_specialKeysHotkey) {
            emit showSpecialKeysRequested();
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
