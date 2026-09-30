// SPDX-License-Identifier: GPL-3.0-or-later
// The real main window against a fake rigctld on this machine: the "Rig not
// answering" line, tuning from the table, the settings kept on close and
// what OK in Settings leaves alone.
//
// The window never goes online here: the data directory is seeded with
// every download, the version check and the online receiver switched off,
// and all traffic is pointed at a proxy that only counts connections.
// The rig's silence timeout is the real one, so some tests take seconds.
#include "BufferBar.h"
#include "DialScale.h"
#include "KiwiPlayer.h"
#include "MainWindow.h"
#include "core/StationModel.h"
#include "core/EibiParser.h"
#include "core/RigClient.h"
#include "core/ScheduleSource.h"
#include "core/StationDb.h"
#include "core/UpdateCheck.h"
#include "core/Updater.h"
#include "fakerigctld.h"

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QRegularExpression>
#include <QWheelEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkProxy>
#include <QScrollBar>
#include <QStatusBar>
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
    e.target = QStringLiteral("North Europe");
    e.lang = QStringLiteral("Finnish");
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

    // Follow rig unticked while the rig answers (listening online, say): a
    // double-click tunes the display and the online receiver, with the
    // station's mode, and leaves the rig and the tick alone.
    void tuneWithFollowOffLeavesTheRigAlone()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port(), false));
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* client = w.findChild<RigClient*>();
        auto* player = w.findChild<KiwiPlayer*>();
        QVERIFY(client && player);
        QTRY_VERIFY_WITH_TIMEOUT(client->isAnswering(), 5000);
        QVERIFY(!followRig(w)->isChecked());
        QCOMPARE(shownFrequency(w), QStringLiteral("6 070.000 kHz"));
        player->setManualMode(QStringLiteral("USB"));

        const int before = rig.received.size();
        QVERIFY(doubleClickStation(w, kStationKHz));
        QCOMPARE(shownFrequency(w), QStringLiteral("7 130.000 kHz"));
        QVERIFY(!followRig(w)->isChecked());
        QCOMPARE(shownMode(w), QStringLiteral("manual"));
        QCOMPARE(player->manualMode(), QStringLiteral("AM"));   // the station's mode
        QTest::qWait(500);   // a command on its way would have arrived by now
        for (int i = before; i < rig.received.size(); ++i)
        {
            const QString& cmd = rig.received.at(i);
            QVERIFY2(!cmd.startsWith(QLatin1String("F ")) && !cmd.startsWith(QLatin1String("M ")), qPrintable(cmd));
        }
        QCOMPARE(rig.hz, qint64(7125000));   // the rig did not move
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

    // The dial view loads a thousand entries either side of the tuned
    // frequency; scrolled near an end, it loads more around what is on
    // screen, without moving the rows on screen or the tuning, as far as
    // the database goes.
    void dialRefillsWhenScrolledToItsEnd()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port(), false));
        {
            StationDb db(dir.path() + QStringLiteral("/stations.db"));
            QVERIFY(db.open());
            StationList many;   // one station per kHz from 5000 to 7999
            for (int k = 5000; k < 8000; ++k)
            {
                StationEntry e;
                e.source = userSourceId();
                e.kHz = k;
                e.station = QStringLiteral("S%1").arg(k);
                e.startMin = 0;
                e.endMin = 1440;
                many << e;
            }
            QVERIFY(db.replaceSource(userSourceId(), many));
        }
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* table = w.findChild<QTableView*>(QStringLiteral("stationTable"));
        QVERIFY(table);
        QCOMPARE(shownFrequency(w), QStringLiteral("6 070.000 kHz"));
        auto loaded = [table]() {
            double lo = 1e12, hi = -1.0;
            for (int r = 0; r < table->model()->rowCount(); ++r)
            {
                const QString name = table->model()->index(r, StationModel::ColStation).data().toString();
                if (name.isEmpty())
                    continue;   // a blank row at an end
                const double kHz = table->model()->index(r, StationModel::ColFrequency).data(StationModel::SortRole).toDouble();
                lo = qMin(lo, kHz);
                hi = qMax(hi, kHz);
            }
            return qMakePair(lo, hi);
        };
        QCOMPARE(loaded(), qMakePair(5070.0, 7069.0));   // a thousand either side
        QScrollBar* bar = table->verticalScrollBar();

        // to the bottom end: more below comes in, the tuning stays
        bar->setValue(bar->maximum());
        QTRY_VERIFY_WITH_TIMEOUT(loaded().second > 7500.0, 3000);
        QCOMPARE(shownFrequency(w), QStringLiteral("6 070.000 kHz"));
        QVERIFY(!followRig(w)->isChecked());
        // the screen stays on real stations near the end it reached, not
        // in the blank rows, and the database's end is the end
        auto midKHz = [table]() {
            const int mid = table->rowAt(table->viewport()->height() / 2);
            return mid < 0 ? -1.0 : table->model()->index(mid, StationModel::ColFrequency).data(StationModel::SortRole).toDouble();
        };
        QVERIFY2(midKHz() > 7000.0, qPrintable(QString::number(midKHz())));
        bar->setValue(bar->maximum());
        QTRY_COMPARE_WITH_TIMEOUT(loaded().second, 7999.0, 3000);
        bar->setValue(bar->maximum());
        QTest::qWait(200);
        QCOMPARE(loaded().second, 7999.0);
        // at the very end the last station is on screen, above the blank rows
        bool lastVisible = false;
        for (int r = 0; r < table->model()->rowCount(); ++r)
            if (table->model()->index(r, StationModel::ColStation).data().toString() == QLatin1String("S7999"))
            {
                const int y = table->rowViewportPosition(r);
                lastVisible = y >= 0 && y < table->viewport()->height();
            }
        QVERIFY(lastVisible);

        // and back up past the top end
        bar->setValue(bar->minimum());
        QTRY_VERIFY_WITH_TIMEOUT(loaded().first < 6000.0, 3000);
        bar->setValue(bar->minimum());
        QTRY_COMPARE_WITH_TIMEOUT(loaded().first, 5000.0, 3000);
        QCOMPARE(shownFrequency(w), QStringLiteral("6 070.000 kHz"));
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

    // Search results can be sorted by a click on a column title: up, down,
    // and back to the usual order. Refining the search keeps the sorting,
    // Escape leaves the search and the dial is in frequency order again,
    // its titles not clickable.
    void searchResultsSortByColumn()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port(), false));
        {
            StationDb db(dir.path() + QStringLiteral("/stations.db"));
            QVERIFY(db.open());
            const QList<std::tuple<double, const char*, int, int>> more = {
                {7200, "Radio Alpha", 18 * 60, 19 * 60},
                {6100, "Radio Bravo", 6 * 60, 7 * 60},
                {9400, "Radio Charlie", 12 * 60, 13 * 60}};
            for (const auto& [kHz, name, start, end] : more)
            {
                StationEntry e;
                e.source = userSourceId();
                e.kHz = kHz;
                e.station = QString::fromLatin1(name);
                e.startMin = start;
                e.endMin = end;
                QVERIFY(db.insertEntry(e));
            }
        }
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* search = w.findChild<QLineEdit*>(QStringLiteral("search"));
        auto* table = w.findChild<QTableView*>(QStringLiteral("stationTable"));
        QVERIFY(search && table);
        table->setColumnHidden(StationModel::ColTime, false);
        QHeaderView* header = table->horizontalHeader();
        auto order = [table]() {
            QStringList names;
            for (int r = 0; r < table->model()->rowCount(); ++r)
            {
                const QString name = table->model()->index(r, StationModel::ColStation).data().toString();
                if (!name.isEmpty())   // the dial's blank rows
                    names << name;
            }
            return names;
        };
        auto clickTitle = [header](int column) {
            const int x = header->sectionViewportPosition(column) + header->sectionSize(column) / 2;
            QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(x, header->height() / 2));
        };

        // the dial: frequency order, and a click on a title changes nothing
        QVERIFY(!header->sectionsClickable());
        const QStringList dial = order();
        QCOMPARE(dial, (QStringList{"Radio Bravo", "Test Radio", "Radio Alpha", "Radio Charlie"}));
        clickTitle(StationModel::ColTime);
        QCOMPARE(order(), dial);

        search->setFocus();
        QTest::keyClicks(search, QStringLiteral("radio"));
        QVERIFY(header->sectionsClickable());
        const QStringList found = order();
        QCOMPARE(found.size(), 4);
        QVERIFY(!header->isSortIndicatorShown());

        clickTitle(StationModel::ColTime);   // by start time
        QCOMPARE(order(), (QStringList{"Test Radio", "Radio Bravo", "Radio Charlie", "Radio Alpha"}));
        QVERIFY(header->isSortIndicatorShown());
        QCOMPARE(header->sortIndicatorSection(), int(StationModel::ColTime));
        clickTitle(StationModel::ColTime);   // the other way round
        QCOMPARE(order(), (QStringList{"Radio Alpha", "Radio Charlie", "Radio Bravo", "Test Radio"}));
        clickTitle(StationModel::ColTime);   // and the usual order again
        QCOMPARE(order(), found);
        QVERIFY(!header->isSortIndicatorShown());

        clickTitle(StationModel::ColStation);   // by name, then refine the search: still by name
        QCOMPARE(order(), (QStringList{"Radio Alpha", "Radio Bravo", "Radio Charlie", "Test Radio"}));
        QTest::keyClicks(search, QStringLiteral(" !bravo"));
        QCOMPARE(order(), (QStringList{"Radio Alpha", "Radio Charlie", "Test Radio"}));
        QVERIFY(header->isSortIndicatorShown());

        QTest::keyClick(search, Qt::Key_Escape);   // out of the search
        QVERIFY(search->text().isEmpty());
        QVERIFY(!header->sectionsClickable());
        QVERIFY(!header->isSortIndicatorShown());
        QCOMPARE(order(), dial);
        // a new search starts in the usual order
        QTest::keyClicks(search, QStringLiteral("radio"));
        QCOMPARE(order(), found);
    }

    // The audio-in-hand bar sits in the status bar's left corner, hidden
    // until the online receiver plays, and follows what the player tells.
    void bufferBarInTheCorner()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port()));
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* bar = w.findChild<BufferBar*>(QStringLiteral("bufferBar"));
        QVERIFY(bar);
        QVERIFY(!bar->isVisible());
        QWidget* statusBar = w.statusBar();
        QVERIFY(bar->parentWidget() == statusBar || bar->parentWidget()->parentWidget() == statusBar);
        // the first thing in the status bar, left of the rig text
        auto* rigText = w.findChild<QLabel*>(QStringLiteral("rigStatus"));
        QVERIFY(rigText);
        QVERIFY(bar->geometry().left() <= rigText->geometry().left());
        auto* player = w.findChild<KiwiPlayer*>();
        QVERIFY(player);
        // not playing: whatever is told, nothing shows
        emit player->audioInHandChanged(0.4);
        QVERIFY(!bar->isVisible());
        QCOMPARE(bar->seconds(), 0.0);
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

    // Follow rig ticked but nothing ever answers: once the rig counts as
    // silent, the dial shows the remembered manual frequency rather than
    // staying empty, the field says the same and stays read-only, and the
    // clock runs in UTC.
    void silentRigStartsOnRememberedFrequency()
    {
        QTcpServer a;
        QVERIFY(a.listen(QHostAddress::LocalHost));
        const quint16 port = a.serverPort();
        a.close();
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), port));
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QCOMPARE(shownFrequency(w), QStringLiteral("---.--- kHz"));
        QTRY_COMPARE_WITH_TIMEOUT(rigStatus(w), notAnswering(port), 3 * kSilenceMs);
        QCOMPARE(shownFrequency(w), QStringLiteral("6 070.000 kHz"));
        QCOMPARE(shownMode(w), QStringLiteral("no rig"));
        auto* edit = w.findChild<QLineEdit*>(QStringLiteral("frequencyEdit"));
        QVERIFY(edit && edit->isReadOnly());
        QCOMPARE(edit->text(), QStringLiteral("6070.000"));
        QVERIFY(followRig(w)->isChecked());
        const QRegularExpression utc(QStringLiteral("^\\d\\d:\\d\\d:\\d\\d UTC$"));
        QVERIFY2(utc.match(labelText(w, QStringLiteral("clockLabel"))).hasMatch(),
                 qPrintable(labelText(w, QStringLiteral("clockLabel"))));
    }

    // Without a rig the arrow keys are the tuning knob: Up/Down 1 kHz,
    // Page Up/Down 5 kHz, a tenth of that with Ctrl. Following the rig,
    // they are not.
    void arrowKeysTuneWithoutRig()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port(), false));
        MainWindow w(dir.path());
        w.show();
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(QTest::qWaitForWindowActive(&w));
        QCOMPARE(shownFrequency(w), QStringLiteral("6 070.000 kHz"));
        QTest::keyClick(&w, Qt::Key_Up);
        QCOMPARE(shownFrequency(w), QStringLiteral("6 071.000 kHz"));
        QTest::keyClick(&w, Qt::Key_PageUp);
        QCOMPARE(shownFrequency(w), QStringLiteral("6 076.000 kHz"));
        QTest::keyClick(&w, Qt::Key_Down, Qt::ControlModifier);
        QCOMPARE(shownFrequency(w), QStringLiteral("6 075.900 kHz"));
        QTest::keyClick(&w, Qt::Key_PageDown);
        QCOMPARE(shownFrequency(w), QStringLiteral("6 070.900 kHz"));
        auto* edit = w.findChild<QLineEdit*>(QStringLiteral("frequencyEdit"));
        QVERIFY(edit);
        QCOMPARE(edit->text(), QStringLiteral("6070.900"));   // the field says the same
        QCOMPARE(shownMode(w), QStringLiteral("manual"));

        followRig(w)->setChecked(true);   // the rig has been answering all along
        QTRY_COMPARE_WITH_TIMEOUT(shownFrequency(w), QStringLiteral("7 125.000 kHz"), 5000);
        QTest::keyClick(&w, Qt::Key_Up);
        QTest::keyClick(&w, Qt::Key_PageUp);
        QCOMPARE(shownFrequency(w), QStringLiteral("7 125.000 kHz"));
    }

    // The mouse wheel over a digit of the big frequency turns that digit,
    // like the tuning step of a radio display; over a space it does nothing.
    void wheelOverDigitStepsThatDigit()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port(), false));
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* label = w.findChild<QLabel*>(QStringLiteral("frequencyLabel"));
        QVERIFY(label);
        QCOMPARE(label->text(), QStringLiteral("6 070.000 kHz"));
        auto wheelAt = [label](int index, int steps) {
            const QFontMetrics fm(label->font());
            const QString text = label->text();
            double x = label->contentsRect().left();
            for (int i = 0; i < index; ++i)
                x += fm.horizontalAdvance(text.at(i));
            x += fm.horizontalAdvance(text.at(index)) / 2.0;
            const QPointF pos(x, label->height() / 2.0);
            QWheelEvent ev(pos, label->mapToGlobal(pos.toPoint()), QPoint(), QPoint(0, 120 * steps),
                           Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(label, &ev);
        };
        wheelAt(3, 1);    // the 7 of "6 070": tens
        QCOMPARE(shownFrequency(w), QStringLiteral("6 080.000 kHz"));
        wheelAt(0, -1);   // the 6: thousands
        QCOMPARE(shownFrequency(w), QStringLiteral("5 080.000 kHz"));
        wheelAt(8, 2);    // the last decimal
        QCOMPARE(shownFrequency(w), QStringLiteral("5 080.002 kHz"));
        wheelAt(1, 1);    // the thousands space
        QCOMPARE(shownFrequency(w), QStringLiteral("5 080.002 kHz"));
        auto* edit = w.findChild<QLineEdit*>(QStringLiteral("frequencyEdit"));
        QVERIFY(edit);
        QCOMPARE(edit->text(), QStringLiteral("5080.002"));

        // following an answering rig, the wheel turns the rig's dial and
        // the display follows what the rig then says
        followRig(w)->setChecked(true);
        QTRY_COMPARE_WITH_TIMEOUT(shownFrequency(w), QStringLiteral("7 125.000 kHz"), 5000);
        wheelAt(3, 1);    // the 2 of "7 125": tens
        QTRY_COMPARE_WITH_TIMEOUT(rig.hz, qint64(7135000), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(shownFrequency(w), QStringLiteral("7 135.000 kHz"), 5000);
        QVERIFY(followRig(w)->isChecked());
        QCOMPARE(w.statusBar()->currentMessage(), QStringLiteral("Tuning rig to 7135.000 kHz"));
    }

    // The band line names the allocation of the tuned frequency from the
    // built-in plan, coloured by its kind, and says so when there is none.
    // With the Traficom table on, its rows replace the plan where they exist.
    void bandLineFollowsPlanAndTraficom()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port(), false));
        {
            MainWindow w(dir.path());
            w.show();
            QVERIFY(QTest::qWaitForWindowExposed(&w));
            auto* band = w.findChild<QLabel*>(QStringLiteral("bandLabel"));
            auto* edit = w.findChild<QLineEdit*>(QStringLiteral("frequencyEdit"));
            QVERIFY(band && edit);
            QCOMPARE(band->text(), QStringLiteral("49 m broadcast"));
            QVERIFY(band->styleSheet().contains(QLatin1String("#e8b339")));
            edit->setText(QStringLiteral("4625"));
            QTest::keyClick(edit, Qt::Key_Return);
            QCOMPARE(band->text(), QStringLiteral("no allocation listed"));
            QVERIFY(band->styleSheet().contains(QLatin1String("palette(mid)")));
            edit->setText(QStringLiteral("7125"));
            QTest::keyClick(edit, Qt::Key_Return);
            QCOMPARE(band->text(), QStringLiteral("40 m amateur"));
            QVERIFY(band->styleSheet().contains(QLatin1String("#7ee787")));
            w.close();   // writes the frequency for the next start
        }
        {
            StationDb db(dir.path() + QStringLiteral("/stations.db"));
            QVERIFY(db.open());
            AppSettings s;
            s.load(&db);
            s.traficomEnabled = true;
            s.save(&db);
            // fresh, so no download is due
            db.setMeta(QStringLiteral("traficom.updated"),
                       QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
            StationDb::Allocation wide, narrow;
            wide.lowKHz = 5000;
            wide.highKHz = 7000;
            wide.service = QStringLiteral("FIXED");
            narrow.lowKHz = 5900;
            narrow.highKHz = 6200;
            narrow.service = QStringLiteral("BROADCASTING");
            narrow.usage = QStringLiteral("HF broadcasting");
            QVERIFY(db.replaceAllocations(QStringLiteral("traficom"), {wide, narrow}));
        }
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* band = w.findChild<QLabel*>(QStringLiteral("bandLabel"));
        QVERIFY(band);
        QCOMPARE(shownFrequency(w), QStringLiteral("7 125.000 kHz"));   // where the last session was
        QCOMPARE(band->text(), QStringLiteral("40 m amateur"));   // no Traficom row there: the plan
        auto* edit = w.findChild<QLineEdit*>(QStringLiteral("frequencyEdit"));
        QVERIFY(edit);
        edit->setText(QStringLiteral("6070"));
        QTest::keyClick(edit, Qt::Key_Return);
        // narrowest first, usage with the service in brackets
        QCOMPARE(band->text(), QStringLiteral("HF broadcasting (broadcasting) \u00b7 FIXED"));
        QVERIFY(band->styleSheet().contains(QLatin1String("#e8b339")));
        QVERIFY2(band->toolTip().startsWith(QLatin1String("Traficom allocation table:")), qPrintable(band->toolTip()));
        QVERIFY(band->toolTip().contains(QLatin1String("5900.000 - 6200.000 kHz: HF broadcasting [BROADCASTING]")));
    }

    // Search results come on air first, then by frequency: an order that
    // does not depend on the VFO, so tuning to a result leaves the list
    // still while the distances follow. A changed text is a new search.
    void searchResultsStayPutWhileTuning()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port(), false));
        {
            StationDb db(dir.path() + QStringLiteral("/stations.db"));
            QVERIFY(db.open());
            for (const auto& [kHz, name] : QList<QPair<double, const char*>>{
                     {7200, "Radio Alpha"}, {6100, "Radio Bravo"}, {9400, "Radio Charlie"}})
            {
                StationEntry e;
                e.source = userSourceId();
                e.kHz = kHz;
                e.station = QString::fromLatin1(name);
                QVERIFY(db.insertEntry(e));
            }
        }
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* search = w.findChild<QLineEdit*>(QStringLiteral("search"));
        auto* table = w.findChild<QTableView*>(QStringLiteral("stationTable"));
        QVERIFY(search && table);
        auto order = [table]() {
            QStringList names;
            for (int r = 0; r < table->model()->rowCount(); ++r)
            {
                const QString name = table->model()->index(r, StationModel::ColStation).data().toString();
                if (!name.isEmpty())
                    names << name;
            }
            return names;
        };
        QTest::keyClicks(search, QStringLiteral("radio"));
        // all on air, so by frequency
        QCOMPARE(order(), (QStringList{"Radio Bravo", "Test Radio", "Radio Alpha", "Radio Charlie"}));
        QVERIFY(doubleClickStation(w, 9400.0));
        QCOMPARE(shownFrequency(w), QStringLiteral("9 400.000 kHz"));
        QCOMPARE(order(), (QStringList{"Radio Bravo", "Test Radio", "Radio Alpha", "Radio Charlie"}));
        // the distances did follow the VFO
        for (int r = 0; r < table->model()->rowCount(); ++r)
            if (table->model()->index(r, StationModel::ColStation).data().toString() == QLatin1String("Radio Charlie"))
                QCOMPARE(table->model()->index(r, StationModel::ColDelta).data(StationModel::DeltaRole).toDouble(), 0.0);
        // a new search text is a new search
        QTest::keyClicks(search, QStringLiteral(" !bravo"));
        QCOMPARE(order(), (QStringList{"Test Radio", "Radio Alpha", "Radio Charlie"}));
    }

    // The count next to the search says what the list holds: "around" the
    // frequency on the dial, "found" while searching, "no entries" when
    // nothing matches.
    void countLabelTellsAroundAndFound()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port(), false));
        {
            StationDb db(dir.path() + QStringLiteral("/stations.db"));
            QVERIFY(db.open());
            StationEntry e;
            e.source = userSourceId();
            e.kHz = 7200;
            e.station = QStringLiteral("Radio Alpha");
            QVERIFY(db.insertEntry(e));
        }
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* search = w.findChild<QLineEdit*>(QStringLiteral("search"));
        QVERIFY(search);
        QCOMPARE(labelText(w, QStringLiteral("countLabel")), QStringLiteral("2 on air / 2 around"));
        QTest::keyClicks(search, QStringLiteral("alpha"));
        QCOMPARE(labelText(w, QStringLiteral("countLabel")), QStringLiteral("1 on air / 1 found"));
        QTest::keyClicks(search, QStringLiteral("zzz"));
        QCOMPARE(labelText(w, QStringLiteral("countLabel")), QStringLiteral("no entries"));
        QTest::keyClick(search, Qt::Key_Escape);
        QCOMPARE(labelText(w, QStringLiteral("countLabel")), QStringLiteral("2 on air / 2 around"));
    }

    // The status bar counts the entries per enabled source, says so when
    // none is enabled, and lists a source the program does not know (a list
    // put into the database by other means) by its id.
    void dbStatusNamesTheSources()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port(), false));
        {
            MainWindow w(dir.path());
            w.show();
            QVERIFY(QTest::qWaitForWindowExposed(&w));
            QCOMPARE(labelText(w, QStringLiteral("dbStatus")), QStringLiteral("No sources enabled"));
        }
        {
            StationDb db(dir.path() + QStringLiteral("/stations.db"));
            QVERIFY(db.open());
            AppSettings s;
            s.load(&db);
            s.eibiEnabled = true;
            s.save(&db);
            // fresh and of the current season, so no download is due
            db.setMeta(QStringLiteral("eibi.updated"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
            db.setMeta(QStringLiteral("eibi.season"), EibiParser::seasonCode(QDate::currentDate()));
            StationList eibi, extra;
            StationEntry e;
            e.source = QStringLiteral("eibi");
            e.kHz = 6005;
            e.station = QStringLiteral("Radio Foo");
            eibi << e;
            QVERIFY(db.replaceSource(QStringLiteral("eibi"), eibi));
            e.source = QStringLiteral("extra");
            extra << e << e;
            QVERIFY(db.replaceSource(QStringLiteral("extra"), extra));
        }
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* updater = w.findChild<Updater*>();
        QVERIFY(updater && updater->source(QStringLiteral("eibi")));
        const QString eibiName = updater->source(QStringLiteral("eibi"))->displayName();
        const QString season = EibiParser::seasonCode(QDate::currentDate()).toUpper();
        QCOMPARE(labelText(w, QStringLiteral("dbStatus")),
                 QStringLiteral("%1 %2: 1  |  EXTRA: 2").arg(eibiName, season));
        const QLabel* status = w.findChild<QLabel*>(QStringLiteral("dbStatus"));
        QVERIFY(status->toolTip().startsWith(QStringLiteral("%1 %2: 1 entries, checked ").arg(eibiName, season)));
    }

    // "On air only" hides the rows that are off the air now (the dial's
    // count still tells how many there are) and is saved at once.
    void onAirOnlyHidesTheRestAndIsSaved()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port(), false));
        {
            StationDb db(dir.path() + QStringLiteral("/stations.db"));
            QVERIFY(db.open());
            // one minute of air time, half a day from now
            const int now = QDateTime::currentDateTimeUtc().time().hour() * 60
                            + QDateTime::currentDateTimeUtc().time().minute();
            StationEntry e;
            e.source = userSourceId();
            e.kHz = 7300;
            e.station = QStringLiteral("Night Owl");
            e.startMin = (now + 720) % 1440;
            e.endMin = e.startMin + 1;
            QVERIFY(db.insertEntry(e));
        }
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* table = w.findChild<QTableView*>(QStringLiteral("stationTable"));
        auto* onAir = w.findChild<QCheckBox*>(QStringLiteral("onAirOnly"));
        QVERIFY(table && onAir && !onAir->isChecked());
        auto names = [table]() {
            QStringList out;
            for (int r = 0; r < table->model()->rowCount(); ++r)
            {
                const QString name = table->model()->index(r, StationModel::ColStation).data().toString();
                if (!name.isEmpty())
                    out << name;
            }
            return out;
        };
        QCOMPARE(names(), (QStringList{"Test Radio", "Night Owl"}));
        QCOMPARE(labelText(w, QStringLiteral("countLabel")), QStringLiteral("1 on air / 2 around"));
        onAir->setChecked(true);
        QCOMPARE(names(), (QStringList{"Test Radio"}));
        QCOMPARE(labelText(w, QStringLiteral("countLabel")), QStringLiteral("1 on air / 2 around"));
        QVERIFY(stored(dir.path()).onAirOnly);
        onAir->setChecked(false);
        QCOMPARE(names(), (QStringList{"Test Radio", "Night Owl"}));
        QVERIFY(!stored(dir.path()).onAirOnly);
    }

    // The highlight range is saved once it stops changing, and the View
    // menu's switches are saved at once.
    void toleranceAndViewSwitchesAreSaved()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port(), false));
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* tolerance = w.findChild<QDoubleSpinBox*>(QStringLiteral("tolerance"));
        QVERIFY(tolerance);
        QCOMPARE(tolerance->value(), 5.0);
        tolerance->setValue(12.5);
        QCOMPARE(stored(dir.path()).toleranceKHz, 5.0);   // not yet
        QTRY_COMPARE_WITH_TIMEOUT(stored(dir.path()).toleranceKHz, 12.5, 3000);

        QAction* dialAction = nullptr;
        for (QAction* a : w.findChildren<QAction*>())
            if (a->text() == QLatin1String("&Dial"))
                dialAction = a;
        auto* scale = w.findChild<DialScale*>();
        QVERIFY(dialAction && scale && dialAction->isChecked() && scale->isVisible());
        dialAction->setChecked(false);
        QVERIFY(!scale->isVisible());
        QVERIFY(!stored(dir.path()).showScale);
        dialAction->setChecked(true);
        QVERIFY(scale->isVisible());
        QVERIFY(stored(dir.path()).showScale);
    }

    // The version check's answer goes to the small link above the clock: a
    // newer version as a link, a note from the project as text, nothing
    // when the check failed. Asked by hand while switched off, it says so.
    void versionCheckSpeaksAboveTheClock()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port()));
        MainWindow w(dir.path());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* label = w.findChild<QLabel*>(QStringLiteral("updateLabel"));
        auto* check = w.findChild<UpdateCheck*>();
        QVERIFY(label && check && !label->isVisible());

        QAction* ask = nullptr;
        for (QAction* a : w.findChildren<QAction*>())
            if (a->text() == QLatin1String("Check for a &new version"))
                ask = a;
        QVERIFY(ask);
        ask->trigger();   // switched off in the seeded settings
        QCOMPARE(w.statusBar()->currentMessage(), QStringLiteral("Version check is switched off in Settings"));
        QVERIFY(!label->isVisible());

        UpdateCheck::Result r;
        r.valid = true;
        r.newer = true;
        r.latest = QStringLiteral("9.9");
        r.url = QStringLiteral("https://otd.oh2gba.eu/");
        emit check->finished(r);
        QVERIFY(label->isVisible());
        QCOMPARE(label->text(), QStringLiteral("<a href=\"https://otd.oh2gba.eu/\">Version 9.9 available</a>"));
        QVERIFY(label->toolTip().startsWith(QLatin1String("You are running")));

        r.newer = false;
        r.message = QStringLiteral("Hello & welcome");
        r.url.clear();
        emit check->finished(r);
        QVERIFY(label->isVisible());
        QCOMPARE(label->text(), QStringLiteral("Hello &amp; welcome"));

        r.valid = false;
        emit check->finished(r);
        QVERIFY(!label->isVisible());
    }
};

QTEST_MAIN(TestMainWindow)
#include "test_mainwindow.moc"
