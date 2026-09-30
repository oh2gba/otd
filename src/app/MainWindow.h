// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "DialScale.h"
#include "KiwiPlayer.h"
#include "SettingsDialog.h"
#include "core/Session.h"
#include <QMainWindow>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class BufferBar;
class QSortFilterProxyModel;
class QTableView;
class QTimer;
class StationModel;
class StationFilter;

// The desktop window: it draws what the Session says (the tuned frequency,
// the list, the rig's state) and hands the Session what the listener does.
// What is only about widgets stays here: the columns, the scrolling that
// keeps the VFO in the middle, the menus and dialogs, the keys and the wheel.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const QString& dataDir, double startKHz = 0.0, QWidget* parent = nullptr);

    // Development aid: save a picture of the window after it settled.
    void screenshotTo(const QString& file, int delayMs = 6000);
    ~MainWindow() override;

    const AppSettings& settings() const { return m_session->settings(); }
    // What OK in Settings does with the dialog's result. Public so that the
    // tests can take this path without the modal dialog.
    void acceptSettings(const AppSettings& updated);

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool tuneByKey(class QKeyEvent* key);
    bool wheelOnFrequency(class QWheelEvent* wheel);

private slots:
    void onCentreChanged(double kHz, Session::Origin origin);
    void onFollowRigChanged(bool follow);
    void onRigMode(const QString& mode);
    void onRigState(bool connected, const QString& message);
    void onRigAnswering(bool answering);
    void onFrequencyEdited();
    void onFollowToggled(bool follow);
    void onToleranceChanged(double kHz);
    void onOnAirOnlyToggled(bool on);
    void onFilterChanged(const QString& text);
    void onRowActivated(const QModelIndex& index);
    void onAlwaysOnTopToggled(bool on);
    void onSettingsApplied();
    void onUpdateFinished(bool ok, const QString& message, bool databaseEmpty);
    void openSettings();
    void tick();
    void about();
    void showWhatsNew();   // once, after an upgrade
    void addMyStation();
    void openMyStations();
    void tableContextMenu(const QPoint& pos);

private:
    void buildUi();
    void applyViewSettings();
    void showUpdateResult(const UpdateCheck::Result& r);
    void refreshLookup();
    void updateHeader();
    void updateDbStatus();
    void updateCountLabel();
    void centreOnMarker();
    void headerContextMenu(const QPoint& pos);
    void refitColumns();
    void refillDial();   // more of the dial when scrolled near an end of it
    void setSearching(bool searching);   // search results can be sorted, the dial cannot
    void onHeaderClicked(int column);
    void applySort();
    void saveColumns();
    void scaleColumns(int width);
    void fitColumns();
    bool m_columnsFitted = false;
    QTimer* m_saveColumnsTimer = nullptr;
    bool m_dialActive = false;
    bool m_searching = false;
    bool m_refilling = false;
    bool m_fromEdit = false;   // the frequency field itself is being read
    int m_sortColumn = -1;                    // search results sorted by this column, -1: as found
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    void setupTable(QTableView* table);
    void updateToleranceHint();
    QModelIndex sourceIndex(const QModelIndex& proxyIndex) const;

    Session* m_session = nullptr;
    QLabel* m_updateLabel = nullptr;
    StationModel* m_model = nullptr;
    StationFilter* m_proxy = nullptr;
    BufferBar* m_bufferBar = nullptr;
    QTimer* m_tick = nullptr;

    QLabel* m_freqLabel = nullptr;
    QLabel* m_modeLabel = nullptr;
    QLabel* m_bandLabel = nullptr;
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
    void updatePlayer();
    DialScale* m_scale = nullptr;
    void updateScale();
    void tuneTo(double kHz, const QString& mode, qint64 flashId = 0);
    void tuneNeighbour(int direction);   // Shift+Up/Down: the next station up or down the band
    QLabel* m_rigText = nullptr;
    QLabel* m_sdrStatus = nullptr;
    QLabel* m_dbStatus = nullptr;
    QAction* m_updateAction = nullptr;
    QAction* m_onTopAction = nullptr;

    int m_lastEvalMinute = -1;
};
