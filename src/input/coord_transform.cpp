#include "coord_transform.h"

#include <algorithm>
#include <cmath>

namespace glasshouse {

namespace {

int toApi(double pxFraction) {
    // PiKVM s16 coord = round(fraction × 65535) − 32768, clamped.
    // The `round` happens before subtracting the bias, so that endpoints
    // (0 and 1) and the center (0.5) all land on exact integers and the
    // mapping stays monotonic and symmetric. Doing `round((f - 0.5) * N)`
    // instead introduces an off-by-one at 75% / 25% because the rounding
    // boundary shifts relative to the desktop midline. Matches the
    // §5.2 worked example and the §10.1 empirical endpoints.
    const double s = std::round(pxFraction * 65535.0) - 32768.0;
    return static_cast<int>(std::clamp(s, -32768.0, 32767.0));
}

}  // namespace

ApiCoord transformToApi(const CoordTransformInput& in) {
    // 1. cursor relative to window origin
    const int wx = in.localCursor.x() - in.windowRect.x();
    const int wy = in.localCursor.y() - in.windowRect.y();

    // 2. clamp into the letterbox rect (letterbox coords are window-local)
    const int lbW = std::max(1, in.letterboxRect.width());
    const int lbH = std::max(1, in.letterboxRect.height());
    const int vx = std::clamp(wx - in.letterboxRect.x(), 0, lbW);
    const int vy = std::clamp(wy - in.letterboxRect.y(), 0, lbH);

    // 3. normalise
    const double nx = static_cast<double>(vx) / static_cast<double>(lbW);
    const double ny = static_cast<double>(vy) / static_cast<double>(lbH);

    // 4. map to target-desktop pixel coords via this window's monitor rect
    const double tx_px = in.monitorRect.x() + nx * in.monitorRect.width();
    const double ty_px = in.monitorRect.y() + ny * in.monitorRect.height();

    // 5+6. to API s16 coords, clamped
    const int dw = std::max(1, in.logicalDesktop.width());
    const int dh = std::max(1, in.logicalDesktop.height());
    return { toApi(tx_px / dw), toApi(ty_px / dh) };
}

}  // namespace glasshouse
