#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QWidget>

namespace glasshouse {

// Freeze-frame rectangular region picker.
//
// Shown *in place of* the live QVideoWidget (both are pages of the window's
// QStackedWidget) rather than overlaid on top of it. Two reasons:
//
//   * Overlaying a child widget on a QVideoWidget means compositing against
//     whatever surface the video backend happens to be using. Swapping pages
//     is a plain widget paint, with none of that uncertainty.
//   * Selecting from a still is better behaviour anyway: the target can't
//     move under the user mid-drag, and the pixels they get are the ones
//     they were looking at when they pressed the hotkey.
//
// The still is painted with the same `Qt::KeepAspectRatio` letterbox maths
// the video widget uses (`computeLetterbox`), at the same widget geometry —
// so the swap is visually seamless apart from the dimming, and a selection
// lands exactly where the user drew it.
//
// Selections are emitted in *image pixel* coordinates, so a caller can crop
// the frame without knowing anything about widget scaling.
class RegionSelector : public QWidget {
    Q_OBJECT
public:
    explicit RegionSelector(QWidget* parent = nullptr);

    // Arm with the still to select from, clearing any previous drag.
    void setImage(const QImage& image);

signals:
    // Non-empty, clamped to the image bounds.
    void selected(const QRect& imageRect);
    // Esc, right-click, or a drag too small to be deliberate.
    void cancelled();

protected:
    void paintEvent(QPaintEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void keyPressEvent(QKeyEvent* ev) override;

private:
    // Where the still actually paints inside this widget (letterbox bars
    // excluded) — the same rect the video widget renders into.
    QRect displayRect() const;

    // Widget coords → image pixel coords, clamped to the image.
    QRect toImageRect(const QRect& widgetRect) const;

    QImage m_image;
    QPoint m_anchor;
    QRect  m_dragRect;            // widget coords, normalised
    bool   m_dragging = false;

    // Below this the drag is treated as a stray click, not a selection.
    static constexpr int kMinDragPx = 8;
};

}  // namespace glasshouse
