#pragma once

#include <QString>

namespace glasshouse {

// PiKVM wire names for mouse buttons. The `Back` / `Forward` entries are the
// 4th/5th buttons (browser navigation) — on the PiKVM wire they are called
// `up` / `down` respectively (not wheel directions).
enum class MouseButton {
    Left,
    Middle,
    Right,
    Back,      // -> "up"
    Forward,   // -> "down"
};

QString mouseButtonToWire(MouseButton b);

// Mirror of the payload in the `hid` state event (identical shape to the REST
// `GET /api/hid` response's `result`).
struct HidState {
    bool    online            = false;
    bool    mouse_online      = false;
    bool    mouse_absolute    = false;
    bool    keyboard_online   = false;
    QString mouse_outputs_active;  // expected `usb` for the HID master
};

}  // namespace glasshouse
