#include "pointerconfiner.h"
#include "logging.h"

// Qt-bearing headers above; Xlib (with its None/Bool/Status macros) last so
// it can't clobber anything they rely on. Nothing Qt is included after this.
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

namespace glasshouse {

struct PointerConfiner::Impl {
    Display*       dpy         = nullptr;
    Window         root        = 0;
    bool           xfixesOk    = false;
    PointerBarrier barriers[4] = {0, 0, 0, 0};
    bool           have        = false;

    Impl() {
        // Our own connection (independent of Qt's xcb one). Barriers are
        // owned by this connection and torn down when it closes, so an
        // orphaned process can't leave the pointer trapped.
        dpy = XOpenDisplay(nullptr);
        if (!dpy) {
            qCInfo(lcHid) << "pointer confiner: no X display (non-X11?) —"
                          << "confinement disabled";
            return;
        }
        int ev = 0, er = 0;
        if (!XFixesQueryExtension(dpy, &ev, &er)) {
            qCWarning(lcHid) << "pointer confiner: XFixes extension absent";
            return;
        }
        int maj = 0, min = 0;
        XFixesQueryVersion(dpy, &maj, &min);
        if (maj < 5) {
            qCWarning(lcHid) << "pointer confiner: XFixes" << maj << "." << min
                             << "lacks pointer barriers (need 5.0+)";
            return;
        }
        root     = DefaultRootWindow(dpy);
        xfixesOk = true;
    }

    ~Impl() {
        destroy();
        if (dpy) XCloseDisplay(dpy);
    }

    void destroy() {
        if (!dpy || !have) return;
        for (auto& b : barriers) {
            if (b) { XFixesDestroyPointerBarrier(dpy, b); b = 0; }
        }
        have = false;
        XSync(dpy, False);
    }
};

PointerConfiner::PointerConfiner() : m_impl(std::make_unique<Impl>()) {}
PointerConfiner::~PointerConfiner() = default;

bool PointerConfiner::confineToBounds(const QList<QRect>& rects) {
    if (!m_impl || !m_impl->xfixesOk) return false;
    m_impl->destroy();
    if (rects.isEmpty()) return false;

    QRect b = rects.first();
    for (const QRect& r : rects) {
        if (r.isValid()) b = b.united(r);
    }
    if (b.width() <= 1 || b.height() <= 1) return false;

    // Barrier lines. A barrier at line X (vertical) lets the pointer reach X
    // but not cross past it; right()/bottom() are the last inside pixel, so
    // the far edges sit at +1 to keep that pixel reachable. directions name
    // the *permitted* crossing direction (verified on this server), so each
    // edge permits only inward travel and blocks escape.
    Display* d    = m_impl->dpy;
    Window   root = m_impl->root;
    const int L = b.left();
    const int T = b.top();
    const int R = b.right()  + 1;
    const int Bo = b.bottom() + 1;

    m_impl->barriers[0] =
        XFixesCreatePointerBarrier(d, root, L, T, L, Bo, BarrierPositiveX, 0, nullptr);
    m_impl->barriers[1] =
        XFixesCreatePointerBarrier(d, root, R, T, R, Bo, BarrierNegativeX, 0, nullptr);
    m_impl->barriers[2] =
        XFixesCreatePointerBarrier(d, root, L, T, R, T, BarrierPositiveY, 0, nullptr);
    m_impl->barriers[3] =
        XFixesCreatePointerBarrier(d, root, L, Bo, R, Bo, BarrierNegativeY, 0, nullptr);
    m_impl->have = true;
    XSync(d, False);
    qCInfo(lcHid).nospace() << "pointer confined to " << b
                            << " (XFixes barriers)";
    return true;
}

void PointerConfiner::release() {
    if (m_impl) m_impl->destroy();
}

bool PointerConfiner::active() const {
    return m_impl && m_impl->have;
}

}  // namespace glasshouse
