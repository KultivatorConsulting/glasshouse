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

// Fold clipboard text down to what PiKVM's `POST /api/hid/print` can
// actually type.
//
// kvmd types the request body through its server-side keymap (`en_US`
// by default) and silently discards every character the keymap can't
// reach. Text copied out of a browser is full of curly quotes and
// en-dashes, so without this a paste loses characters mid-word with no
// error reported anywhere. We fold the common Unicode punctuation to its
// ASCII equivalent, normalise CRLF (a lone CR types a second Enter), drop
// zero-width filler, and discard whatever is still unreachable.
//
// `droppedOut`, when non-null, receives the number of characters that
// were discarded, so the caller can tell the user what didn't make it.
QString sanitiseForPrint(const QString& in, int* droppedOut = nullptr);

}  // namespace glasshouse
