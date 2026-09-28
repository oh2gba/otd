// SPDX-License-Identifier: GPL-3.0-or-later
// KiwiClient: the ADPCM decoder, the WebSocket address rule, and whole
// sessions against a small fake KiwiSDR on localhost that speaks the same
// messages a real receiver sends.
#include "core/KiwiClient.h"

#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtEndian>
#include <QtTest>

namespace
{
// A fake receiver. Each connection is scripted by the test through the
// hooks; everything the client sends is recorded.
class FakeKiwi : public QObject
{
public:
    QStringList received;              // text commands from the client
    QList<QWebSocket*> peers;
    QString lastPath;                  // the path the client asked for
    bool greet = true;                 // answer auth with audio_rate/sample_rate
    QByteArray msgOnAuth;              // extra MSG body sent right after auth

    FakeKiwi()
        : server(QStringLiteral("fake kiwi"), QWebSocketServer::NonSecureMode)
    {
        connect(&server, &QWebSocketServer::newConnection, this, [this]() {
            while (QWebSocket* ws = server.nextPendingConnection())
            {
                peers << ws;
                lastPath = ws->requestUrl().path();
                connect(ws, &QWebSocket::textMessageReceived, this, [this, ws](const QString& t) {
                    received << t;
                    if (t.startsWith(QLatin1String("SET auth")))
                    {
                        if (!msgOnAuth.isEmpty())
                            sendMsg(ws, msgOnAuth);
                        if (greet)
                        {
                            sendMsg(ws, "audio_rate=12000");
                            sendMsg(ws, "sample_rate=12000.000");
                        }
                    }
                });
            }
        });
        server.listen(QHostAddress::LocalHost);
    }
    QString address() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
    static void sendMsg(QWebSocket* ws, const QByteArray& body)
    {
        ws->sendBinaryMessage(QByteArray("MSG ") + body);
    }
    // an SND frame: flags, sequence, S-meter (big endian, 0.1 dB + 127), data
    static void sendSnd(QWebSocket* ws, quint8 flags, quint16 smeter, const QByteArray& data)
    {
        QByteArray f("SND");
        f.append(char(flags));
        f.append(QByteArray(4, '\0'));
        uchar sm[2];
        qToBigEndian<quint16>(smeter, sm);
        f.append(reinterpret_cast<const char*>(sm), 2);
        f.append(data);
        ws->sendBinaryMessage(f);
    }
    int count(const QString& prefix) const
    {
        int n = 0;
        for (const QString& r : received)
            if (r.startsWith(prefix))
                ++n;
        return n;
    }
    QString last(const QString& prefix) const
    {
        for (int i = received.size() - 1; i >= 0; --i)
            if (received[i].startsWith(prefix))
                return received[i];
        return QString();
    }

    QWebSocketServer server;
};
}

class TestKiwi : public QObject
{
    Q_OBJECT
private slots:
    void adpcmDecodesKnownBytes()
    {
        // reference values from the standard IMA ADPCM algorithm
        const QByteArray in = QByteArray::fromHex("0077f812ab34ff01");
        const QList<int> expected = {0, 0, 11, 41, 37, -19, 22, 44, -2, -33, 17, 63, -30, -229, -144, -118};
        KiwiClient::Adpcm d;
        const QByteArray pcm = d.decode(in);
        QCOMPARE(pcm.size(), in.size() * 4);
        for (int i = 0; i < expected.size(); ++i)
            QCOMPARE(int(qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(pcm.constData() + 2 * i))),
                     expected[i]);
        QCOMPARE(d.index, 34);
        QCOMPARE(d.prev, -118);
    }

    void adpcmHonoursPresetState()
    {
        KiwiClient::Adpcm d;
        d.index = 30;
        d.prev = 1000;
        const QByteArray pcm = d.decode(QByteArray::fromHex("2143"));
        const QList<int> expected = {1048, 1121, 1213, 1322};
        for (int i = 0; i < expected.size(); ++i)
            QCOMPARE(int(qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(pcm.constData() + 2 * i))),
                     expected[i]);
    }

    void silenceStaysNearZero()
    {
        KiwiClient::Adpcm d;
        const QByteArray pcm = d.decode(QByteArray(100, char(0x88)));   // -0 steps
        for (int i = 0; i < pcm.size(); i += 2)
            QVERIFY(qAbs(qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(pcm.constData() + i))) < 64);
    }

    // The port rule: an explicit port wins; otherwise 8073, the KiwiSDR
    // default. Half of the public directory (proxy.kiwisdr.com) is listed
    // without a port and serves the WebSocket on 8073; its port 80 only
    // answers with a redirect that QWebSocket would not follow.
    void webSocketUrlPorts_data()
    {
        QTest::addColumn<QString>("receiver");
        QTest::addColumn<QString>("expected");
        QTest::newRow("explicit port") << "http://kiwi.example.org:8074" << "ws://kiwi.example.org:8074/kiwi/42/SND";
        QTest::newRow("proxy, no port") << "http://dk0ep.proxy.kiwisdr.com" << "ws://dk0ep.proxy.kiwisdr.com:8073/kiwi/42/SND";
        QTest::newRow("trailing slash") << "http://dk0ep.proxy.kiwisdr.com/" << "ws://dk0ep.proxy.kiwisdr.com:8073/kiwi/42/SND";
        QTest::newRow("https, no port") << "https://kiwi.example.org" << "wss://kiwi.example.org:443/kiwi/42/SND";
        QTest::newRow("https, port") << "https://kiwi.example.org:8443" << "wss://kiwi.example.org:8443/kiwi/42/SND";
        QTest::newRow("bare host") << "kiwi.example.org" << "ws://kiwi.example.org:8073/kiwi/42/SND";
        QTest::newRow("bare host:port") << "kiwi.example.org:8075" << "ws://kiwi.example.org:8075/kiwi/42/SND";
        QTest::newRow("spaces") << "  http://kiwi.example.org:8073  " << "ws://kiwi.example.org:8073/kiwi/42/SND";
    }
    void webSocketUrlPorts()
    {
        QFETCH(QString, receiver);
        QFETCH(QString, expected);
        QCOMPARE(KiwiClient::webSocketUrl(receiver, 42).toString(), expected);
    }

    // A whole session: auth, the receiver's greeting, the set-up commands,
    // the tuning, and audio plus S-meter from an SND frame.
    void sessionTunesAndPlays()
    {
        FakeKiwi kiwi;
        KiwiClient client;
        QSignalSpy audio(&client, &KiwiClient::audio);
        QSignalSpy meter(&client, &KiwiClient::sMeter);
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.tune(9500.0, QStringLiteral("AM"));
        client.open(kiwi.address());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        QVERIFY(kiwi.lastPath.startsWith(QLatin1String("/kiwi/")));
        QVERIFY(kiwi.lastPath.endsWith(QLatin1String("/SND")));
        QCOMPARE(kiwi.count(QStringLiteral("SET auth t=kiwi")), 1);
        QTRY_VERIFY_WITH_TIMEOUT(kiwi.count(QStringLiteral("SET mod=")) == 1, 3000);
        QCOMPARE(kiwi.last(QStringLiteral("SET mod=")),
                 QStringLiteral("SET mod=am low_cut=-4900 high_cut=4900 freq=9500.000"));
        QVERIFY(kiwi.count(QStringLiteral("SET AR OK in=12000")) == 1);
        QVERIFY(kiwi.count(QStringLiteral("SET ident_user=otd")) == 1);

        // retune while playing: USB with the receiver's frequency offset
        FakeKiwi::sendMsg(kiwi.peers.first(), "freq_offset=1000.000");
        QTest::qWait(100);
        client.tune(7125.0, QStringLiteral("usb"));
        QTRY_VERIFY_WITH_TIMEOUT(kiwi.count(QStringLiteral("SET mod=")) == 2, 3000);
        QCOMPARE(kiwi.last(QStringLiteral("SET mod=")),
                 QStringLiteral("SET mod=usb low_cut=300 high_cut=2700 freq=6125.000"));

        // compressed audio: 4 bytes of ADPCM become 16 bytes of PCM
        FakeKiwi::sendSnd(kiwi.peers.first(), 0x10, 1270 - 730, QByteArray::fromHex("0077f812"));
        QTRY_COMPARE_WITH_TIMEOUT(audio.count(), 1, 3000);
        QCOMPARE(audio.first().first().toByteArray().size(), 16);
        QCOMPARE(meter.count(), 1);
        QCOMPARE(meter.first().first().toDouble(), -73.0);   // S9
        QCOMPARE(closed.count(), 0);
        client.close();
    }

    // A frequency the receiver does not cover is not sent, and said.
    void outsideRangeIsNotSent()
    {
        FakeKiwi kiwi;
        KiwiClient client;
        QSignalSpy state(&client, &KiwiClient::stateChanged);
        client.tune(145000.0, QStringLiteral("FM"));
        client.open(kiwi.address());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        QTest::qWait(200);
        QCOMPARE(kiwi.count(QStringLiteral("SET mod=")), 0);
        bool said = false;
        for (const QList<QVariant>& s : state)
            said = said || s.first().toString().contains(QLatin1String("outside"));
        QVERIFY(said);
        client.close();
    }

    // "All channels busy" reaches the caller exactly once, with the reason.
    void tooBusyReportsReasonOnce()
    {
        FakeKiwi kiwi;
        kiwi.greet = false;
        kiwi.msgOnAuth = "too_busy=4";
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.open(kiwi.address());
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 5000);
        QTest::qWait(300);   // a late disconnect must not add a second report
        QCOMPARE(closed.count(), 1);
        QVERIFY(closed.first().first().toString().contains(QLatin1String("all 4 channels")));
        QVERIFY(!client.isOpen());
    }

    void passwordAndDownReasons_data()
    {
        QTest::addColumn<QByteArray>("msg");
        QTest::addColumn<QString>("expected");
        QTest::newRow("full or password") << QByteArray("badp=1") << "full, or needs a password";
        QTest::newRow("one per address") << QByteArray("badp=5") << "one connection per address";
        QTest::newRow("down") << QByteArray("down") << "down";
        QTest::newRow("redirect") << QByteArray("redirect=http%3A%2F%2Fother.example%3A8073") << "other.example";
    }
    void passwordAndDownReasons()
    {
        QFETCH(QByteArray, msg);
        QFETCH(QString, expected);
        FakeKiwi kiwi;
        kiwi.greet = false;
        kiwi.msgOnAuth = msg;
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.open(kiwi.address());
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 5000);
        QTest::qWait(200);
        QCOMPARE(closed.count(), 1);
        QVERIFY2(closed.first().first().toString().contains(expected),
                 qPrintable(closed.first().first().toString()));
    }

    // The receiver announces the end of the listening time, then hangs up:
    // the announced reason is the one reported, once.
    void timeLimitThenHangUp()
    {
        FakeKiwi kiwi;
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.open(kiwi.address());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        FakeKiwi::sendMsg(kiwi.peers.first(), "time_limit=60");
        QTest::qWait(100);
        kiwi.peers.first()->close();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 5000);
        QTest::qWait(200);
        QCOMPARE(closed.count(), 1);
        QVERIFY(closed.first().first().toString().contains(QLatin1String("time limit")));
    }

    // The receiver hangs up without saying why, with a close frame or by
    // dropping the connection: the general text, once, not a socket error.
    void plainHangUpSaysClosedByReceiver_data()
    {
        QTest::addColumn<bool>("drop");
        QTest::newRow("close frame") << false;
        QTest::newRow("dropped") << true;
    }
    void plainHangUpSaysClosedByReceiver()
    {
        QFETCH(bool, drop);
        FakeKiwi kiwi;
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.open(kiwi.address());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        if (drop)
            kiwi.peers.first()->abort();
        else
            kiwi.peers.first()->close();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 5000);
        QTest::qWait(300);
        QCOMPARE(closed.count(), 1);
        QCOMPARE(closed.first().first().toString(), QStringLiteral("connection closed by the receiver"));
        QVERIFY(!client.isOpen());
    }

    // Nothing listening: the attempt ends with the reason instead of hanging
    // in "Connecting ..." for ever. Qt reports disconnected() before the
    // error, and the reason must not get lost in between.
    void connectionRefusedIsReported()
    {
        QWebSocketServer probe(QStringLiteral("probe"), QWebSocketServer::NonSecureMode);
        probe.listen(QHostAddress::LocalHost);
        const quint16 deadPort = probe.serverPort();
        probe.close();
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.open(QStringLiteral("http://127.0.0.1:%1").arg(deadPort));
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 5000);
        QTest::qWait(200);
        QCOMPARE(closed.count(), 1);
        const QString reason = closed.first().first().toString();
        QVERIFY2(reason.contains(QLatin1String("refused"), Qt::CaseInsensitive), qPrintable(reason));
        QVERIFY(!client.isOpen());
    }

    // A web server that is not a KiwiSDR (or a proxy address on the wrong
    // port) answers the upgrade with a plain HTTP status: that status is
    // the reason given, not the general "closed by the receiver".
    void handshakeRejectedIsReported_data()
    {
        QTest::addColumn<QByteArray>("reply");
        QTest::addColumn<QString>("expected");
        QTest::newRow("404") << QByteArray("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n") << QStringLiteral("404");
        QTest::newRow("301") << QByteArray("HTTP/1.1 301 Moved Permanently\r\nLocation: http://example.org:8073/\r\n"
                                           "Content-Length: 0\r\nConnection: close\r\n\r\n")
                             << QStringLiteral("301");
    }
    void handshakeRejectedIsReported()
    {
        QFETCH(QByteArray, reply);
        QFETCH(QString, expected);
        QTcpServer web;
        QVERIFY(web.listen(QHostAddress::LocalHost));
        connect(&web, &QTcpServer::newConnection, this, [&web, reply]() {
            while (QTcpSocket* s = web.nextPendingConnection())
                connect(s, &QTcpSocket::readyRead, s, [s, reply]() {
                    s->readAll();
                    s->write(reply);
                });
        });
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.open(QStringLiteral("127.0.0.1:%1").arg(web.serverPort()));
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 5000);
        QTest::qWait(200);
        QCOMPARE(closed.count(), 1);
        const QString reason = closed.first().first().toString();
        QVERIFY2(reason.contains(expected), qPrintable(reason));
        QVERIFY(!client.isOpen());
    }

    // close() on request is silent: no closed() for something the caller did.
    void closeOnRequestIsSilent()
    {
        FakeKiwi kiwi;
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.open(kiwi.address());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        client.close();
        QTest::qWait(300);
        QCOMPARE(closed.count(), 0);
        QVERIFY(!client.isOpen());
    }

    // Switching receivers: what the old connection does after the switch
    // (its disconnect arriving late) must not end the new session.
    void switchingReceiversKeepsNewSession()
    {
        FakeKiwi first;
        FakeKiwi second;
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.open(first.address());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        client.open(second.address());   // as picking another receiver does
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        QTest::qWait(500);
        QCOMPARE(closed.count(), 0);
        QVERIFY(client.isOpen());
        QCOMPARE(second.count(QStringLiteral("SET auth")), 1);
        // the new session keeps its keepalives going
        const int before = second.count(QStringLiteral("SET keepalive"));
        QTest::qWait(1300);
        QVERIFY(second.count(QStringLiteral("SET keepalive")) > before);
        client.close();
    }

    // The race itself: on a real network the old connection's goodbye
    // (a last message, an error, the disconnect) can arrive after the new
    // session has started. On localhost it never does, so deliver it by hand.
    void staleSocketEventsAfterSwitchAreIgnored()
    {
        FakeKiwi first;
        FakeKiwi second;
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.open(first.address());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        const QList<QWebSocket*> before = client.findChildren<QWebSocket*>();
        QCOMPARE(before.size(), 1);
        QPointer<QWebSocket> old = before.first();
        client.open(second.address());
        QVERIFY(old);   // disposed of later, not yet
        emit old->binaryMessageReceived(QByteArray("MSG too_busy=4"));
        emit old->errorOccurred(QAbstractSocket::RemoteHostClosedError);
        emit old->disconnected();
        QVERIFY(client.isOpen());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        QTest::qWait(300);
        QCOMPARE(closed.count(), 0);
        QVERIFY(client.isOpen());
        QCOMPARE(second.count(QStringLiteral("SET auth")), 1);
        // the old socket goes away; only the live one is left
        QTRY_VERIFY_WITH_TIMEOUT(old.isNull(), 2000);
        QCOMPARE(client.findChildren<QWebSocket*>().size(), 1);
        client.close();
    }

    // open() takes the addresses the port rule covers, not only full URLs:
    // "host:port" as typed (QUrl alone reads "localhost" there as a scheme
    // and rejects "127.0.0.1:port"). An address that cannot be used ends in
    // one closed() with the reason, after open() has returned.
    void openAcceptsBareHostPort_data()
    {
        QTest::addColumn<QString>("form");
        QTest::addColumn<bool>("usable");
        QTest::newRow("ip:port") << "127.0.0.1:%1" << true;
        QTest::newRow("localhost:port") << "localhost:%1" << true;
        QTest::newRow("spaces") << "  http://127.0.0.1:%1  " << true;
        QTest::newRow("empty") << "  " << false;
        QTest::newRow("no host") << "http://:%1" << false;
        QTest::newRow("garbage") << "no such host:%1" << false;
    }
    void openAcceptsBareHostPort()
    {
        QFETCH(QString, form);
        QFETCH(bool, usable);
        FakeKiwi kiwi;
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        const QString address = form.contains(QLatin1String("%1")) ? form.arg(kiwi.server.serverPort()) : form;
        client.open(address);
        if (usable)
        {
            QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
            QCOMPARE(closed.count(), 0);
            QCOMPARE(client.receiver().scheme(), QStringLiteral("http"));
            QVERIFY(!client.receiver().host().isEmpty());
            QCOMPARE(kiwi.count(QStringLiteral("SET auth")), 1);
            client.close();
            return;
        }
        QCOMPARE(closed.count(), 0);   // not yet: the caller may still connect
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 5000);
        QTest::qWait(200);
        QCOMPARE(closed.count(), 1);
        // a clear reason about the address, not a socket error about an empty host
        const QString reason = closed.first().first().toString();
        QVERIFY2(reason.contains(QLatin1String("receiver address")), qPrintable(reason));
        QVERIFY(!client.isOpen());
        QVERIFY(kiwi.peers.isEmpty());
    }

    // A queued "unusable address" report belongs to its own open(): a good
    // address opened right after it must not be ended by it.
    void unusableAddressThenGoodOne()
    {
        FakeKiwi kiwi;
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.open(QString());
        client.open(kiwi.address());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        QTest::qWait(200);
        QCOMPARE(closed.count(), 0);
        QVERIFY(client.isOpen());
        client.close();
    }

    // A host that takes the connection and then says nothing: the session
    // is given up with a reason instead of "Connecting ..." for ever. The
    // host is either not a WebSocket server at all, or one that never sends
    // the KiwiSDR greeting.
    void handshakeTimeout_data()
    {
        QTest::addColumn<bool>("webSocket");
        QTest::newRow("silent tcp") << false;
        QTest::newRow("silent websocket") << true;
    }
    void handshakeTimeout()
    {
        QFETCH(bool, webSocket);
        QTcpServer silent;   // accepts connections (the system does) and never answers
        QVERIFY(silent.listen(QHostAddress::LocalHost));
        FakeKiwi kiwi;
        kiwi.greet = false;
        const QString address = webSocket ? kiwi.address()
                                          : QStringLiteral("http://127.0.0.1:%1").arg(silent.serverPort());
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.setConnectTimeout(500);
        client.open(address);
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 5000);
        QTest::qWait(300);
        QCOMPARE(closed.count(), 1);
        const QString reason = closed.first().first().toString();
        QVERIFY2(reason.contains(QLatin1String("did not answer")), qPrintable(reason));
        QVERIFY(!client.isOpen());
    }

    // The connect timeout only covers the start: a session that is under
    // way is never ended by it, however short it is.
    void handshakeTimeoutSparesReadySession()
    {
        FakeKiwi kiwi;
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.open(kiwi.address());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        // set once ready, so a slow start under load cannot trip it
        client.setConnectTimeout(300);
        QTest::qWait(1000);
        QCOMPARE(closed.count(), 0);
        QVERIFY(client.isOpen());
        QVERIFY(client.isReady());
        client.close();
    }

    // The receiver announces the end of the listening time, then the
    // connection drops without a proper close: the announced reason is the
    // one reported, once, not the socket error.
    void errorAfterTimeLimitKeepsReason()
    {
        FakeKiwi kiwi;
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.open(kiwi.address());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        QWebSocket* peer = kiwi.peers.first();
        FakeKiwi::sendMsg(peer, "time_limit=60");
        // the client answers audio_rate; its answer shows it has read the
        // time limit message sent before
        FakeKiwi::sendMsg(peer, "audio_rate=12000");
        QTRY_COMPARE_WITH_TIMEOUT(kiwi.count(QStringLiteral("SET AR OK")), 2, 5000);
        peer->abort();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 5000);
        QTest::qWait(300);
        QCOMPARE(closed.count(), 1);
        const QString reason = closed.first().first().toString();
        QVERIFY2(reason.contains(QLatin1String("time limit")), qPrintable(reason));
        QVERIFY(!client.isOpen());
    }

    // One receiver's frequency offset (a converter) and coverage must not
    // shift or block the tuning on the next receiver.
    void offsetDoesNotCarryOverToNextReceiver()
    {
        FakeKiwi first;
        first.msgOnAuth = "bandwidth=5000000 freq_offset=1000.000";   // covers 1000-6000 kHz
        FakeKiwi second;
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.tune(5500.0, QStringLiteral("USB"));
        client.open(first.address());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(first.count(QStringLiteral("SET mod=")), 1, 3000);
        QCOMPARE(first.last(QStringLiteral("SET mod=")),
                 QStringLiteral("SET mod=usb low_cut=300 high_cut=2700 freq=4500.000"));

        client.open(second.address());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        client.tune(7125.0, QStringLiteral("USB"));
        QTRY_COMPARE_WITH_TIMEOUT(second.last(QStringLiteral("SET mod=")),
                                  QStringLiteral("SET mod=usb low_cut=300 high_cut=2700 freq=7125.000"), 3000);
        QCOMPARE(closed.count(), 0);
        client.close();
    }

    // The player closes the client from inside the audio slot when the
    // sound device fails. That is a close on request: silent, and nothing
    // more arrives from the socket being dropped, even with a second frame
    // already on its way.
    void closeFromAudioSlotIsSilent()
    {
        FakeKiwi kiwi;
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        int audioCount = 0;
        connect(&client, &KiwiClient::audio, &client, [&]() {
            ++audioCount;
            client.close();
        });
        client.open(kiwi.address());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        QPointer<QWebSocket> peer = kiwi.peers.first();
        FakeKiwi::sendSnd(peer, 0x10, 1270 - 730, QByteArray::fromHex("0077f812"));
        FakeKiwi::sendSnd(peer, 0x10, 1270 - 730, QByteArray::fromHex("0077f812"));
        QTRY_COMPARE_WITH_TIMEOUT(audioCount, 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!peer || peer->state() == QAbstractSocket::UnconnectedState, 5000);
        QTest::qWait(300);
        QCOMPARE(audioCount, 1);
        QCOMPARE(closed.count(), 0);
        QVERIFY(!client.isOpen());
        QVERIFY(!client.isReady());
    }

    // After close() the receiver hears nothing more, keepalives included.
    void keepaliveStopsOnClose()
    {
        FakeKiwi kiwi;
        KiwiClient client;
        client.open(kiwi.address());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(kiwi.count(QStringLiteral("SET keepalive")) >= 1, 3000);
        QPointer<QWebSocket> peer = kiwi.peers.first();
        client.close();
        // whatever was sent before the close has arrived once the fake sees the hang-up
        QTRY_VERIFY_WITH_TIMEOUT(!peer || peer->state() == QAbstractSocket::UnconnectedState, 5000);
        const int before = kiwi.count(QStringLiteral("SET keepalive"));
        QTest::qWait(1500);
        QCOMPARE(kiwi.count(QStringLiteral("SET keepalive")), before);
    }
};

QTEST_GUILESS_MAIN(TestKiwi)
#include "test_kiwi.moc"
