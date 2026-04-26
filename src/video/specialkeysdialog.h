#pragma once

#include "config.h"

#include <QDialog>
#include <QHash>
#include <QString>
#include <QStringList>

class QPushButton;
class QPlainTextEdit;
class QLabel;
class QCheckBox;
class QSpinBox;
class QVBoxLayout;

namespace glasshouse {

// Floating "send special keys" palette. Solves the cases where the local
// compositor swallows a keypress before our captured window sees it
// (Print Screen on KDE Wayland is the canonical example) — and gives the
// user clipboard-paste-as-keystrokes via PiKVM's `POST /api/hid/print`.
//
// Three tabs:
//   1. Keyboard  — full US-QWERTY on-screen keyboard with sticky-toggle
//                  modifiers. Click a non-modifier key → emits
//                  `shortcut(active_modifiers + key)`.
//   2. Shortcuts — curated chords (Ctrl+Alt+Del, Win+L, …) plus any
//                  custom entries supplied via `setCustomShortcuts`.
//   3. Paste     — multiline editor with a "Type N chars" button that
//                  emits `typeText` for the server-side keystroke
//                  endpoint. Slow / delay knobs surfaced for picky
//                  BIOS prompts.
//
// Non-modal, stays-on-top, owned by main.cpp. Opening the dialog is
// expected to release the parent window's keyboard grab (Qt focus
// semantics) — that's fine, the user re-captures with a click after.
class SpecialKeysDialog : public QDialog {
    Q_OBJECT
public:
    explicit SpecialKeysDialog(QWidget* parent = nullptr);
    ~SpecialKeysDialog() override;

    // Replace the "Custom" section with shortcuts from config.
    void setCustomShortcuts(const QList<ShortcutSpec>& shortcuts);

public slots:
    // Hotkey-driven entry point: show if hidden, hide if visible.
    void toggle();

signals:
    // Single chord, press-in-order release-in-reverse (matches kvmd's
    // `send_shortcut`).
    void shortcut(const QStringList& keys);
    // UTF-8 text → server-side typing endpoint (`POST /api/hid/print`).
    void typeText(const QString& text, bool slow, int delayMs);

private:
    void buildKeyboardTab();
    void buildShortcutsTab();
    void buildPasteTab();

    // Modifier state for the OSK tab. Keys are MDN web_names ("ShiftLeft"
    // etc.); value is the QPushButton (which we ask whether it's still
    // checked when emitting a chord).
    QHash<QString, QPushButton*> m_modButtons;

    // Emit a chord for a non-modifier press: every modifier whose button
    // is currently `isChecked()` is prepended.
    void sendKeyWithModifiers(const QString& keyName);

    QWidget*        m_keyboardTab  = nullptr;
    QWidget*        m_shortcutsTab = nullptr;
    QWidget*        m_pasteTab     = nullptr;
    QVBoxLayout*    m_customLayout = nullptr;  // populated by setCustomShortcuts

    QPlainTextEdit* m_pasteEdit  = nullptr;
    QPushButton*    m_pasteBtn   = nullptr;
    QLabel*         m_pasteCount = nullptr;
    QCheckBox*      m_pasteSlow  = nullptr;
    QSpinBox*       m_pasteDelay = nullptr;
};

}  // namespace glasshouse
