// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

// How much audio is in hand for the online receiver, in otd's queue and
// in the sound device: a small bar, full at a second. It sinks when the
// network stalls and goes red when a gap is about to be heard. Shown in
// the status bar while a receiver plays.
class BufferBar : public QWidget
{
    Q_OBJECT
public:
    explicit BufferBar(QWidget* parent = nullptr);
    void setSeconds(double s);
    void clear();
    double seconds() const { return m_valid ? m_seconds : 0.0; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    double m_seconds = 0.0;
    bool m_valid = false;
};
