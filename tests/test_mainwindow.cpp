// SPDX-License-Identifier: GPL-3.0-or-later
// The real main window against a fake rigctld on this machine: the "Rig not
// answering" line, tuning from the table, the settings kept on close and
// what OK in Settings leaves alone.
//
// The window never goes online here: the data directory is seeded with
// every download, the version check and the online receiver switched off,
// and all traffic is pointed at a proxy that only counts connections.
// The rig's silence timeout is the real one, so some tests take seconds.
#include "KiwiPlayer.h"
#include "MainWindow.h"
#include "StationModel.h"
#include "core/RigClient.h"
#include "core/StationDb.h"
#include "fakerigctld.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkProxy>
#include <QTableView>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace
{
// RigClient's silence timeout at the seeded poll interval: max(5 s, 3 polls)
const int kSilenceMs = 5000;
// the listener's own station on the dial, near the fake rig's 7125 kHz
const double kStationKHz = 7130.0;

// Settings that keep the window off the internet: no downloads, no version
// check, no online receiver, no rigctld of its own. The rig is the fake on
// this machine. Written before the window opens the database.
bool seed(const QString& dir, quint16 rigPort, bool followRig = true)
{
    StationDb db(dir + QStringLiteral("/stations.db"));
    if (!db.open())
        return false;
    AppSettings s;
    s.eibiEnabled = false;
    s.hfccEnabled = false;
    s.aokiEnabled = false;
    s.traficomEnabled = false;
    s.updateCheck = false;
    s.showPlayer = false;
    s.launchRigctld = false;
    s.followRig = followRig;
    s.rigHost = QStringLiteral("127.0.0.1");
    s.rigPort = rigPort;
    s.pollIntervalMs = 100;
    s.save(&db);
    StationEntry e;
    e.source = userSourceId();
    e.kHz = kStationKHz;
    e.station = QStringLiteral("Test Radio");
    e.mode = QStringLiteral("AM");
    return db.insertEntry(e);
}

// the settings as the next start would find them
AppSettings stored(const QString& dir)
{
    StationDb db(dir + QStringLiteral("/stations.db"));
    AppSettings s;
    if (db.open())
        s.load(&db);
    return s;
}

QString labelText(const MainWindow& w, const QString& name)
{
    const QLabel* label = w.findChild<QLabel*>(name);
    return label ? label->text() : QStringLiteral("(no label %1)").arg(name);
}
QString rigStatus(const MainWindow& w) { return labelText(w, QStringLiteral("rigStatus")); }
QString shownFrequency(const MainWindow& w) { return labelText(w, QStringLiteral("frequencyLabel")); }
QString shownMode(const MainWindow& w) { return labelText(w, QStringLiteral("modeLabel")); }
QString notAnswering(quint16 port) { return QStringLiteral("Rig not answering (127.0.0.1:%1)").arg(port); }

QCheckBox* followRig(const MainWindow& w) { return w.findChild<QCheckBox*>(QStringLiteral("followRig")); }

// Double-clicks the row of the station on the given frequency, as the
// listener would. False when there is no such row on screen.
bool doubleClickStation(MainWindow& w, double kHz)
{
    auto* table = w.findChild<QTableView*>(QStringLiteral("stationTable"));
    if (!table)
        return false;
    const QAbstractItemModel* model = table->model();
    for (int r = 0; r < model->rowCount(); ++r)
    {
        const QModelIndex freq = model->index(r, StationModel::ColFrequency);
        if (model->data(freq, StationModel::SortRole).toDouble() != kHz)
            continue;
        const QModelIndex cell = model->index(r, StationModel::ColStation);
        table->scrollTo(cell);
        const QRect rect = table->visualRect(cell);
        if (rect.isEmpty())
            return false;
        // a widget gets only the double-click event from QTest; the view
        // wants the press before it, as a real mouse sends
        QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
        QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
        return true;
    }
    return false;
}

// The online receiver's mode box, greyed while the receiver takes the
// rig's mode.
QComboBox* receiverModeBox(const MainWindow& w)
{
    const KiwiPlayer* player = w.findChild<KiwiPlayer*>();
    if (!player)
        return nullptr;
    for (QComboBox* box : player->findChildren<QComboBox*>())
        if (box->findText(QStringLiteral("NFM")) >= 0)
            return box;
    return nullptr;
}
}

class TestMainWindow : public QObject
{
    Q_OBJECT
private:
    QTcpServer m_trap;   // a "proxy" that only counts the requests it gets
    int m_trapHits = 0;
    int m_trapHitsBefore = 0;   // the count when the current test began

private slots:
    void initTestCase()
    {
        // the same number format on every machine; frequencyEntryIgnoresLocale
        // switches to Finnish on purpose
        QLocale::setDefault(QLocale::c());
        // A request to anywhere but this machine (the fake rig) would go
        // through this proxy, and is counted as a failure.
        QVERIFY(m_trap.listen(QHostAddress::LocalHost));
        connect(&m_trap, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* s = m_trap.nextPendingConnection())
            {
                ++m_trapHits;
                s->abort();
                s->deleteLater();
            }
        });
        QNetworkProxy::setApplicationProxy(
            QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), m_trap.serverPort()));
    }

    void init()
    {
        m_trapHitsBefore = m_trapHits;
    }

    // Each test answers for the requests made while it ran. A connection
    // is counted when the event loop takes it, so let it do that first.
    void cleanup()
    {
        QTest::qWait(50);
        QCOMPARE(m_trapHits - m_trapHitsBefore, 0);
    }

    void cleanupTestCase()
    {
        QTest::qWait(200);   // anything the last window started on its way out
        QCOMPARE(m_trapHits, 0);
    }

    // The icons are compiled in; the app code is built as an object library
    // so that they are not dropped on the way.
    void iconsAreBuiltIn()
    {
        QVERIFY(QFile::exists(QStringLiteral(":/otd-64.png")));
        QVERIFY(QFile::exists(QStringLiteral(":/otd.svg")));
    }

    // A rig that keeps answering the same frequency is not "not answering"
    // (the watchdog once listened for changes only).
    void steadyRigNoWarning()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port()));
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTRY_COMPARE_WITH_TIMEOUT(shownFrequency(w), QStringLiteral("7 125.000 kHz"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(shownMode(w), QStringLiteral("USB"), 5000);

        // three silence timeouts of the same answer, looked at all along
        const int pollsBefore = rig.received.count(QStringLiteral("f"));
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 3 * kSilenceMs)
        {
            QVERIFY2(rigStatus(w).isEmpty(), qPrintable(rigStatus(w)));
            QTest::qWait(100);
        }
        QVERIFY2(rigStatus(w).isEmpty(), qPrintable(rigStatus(w)));
        QCOMPARE(shownFrequency(w), QStringLiteral("7 125.000 kHz"));
        QCOMPARE(shownMode(w), QStringLiteral("USB"));
        QVERIFY(rig.received.count(QStringLiteral("f")) - pollsBefore >= 10);   // asked all the time
    }

    // rigctld up but the radio off: the warning names the address; the
    // radio back on (on another frequency): the warning goes and the
    // display follows the rig again.
    void silentRigShowsWarningAndClears()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port()));
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTRY_COMPARE_WITH_TIMEOUT(shownFrequency(w), QStringLiteral("7 125.000 kHz"), 5000);
        QVERIFY(rigStatus(w).isEmpty());

        rig.radioOn = false;
        QTRY_COMPARE_WITH_TIMEOUT(rigStatus(w), notAnswering(rig.port()), 3 * kSilenceMs);
        QCOMPARE(shownMode(w), QStringLiteral("no rig"));

        rig.hz = 7140000;
        rig.radioOn = true;
        QTRY_VERIFY_WITH_TIMEOUT(rigStatus(w).isEmpty(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(shownFrequency(w), QStringLiteral("7 140.000 kHz"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(shownMode(w), QStringLiteral("USB"), 5000);
    }

    // A double-click on a row tunes the rig: the mode first, because many
    // rigs shift the dial when the mode changes.
    void tuneSendsModeBeforeFrequency()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port()));
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTRY_COMPARE_WITH_TIMEOUT(shownMode(w), QStringLiteral("USB"), 5000);

        QVERIFY(doubleClickStation(w, kStationKHz));
        QTRY_COMPARE_WITH_TIMEOUT(rig.hz, qint64(7130000), 5000);
        const int m = rig.received.indexOf(QStringLiteral("M AM 0"));
        const int f = rig.received.indexOf(QStringLiteral("F 7130000"));
        QVERIFY2(m >= 0, qPrintable(rig.received.join(QLatin1Char(' '))));
        QVERIFY2(f > m, qPrintable(rig.received.join(QLatin1Char(' '))));
        // the display follows what the rig then says
        QTRY_COMPARE_WITH_TIMEOUT(shownFrequency(w), QStringLiteral("7 130.000 kHz"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(shownMode(w), QStringLiteral("AM"), 5000);
        QVERIFY(followRig(w)->isChecked());
    }

    // With the rig silent a double-click tunes by hand: nothing is sent to
    // the rig, Follow rig goes off. Ticking it again while the rig is still
    // silent leaves the dial where it is: nothing the rig said still holds.
    void tuneWithSilentRigTunesManually()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port()));
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTRY_COMPARE_WITH_TIMEOUT(shownFrequency(w), QStringLiteral("7 125.000 kHz"), 5000);
        rig.radioOn = false;
        QTRY_COMPARE_WITH_TIMEOUT(rigStatus(w), notAnswering(rig.port()), 3 * kSilenceMs);

        const int before = rig.received.size();
        QVERIFY(doubleClickStation(w, kStationKHz));
        QCOMPARE(shownFrequency(w), QStringLiteral("7 130.000 kHz"));
        QCOMPARE(shownMode(w), QStringLiteral("manual"));
        QVERIFY(!followRig(w)->isChecked());
        QVERIFY(rigStatus(w).isEmpty());   // manual: no complaint about the rig
        QTest::qWait(1000);                // a command on its way would have arrived by now
        for (int i = before; i < rig.received.size(); ++i)
        {
            const QString& cmd = rig.received.at(i);
            QVERIFY2(!cmd.startsWith(QLatin1String("F ")) && !cmd.startsWith(QLatin1String("M ")),
                     qPrintable(cmd));
        }

        followRig(w)->setChecked(true);
        QCOMPARE(shownFrequency(w), QStringLiteral("7 130.000 kHz"));
        QCOMPARE(rigStatus(w), notAnswering(rig.port()));
    }

    // rigctld restarting (the connection drops and comes back within the
    // silence timeout) is not the rig going away: no warning, and the
    // online receiver keeps taking the rig's mode.
    void connectionDropIsNotSilence()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port()));
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTRY_COMPARE_WITH_TIMEOUT(shownMode(w), QStringLiteral("USB"), 5000);
        auto* client = w.findChild<RigClient*>();
        QComboBox* modeBox = receiverModeBox(w);
        QVERIFY(client && modeBox);
        QVERIFY(!modeBox->isEnabled());

        rig.dropClients();
        QTRY_VERIFY_WITH_TIMEOUT(!client->isConnected(), 2000);
        followRig(w)->setChecked(false);   // anything that hands the receiver its mode again
        followRig(w)->setChecked(true);
        // all through the gap, until rigctld answers again: no warning, and
        // the receiver keeps the rig's mode
        const int pollsBefore = rig.received.count(QStringLiteral("f"));
        QElapsedTimer clock;
        clock.start();
        while (!(client->isConnected() && rig.received.count(QStringLiteral("f")) > pollsBefore + 2))
        {
            QVERIFY2(clock.elapsed() < 3 * kSilenceMs, "rigctld was not reached again");
            QVERIFY2(rigStatus(w).isEmpty(), qPrintable(rigStatus(w)));
            QVERIFY(!modeBox->isEnabled());
            QCOMPARE(shownMode(w), QStringLiteral("USB"));
            QTest::qWait(50);
        }
        QVERIFY(rigStatus(w).isEmpty());
        QVERIFY(!modeBox->isEnabled());
    }

    // The manual frequency is saved once tuning pauses; closing the window
    // before that must not lose it.
    void pendingManualFrequencySavedOnClose()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port(), false));
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QCOMPARE(shownFrequency(w), QStringLiteral("6 070.000 kHz"));   // the first start's default

        auto* edit = w.findChild<QLineEdit*>(QStringLiteral("frequencyEdit"));
        QVERIFY(edit && !edit->isReadOnly());
        edit->setText(QStringLiteral("9500.125"));
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(shownFrequency(w), QStringLiteral("9 500.125 kHz"));
        // not saved yet: that waits for tuning to pause, and no event has
        // been handled since
        auto* pending = w.findChild<QTimer*>(QStringLiteral("saveTimer"));
        QVERIFY(pending && pending->isActive());
        QCOMPARE(stored(dir.path()).manualKHz, 6070.0);

        w.close();
        QCOMPARE(stored(dir.path()).manualKHz, 9500.125);
        QVERIFY(!pending->isActive());   // nothing left to write after closing
    }

    // OK in Settings keeps the KiwiSDR volume the listener set (the dialog
    // does not show it), and it is saved on exit.
    void settingsOkKeepsVolume()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port()));
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* player = w.findChild<KiwiPlayer*>();
        QVERIFY(player);
        QCOMPARE(player->volume(), 70);

        player->setVolume(25);
        w.acceptSettings(w.settings());   // OK with nothing changed in the dialog
        QCOMPARE(player->volume(), 25);
        w.close();
        QCOMPARE(stored(dir.path()).kiwiVolume, 25);
    }

    // Started with -f the window is in manual mode for the session while the
    // stored preference stays "follow". OK in Settings changes neither.
    void settingsOkKeepsFollowState()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port()));
        MainWindow w(dir.path(), 9500.0);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* client = w.findChild<RigClient*>();
        auto* edit = w.findChild<QLineEdit*>(QStringLiteral("frequencyEdit"));
        QVERIFY(client && edit && followRig(w));
        QTRY_VERIFY_WITH_TIMEOUT(client->isAnswering(), 5000);   // a rig is there, but not followed
        QVERIFY(!followRig(w)->isChecked());
        QVERIFY(!edit->isReadOnly());

        w.acceptSettings(w.settings());
        QVERIFY(!followRig(w)->isChecked());
        QVERIFY(!edit->isReadOnly());
        QCOMPARE(shownMode(w), QStringLiteral("manual"));
        QCOMPARE(shownFrequency(w), QStringLiteral("9 500.000 kHz"));
        QVERIFY(stored(dir.path()).followRig);
    }

    // A machine set to Finnish (or German) numbers writes a decimal comma;
    // the frequency field takes "9500.125" and "7300,5" alike.
    void frequencyEntryIgnoresLocale()
    {
        struct RestoreLocale
        {
            ~RestoreLocale() { QLocale::setDefault(QLocale::c()); }
        } restore;
        QLocale::setDefault(QLocale(QLocale::Finnish, QLocale::Finland));
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port(), false));
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* edit = w.findChild<QLineEdit*>(QStringLiteral("frequencyEdit"));
        QVERIFY(edit && !edit->isReadOnly());
        edit->clear();
        QTest::keyClicks(edit, QStringLiteral("9500.125"));
        QCOMPARE(edit->text(), QStringLiteral("9500.125"));   // the validator let every key in
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(shownFrequency(w), QStringLiteral("9 500.125 kHz"));
        // the decimal comma the machine uses is fine too
        edit->clear();
        QTest::keyClicks(edit, QStringLiteral("7300,5"));
        QCOMPARE(edit->text(), QStringLiteral("7300,5"));
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(shownFrequency(w), QStringLiteral("7 300.500 kHz"));
    }

    // A right-click on the table header opens the column menu once (it was
    // once connected twice and came back after the first was closed).
    void columnMenuOpensOnce()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port()));
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* table = w.findChild<QTableView*>(QStringLiteral("stationTable"));
        QVERIFY(table);
        // close every menu that shows up, and count them
        int menus = 0;
        QTimer closer;
        closer.setInterval(50);
        connect(&closer, &QTimer::timeout, this, [&menus]() {
            if (QWidget* popup = QApplication::activePopupWidget())
            {
                ++menus;
                popup->close();
            }
        });
        closer.start();
        emit table->horizontalHeader()->customContextMenuRequested(QPoint(10, 5));
        QTest::qWait(300);   // a second menu would open right after the first
        QCOMPARE(menus, 1);
    }

    // A new rig address from Settings: once the new one has been silent
    // for the timeout, the warning names it.
    void warningFollowsNewEndpoint()
    {
        // two ports nobody listens on
        QTcpServer a, b;
        QVERIFY(a.listen(QHostAddress::LocalHost) && b.listen(QHostAddress::LocalHost));
        const quint16 first = a.serverPort(), second = b.serverPort();
        a.close();
        b.close();

        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), first));
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTRY_COMPARE_WITH_TIMEOUT(rigStatus(w), notAnswering(first), 3 * kSilenceMs);

        AppSettings s = w.settings();
        s.rigPort = second;
        w.acceptSettings(s);
        QCOMPARE(rigStatus(w), notAnswering(second));   // the address in use, at once
        // and once the new address has been silent for the timeout too, the
        // warning is still there and still names it
        QTest::qWait(kSilenceMs + 1000);
        QCOMPARE(rigStatus(w), notAnswering(second));
    }
};

QTEST_MAIN(TestMainWindow)
#include "test_mainwindow.moc"
