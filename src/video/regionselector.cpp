#include "regionselector.h"

#include "coord_transform.h"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace glasshouse {

namespace {

// Dim applied outside the selection — enough to separate it from the rest
// without hiding the surroundings the user is judging the edges against.
const QColor kDim(0, 0, 0, 140);
// The border does the real work. Dimming is nearly invisible over a dark
// target (black over black), so the selection has to be legible from its
// outline alone — hence a white line with a dark halo under it, which
// reads on both a terminal and a white web page.
const QColor kBorder(255, 255, 255, 245);
const QColor kBorderHalo(0, 0, 0, 160);
const QColor kChrome(0, 0, 0, 190);

}  // namespace

RegionSelector::RegionSelector(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    // Opaque: we paint every pixel, including the letterbox bars.
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void RegionSelector::setImage(const QImage& image) {
    m_image    = image;
    m_dragRect = QRect();
    m_dragging = false;
    m_anchor   = QPoint();
    update();
}

QRect RegionSelector::displayRect() const {
    return computeLetterbox(size(), m_image.size());
}

QRect RegionSelector::toImageRect(const QRect& widgetRect) const {
    const QRect disp = displayRect();
    if (m_image.isNull() || disp.width() <= 0 || disp.height() <= 0) return {};

    const double sx = static_cast<double>(m_image.width())  / disp.width();
    const double sy = static_cast<double>(m_image.height()) / disp.height();

    // Clip to the painted area first, so a drag that ran out onto a
    // letterbox bar doesn't map to pixels outside the frame.
    const QRect clipped = widgetRect.intersected(disp);
    if (clipped.isEmpty()) return {};

    // Treat the drag as a half-open interval and take every image pixel it
    // touches: widget pixel N covers image pixels [N*s, (N+1)*s). Rounding
    // the inclusive edges instead loses the fraction of a pixel the last
    // column covers, so selecting the whole frame yields 1919x1079.
    const int x1 = static_cast<int>(std::floor((clipped.left() - disp.x()) * sx));
    const int y1 = static_cast<int>(std::floor((clipped.top()  - disp.y()) * sy));
    const int x2 = static_cast<int>(
        std::ceil((clipped.right() + 1 - disp.x()) * sx));
    const int y2 = static_cast<int>(
        std::ceil((clipped.bottom() + 1 - disp.y()) * sy));

    return QRect(x1, y1, x2 - x1, y2 - y1)
        .intersected(QRect(QPoint(0, 0), m_image.size()));
}

void RegionSelector::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_image.isNull()) return;

    const QRect disp = displayRect();
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(disp, m_image);

    const QRect sel = m_dragRect.intersected(disp);

    if (sel.isEmpty()) {
        p.fillRect(disp, kDim);
    } else {
        // Dim the four bands around the selection rather than the whole
        // frame, so the chosen region stays at full brightness.
        p.fillRect(QRect(disp.left(), disp.top(),
                         disp.width(), sel.top() - disp.top()), kDim);
        p.fillRect(QRect(disp.left(), sel.bottom() + 1,
                         disp.width(), disp.bottom() - sel.bottom()), kDim);
        p.fillRect(QRect(disp.left(), sel.top(),
                         sel.left() - disp.left(), sel.height()), kDim);
        p.fillRect(QRect(sel.right() + 1, sel.top(),
                         disp.right() - sel.right(), sel.height()), kDim);

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(kBorderHalo, 1));
        p.drawRect(sel.adjusted(-1, -1, 0, 0));
        p.drawRect(sel.adjusted(1, 1, -2, -2));
        p.setPen(QPen(kBorder, 1));
        p.drawRect(sel.adjusted(0, 0, -1, -1));

        // Live pixel size, in frame pixels — what the caller will crop,
        // not what is on screen after letterbox scaling.
        const QRect imgRect = toImageRect(sel);
        const QString label = QStringLiteral("%1 × %2")
                                  .arg(imgRect.width()).arg(imgRect.height());
        const QFontMetrics fm(font());
        const QSize ts = fm.size(Qt::TextSingleLine, label);
        QRect box(sel.left(), sel.top() - ts.height() - 8,
                  ts.width() + 12, ts.height() + 6);
        // Above by preference; below when the selection is hard against
        // the top. Only sits inside the selection as a last resort, since
        // there it covers the content being selected.
        if (box.top() < disp.top()) {
            const int below = sel.bottom() + 8;
            box.moveTop(below + box.height() <= disp.bottom() ? below
                                                              : sel.top() + 4);
        }
        box.moveLeft(std::min(box.left(), disp.right() - box.width()));
        box.moveLeft(std::max(box.left(), disp.left()));
        p.setPen(Qt::NoPen);
        p.setBrush(kChrome);
        p.drawRoundedRect(box, 3, 3);
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, label);
    }

    // Instructions, top-centre, only until the drag starts.
    if (!m_dragging && sel.isEmpty()) {
        const QString hint =
            QStringLiteral("Drag to select a region  ·  Esc to cancel");
        const QFontMetrics fm(font());
        const QSize ts = fm.size(Qt::TextSingleLine, hint);
        QRect box((width() - ts.width()) / 2 - 10, disp.top() + 12,
                  ts.width() + 20, ts.height() + 10);
        p.setPen(Qt::NoPen);
        p.setBrush(kChrome);
        p.drawRoundedRect(box, 4, 4);
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, hint);
    }
}

void RegionSelector::mousePressEvent(QMouseEvent* ev) {
    if (ev->button() == Qt::RightButton) {
        emit cancelled();
        return;
    }
    if (ev->button() != Qt::LeftButton) return;
    m_anchor   = ev->pos();
    m_dragRect = QRect(m_anchor, m_anchor);
    m_dragging = true;
    update();
}

void RegionSelector::mouseMoveEvent(QMouseEvent* ev) {
    if (!m_dragging) return;
    m_dragRect = QRect(m_anchor, ev->pos()).normalized();
    update();
}

void RegionSelector::mouseReleaseEvent(QMouseEvent* ev) {
    if (ev->button() != Qt::LeftButton || !m_dragging) return;
    m_dragging = false;
    m_dragRect = QRect(m_anchor, ev->pos()).normalized();

    // A click, or a nudge, is someone changing their mind — not a request
    // to send a 3-pixel image off to a model.
    if (m_dragRect.width() < kMinDragPx || m_dragRect.height() < kMinDragPx) {
        emit cancelled();
        return;
    }
    const QRect imgRect = toImageRect(m_dragRect);
    if (imgRect.isEmpty()) {
        emit cancelled();
        return;
    }
    emit selected(imgRect);
}

void RegionSelector::keyPressEvent(QKeyEvent* ev) {
    if (ev->key() == Qt::Key_Escape) {
        emit cancelled();
        return;
    }
    QWidget::keyPressEvent(ev);
}

}  // namespace glasshouse
