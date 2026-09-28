// SPDX-License-Identifier: GPL-3.0-or-later
#include "RigClient.h"

// The plain rigctl protocol is used on purpose: besides Hamlib's rigctld it
// is spoken by gqrx (port 7356), SDR++ ("Rigctl Server") and others, which
// do not all implement the extended "+f" response format.
//   f          -> "7125940"
//   m          -> "LSB" / "3000"
//   F 7125940  -> "RPRT 0"
//   M USB 0    -> "RPRT 0"

RigClient::RigClient(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QTcpSocket::connected, this, &RigClient::onConnected);
    connect(&m_socket, &QTcpSocket::disconnected, this, &RigClient::onDisconnected);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, &RigClient::onError);
    connect(&m_socket, &QTcpSocket::readyRead, this, &RigClient::onReadyRead);

    m_pollTimer.setInterval(500);
    connect(&m_pollTimer, &QTimer::timeout, this, &RigClient::poll);

    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(3000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &RigClient::connectNow);

    m_silenceTimer.setSingleShot(true);
    connect(&m_silenceTimer, &QTimer::timeout, this, &RigClient::onSilence);
}

RigClient::~RigClient()
{
    // The socket outlives the timers (members go in reverse order) and says
    // "disconnected" as it closes; nothing here may run on the way out.
    m_socket.disconnect(this);
}

void RigClient::setSilenceTimeout(int ms)
{
    m_silenceTimeoutMs = qMax(0, ms);
    if (m_silenceTimer.isActive())
        armSilenceTimer();
}

void RigClient::armSilenceTimer()
{
    const int ms = m_silenceTimeoutMs > 0 ? m_silenceTimeoutMs : qMax(5000, 3 * m_pollTimer.interval());
    m_silenceTimer.start(ms);
}

void RigClient::onSilence()
{
    if (!m_enabled || m_answer == Answer::No)
        return;
    m_answer = Answer::No;
    // forget what the rig said last, so that the first answer after the
    // silence is reported even when the dial has not moved
    m_frequencyHz = 0;
    m_mode.clear();
    emit answeringChanged(false);
}

void RigClient::setEndpoint(const QString& host, quint16 port)
{
    if (host == m_host && port == m_port)
        return;
    m_host = host;
    m_port = port;
    if (m_enabled)
    {
        // The new address gets its own verdict: forget a "not answering" so
        // that it is said again if the new one is silent too. A rig that
        // answered stays answering until the watchdog says otherwise, so the
        // warning does not flash while the new one is reached.
        if (m_answer == Answer::No)
            m_answer = Answer::Unknown;
        armSilenceTimer();
        m_socket.abort();
        m_reconnectTimer.stop();
        connectNow();
    }
}

void RigClient::setPollInterval(int ms)
{
    ms = qBound(100, ms, 60000);
    if (ms == m_pollTimer.interval())
        return;
    m_pollTimer.setInterval(ms);
    // The default silence timeout follows the interval. Re-arm it now, or a
    // longer interval trips the watchdog between two good polls.
    if (m_silenceTimer.isActive())
        armSilenceTimer();
}

void RigClient::start()
{
    m_enabled = true;
    m_answer = Answer::Unknown;
    armSilenceTimer();
    connectNow();
}

void RigClient::stop()
{
    m_enabled = false;
    m_pollTimer.stop();
    m_reconnectTimer.stop();
    m_silenceTimer.stop();
    m_socket.abort();
    m_expect.clear();
    const bool was = m_answer == Answer::Yes;
    m_answer = Answer::Unknown;
    m_frequencyHz = 0;
    m_mode.clear();
    emit stateChanged(false, tr("Rig polling disabled"));
    if (was)
        emit answeringChanged(false);
}

void RigClient::reconnectSoon()
{
    m_socket.abort();
    m_expect.clear();
    if (m_enabled)
    {
        m_reconnectTimer.stop();
        m_reconnectTimer.start(500);
    }
}

void RigClient::setFrequency(qint64 hz)
{
    if (!isConnected() || hz <= 0)
        return;
    m_socket.write(QByteArray("F ") + QByteArray::number(hz) + '\n');
    m_expect.enqueue(ExReport);
}

void RigClient::setMode(const QString& mode)
{
    if (!isConnected() || mode.isEmpty())
        return;
    m_socket.write(QByteArray("M ") + mode.toLatin1() + " 0\n");
    m_expect.enqueue(ExReport);
}

void RigClient::connectNow()
{
    if (!m_enabled || m_socket.state() != QAbstractSocket::UnconnectedState)
        return;
    emit stateChanged(false, tr("Connecting to %1:%2 ...").arg(m_host).arg(m_port));
    m_buffer.clear();
    m_expect.clear();
    m_missedPolls = 0;
    m_socket.connectToHost(m_host, m_port);
}

void RigClient::onConnected()
{
    emit stateChanged(true, tr("Connected to rigctld at %1:%2").arg(m_host).arg(m_port));
    m_pollTimer.start();
    poll();
}

void RigClient::onDisconnected()
{
    m_pollTimer.stop();
    m_expect.clear();
    emit stateChanged(false, tr("Disconnected from rigctld"));
    scheduleReconnect();
}

void RigClient::onError(QAbstractSocket::SocketError)
{
    m_pollTimer.stop();
    m_expect.clear();
    emit stateChanged(false, tr("rigctld %1:%2: %3").arg(m_host).arg(m_port, 0)
                                                     .arg(m_socket.errorString()));
    if (m_socket.state() != QAbstractSocket::UnconnectedState)
        m_socket.abort();
    scheduleReconnect();
}

void RigClient::scheduleReconnect()
{
    if (m_enabled && !m_reconnectTimer.isActive())
        m_reconnectTimer.start();
}

void RigClient::poll()
{
    if (!isConnected())
        return;
    if (!m_expect.isEmpty())
    {
        // The other side did not answer the previous poll; give it a few
        // more intervals, then drop the connection so the reconnect logic
        // kicks in.
        if (++m_missedPolls >= 6)
        {
            emit stateChanged(false, tr("rigctld is not responding, reconnecting"));
            m_socket.abort();
            scheduleReconnect();
        }
        return;
    }
    m_missedPolls = 0;
    m_socket.write("f\nm\n");
    m_expect.enqueue(ExFreq);
    m_expect.enqueue(ExMode);
    m_expect.enqueue(ExPassband);
}

void RigClient::onReadyRead()
{
    m_buffer += m_socket.readAll();
    int idx;
    while ((idx = m_buffer.indexOf('\n')) >= 0)
    {
        const QByteArray line = m_buffer.left(idx).trimmed();
        m_buffer.remove(0, idx + 1);
        if (!line.isEmpty())
            handleLine(line);
    }
}

void RigClient::handleLine(const QByteArray& line)
{
    if (line.startsWith("RPRT"))
    {
        const int rc = line.mid(4).trimmed().toInt();
        if (!m_expect.isEmpty())
        {
            // An error report replaces the answer of the command in flight.
            const Expect front = m_expect.dequeue();
            if (front == ExMode && !m_expect.isEmpty() && m_expect.head() == ExPassband)
                m_expect.dequeue();
        }
        if (rc != 0)
            emit stateChanged(true, tr("rigctld reported error %1").arg(rc));
        return;
    }
    if (m_expect.isEmpty())
        return;   // unsolicited output, e.g. a banner

    switch (m_expect.dequeue())
    {
    case ExFreq:
    {
        bool ok = false;
        const qint64 hz = QString::fromLatin1(line).toDouble(&ok);
        if (!ok || hz <= 0)
            break;
        // every valid answer keeps the rig alive, changed or not
        armSilenceTimer();
        if (m_answer != Answer::Yes)
        {
            m_answer = Answer::Yes;
            emit answeringChanged(true);
        }
        if (hz != m_frequencyHz)
        {
            m_frequencyHz = hz;
            emit frequencyChanged(hz);
        }
        break;
    }
    case ExMode:
    {
        const QString mode = QString::fromLatin1(line);
        if (mode != m_mode)
        {
            m_mode = mode;
            emit modeChanged(m_mode, m_passband);
        }
        break;
    }
    case ExPassband:
        m_passband = line.toInt();
        break;
    case ExReport:
        break;
    }
}
