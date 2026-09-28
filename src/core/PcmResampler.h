// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QVector>

// Turns the receiver's audio (16-bit mono at its own rate, 12 kHz for a
// KiwiSDR) into a sound device's own format, for a device that takes
// nothing else: Windows without Media Foundation (the "N" editions) opens
// the sink only in the device's format, 48 kHz stereo float or the like.
// Linear interpolation upwards, an average of the samples covered
// downwards; carried on from one block to the next without a click. Mono
// goes to the first two channels, the others (centre, sub, surround) stay
// silent. Samples are written in the machine's own byte order, as Qt
// hands them to the device.
class PcmResampler
{
public:
    enum class Format { UInt8, Int16, Int32, Float };

    PcmResampler(int inRate, int outRate, int channels, Format format);
    QByteArray convert(const QByteArray& pcm16mono);
    int bytesPerFrame() const;   // one sample for every channel

private:
    void appendFrame(QByteArray& out, float sample) const;

    double m_step;            // input samples per output sample
    double m_pos = 0.0;       // the next output, counted from m_in[0]
    QVector<float> m_in;      // input not yet used up
    int m_channels;
    Format m_format;
};
