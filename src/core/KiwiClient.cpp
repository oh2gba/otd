// SPDX-License-Identifier: GPL-3.0-or-later
#include "KiwiClient.h"

#include <QDateTime>
#include <QWebSocket>
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

// Without a scheme the address is taken as http://, so "host:port" and
// "IP:port" work: QUrl alone would read "localhost:8073" as a scheme and
// reject "192.168.1.50:8073".
QUrl KiwiClient::receiverUrl(const QString& receiver)
{
    QString text = receiver.trimmed();
    if (!text.contains(QLatin1String("://")))
        text.prepend(QStringLiteral("http://"));
    return QUrl(text);
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
    m_keepalive.setInterval(1000);
    connect(&m_keepalive, &QTimer::timeout, this, [this]() {
        if (m_open)
            send(QStringLiteral("SET keepalive"));
    });
    // A host can take the connection and then say nothing, which would
    // leave the caller on "Connecting ..." for ever. The timer runs from
    // open() until the receiver's greeting.
    m_connectTimer.setSingleShot(true);
    m_connectTimer.setInterval(30000);
    connect(&m_connectTimer, &QTimer::timeout, this, [this]() {
        fail(tr("the receiver did not answer"));
    });
}

KiwiClient::~KiwiClient()
{
    dropSocket();
}

QUrl KiwiClient::webSocketUrl(const QString& receiver, qint64 stamp)
{
    const QUrl url = receiverUrl(receiver);
    const bool secure = url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0;
    // Without a port: 8073, the KiwiSDR default, which is also where the
    // proxy.kiwisdr.com hosts (listed without a port) serve the WebSocket;
    // their port 80 only redirects there, and QWebSocket does not follow
    // redirects. An https address without a port is taken to sit behind a
    // TLS front end on 443.
    int port = url.port();
    if (port <= 0)
        port = secure ? 443 : 8073;
    // ws://host:port/kiwi/<timestamp>/SND, as the browser page does
    QUrl ws;
    ws.setScheme(secure ? QStringLiteral("wss") : QStringLiteral("ws"));
    ws.setHost(url.host());
    ws.setPort(port);
    ws.setPath(QStringLiteral("/kiwi/%1/SND").arg(stamp & 0xffffffff));
    return ws;
}

void KiwiClient::dropSocket()
{
    if (!m_ws)
        return;
    QWebSocket* ws = m_ws;
    m_ws = nullptr;
    ws->disconnect(this);   // whatever it still reports is not for us
    ws->abort();
    ws->deleteLater();
}

void KiwiClient::open(const QString& receiver, const QString& password)
{
    close();
    const int session = ++m_session;
    m_receiver = receiverUrl(receiver);
    m_password = password;
    m_pendingReason.clear();
    m_ready = false;
    m_adpcm = Adpcm();
    m_maxKHz = 30000.0;
    m_offsetKHz = 0.0;
    m_open = true;

    if (!m_receiver.isValid() || m_receiver.host().isEmpty())
    {
        const QString text = receiver.trimmed();
        const QString why = text.isEmpty() ? tr("no receiver address given")
                                           : tr("\"%1\" is not a receiver address").arg(text);
        m_receiver = QUrl();
        // Reported once open() has returned, like any other failure, so a
        // caller that connects to closed() after open() still hears it. A
        // later open() or close() makes it moot.
        QTimer::singleShot(0, this, [this, session, why]() {
            if (session == m_session)
                fail(why);
        });
        return;
    }

    m_ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_ws, &QWebSocket::connected, this, &KiwiClient::onConnected);
    connect(m_ws, &QWebSocket::disconnected, this, &KiwiClient::onDisconnected);
    connect(m_ws, &QWebSocket::textMessageReceived, this, &KiwiClient::onText);
    connect(m_ws, &QWebSocket::binaryMessageReceived, this, &KiwiClient::onBinary);
    connect(m_ws, &QWebSocket::errorOccurred, this, &KiwiClient::onSocketError);
    emit stateChanged(tr("Connecting to %1 ...").arg(m_receiver.host()));
    m_connectTimer.start();
    m_ws->open(webSocketUrl(receiver, QDateTime::currentSecsSinceEpoch()));
}

void KiwiClient::close()
{
    m_open = false;
    m_ready = false;
    m_keepalive.stop();
    m_connectTimer.stop();
    dropSocket();
}

void KiwiClient::setConnectTimeout(int ms)
{
    m_connectTimer.setInterval(ms);
}

void KiwiClient::fail(const QString& reason)
{
    if (!m_open)
        return;   // already over, or closed on request: nothing to report
    close();
    emit closed(reason.isEmpty() ? tr("connection closed by the receiver") : reason);
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
    if (m_ws && m_ws->state() == QAbstractSocket::ConnectedState)
        m_ws->sendTextMessage(command);
}

void KiwiClient::onConnected()
{
    // the password is empty for public receivers
    send(QStringLiteral("SET auth t=kiwi p=%1").arg(m_password));
    m_keepalive.start();
    emit stateChanged(tr("Connected to %1, waiting for audio ...").arg(m_receiver.host()));
}

QString KiwiClient::endReason() const
{
    // what the receiver said before it hung up comes first
    if (!m_pendingReason.isEmpty())
        return m_pendingReason;
    // then the socket's own error, such as a refused connection; a plain
    // hang-up is left to fail()'s general text. ConnectionRefusedError is
    // 0, so "no error" is UnknownSocketError, not 0.
    if (m_ws && m_ws->error() != QAbstractSocket::UnknownSocketError
        && m_ws->error() != QAbstractSocket::RemoteHostClosedError)
        return m_ws->errorString();
    return QString();
}

void KiwiClient::onDisconnected()
{
    // The receiver hung up, or the connection never came about: Qt reports
    // a refused connection with disconnected() before errorOccurred(), and
    // fail() stops listening to the socket, so the error is read here.
    fail(endReason());
}

void KiwiClient::onSocketError(QAbstractSocket::SocketError error)
{
    // A refused or broken connection does not always end in disconnected().
    // The error handed over here counts even when the socket's own error()
    // says nothing, as after a failed WebSocket handshake (an HTTP 404 or a
    // redirect), where only errorString() tells what happened.
    QString why = m_pendingReason;
    if (why.isEmpty() && error != QAbstractSocket::RemoteHostClosedError && m_ws)
        why = m_ws->errorString();
    fail(why);
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
            m_connectTimer.stop();   // it answered: the session is under way
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
            fail(tr("all %1 channels of this receiver are in use").arg(value));
            return;
        }
        else if (name == QLatin1String("badp") && value == QLatin1String("1"))
        {
            // the receiver says this both for a wrong password and when
            // all channels open without a password are taken
            fail(tr("the receiver is full, or needs a password"));
            return;
        }
        else if (name == QLatin1String("badp") && value == QLatin1String("5"))
        {
            fail(tr("the receiver allows one connection per address, and one is open already"));
            return;
        }
        else if (name == QLatin1String("down"))
        {
            fail(tr("the receiver is down"));
            return;
        }
        else if (name == QLatin1String("inactivity_timeout") || name == QLatin1String("time_limit"))
        {
            // the receiver hangs up right after this
            m_pendingReason = tr("the receiver's time limit is up");
        }
        else if (name == QLatin1String("redirect"))
        {
            fail(tr("the receiver redirects to %1").arg(value));
            return;
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
