// Spot checks for KeyMap. Full coverage is a treadmill — this covers the
// shapes we rely on (letter/digit/F-key arithmetic, L/R modifier
// disambiguation via nativeVirtualKey, common specials, unknown-key drop).

#include "keymap.h"

#include <QKeyEvent>
#include <QTest>

using namespace glasshouse;

namespace {

QKeyEvent mkKey(int key, quint32 nativeVirtualKey = 0) {
    return QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier,
                     /*nativeScanCode=*/0, nativeVirtualKey,
                     /*nativeModifiers=*/0);
}

constexpr quint32 XKB_Shift_L   = 0xffe1;
constexpr quint32 XKB_Shift_R   = 0xffe2;
constexpr quint32 XKB_Control_L = 0xffe3;
constexpr quint32 XKB_Alt_R     = 0xffea;

}  // namespace

class TestKeyMap : public QObject {
    Q_OBJECT
private slots:
    void lettersAreContiguous();
    void digitsAreContiguous();
    void functionKeysAreContiguous();
    void modifierLeftRightFromNative();
    void modifierFallbackWhenNativeMissing();
    void commonSpecials();
    void unknownKeyReturnsEmpty();
    void enterVsNumpadEnterDistinct();
};

void TestKeyMap::lettersAreContiguous() {
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_A)), QStringLiteral("KeyA"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_M)), QStringLiteral("KeyM"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_Z)), QStringLiteral("KeyZ"));
}

void TestKeyMap::digitsAreContiguous() {
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_0)), QStringLiteral("Digit0"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_5)), QStringLiteral("Digit5"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_9)), QStringLiteral("Digit9"));
}

void TestKeyMap::functionKeysAreContiguous() {
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_F1)),  QStringLiteral("F1"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_F12)), QStringLiteral("F12"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_F24)), QStringLiteral("F24"));
}

void TestKeyMap::modifierLeftRightFromNative() {
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_Shift,   XKB_Shift_L)),
             QStringLiteral("ShiftLeft"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_Shift,   XKB_Shift_R)),
             QStringLiteral("ShiftRight"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_Control, XKB_Control_L)),
             QStringLiteral("ControlLeft"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_Alt,     XKB_Alt_R)),
             QStringLiteral("AltRight"));
}

void TestKeyMap::modifierFallbackWhenNativeMissing() {
    // No nativeVirtualKey → default to the left variant. The target OS
    // treats both as the same modifier for standard bindings, and this
    // keeps the feature working in embed paths (offscreen test, eventsim)
    // that don't populate native codes.
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_Shift)),
             QStringLiteral("ShiftLeft"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_Control)),
             QStringLiteral("ControlLeft"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_Meta)),
             QStringLiteral("MetaLeft"));
}

void TestKeyMap::commonSpecials() {
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_Escape)),   QStringLiteral("Escape"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_Tab)),      QStringLiteral("Tab"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_Space)),    QStringLiteral("Space"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_Left)),     QStringLiteral("ArrowLeft"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_PageDown)), QStringLiteral("PageDown"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_QuoteLeft)),QStringLiteral("Backquote"));
}

void TestKeyMap::enterVsNumpadEnterDistinct() {
    // Qt distinguishes the main Enter (Key_Return) from numpad Enter
    // (Key_Enter); PiKVM's wire names follow the same split.
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_Return)), QStringLiteral("Enter"));
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_Enter)),  QStringLiteral("NumpadEnter"));
}

void TestKeyMap::unknownKeyReturnsEmpty() {
    // Qt::Key_MediaPlay etc. are not mapped; we drop them rather than
    // invent wire names the server will silently discard anyway.
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_MediaPlay)), QString());
    QCOMPARE(keyEventToWire(mkKey(Qt::Key_VolumeUp)),  QString());
}

QTEST_APPLESS_MAIN(TestKeyMap)
#include "test_keymap.moc"
