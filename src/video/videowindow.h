#pragma once

#include "coord_transform.h"
#include "pikvmevents.h"

#include <QKeySequence>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QRect>
#include <QSize>

class QVideoSink;
class QVideoWidget;
class QLabel;

namespace glasshouse {

// Phase 2 + 3 viewer window:
// * QVideoWidget central widget rendering the PiKVM's target monitor.
// * Click-to-capture input: clicking inside the video area starts forwarding
//   mouse + keyboard to the owning PiKvmClient (via Qt signals). A
//   configurable release hotkey ends capture.
// * Coordinate transform happens here so main.cpp sees API-space values and
//   wire-name keys; see DESIGN.md §5.2 and §10.1 for the math and the
//   empirical verification of its numeric behaviour.
//
// True compositor-level pointer locking (Wayland pointer-constraints) is
// deferred; current Phase 3 implementation uses `QWidget::grabMouse()` and
// an app-wide `Qt::BlankCursor` override. The local cursor therefore
// "leaves" the window invisibly if the target cursor runs off-edge — the
// release hotkey always recovers.
class VideoWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit VideoWindow(QWidget* parent = nullptr);
    ~VideoWindow() override;

    // Pipeline pushes frames into this sink.
    QVideoSink* videoSink() const;

    // Configure the target-coord math and the configurable hotkeys.
    // Must be called before the user can meaningfully capture. Hotkey
    // strings are QKeySequence-parseable, e.g. "Ctrl+Alt+Shift+Escape" /
    // "F11" / "Ctrl+Alt+K". Any of the hotkey strings may be empty to
    // disable that toggle.
    void setCaptureContext(const QRect& targetMonitor,
                           const QSize& logicalDesktop,
                           const QString& releaseHotkey,
                           const QString& fullscreenHotkey,
                           const QString& specialKeysHotkey);

    // Tell this window about every other window in the session, so that
    // when capture is held here, mouse events whose cursor is over a
    // sibling can be transformed using *that* sibling's monitor rect
    // before being forwarded. Capture is session-wide: one window holds
    // the Qt grab, but the active CoordTransform follows the cursor.
    void setSiblings(const QList<QPointer<VideoWindow>>& siblings);

    bool isCaptured() const { return m_captured; }

    // Map a global cursor position into PiKVM API space using *this*
    // window's monitor rect / logical desktop / video-widget letterbox.
    // Used by the holder window to apply a sibling's transform when the
    // cursor is currently over the sibling.
    ApiCoord transformFor(const QPoint& globalPos) const;

public slots:
    void setConnectionStatus(const QString& text);
    void setDecoderLabel(const QString& decoder);
    void setLatencyLabel(const QString& latency);

    // Public so the multi-window single-capture enforcement in main.cpp
    // can release this window's capture when a sibling claims one.
    // Idempotent: no-op when not captured.
    void stopCapture();

signals:
    void captureStateChanged(bool captured);
    // Coordinates are already in PiKVM s16 API space; main.cpp forwards
    // them straight to PiKvmClient::sendMouseMove.
    void mouseMoved(int api_x, int api_y);
    void mouseButton(glasshouse::MouseButton button, bool pressed);
    void mouseWheel(int delta_x, int delta_y);
    // `wireName` is the MDN KeyboardEvent.code string (e.g. "KeyA",
    // "ShiftLeft"). Empty names are filtered out before the signal.
    void keyEvent(const QString& wireName, bool pressed);
    // Fires when the user hits the special-keys hotkey. main.cpp wires
    // this to the shared SpecialKeysDialog::toggle() slot.
    void showSpecialKeysRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* ev) override;

    // Toggles full-screen state on this window, hiding/showing the
    // status bar. Wired to the configured fullscreen hotkey both via
    // an in-app QShortcut (works pre-capture) and via handleKey
    // (works while captured, where grabKeyboard bypasses QShortcut).
    void toggleFullscreen();

private:
    void startCapture();

    // Pick the right CoordTransform for a cursor at globalPos: if the
    // cursor is currently over one of our sibling windows, use *that*
    // window's monitor + letterbox; otherwise fall back to self (clamped
    // to our own letterbox edge). This is what makes capture session-wide
    // — moving across windows continues to drive the same target HID
    // session, just routed through whichever window's transform fits.
    ApiCoord apiCoordForCursor(const QPoint& globalPos) const;

    bool handleMouseMove(QMouseEvent* ev);
    bool handleMouseButton(QMouseEvent* ev, bool pressed);
    bool handleWheel(class QWheelEvent* ev);
    bool handleKey(QKeyEvent* ev, bool pressed);

    QVideoWidget* m_video         = nullptr;
    QLabel*       m_statusLabel   = nullptr;
    QLabel*       m_decoderLabel  = nullptr;
    QLabel*       m_latencyLabel  = nullptr;

    QRect         m_targetMonitor;
    QSize         m_logicalDesktop;
    // Single-chord hotkeys, encoded as (Qt::Key | modifiers). 0 = unset.
    int           m_releaseHotkey      = 0;
    int           m_fullscreenHotkey   = 0;
    int           m_specialKeysHotkey  = 0;

    bool          m_captured         = false;

    // Other VideoWindows in this session. QPointer because windows can
    // be closed independently; iteration tolerates a stale entry.
    QList<QPointer<VideoWindow>> m_siblings;
};

}  // namespace glasshouse
