#pragma once

#include "coord_transform.h"
#include "pikvmevents.h"
#include "pointerconfiner.h"

#include <QCursor>
#include <QElapsedTimer>
#include <QImage>
#include <QKeySequence>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QRect>
#include <QSize>

class QVideoSink;
class QVideoWidget;
class QLabel;
class QStackedWidget;

namespace glasshouse {

class RegionSelector;

// The configurable single-chord hotkeys. Grouped rather than passed as
// five adjacent QString parameters, which is a swap waiting to happen —
// two transposed hotkeys would bind silently and misbehave later. Each
// string is QKeySequence-parseable ("Ctrl+Alt+Shift+Backspace", "F11",
// "Ctrl+Alt+K"); any may be empty to disable that hotkey.
struct CaptureHotkeys {
    QString release;
    QString fullscreen;
    QString specialKeys;
    QString paste;
    QString region;
};

// Phase 2 + 3 viewer window:
// * QVideoWidget central widget rendering the PiKVM's target monitor.
// * Click-to-capture input: clicking inside the video area starts forwarding
//   mouse + keyboard to the owning PiKvmClient (via Qt signals). A
//   configurable release hotkey ends capture.
// * Coordinate transform happens here so main.cpp sees API-space values and
//   wire-name keys; see DESIGN.md §5.2 and §10.1 for the math and the
//   empirical verification of its numeric behaviour.
//
// While captured, the pointer is confined to the bounding rectangle of the
// session windows using X11 XFixes pointer barriers (PointerConfiner):
// server-side, no grab and no cursor warping, so it cannot stall the event
// loop the way QCursor::setPos did. `grabMouse` still routes events to the
// holder even past a window edge; `grabKeyboard` pins key input; the release
// hotkey always recovers. (Barriers are X11-only; on Wayland confinement is
// a no-op and would need the compositor pointer-constraints protocol.)
class VideoWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit VideoWindow(QWidget* parent = nullptr);
    ~VideoWindow() override;

    // Pipeline pushes frames into this sink.
    QVideoSink* videoSink() const;

    // Configure the target-coord math and the configurable hotkeys.
    // Must be called before the user can meaningfully capture.
    void setCaptureContext(const QRect& targetMonitor,
                           const QSize& logicalDesktop,
                           const CaptureHotkeys& hotkeys);

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

    // Used as the QSettings key root for geometry persistence (one slot
    // per PiKVM host so multiple windows don't trample each other).
    void setPersistenceHost(const QString& host) { m_persistHost = host; }

    // Capture cursor marker: a local, zero-lag pointer drawn during capture
    // that the guest cursor converges to. style = "ring" | "crosshair" |
    // "hidden"; color = "#RRGGBB"; size = ring outer diameter (px).
    void setCursorMarker(const QString& style, const QString& color, int size);

    // Marks a session-wide capture as active/inactive on *this* window.
    // The holder sets it on every session window (including itself) when
    // capture starts and stops, so non-holder windows know to act on the
    // input Qt delivers to them directly — see handleWheel.
    void setSessionCapture(bool active);

    // Marks this window as the one the cursor is currently over during a
    // session capture, so the status badge can follow the pointer.
    void setCaptureFocus(bool focused);

public slots:
    void setConnectionStatus(const QString& text);
    void setDecoderLabel(const QString& decoder);
    void setLatencyLabel(const QString& latency);
    // Live steady-state readout (effective render FPS + GUI-thread coalesced
    // drops), updated on a poll timer by main.cpp. Distinct from the latency
    // label, which holds the one-shot auth-to-first-frame time.
    void setVideoStats(const QString& stats);
    // Capture resolution readout, plus any mismatch-against-config warning.
    // Written only by main.cpp, which is the only place that can compare
    // every window's actual resolution against the configured layout.
    void setResolutionLabel(const QString& text);
    // Transient status-bar message (Qt's temporary message area, so it
    // leaves the permanent readouts alone). Used for paste feedback.
    void flashStatus(const QString& text, int ms = 4000);

    // Long-running work indicator. Text extraction is a network round
    // trip to a model and can take many seconds, so a static line in the
    // status bar isn't enough — there's no way to tell it apart from a
    // hang. Shows an animated spinner with an elapsed-seconds count, and
    // puts a busy cursor under the pointer, which is where the user is
    // actually looking. Must be paired with clearBusy() on every exit
    // path, or the override cursor outlives the work.
    void setBusy(const QString& text);
    void clearBusy();
    bool isBusy() const { return m_busy; }

    // Public so the multi-window single-capture enforcement in main.cpp
    // can release this window's capture when a sibling claims one.
    // Idempotent: no-op when not captured.
    void stopCapture();

signals:
    // `sessionActive` is true while any window in the session holds capture;
    // `focused` is true only on the window the cursor is currently over. The
    // pair lets main.cpp show the capture badge on the window being worked
    // in rather than on whichever one happened to start the capture.
    void captureStatusChanged(bool sessionActive, bool focused);
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
    // Emitted from the Target menu after the user confirms in a
    // QMessageBox. main.cpp routes through InputRouter to the HID master.
    // `button` ∈ {power, power_long, reset}.
    void atxClickRequested(const QString& button);
    // Target → Mass Storage… menu item; main.cpp toggles the shared
    // MsdDialog.
    void showMsdRequested();
    // The paste hotkey fired. main.cpp owns the clipboard read, since
    // only it can reach the HID master. Any held modifiers have already
    // been released on the target by the time this is emitted.
    void pasteClipboardRequested();
    // A region of the frozen video frame, cropped and in frame pixels.
    // main.cpp owns what happens next (encode, send to Tui, clipboard).
    // The crop the user dragged, plus where it came from in the frame's
    // own pixels. Both, because the extraction providers want different
    // things: a model or a host tool reads the image, while the PiKVM's
    // own OCR takes a rectangle and reads its own live frame.
    void regionCaptured(const QImage& image, const QRect& frameRect);
    // The user asked to stop the long-running work this window is
    // showing a spinner for (by pressing the hotkey again).
    void busyCancelRequested();

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
    void flushMousePending();
    void stepPulse();          // advances the click-pulse cursor animation
    void buildMenuBar();

    // Pick the right CoordTransform for a cursor at globalPos: if the
    // cursor is currently over one of our sibling windows, use *that*
    // window's monitor + letterbox; otherwise fall back to self (clamped
    // to our own letterbox edge). This is what makes capture session-wide
    // — moving across windows continues to drive the same target HID
    // session, just routed through whichever window's transform fits.
    ApiCoord apiCoordForCursor(const QPoint& globalPos) const;

    // The session window whose frame contains globalPos, or self when the
    // cursor is over none of them (a gap, or our own window).
    VideoWindow* windowForCursor(const QPoint& globalPos) const;

    // Holder-side: move the capture badge to `w`, clearing it everywhere
    // else. Cheap no-op when the focused window hasn't changed.
    void updateCaptureFocus(VideoWindow* w);

    // Every window in the session, including self. m_siblings is populated
    // that way by main.cpp; falls back to just self if it's empty.
    QList<VideoWindow*> sessionWindows() const;

    // frameGeometry() of every session window (self + siblings) in global
    // coords — the region the pointer is confined to during capture.
    QList<QRect> sessionWindowRects() const;

    // Send key-up for every modifier wire name, whether or not we think
    // it is down — the target has no way to tell us, and a stuck Ctrl is
    // worse than a redundant release (the server no-ops the latter).
    void releaseHeldModifiers();

    // The single entry point for the paste hotkey. Two routes reach it —
    // the QShortcut while uncaptured, handleKey while the keyboard grab
    // is active — and which one wins is not something we should depend
    // on, so the modifier release lives here rather than in either.
    void requestClipboardPaste();

    // Region capture. The hotkey arrives at whichever window holds the
    // grabs, but the user means the window their pointer is over, so the
    // request is forwarded there before anything is shown.
    void requestRegionCapture();
    bool beginRegionSelect();
    void endRegionSelect();

    // Pause/restore just the Qt grabs, leaving m_captured, the pointer
    // confiner and the capture badge alone. Region selection needs the
    // drag and Esc to reach the selector widget, which can't happen while
    // the video widget holds grabMouse/grabKeyboard. Keeping the confiner
    // up means the pointer still can't stray off the session windows.
    void suspendCaptureGrabs();
    void resumeCaptureGrabs();

    // The session window that currently owns the Qt grabs, or nullptr.
    VideoWindow* captureHolder() const;

    bool handleMouseMove(QMouseEvent* ev);
    bool handleMouseButton(QMouseEvent* ev, bool pressed);
    bool handleWheel(class QWheelEvent* ev);
    bool handleKey(QKeyEvent* ev, bool pressed);

    QStackedWidget*  m_stack      = nullptr;   // video <-> region selector
    QVideoWidget* m_video         = nullptr;
    RegionSelector* m_selector    = nullptr;
    QLabel*       m_statusLabel   = nullptr;
    QLabel*       m_decoderLabel  = nullptr;
    QLabel*       m_latencyLabel  = nullptr;
    QLabel*       m_statsLabel    = nullptr;
    QLabel*       m_resLabel      = nullptr;
    QLabel*       m_busyLabel     = nullptr;

    QString       m_persistHost;
    QRect         m_targetMonitor;
    QSize         m_logicalDesktop;
    // Single-chord hotkeys, encoded as (Qt::Key | modifiers). 0 = unset.
    int           m_releaseHotkey      = 0;
    int           m_fullscreenHotkey   = 0;
    int           m_specialKeysHotkey  = 0;
    int           m_pasteHotkey        = 0;
    int           m_regionHotkey       = 0;

    // m_captured: this window owns the Qt grabs (it is the holder).
    // m_sessionCaptured: *some* window in the session is the holder — true
    // on the holder too. Qt delivers wheel events without consulting the
    // mouse grabber (unlike moves/buttons), so a non-holder window has to
    // act on the wheel events it receives directly; see handleWheel.
    bool          m_captured         = false;
    bool          m_sessionCaptured  = false;
    bool          m_captureFocus     = false;
    // True while this window is showing the region selector in place of
    // its video. Input must not reach the target during that time.
    bool          m_selecting        = false;
    // The still being selected from, kept at full frame resolution so
    // the crop comes out of the original pixels rather than anything the
    // selector scaled for display.
    QImage        m_selectorImage;

    // Busy spinner state (see setBusy).
    bool          m_busy       = false;
    int           m_busyFrame  = 0;
    QString       m_busyText;
    QElapsedTimer m_busyClock;
    class QTimer* m_busyTimer  = nullptr;

    // Holder-side bookkeeping for updateCaptureFocus.
    QPointer<VideoWindow> m_focusWindow;

    // Confines the pointer to the session windows while captured (X11).
    PointerConfiner m_confiner;

    // Capture cursor marker + click-pulse animation (see setCursorMarker).
    QString        m_markerStyle = QStringLiteral("ring");
    QCursor        m_markerCursor;          // base marker shown during capture
    QList<QCursor> m_pulseCursors;          // click-pulse animation frames
    class QTimer*  m_pulseTimer  = nullptr;
    int            m_pulseStep   = 0;

    // Other VideoWindows in this session. QPointer because windows can
    // be closed independently; iteration tolerates a stale entry.
    QList<QPointer<VideoWindow>> m_siblings;

    // Mouse-move coalescer. Wayland delivers mouse moves at up to 1 kHz;
    // kvmd's USB HID gadget can drain ~125–500 Hz. Sending every event
    // queues frames on the server side and surfaces as cursor lag. We
    // hold the latest API-space position and flush it through a timer
    // tick at ~120 Hz instead. Button presses force-flush to keep the
    // "move-then-click" order tight.
    class QTimer* m_mouseFlushTimer = nullptr;
    bool          m_mousePending    = false;
    int           m_pendingMouseX   = 0;
    int           m_pendingMouseY   = 0;

    // Wheel-event accumulator. High-resolution scroll wheels (and
    // Wayland) deliver angleDelta in sub-notch increments (e.g. ±16 per
    // event); 120 1/8-degrees == one mouse "notch" and PiKVM expects
    // ±1 per notch on the wire. Without an accumulator each sub-notch
    // event rounds to 0 and we drop the scroll entirely.
    QPoint        m_wheelAccum      = {0, 0};
};

}  // namespace glasshouse
