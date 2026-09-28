// SPDX-License-Identifier: GPL-3.0-or-later
// RigClient against a small fake rigctld on localhost (fakerigctld.h).
#include "core/RigClient.h"
#include "fakerigctld.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QtTest>

#include <memory>

class TestRigClient : public QObject
{
    Q_OBJECT
private slots:
    // The watchdog must stay quiet while the rig answers, even when the
    // frequency does not change (the case that showed "not answering").
    void steadyFrequencyKeepsAnswering()
    {
        FakeRigctld rig;
        RigClient client;
        client.setPollInterval(100);
        client.setSilenceTimeout(1000);
        client.setEndpoint(QStringLiteral("127.0.0.1"), rig.port());
        QSignalSpy answering(&client, &RigClient::answeringChanged);
        QSignalSpy freq(&client, &RigClient::frequencyChanged);
        client.start();
        QTRY_VERIFY_WITH_TIMEOUT(client.isAnswering(), 3000);
        QTest::qWait(3000);   // three silence timeouts' worth of unchanged answers
        QVERIFY(client.isAnswering());
        QCOMPARE(answering.count(), 1);
        QCOMPARE(answering.first().first().toBool(), true);
        QCOMPARE(freq.count(), 1);
        QCOMPARE(freq.first().first().toLongLong(), 7125000);
        client.stop();
    }

    // rigctld up but the radio off: the connection works, every poll gets an
    // error report. That is not "answering", and it must not flap.
    void errorReportsAreNotAnswers()
    {
        FakeRigctld rig;
        rig.radioOn = false;
        RigClient client;
        client.setPollInterval(100);
        client.setSilenceTimeout(400);
        client.setEndpoint(QStringLiteral("127.0.0.1"), rig.port());
        QSignalSpy answering(&client, &RigClient::answeringChanged);
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(answering.count(), 1, 3000);
        QCOMPARE(answering.first().first().toBool(), false);
        QTest::qWait(1500);
        QVERIFY(!client.isAnswering());
        QCOMPARE(answering.count(), 1);   // said once, not over and over
        client.stop();
    }

    // Some rigs answer "0" for the frequency while the radio is off. That is
    // not an answer either, and not a frequency.
    void zeroFrequencyIsNotAnAnswer()
    {
        FakeRigctld rig;
        rig.radioOn = false;
        rig.zeroWhenOff = true;
        RigClient client;
        client.setPollInterval(100);
        client.setSilenceTimeout(400);
        client.setEndpoint(QStringLiteral("127.0.0.1"), rig.port());
        QSignalSpy answering(&client, &RigClient::answeringChanged);
        QSignalSpy freq(&client, &RigClient::frequencyChanged);
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(answering.count(), 1, 3000);
        QTest::qWait(1200);   // three silence timeouts of "0" answers
        QVERIFY(rig.received.count(QStringLiteral("f")) >= 3);
        QCOMPARE(answering.count(), 1);
        QCOMPARE(answering.first().first().toBool(), false);
        QCOMPARE(freq.count(), 0);
        QVERIFY(!client.isAnswering());
        client.stop();
    }

    // The radio goes off and on again on the same frequency: silence is
    // reported, and the frequency is reported again when it comes back.
    void silenceAndReturnOnSameFrequency()
    {
        FakeRigctld rig;
        RigClient client;
        client.setPollInterval(100);
        client.setSilenceTimeout(1000);
        client.setEndpoint(QStringLiteral("127.0.0.1"), rig.port());
        QSignalSpy answering(&client, &RigClient::answeringChanged);
        QSignalSpy freq(&client, &RigClient::frequencyChanged);
        client.start();
        QTRY_VERIFY_WITH_TIMEOUT(client.isAnswering(), 3000);
        rig.radioOn = false;
        QTRY_VERIFY_WITH_TIMEOUT(!client.isAnswering(), 5000);
        rig.radioOn = true;
        QTRY_VERIFY_WITH_TIMEOUT(client.isAnswering(), 3000);
        QCOMPARE(answering.count(), 3);   // true, false, true
        QCOMPARE(freq.count(), 2);        // the same 7125000, reported again
        client.stop();
    }

    // No rigctld at all: one "not answering" after the timeout.
    void nothingListening()
    {
        QTcpServer probe;
        probe.listen(QHostAddress::LocalHost);
        const quint16 deadPort = probe.serverPort();
        probe.close();
        RigClient client;
        client.setPollInterval(100);
        client.setSilenceTimeout(400);
        client.setEndpoint(QStringLiteral("127.0.0.1"), deadPort);
        QSignalSpy answering(&client, &RigClient::answeringChanged);
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(answering.count(), 1, 3000);
        QCOMPARE(answering.first().first().toBool(), false);
        client.stop();
    }

    // A shorter silence timeout takes effect at once, not only after the
    // next answer (which may never come).
    void silenceTimeoutShortenedWhileWaiting()
    {
        QTcpServer probe;
        probe.listen(QHostAddress::LocalHost);
        const quint16 deadPort = probe.serverPort();
        probe.close();
        RigClient client;
        client.setPollInterval(100);   // default silence timeout: 5 s
        client.setEndpoint(QStringLiteral("127.0.0.1"), deadPort);
        QSignalSpy answering(&client, &RigClient::answeringChanged);
        client.start();
        client.setSilenceTimeout(300);
        QTRY_COMPARE_WITH_TIMEOUT(answering.count(), 1, 3000);
        QCOMPARE(answering.first().first().toBool(), false);
        client.stop();
    }

    // Raising the poll interval while the rig answers must not trip the
    // watchdog: with the default silence timeout it would still count down
    // the old five seconds, but the next poll only comes after six.
    void pollIntervalRaisedWhileAnswering()
    {
        FakeRigctld rig;
        RigClient client;
        client.setPollInterval(100);   // default silence timeout: 5 s
        client.setEndpoint(QStringLiteral("127.0.0.1"), rig.port());
        QSignalSpy answering(&client, &RigClient::answeringChanged);
        // Raise it right after an answer, so that no poll is in flight and
        // the next one goes out a whole new interval later.
        connect(&client, &RigClient::frequencyChanged, &client, [&](qint64 hz) {
            if (hz == 7200000)
            {
                client.setPollInterval(6000);
                rig.hz = 9500000;   // what that next poll will find
            }
        });
        client.start();
        QTRY_VERIFY_WITH_TIMEOUT(client.isAnswering(), 3000);
        rig.hz = 7200000;
        QTRY_COMPARE_WITH_TIMEOUT(client.frequencyHz(), qint64(9500000), 15000);
        QCOMPARE(answering.count(), 1);   // the first true, no false since
        QVERIFY(client.isAnswering());
        client.stop();
    }

    // A rig that answers "f" but fails "m": the error report stands in for
    // both lines of the mode answer, so later answers still line up with
    // the questions and a new frequency is read as a frequency.
    void modeErrorKeepsQueueAligned()
    {
        FakeRigctld rig;
        rig.modeFails = true;
        RigClient client;
        client.setPollInterval(100);
        client.setSilenceTimeout(1000);
        client.setEndpoint(QStringLiteral("127.0.0.1"), rig.port());
        QSignalSpy answering(&client, &RigClient::answeringChanged);
        QSignalSpy freq(&client, &RigClient::frequencyChanged);
        QSignalSpy mode(&client, &RigClient::modeChanged);
        client.start();
        QTRY_VERIFY_WITH_TIMEOUT(client.isAnswering(), 3000);
        // several polls with a failed mode go by ...
        QTRY_VERIFY_WITH_TIMEOUT(rig.received.count(QStringLiteral("m")) >= 3, 3000);
        // ... and the next change of frequency still comes through
        rig.hz = 9500000;
        QTRY_COMPARE_WITH_TIMEOUT(client.frequencyHz(), qint64(9500000), 3000);
        QCOMPARE(freq.count(), 2);
        QCOMPARE(freq.at(0).first().toLongLong(), 7125000);
        QCOMPARE(freq.at(1).first().toLongLong(), 9500000);
        QCOMPARE(answering.count(), 1);
        QVERIFY(client.isAnswering());
        QCOMPARE(mode.count(), 0);
        client.stop();
    }

    // Settings point the client elsewhere while nothing answers: the new
    // address gets its own "not answering", and a rig there is found.
    void endpointChangeReportsAgain()
    {
        FakeRigctld rig;   // first, so that the probes cannot get its port
        QTcpServer probeA, probeB;
        probeA.listen(QHostAddress::LocalHost);
        probeB.listen(QHostAddress::LocalHost);
        const quint16 portA = probeA.serverPort();
        const quint16 portB = probeB.serverPort();
        probeA.close();
        probeB.close();
        RigClient client;
        client.setPollInterval(100);
        client.setSilenceTimeout(400);
        client.setEndpoint(QStringLiteral("127.0.0.1"), portA);
        QSignalSpy answering(&client, &RigClient::answeringChanged);
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(answering.count(), 1, 3000);
        QCOMPARE(answering.at(0).first().toBool(), false);

        // the same address again is no change: nothing more is said
        client.setEndpoint(QStringLiteral("127.0.0.1"), portA);
        QTest::qWait(1200);
        QCOMPARE(answering.count(), 1);

        // another dead address: silent at first, then "not answering" again
        client.setEndpoint(QStringLiteral("127.0.0.1"), portB);
        QCOMPARE(answering.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(answering.count(), 2, 3000);
        QCOMPARE(answering.at(1).first().toBool(), false);

        // a live one: answering, with no "not answering" first
        client.setEndpoint(QStringLiteral("127.0.0.1"), rig.port());
        QTRY_COMPARE_WITH_TIMEOUT(answering.count(), 3, 3000);
        QCOMPARE(answering.at(2).first().toBool(), true);
        QVERIFY(client.isAnswering());
        client.stop();
    }

    // From one answering rig to another: no "not answering" in between, and
    // the new rig's frequency is reported.
    void endpointChangeBetweenRigsDoesNotWarn()
    {
        FakeRigctld rigA;
        FakeRigctld rigB;
        rigB.hz = 9500000;
        RigClient client;
        client.setPollInterval(100);
        client.setSilenceTimeout(1000);
        client.setEndpoint(QStringLiteral("127.0.0.1"), rigA.port());
        QSignalSpy answering(&client, &RigClient::answeringChanged);
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.frequencyHz(), qint64(7125000), 3000);
        client.setEndpoint(QStringLiteral("127.0.0.1"), rigB.port());
        QTRY_COMPARE_WITH_TIMEOUT(client.frequencyHz(), qint64(9500000), 3000);
        QCOMPARE(answering.count(), 1);   // the first true only
        QVERIFY(client.isAnswering());
        client.stop();
    }

    // Commands reach the rig in the order they are called, and both are
    // carried out. Which one to send first when tuning is decided in
    // MainWindow::tuneTo; the MainWindow test covers that.
    void commandsArriveInCallOrder()
    {
        FakeRigctld rig;
        RigClient client;
        client.setPollInterval(100);
        client.setEndpoint(QStringLiteral("127.0.0.1"), rig.port());
        client.start();
        QTRY_VERIFY_WITH_TIMEOUT(client.isAnswering(), 3000);
        client.setMode(QStringLiteral("AM"));
        client.setFrequency(9500000);
        QTRY_COMPARE_WITH_TIMEOUT(rig.hz, qint64(9500000), 3000);
        const int m = rig.received.indexOf(QStringLiteral("M AM 0"));
        const int f = rig.received.indexOf(QStringLiteral("F 9500000"));
        QVERIFY(m >= 0);
        QVERIFY(f > m);
        QCOMPARE(rig.mode, QStringLiteral("AM"));
        client.stop();
    }

    // stop() ends the answering state and says so.
    void stopReportsNotAnswering()
    {
        FakeRigctld rig;
        RigClient client;
        client.setPollInterval(100);
        client.setEndpoint(QStringLiteral("127.0.0.1"), rig.port());
        client.start();
        QTRY_VERIFY_WITH_TIMEOUT(client.isAnswering(), 3000);
        QSignalSpy answering(&client, &RigClient::answeringChanged);
        client.stop();
        QVERIFY(!client.isAnswering());
        QCOMPARE(answering.count(), 1);
        QCOMPARE(answering.first().first().toBool(), false);
    }

    // Deleting a connected client without stop(): the socket closes as the
    // client goes, and nothing may run or be emitted on the way out.
    void destroyWhileConnected()
    {
        FakeRigctld rig;
        // a unique_ptr, so that a failed check below still deletes it
        std::unique_ptr<RigClient> client(new RigClient);
        client->setPollInterval(100);
        client->setEndpoint(QStringLiteral("127.0.0.1"), rig.port());
        client->start();
        QTRY_VERIFY_WITH_TIMEOUT(client->isAnswering(), 3000);
        QVERIFY(client->isConnected());
        QSignalSpy state(client.get(), &RigClient::stateChanged);
        QSignalSpy answering(client.get(), &RigClient::answeringChanged);
        client.reset();   // delete, still connected
        QCOMPARE(state.count(), 0);
        QCOMPARE(answering.count(), 0);
    }
};

QTEST_GUILESS_MAIN(TestRigClient)
#include "test_rigclient.moc"
