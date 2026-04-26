#include "input_router.h"

#include "logging.h"
#include "pikvmclient.h"

namespace glasshouse {

InputRouter::InputRouter(PiKvmClient* hidMaster, QObject* parent)
    : QObject(parent), m_master(hidMaster) {}

void InputRouter::routeMouseMove(int api_x, int api_y) {
    if (!m_master) return;
    m_master->sendMouseMove(api_x, api_y);
}

void InputRouter::routeMouseButton(MouseButton button, bool pressed) {
    if (!m_master) return;
    m_master->sendMouseButton(button, pressed);
}

void InputRouter::routeMouseWheel(int delta_x, int delta_y) {
    if (!m_master) return;
    m_master->sendMouseWheel(delta_x, delta_y);
}

void InputRouter::routeKey(const QString& wireName, bool pressed) {
    if (!m_master) return;
    m_master->sendKey(wireName, pressed, /*finish=*/false);
}

void InputRouter::routeShortcut(const QStringList& keys) {
    if (!m_master) return;
    m_master->sendShortcut(keys);
}

void InputRouter::routeTypeText(const QString& text, bool slow, int delayMs) {
    if (!m_master) return;
    m_master->pasteText(text, slow, delayMs);
}

void InputRouter::routeAtxClick(const QString& button) {
    if (!m_master) return;
    m_master->atxClick(button);
}

}  // namespace glasshouse
