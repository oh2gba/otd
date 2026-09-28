// SPDX-License-Identifier: GPL-3.0-or-later
#include "PcmQueue.h"

#include <cstring>

PcmQueue::PcmQueue(int bytesPerSecond, char silence, QObject* parent)
    : QIODevice(parent)
    , m_preroll(bytesPerSecond * 3 / 10)
    , m_max(bytesPerSecond * 3)
    , m_silence(silence)
{
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
}

void PcmQueue::push(const QByteArray& pcm)
{
    if (pcm.isEmpty())
        return;
    // a long stall (not the usual gap between two bursts): the device has
    // long run out, so collect a little again before going on
    if (m_primed && m_buf.isEmpty() && m_emptySince.isValid() && m_emptySince.elapsed() > 1500)
        m_primed = false;
    m_emptySince.invalidate();
    m_buf.append(pcm);
    if (m_buf.size() > m_max)
        m_buf.remove(0, m_buf.size() - m_max);
    if (m_buf.size() >= m_preroll)
        m_primed = true;
    if (m_primed)
        emit readyRead();
}

qint64 PcmQueue::bytesAvailable() const
{
    return (m_primed ? m_buf.size() : 0) + QIODevice::bytesAvailable();
}

qint64 PcmQueue::readData(char* data, qint64 maxlen)
{
    if (maxlen <= 0)
        return 0;
    qint64 n = 0;
    if (m_primed)
    {
        n = qMin<qint64>(maxlen, m_buf.size());
        std::memcpy(data, m_buf.constData(), size_t(n));
        m_buf.remove(0, int(n));
        if (m_buf.isEmpty() && !m_emptySince.isValid())
            m_emptySince.start();
    }
    // A sink that asks for more than there is gets silence for the rest
    // and never a read of nothing: Qt's PulseAudio sink takes that as the
    // end of the source and stops pulling for good. It asks for a fixed
    // period on its own clock; Windows asks for what bytesAvailable() says,
    // nothing before the pre-roll, and never more than there is.
    if (n < maxlen)
        std::memset(data + n, m_silence, size_t(maxlen - n));
    return maxlen;
}
