// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/KiwiClient.h"
#include "core/PcmResampler.h"

#include <QAudio>
#include <QLoggingCategory>
#include <QObject>
#include <memory>

class PcmQueue;
class QAudioSink;
class QTimer;

Q_DECLARE_LOGGING_CATEGORY(lcPlayer)

// One listening session on a KiwiSDR, without widgets: the connection, the
// sound device it plays into (in the receiver's format, or converted to the
// device's own when it takes nothing else), the volume and the amount of
// audio in hand. The player's row of controls sits on top of this.
class KiwiAudio : public QObject
{
    Q_OBJECT
public:
    explicit KiwiAudio(QObject* parent = nullptr);
    ~KiwiAudio() override;

    bool isPlaying() const { return m_client.isOpen(); }
    QUrl receiver() const { return m_client.receiver(); }   // the one being heard
    // what the receiver demodulates; before play() the values it starts with
    void tune(double kHz, const QString& mode);
    void play(const QString& address);   // starts the session; the status says how it goes
    void stop();                          // on request: no status left behind
    int volume() const { return m_volume; }   // 0..100
    void setVolume(int percent);
    // seconds of audio waiting to be heard (otd's queue and the sound
    // device's buffer); 0 when not playing
    double audioInHand() const;
    // What the sound device reports; a failure ends the session. Public for
    // the unit test, which has no sound device that could fail.
    void onSinkState(QAudio::State state, QAudio::Error error);

signals:
    void statusChanged(const QString& text);   // what the session is doing, empty when idle
    void stopped();                            // the session ended, on request or otherwise
    void sMeter(double dBm);
    // seconds of audio in hand, a few times a second while playing, and 0
    // once playing has ended (isPlaying() tells the two apart)
    void audioInHandChanged(double seconds);

private:
    void onAudio(const QByteArray& pcm);
    void onClosed(const QString& reason);
    void stopWith(const QString& status);
    // opens the sink and the queue in this format; false (and nothing left
    // behind) when the device refuses it
    bool openSink(const class QAudioDevice& dev, const class QAudioFormat& fmt);

    KiwiClient m_client;
    QAudioSink* m_sink = nullptr;
    PcmQueue* m_queue = nullptr;
    std::unique_ptr<PcmResampler> m_resampler;   // when the device takes only its own format
    QTimer* m_bufferTick = nullptr;
    int m_volume = 70;
    int m_sinkBytesPerSecond = 0;
    int m_sinkRate = 0;
    bool m_opening = false;      // inside QAudioSink::start()
    quint64 m_audioFrames = 0;   // for the log
    int m_sessionsEnded = 0;     // a late failure report for an ended session is dropped
};
