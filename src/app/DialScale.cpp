// SPDX-License-Identifier: GPL-3.0-or-later
#include "DialScale.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace
{
constexpr int kLaneHeight = 17;
constexpr int kLanes = 4;
constexpr int kScaleHeight = 34;   // ticks and numbers at the bottom
constexpr int kLabelMaxWidth = 170;

QString kHzText(double kHz, double step)
{
    // whole kHz on coarse scales, decimals when the steps are below 1 kHz
    const int decimals = step >= 1.0 ? 0 : (step >= 0.1 ? 1 : 2);
    QString s = QString::number(kHz, 'f', decimals);
    // thin group separation, "9 500"
    int dot = s.indexOf(QLatin1Char('.'));
    int end = dot < 0 ? s.size() : dot;
    for (int i = end - 3; i > 0; i -= 3)
        s.insert(i, QChar(0x2009));
    return s;
}
}

DialScale::DialScale(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(false);
    setCursor(Qt::OpenHandCursor);
    setToolTip(tr("The tuned frequency under the pointer, stations around it on the scale.\n"
                  "Wheel: zoom. Drag: look around. Double-click: tune there."));
}

QSize DialScale::sizeHint() const
{
    return QSize(600, kLanes * kLaneHeight + kScaleHeight + 8);
}

QSize DialScale::minimumSizeHint() const
{
    return QSize(200, 2 * kLaneHeight + kScaleHeight + 8);
}

void DialScale::setCentre(double kHz)
{
    if (qFuzzyCompare(kHz + 1.0, m_centre + 1.0))
        return;
    m_centre = kHz;
    m_dragOffsetKHz = 0.0;
    update();
}

void DialScale::setMarks(const QVector<Mark>& marks)
{
    m_marks = marks;
    std::sort(m_marks.begin(), m_marks.end(), [](const Mark& a, const Mark& b) { return a.kHz < b.kHz; });
    update();
}

void DialScale::setHighlightKHz(double kHz)
{
    m_highlight = kHz;
    update();
}

void DialScale::setSpanKHz(double kHz)
{
    m_span = qBound(5.0, kHz, 20000.0);
    update();
}

double DialScale::xOf(double kHz) const
{
    return width() / 2.0 + (kHz - (m_centre + m_dragOffsetKHz)) / m_span * width();
}

double DialScale::kHzAt(double x) const
{
    return m_centre + m_dragOffsetKHz + (x - width() / 2.0) / width() * m_span;
}

double DialScale::niceStep(double kHzPerPixel, int pixelsWanted)
{
    const double raw = kHzPerPixel * pixelsWanted;
    static const double steps[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000};
    for (double s : steps)
        if (s >= raw)
            return s;
    return 10000;
}

void DialScale::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    const int w = width();
    const int h = height();
    const int scaleTop = h - kScaleHeight;

    p.fillRect(rect(), pal.base());

    if (m_centre <= 0.0)
    {
        p.setPen(pal.color(QPalette::Mid));
        p.drawText(rect(), Qt::AlignCenter, tr("no frequency"));
        return;
    }

    const double kHzPerPixel = m_span / w;
    const double major = niceStep(kHzPerPixel, 110);
    const double minor = major / (major >= 1.0 && int(std::round(major)) % 5 == 0 ? 5 : 2);
    const double left = kHzAt(0);
    const double right = kHzAt(w);

    // --- scale line and ticks ------------------------------------------
    QPen linePen(pal.color(QPalette::Mid));
    p.setPen(linePen);
    p.drawLine(0, scaleTop, w, scaleTop);
    QFont small = font();
    small.setPointSizeF(font().pointSizeF() * 0.85);
    p.setFont(small);
    const QFontMetrics fm(small);
    for (double f = std::floor(left / minor) * minor; f <= right + minor; f += minor)
    {
        if (f < 0.0)
            continue;
        const double x = xOf(f);
        const bool isMajor = std::fabs(f / major - std::round(f / major)) < 1e-6;
        p.setPen(linePen);
        p.drawLine(QPointF(x, scaleTop), QPointF(x, scaleTop + (isMajor ? 12 : 6)));
        if (isMajor)
        {
            p.setPen(pal.color(QPalette::Text));
            const QString t = kHzText(f, major);
            p.drawText(QRectF(x - 60, scaleTop + 13, 120, fm.height() + 2), Qt::AlignHCenter | Qt::AlignTop, t);
        }
    }

    // --- station marks ---------------------------------------------------
    p.setFont(font());
    const QFontMetrics fmLabel(font());
    double laneEnd[kLanes];
    for (double& e : laneEnd)
        e = -1e9;
    const QColor onAir = pal.color(QPalette::Text);
    const QColor maybe = pal.color(QPalette::Text);
    // off-air labels: between the text and the frame colour, readable on
    // light and dark themes alike
    QColor off = pal.color(QPalette::Text);
    const QColor bg = pal.color(QPalette::Base);
    off = QColor((off.red() + bg.red()) / 2, (off.green() + bg.green()) / 2, (off.blue() + bg.blue()) / 2);
    const QColor red(0xf8, 0x51, 0x49);
    for (const Mark& m : m_marks)
    {
        if (m.kHz < left - 1.0 || m.kHz > right + 1.0)
            continue;
        const double x = xOf(m.kHz);
        const bool hot = std::fabs(m.kHz - m_centre) <= m_highlight + 1e-6;
        QColor colour = m.rank == 0 ? onAir : (m.rank == 1 ? maybe : off);
        if (hot)
            colour = red;
        QFont f = font();
        f.setBold(m.rank == 0);
        f.setItalic(m.rank == 1);
        p.setFont(f);
        const QFontMetrics fmM(f);
        QString label = m.name;
        if (m.count > 1)
            label += QStringLiteral(" +%1").arg(m.count - 1);
        label = fmM.elidedText(label, Qt::ElideRight, kLabelMaxWidth);
        const double tw = fmM.horizontalAdvance(label);
        // first lane where the label fits after the previous one
        int lane = -1;
        for (int l = 0; l < kLanes; ++l)
            if (x - 2 > laneEnd[l] + 8)
            {
                lane = l;
                break;
            }
        // tick on the scale for every frequency, labelled or not
        p.setPen(QPen(colour, m.rank == 0 ? 2.0 : 1.0));
        const int laneY = lane < 0 ? scaleTop - 6 : scaleTop - 6 - (kLanes - 1 - lane) * kLaneHeight;
        p.drawLine(QPointF(x, scaleTop), QPointF(x, laneY));
        if (lane < 0)
            continue;
        laneEnd[lane] = x + tw;
        p.setPen(colour);
        p.drawText(QPointF(x + 3, laneY + fmM.ascent() - kLaneHeight + 4), label);
    }

    // --- the pointer -----------------------------------------------------
    const double cx = xOf(m_centre);
    QPen pointer(red, 2.0);
    p.setPen(pointer);
    p.drawLine(QPointF(cx, 0), QPointF(cx, h));
    QPainterPath tri;
    tri.moveTo(cx - 6, 0);
    tri.lineTo(cx + 6, 0);
    tri.lineTo(cx, 8);
    tri.closeSubpath();
    p.fillPath(tri, red);
    if (std::fabs(m_dragOffsetKHz) > 1e-6)
    {
        // while looking around, say where the pointer would land
        const double under = kHzAt(w / 2.0);
        p.setFont(small);
        p.setPen(pal.color(QPalette::Mid));
        p.drawText(QRectF(w / 2.0 - 80, 2, 160, fm.height() + 2), Qt::AlignHCenter,
                   tr("double-click to tune to %1").arg(kHzText(under, 1.0)));
        p.setPen(QPen(pal.color(QPalette::Mid), 1.0, Qt::DashLine));
        p.drawLine(QPointF(w / 2.0, 0), QPointF(w / 2.0, h));
    }
}

void DialScale::wheelEvent(QWheelEvent* event)
{
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0)
        return;
    static const double spans[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000};
    int idx = 0;
    for (int i = 0; i < int(std::size(spans)); ++i)
        if (spans[i] <= m_span + 1e-6)
            idx = i;
    idx = qBound(0, idx - steps, int(std::size(spans)) - 1);   // wheel up = zoom in
    setSpanKHz(spans[idx]);
    event->accept();
}

void DialScale::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_dragging = true;
    m_dragStartX = event->position().x();
    m_dragStartCentre = m_dragOffsetKHz;
    setCursor(Qt::ClosedHandCursor);
}

void DialScale::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging)
        return;
    m_dragOffsetKHz = m_dragStartCentre - (event->position().x() - m_dragStartX) / width() * m_span;
    update();
}

void DialScale::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_dragging = false;
    setCursor(Qt::OpenHandCursor);
}

void DialScale::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_centre <= 0.0)
        return;
    const double x = event->position().x();
    // snap to a station within a few pixels, otherwise to a round kHz
    double best = -1.0;
    double bestDist = 7.0;
    for (const Mark& m : m_marks)
    {
        const double d = std::fabs(xOf(m.kHz) - x);
        if (d < bestDist)
        {
            bestDist = d;
            best = m.kHz;
        }
    }
    const double kHz = best > 0.0 ? best : std::round(kHzAt(x));
    m_dragOffsetKHz = 0.0;
    emit tuneRequested(kHz);
}
