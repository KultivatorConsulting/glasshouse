#include "keymap.h"

#include <QKeyEvent>

namespace glasshouse {

namespace {

// XKB keysym values (= X11 KeySym = xkbcommon XKB_KEY_*). Linux-only; we
// target Kubuntu per DESIGN.md, so Wayland/X11 nativeVirtualKey() is the
// only input we need to decode. Inlined here so we don't drag libxkbcommon
// into the build just for a handful of constants.
constexpr quint32 XKB_Shift_L    = 0xffe1;
constexpr quint32 XKB_Shift_R    = 0xffe2;
constexpr quint32 XKB_Control_L  = 0xffe3;
constexpr quint32 XKB_Control_R  = 0xffe4;
constexpr quint32 XKB_Meta_L     = 0xffe7;
constexpr quint32 XKB_Meta_R     = 0xffe8;
constexpr quint32 XKB_Alt_L      = 0xffe9;
constexpr quint32 XKB_Alt_R      = 0xffea;
constexpr quint32 XKB_Super_L    = 0xffeb;
constexpr quint32 XKB_Super_R    = 0xffec;

// Modifiers are distinguished via the XKB keysym because Qt::Key collapses
// Shift/Control/Alt/Meta L/R pairs into a single enum.
QString modifierFromNative(quint32 keysym) {
    switch (keysym) {
        case XKB_Shift_L:   return QStringLiteral("ShiftLeft");
        case XKB_Shift_R:   return QStringLiteral("ShiftRight");
        case XKB_Control_L: return QStringLiteral("ControlLeft");
        case XKB_Control_R: return QStringLiteral("ControlRight");
        case XKB_Alt_L:     return QStringLiteral("AltLeft");
        case XKB_Alt_R:     return QStringLiteral("AltRight");
        case XKB_Super_L:
        case XKB_Meta_L:    return QStringLiteral("MetaLeft");
        case XKB_Super_R:
        case XKB_Meta_R:    return QStringLiteral("MetaRight");
    }
    return {};
}

}  // namespace

QString keyEventToWire(const QKeyEvent& ev) {
    const int key = ev.key();

    // Letters and digits — Qt::Key_A..Z and Qt::Key_0..9 are contiguous.
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return QStringLiteral("Key%1").arg(QChar(QLatin1Char('A' + (key - Qt::Key_A))));
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return QStringLiteral("Digit%1").arg(key - Qt::Key_0);
    }

    // Function keys F1..F24 — also contiguous.
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        return QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
    }

    switch (key) {
        // Modifier keys — disambiguate L/R via the XKB keysym.
        case Qt::Key_Shift:
        case Qt::Key_Control:
        case Qt::Key_Alt:
        case Qt::Key_AltGr:
        case Qt::Key_Meta:
        case Qt::Key_Super_L:
        case Qt::Key_Super_R: {
            const QString via = modifierFromNative(ev.nativeVirtualKey());
            if (!via.isEmpty()) return via;
            // Fallback when nativeVirtualKey is unavailable (some embed
            // paths). Default to the left variant — on the target, either
            // side counts as the same modifier for standard bindings.
            switch (key) {
                case Qt::Key_Shift:    return QStringLiteral("ShiftLeft");
                case Qt::Key_Control:  return QStringLiteral("ControlLeft");
                case Qt::Key_Alt:      return QStringLiteral("AltLeft");
                case Qt::Key_AltGr:    return QStringLiteral("AltRight");
                case Qt::Key_Meta:     return QStringLiteral("MetaLeft");
                case Qt::Key_Super_L:  return QStringLiteral("MetaLeft");
                case Qt::Key_Super_R:  return QStringLiteral("MetaRight");
            }
            return {};
        }

        case Qt::Key_Escape:     return QStringLiteral("Escape");
        case Qt::Key_Tab:        return QStringLiteral("Tab");
        case Qt::Key_Backtab:    return QStringLiteral("Tab");
        case Qt::Key_Backspace:  return QStringLiteral("Backspace");
        case Qt::Key_Return:     return QStringLiteral("Enter");
        case Qt::Key_Enter:      return QStringLiteral("NumpadEnter");
        case Qt::Key_Space:      return QStringLiteral("Space");
        case Qt::Key_Insert:     return QStringLiteral("Insert");
        case Qt::Key_Delete:     return QStringLiteral("Delete");
        case Qt::Key_Home:       return QStringLiteral("Home");
        case Qt::Key_End:        return QStringLiteral("End");
        case Qt::Key_PageUp:     return QStringLiteral("PageUp");
        case Qt::Key_PageDown:   return QStringLiteral("PageDown");
        case Qt::Key_Left:       return QStringLiteral("ArrowLeft");
        case Qt::Key_Right:      return QStringLiteral("ArrowRight");
        case Qt::Key_Up:         return QStringLiteral("ArrowUp");
        case Qt::Key_Down:       return QStringLiteral("ArrowDown");

        case Qt::Key_CapsLock:   return QStringLiteral("CapsLock");
        case Qt::Key_NumLock:    return QStringLiteral("NumLock");
        case Qt::Key_ScrollLock: return QStringLiteral("ScrollLock");
        case Qt::Key_Print:      return QStringLiteral("PrintScreen");
        case Qt::Key_Pause:      return QStringLiteral("Pause");
        case Qt::Key_Menu:       return QStringLiteral("ContextMenu");

        // Punctuation — Qt provides these by unshifted-US-layout key.
        case Qt::Key_Minus:        return QStringLiteral("Minus");
        case Qt::Key_Equal:        return QStringLiteral("Equal");
        case Qt::Key_BracketLeft:  return QStringLiteral("BracketLeft");
        case Qt::Key_BracketRight: return QStringLiteral("BracketRight");
        case Qt::Key_Backslash:    return QStringLiteral("Backslash");
        case Qt::Key_Semicolon:    return QStringLiteral("Semicolon");
        case Qt::Key_Apostrophe:   return QStringLiteral("Quote");
        case Qt::Key_Comma:        return QStringLiteral("Comma");
        case Qt::Key_Period:       return QStringLiteral("Period");
        case Qt::Key_Slash:        return QStringLiteral("Slash");
        case Qt::Key_QuoteLeft:    return QStringLiteral("Backquote");

        // Shifted-symbol forms (US layout). When Shift is held, Qt
        // resolves the produced character into a different Qt::Key
        // enum than the unshifted physical key — Shift+2 reports
        // Qt::Key_At rather than Qt::Key_2. We send the *physical*
        // MDN code (Digit2) and let the target's keymap apply Shift
        // via the modifier signal, so map every shifted-symbol form
        // back to the same code as its unshifted sibling above.
        // Layout assumption matches DESIGN.md (Kubuntu en-us).
        case Qt::Key_Exclam:       return QStringLiteral("Digit1");
        case Qt::Key_At:           return QStringLiteral("Digit2");
        case Qt::Key_NumberSign:   return QStringLiteral("Digit3");
        case Qt::Key_Dollar:       return QStringLiteral("Digit4");
        case Qt::Key_Percent:      return QStringLiteral("Digit5");
        case Qt::Key_AsciiCircum:  return QStringLiteral("Digit6");
        case Qt::Key_Ampersand:    return QStringLiteral("Digit7");
        case Qt::Key_Asterisk:     return QStringLiteral("Digit8");
        case Qt::Key_ParenLeft:    return QStringLiteral("Digit9");
        case Qt::Key_ParenRight:   return QStringLiteral("Digit0");
        case Qt::Key_Underscore:   return QStringLiteral("Minus");
        case Qt::Key_Plus:         return QStringLiteral("Equal");
        case Qt::Key_BraceLeft:    return QStringLiteral("BracketLeft");
        case Qt::Key_BraceRight:   return QStringLiteral("BracketRight");
        case Qt::Key_Bar:          return QStringLiteral("Backslash");
        case Qt::Key_Colon:        return QStringLiteral("Semicolon");
        case Qt::Key_QuoteDbl:     return QStringLiteral("Quote");
        case Qt::Key_Less:         return QStringLiteral("Comma");
        case Qt::Key_Greater:      return QStringLiteral("Period");
        case Qt::Key_Question:     return QStringLiteral("Slash");
        case Qt::Key_AsciiTilde:   return QStringLiteral("Backquote");
    }

    return {};
}

}  // namespace glasshouse
