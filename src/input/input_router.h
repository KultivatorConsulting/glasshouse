#pragma once

#include "pikvmevents.h"

#include <QObject>
#include <QPointer>
#include <QString>

namespace glasshouse {

class PiKvmClient;

// Routes HID input from any captured `VideoWindow` to the configured
// HID-master `PiKvmClient`. DESIGN.md §5.3: "Keyboard events while
// captured always go to the HID master, regardless of which window is
// captured. Mouse events use the captured window's coordinate transform
// but still output via the HID master." The transform happens upstream
// in `VideoWindow`, so the router only needs to know who the master is.
//
// Phase 4 is a pure passthrough; Phase 6 (reliability) will grow this to
// detect a downed master, queue events, and recover. Keeping the
// indirection now means that growth doesn't touch every signal/slot
// connection in `main.cpp`.
class InputRouter : public QObject {
    Q_OBJECT
public:
    explicit InputRouter(PiKvmClient* hidMaster, QObject* parent = nullptr);

public slots:
    void routeMouseMove(int api_x, int api_y);
    void routeMouseButton(glasshouse::MouseButton button, bool pressed);
    void routeMouseWheel(int delta_x, int delta_y);
    void routeKey(const QString& wireName, bool pressed);

    // Used by the special-keys palette: a chord (press-in-order,
    // release-in-reverse) and server-side type-text-as-keystrokes.
    void routeShortcut(const QStringList& keys);
    void routeTypeText(const QString& text, bool slow, int delayMs);

    // Target-side ATX actions: power / power_long / reset. Always sent
    // to the HID master (which is also the PiKVM whose ATX wiring we
    // assume is connected — typically the same box for the same target).
    void routeAtxClick(const QString& button);

private:
    QPointer<PiKvmClient> m_master;
};

}  // namespace glasshouse
