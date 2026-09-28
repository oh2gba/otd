// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QIODevice>

// Between the receiver and the sound device. The receiver's audio arrives
// in bursts over the network and the sink pulls at its own steady pace;
// this queue sits between them. It offers nothing until a little has
// collected (so the sound does not start and stop), then whatever it has,
// silence for the rest: the device's own buffer rides out the gaps between
// bursts. After a long stall it collects afresh. When the network runs
// far ahead the oldest is dropped. Once primed, each push says so with
// readyRead(): a sink that found nothing waits for that signal.
class PcmQueue : public QIODevice
{
    Q_OBJECT
public:
    // silence: the byte that is quiet in the device's format (0x80 for
    // unsigned 8-bit samples, 0 otherwise)
    explicit PcmQueue(int bytesPerSecond, char silence = 0, QObject* parent = nullptr);
    void push(const QByteArray& pcm);
    int buffered() const { return m_buf.size(); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;

protected:
    qint64 readData(char* data, qint64 maxlen) override;
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    QByteArray m_buf;
    const int m_preroll;   // 0.3 s before the first sound
    const int m_max;       // never more than 3 s behind
    const char m_silence;
    bool m_primed = false;
    QElapsedTimer m_emptySince;   // runs while the queue is empty
};
