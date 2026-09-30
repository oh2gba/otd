// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/DialMarks.h"
#include <QVector>
#include <QWidget>

// A horizontal tuning scale, the way an old radio dial reads: frequencies
// run from left to right, the tuned frequency sits under the pointer in
// the middle, and the stations around it are printed above the scale.
class DialScale : public QWidget
{
    Q_OBJECT
public:
    using Mark = DialMark;

    explicit DialScale(QWidget* parent = nullptr);

    void setCentre(double kHz);
    void setMarks(const QVector<Mark>& marks);
    void setHighlightKHz(double kHz);
    double spanKHz() const { return m_span; }
    void setSpanKHz(double kHz);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void tuneRequested(double kHz);
    void spanChanged(double kHz);

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    double xOf(double kHz) const;
    double kHzAt(double x) const;
    static double niceStep(double kHzPerPixel, int pixelsWanted);

    double m_centre = 0.0;
    double m_span = 100.0;       // width of the visible scale in kHz
    double m_highlight = 0.0;
    QVector<Mark> m_marks;
    // dragging the scale moves the pointer without touching the rig
    bool m_dragging = false;
    double m_dragStartX = 0.0;
    double m_dragStartCentre = 0.0;
    double m_dragOffsetKHz = 0.0;
};
