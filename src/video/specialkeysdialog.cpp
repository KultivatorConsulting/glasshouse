#include "specialkeysdialog.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace glasshouse {

namespace {

// Compact key descriptor for the OSK grid. All non-zero `colSpan` values
// are integer multiples of one "letter cell". The grid uses 1 cell per
// integer; modifier widths are approximations rather than pixel-perfect
// reproductions of a real keyboard.
struct K {
    const char* wire;     // PiKVM web_name (or "" for layout spacers)
    const char* label;    // shown on the button
    bool        isMod;
    int         colSpan;
};

QPushButton* makeKeyButton(const K& k) {
    auto* b = new QPushButton(QString::fromUtf8(k.label));
    b->setCheckable(k.isMod);
    b->setMinimumWidth(38 * std::max(1, k.colSpan));
    b->setMinimumHeight(34);
    b->setFocusPolicy(Qt::NoFocus);  // don't steal tab-focus from the editor
    return b;
}

// Build a horizontal row of keys. Modifier buttons are stored in
// `mods`; non-modifier buttons get connected to `onPress(wireName)`.
template <typename Press>
QHBoxLayout* makeRow(const QList<K>& keys,
                     QHash<QString, QPushButton*>& mods,
                     Press onPress) {
    auto* row = new QHBoxLayout;
    row->setSpacing(2);
    for (const K& k : keys) {
        if (k.label[0] == '\0') {
            row->addSpacing(20);
            continue;
        }
        QPushButton* b = makeKeyButton(k);
        if (k.isMod) {
            mods.insert(QString::fromLatin1(k.wire), b);
        } else {
            const QString wire = QString::fromLatin1(k.wire);
            QObject::connect(b, &QPushButton::clicked, b, [wire, onPress]() {
                onPress(wire);
            });
        }
        row->addWidget(b);
    }
    row->addStretch(1);
    return row;
}

// US QWERTY layout. Six rows for the alphanumeric block, plus a
// nav/arrow block we add separately.
const QList<QList<K>> kKeyboardRows = {
    // F-row
    {
        {"Escape", "Esc", false, 1},
        {"", "", false, 0},
        {"F1", "F1", false, 1}, {"F2", "F2", false, 1},
        {"F3", "F3", false, 1}, {"F4", "F4", false, 1},
        {"", "", false, 0},
        {"F5", "F5", false, 1}, {"F6", "F6", false, 1},
        {"F7", "F7", false, 1}, {"F8", "F8", false, 1},
        {"", "", false, 0},
        {"F9", "F9",  false, 1}, {"F10", "F10", false, 1},
        {"F11", "F11", false, 1}, {"F12", "F12", false, 1},
    },
    // Number row
    {
        {"Backquote", "`",  false, 1},
        {"Digit1", "1", false, 1}, {"Digit2", "2", false, 1},
        {"Digit3", "3", false, 1}, {"Digit4", "4", false, 1},
        {"Digit5", "5", false, 1}, {"Digit6", "6", false, 1},
        {"Digit7", "7", false, 1}, {"Digit8", "8", false, 1},
        {"Digit9", "9", false, 1}, {"Digit0", "0", false, 1},
        {"Minus", "-",  false, 1}, {"Equal", "=", false, 1},
        {"Backspace", "⌫ Bksp", false, 2},
    },
    // Tab row
    {
        {"Tab", "Tab", false, 2},
        {"KeyQ", "Q", false, 1}, {"KeyW", "W", false, 1},
        {"KeyE", "E", false, 1}, {"KeyR", "R", false, 1},
        {"KeyT", "T", false, 1}, {"KeyY", "Y", false, 1},
        {"KeyU", "U", false, 1}, {"KeyI", "I", false, 1},
        {"KeyO", "O", false, 1}, {"KeyP", "P", false, 1},
        {"BracketLeft", "[", false, 1}, {"BracketRight", "]", false, 1},
        {"Backslash", "\\", false, 1},
    },
    // Caps row
    {
        {"CapsLock", "Caps", true, 2},
        {"KeyA", "A", false, 1}, {"KeyS", "S", false, 1},
        {"KeyD", "D", false, 1}, {"KeyF", "F", false, 1},
        {"KeyG", "G", false, 1}, {"KeyH", "H", false, 1},
        {"KeyJ", "J", false, 1}, {"KeyK", "K", false, 1},
        {"KeyL", "L", false, 1},
        {"Semicolon", ";", false, 1}, {"Quote", "'", false, 1},
        {"Enter", "↵ Enter", false, 2},
    },
    // Shift row
    {
        {"ShiftLeft", "⇧ Shift", true, 2},
        {"KeyZ", "Z", false, 1}, {"KeyX", "X", false, 1},
        {"KeyC", "C", false, 1}, {"KeyV", "V", false, 1},
        {"KeyB", "B", false, 1}, {"KeyN", "N", false, 1},
        {"KeyM", "M", false, 1},
        {"Comma", ",", false, 1}, {"Period", ".", false, 1},
        {"Slash", "/", false, 1},
        {"ShiftRight", "⇧ Shift", true, 2},
    },
    // Bottom row
    {
        {"ControlLeft", "Ctrl", true, 1},
        {"MetaLeft",    "Win",  true, 1},
        {"AltLeft",     "Alt",  true, 1},
        {"Space",       "Space", false, 6},
        {"AltRight",    "Alt",  true, 1},
        {"MetaRight",   "Win",  true, 1},
        {"ContextMenu", "Menu", false, 1},
        {"ControlRight","Ctrl", true, 1},
    },
};

// Right-hand nav block (Insert/Home/PageUp + Delete/End/PageDown +
// arrow cluster). Built as its own grid to the right of the alphanum
// area so it doesn't disturb the row alignment.
const QList<K> kNavBlock = {
    {"Insert", "Ins",  false, 1}, {"Home", "Home", false, 1}, {"PageUp",   "PgUp", false, 1},
    {"Delete", "Del",  false, 1}, {"End",  "End",  false, 1}, {"PageDown", "PgDn", false, 1},
};
const QList<K> kArrowBlock = {
    {"ArrowUp",    "↑", false, 1},
    {"ArrowLeft",  "←", false, 1},
    {"ArrowDown",  "↓", false, 1},
    {"ArrowRight", "→", false, 1},
};

// Curated chords for the Shortcuts tab. Each entry is the chord we'd
// pass to `send_shortcut` (PiKVM keymap.csv `web_name`s).
struct ChordEntry {
    const char* label;
    QStringList keys;
};

const QList<ChordEntry> kCuratedChords = {
    // System / login
    {"Ctrl+Alt+Del",       {"ControlLeft", "AltLeft", "Delete"}},
    {"Ctrl+Alt+Backspace", {"ControlLeft", "AltLeft", "Backspace"}},

    // Linux VT switching
    {"Ctrl+Alt+F1", {"ControlLeft", "AltLeft", "F1"}},
    {"Ctrl+Alt+F2", {"ControlLeft", "AltLeft", "F2"}},
    {"Ctrl+Alt+F3", {"ControlLeft", "AltLeft", "F3"}},
    {"Ctrl+Alt+F7", {"ControlLeft", "AltLeft", "F7"}},

    // Windows / desktop
    {"Win",   {"MetaLeft"}},
    {"Win+L", {"MetaLeft", "KeyL"}},
    {"Win+R", {"MetaLeft", "KeyR"}},
    {"Win+E", {"MetaLeft", "KeyE"}},
    {"Win+D", {"MetaLeft", "KeyD"}},

    // Single-key things the local compositor often eats
    {"Print Screen", {"PrintScreen"}},
    {"Pause / Break", {"Pause"}},
    {"Scroll Lock",   {"ScrollLock"}},
    {"Menu",          {"ContextMenu"}},

    // App-level
    {"Alt+F4",  {"AltLeft", "F4"}},
    {"Alt+Tab", {"AltLeft", "Tab"}},
    {"Ctrl+Esc",{"ControlLeft", "Escape"}},
};

}  // namespace

// ---------------------------------------------------------------------------

SpecialKeysDialog::SpecialKeysDialog(QWidget* parent)
    : QDialog(parent,
              Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowCloseButtonHint) {
    setWindowTitle(QStringLiteral("Special Keys"));

    auto* tabs = new QTabWidget(this);

    buildKeyboardTab();
    buildShortcutsTab();
    buildPasteTab();
    tabs->addTab(m_keyboardTab,  QStringLiteral("Keyboard"));
    tabs->addTab(m_shortcutsTab, QStringLiteral("Shortcuts"));
    tabs->addTab(m_pasteTab,     QStringLiteral("Paste"));

    auto* root = new QVBoxLayout(this);
    root->addWidget(tabs);
    root->setContentsMargins(8, 8, 8, 8);

    resize(720, 360);
}

SpecialKeysDialog::~SpecialKeysDialog() = default;

// ---------------------------------------------------------------------------

void SpecialKeysDialog::buildKeyboardTab() {
    m_keyboardTab = new QWidget(this);

    auto press = [this](const QString& wire) { sendKeyWithModifiers(wire); };

    // Alphanumeric area: stack of horizontal rows.
    auto* alpha = new QVBoxLayout;
    alpha->setSpacing(2);
    for (const auto& row : kKeyboardRows) {
        alpha->addLayout(makeRow(row, m_modButtons, press));
    }

    // Nav block (Ins/Home/PgUp + Del/End/PgDn): 3-col grid, two rows.
    auto* nav = new QGridLayout;
    nav->setSpacing(2);
    for (int i = 0; i < kNavBlock.size(); ++i) {
        const K& k = kNavBlock[i];
        QPushButton* b = makeKeyButton(k);
        const QString wire = QString::fromLatin1(k.wire);
        QObject::connect(b, &QPushButton::clicked, b, [wire, press]() {
            press(wire);
        });
        nav->addWidget(b, i / 3, i % 3);
    }

    // Arrow cluster: classic + shape.
    auto* arrow = new QGridLayout;
    arrow->setSpacing(2);
    auto place = [&](const K& k, int r, int c) {
        QPushButton* b = makeKeyButton(k);
        const QString wire = QString::fromLatin1(k.wire);
        QObject::connect(b, &QPushButton::clicked, b, [wire, press]() {
            press(wire);
        });
        arrow->addWidget(b, r, c);
    };
    place(kArrowBlock[0], 0, 1);  // up
    place(kArrowBlock[1], 1, 0);  // left
    place(kArrowBlock[2], 1, 1);  // down
    place(kArrowBlock[3], 1, 2);  // right

    auto* rightCol = new QVBoxLayout;
    rightCol->addLayout(nav);
    rightCol->addSpacing(8);
    rightCol->addLayout(arrow);
    rightCol->addStretch(1);

    auto* hint = new QLabel(QStringLiteral(
        "Modifier keys are sticky toggles — click again to release."));
    hint->setStyleSheet(QStringLiteral("color: gray;"));

    auto* outer = new QHBoxLayout;
    outer->addLayout(alpha, 1);
    outer->addLayout(rightCol);

    auto* root = new QVBoxLayout(m_keyboardTab);
    root->addLayout(outer);
    root->addWidget(hint);
}

// ---------------------------------------------------------------------------

void SpecialKeysDialog::buildShortcutsTab() {
    m_shortcutsTab = new QWidget(this);

    auto* curated = new QGroupBox(QStringLiteral("Curated"), m_shortcutsTab);
    auto* curatedGrid = new QGridLayout(curated);
    curatedGrid->setSpacing(4);
    for (int i = 0; i < kCuratedChords.size(); ++i) {
        const ChordEntry& e = kCuratedChords[i];
        auto* b = new QPushButton(QString::fromLatin1(e.label));
        b->setMinimumHeight(32);
        b->setFocusPolicy(Qt::NoFocus);
        const QStringList keys = e.keys;
        connect(b, &QPushButton::clicked, this, [this, keys]() {
            emit shortcut(keys);
        });
        curatedGrid->addWidget(b, i / 4, i % 4);
    }

    auto* customBox = new QGroupBox(QStringLiteral("Custom"), m_shortcutsTab);
    m_customLayout = new QVBoxLayout(customBox);
    auto* placeholder = new QLabel(QStringLiteral(
        "(none — populate the `shortcuts:` list in your YAML config)"));
    placeholder->setStyleSheet(QStringLiteral("color: gray;"));
    m_customLayout->addWidget(placeholder);
    m_customLayout->addStretch(1);

    auto* root = new QVBoxLayout(m_shortcutsTab);
    root->addWidget(curated);
    root->addWidget(customBox);
    root->addStretch(1);
}

// ---------------------------------------------------------------------------

void SpecialKeysDialog::buildPasteTab() {
    m_pasteTab = new QWidget(this);

    m_pasteEdit = new QPlainTextEdit(m_pasteTab);
    m_pasteEdit->setPlaceholderText(QStringLiteral(
        "Paste here (Ctrl+V). Click 'Type N chars' to send via the "
        "PiKVM /api/hid/print endpoint — server-side keymap conversion "
        "applies, so the target's keyboard layout is what determines "
        "what gets typed."));

    m_pasteCount = new QLabel(QStringLiteral("0 chars"));
    m_pasteSlow  = new QCheckBox(QStringLiteral("Slow (insert delay between keystrokes)"));
    m_pasteDelay = new QSpinBox;
    m_pasteDelay->setRange(0, 5000);
    m_pasteDelay->setSingleStep(10);
    m_pasteDelay->setValue(50);
    m_pasteDelay->setSuffix(QStringLiteral(" ms"));
    m_pasteDelay->setEnabled(false);
    connect(m_pasteSlow, &QCheckBox::toggled, m_pasteDelay, &QSpinBox::setEnabled);

    m_pasteBtn = new QPushButton(QStringLiteral("Type 0 chars"));
    m_pasteBtn->setEnabled(false);
    m_pasteBtn->setFocusPolicy(Qt::NoFocus);

    connect(m_pasteEdit, &QPlainTextEdit::textChanged, this, [this]() {
        const int n = m_pasteEdit->toPlainText().size();
        m_pasteCount->setText(QStringLiteral("%1 chars").arg(n));
        m_pasteBtn->setText(QStringLiteral("Type %1 chars").arg(n));
        m_pasteBtn->setEnabled(n > 0);
    });

    connect(m_pasteBtn, &QPushButton::clicked, this, [this]() {
        const QString text = m_pasteEdit->toPlainText();
        if (text.isEmpty()) return;
        emit typeText(text, m_pasteSlow->isChecked(), m_pasteDelay->value());
    });

    auto* opts = new QHBoxLayout;
    opts->addWidget(m_pasteSlow);
    opts->addWidget(m_pasteDelay);
    opts->addStretch(1);
    opts->addWidget(m_pasteCount);
    opts->addWidget(m_pasteBtn);

    auto* root = new QVBoxLayout(m_pasteTab);
    root->addWidget(m_pasteEdit, 1);
    root->addLayout(opts);
}

// ---------------------------------------------------------------------------

void SpecialKeysDialog::setCustomShortcuts(const QList<ShortcutSpec>& shortcuts) {
    if (!m_customLayout) return;

    // Clear existing rows (placeholder + any previous entries) before
    // repopulating. The trailing stretch we re-add at the bottom.
    while (QLayoutItem* item = m_customLayout->takeAt(0)) {
        if (auto* w = item->widget()) w->deleteLater();
        delete item;
    }

    if (shortcuts.isEmpty()) {
        auto* placeholder = new QLabel(QStringLiteral(
            "(none — populate the `shortcuts:` list in your YAML config)"));
        placeholder->setStyleSheet(QStringLiteral("color: gray;"));
        m_customLayout->addWidget(placeholder);
    } else {
        // Lay out custom shortcuts as a 4-wide grid inside the group's
        // vertical layout — wrap the grid in its own widget so we can
        // mix it cleanly with the trailing stretch.
        auto* gridHost = new QWidget;
        auto* grid = new QGridLayout(gridHost);
        grid->setSpacing(4);
        grid->setContentsMargins(0, 0, 0, 0);
        for (int i = 0; i < shortcuts.size(); ++i) {
            const ShortcutSpec& s = shortcuts[i];
            auto* b = new QPushButton(s.label);
            b->setMinimumHeight(32);
            b->setFocusPolicy(Qt::NoFocus);
            const QStringList keys = s.keys;
            connect(b, &QPushButton::clicked, this, [this, keys]() {
                emit shortcut(keys);
            });
            grid->addWidget(b, i / 4, i % 4);
        }
        m_customLayout->addWidget(gridHost);
    }
    m_customLayout->addStretch(1);
}

// ---------------------------------------------------------------------------

void SpecialKeysDialog::toggle() {
    if (isVisible()) {
        hide();
    } else {
        show();
        raise();
        activateWindow();
    }
}

void SpecialKeysDialog::sendKeyWithModifiers(const QString& keyName) {
    QStringList chord;
    // Modifier order is somewhat arbitrary — the target only cares that
    // they're pressed before the non-modifier. Iterate the QHash order
    // (insertion order in modern Qt for the common case).
    for (auto it = m_modButtons.constBegin(); it != m_modButtons.constEnd(); ++it) {
        if (it.value() && it.value()->isChecked()) {
            chord << it.key();
        }
    }
    chord << keyName;
    emit shortcut(chord);
}

}  // namespace glasshouse
