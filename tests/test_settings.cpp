// SPDX-License-Identifier: GPL-3.0-or-later
// The Settings dialog's result: every field it shows comes back, and the
// fields it does not show stay as they were.
#include "SettingsDialog.h"
#include "settings_helpers.h"

#include <QCheckBox>
#include <QLineEdit>
#include <QtTest>

class TestSettings : public QObject
{
    Q_OBJECT
private slots:
    // The dialog hands back every field it shows, unchanged when nothing
    // was touched, and keeps the fields it does not show (the KiwiSDR list,
    // the manual frequency, ...) as they were. Its result replaces the
    // window's settings whole, so every field counts.
    void dialogRoundTripKeepsEverything()
    {
        AppSettings cur = everythingChanged();
        cur.launchRigctld = false;   // so the host field is the user's
        SettingsDialog dlg(cur);
        expectSame(dlg.settings(cur), cur);
    }

    // Ticking the Traficom switch in the dialog comes back as on (it was
    // once dropped on the way to the saved settings).
    void dialogTraficomSwitch()
    {
        AppSettings cur;
        cur.traficomEnabled = false;
        SettingsDialog dlg(cur);
        QCheckBox* traficom = nullptr;
        for (QCheckBox* box : dlg.findChildren<QCheckBox*>())
            if (box->text() == QLatin1String("Traficom"))
                traficom = box;
        QVERIFY(traficom);
        traficom->setChecked(true);
        QVERIFY(dlg.settings(cur).traficomEnabled);
    }

    // With "start rigctld for me" the host is this machine, whatever the
    // field said before.
    void startingRigctldPinsLocalhost()
    {
        AppSettings cur;
        cur.rigHost = QStringLiteral("shack-pi");
        cur.launchRigctld = false;
        SettingsDialog dlg(cur);
        QCheckBox* launch = nullptr;
        for (QCheckBox* box : dlg.findChildren<QCheckBox*>())
            if (box->text().startsWith(QLatin1String("Start Hamlib")))
                launch = box;
        QVERIFY(launch);
        launch->setChecked(true);
        const AppSettings s = dlg.settings(cur);
        QCOMPARE(s.rigHost, QStringLiteral("localhost"));
        QVERIFY(s.launchRigctld);
        launch->setChecked(false);
        QCOMPARE(dlg.settings(cur).launchRigctld, false);
    }
};

QTEST_MAIN(TestSettings)
#include "test_settings.moc"
