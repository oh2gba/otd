// SPDX-License-Identifier: GPL-3.0-or-later
#include "KiwiAudio.h"
#include "core/PcmQueue.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QTimer>

#include <optional>

Q_LOGGING_CATEGORY(lcPlayer, "otd.player")

namespace
{
QString sinkStateText(QAudio::State st, QAudio::Error err)
{
    if (err == QAudio::OpenError)
        return KiwiAudio::tr("sound device could not be opened");
    if (err == QAudio::IOError)
        return KiwiAudio::tr("sound device error");
    if (err == QAudio::FatalError)
        return KiwiAudio::tr("sound device failed");
    switch (st)
    {
    case QAudio::ActiveState:    return KiwiAudio::tr("playing");
    case QAudio::SuspendedState: return KiwiAudio::tr("sound suspended");
    case QAudio::StoppedState:   return KiwiAudio::tr("sound stopped");
    case QAudio::IdleState:      return KiwiAudio::tr("waiting for audio");
    }
    return QString();
}

// the converter's name for a device format, if it can write it
std::optional<PcmResampler::Format> resamplerFormat(const QAudioFormat& f)
{
    switch (f.sampleFormat())
    {
    case QAudioFormat::UInt8: return PcmResampler::Format::UInt8;
    case QAudioFormat::Int16: return PcmResampler::Format::Int16;
    case QAudioFormat::Int32: return PcmResampler::Format::Int32;
    case QAudioFormat::Float: return PcmResampler::Format::Float;
    default: return std::nullopt;
    }
}
} // namespace

KiwiAudio::KiwiAudio(QObject* parent)
    : QObject(parent)
{
    // the amount in hand, told a few times a second while playing
    m_bufferTick = new QTimer(this);
    m_bufferTick->setInterval(200);
    connect(m_bufferTick, &QTimer::timeout, this, [this]() { emit audioInHandChanged(audioInHand()); });

    connect(&m_client, &KiwiClient::stateChanged, this, &KiwiAudio::statusChanged);
    connect(&m_client, &KiwiClient::audio, this, &KiwiAudio::onAudio);
    connect(&m_client, &KiwiClient::closed, this, &KiwiAudio::onClosed);
    connect(&m_client, &KiwiClient::sMeter, this, &KiwiAudio::sMeter);
}

KiwiAudio::~KiwiAudio()
{
    m_client.close();
    if (m_sink)
        m_sink->disconnect(this);
    delete m_sink;
}

void KiwiAudio::tune(double kHz, const QString& mode)
{
    m_client.tune(kHz, mode);
}

void KiwiAudio::play(const QString& address)
{
    qCDebug(lcPlayer) << "play" << address << "volume" << m_volume;
    m_client.open(address);
}

void KiwiAudio::stop()
{
    stopWith(QString());
}

// The one way playing ends: the session and the sound go back to rest, and
// the status says why (empty: stopped on request).
void KiwiAudio::stopWith(const QString& status)
{
    qCDebug(lcPlayer) << "stop:" << (status.isEmpty() ? QStringLiteral("(on request)") : status) << "frames" << m_audioFrames;
    m_client.close();   // emits nothing, so the status below stays
    ++m_sessionsEnded;
    if (m_sink)
    {
        m_sink->disconnect(this);   // no state texts from a sink on its way out
        m_sink->stop();
    }
    delete m_sink;
    m_sink = nullptr;
    delete m_queue;
    m_queue = nullptr;
    m_resampler.reset();
    m_sinkRate = 0;
    m_bufferTick->stop();
    emit audioInHandChanged(0.0);   // not playing: nothing in hand
    emit stopped();
    emit statusChanged(status);
}

void KiwiAudio::setVolume(int percent)
{
    m_volume = qBound(0, percent, 100);
    if (m_sink)
        m_sink->setVolume(m_volume / 100.0);
}

void KiwiAudio::onAudio(const QByteArray& pcm)
{
    const int rate = m_client.sampleRate();
    if (!m_sink || m_sinkRate != rate)
    {
        if (m_sink)
            m_sink->disconnect(this);
        delete m_sink;
        m_sink = nullptr;
        delete m_queue;
        m_queue = nullptr;
        m_resampler.reset();
        const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
        if (dev.isNull())
        {
            stopWith(tr("No sound output device"));
            return;
        }
        // First the receiver's own format: Linux, macOS and a Windows with
        // Media Foundation convert it to the device's themselves. A Windows
        // without (the "N" editions) opens the sink only in the device's own
        // format, so then that is opened and the audio converted here.
        QAudioFormat fmt;
        fmt.setSampleRate(rate);
        fmt.setChannelCount(1);
        fmt.setSampleFormat(QAudioFormat::Int16);
        m_audioFrames = 0;
        qCDebug(lcPlayer) << "sound device" << dev.description() << "preferred" << dev.preferredFormat()
                          << "takes 12 kHz mono per Qt:" << dev.isFormatSupported(fmt);
        if (!openSink(dev, fmt))
        {
            const QAudioFormat own = dev.preferredFormat();
            qCDebug(lcPlayer) << "receiver format refused; trying the device's own" << own;
            const std::optional<PcmResampler::Format> sample = resamplerFormat(own);
            if (!own.isValid() || !sample || own == fmt || !openSink(dev, own))
            {
                stopWith(tr("Stopped: sound device %1 takes neither %2 Hz mono nor its own format")
                             .arg(dev.description()).arg(rate));
                return;
            }
            m_resampler = std::make_unique<PcmResampler>(rate, own.sampleRate(), own.channelCount(), *sample);
            qCDebug(lcPlayer) << "converting" << rate << "Hz mono ->" << own;
        }
        m_sinkRate = rate;
    }
    const QByteArray out = m_resampler ? m_resampler->convert(pcm) : pcm;
    m_queue->push(out);
    if (m_audioFrames++ % 100 == 0)
        qCDebug(lcPlayer) << "audio frame" << m_audioFrames << "in" << pcm.size() << "out" << out.size()
                          << "queued" << m_queue->buffered() << "sink state" << m_sink->state() << "error" << m_sink->error()
                          << "played us" << m_sink->processedUSecs() << "free" << m_sink->bytesFree()
                          << "in hand s" << audioInHand();
}

bool KiwiAudio::openSink(const QAudioDevice& dev, const QAudioFormat& fmt)
{
    const int bytesPerSecond = fmt.bytesForDuration(1000000);
    m_queue = new PcmQueue(bytesPerSecond, fmt.sampleFormat() == QAudioFormat::UInt8 ? char(0x80) : char(0), this);
    m_sink = new QAudioSink(dev, fmt, this);
    m_sink->setBufferSize(bytesPerSecond / 2);   // half a second in the device
    m_sink->setVolume(m_volume / 100.0);
    connect(m_sink, &QAudioSink::stateChanged, this, [this](QAudio::State st) {
        onSinkState(st, m_sink->error());
    });
    m_opening = true;
    m_sink->start(m_queue);   // pull mode: the sink asks, the queue answers
    m_opening = false;
    qCDebug(lcPlayer) << "sink" << fmt << "started: error" << m_sink->error() << "state" << m_sink->state()
                      << "buffer" << m_sink->bufferSize();
    if (m_sink->error() == QAudio::NoError)
    {
        m_sinkBytesPerSecond = bytesPerSecond;
        m_bufferTick->start();
        return true;
    }
    // refused right away (Qt says so without a state change)
    m_sink->disconnect(this);
    delete m_sink;
    m_sink = nullptr;
    delete m_queue;
    m_queue = nullptr;
    return false;
}

double KiwiAudio::audioInHand() const
{
    if (!m_sink || !m_queue || m_sinkBytesPerSecond <= 0)
        return 0.0;
    // what waits in the queue, and what the device holds already
    const qint64 held = qMax<qint64>(0, m_sink->bufferSize() - m_sink->bytesFree());
    return double(m_queue->buffered() + held) / m_sinkBytesPerSecond;
}

void KiwiAudio::onSinkState(QAudio::State state, QAudio::Error error)
{
    qCDebug(lcPlayer) << "sink state" << state << "error" << error << "queued" << (m_queue ? m_queue->buffered() : -1);
    const QString t = sinkStateText(state, error);
    const bool failed = state == QAudio::StoppedState
                        && (error == QAudio::OpenError || error == QAudio::IOError || error == QAudio::FatalError);
    if (failed && m_opening)
        return;   // a refused start is handled by openSink itself
    if (!failed)
    {
        if (!t.isEmpty())
            emit statusChanged(tr("%1: %2").arg(m_client.receiver().host(), t));
        return;
    }
    // The sound is gone, so the session ends too. Not from here: this runs
    // inside the sink's own signal and stopWith deletes the sink. If the
    // session has ended some other way meanwhile, there is nothing to do.
    const int session = m_sessionsEnded;
    QMetaObject::invokeMethod(this, [this, session, t]() {
        if (session == m_sessionsEnded)
            stopWith(tr("Stopped: %1").arg(t));
    }, Qt::QueuedConnection);
}

void KiwiAudio::onClosed(const QString& reason)
{
    stopWith(tr("Stopped: %1").arg(reason));
}
