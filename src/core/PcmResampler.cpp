// SPDX-License-Identifier: GPL-3.0-or-later
#include "PcmResampler.h"

#include <QtEndian>
#include <cmath>
#include <cstring>

PcmResampler::PcmResampler(int inRate, int outRate, int channels, Format format)
    : m_step(outRate > 0 ? double(inRate) / outRate : 1.0)
    , m_channels(qMax(1, channels))
    , m_format(format)
{
}

int PcmResampler::bytesPerFrame() const
{
    switch (m_format)
    {
    case Format::UInt8: return m_channels;
    case Format::Int16: return 2 * m_channels;
    case Format::Int32:
    case Format::Float: return 4 * m_channels;
    }
    return 2 * m_channels;
}

void PcmResampler::appendFrame(QByteArray& out, float sample) const
{
    for (int c = 0; c < m_channels; ++c)
    {
        const float s = c < 2 ? sample : 0.0f;   // left and right; the rest silent
        switch (m_format)
        {
        case Format::UInt8:
            out.append(char(quint8(qBound(0, int(std::lround(s / 256.0f)) + 128, 255))));
            break;
        case Format::Int16:
        {
            const qint16 v = qint16(qBound(-32768, int(std::lround(s)), 32767));
            out.append(reinterpret_cast<const char*>(&v), sizeof v);
            break;
        }
        case Format::Int32:
        {
            const qint32 v = qint32(qBound(-32768, int(std::lround(s)), 32767)) * 65536;
            out.append(reinterpret_cast<const char*>(&v), sizeof v);
            break;
        }
        case Format::Float:
        {
            const float v = s / 32768.0f;
            out.append(reinterpret_cast<const char*>(&v), sizeof v);
            break;
        }
        }
    }
}

QByteArray PcmResampler::convert(const QByteArray& pcm16mono)
{
    const auto* p = reinterpret_cast<const uchar*>(pcm16mono.constData());
    for (int i = 0; i + 1 < pcm16mono.size(); i += 2)
        m_in << float(qFromLittleEndian<qint16>(p + i));
    QByteArray out;
    out.reserve(int(m_in.size() / m_step + 2) * bytesPerFrame());
    if (m_step <= 1.0)
    {
        // upwards: between two input samples, in proportion
        while (m_pos + 1.0 < m_in.size())
        {
            const int i = int(m_pos);
            const double frac = m_pos - i;
            appendFrame(out, float(m_in[i] * (1.0 - frac) + m_in[i + 1] * frac));
            m_pos += m_step;
        }
    }
    else
    {
        // downwards: the average of the input samples the output sample
        // covers, so that nothing above the new rate's half folds back
        while (m_pos + m_step <= m_in.size())
        {
            const int from = int(m_pos);
            const int to = qMax(from + 1, int(m_pos + m_step));
            double sum = 0.0;
            for (int i = from; i < to; ++i)
                sum += m_in[i];
            appendFrame(out, float(sum / (to - from)));
            m_pos += m_step;
        }
    }
    // what is used up goes; the next block continues from the same place
    const int used = int(m_pos);
    if (used > 0)
    {
        m_in.remove(0, qMin(used, int(m_in.size())));
        m_pos -= used;
    }
    return out;
}
