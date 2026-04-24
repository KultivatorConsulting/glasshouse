#include "pikvmevents.h"

namespace glasshouse {

QString mouseButtonToWire(MouseButton b) {
    switch (b) {
        case MouseButton::Left:    return QStringLiteral("left");
        case MouseButton::Middle:  return QStringLiteral("middle");
        case MouseButton::Right:   return QStringLiteral("right");
        case MouseButton::Back:    return QStringLiteral("up");
        case MouseButton::Forward: return QStringLiteral("down");
    }
    return {};
}

}  // namespace glasshouse
