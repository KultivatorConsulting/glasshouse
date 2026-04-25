#pragma once

#include "pikvmevents.h"

#include <QKeySequence>
#include <QMainWindow>
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

    // Configure the target-coord math and the release hotkey. Must be
    // called before the user can meaningfully capture. `releaseHotkey` is
    // a QKeySequence-parseable string, e.g. "Ctrl+Alt+Shift+Escape".
    void setCaptureContext(const QRect& targetMonitor,
                           const QSize& logicalDesktop,
                           const QString& releaseHotkey);

    bool isCaptured() const { return m_captured; }

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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* ev) override;

private:
    void startCapture();

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
    // Single-chord hotkey, encoded as (Qt::Key | modifiers). 0 = unset.
    int           m_releaseHotkey = 0;

    bool          m_captured      = false;
};

}  // namespace glasshouse
