// SPDX-License-Identifier: GPL-3.0-or-later
// KiwiClient: the ADPCM decoder, the WebSocket address rule, and whole
// sessions against a small fake KiwiSDR on localhost that speaks the same
// messages a real receiver sends.
#include "core/KiwiClient.h"
#include "core/Logging.h"
#include "core/PcmQueue.h"
#include "core/PcmResampler.h"

#include <QPointer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtEndian>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
// A fake receiver. Each connection is scripted by the test through the
// hooks; everything the client sends is recorded. Like a KiwiSDR it serves
// its web page on the same port as the audio: a plain request gets a small
// answer and is noted in pages, a WebSocket handshake goes to the server.
class FakeKiwi : public QObject
{
public:
    QStringList received;              // text commands from the client
    QStringList pages;                 // paths fetched over plain HTTP
    QList<QWebSocket*> peers;
    QString lastPath;                  // the path the client asked for
    bool greet = true;                 // answer auth with audio_rate/sample_rate
    QByteArray msgOnAuth;              // extra MSG body sent right after auth

    FakeKiwi()
        : server(QStringLiteral("fake kiwi"), QWebSocketServer::NonSecureMode)
    {
        connect(&front, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* s = front.nextPendingConnection())
                connect(s, &QTcpSocket::readyRead, this, [this, s]() { route(s); });
        });
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
        front.listen(QHostAddress::LocalHost);
    }
    quint16 port() const { return front.serverPort(); }
    QString address() const { return QStringLiteral("http://127.0.0.1:%1").arg(front.serverPort()); }
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

    QTcpServer front;
    QWebSocketServer server;

private:
    // the first bytes tell a page request from a handshake
    void route(QTcpSocket* s)
    {
        const QByteArray head = s->peek(4096);
        if (!head.contains("\r\n\r\n"))
            return;   // not all of the request yet
        s->disconnect(this);
        if (head.contains("Upgrade: websocket"))
        {
            server.handleConnection(s);
            return;
        }
        const QByteArray path = head.split(' ').value(1);
        pages << QString::fromLatin1(path);
        s->readAll();
        const QByteArray body = path == "/" ? QByteArray("<html>kiwi</html>") : QByteArray("PNG");
        s->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        s->disconnectFromHost();
    }
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

        // back in range: the complaint is replaced by the usual text, once
        state.clear();
        client.tune(7125.0, QStringLiteral("USB"));
        QTRY_VERIFY_WITH_TIMEOUT(kiwi.count(QStringLiteral("SET mod=")) == 1, 3000);
        QCOMPARE(state.count(), 1);
        QVERIFY2(state.first().first().toString().startsWith(QLatin1String("Listening on")),
                 qPrintable(state.first().first().toString()));
        client.tune(7130.0, QStringLiteral("USB"));   // still in range: nothing new to say
        QTRY_VERIFY_WITH_TIMEOUT(kiwi.count(QStringLiteral("SET mod=")) == 2, 3000);
        QCOMPARE(state.count(), 1);
        client.tune(145000.0, QStringLiteral("FM"));  // out again: said again
        QTRY_COMPARE_WITH_TIMEOUT(state.count(), 2, 3000);
        QVERIFY(state.last().first().toString().contains(QLatin1String("outside")));
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
        QTest::newRow("301, nowhere") << QByteArray("HTTP/1.1 301 Moved Permanently\r\n"
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

    // too_busy once the session is under way is the receiver's limit on
    // programs other than its web page (decided about ten seconds in), not
    // a full receiver: said so, with the number when there is one.
    void apiLimitIsToldApart_data()
    {
        QTest::addColumn<QByteArray>("msg");
        QTest::addColumn<QString>("expected");
        QTest::newRow("no programs allowed") << QByteArray("too_busy=0") << QStringLiteral("only its own web page");
        QTest::newRow("program slots taken") << QByteArray("too_busy=2") << QStringLiteral("2 connections from programs");
    }
    void apiLimitIsToldApart()
    {
        QFETCH(QByteArray, msg);
        QFETCH(QString, expected);
        FakeKiwi kiwi;
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.open(kiwi.address());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        FakeKiwi::sendMsg(kiwi.peers.first(), msg);
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 5000);
        const QString reason = closed.first().first().toString();
        QVERIFY2(reason.contains(expected), qPrintable(reason));
        QVERIFY(!reason.contains(QLatin1String("channels of this receiver")));
        QVERIFY(!client.isOpen());
    }

    // Once connected, the client fetches the receiver's page and two of its
    // icons, as a visitor's browser does: the receiver counts those and
    // otherwise takes the client for an automated program after ten seconds.
    void visitsThePageLikeABrowser()
    {
        FakeKiwi kiwi;
        KiwiClient client;
        client.open(kiwi.address());
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(kiwi.pages.size(), 3, 5000);
        QStringList pages = kiwi.pages;
        pages.sort();
        QCOMPARE(pages, (QStringList{"/", "/gfx/openwebrx-bottom-arrow-hide.png", "/gfx/openwebrx-bottom-arrow-show.png"}));
        QTest::qWait(300);
        QCOMPARE(kiwi.pages.size(), 3);   // once, not again and again
        QVERIFY(client.isOpen());
        client.close();
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
        const QString address = form.contains(QLatin1String("%1")) ? form.arg(kiwi.port()) : form;
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

    // The receiver's 12 kHz mono for a device that takes only its own
    // format (Windows without Media Foundation: 48 kHz stereo float or so).
    void resamplerForTheSoundDevice()
    {
        auto pcm = [](const QList<int>& samples) {
            QByteArray b;
            for (int v : samples)
            {
                uchar le[2];
                qToLittleEndian<qint16>(qint16(v), le);
                b.append(reinterpret_cast<const char*>(le), 2);
            }
            return b;
        };
        auto floats = [](const QByteArray& b) {
            QList<float> out;
            for (int i = 0; i + 3 < b.size(); i += 4)
            {
                float f;
                std::memcpy(&f, b.constData() + i, 4);
                out << f;
            }
            return out;
        };
        auto shorts = [](const QByteArray& b) {
            QList<qint16> out;
            for (int i = 0; i + 1 < b.size(); i += 2)
            {
                qint16 v;
                std::memcpy(&v, b.constData() + i, 2);   // the machine's own order, as Qt takes it
                out << v;
            }
            return out;
        };

        // four times the rate, both channels: the steps in between filled in
        PcmResampler up(12000, 48000, 2, PcmResampler::Format::Float);
        QCOMPARE(up.bytesPerFrame(), 8);
        const QList<float> f = floats(up.convert(pcm({0, 400, 800})));
        QCOMPARE(f.size(), 8 * 2);   // 8 frames so far; the last sample waits for the next block
        const QList<float> expect = {0, 100, 200, 300, 400, 500, 600, 700};
        for (int i = 0; i < expect.size(); ++i)
        {
            QCOMPARE(f[2 * i], expect[i] / 32768.0f);
            QCOMPARE(f[2 * i + 1], expect[i] / 32768.0f);   // the same on both sides
        }
        // the next block goes on where this one stopped, no jump
        const QList<float> g = floats(up.convert(pcm({1200})));
        QCOMPARE(g.size(), 4 * 2);
        QCOMPARE(g[0], 800 / 32768.0f);
        QCOMPARE(g[6], 1100 / 32768.0f);

        // blocks of any size give the same sound as one block
        QList<int> tone;
        for (int i = 0; i < 997; ++i)
            tone << int(10000 * std::sin(i * 0.3));
        PcmResampler whole(12000, 44100, 1, PcmResampler::Format::Int16);
        PcmResampler pieces(12000, 44100, 1, PcmResampler::Format::Int16);
        const QByteArray all = whole.convert(pcm(tone));
        QByteArray parts;
        for (int at = 0, n = 1; at < tone.size(); at += n, n = n % 37 + 3)
            parts += pieces.convert(pcm(tone.mid(at, n)));
        QCOMPARE(parts, all);
        QVERIFY(qAbs(all.size() / 2 - int(996 * 44100.0 / 12000)) <= 1);

        // downwards: each output sample is the average of the ones it covers
        PcmResampler down(12000, 6000, 1, PcmResampler::Format::Int16);
        QCOMPARE(shorts(down.convert(pcm({0, 10, 20, 30, 40, 50, 60}))), (QList<qint16>{5, 25, 45}));
        QCOMPARE(shorts(down.convert(pcm({70}))), QList<qint16>{65});   // 60 waited for 70
        PcmResampler down15(12000, 8000, 1, PcmResampler::Format::Int16);
        QCOMPARE(shorts(down15.convert(pcm({100, 200, 300, 400, 500, 600}))), (QList<qint16>{100, 250, 400, 550}));

        // a surround device: left and right get the sound, the rest silence
        PcmResampler six(12000, 12000, 6, PcmResampler::Format::Int16);
        QCOMPARE(shorts(six.convert(pcm({-7, 9, 11}))), (QList<qint16>{-7, -7, 0, 0, 0, 0, 9, 9, 0, 0, 0, 0}));

        // same rate, other sample formats
        PcmResampler i32(12000, 12000, 1, PcmResampler::Format::Int32);
        const QByteArray w = i32.convert(pcm({-2, 3, 5}));
        QCOMPARE(w.size(), 8);
        qint32 v0, v1;
        std::memcpy(&v0, w.constData(), 4);
        std::memcpy(&v1, w.constData() + 4, 4);
        QCOMPARE(v0, -2 * 65536);
        QCOMPARE(v1, 3 * 65536);
        PcmResampler u8(12000, 12000, 1, PcmResampler::Format::UInt8);
        const QByteArray b = u8.convert(pcm({-32768, 0, 32767, 0}));
        QCOMPARE(b.size(), 3);
        QCOMPARE(quint8(b[0]), quint8(0));
        QCOMPARE(quint8(b[1]), quint8(128));
        QCOMPARE(quint8(b[2]), quint8(255));
        QVERIFY(u8.convert(QByteArray()).isEmpty());
    }

    // The queue between the receiver and the sound device: nothing until a
    // little has collected, then whatever it has (the device's own buffer
    // rides out the gaps), told with readyRead() each time; never more than
    // three seconds; after a long stall it collects afresh.
    void pcmQueueFeedsTheSink()
    {
        PcmQueue q(1000);   // 1000 bytes a second: 300 pre-roll, 3000 at most
        QSignalSpy ready(&q, &QIODevice::readyRead);
        QVERIFY(q.isOpen() && q.isSequential());
        QCOMPARE(q.bytesAvailable(), 0);
        char buf[400];
        QCOMPARE(q.read(buf, 100), 100);   // nothing yet: silence, never a read of nothing
        QVERIFY(std::all_of(buf, buf + 100, [](char c) { return c == 0; }));

        q.push(QByteArray(200, 'a'));
        QCOMPARE(ready.count(), 0);       // below the pre-roll: not offered yet
        QCOMPARE(q.bytesAvailable(), 0);
        QCOMPARE(q.read(buf, 100), 100);  // still silence, the data kept
        QVERIFY(std::all_of(buf, buf + 100, [](char c) { return c == 0; }));
        QCOMPARE(q.buffered(), 200);

        q.push(QByteArray(200, 'b'));
        QCOMPARE(ready.count(), 1);       // primed: the sink is told
        QCOMPARE(q.bytesAvailable(), 400);
        QCOMPARE(q.read(buf, 300), 300);  // the data, oldest first
        QVERIFY(std::all_of(buf, buf + 200, [](char c) { return c == 'a'; }));
        QVERIFY(std::all_of(buf + 200, buf + 300, [](char c) { return c == 'b'; }));
        QCOMPARE(q.read(buf, 400), 400);  // the rest, then silence for a sink that asks for more
        QVERIFY(std::all_of(buf, buf + 100, [](char c) { return c == 'b'; }));
        QVERIFY(std::all_of(buf + 100, buf + 400, [](char c) { return c == 0; }));
        QCOMPARE(q.buffered(), 0);
        QCOMPARE(q.bytesAvailable(), 0);  // a sink that asks first is told: nothing
        QCOMPARE(q.read(buf, 100), 100);  // one that just asks gets silence, and stays running
        q.push(QByteArray(50, 'c'));      // the usual gap between two bursts: goes on at once
        QCOMPARE(ready.count(), 2);
        QCOMPARE(q.bytesAvailable(), 50);
        QCOMPARE(q.read(buf, 100), 100);
        QVERIFY(std::all_of(buf, buf + 50, [](char c) { return c == 'c'; }));

        QTest::qWait(1600);               // a long stall: collects afresh
        q.push(QByteArray(100, 'd'));
        QCOMPARE(ready.count(), 2);
        QCOMPARE(q.bytesAvailable(), 0);
        QCOMPARE(q.read(buf, 100), 100);  // silence meanwhile, the data kept
        QCOMPARE(q.buffered(), 100);
        q.push(QByteArray(250, 'd'));
        QCOMPARE(ready.count(), 3);
        QCOMPARE(q.bytesAvailable(), 350);

        q.push(QByteArray(5000, 'e'));    // far ahead: the oldest goes
        QCOMPARE(q.buffered(), 3000);
        QCOMPARE(q.read(buf, 10), 10);
        QVERIFY(std::all_of(buf, buf + 10, [](char c) { return c == 'e'; }));
        q.push(QByteArray());             // nothing arrived: nothing said
        QCOMPARE(ready.count(), 4);

        PcmQueue u8(1000, char(0x80));    // unsigned 8-bit: silence is 0x80
        u8.push(QByteArray(300, 'x'));
        QCOMPARE(u8.read(buf, 304), 304);
        QVERIFY(std::all_of(buf + 300, buf + 304, [](char c) { return quint8(c) == 0x80; }));
    }

    // --log: what the program reports lands in the file, with time and
    // category, the detailed categories switched on.
    void logFileGetsEverything()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("otd.log"));
        QVERIFY(Logging::toFile(path));
        QLoggingCategory kiwi("otd.kiwi");
        qCDebug(kiwi) << "hello from the test" << 42;
        qWarning("a warning too");
        qInstallMessageHandler(nullptr);   // back to normal for the other tests
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString text = QString::fromUtf8(f.readAll());
        QVERIFY2(text.contains(QLatin1String("otd.kiwi debug: hello from the test 42")), qPrintable(text));
        QVERIFY2(text.contains(QLatin1String("default warning: a warning too")), qPrintable(text));
        QVERIFY(text.contains(QLatin1String("log started")));
        QVERIFY2(QRegularExpression(QStringLiteral("^\\d\\d:\\d\\d:\\d\\d\\.\\d\\d\\d ")).match(text).hasMatch(), qPrintable(text.left(40)));
        QVERIFY(!Logging::toFile(dir.filePath(QStringLiteral("no/such/dir/x.log"))));
    }

    // The proxy.kiwisdr.com hosts answer the handshake with a redirect to
    // the receiver's real address (another host, port 80). QWebSocket
    // cannot follow it, so the client asks where it goes and connects there,
    // with its own path.
    void redirectedProxyIsFollowed()
    {
        FakeKiwi kiwi;
        // two hops, as the real proxies do: proxy -> proxy2 (port 80) -> proxy2:8073
        QTcpServer proxy, hop;
        QVERIFY(proxy.listen(QHostAddress::LocalHost) && hop.listen(QHostAddress::LocalHost));
        int hits = 0;
        auto redirectTo = [&hits](QTcpServer& server, const QByteArray& location) {
            connect(&server, &QTcpServer::newConnection, &server, [&server, location, &hits]() {
                while (QTcpSocket* s = server.nextPendingConnection())
                    connect(s, &QTcpSocket::readyRead, s, [s, location, &hits]() {
                        s->readAll();
                        ++hits;
                        s->write("HTTP/1.0 307 Temporary Redirect\r\nLocation: " + location
                                 + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        s->disconnectFromHost();
                    });
            });
        };
        redirectTo(proxy, "http://127.0.0.1:" + QByteArray::number(hop.serverPort()) + "/kiwi/0/snd");
        redirectTo(hop, "http://127.0.0.1:" + QByteArray::number(kiwi.port()) + "/kiwi/0/snd");
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        QSignalSpy state(&client, &KiwiClient::stateChanged);
        client.tune(6070.0, QStringLiteral("AM"));
        client.open(QStringLiteral("http://127.0.0.1:%1").arg(proxy.serverPort()));
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 5000);
        QCOMPARE(hits, 4);   // at each hop: the handshake, then the question where to
        QTRY_COMPARE_WITH_TIMEOUT(kiwi.pages.size(), 3, 5000);   // the page fetched from the receiver itself
        QCOMPARE(hits, 4);                                          // not from the proxy
        QVERIFY2(kiwi.lastPath.endsWith(QLatin1String("/SND")), qPrintable(kiwi.lastPath));   // our path, not the redirect's
        QCOMPARE(kiwi.count(QStringLiteral("SET auth")), 1);
        QTRY_VERIFY_WITH_TIMEOUT(kiwi.count(QStringLiteral("SET mod=")) == 1, 3000);
        QCOMPARE(closed.count(), 0);
        QCOMPARE(client.receiver().port(), int(proxy.serverPort()));   // still the address as listed
        client.close();
    }

    // A redirect that leads nowhere, or round in circles, is given up on
    // and said once.
    void redirectLoopsAndDeadEndsAreReported()
    {
        QTcpServer loop;   // redirects to itself
        QVERIFY(loop.listen(QHostAddress::LocalHost));
        const QByteArray self = "http://127.0.0.1:" + QByteArray::number(loop.serverPort()) + "/";
        connect(&loop, &QTcpServer::newConnection, this, [&]() {
            while (QTcpSocket* s = loop.nextPendingConnection())
                connect(s, &QTcpSocket::readyRead, s, [s, self]() {
                    s->readAll();
                    s->write("HTTP/1.0 307 Temporary Redirect\r\nLocation: " + self
                             + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    s->disconnectFromHost();
                });
        });
        KiwiClient client;
        QSignalSpy closed(&client, &KiwiClient::closed);
        client.open(QStringLiteral("127.0.0.1:%1").arg(loop.serverPort()));
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QVERIFY2(closed.first().first().toString().contains(QLatin1String("redirect")), qPrintable(closed.first().first().toString()));
        QVERIFY(!client.isOpen());
        QTest::qWait(300);
        QCOMPARE(closed.count(), 1);

        // a redirect to a port nobody listens on: the refusal is reported
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost));
        const quint16 dead = probe.serverPort();
        probe.close();
        QTcpServer toDead;
        QVERIFY(toDead.listen(QHostAddress::LocalHost));
        const QByteArray deadUrl = "http://127.0.0.1:" + QByteArray::number(dead) + "/";
        connect(&toDead, &QTcpServer::newConnection, this, [&]() {
            while (QTcpSocket* s = toDead.nextPendingConnection())
                connect(s, &QTcpSocket::readyRead, s, [s, deadUrl]() {
                    s->readAll();
                    s->write("HTTP/1.0 302 Found\r\nLocation: " + deadUrl + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    s->disconnectFromHost();
                });
        });
        KiwiClient second;
        QSignalSpy closed2(&second, &KiwiClient::closed);
        second.open(QStringLiteral("127.0.0.1:%1").arg(toDead.serverPort()));
        QTRY_COMPARE_WITH_TIMEOUT(closed2.count(), 1, 10000);
        QVERIFY2(closed2.first().first().toString().contains(QLatin1String("refused"), Qt::CaseInsensitive),
                 qPrintable(closed2.first().first().toString()));
    }

    // The receiver's name for a rig's mode: Hamlib's data and ECSS modes end
    // in the sideband, CW variants start with CW, anything FM is NFM, the
    // rest is AM.
    void receiverModeForRigMode()
    {
        QCOMPARE(KiwiClient::receiverMode(QStringLiteral("USB")), QStringLiteral("USB"));
        QCOMPARE(KiwiClient::receiverMode(QStringLiteral("PKTUSB")), QStringLiteral("USB"));
        QCOMPARE(KiwiClient::receiverMode(QStringLiteral("ECSSLSB")), QStringLiteral("LSB"));
        QCOMPARE(KiwiClient::receiverMode(QStringLiteral("CWR")), QStringLiteral("CW"));
        QCOMPARE(KiwiClient::receiverMode(QStringLiteral("PKTFM")), QStringLiteral("NFM"));
        QCOMPARE(KiwiClient::receiverMode(QStringLiteral("WFM")), QStringLiteral("NFM"));
        QCOMPARE(KiwiClient::receiverMode(QStringLiteral("SAM")), QStringLiteral("AM"));
        QCOMPARE(KiwiClient::receiverMode(QStringLiteral("am")), QStringLiteral("AM"));
        QCOMPARE(KiwiClient::receiverMode(QString()), QStringLiteral("AM"));
    }
};

QTEST_GUILESS_MAIN(TestKiwi)
#include "test_kiwi.moc"
