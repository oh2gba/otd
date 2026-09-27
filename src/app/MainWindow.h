// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "DialScale.h"
#include "KiwiPlayer.h"
#include "SettingsDialog.h"
#include "core/BandPlan.h"
#include "core/UpdateCheck.h"
#include "core/KiwiDirectory.h"
#include <QMainWindow>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QSortFilterProxyModel;
class QTableView;
class QTimer;
class Updater;
class RigClient;
class RigctldLauncher;
class StationDb;
class StationModel;
class StationFilter;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const QString& dataDir, double startKHz = 0.0, QWidget* parent = nullptr);

    // Development aid: save a picture of the window after it settled.
    void screenshotTo(const QString& file, int delayMs = 6000);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool tuneByKey(class QKeyEvent* key);

private slots:
    void onRigFrequency(qint64 hz);
    void onRigMode(const QString& mode, int passband);
    void onRigState(bool connected, const QString& message);
    void onFrequencyEdited();
    void onFollowToggled(bool follow);
    void onToleranceChanged(double kHz);
    void onOnAirOnlyToggled(bool on);
    void onFilterChanged(const QString& text);
    void onRowActivated(const QModelIndex& index);
    void onAlwaysOnTopToggled(bool on);
    void updateDatabases();
    void onUpdateFinished(bool ok, const QString& message);
    void openSettings();
    void tick();
    void about();
    void addMyStation();
    void openMyStations();
    void tableContextMenu(const QPoint& pos);

private:
    void buildUi();
    void applySettings();
    void applyLauncher();
    void startUpdateCheck();
    void showUpdateResult(const UpdateCheck::Result& r);
    void setCentreKHz(double kHz, bool fromRig);
    void refreshLookup();
    void updateHeader();
    void updateDbStatus();
    QStringList enabledSources() const;
    void updateCountLabel();
    void centreOnMarker();
    void headerContextMenu(const QPoint& pos);
    void saveColumns();
    void scaleColumns(int width);
    void fitColumns();
    bool m_columnsFitted = false;
    bool m_dialActive = false;
    QString m_lastSearchKey;      // search text and sources of the list on screen    // the list is currently in dial order (no search text)
    void setupTable(QTableView* table);
    void updateToleranceHint();
    QModelIndex sourceIndex(const QModelIndex& proxyIndex) const;
    static QString formatKHz(double kHz);

    AppSettings m_settings;
    QString m_dataDir;
    StationDb* m_db = nullptr;
    QNetworkAccessManager* m_nam = nullptr;
    Updater* m_updater = nullptr;
    UpdateCheck* m_updateCheck = nullptr;
    QLabel* m_updateLabel = nullptr;
    RigClient* m_rig = nullptr;
    RigctldLauncher* m_launcher = nullptr;
    StationModel* m_model = nullptr;
    StationFilter* m_proxy = nullptr;
    QTimer* m_tick = nullptr;

    QLabel* m_freqLabel = nullptr;
    QLabel* m_modeLabel = nullptr;
    QLabel* m_bandLabel = nullptr;
    BandPlan m_bandPlan;
    QLabel* m_clockLabel = nullptr;
    QLabel* m_countLabel = nullptr;
    QCheckBox* m_followRig = nullptr;
    QLineEdit* m_freqEdit = nullptr;
    QDoubleSpinBox* m_tolerance = nullptr;
    QCheckBox* m_onAirOnly = nullptr;
    QLineEdit* m_filter = nullptr;
    QTableView* m_table = nullptr;
    QTableView* m_menuTable = nullptr;
    QAction* m_scaleAction = nullptr;
    QAction* m_tableAction = nullptr;
    QAction* m_playerAction = nullptr;
    KiwiPlayer* m_player = nullptr;
    KiwiDirectory* m_kiwiDirectory = nullptr;
    void updatePlayer();
    DialScale* m_scale = nullptr;
    void updateScale();
    void tuneTo(double kHz, const QString& mode, qint64 flashId = 0);
    QLabel* m_rigStatus = nullptr;
    QLabel* m_dbStatus = nullptr;
    QAction* m_updateAction = nullptr;
    QAction* m_onTopAction = nullptr;

    double m_centreKHz = 0.0;
    qint64 m_rigHz = 0;
    QString m_rigMode;
    bool m_rigConnected = false;
    int m_lastEvalMinute = -1;
};
