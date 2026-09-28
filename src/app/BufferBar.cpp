// SPDX-License-Identifier: GPL-3.0-or-later
#include "BufferBar.h"

#include <QPainter>

BufferBar::BufferBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("bufferBar"));
    setFixedSize(46, 14);
}

void BufferBar::setSeconds(double s)
{
    m_seconds = qMax(0.0, s);
    m_valid = true;
    setToolTip(tr("%1 s of audio in hand for the online receiver").arg(m_seconds, 0, 'f', 1));
    update();
}

void BufferBar::clear()
{
    m_valid = false;
    setToolTip(QString());
    update();
}

void BufferBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(palette().color(QPalette::Mid));
    p.setBrush(palette().base());
    p.drawRoundedRect(r, 3, 3);
    if (!m_valid)
        return;
    const QRectF inner = r.adjusted(1, 1, -1, -1);
    QRectF fill = inner;
    fill.setWidth(inner.width() * qBound(0.0, m_seconds / 1.0, 1.0));   // full at a second
    p.setPen(Qt::NoPen);
    p.setBrush(m_seconds < 0.1 ? QColor(0xf8, 0x51, 0x49) : QColor(0x3f, 0xb9, 0x50));
    p.drawRect(fill);
}
