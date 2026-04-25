#pragma once

#include <QString>

class QKeyEvent;

namespace glasshouse {

// Translate a Qt key event into PiKVM's wire key name.
//
// The PiKVM wire protocol uses MDN `KeyboardEvent.code` values — physical,
// position-based identifiers ("KeyA" for the physical A key, "Digit1",
// "ShiftLeft", "F1", etc.). See kvmd/keyboard/mappings.py → `WEB_TO_EVDEV`.
//
// Qt::Key is layout-aware and, for modifiers, collapses L/R pairs
// (`Qt::Key_Shift`). For non-modifier keys the Qt enum → MDN name is a
// static lookup; for Shift/Control/Alt/Meta we read
// `QKeyEvent::nativeVirtualKey()` — which on Wayland/X11 carries the
// XKB keysym — so we can emit the correct `…Left` / `…Right` suffix.
//
// Returns an empty string for keys we don't handle (IME composition,
// dead keys, multimedia keys we haven't mapped). Callers should log and
// drop — the server silently discards unknown names anyway.
QString keyEventToWire(const QKeyEvent& ev);

}  // namespace glasshouse
