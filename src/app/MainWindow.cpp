// SPDX-License-Identifier: GPL-3.0-or-later
#include "MainWindow.h"
#include "MyStationsDialog.h"
#include "StationEditDialog.h"
#include "StationModel.h"
#include "core/SigidWiki.h"
#include "core/AokiSource.h"
#include "core/EibiSource.h"
#include "core/TraficomSource.h"
#include "core/HfccSource.h"
#include "core/RigClient.h"
#include "core/RigctldLauncher.h"
#include "core/StationDb.h"
#include "core/Updater.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QEvent>
#include <QHash>
#include <QAbstractItemView>
#include <QKeyEvent>
#include <QAbstractSpinBox>
#include <QResizeEvent>
#include <QScreen>
#include <QSet>
#include <QShortcut>
#include <QShowEvent>
#include <QDesktopServices>
#include <QMenu>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QSortFilterProxyModel>
#include <QScrollBar>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

// Text filter over all columns plus an optional "on air only" gate.
class StationFilter : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setOnAirOnly(bool on)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
        m_onAirOnly = on;
        endFilterChange();
#else
        m_onAirOnly = on;
        invalidateFilter();   // deprecated from Qt 6.10 on, replaced above
#endif
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override
    {
        const QModelIndex idx = sourceModel()->index(row, 0, parent);
        if (m_onAirOnly)
        {
            const int rank = sourceModel()->data(idx, StationModel::OnAirRankRole).toInt();
            if (rank > 1)   // keep "on air" and "maybe"
                return false;
        }
        return QSortFilterProxyModel::filterAcceptsRow(row, parent);
    }

private:
    bool m_onAirOnly = false;
};

MainWindow::MainWindow(const QString& dataDir, double startKHz, QWidget* parent)
    : QMainWindow(parent)
    , m_dataDir(dataDir)
{
    m_db = new StationDb(m_dataDir + QStringLiteral("/stations.db"), this);
    if (!m_db->open())
        QMessageBox::critical(this, tr("Database error"),
                              tr("Cannot open %1:\n%2").arg(m_db->filePath(), m_db->lastError()));
    m_settings.load(m_db);

    m_nam = new QNetworkAccessManager(this);
    m_updater = new Updater(this);
    m_updater->addSource(new EibiSource(m_db, m_nam, this));
    m_updater->addSource(new HfccSource(m_db, m_nam, this));
    m_updater->addSource(new AokiSource(m_db, m_nam, this));
    m_updater->addSource(new TraficomSource(m_db, m_nam, this));
    m_updateCheck = new UpdateCheck(m_nam, this);
    connect(m_updateCheck, &UpdateCheck::finished, this, &MainWindow::showUpdateResult);
    m_rig = new RigClient(this);
    m_launcher = new RigctldLauncher(this);
    connect(m_launcher, &RigctldLauncher::started, this, [this]() {
        statusBar()->showMessage(tr("rigctld started"), 5000);
        m_rig->reconnectSoon();
    });
    connect(m_launcher, &RigctldLauncher::stopped, this, [this](const QString& msg) {
        statusBar()->showMessage(msg, 15000);
    });
    m_bandPlan = BandPlan::builtIn();
    m_model = new StationModel(m_db, this);
    m_proxy = new StationFilter(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(StationModel::SortRole);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(-1);

    buildUi();
    applySettings();

    connect(m_rig, &RigClient::frequencyChanged, this, &MainWindow::onRigFrequency);
    connect(m_rig, &RigClient::modeChanged, this, &MainWindow::onRigMode);
    connect(m_rig, &RigClient::stateChanged, this, &MainWindow::onRigState);
    connect(m_updater, &Updater::progress, this, [this](const QString& m) {
        statusBar()->showMessage(m);
    });
    connect(m_updater, &Updater::sourceFinished, this,
            [this](ScheduleSource*, bool, const QString&) { updateDbStatus(); });
    connect(m_updater, &Updater::finished, this, &MainWindow::onUpdateFinished);
    connect(m_db, &StationDb::changed, this, &MainWindow::refreshLookup);

    m_tick = new QTimer(this);
    m_tick->setInterval(1000);
    connect(m_tick, &QTimer::timeout, this, &MainWindow::tick);
    m_tick->start();
    tick();

    restoreGeometry(QByteArray::fromBase64(
        m_db->meta(QStringLiteral("window.geometry")).toLatin1()));
    const QByteArray savedColumns = QByteArray::fromBase64(
        m_db->meta(QStringLiteral("window.columns")).toLatin1());
    if (!savedColumns.isEmpty() && m_table->horizontalHeader()->restoreState(savedColumns))
        m_columnsFitted = true;   // the user's layout is back; no automatic fit
    // The saved state also carries resize modes; a state written by an
    // older build had the Station column locked to "stretch". Re-assert
    // that the user may drag every column.
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(false);
    // the saved state may also carry a sort indicator and clickable sections
    m_table->horizontalHeader()->setSortIndicatorShown(false);
    // widths are saved shortly after a drag, so a crash or kill loses nothing
    m_saveColumnsTimer = new QTimer(this);
    m_saveColumnsTimer->setSingleShot(true);
    m_saveColumnsTimer->setInterval(1500);
    connect(m_saveColumnsTimer, &QTimer::timeout, this, &MainWindow::saveColumns);
    connect(m_table->horizontalHeader(), &QHeaderView::sectionResized, this, [this]() {
        if (m_columnsFitted && isVisible())
            m_saveColumnsTimer->start();
    });
    // right-click on the header: choose the columns
    m_table->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table->horizontalHeader(), &QWidget::customContextMenuRequested,
            this, &MainWindow::headerContextMenu);
    // double-click anywhere on the header, title or divider: lay out all
    // columns again to fit the data and the window (deferred, because Qt
    // resizes the one column at a divider right after the signal)
    const auto refit = [this](int) {
        QTimer::singleShot(0, this, [this]() {
            m_columnsFitted = false;
            fitColumns();
            centreOnMarker();
        });
    };
    connect(m_table->horizontalHeader(), &QHeaderView::sectionDoubleClicked, this, refit);
    connect(m_table->horizontalHeader(), &QHeaderView::sectionHandleDoubleClicked, this, refit);
    m_table->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    m_table->horizontalHeader()->setSectionsClickable(false);
    // columns grow and shrink with the window
    m_table->viewport()->installEventFilter(this);
    // arrow keys tune in manual mode, wherever the focus is
    qApp->installEventFilter(this);

    if (startKHz > 0.0)
    {
        // Manual start for this session only; the stored "Follow rig"
        // preference is left untouched.
        const QSignalBlocker b(m_followRig);
        m_followRig->setChecked(false);
        m_freqEdit->setReadOnly(false);
        m_freqEdit->setText(QString::number(startKHz, 'f', 3));
        setCentreKHz(startKHz, false);
    }

    updateDbStatus();
    if (m_updater->anyStale(m_settings.refreshDays))
        QTimer::singleShot(0, this, [this]() {
            m_updateAction->setEnabled(false);
            m_updater->update(m_settings.refreshDays, false);
        });

    applyLauncher();
    m_rig->start();
    // ask once after start, then every 24 hours while the program runs
    QTimer::singleShot(3000, this, &MainWindow::startUpdateCheck);
    auto* daily = new QTimer(this);
    daily->setInterval(24 * 60 * 60 * 1000);
    connect(daily, &QTimer::timeout, this, &MainWindow::startUpdateCheck);
    daily->start();
}

void MainWindow::startUpdateCheck()
{
    if (!m_settings.updateCheck)
    {
        m_updateLabel->hide();
        return;
    }
    m_updateCheck->run(QUrl(m_settings.updateUrl));
}

void MainWindow::showUpdateResult(const UpdateCheck::Result& r)
{
    if (!r.valid)
    {
        m_updateLabel->hide();
        return;
    }
    QString text;
    if (r.newer)
        text = tr("Version %1 available").arg(r.latest);
    if (!r.message.isEmpty())
        text += (text.isEmpty() ? QString() : QStringLiteral(" · ")) + r.message.toHtmlEscaped();
    if (text.isEmpty())
    {
        m_updateLabel->hide();
        return;
    }
    if (!r.url.isEmpty())
        text = QStringLiteral("<a href=\"%1\">%2</a>").arg(r.url.toHtmlEscaped(), text);
    m_updateLabel->setText(text);
    m_updateLabel->show();
    m_updateLabel->setToolTip(r.newer ? tr("You are running %1. Click to open the download page.")
                                            .arg(QCoreApplication::applicationVersion())
                                      : QString());
}

MainWindow::~MainWindow()
{
    // The socket's destructor emits disconnected(); by then the widgets that
    // listen to the rig state are gone, so cut the connections first.
    m_rig->disconnect(this);
    m_rig->stop();
    m_launcher->stop();
}

void MainWindow::applyLauncher()
{
    if (!m_settings.launchRigctld)
    {
        m_launcher->stop();
        return;
    }
    RigctldLauncher::Config cfg;
    cfg.enabled = true;
    cfg.path = m_settings.rigctldPath;
    cfg.model = m_settings.rigModel;
    cfg.device = m_settings.rigDevice;
    cfg.baud = m_settings.rigBaud;
    cfg.extraArgs = m_settings.rigctldExtra;
    cfg.port = quint16(m_settings.rigPort);
    if (!m_launcher->start(cfg))
        QMessageBox::warning(this, tr("rigctld"),
                             tr("Could not start rigctld:\n%1\n\nCheck File > Settings > Radio.")
                                 .arg(m_launcher->lastError()));
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("otd"));
    // PNG sizes first: they need no SVG plugin, so the icon shows on every
    // platform; the SVG covers any other size
    QIcon icon;
    icon.addFile(QStringLiteral(":/otd-64.png"), QSize(64, 64));
    icon.addFile(QStringLiteral(":/otd-128.png"), QSize(128, 128));
    icon.addFile(QStringLiteral(":/otd-256.png"), QSize(256, 256));
    icon.addFile(QStringLiteral(":/otd.svg"));
    setWindowIcon(icon);
    QApplication::setWindowIcon(icon);

    // --- menu ---------------------------------------------------------
    QMenu* file = menuBar()->addMenu(tr("&File"));
    m_updateAction = file->addAction(tr("&Update databases now"), this, &MainWindow::updateDatabases);
    file->addAction(tr("Check for a &new version"), this, [this]() {
        if (!m_settings.updateCheck)
        {
            statusBar()->showMessage(tr("Version check is switched off in Settings"), 5000);
            return;
        }
        statusBar()->showMessage(tr("Checking for a new version ..."), 5000);
        startUpdateCheck();
    });
    file->addAction(tr("&Settings..."), this, &MainWindow::openSettings);
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, qApp, &QApplication::quit);

    QMenu* stations = menuBar()->addMenu(tr("&Stations"));
    stations->addAction(tr("&Add station at this frequency..."), QKeySequence(Qt::CTRL | Qt::Key_N),
                        this, &MainWindow::addMyStation);
    stations->addAction(tr("&My stations..."), QKeySequence(Qt::CTRL | Qt::Key_M),
                        this, &MainWindow::openMyStations);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    m_scaleAction = view->addAction(tr("&Dial"));
    m_scaleAction->setCheckable(true);
    m_scaleAction->setChecked(true);
    connect(m_scaleAction, &QAction::toggled, this, [this](bool on) {
        m_settings.showScale = on;
        m_scale->setVisible(on);
        if (on)
            updateScale();
    });
    m_tableAction = view->addAction(tr("Station &table"));
    m_tableAction->setCheckable(true);
    m_tableAction->setChecked(true);
    connect(m_tableAction, &QAction::toggled, this, [this](bool on) {
        m_settings.showTable = on;
        m_table->setVisible(on);
        if (on)
        {
            centreOnMarker();
            QTimer::singleShot(0, this, &MainWindow::centreOnMarker);
        }
    });
    m_playerAction = view->addAction(tr("&Online receiver (KiwiSDR)"));
    m_playerAction->setCheckable(true);
    m_playerAction->setChecked(false);
    connect(m_playerAction, &QAction::toggled, this, [this](bool on) {
        m_settings.showPlayer = on;
        m_player->setVisible(on);
        if (on)
            m_kiwiDirectory->refresh();   // at most once a day
        else
            m_player->stop();
    });
    view->addSeparator();
    m_onTopAction = view->addAction(tr("Always on &top"));
    m_onTopAction->setCheckable(true);
    connect(m_onTopAction, &QAction::toggled, this, &MainWindow::onAlwaysOnTopToggled);

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&Connecting your radio (web)"), this, []() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://otd.oh2gba.eu/rig.html")));
    });
    help->addAction(tr("&Signal Identification Wiki (web)"), this, []() {
        QDesktopServices::openUrl(SigidWiki::homeUrl());
    });
    help->addSeparator();
    help->addAction(tr("&About otd"), this, &MainWindow::about);

    // --- header -------------------------------------------------------
    m_freqLabel = new QLabel(QStringLiteral("---.--- kHz"));
    QFont big = m_freqLabel->font();
    big.setPointSize(big.pointSize() * 2 + 4);
    big.setBold(true);
    big.setFamily(QStringLiteral("monospace"));
    big.setStyleHint(QFont::Monospace);
    m_freqLabel->setFont(big);

    m_modeLabel = new QLabel;
    QFont mid = m_modeLabel->font();
    mid.setPointSize(mid.pointSize() + 3);
    m_modeLabel->setFont(mid);

    m_bandLabel = new QLabel;
    m_bandLabel->setFont(mid);
    m_bandLabel->setToolTip(tr("Allocation of the tuned frequency (ITU region set in Settings)"));

    m_clockLabel = new QLabel;
    m_clockLabel->setFont(mid);
    m_clockLabel->setToolTip(tr("Current UTC time; schedules are evaluated against it"));

    // a small link above the clock when a newer version exists
    m_updateLabel = new QLabel;
    m_updateLabel->setOpenExternalLinks(true);
    m_updateLabel->setTextFormat(Qt::RichText);
    m_updateLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_updateLabel->hide();
    auto* clockBox = new QVBoxLayout;
    clockBox->setContentsMargins(0, 0, 0, 0);
    clockBox->setSpacing(0);
    clockBox->addWidget(m_updateLabel, 0, Qt::AlignRight);
    clockBox->addWidget(m_clockLabel, 0, Qt::AlignRight);

    auto* header = new QHBoxLayout;
    header->addWidget(m_freqLabel);
    header->addSpacing(12);
    header->addWidget(m_modeLabel);
    header->addSpacing(18);
    header->addWidget(m_bandLabel);
    header->addStretch();
    header->addLayout(clockBox);

    // --- controls -----------------------------------------------------
    m_followRig = new QCheckBox(tr("Follow rig"));
    m_followRig->setToolTip(tr("Track the VFO of rigctld. Untick to type a frequency yourself."));
    connect(m_followRig, &QCheckBox::toggled, this, &MainWindow::onFollowToggled);

    m_freqEdit = new QLineEdit;
    m_freqEdit->setPlaceholderText(tr("kHz"));
    m_freqEdit->setMaximumWidth(110);
    m_freqEdit->setValidator(new QDoubleValidator(0.0, 100000.0, 3, m_freqEdit));
    connect(m_freqEdit, &QLineEdit::editingFinished, this, &MainWindow::onFrequencyEdited);

    m_tolerance = new QDoubleSpinBox;
    m_tolerance->setRange(0.1, 500.0);
    m_tolerance->setDecimals(1);
    m_tolerance->setSingleStep(1.0);
    m_tolerance->setPrefix(QStringLiteral("± "));
    m_tolerance->setSuffix(tr(" kHz"));
    m_tolerance->setToolTip(tr("Show entries this close to the tuned frequency"));
    connect(m_tolerance, &QDoubleSpinBox::valueChanged, this, &MainWindow::onToleranceChanged);

    m_onAirOnly = new QCheckBox(tr("On air only"));
    connect(m_onAirOnly, &QCheckBox::toggled, this, &MainWindow::onOnAirOnlyToggled);

    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Search all stations (name, language, site, country) ..."));
    m_filter->setToolTip(tr("With text here the whole database is searched instead of the "
                            "frequencies nearby. Double-click a row to tune the rig to it."));
    m_filter->setClearButtonEnabled(true);
    // Escape anywhere in the window drops the search and returns to the dial
    auto* esc = new QShortcut(QKeySequence::Cancel, this);
    esc->setContext(Qt::WindowShortcut);
    connect(esc, &QShortcut::activated, this, [this]() {
        if (!m_filter->text().isEmpty())
            m_filter->clear();
        else
            centreOnMarker();   // back to the VFO after scrolling around
    });
    connect(m_filter, &QLineEdit::textChanged, this, &MainWindow::onFilterChanged);

    m_countLabel = new QLabel;

    auto* controls = new QHBoxLayout;
    controls->addWidget(m_followRig);
    controls->addWidget(m_freqEdit);
    controls->addWidget(m_tolerance);
    controls->addWidget(m_onAirOnly);
    controls->addWidget(m_filter, 1);
    controls->addWidget(m_countLabel);

    // --- tables -------------------------------------------------------
    m_table = new QTableView;
    m_table->setModel(m_proxy);
    setupTable(m_table);
    m_table->setSortingEnabled(false);               // the model order is the dial
    m_table->horizontalHeader()->setSectionsClickable(false);
    m_table->horizontalHeader()->setSortIndicatorShown(false);
    // widths are saved shortly after a drag, so a crash or kill loses nothing
    m_saveColumnsTimer = new QTimer(this);
    m_saveColumnsTimer->setSingleShot(true);
    m_saveColumnsTimer->setInterval(1500);
    connect(m_saveColumnsTimer, &QTimer::timeout, this, &MainWindow::saveColumns);
    connect(m_table->horizontalHeader(), &QHeaderView::sectionResized, this, [this]() {
        if (m_columnsFitted && isVisible())
            m_saveColumnsTimer->start();
    });
    // right-click on the header: choose the columns
    m_table->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table->horizontalHeader(), &QWidget::customContextMenuRequested,
            this, &MainWindow::headerContextMenu);
    // double-click anywhere on the header, title or divider: lay out all
    // columns again to fit the data and the window (deferred, because Qt
    // resizes the one column at a divider right after the signal)
    const auto refit = [this](int) {
        QTimer::singleShot(0, this, [this]() {
            m_columnsFitted = false;
            fitColumns();
            centreOnMarker();
        });
    };
    connect(m_table->horizontalHeader(), &QHeaderView::sectionDoubleClicked, this, refit);
    connect(m_table->horizontalHeader(), &QHeaderView::sectionHandleDoubleClicked, this, refit);

    // per pixel, so the dial view can put the VFO exactly in the middle
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    m_scale = new DialScale;
    connect(m_scale, &DialScale::tuneRequested, this, [this](double kHz) {
        tuneTo(kHz, QString());
    });
    connect(m_scale, &DialScale::spanChanged, this, [this](double kHz) {
        m_settings.scaleSpanKHz = kHz;   // the zoom is remembered
    });

    m_player = new KiwiPlayer;
    m_player->setVisible(false);
    m_kiwiDirectory = new KiwiDirectory(m_db, m_nam, this);
    m_player->setDirectory(m_kiwiDirectory->receivers());
    connect(m_kiwiDirectory, &KiwiDirectory::updated, this, [this]() {
        m_player->setDirectory(m_kiwiDirectory->receivers());
    });
    connect(m_kiwiDirectory, &KiwiDirectory::failed, this, [this](const QString& why) {
        statusBar()->showMessage(tr("KiwiSDR directory not available: %1").arg(why), 8000);
    });
    connect(m_player, &KiwiPlayer::receiversChanged, this, [this]() {
        m_settings.kiwiReceivers = m_player->receivers();
        m_settings.kiwiCurrent = m_player->currentReceiver();
        m_settings.save(m_db);
    });

    auto* central = new QWidget;
    auto* layout = new QVBoxLayout(central);
    layout->addLayout(header);
    layout->addLayout(controls);
    layout->addWidget(m_scale);
    layout->addWidget(m_table, 1);
    layout->addWidget(m_player);
    setCentralWidget(central);

    // --- status bar ---------------------------------------------------
    m_rigStatus = new QLabel;
    m_dbStatus = new QLabel;
    statusBar()->addWidget(m_rigStatus, 1);
    statusBar()->addPermanentWidget(m_dbStatus);

    resize(1400, 760);
}

void MainWindow::setupTable(QTableView* table)
{
    // nothing to select: a double-click tunes, a right-click opens the menu
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->setAlternatingRowColors(false);   // the model shades per frequency instead
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(table->fontMetrics().height() + 6);
    table->horizontalHeader()->setSectionsMovable(false);
    table->horizontalHeader()->setStretchLastSection(false);
    table->setWordWrap(false);
    connect(table, &QTableView::doubleClicked, this, &MainWindow::onRowActivated);
    table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table, &QTableView::customContextMenuRequested, this, [this, table](const QPoint& pos) {
        m_menuTable = table;
        tableContextMenu(pos);
    });

    QHeaderView* h = table->horizontalHeader();
    const int em = table->fontMetrics().horizontalAdvance(QLatin1Char('M'));
    h->resizeSection(StationModel::ColDelta, em * 6);
    h->resizeSection(StationModel::ColFrequency, em * 7);
    h->resizeSection(StationModel::ColStatus, em * 7);
    h->resizeSection(StationModel::ColMode, em * 5);
    h->resizeSection(StationModel::ColStation, em * 24);
    h->resizeSection(StationModel::ColLanguage, em * 10);
    h->resizeSection(StationModel::ColTime, em * 9);
    h->resizeSection(StationModel::ColDays, em * 6);
    h->resizeSection(StationModel::ColCountry, em * 13);
    h->resizeSection(StationModel::ColSite, em * 16);
    h->resizeSection(StationModel::ColTarget, em * 12);
    h->resizeSection(StationModel::ColLastHeard, em * 6);
    h->resizeSection(StationModel::ColSource, em * 6);
    h->resizeSection(StationModel::ColRemarks, em * 20);
    h->setSectionResizeMode(QHeaderView::Interactive);
}

QModelIndex MainWindow::sourceIndex(const QModelIndex& proxyIndex) const
{
    auto* proxy = qobject_cast<const QSortFilterProxyModel*>(proxyIndex.model());
    return proxy ? proxy->mapToSource(proxyIndex) : QModelIndex();
}

void MainWindow::applySettings()
{
    m_rig->setEndpoint(m_settings.rigHost, quint16(m_settings.rigPort));
    m_rig->setPollInterval(m_settings.pollIntervalMs);
    if (ScheduleSource* src = m_updater->source(QStringLiteral("eibi")))
    {
        src->setBaseUrl(QUrl(m_settings.eibiUrl));
        src->setEnabled(m_settings.eibiEnabled);
    }
    if (ScheduleSource* src = m_updater->source(QStringLiteral("hfcc")))
    {
        src->setBaseUrl(QUrl(m_settings.hfccUrl));
        src->setEnabled(m_settings.hfccEnabled);
    }
    if (ScheduleSource* src = m_updater->source(QStringLiteral("aoki")))
    {
        src->setBaseUrl(QUrl(m_settings.aokiUrl));
        src->setEnabled(m_settings.aokiEnabled);
    }
    if (ScheduleSource* src = m_updater->source(QStringLiteral("traficom")))
    {
        src->setBaseUrl(QUrl(m_settings.traficomUrl));
        src->setEnabled(m_settings.traficomEnabled);
    }

    const QSignalBlocker b1(m_tolerance), b2(m_onAirOnly), b3(m_followRig), b4(m_onTopAction),
        b5(m_scaleAction), b6(m_tableAction);
    m_scaleAction->setChecked(m_settings.showScale);
    m_scale->setVisible(m_settings.showScale);
    m_scale->setSpanKHz(m_settings.scaleSpanKHz);
    m_tableAction->setChecked(m_settings.showTable);
    m_table->setVisible(m_settings.showTable);
    {
        const QSignalBlocker b7(m_playerAction);
        m_playerAction->setChecked(m_settings.showPlayer);
    }
    m_player->setVisible(m_settings.showPlayer);
    m_player->setReceivers(m_settings.kiwiReceivers, m_settings.kiwiCurrent);
    m_player->setVolume(m_settings.kiwiVolume);
    if (m_settings.showPlayer)
        m_kiwiDirectory->refresh();
    m_tolerance->setValue(m_settings.toleranceKHz);
    updateToleranceHint();
    m_onAirOnly->setChecked(m_settings.onAirOnly);
    m_proxy->setOnAirOnly(m_settings.onAirOnly);
    m_followRig->setChecked(m_settings.followRig);
    m_freqEdit->setReadOnly(m_settings.followRig);
    m_onTopAction->setChecked(m_settings.alwaysOnTop);
    onAlwaysOnTopToggled(m_settings.alwaysOnTop);
}

QString MainWindow::formatKHz(double kHz)
{
    // 7125.940 -> "7 125.940"
    QString s = QString::number(kHz, 'f', 3);
    const int dot = s.indexOf(QLatin1Char('.'));
    for (int i = dot - 3; i > 0; i -= 3)
        s.insert(i, QLatin1Char(' '));
    return s;
}

void MainWindow::updateHeader()
{
    if (m_centreKHz > 0.0)
        m_freqLabel->setText(formatKHz(m_centreKHz) + tr(" kHz"));
    else
        m_freqLabel->setText(QStringLiteral("---.--- kHz"));

    if (m_followRig->isChecked())
        m_modeLabel->setText(m_rigConnected ? m_rigMode : tr("no rig"));
    else
        m_modeLabel->setText(tr("manual"));

    QString band = m_centreKHz > 0.0 ? m_bandPlan.describe(m_centreKHz, m_settings.ituRegion) : QString();
    const QVector<BandPlan::Band> bands = m_bandPlan.lookup(m_centreKHz, m_settings.ituRegion);
    QString kind = bands.isEmpty() ? QString() : bands.first().kind;
    QString tip = tr("Allocation of the tuned frequency (ITU region set in Settings)");
    if (m_settings.traficomEnabled && m_centreKHz > 0.0)
    {
        // the national table replaces the built-in plan wherever it has a row
        const QList<StationDb::Allocation> rows = m_db->allocationsAt(m_centreKHz);
        if (!rows.isEmpty())
        {
            QStringList names;
            for (const StationDb::Allocation& a : rows)
            {
                QString n = a.usage.isEmpty() ? a.service : a.usage;
                if (!a.service.isEmpty() && a.service.compare(n, Qt::CaseInsensitive) != 0)
                    n += QStringLiteral(" (%1)").arg(a.service.toLower());
                if (!names.contains(n))
                    names << n;
            }
            if (names.size() > 3)
            {
                const int more = names.size() - 3;
                names = names.mid(0, 3);
                names << tr("+%1 more").arg(more);
            }
            band = names.join(QStringLiteral(" \u00b7 "));
            const QString svc = rows.first().service.toUpper();
            kind = svc.contains(QLatin1String("BROADCAST")) ? QStringLiteral("broadcast")
                 : svc.contains(QLatin1String("AMATEUR")) ? QStringLiteral("amateur")
                 : svc.contains(QLatin1String("AERONAUTICAL")) ? QStringLiteral("aero")
                 : svc.contains(QLatin1String("MARITIME")) ? QStringLiteral("maritime")
                 : svc.contains(QLatin1String("STANDARD FREQ")) ? QStringLiteral("time")
                 : svc.contains(QLatin1String("RADIONAVIGATION")) ? QStringLiteral("beacon")
                 : QStringLiteral("other");
            tip = tr("Use in Finland per Traficom's allocation table (CC BY 4.0):\n");
            for (const StationDb::Allocation& a : rows)
            {
                tip += QStringLiteral("%1 - %2 kHz: %3").arg(a.lowKHz, 0, 'f', 3).arg(a.highKHz, 0, 'f', 3)
                           .arg(a.usage.isEmpty() ? a.service : a.usage);
                if (!a.service.isEmpty() && a.service.compare(a.usage, Qt::CaseInsensitive) != 0)
                    tip += QStringLiteral(" [%1]").arg(a.service);
                if (!a.mode.isEmpty())
                    tip += QStringLiteral(", %1").arg(a.mode);
                if (!a.info.isEmpty())
                    tip += QStringLiteral(". %1").arg(a.info);
                if (!a.comment.isEmpty())
                    tip += QStringLiteral(" %1").arg(a.comment);
                tip += QLatin1Char('\n');
            }
        }
    }
    m_bandLabel->setToolTip(tip.trimmed());
    if (band.isEmpty() && m_centreKHz > 0.0)
        band = tr("no allocation listed");
    QString colour;
    if (!kind.isEmpty())
    {
        if (kind == QLatin1String("broadcast"))     colour = QStringLiteral("#e8b339");
        else if (kind == QLatin1String("amateur"))  colour = QStringLiteral("#7ee787");
        else if (kind == QLatin1String("aero"))     colour = QStringLiteral("#79c0ff");
        else if (kind == QLatin1String("maritime")) colour = QStringLiteral("#56d4dd");
        else if (kind == QLatin1String("time"))     colour = QStringLiteral("#d2a8ff");
        else if (kind == QLatin1String("beacon"))   colour = QStringLiteral("#c9d1d9");
        else if (kind == QLatin1String("informal")) colour = QStringLiteral("#ff7b72");
        else                                        colour = QStringLiteral("#ffa657");
    }
    m_bandLabel->setStyleSheet(colour.isEmpty() ? QStringLiteral("color: palette(mid);")
                                                : QStringLiteral("color: %1;").arg(colour));
    m_bandLabel->setText(band);
}

void MainWindow::setCentreKHz(double kHz, bool fromRig)
{
    if (fromRig && !m_followRig->isChecked())
        return;
    m_centreKHz = kHz;
    if (fromRig)
    {
        const QSignalBlocker b(m_freqEdit);
        m_freqEdit->setText(QString::number(kHz, 'f', 3));
    }
    updateHeader();
    refreshLookup();
}

void MainWindow::refreshLookup()
{
    const QString text = m_filter->text().trimmed();
    StationList list;
    const bool dial = text.isEmpty() && m_centreKHz > 0.0;
    m_dialActive = dial;
    if (!text.isEmpty())
    {
        // the same search again, only the VFO moved: keep the list as it
        // is and just update the distances, so tuning to a row does not
        // reshuffle the results
        const QString key = text + QLatin1Char('\n') + enabledSources().join(QLatin1Char(','));
        if (key == m_lastSearchKey && !m_model->isDialOrder())
        {
            m_model->setCentre(m_centreKHz);
            updateCountLabel();
            updateScale();
            updatePlayer();
            return;
        }
        m_lastSearchKey = key;
        list = m_db->search(text, enabledSources());
    }
    else
    {
        m_lastSearchKey.clear();
        if (dial)
            list = m_db->around(m_centreKHz, 1000, enabledSources());
        else if (m_centreKHz > 0.0)
            list = m_db->lookup(m_centreKHz, m_tolerance->value(), enabledSources());
    }

    if (dial)
    {
        m_model->setHighlightKHz(m_tolerance->value());
        // enough blank rows to fill half a screen at either end
        const int rowH = qMax(1, m_table->verticalHeader()->defaultSectionSize());
        const QScreen* scr = screen();
        const int screenH = scr ? scr->availableGeometry().height() : 1200;
        m_model->setPadding(screenH / rowH / 2 + 2);
        m_model->setDialEntries(list, m_centreKHz);
        m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        centreOnMarker();
        QTimer::singleShot(0, this, &MainWindow::centreOnMarker);
    }
    else
    {
        m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_model->setEntries(list, m_centreKHz);
    }
    m_lastEvalMinute = QDateTime::currentDateTimeUtc().time().minute();
    updateCountLabel();
    updateScale();
    updatePlayer();
    if (!m_columnsFitted)
        QTimer::singleShot(0, this, [this]() { fitColumns(); centreOnMarker(); });
}

void MainWindow::onFilterChanged(const QString&)
{
    refreshLookup();
}

void MainWindow::onRowActivated(const QModelIndex& index)
{
    const QModelIndex src = sourceIndex(index);
    if (!src.isValid() || m_model->isBlank(src.row()))
        return;
    const double kHz = m_model->data(m_model->index(src.row(), StationModel::ColFrequency),
                                     StationModel::SortRole).toDouble();
    if (kHz <= 0.0)
        return;
    const QString mode = m_model->data(m_model->index(src.row(), StationModel::ColMode),
                                       Qt::DisplayRole).toString();


    tuneTo(kHz, mode, m_model->entryAt(src.row()).id);
}

void MainWindow::tuneTo(double kHz, const QString& mode, qint64 flashId)
{
    if (m_rig->isConnected())
    {
        if (flashId != 0)
        {
            // a short blink on the row, as a receipt
            m_model->setFlash(flashId);
            QTimer::singleShot(350, this, [this]() { m_model->setFlash(0); });
        }
        // Send it to the radio and let the display follow the rig's answer.
        m_rig->setFrequency(qRound64(kHz * 1000.0));
        static const QStringList rigModes = {QStringLiteral("AM"), QStringLiteral("USB"),
                                             QStringLiteral("LSB"), QStringLiteral("CW")};
        if (rigModes.contains(mode))
            m_rig->setMode(mode);
        if (!m_followRig->isChecked())
            m_followRig->setChecked(true);
        statusBar()->showMessage(tr("Tuning rig to %1 kHz %2").arg(kHz, 0, 'f', 3).arg(mode), 5000);
        return;
    }

    if (m_followRig->isChecked())
        m_followRig->setChecked(false);   // no rig: manual mode
    m_freqEdit->setText(QString::number(kHz, 'f', 3));
    setCentreKHz(kHz, false);
}

// The scale shows one label per frequency: the best entry on it (on air
// first), with the number of further entries.
void MainWindow::updateScale()
{
    if (m_scale->isHidden())   // switched off in the View menu
        return;
    m_scale->setCentre(m_centreKHz);
    m_scale->setHighlightKHz(m_tolerance->value());
    if (!m_dialActive)
        return;   // keep the last marks while a search is open
    QVector<DialScale::Mark> marks;
    const int rows = m_model->rowCount();
    for (int r = 0; r < rows; ++r)
    {
        if (m_model->isBlank(r))
            continue;
        const StationEntry& e = m_model->entryAt(r);
        const int rank = StationModel::rank(m_model->statusAt(r));
        if (!marks.isEmpty() && qFuzzyCompare(marks.last().kHz + 1.0, e.kHz + 1.0))
        {
            DialScale::Mark& m = marks.last();
            ++m.count;
            if (rank < m.rank)
            {
                m.rank = rank;
                m.name = e.station;
            }
            continue;
        }
        DialScale::Mark m;
        m.kHz = e.kHz;
        m.name = e.station;
        m.rank = rank;
        marks.push_back(m);
    }
    m_scale->setMarks(marks);
}

void MainWindow::updateCountLabel()
{
    if (m_model->entryCount() == 0)
    {
        m_countLabel->setText(tr("no entries"));
        return;
    }
    m_countLabel->setText((m_filter->text().trimmed().isEmpty()
                               ? tr("%1 on air / %2 around")
                               : tr("%1 on air / %2 found"))
                              .arg(m_model->onAirCount())
                              .arg(m_model->entryCount()));
}

QStringList MainWindow::enabledSources() const
{
    // Returns the sources to leave out: downloadable ones that are switched
    // off. Everything else in the database is shown, the personal list and
    // any list that was put there by other means included.
    QStringList disabled;
    for (ScheduleSource* src : m_updater->sources())
        if (!src->isEnabled())
            disabled << src->id();
    return disabled;
}

void MainWindow::addMyStation()
{
    StationEntry e;
    e.kHz = m_centreKHz;
    e.mode = m_followRig->isChecked() && m_rigConnected ? m_rigMode : QString();
    StationEditDialog dlg(e, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    StationEntry fresh = dlg.entry();
    if (!m_db->insertEntry(fresh))
        QMessageBox::warning(this, tr("My stations"), m_db->lastError());
    m_lastSearchKey.clear();   // the data changed: search again
    refreshLookup();
}

void MainWindow::openMyStations()
{
    MyStationsDialog dlg(m_db, m_centreKHz, m_rigConnected ? m_rigMode : QString(), this);
    dlg.exec();
    m_lastSearchKey.clear();   // the data changed: search again
    refreshLookup();
}

void MainWindow::tableContextMenu(const QPoint& pos)
{
    QTableView* table = m_menuTable ? m_menuTable : m_table;
    const QModelIndex proxyIdx = table->indexAt(pos);
    const QModelIndex idx = sourceIndex(proxyIdx);
    QMenu menu(this);
    if (idx.isValid() && !m_model->isBlank(idx.row()))
    {
        const StationEntry e = m_model->entryAt(idx.row());
        menu.addAction(tr("Tune to %1 kHz").arg(e.kHz), this, [this, proxyIdx]() {
            onRowActivated(proxyIdx);
        });
        menu.addSeparator();
        const QUrl modeUrl = SigidWiki::modeUrl(e.mode);
        if (!modeUrl.isEmpty())
            menu.addAction(tr("What does %1 sound like? (sigidwiki)").arg(e.mode), this, [modeUrl]() {
                QDesktopServices::openUrl(modeUrl);
            });
        menu.addAction(tr("Search sigidwiki for \"%1\"").arg(e.station), this, [e]() {
            QDesktopServices::openUrl(SigidWiki::searchUrl(e.station));
        });
        menu.addSeparator();
        if (e.source == userSourceId())
        {
            menu.addAction(tr("Edit my entry..."), this, [this, e]() {
                StationEditDialog dlg(e, this);
                if (dlg.exec() == QDialog::Accepted)
                {
                    m_db->updateEntry(dlg.entry());
                    m_lastSearchKey.clear();   // the data changed: search again
                    refreshLookup();
                }
            });
            menu.addAction(tr("Delete my entry"), this, [this, e]() {
                if (QMessageBox::question(this, tr("Delete"), tr("Delete \"%1\" on %2 kHz?")
                                                                    .arg(e.station).arg(e.kHz))
                    == QMessageBox::Yes)
                {
                    m_db->removeEntry(e.id);
                    m_lastSearchKey.clear();   // the data changed: search again
                    refreshLookup();
                }
            });
        }
        else
        {
            menu.addAction(tr("Copy to my stations..."), this, [this, e]() {
                StationEntry copy = e;
                copy.id = 0;
                copy.source = userSourceId();
                if (copy.langText.isEmpty())
                    copy.langText = m_db->languageName(e.lang).section(QLatin1Char(':'), 0, 0);
                if (copy.siteText.isEmpty())
                    copy.siteText = m_db->siteName(e.itu, e.site);
                StationEditDialog dlg(copy, this);
                if (dlg.exec() == QDialog::Accepted)
                {
                    StationEntry fresh = dlg.entry();
                    m_db->insertEntry(fresh);
                    m_lastSearchKey.clear();   // the data changed: search again
                    refreshLookup();
                }
            });
        }
        menu.addSeparator();
    }
    menu.addAction(tr("Add station at %1 kHz...").arg(m_centreKHz, 0, 'f', 3), this, &MainWindow::addMyStation);
    menu.addAction(tr("My stations..."), this, &MainWindow::openMyStations);
    menu.exec(table->viewport()->mapToGlobal(pos));
}

void MainWindow::updateDbStatus()
{
    QStringList parts;
    QString tip;
    for (ScheduleSource* src : m_updater->sources())
    {
        if (!src->isEnabled())
            continue;
        const int n = src->count();
        if (n == 0)
        {
            parts << tr("%1: no data").arg(src->displayName());
            continue;
        }
        parts << (src->season().isEmpty()
                      ? QStringLiteral("%1: %2").arg(src->displayName()).arg(n)
                      : QStringLiteral("%1 %2: %3").arg(src->displayName(), src->season().toUpper()).arg(n));
        const QDateTime updated = src->lastUpdate().toLocalTime();
        tip += tr("%1 %2: %3 entries, checked %4\n")
                   .arg(src->displayName(), src->season().toUpper())
                   .arg(n)
                   .arg(updated.isValid() ? updated.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                                          : tr("never"));
    }
    QStringList known;
    for (ScheduleSource* src : m_updater->sources())
        known << src->id();
    known << userSourceId();
    for (const auto& sc : m_db->sourceCounts())
        if (!known.contains(sc.first))
            parts << QStringLiteral("%1: %2").arg(sc.first.toUpper()).arg(sc.second);
    m_dbStatus->setText(parts.isEmpty() ? tr("No sources enabled") : parts.join(QStringLiteral("  |  ")));
    m_dbStatus->setToolTip(tip.trimmed());
}

void MainWindow::onRigFrequency(qint64 hz)
{
    m_rigHz = hz;
    setCentreKHz(hz / 1000.0, true);
}

void MainWindow::onRigMode(const QString& mode, int)
{
    m_rigMode = mode;
    updateHeader();
    updatePlayer();
}

// The online receiver follows the tuned frequency, in the rig's mode when
// there is a rig, otherwise in AM.
void MainWindow::updatePlayer()
{
    if (m_centreKHz <= 0.0)
        return;
    // the online receiver follows the rig when there is one, never a
    // frequency merely typed for a look
    const bool rig = m_rigConnected && m_followRig->isChecked() && m_rigHz > 0;
    const QString mode = rig ? m_rigMode : QStringLiteral("AM");
    m_player->tune(rig ? m_rigHz / 1000.0 : m_centreKHz, mode);
}

void MainWindow::onRigState(bool connected, const QString& message)
{
    m_rigConnected = connected;
    m_rigStatus->setText(message);
    updateHeader();
}

void MainWindow::onFrequencyEdited()
{
    if (m_followRig->isChecked())
        return;
    bool ok = false;
    const double kHz = m_freqEdit->text().toDouble(&ok);
    if (ok && kHz > 0.0)
        setCentreKHz(kHz, false);
}

void MainWindow::onFollowToggled(bool follow)
{
    m_settings.followRig = follow;
    m_freqEdit->setReadOnly(follow);
    if (follow && m_rigHz > 0)
        setCentreKHz(m_rigHz / 1000.0, true);
    else
        updateHeader();
    if (!follow)
    {
        m_freqEdit->setFocus();
        m_freqEdit->selectAll();
    }
}

void MainWindow::onToleranceChanged(double kHz)
{
    m_settings.toleranceKHz = kHz;
    m_model->setHighlightKHz(kHz);   // recolour only, the list stays put
    updateScale();
}

void MainWindow::updateToleranceHint()
{
    m_tolerance->setToolTip(tr("Entries this close to the tuned frequency are shown in red"));
}

void MainWindow::onOnAirOnlyToggled(bool on)
{
    m_settings.onAirOnly = on;
    m_proxy->setOnAirOnly(on);
    updateCountLabel();
    // the rows changed under the view: put the VFO back in the middle
    centreOnMarker();
    QTimer::singleShot(0, this, &MainWindow::centreOnMarker);
}

void MainWindow::onAlwaysOnTopToggled(bool on)
{
    m_settings.alwaysOnTop = on;
    const bool visible = isVisible();
    setWindowFlag(Qt::WindowStaysOnTopHint, on);
    if (visible)
        show();
}

void MainWindow::updateDatabases()
{
    if (m_updater->isBusy())
        return;
    m_updateAction->setEnabled(false);
    m_updater->update(m_settings.refreshDays, true);
}

void MainWindow::onUpdateFinished(bool ok, const QString& message)
{
    m_updateAction->setEnabled(true);
    statusBar()->showMessage(message, ok ? 15000 : 0);
    updateDbStatus();
    if (ok)
    {
        m_lastSearchKey.clear();   // the data changed: search again
        refreshLookup();
        updateHeader();            // the allocation table may have arrived
    }
    else if (m_db->count() == 0)
        QMessageBox::warning(this, tr("Database update failed"),
                             tr("%1\n\nYou can retry from File > Update databases now.").arg(message));
}

void MainWindow::openSettings()
{
    SettingsDialog dlg(m_settings, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const AppSettings updated = dlg.settings(m_settings);
    m_settings.rigHost = updated.rigHost;
    m_settings.rigPort = updated.rigPort;
    m_settings.pollIntervalMs = updated.pollIntervalMs;
    m_settings.toleranceKHz = updated.toleranceKHz;
    m_settings.ituRegion = updated.ituRegion;
    m_settings.refreshDays = updated.refreshDays;
    const bool updateChanged = updated.updateCheck != m_settings.updateCheck
                               || updated.updateUrl != m_settings.updateUrl;
    m_settings.updateCheck = updated.updateCheck;
    m_settings.updateUrl = updated.updateUrl;
    m_settings.eibiUrl = updated.eibiUrl;
    m_settings.hfccUrl = updated.hfccUrl;
    m_settings.aokiUrl = updated.aokiUrl;
    m_settings.eibiEnabled = updated.eibiEnabled;
    m_settings.hfccEnabled = updated.hfccEnabled;
    m_settings.aokiEnabled = updated.aokiEnabled;
    m_settings.traficomUrl = updated.traficomUrl;
    m_settings.traficomEnabled = updated.traficomEnabled;
    const bool launcherChanged =
        updated.launchRigctld != m_settings.launchRigctld || updated.rigctldPath != m_settings.rigctldPath
        || updated.rigModel != m_settings.rigModel || updated.rigDevice != m_settings.rigDevice
        || updated.rigBaud != m_settings.rigBaud || updated.rigctldExtra != m_settings.rigctldExtra
        || updated.rigPort != m_settings.rigPort;
    m_settings.launchRigctld = updated.launchRigctld;
    m_settings.rigctldPath = updated.rigctldPath;
    m_settings.rigModel = updated.rigModel;
    m_settings.rigDevice = updated.rigDevice;
    m_settings.rigBaud = updated.rigBaud;
    m_settings.rigctldExtra = updated.rigctldExtra;
    m_settings.save(m_db);
    applySettings();
    if (launcherChanged)
        applyLauncher();
    updateHeader();
    updateDbStatus();
    m_lastSearchKey.clear();   // the data changed: search again
    refreshLookup();
    if (m_updater->anyStale(m_settings.refreshDays))
        updateDatabases();
    if (updateChanged)
        startUpdateCheck();
}

void MainWindow::tick()
{
    const QDateTime utc = QDateTime::currentDateTimeUtc();
    m_clockLabel->setText(utc.toString(QStringLiteral("HH:mm:ss")) + tr(" UTC"));
    if (utc.time().minute() != m_lastEvalMinute)
    {
        m_lastEvalMinute = utc.time().minute();
        m_model->refreshStatus(utc);
        updateCountLabel();
    }
}

void MainWindow::about()
{
    QMessageBox::about(
        this, tr("About otd"),
        tr("<h3>otd %1 <small style=\"font-weight:normal\">On The Dial</small></h3>"
           "<p>Shows which shortwave stations are scheduled on the frequency your "
           "receiver is tuned to, following the VFO through Hamlib's rigctld.</p>"
           "<p>Schedule data:</p><ul>"
           "<li><a href=\"http://www.eibispace.de/\">EiBi</a> by Eike Bierwirth, free for third-party software</li>"
           "<li><a href=\"http://www.hfcc.org/data/\">HFCC</a> public data files</li>"
           "<li><a href=\"http://www1.s2.starcat.ne.jp/ndxc/\">Aoki / Bi Newsletter</a> by the Nagoya DXers Circle</li>"
           "<li><a href=\"https://www.avoindata.fi/data/fi/dataset/taajuusjakotaulukko\">Traficom</a> frequency allocation table, open data, CC BY 4.0</li>"
           "<li>Public <a href=\"http://kiwisdr.com/public/\">KiwiSDR</a> receivers, listed via <a href=\"http://rx.linkfanel.net/\">rx.linkfanel.net</a></li>"
           "</ul><p>Thank you to everyone compiling these lists.</p>"
           "<p>Rig control through <a href=\"https://hamlib.github.io/\">Hamlib</a>'s rigctld. "
           "Web page: <a href=\"https://otd.oh2gba.eu/\">otd.oh2gba.eu</a></p>"
           "<p>Data directory: %2</p>"
           "<p>Licensed under the GNU GPL v3 or later. Built with Qt %3.</p>")
            .arg(QLatin1String(OTD_VERSION), m_dataDir.toHtmlEscaped(),
                 QLatin1String(qVersion())));
}

void MainWindow::saveColumns()
{
    m_db->setMeta(QStringLiteral("window.columns"),
                  QString::fromLatin1(m_table->horizontalHeader()->saveState().toBase64()));
}

void MainWindow::headerContextMenu(const QPoint& pos)
{
    QHeaderView* h = m_table->horizontalHeader();
    QMenu menu(this);
    int shown = 0;
    for (int c = 0; c < StationModel::ColumnCount; ++c)
        if (!h->isSectionHidden(c))
            ++shown;
    for (int c = 0; c < StationModel::ColumnCount; ++c)
    {
        QAction* a = menu.addAction(m_model->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString());
        a->setCheckable(true);
        a->setChecked(!h->isSectionHidden(c));
        a->setEnabled(h->isSectionHidden(c) || shown > 1);   // keep one column
        connect(a, &QAction::toggled, this, [this, c](bool on) {
            m_table->setColumnHidden(c, !on);
            scaleColumns(m_table->viewport()->width());
            centreOnMarker();
            saveColumns();   // right away, not only on a clean exit
        });
    }
    menu.exec(h->mapToGlobal(pos));
}

void MainWindow::centreOnMarker()
{
    if (!m_dialActive)
        return;
    // a model reset only schedules the layout; do it now so that the
    // scroll range is right before moving
    m_table->doItemsLayout();
    // the seam between "below the VFO" and "at or above it" goes to the
    // middle of the rows on screen
    const int rows = m_proxy->rowCount();
    int seam = rows;
    for (int r = 0; r < rows; ++r)
        if (m_proxy->index(r, 0).data(StationModel::DialSideRole).toInt() > 0)
        {
            seam = r;
            break;
        }
    QScrollBar* bar = m_table->verticalScrollBar();
    const int y = seam < rows ? m_table->rowViewportPosition(seam) + bar->value()
                              : m_table->verticalHeader()->length();
    bar->setValue(y - m_table->viewport()->height() / 2);
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    QTimer::singleShot(0, this, &MainWindow::centreOnMarker);
}

// Without a rig the arrow keys are the tuning knob: Up/Down 1 kHz,
// Page Up/Down 5 kHz, with Ctrl 0.1 kHz. Widgets that use the arrows
// themselves (lists, spin boxes) keep them.
bool MainWindow::tuneByKey(QKeyEvent* key)
{
    if (m_followRig->isChecked() || m_centreKHz <= 0.0)
        return false;
    double step = 0.0;
    switch (key->key())
    {
    case Qt::Key_Up:       step = 1.0;  break;
    case Qt::Key_Down:     step = -1.0; break;
    case Qt::Key_PageUp:   step = 5.0;  break;
    case Qt::Key_PageDown: step = -5.0; break;
    default: return false;
    }
    if (key->modifiers() & Qt::ControlModifier)
        step /= 10.0;
    QWidget* focus = QApplication::focusWidget();
    if (qobject_cast<QComboBox*>(focus) || qobject_cast<QAbstractSpinBox*>(focus)
        || qobject_cast<QAbstractItemView*>(focus) || (focus && qobject_cast<QComboBox*>(focus->parentWidget())))
        return false;
    const double kHz = qMax(0.0, m_centreKHz + step);
    m_freqEdit->setText(QString::number(kHz, 'f', 3));
    setCentreKHz(kHz, false);
    return true;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    // an application-wide filter sees every receiver, not the application
    // object: any key press while this window is active may be a tuning step
    if (event->type() == QEvent::KeyPress && isActiveWindow()
        && tuneByKey(static_cast<QKeyEvent*>(event)))
        return true;
    if (watched == m_table->viewport() && event->type() == QEvent::Resize && isVisible())
    {
        // scale every column by the same factor, so the layout the user
        // made keeps its proportions when the window changes width
        const auto* re = static_cast<QResizeEvent*>(event);
        const int oldW = re->oldSize().width();
        const int newW = re->size().width();
        if (oldW > 0 && newW > 0 && oldW != newW && m_columnsFitted)
            scaleColumns(newW);
    }
    return QMainWindow::eventFilter(watched, event);
}

// Make the visible columns add up to the given width. The narrow, fixed
// format columns (frequency, times, codes) keep their size; the text
// columns share whatever is left, in proportion to their current widths.
// Pixels are handed out cumulatively so rounding cannot drift.
void MainWindow::scaleColumns(int width)
{
    QHeaderView* h = m_table->horizontalHeader();
    static const QSet<int> fixed = {StationModel::ColDelta, StationModel::ColFrequency,
                                    StationModel::ColStatus, StationModel::ColMode,
                                    StationModel::ColTime, StationModel::ColDays,
                                    StationModel::ColLastHeard, StationModel::ColSource};
    int fixedTotal = 0, flexTotal = 0, flexCount = 0;
    for (int c = 0; c < StationModel::ColumnCount; ++c)
    {
        if (h->isSectionHidden(c))
            continue;
        if (fixed.contains(c))
            fixedTotal += h->sectionSize(c);
        else
        {
            flexTotal += h->sectionSize(c);
            ++flexCount;
        }
    }
    const int minimum = h->minimumSectionSize();
    int available = width - fixedTotal;
    if (flexCount == 0 || flexTotal <= 0 || width <= 0)
        return;
    if (available < flexCount * minimum)
        available = flexCount * minimum;   // too narrow: the scrollbar takes over
    int given = 0, seen = 0;
    for (int c = 0; c < StationModel::ColumnCount; ++c)
    {
        if (h->isSectionHidden(c) || fixed.contains(c))
            continue;
        seen += h->sectionSize(c);
        const int target = int(qint64(available) * seen / flexTotal);
        h->resizeSection(c, qMax(minimum, target - given));
        given = target;
    }
}

// Once per start, when the first rows are in: size every column to its
// contents, then fit the lot into the window.
void MainWindow::fitColumns()
{
    if (m_columnsFitted || !isVisible() || m_proxy->rowCount() == 0)
        return;
    m_columnsFitted = true;
    QHeaderView* h = m_table->horizontalHeader();
    const QFontMetrics fm = m_table->fontMetrics();
    const int em = fm.horizontalAdvance(QLatin1Char('M'));
    const int pad = em * 2;
    // the narrow format columns are sized for their widest possible value,
    // so that "▼ 999.9" or "12345.678" never get clipped when the rows change
    const QHash<int, int> least = {
        {StationModel::ColDelta,     fm.horizontalAdvance(QStringLiteral("\u25BC 999.9")) + pad},
        {StationModel::ColFrequency, fm.horizontalAdvance(QStringLiteral("12345.678")) + pad},
        {StationModel::ColStatus,    fm.horizontalAdvance(tr("inactive")) + pad},
        {StationModel::ColMode,      fm.horizontalAdvance(QStringLiteral("HFDL")) + pad},
        {StationModel::ColTime,      fm.horizontalAdvance(QStringLiteral("0000-2400")) + pad},
        {StationModel::ColDays,      fm.horizontalAdvance(QStringLiteral("Mo-Fr")) + pad},
    };
    for (int c = 0; c < StationModel::ColumnCount; ++c)
    {
        if (h->isSectionHidden(c))
            continue;
        m_table->resizeColumnToContents(c);
        h->resizeSection(c, qBound(qMax(em * 4, least.value(c, 0)), h->sectionSize(c), em * 28));
    }
    scaleColumns(m_table->viewport()->width());
    saveColumns();
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    QTimer::singleShot(0, this, &MainWindow::centreOnMarker);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    m_db->setMeta(QStringLiteral("window.geometry"),
                  QString::fromLatin1(saveGeometry().toBase64()));
    saveColumns();
    m_settings.kiwiVolume = m_player->volume();
    m_settings.kiwiReceivers = m_player->receivers();
    m_settings.kiwiCurrent = m_player->currentReceiver();
    m_settings.save(m_db);
    QMainWindow::closeEvent(event);
}

void MainWindow::screenshotTo(const QString& file, int delayMs)
{
    QTimer::singleShot(delayMs, this, [this, file]() {
        // grab() renders the widget itself; it also works on Wayland where
        // screen grabbing is not available to applications.
        if (!grab().save(file))
            qWarning("Could not save screenshot to %s", qPrintable(file));
        close();
    });
}
