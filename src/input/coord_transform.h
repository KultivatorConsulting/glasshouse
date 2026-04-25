#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>

namespace glasshouse {

// Pure coordinate transform from a local cursor position to PiKVM HID
// API coordinates. Implements DESIGN.md §5.2:
//
//     1. local cursor → window-relative
//     2. window-relative → clamped into the letterbox video rect
//     3. letterbox-relative → [0..1]
//     4. [0..1] × target-monitor rect → target-desktop pixel
//     5. target-desktop pixel → PiKVM s16 API coord, centered on (0,0)
//     6. clamp to [-32768, 32767]
//
// Empirical coord-range verification is in §10.1.
struct CoordTransformInput {
    QPoint localCursor;     // global screen coords of the cursor
    QRect  windowRect;      // window geometry in global screen coords
    QRect  letterboxRect;   // video rect inside the window, window-local
    QRect  monitorRect;     // this window's target monitor within the desktop
    QSize  logicalDesktop;  // full target desktop extent
};

struct ApiCoord {
    int x;
    int y;
};

ApiCoord transformToApi(const CoordTransformInput& in);

}  // namespace glasshouse
