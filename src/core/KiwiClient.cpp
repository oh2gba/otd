// SPDX-License-Identifier: GPL-3.0-or-later
#include "KiwiClient.h"

#include <QDateTime>
#include <QtEndian>

#include <algorithm>

namespace
{
const int kStepSize[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int kIndexAdjust[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

constexpr quint8 kFlagCompressed = 0x10;
constexpr quint8 kFlagStereo = 0x08;
constexpr quint8 kFlagLittleEndian = 0x80;
}

QByteArray KiwiClient::Adpcm::decode(const QByteArray& nibbles)
{
    QByteArray out;
    out.resize(nibbles.size() * 4);
    char* w = out.data();
    auto sample = [this](int code) {
        const int step = kStepSize[index];
        index = std::clamp(index + kIndexAdjust[code], 0, 88);
        int diff = step >> 3;
        if (code & 1) diff += step >> 2;
        if (code & 2) diff += step >> 1;
        if (code & 4) diff += step;
        if (code & 8) diff = -diff;
        prev = std::clamp(prev + diff, -32768, 32767);
        return qint16(prev);
    };
    for (unsigned char b : nibbles)
    {
        qToLittleEndian<qint16>(sample(b & 0x0f), w);
        w += 2;
        qToLittleEndian<qint16>(sample(b >> 4), w);
        w += 2;
    }
    return out;
}

KiwiClient::KiwiClient(QObject* parent)
    : QObject(parent)
{
    connect(&m_ws, &QWebSocket::connected, this, &KiwiClient::onConnected);
    connect(&m_ws, &QWebSocket::disconnected, this, &KiwiClient::onDisconnected);
    connect(&m_ws, &QWebSocket::textMessageReceived, this, &KiwiClient::onText);
    connect(&m_ws, &QWebSocket::binaryMessageReceived, this, &KiwiClient::onBinary);
    connect(&m_ws, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        m_closeReason = m_ws.errorString();
    });
    m_keepalive.setInterval(1000);
    connect(&m_keepalive, &QTimer::timeout, this, [this]() {
        if (m_open)
            send(QStringLiteral("SET keepalive"));
    });
}

void KiwiClient::open(const QUrl& receiver, const QString& password)
{
    close();
    QUrl url = receiver;
    if (url.scheme().isEmpty())
        url = QUrl(QStringLiteral("http://") + receiver.toString());
    m_receiver = url;
    m_password = password;
    m_closeReason.clear();
    m_ready = false;
    m_adpcm = Adpcm();

    // ws://host:port/kiwi/<timestamp>/SND, as the browser page does
    QUrl ws;
    ws.setScheme(url.scheme() == QLatin1String("https") ? QStringLiteral("wss") : QStringLiteral("ws"));
    ws.setHost(url.host());
    ws.setPort(url.port(8073));
    ws.setPath(QStringLiteral("/kiwi/%1/SND").arg(QDateTime::currentSecsSinceEpoch() & 0xffffffff));
    m_open = true;
    emit stateChanged(tr("Connecting to %1 ...").arg(url.host()));
    m_ws.open(ws);
}

void KiwiClient::close()
{
    if (!m_open)
        return;
    m_open = false;
    m_ready = false;
    m_keepalive.stop();
    m_closeReason.clear();
    m_ws.close();
}

void KiwiClient::tune(double kHz, const QString& mode)
{
    m_kHz = kHz;
    m_mode = mode.toUpper();
    if (m_ready)
        applyTuning();
}

void KiwiClient::send(const QString& command)
{
    if (m_ws.state() == QAbstractSocket::ConnectedState)
        m_ws.sendTextMessage(command);
}

void KiwiClient::onConnected()
{
    // the password is empty for public receivers
    send(QStringLiteral("SET auth t=kiwi p=%1").arg(m_password));
    m_keepalive.start();
    emit stateChanged(tr("Connected to %1, waiting for audio ...").arg(m_receiver.host()));
}

void KiwiClient::onDisconnected()
{
    const bool wanted = !m_open;
    m_open = false;
    m_ready = false;
    m_keepalive.stop();
    emit closed(wanted ? QString() : (m_closeReason.isEmpty() ? tr("connection closed by the receiver")
                                                              : m_closeReason));
}

void KiwiClient::onText(const QString& text)
{
    if (text.startsWith(QLatin1String("MSG ")))
        handleMsg(text.mid(4));
}

void KiwiClient::onBinary(const QByteArray& frame)
{
    if (frame.size() < 4)
        return;
    const QByteArray tag = frame.left(3);
    if (tag == "MSG")
        handleMsg(QString::fromUtf8(frame.mid(4)));   // "MSG " then the body
    else if (tag == "SND")
        handleSnd(frame.mid(3));
}

void KiwiClient::handleMsg(const QString& body)
{
    const QStringList items = body.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& item : items)
    {
        const int eq = item.indexOf(QLatin1Char('='));
        const QString name = eq < 0 ? item : item.left(eq);
        const QString value = eq < 0 ? QString() : QUrl::fromPercentEncoding(item.mid(eq + 1).toUtf8());

        if (name == QLatin1String("audio_rate"))
        {
            m_rate = value.toInt() > 0 ? value.toInt() : 12000;
            send(QStringLiteral("SET AR OK in=%1 out=44100").arg(m_rate));
        }
        else if (name == QLatin1String("sample_rate"))
        {
            // the receiver is ours now: set it up and start the sound
            send(QStringLiteral("SET squelch=0 max=0"));
            send(QStringLiteral("SET genattn=0"));
            send(QStringLiteral("SET gen=0 mix=-1"));
            send(QStringLiteral("SET agc=1 hang=0 thresh=-100 slope=6 decay=1000 manGain=50"));
            send(QStringLiteral("SET compression=1"));
            send(QStringLiteral("SET ident_user=otd"));
            m_ready = true;
            applyTuning();
            send(QStringLiteral("SET keepalive"));
            emit stateChanged(tr("Listening on %1").arg(m_receiver.host()));
        }
        else if (name == QLatin1String("bandwidth"))
            m_maxKHz = value.toDouble() / 1000.0;
        else if (name == QLatin1String("freq_offset"))
            m_offsetKHz = value.toDouble();
        else if (name == QLatin1String("audio_adpcm_state"))
        {
            const QStringList v = value.split(QLatin1Char(','));
            if (v.size() == 2)
            {
                m_adpcm.index = std::clamp(v[0].toInt(), 0, 88);
                m_adpcm.prev = std::clamp(v[1].toInt(), -32768, 32767);
            }
        }
        else if (name == QLatin1String("too_busy"))
        {
            m_closeReason = tr("all %1 channels of this receiver are in use").arg(value);
            close();
            emit closed(m_closeReason);
        }
        else if (name == QLatin1String("badp") && value == QLatin1String("1"))
        {
            m_closeReason = tr("this receiver needs a password");
            close();
            emit closed(m_closeReason);
        }
        else if (name == QLatin1String("badp") && value == QLatin1String("5"))
        {
            m_closeReason = tr("the receiver allows one connection per address, and one is open already");
            close();
            emit closed(m_closeReason);
        }
        else if (name == QLatin1String("down"))
        {
            m_closeReason = tr("the receiver is down");
            close();
            emit closed(m_closeReason);
        }
        else if (name == QLatin1String("inactivity_timeout") || name == QLatin1String("time_limit"))
        {
            m_closeReason = tr("the receiver's time limit is up");
        }
        else if (name == QLatin1String("redirect"))
        {
            m_closeReason = tr("the receiver redirects to %1").arg(value);
            close();
            emit closed(m_closeReason);
        }
    }
}

void KiwiClient::handleSnd(const QByteArray& body)
{
    if (body.size() < 7)
        return;
    const quint8 flags = quint8(body[0]);
    const quint16 smeter = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(body.constData() + 5));
    emit sMeter(0.1 * smeter - 127.0);
    QByteArray data = body.mid(7);
    if (flags & kFlagStereo)
        return;   // IQ and DRM modes are never asked for
    QByteArray pcm;
    if (flags & kFlagCompressed)
        pcm = m_adpcm.decode(data);
    else
    {
        // plain 16-bit samples, big-endian unless flagged
        pcm.resize(data.size() & ~1);
        const bool le = flags & kFlagLittleEndian;
        for (int i = 0; i + 1 < data.size(); i += 2)
        {
            if (le)
            {
                pcm[i] = data[i];
                pcm[i + 1] = data[i + 1];
            }
            else
            {
                pcm[i] = data[i + 1];
                pcm[i + 1] = data[i];
            }
        }
    }
    if (!pcm.isEmpty())
        emit audio(pcm);
}

void KiwiClient::applyTuning()
{
    if (m_kHz <= 0.0)
        return;
    const double base = m_kHz - m_offsetKHz;
    if (base < 0.0 || base > m_maxKHz)
    {
        emit stateChanged(tr("%1 kHz is outside this receiver's range").arg(m_kHz, 0, 'f', 3));
        return;
    }
    QString mod = QStringLiteral("am");
    int low = -4900, high = 4900;
    if (m_mode == QLatin1String("USB"))       { mod = QStringLiteral("usb");  low = 300;   high = 2700; }
    else if (m_mode == QLatin1String("LSB"))  { mod = QStringLiteral("lsb");  low = -2700; high = -300; }
    else if (m_mode == QLatin1String("CW"))   { mod = QStringLiteral("cw");   low = 300;   high = 700; }
    else if (m_mode == QLatin1String("NFM") || m_mode == QLatin1String("FM"))
                                              { mod = QStringLiteral("nbfm"); low = -6000; high = 6000; }
    send(QStringLiteral("SET mod=%1 low_cut=%2 high_cut=%3 freq=%4")
             .arg(mod).arg(low).arg(high).arg(base, 0, 'f', 3));
}
