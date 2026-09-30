// SPDX-License-Identifier: GPL-3.0-or-later
// KiwiPlayer against a fake KiwiSDR on localhost: how a session starts and
// ends, what the status line says afterwards, which receiver the list names
// and which mode the receiver is asked for.
#include "KiwiAudio.h"
#include "KiwiPlayer.h"

#include <QApplication>
#include <QAudioDevice>
#include <QComboBox>
#include <QDesktopServices>
#include <QInputDialog>
#include <QLineEdit>
#include <QMediaDevices>
#include <QSet>
#include <QSignalSpy>
#include <QTimer>
#include <QToolButton>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtEndian>
#include <QtTest>

namespace
{
// A fake receiver (the one from test_kiwi.cpp). Each connection is scripted
// by the test through the hooks; everything the client sends is recorded.
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

QToolButton* playButton(KiwiPlayer& p)
{
    for (QToolButton* b : p.findChildren<QToolButton*>())
        if (b->toolTip() == KiwiPlayer::tr("Start or stop listening"))
            return b;
    return nullptr;
}

QComboBox* modeBox(KiwiPlayer& p)
{
    for (QComboBox* b : p.findChildren<QComboBox*>())
        if (b->findText(QStringLiteral("USB")) >= 0)
            return b;
    return nullptr;
}

QComboBox* receiverList(KiwiPlayer& p)
{
    for (QComboBox* b : p.findChildren<QComboBox*>())
        if (b != modeBox(p))
            return b;
    return nullptr;
}

QLineEdit* searchBox(KiwiPlayer& p)
{
    return p.findChild<QLineEdit*>();
}

QToolButton* starButton(KiwiPlayer& p)
{
    for (QToolButton* b : p.findChildren<QToolButton*>())
        if (b->toolTip().contains(QLatin1String("star"), Qt::CaseInsensitive))
            return b;
    return nullptr;
}

QToolButton* addButton(KiwiPlayer& p)
{
    for (QToolButton* b : p.findChildren<QToolButton*>())
        if (b->text() == QLatin1String("+"))
            return b;
    return nullptr;
}

// how many entries of the list lead to this address
int entriesFor(QComboBox* list, const QString& url)
{
    int n = 0;
    for (int i = 0; i < list->count(); ++i)
        if (list->itemData(i).toString() == url)
            ++n;
    return n;
}

// Types an address into the dialog that + opens, as soon as it is up.
void answerAddDialog(const QString& address)
{
    QTimer::singleShot(0, [address]() {
        auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        dialog->setTextValue(address);
        dialog->accept();
    });
}

QString lastStatus(const QSignalSpy& spy)
{
    return spy.isEmpty() ? QStringLiteral("<none>") : spy.last().first().toString();
}

KiwiDirectory::Receiver receiver(const QString& url, const QString& location, bool offline = false)
{
    KiwiDirectory::Receiver r;
    r.url = url;
    r.name = QStringLiteral("SDR");
    r.location = location;
    r.usersMax = 4;
    r.offline = offline;
    return r;
}
}

// stands in for the browser: QDesktopServices hands the address to it
class UrlCatcher : public QObject
{
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void handle(const QUrl& url) { urls << url; }
};

class TestKiwiPlayer : public QObject
{
    Q_OBJECT
private slots:
    // Audio arrives but there is nowhere to play it: the reason stays on
    // the status line, nothing that follows wipes it.
    void noSoundDeviceStatusStays()
    {
        if (!QMediaDevices::defaultAudioOutput().isNull())
            QSKIP("this machine has a sound output; the no-device path needs none");
        FakeKiwi kiwi;
        KiwiPlayer p;
        p.setReceivers({kiwi.address()}, {}, kiwi.address());
        QSignalSpy status(&p, &KiwiPlayer::statusChanged);
        QToolButton* play = playButton(p);
        QVERIFY(play);
        play->click();
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).startsWith(QLatin1String("Listening on")), 5000);
        FakeKiwi::sendSnd(kiwi.peers.first(), 0x10, 1270 - 730, QByteArray::fromHex("0077f812"));
        QTRY_COMPARE_WITH_TIMEOUT(lastStatus(status), QStringLiteral("No sound output device"), 5000);
        QTest::qWait(500);   // the connection's end has long arrived by now
        QCOMPARE(lastStatus(status), QStringLiteral("No sound output device"));
        QVERIFY(!p.isPlaying());
        QCOMPARE(p.audioInHand(), 0.0);
        // the receiver's channel is given back
        QTRY_VERIFY_WITH_TIMEOUT(kiwi.peers.first()->state() == QAbstractSocket::UnconnectedState, 5000);
    }

    // Stop on request: the status line goes quiet and stays so.
    void stopOnRequestClearsStatus()
    {
        FakeKiwi kiwi;
        KiwiPlayer p;
        p.setReceivers({kiwi.address()}, {}, kiwi.address());
        QSignalSpy status(&p, &KiwiPlayer::statusChanged);
        QToolButton* play = playButton(p);
        QVERIFY(play);
        play->click();
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).startsWith(QLatin1String("Listening on")), 5000);
        play->click();
        QVERIFY(!p.isPlaying());
        QCOMPARE(lastStatus(status), QString());
        QTest::qWait(500);
        QCOMPARE(lastStatus(status), QString());
    }

    // The receiver hangs up: the reason is shown once and stays.
    void hangUpShowsReason()
    {
        FakeKiwi kiwi;
        KiwiPlayer p;
        p.setReceivers({kiwi.address()}, {}, kiwi.address());
        QSignalSpy status(&p, &KiwiPlayer::statusChanged);
        QToolButton* play = playButton(p);
        QVERIFY(play);
        play->click();
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).startsWith(QLatin1String("Listening on")), 5000);
        kiwi.peers.first()->close();
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).startsWith(QLatin1String("Stopped: ")), 5000);
        const QString shown = lastStatus(status);
        QVERIFY(shown.size() > QStringLiteral("Stopped: ").size());
        QTest::qWait(500);
        QCOMPARE(lastStatus(status), shown);
        QVERIFY(!p.isPlaying());
    }

    // Each start asks once for the receiver list to be looked at; stopping
    // does not, nor does a start that finds no receiver to play.
    void playRequestedOncePerStart()
    {
        FakeKiwi kiwi, other;
        KiwiPlayer p;
        p.setReceivers({kiwi.address(), other.address()}, {}, kiwi.address());
        QSignalSpy requested(&p, &KiwiPlayer::playRequested);
        QToolButton* play = playButton(p);
        QVERIFY(play);
        play->click();
        QCOMPARE(requested.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(kiwi.peers.size() == 1, 5000);
        // picking another receiver while playing switches over: one more start
        QComboBox* list = nullptr;
        for (QComboBox* b : p.findChildren<QComboBox*>())
            if (b != modeBox(p))
                list = b;
        QVERIFY(list);
        const int idx = list->findData(other.address());
        QVERIFY(idx >= 0);
        list->setCurrentIndex(idx);
        emit list->activated(idx);   // as a pick in the list does
        QCOMPARE(requested.count(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(other.peers.size() == 1, 5000);
        QVERIFY(p.isPlaying());
        play->click();   // stop
        QVERIFY(!p.isPlaying());
        QCOMPARE(requested.count(), 2);

        KiwiPlayer empty;
        QSignalSpy none(&empty, &KiwiPlayer::playRequested);
        QSignalSpy status(&empty, &KiwiPlayer::statusChanged);
        QToolButton* play2 = playButton(empty);
        QVERIFY(play2);
        play2->click();
        QVERIFY(!empty.isPlaying());
        QCOMPARE(none.count(), 0);
        QVERIFY(lastStatus(status).startsWith(QLatin1String("Pick a receiver")));
    }

    // A fresh install has no receiver chosen: when the directory arrives,
    // one is picked at random among the receivers on line with a channel
    // free that cover the tuned frequency, and remembered.
    void freshInstallPicksAFreeReceiver()
    {
        const QString a = QStringLiteral("http://a.example:8073");
        const QString b = QStringLiteral("http://b.example:8073");
        const QString c = QStringLiteral("http://c.example:8073");
        const QString d = QStringLiteral("http://d.example:8073");
        const QString e = QStringLiteral("http://e.example:8073");
        KiwiDirectory::Receiver full = receiver(c, QStringLiteral("Cork"));
        full.users = full.usersMax;
        KiwiDirectory::Receiver low = receiver(d, QStringLiteral("Dover"));
        low.highKHz = 3000;   // does not reach 4625
        const QList<KiwiDirectory::Receiver> directory = {
            receiver(a, QStringLiteral("Aland")), receiver(b, QStringLiteral("Bergen"), true), full, low,
            receiver(e, QStringLiteral("Essen"))};
        QSet<QString> picks;
        for (int i = 0; i < 40; ++i)
        {
            KiwiPlayer p;
            QSignalSpy changed(&p, &KiwiPlayer::receiversChanged);
            p.setReceivers({}, {}, QString());   // nothing chosen yet
            p.tune(4625, QString());
            QVERIFY(p.currentReceiver().isEmpty());
            p.setDirectory(directory);
            const QString pick = p.currentReceiver();
            QVERIFY2(pick == a || pick == e, qPrintable(pick));
            QCOMPARE(changed.size(), 1);   // remembered
            picks << pick;
            // and it stays across later directory updates
            p.setDirectory(directory);
            QCOMPARE(p.currentReceiver(), pick);
            QCOMPARE(changed.size(), 1);
        }
        QCOMPARE(picks.size(), 2);   // random: both turned up in forty tries

        // a saved choice is never replaced
        KiwiPlayer p;
        p.setReceivers({}, {}, c);
        p.setDirectory(directory);
        QCOMPARE(p.currentReceiver(), c);
    }

    // A directory update that calls the chosen receiver offline, or leaves
    // it out, keeps it chosen: the list goes on naming the receiver being
    // heard, and playing goes on undisturbed.
    void directoryUpdateKeepsCurrentReceiver()
    {
        FakeKiwi kiwi;
        const QString a = QStringLiteral("http://a.example:8073");   // never connected to
        const QString b = kiwi.address();
        const QString c = QStringLiteral("http://c.example:8073");

        // chosen but not playing
        {
            KiwiPlayer p;
            p.setDirectory({receiver(a, QStringLiteral("Aland")), receiver(b, QStringLiteral("Bergen"))});
            p.setReceivers({}, {}, b);
            QCOMPARE(p.currentReceiver(), b);
            p.setDirectory({receiver(a, QStringLiteral("Aland")), receiver(b, QStringLiteral("Bergen"), true)});
            QCOMPARE(p.currentReceiver(), b);
            p.setDirectory({receiver(a, QStringLiteral("Aland")), receiver(c, QStringLiteral("Cork"))});
            QCOMPARE(p.currentReceiver(), b);
            // once the directory lists it again, it is shown with its label
            p.setDirectory({receiver(a, QStringLiteral("Aland")), receiver(b, QStringLiteral("Bergen"))});
            QCOMPARE(p.currentReceiver(), b);
        }

        // playing
        KiwiPlayer p;
        p.setDirectory({receiver(a, QStringLiteral("Aland")), receiver(b, QStringLiteral("Bergen"))});
        p.setReceivers({}, {}, b);
        QSignalSpy status(&p, &KiwiPlayer::statusChanged);
        QToolButton* play = playButton(p);
        QVERIFY(play);
        play->click();
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).startsWith(QLatin1String("Listening on")), 5000);
        QCOMPARE(kiwi.peers.size(), 1);

        p.setDirectory({receiver(a, QStringLiteral("Aland")), receiver(b, QStringLiteral("Bergen"), true)});
        QCOMPARE(p.currentReceiver(), b);
        p.setDirectory({receiver(a, QStringLiteral("Aland")), receiver(c, QStringLiteral("Cork"))});
        QCOMPARE(p.currentReceiver(), b);
        QTest::qWait(300);   // a restart would have reached the receiver by now
        QVERIFY(p.isPlaying());
        QCOMPARE(kiwi.peers.size(), 1);
        QCOMPARE(kiwi.peers.first()->state(), QAbstractSocket::ConnectedState);
        QVERIFY(lastStatus(status).startsWith(QLatin1String("Listening on")));
    }

    // While a rig is followed the box shows the rig's mode, greyed. When
    // the rig lets go, the listener's own mode comes back, and a mode the
    // rig chose is never reported as the listener's choice.
    void modeReturnsToListenerChoice()
    {
        FakeKiwi kiwi;
        KiwiPlayer p;
        p.setReceivers({kiwi.address()}, {}, kiwi.address());
        QComboBox* box = modeBox(p);
        QVERIFY(box);
        QSignalSpy chosen(&p, &KiwiPlayer::manualModeChanged);
        p.setManualMode(QStringLiteral("AM"));   // the stored choice
        p.tune(7125, QString());
        QToolButton* play = playButton(p);
        QVERIFY(play);
        play->click();
        QTRY_VERIFY_WITH_TIMEOUT(kiwi.last(QStringLiteral("SET mod=")).startsWith(QLatin1String("SET mod=am ")), 5000);

        p.tune(7125, QStringLiteral("USB"));
        QCOMPARE(box->currentText(), QStringLiteral("USB"));
        QVERIFY(!box->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(kiwi.last(QStringLiteral("SET mod=")).startsWith(QLatin1String("SET mod=usb ")), 5000);

        p.tune(7125, QString());
        QCOMPARE(box->currentText(), QStringLiteral("AM"));
        QVERIFY(box->isEnabled());
        QCOMPARE(p.manualMode(), QStringLiteral("AM"));
        QTRY_VERIFY_WITH_TIMEOUT(kiwi.last(QStringLiteral("SET mod=")).startsWith(QLatin1String("SET mod=am ")), 5000);

        // the listener picks LSB; the rig takes over and lets go again
        box->setCurrentText(QStringLiteral("LSB"));
        emit box->activated(box->currentIndex());   // as a pick in the box does
        QCOMPARE(chosen.count(), 1);
        QCOMPARE(chosen.last().first().toString(), QStringLiteral("LSB"));
        QTRY_VERIFY_WITH_TIMEOUT(kiwi.last(QStringLiteral("SET mod=")).startsWith(QLatin1String("SET mod=lsb ")), 5000);
        p.tune(7125, QStringLiteral("USB"));
        QCOMPARE(box->currentText(), QStringLiteral("USB"));
        QCOMPARE(p.manualMode(), QStringLiteral("LSB"));
        QTRY_VERIFY_WITH_TIMEOUT(kiwi.last(QStringLiteral("SET mod=")).startsWith(QLatin1String("SET mod=usb ")), 5000);
        // the stored choice may be handed in again while the rig leads
        p.setManualMode(QStringLiteral("LSB"));
        QCOMPARE(box->currentText(), QStringLiteral("USB"));
        p.tune(7125, QString());
        QCOMPARE(box->currentText(), QStringLiteral("LSB"));
        QTRY_VERIFY_WITH_TIMEOUT(kiwi.last(QStringLiteral("SET mod=")).startsWith(QLatin1String("SET mod=lsb ")), 5000);

        // only the listener's pick was reported, never the rig's USB
        QCOMPARE(chosen.count(), 1);
        for (const QList<QVariant>& args : chosen)
            QVERIFY(args.first().toString() != QLatin1String("USB"));
    }

    // The sound device fails: the session ends as when the receiver hangs
    // up, with the reason on the status line. The teardown waits until the
    // sink's signal is over, and a report that comes after the session has
    // ended another way changes nothing.
    void soundDeviceFailureEndsSession()
    {
        FakeKiwi kiwi;
        KiwiPlayer p;
        auto* audio = p.findChild<KiwiAudio*>();
        QVERIFY(audio);
        p.setReceivers({kiwi.address()}, {}, kiwi.address());
        QSignalSpy status(&p, &KiwiPlayer::statusChanged);
        QToolButton* play = playButton(p);
        QVERIFY(play);
        play->click();
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).startsWith(QLatin1String("Listening on")), 5000);

        // ordinary states only show on the status line
        audio->onSinkState(QAudio::IdleState, QAudio::UnderrunError);
        audio->onSinkState(QAudio::ActiveState, QAudio::NoError);
        QTest::qWait(300);
        QVERIFY(p.isPlaying());

        audio->onSinkState(QAudio::StoppedState, QAudio::IOError);
        QVERIFY(p.isPlaying());   // not inside the sink's signal
        QTRY_VERIFY_WITH_TIMEOUT(!p.isPlaying(), 5000);
        QCOMPARE(lastStatus(status), QStringLiteral("Stopped: sound device error"));
        QTRY_VERIFY_WITH_TIMEOUT(kiwi.peers.first()->state() == QAbstractSocket::UnconnectedState, 5000);

        // a failure reported just before a stop and a fresh start does not
        // end the new session
        play->click();
        QTRY_COMPARE_WITH_TIMEOUT(kiwi.peers.size(), 2, 5000);
        audio->onSinkState(QAudio::StoppedState, QAudio::OpenError);
        play->click();   // stop
        play->click();   // and start again at once
        QTest::qWait(300);   // the queued report has long been handled
        QVERIFY(p.isPlaying());
        QTRY_VERIFY_WITH_TIMEOUT(lastStatus(status).startsWith(QLatin1String("Listening on")), 5000);
    }

    // The arrow button next to the star opens the chosen receiver's own web
    // page in the browser, with a scheme even for a bare host:port; without
    // a receiver it is greyed.
    void openButtonShowsTheReceiverPage()
    {
        KiwiPlayer p;
        auto* open = p.findChild<QToolButton*>(QStringLiteral("openReceiverPage"));
        QVERIFY(open);
        QVERIFY(!open->isEnabled());   // nothing chosen yet

        UrlCatcher catcher;   // catches what would go to the browser
        QDesktopServices::setUrlHandler(QStringLiteral("http"), &catcher, "handle");

        p.setReceivers({QStringLiteral("192.168.1.50:8073"), QStringLiteral("http://kiwi.example:8074")}, {},
                       QStringLiteral("192.168.1.50:8073"));
        QVERIFY(open->isEnabled());
        QVERIFY(open->toolTip().contains(QLatin1String("192.168.1.50:8073")));
        open->click();
        QCOMPARE(catcher.urls.size(), 1);
        QCOMPARE(catcher.urls.first().toString(), QStringLiteral("http://192.168.1.50:8073"));

        QComboBox* list = receiverList(p);
        QVERIFY(list);
        list->setCurrentIndex(list->findData(QStringLiteral("http://kiwi.example:8074")));
        emit list->activated(list->currentIndex());   // as a pick in the list does (and starts it)
        open->click();
        QCOMPARE(catcher.urls.size(), 2);
        QCOMPARE(catcher.urls.last().toString(), QStringLiteral("http://kiwi.example:8074"));
        p.stop();
        QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
    }

    // A search that hides the receiver being heard does not change which
    // receiver the list names: the star, + and the saved choice keep acting
    // on the one playing, and the matches are there to pick from.
    void searchKeepsCurrentReceiver()
    {
        FakeKiwi kiwi;
        const QString a = QStringLiteral("http://a.example:8073");
        const QString b = kiwi.address();
        KiwiPlayer p;
        p.setDirectory({receiver(a, QStringLiteral("Aland")), receiver(b, QStringLiteral("Bergen"))});
        p.setReceivers({}, {}, b);
        QToolButton* play = playButton(p);
        QComboBox* list = receiverList(p);
        QLineEdit* search = searchBox(p);
        QToolButton* star = starButton(p);
        QVERIFY(play && list && search && star);
        play->click();
        QTRY_COMPARE_WITH_TIMEOUT(kiwi.peers.size(), 1, 5000);

        search->setText(QStringLiteral("aland"));
        QCOMPARE(p.currentReceiver(), b);
        QCOMPARE(list->currentData().toString(), b);
        QCOMPARE(entriesFor(list, a), 1);   // the match is offered
        QCOMPARE(entriesFor(list, b), 1);
        star->click();
        QCOMPARE(p.favourites(), QStringList{b});
        QCOMPARE(p.currentReceiver(), b);
        QVERIFY(p.isPlaying());
        QCOMPARE(kiwi.peers.size(), 1);
    }

    // + while a search is typed plays the address just added, and a plain
    // host name without a dot (a receiver on the home network) is taken.
    void addWhileSearchingPlaysTheNewAddress()
    {
        FakeKiwi mine;
        const QString a = QStringLiteral("http://a.example:8073");
        const QString added = QStringLiteral("localhost:%1").arg(mine.server.serverPort());
        KiwiPlayer p;
        p.setDirectory({receiver(a, QStringLiteral("Aland"))});
        p.setReceivers({}, {}, a);
        QLineEdit* search = searchBox(p);
        QToolButton* add = addButton(p);
        QVERIFY(search && add);
        search->setText(QStringLiteral("aland"));
        answerAddDialog(added);
        add->click();
        QCOMPARE(p.currentReceiver(), added);
        QCOMPARE(p.receivers(), QStringList{added});
        QTRY_COMPARE_WITH_TIMEOUT(mine.peers.size(), 1, 5000);
        QVERIFY(p.isPlaying());
        p.stop();

        // the dialog's untouched "http://" is no address: nothing happens
        answerAddDialog(QStringLiteral("http://"));
        add->click();
        QCOMPARE(p.receivers(), QStringList{added});
        QCOMPARE(p.currentReceiver(), added);
    }

    // A directory receiver that is off the list for a while can still be
    // played, but is not made one of the listener's own addresses; once the
    // directory lists it again it is there once.
    void droppedReceiverIsNotCopiedToOwn()
    {
        FakeKiwi kiwi;
        const QString a = QStringLiteral("http://a.example:8073");
        const QString b = kiwi.address();
        KiwiPlayer p;
        p.setDirectory({receiver(a, QStringLiteral("Aland")), receiver(b, QStringLiteral("Bergen"))});
        p.setReceivers({}, {}, b);
        p.setDirectory({receiver(a, QStringLiteral("Aland"))});
        QToolButton* play = playButton(p);
        QComboBox* list = receiverList(p);
        QVERIFY(play && list);
        play->click();
        QTRY_COMPARE_WITH_TIMEOUT(kiwi.peers.size(), 1, 5000);
        QCOMPARE(p.currentReceiver(), b);
        QVERIFY(p.receivers().isEmpty());
        p.stop();
        p.setDirectory({receiver(a, QStringLiteral("Aland")), receiver(b, QStringLiteral("Bergen"))});
        QCOMPARE(entriesFor(list, b), 1);
        QCOMPARE(p.currentReceiver(), b);
    }

    // The rig's mode as Hamlib names it, mapped onto what a KiwiSDR has: the
    // data and ECSS modes are their sideband, not AM.
    void rigModesMapToReceiverModes_data()
    {
        QTest::addColumn<QString>("rigMode");
        QTest::addColumn<QString>("shown");
        QTest::newRow("USB") << QStringLiteral("USB") << QStringLiteral("USB");
        QTest::newRow("PKTUSB") << QStringLiteral("PKTUSB") << QStringLiteral("USB");
        QTest::newRow("ECSSUSB") << QStringLiteral("ECSSUSB") << QStringLiteral("USB");
        QTest::newRow("LSB") << QStringLiteral("LSB") << QStringLiteral("LSB");
        QTest::newRow("PKTLSB") << QStringLiteral("PKTLSB") << QStringLiteral("LSB");
        QTest::newRow("CWR") << QStringLiteral("CWR") << QStringLiteral("CW");
        QTest::newRow("PKTFM") << QStringLiteral("PKTFM") << QStringLiteral("NFM");
        QTest::newRow("SAM") << QStringLiteral("SAM") << QStringLiteral("AM");
        QTest::newRow("AM") << QStringLiteral("AM") << QStringLiteral("AM");
    }
    void rigModesMapToReceiverModes()
    {
        QFETCH(QString, rigMode);
        QFETCH(QString, shown);
        KiwiPlayer p;
        QComboBox* box = modeBox(p);
        QVERIFY(box);
        p.tune(7074, rigMode);
        QCOMPARE(box->currentText(), shown);
        QVERIFY(!box->isEnabled());
    }
};

QTEST_MAIN(TestKiwiPlayer)
#include "test_kiwiplayer.moc"
