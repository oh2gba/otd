// SPDX-License-Identifier: GPL-3.0-or-later
// The KiwiSDR player through a real sound device: a PulseAudio server of
// this test's own, with a silent sink, started here and stopped at the end.
// The sink is opened in the receiver's format (12 kHz mono, or whatever
// the receiver says) and the sound server converts; what arrives at the
// device is recorded and compared with what the receiver sent. Skipped
// where there is no pulseaudio program.
#include "KiwiPlayer.h"

#include <QAudioSink>
#include <QComboBox>
#include <QMediaDevices>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtEndian>
#include <QtTest>

#include <cmath>

namespace
{
// A fake receiver (the one from test_kiwi.cpp, with the sample rate it
// announces settable).
class FakeKiwi : public QObject
{
public:
    QStringList received;
    QList<QWebSocket*> peers;
    int audioRate = 12000;

    FakeKiwi()
        : server(QStringLiteral("fake kiwi"), QWebSocketServer::NonSecureMode)
    {
        connect(&server, &QWebSocketServer::newConnection, this, [this]() {
            while (QWebSocket* ws = server.nextPendingConnection())
            {
                peers << ws;
                connect(ws, &QWebSocket::textMessageReceived, this, [this, ws](const QString& t) {
                    received << t;
                    if (t.startsWith(QLatin1String("SET auth")))
                    {
                        sendMsg(ws, "audio_rate=" + QByteArray::number(audioRate));
                        sendMsg(ws, "sample_rate=" + QByteArray::number(audioRate) + ".000");
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
    // an SND frame with plain 16-bit samples (big-endian, as the receiver
    // sends them uncompressed): flags 0, sequence, S-meter, then the samples
    static void sendPcm(QWebSocket* ws, const QList<qint16>& samples)
    {
        QByteArray f("SND");
        f.append(char(0));
        f.append(QByteArray(4, '\0'));
        f.append(QByteArray(2, '\0'));
        for (qint16 v : samples)
        {
            uchar b[2];
            qToBigEndian<qint16>(v, b);
            f.append(reinterpret_cast<const char*>(b), 2);
        }
        ws->sendBinaryMessage(f);
    }

    QWebSocketServer server;
};

QToolButton* playButton(KiwiPlayer& p)
{
    for (QToolButton* b : p.findChildren<QToolButton*>())
        if (b->toolTip() == KiwiPlayer::tr("Start or stop listening"))
            return b;
    return nullptr;
}

QString lastStatus(const QSignalSpy& spy)
{
    return spy.isEmpty() ? QStringLiteral("<none>") : spy.last().first().toString();
}

// a second of a 1 kHz tone at the given rate, loud enough to be unmistakable
QList<qint16> tone(int rate)
{
    QList<qint16> out;
    for (int i = 0; i < rate; ++i)
        out << qint16(std::lround(12000.0 * std::sin(2.0 * M_PI * 1000.0 * i / rate)));
    return out;
}

QList<qint16> samplesOf(const QByteArray& raw)
{
    QList<qint16> out;
    for (int i = 0; i + 1 < raw.size(); i += 2)
        out << qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(raw.constData() + i));
    return out;
}
}

class TestKiwiSound : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    QProcess m_server;
    QString m_pactl;

    QString runPactl(const QStringList& args)
    {
        QProcess p;
        p.start(m_pactl, args);
        p.waitForFinished(5000);
        return QString::fromUtf8(p.readAllStandardOutput());
    }
    // the streams playing into the server right now
    int sinkInputs() { return runPactl({QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sink-inputs")}).split(QLatin1Char('\n'), Qt::SkipEmptyParts).size(); }

    // plays a receiver and waits for the sound device to be running
    void startPlaying(KiwiPlayer& p, FakeKiwi& kiwi, QSignalSpy& status)
    {
        p.setReceivers({kiwi.address()}, {}, kiwi.address());
        QToolButton* play = playButton(p);
        QVERIFY(play);
        play->click();
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).startsWith(QLatin1String("Listening on")), 5000);
        QCOMPARE(kiwi.peers.size(), 1);
    }

private slots:
    void initTestCase()
    {
        const QString pulseaudio = QStandardPaths::findExecutable(QStringLiteral("pulseaudio"));
        m_pactl = QStandardPaths::findExecutable(QStringLiteral("pactl"));
        if (pulseaudio.isEmpty() || m_pactl.isEmpty())
            QSKIP("no pulseaudio and pactl on this machine: the sound device tests need them");
        QVERIFY(m_dir.isValid());
        const QString socket = m_dir.filePath(QStringLiteral("native"));
        // this server, and no other, for everything in this process
        qputenv("PULSE_SERVER", ("unix:" + socket).toUtf8());
        qputenv("PULSE_RUNTIME_PATH", m_dir.path().toUtf8());
        qputenv("XDG_RUNTIME_DIR", m_dir.path().toUtf8());
        // a silent sink in the receiver's own format, so that what the
        // recording sees is what the player handed over
        m_server.start(pulseaudio, {QStringLiteral("-n"), QStringLiteral("--daemonize=no"),
                                    QStringLiteral("--exit-idle-time=-1"), QStringLiteral("--use-pid-file=no"),
                                    QStringLiteral("--disable-shm=yes"), QStringLiteral("--log-target=stderr"),
                                    QStringLiteral("-L"),
                                    QStringLiteral("module-null-sink sink_name=otd format=s16le rate=12000 channels=1"),
                                    QStringLiteral("-L"),
                                    QStringLiteral("module-native-protocol-unix socket=%1").arg(socket)});
        QVERIFY2(m_server.waitForStarted(5000), "pulseaudio did not start");
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(socket), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(runPactl({QStringLiteral("info")}).contains(QLatin1String("Server String")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!QMediaDevices::defaultAudioOutput().isNull(), 10000);
    }

    void cleanupTestCase()
    {
        if (m_server.state() != QProcess::NotRunning)
        {
            m_server.terminate();
            m_server.waitForFinished(5000);
        }
    }

    void cleanup()
    {
        QTRY_COMPARE_WITH_TIMEOUT(sinkInputs(), 0, 5000);   // every test leaves the device alone
    }

    // The receiver's samples come out of the sound device as they went in:
    // the sink runs in the receiver's format, 12 kHz mono, and the tone is
    // found in the recording, sample for sample.
    void receiverAudioReachesTheDevice()
    {
        FakeKiwi kiwi;
        KiwiPlayer p;
        QSignalSpy status(&p, &KiwiPlayer::statusChanged);
        QSignalSpy inHand(&p, &KiwiPlayer::audioInHandChanged);
        p.setVolume(100);   // as sent; the volume slider scales the samples
        startPlaying(p, kiwi, status);

        // record what the sink plays, through its monitor, in the same format
        const QString recording = m_dir.filePath(QStringLiteral("heard.raw"));
        QProcess parec;
        parec.start(QStandardPaths::findExecutable(QStringLiteral("parec")),
                    {QStringLiteral("--device=otd.monitor"), QStringLiteral("--format=s16le"),
                     QStringLiteral("--rate=12000"), QStringLiteral("--channels=1"), QStringLiteral("--raw"),
                     QStringLiteral("--latency-msec=10"),   // written as it comes, not held by the server
                     recording});
        QVERIFY(parec.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(recording), 5000);
        QTest::qWait(300);

        const QList<qint16> sent = tone(12000);
        for (int at = 0; at < sent.size(); at += 480)   // 40 ms frames, as a receiver sends them
        {
            FakeKiwi::sendPcm(kiwi.peers.first(), sent.mid(at, 480));
            QTest::qWait(40);
        }
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).endsWith(QLatin1String(": playing")), 5000);
        auto* sink = p.findChild<QAudioSink*>();
        QVERIFY(sink);
        QCOMPARE(sink->format().sampleRate(), 12000);
        QCOMPARE(sink->format().channelCount(), 1);
        QCOMPARE(sink->format().sampleFormat(), QAudioFormat::Int16);
        QCOMPARE(sink->state(), QAudio::ActiveState);
        QCOMPARE(sinkInputs(), 1);
        QVERIFY(p.isPlaying());
        // audio in hand: a good part of a second, told while playing
        QTRY_VERIFY_WITH_TIMEOUT(p.audioInHand() > 0.15, 3000);
        QVERIFY2(p.audioInHand() < 3.5, qPrintable(QString::number(p.audioInHand())));
        QTRY_VERIFY_WITH_TIMEOUT(inHand.count() >= 2, 3000);
        QVERIFY(inHand.last().first().toDouble() > 0.15);
        QTest::qWait(1500);   // the rest of the second, and the queue's pre-roll
        parec.terminate();
        QVERIFY(parec.waitForFinished(5000));

        QFile::remove(QStringLiteral("build/heard.raw"));
        QFile::copy(recording, QStringLiteral("build/heard.raw"));   // for looking at, when it goes wrong
        QFile f(recording);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QList<qint16> heard = samplesOf(f.readAll());
        QVERIFY2(heard.size() >= 9000, qPrintable(QString::number(heard.size())));   // most of the second
        // the tone starts where the recording first leaves silence
        int start = -1;
        for (int i = 0; i < heard.size() && start < 0; ++i)
            if (qAbs(heard[i]) > 100)
                start = i;
        QVERIFY2(start >= 0, "nothing but silence was heard");
        // the first samples of the tone are small; line up on the first big one
        int sentStart = 0;
        while (sentStart < sent.size() && qAbs(sent[sentStart]) <= 100)
            ++sentStart;
        const int n = qMin(sent.size() - sentStart, heard.size() - start);
        QVERIFY(n >= 6000);   // at least half a second matched
        int off = 0;
        QString firstOff;
        for (int i = 0; i < n; ++i)
            if (qAbs(int(heard[start + i]) - int(sent[sentStart + i])) > 1)   // a rounding step at most
            {
                if (++off <= 6)
                    firstOff += QStringLiteral(" [%1] %2 vs %3").arg(i).arg(heard[start + i]).arg(sent[sentStart + i]);
            }
        QVERIFY2(off == 0, qPrintable(QStringLiteral("%1 of %2 samples differ:%3").arg(off).arg(n).arg(firstOff)));
        p.stop();
        QCOMPARE(p.audioInHand(), 0.0);   // nothing in hand once stopped, and said so
        QCOMPARE(inHand.last().first().toDouble(), 0.0);
    }

    // A receiver with an unusual rate: the sink takes that rate, the sound
    // server does the converting.
    void unusualRateIsTakenAsItIs()
    {
        FakeKiwi kiwi;
        kiwi.audioRate = 20250;
        KiwiPlayer p;
        QSignalSpy status(&p, &KiwiPlayer::statusChanged);
        startPlaying(p, kiwi, status);
        const QList<qint16> t = tone(20250);
        for (int at = 0; at < t.size(); at += 810)
        {
            FakeKiwi::sendPcm(kiwi.peers.first(), t.mid(at, 810));
            QTest::qWait(40);
        }
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).endsWith(QLatin1String(": playing")), 5000);
        auto* sink = p.findChild<QAudioSink*>();
        QVERIFY(sink);
        QCOMPARE(sink->format().sampleRate(), 20250);
        QCOMPARE(sink->format().channelCount(), 1);
        QCOMPARE(sink->state(), QAudio::ActiveState);
        QVERIFY(p.isPlaying());
        p.stop();
    }

    // A rate the sound server refuses: the device's own format is opened
    // instead and the receiver's audio converted to it, and the session goes
    // on (as on a Windows that converts nothing itself).
    void refusedRateFallsBackToTheDeviceFormat()
    {
        FakeKiwi kiwi;
        kiwi.audioRate = 999999;   // no sound server takes that
        KiwiPlayer p;
        QSignalSpy status(&p, &KiwiPlayer::statusChanged);
        startPlaying(p, kiwi, status);
        for (int i = 0; i < 20; ++i)
        {
            FakeKiwi::sendPcm(kiwi.peers.first(), tone(12000).mid(0, 4800));
            QTest::qWait(40);
        }
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).endsWith(QLatin1String(": playing")), 5000);
        auto* sink = p.findChild<QAudioSink*>();
        QVERIFY(sink);
        QCOMPARE(sink->format(), QMediaDevices::defaultAudioOutput().preferredFormat());
        QCOMPARE(sink->format().sampleRate(), 12000);   // the test server's sink
        QCOMPARE(sink->state(), QAudio::ActiveState);
        QVERIFY(p.isPlaying());
        QCOMPARE(sinkInputs(), 1);
        QVERIFY(!lastStatus(status).startsWith(QLatin1String("Stopped")));
        p.stop();
    }

    // Stop gives the device back; play again opens it afresh.
    void stopReleasesTheDevice()
    {
        FakeKiwi kiwi;
        KiwiPlayer p;
        QSignalSpy status(&p, &KiwiPlayer::statusChanged);
        startPlaying(p, kiwi, status);
        FakeKiwi::sendPcm(kiwi.peers.first(), tone(12000).mid(0, 4800));
        QTRY_VERIFY_WITH_TIMEOUT(p.findChild<QAudioSink*>() != nullptr, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(sinkInputs(), 1, 5000);
        playButton(p)->click();   // stop
        QVERIFY(!p.isPlaying());
        QVERIFY(!p.findChild<QAudioSink*>());
        QTRY_COMPARE_WITH_TIMEOUT(sinkInputs(), 0, 5000);
        QCOMPARE(lastStatus(status), QString());

        playButton(p)->click();   // and again
        QTRY_COMPARE_WITH_TIMEOUT(kiwi.peers.size(), 2, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).startsWith(QLatin1String("Listening on")), 5000);
        FakeKiwi::sendPcm(kiwi.peers.last(), tone(12000).mid(0, 4800));
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).endsWith(QLatin1String(": playing")), 5000);
        QCOMPARE(sinkInputs(), 1);
        p.stop();
    }

    // Switching to another receiver while playing: the sound follows, one
    // stream at a time.
    void switchingReceiversKeepsTheSound()
    {
        FakeKiwi a, b;
        KiwiPlayer p;
        QSignalSpy status(&p, &KiwiPlayer::statusChanged);
        p.setReceivers({a.address(), b.address()}, {}, a.address());
        playButton(p)->click();
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).startsWith(QLatin1String("Listening on")), 5000);
        FakeKiwi::sendPcm(a.peers.first(), tone(12000).mid(0, 4800));
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).endsWith(QLatin1String(": playing")), 5000);

        QComboBox* list = nullptr;
        for (QComboBox* box : p.findChildren<QComboBox*>())
            if (box->findData(b.address()) >= 0)
                list = box;
        QVERIFY(list);
        list->setCurrentIndex(list->findData(b.address()));
        emit list->activated(list->currentIndex());
        QTRY_COMPARE_WITH_TIMEOUT(b.peers.size(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(a.peers.first()->state() == QAbstractSocket::UnconnectedState, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).startsWith(QLatin1String("Listening on")), 5000);
        FakeKiwi::sendPcm(b.peers.first(), tone(12000).mid(0, 4800));
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).endsWith(QLatin1String(": playing")), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(sinkInputs(), 1, 5000);
        QVERIFY(p.isPlaying());
        p.stop();
    }
};

QTEST_MAIN(TestKiwiSound)
#include "test_kiwisound.moc"
