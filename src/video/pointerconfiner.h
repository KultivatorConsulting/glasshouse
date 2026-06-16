#pragma once

#include <QList>
#include <QRect>

#include <memory>

namespace glasshouse {

// Confines the mouse pointer to the bounding rectangle of a set of windows
// using X11 XFixes pointer barriers. Barriers are server-side: they block
// pointer motion at the boundary lines without grabbing the pointer or
// warping it, so — unlike QCursor::setPos — they cannot feed events back
// into the application or stall its event loop.
//
// X11/xcb only. On any other platform (Wayland, etc.) every call is a no-op
// and confineToBounds() returns false; true confinement there would need the
// compositor's pointer-constraints protocol, which is not implemented.
//
// All X11 specifics live in the .cpp; this header is Qt-only so it can be
// included from Qt-heavy translation units without dragging in Xlib's macro
// soup (None/Bool/Status/…).
class PointerConfiner {
public:
    PointerConfiner();
    ~PointerConfiner();
    PointerConfiner(const PointerConfiner&)            = delete;
    PointerConfiner& operator=(const PointerConfiner&) = delete;

    // Confine the pointer to the bounding rectangle of `rects` (global
    // screen coordinates, e.g. each window's frameGeometry()). Any prior
    // confinement is released first. Returns true if barriers were installed.
    bool confineToBounds(const QList<QRect>& rects);

    // Remove all barriers. Safe to call when not active.
    void release();

    bool active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace glasshouse
