// SPDX-License-Identifier: GPL-3.0-or-later
#include "MainWindow.h"
#include "BufferBar.h"
#include "MyStationsDialog.h"
#include "StationEditDialog.h"
#include "core/StationModel.h"
#include "core/DialMarks.h"
#include "core/Format.h"
#include "core/ReleaseNotes.h"
#include "core/KiwiDirectory.h"
#include "core/SigidWiki.h"
#include "core/StationDb.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QRegularExpression>
#include <QEvent>
#include <QWheelEvent>
#include <QHash>
#include <QAbstractItemView>
#include <QKeyEvent>
#include <QAbstractSpinBox>
#include <QRegularExpressionValidator>
#include <QResizeEvent>
#include <QScreen>
#include <QSet>
#include <QShortcut>
#include <QShowEvent>
#include <QDesktopServices>
#include <QMenu>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
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
{
    m_session = new Session(dataDir, this);
    if (!m_session->isOpen())
        QMessageBox::critical(this, tr("Database error"), m_session->lastError());

    m_model = new StationModel(m_session->db(), this);
    m_proxy = new StationFilter(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(StationModel::SortRole);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(-1);

    buildUi();
    applyViewSettings();
    // The rest of the window state is restored once, here: Settings does not
    // show it, and from now on the widgets hold the current values (Follow
    // rig may be unticked for this session only, the volume is saved on
    // exit). OK in Settings must not put the values from the start back.
    const AppSettings& s = m_session->settings();
    {
        const QSignalBlocker b(m_followRig);
        m_followRig->setChecked(m_session->followRig());
    }
    m_freqEdit->setReadOnly(m_session->followRig());
    m_player->setReceivers(s.kiwiReceivers, s.kiwiFavourites, s.kiwiCurrent);
    m_player->setManualMode(s.kiwiMode);
    m_player->setVolume(s.kiwiVolume);

    connect(m_session, &Session::centreChanged, this, &MainWindow::onCentreChanged);
    connect(m_session, &Session::followRigChanged, this, &MainWindow::onFollowRigChanged);
    connect(m_session, &Session::rigModeChanged, this, &MainWindow::onRigMode);
    connect(m_session, &Session::rigStateChanged, this, &MainWindow::onRigState);
    connect(m_session, &Session::rigAnsweringChanged, this, &MainWindow::onRigAnswering);
    connect(m_session, &Session::manualModeChosen, m_player, &KiwiPlayer::setManualMode);
    connect(m_session, &Session::message, this, [this](const QString& text, int ms) {
        statusBar()->showMessage(text, ms);
    });
    connect(m_session, &Session::settingsApplied, this, &MainWindow::onSettingsApplied);
    connect(m_session, &Session::updateStarted, this, [this]() { m_updateAction->setEnabled(false); });
    connect(m_session, &Session::updateFinished, this, &MainWindow::onUpdateFinished);
    connect(m_session, &Session::sourcesChanged, this, &MainWindow::updateDbStatus);
    connect(m_session, &Session::versionChecked, this, &MainWindow::showUpdateResult);
    connect(m_session, &Session::launcherFailed, this, [this](const QString& error) {
        QMessageBox::warning(this, tr("rigctld"),
                             tr("Could not start rigctld:\n%1\n\nCheck File > Settings > Radio.").arg(error));
    });
    connect(m_session->db(), &StationDb::changed, this, &MainWindow::refreshLookup);

    m_tick = new QTimer(this);
    m_tick->setInterval(1000);
    connect(m_tick, &QTimer::timeout, this, &MainWindow::tick);
    m_tick->start();
    tick();

    StationDb* db = m_session->db();
    restoreGeometry(QByteArray::fromBase64(db->meta(QStringLiteral("window.geometry")).toLatin1()));
    const QByteArray savedColumns = QByteArray::fromBase64(
        db->meta(QStringLiteral("window.columns")).toLatin1());
    if (!savedColumns.isEmpty() && m_table->horizontalHeader()->restoreState(savedColumns))
        m_columnsFitted = true;   // the user's layout is back; no automatic fit
    // The saved state also carries resize modes; a state written by an
    // older build had the Station column locked to "stretch". Re-assert
    // that the user may drag every column.
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(false);
    // the saved state may also carry a sort indicator and clickable sections
    m_table->horizontalHeader()->setSortIndicatorShown(false);
    // (the column timer, the header menu and the double-click fit are set
    // up in buildUi)
    m_table->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    m_table->horizontalHeader()->setSectionsClickable(false);
    // columns grow and shrink with the window
    m_table->viewport()->installEventFilter(this);
    // arrow keys tune in manual mode, wherever the focus is
    qApp->installEventFilter(this);
    // and the wheel over a digit of the big frequency turns that digit
    m_freqLabel->installEventFilter(this);
    m_freqLabel->setToolTip(tr("Turn the mouse wheel over a digit to change it (the rig follows when one is connected)"));

    updateDbStatus();
    m_session->start(startKHz);
    if (m_session->isUpgrade())
        QTimer::singleShot(0, this, &MainWindow::showWhatsNew);   // once the window is on screen
}

void MainWindow::showWhatsNew()
{
    QString items;
    for (const QString& note : ReleaseNotes::current())
        items += QStringLiteral("<li>%1</li>").arg(note.toHtmlEscaped());
    auto* box = new QMessageBox(this);
    box->setObjectName(QStringLiteral("whatsNew"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowTitle(tr("What's new"));
    box->setTextFormat(Qt::RichText);
    box->setText(tr("<h3>What's new in On The Dial %1</h3>"
                    "<p>Thanks for updating! Here is what changed:</p><ul>%2</ul>"
                    "<p>A big thank you to everyone who sent feedback: several of these changes "
                    "come straight from your messages. Keep them coming!</p>")
                     .arg(QCoreApplication::applicationVersion(), items));
    box->setStandardButtons(QMessageBox::Ok);
    box->button(QMessageBox::Ok)->setText(tr("Got it"));
    box->open();   // not blocking: the program goes on starting behind it
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
    // the widgets that listen to the rig go with this window: nothing the
    // rig's socket says on its way out may reach them
    m_session->stop();
}

void MainWindow::buildUi()
{
    // a pre-release build says so in the title, so the tester knows what runs
    setWindowTitle(QLatin1String(OTD_VERSION).contains(QLatin1Char('-'))
                       ? QStringLiteral("On The Dial beta")
                       : QStringLiteral("On The Dial"));
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
    m_updateAction = file->addAction(tr("&Update databases now"), m_session, &Session::updateDatabases);
    file->addAction(tr("Check for a &new version"), m_session, &Session::checkForNewVersionNow);
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
        m_session->settings().showScale = on;
        m_session->saveSettings();   // at once, not only on a clean exit
        m_scale->setVisible(on);
        if (on)
            updateScale();
    });
    m_tableAction = view->addAction(tr("Station &table"));
    m_tableAction->setCheckable(true);
    m_tableAction->setChecked(true);
    connect(m_tableAction, &QAction::toggled, this, [this](bool on) {
        m_session->settings().showTable = on;
        m_session->saveSettings();
        m_table->setVisible(on);
        if (on)
        {
            centreOnMarker();
            QTimer::singleShot(0, this, &MainWindow::centreOnMarker);
        }
    });
    m_playerAction = view->addAction(tr("&Online receiver (KiwiSDR)"));
    m_playerAction->setCheckable(true);
    m_playerAction->setChecked(true);
    connect(m_playerAction, &QAction::toggled, this, [this](bool on) {
        m_session->settings().showPlayer = on;
        m_session->saveSettings();
        m_player->setVisible(on);
        if (on && m_session->kiwiDirectory()->receivers().isEmpty())
            m_session->kiwiDirectory()->refresh(true);   // first fill; later refreshes come with use
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
    m_freqLabel->setObjectName(QStringLiteral("frequencyLabel"));   // the tests look widgets up by name
    QFont big = m_freqLabel->font();
    big.setPointSize(big.pointSize() * 2 + 4);
    big.setBold(true);
    big.setFamily(QStringLiteral("monospace"));
    big.setStyleHint(QFont::Monospace);
    m_freqLabel->setFont(big);

    m_modeLabel = new QLabel;
    m_modeLabel->setObjectName(QStringLiteral("modeLabel"));
    QFont mid = m_modeLabel->font();
    mid.setPointSize(mid.pointSize() + 3);
    m_modeLabel->setFont(mid);

    m_bandLabel = new QLabel;
    m_bandLabel->setObjectName(QStringLiteral("bandLabel"));
    m_bandLabel->setFont(mid);
    m_bandLabel->setToolTip(tr("Allocation of the tuned frequency (ITU region set in Settings)"));

    m_clockLabel = new QLabel;
    m_clockLabel->setObjectName(QStringLiteral("clockLabel"));
    m_clockLabel->setFont(mid);
    m_clockLabel->setToolTip(tr("Current UTC time; schedules are evaluated against it"));

    // a small link above the clock when a newer version exists
    m_updateLabel = new QLabel;
    m_updateLabel->setObjectName(QStringLiteral("updateLabel"));
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
    m_followRig->setObjectName(QStringLiteral("followRig"));
    m_followRig->setToolTip(tr("Track the VFO of rigctld. Untick to type a frequency yourself."));
    connect(m_followRig, &QCheckBox::toggled, this, &MainWindow::onFollowToggled);

    m_freqEdit = new QLineEdit;
    m_freqEdit->setObjectName(QStringLiteral("frequencyEdit"));
    m_freqEdit->setPlaceholderText(tr("kHz"));
    m_freqEdit->setMaximumWidth(110);
    // kHz with a point or a comma before the decimals, whatever the
    // system's number format (a locale-aware validator lets "9,500" through
    // as a thousands group, or rejects the point, and onFrequencyEdited
    // could not read either)
    m_freqEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("\\d{1,6}([.,]\\d{0,3})?")), m_freqEdit));
    connect(m_freqEdit, &QLineEdit::editingFinished, this, &MainWindow::onFrequencyEdited);

    m_tolerance = new QDoubleSpinBox;
    m_tolerance->setObjectName(QStringLiteral("tolerance"));
    m_tolerance->setRange(0.1, 500.0);
    m_tolerance->setDecimals(1);
    m_tolerance->setSingleStep(1.0);
    m_tolerance->setPrefix(QStringLiteral("± "));
    m_tolerance->setSuffix(tr(" kHz"));
    m_tolerance->setToolTip(tr("Show entries this close to the tuned frequency"));
    connect(m_tolerance, &QDoubleSpinBox::valueChanged, this, &MainWindow::onToleranceChanged);

    m_onAirOnly = new QCheckBox(tr("On air only"));
    m_onAirOnly->setObjectName(QStringLiteral("onAirOnly"));
    connect(m_onAirOnly, &QCheckBox::toggled, this, &MainWindow::onOnAirOnlyToggled);

    m_filter = new QLineEdit;
    m_filter->setObjectName(QStringLiteral("search"));
    m_filter->setPlaceholderText(tr("Search all stations, or target:, language:, country:, site:, station:, mode: ..."));
    m_filter->setToolTip(tr("With text here the whole database is searched instead of the "
                            "frequencies nearby. Every word must match; !word leaves matches out.\n"
                            "field:value looks in one column only: target:, language:, "
                            "country:, site:, station:, mode: (a quoted value may have spaces)\n"
                            "Double-click a row to tune the rig to it."));
    m_filter->setClearButtonEnabled(true);
    // Escape anywhere in the window drops the search and returns to the dial
    auto* esc = new QShortcut(QKeySequence::Cancel, this);
    esc->setContext(Qt::WindowShortcut);
    connect(esc, &QShortcut::activated, this, [this]() {
        if (!m_filter->text().isEmpty())
            m_filter->clear();
        else
        {
            m_anchorId = 0;     // centred on the VFO, whatever was clicked
            centreOnMarker();   // back to the VFO after scrolling around
        }
    });
    connect(m_filter, &QLineEdit::textChanged, this, &MainWindow::onFilterChanged);

    m_countLabel = new QLabel;
    m_countLabel->setObjectName(QStringLiteral("countLabel"));

    auto* controls = new QHBoxLayout;
    controls->addWidget(m_followRig);
    controls->addWidget(m_freqEdit);
    controls->addWidget(m_tolerance);
    controls->addWidget(m_onAirOnly);
    controls->addWidget(m_filter, 1);
    controls->addWidget(m_countLabel);

    // --- tables -------------------------------------------------------
    m_table = new QTableView;
    m_table->setObjectName(QStringLiteral("stationTable"));
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
    // double-click on a divider (and on a title in the dial view, where a
    // click does nothing else): lay out all columns again to fit the data
    // and the window
    connect(m_table->horizontalHeader(), &QHeaderView::sectionDoubleClicked, this, [this](int) {
        if (!m_searching)
            refitColumns();
    });
    connect(m_table->horizontalHeader(), &QHeaderView::sectionHandleDoubleClicked, this,
            [this](int) { refitColumns(); });
    // while searching, a click on a title sorts the results by that column
    connect(m_table->horizontalHeader(), &QHeaderView::sectionClicked, this, &MainWindow::onHeaderClicked);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);

    // per pixel, so the dial view can put the VFO exactly in the middle
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // scrolling towards an end of the loaded dial loads more
    connect(m_table->verticalScrollBar(), &QScrollBar::valueChanged, this, &MainWindow::refillDial);

    m_scale = new DialScale;
    connect(m_scale, &DialScale::tuneRequested, this, [this](double kHz) {
        tuneTo(kHz, QString());
    });
    connect(m_scale, &DialScale::spanChanged, this, [this](double kHz) {
        m_session->settings().scaleSpanKHz = kHz;   // the zoom is remembered
        m_session->saveSettingsSoon();
    });

    m_player = new KiwiPlayer;
    m_player->setVisible(false);
    KiwiDirectory* directory = m_session->kiwiDirectory();
    m_player->setDirectory(directory->receivers());
    connect(directory, &KiwiDirectory::updated, this, [this, directory]() {
        m_player->setDirectory(directory->receivers());
    });
    // the receiver list is refreshed when the player is actually used, and
    // at most once a week (the file changes every few minutes, so a
    // conditional request would not save anything)
    connect(m_player, &KiwiPlayer::playRequested, this, [directory]() { directory->refresh(); });
    connect(m_player, &KiwiPlayer::manualModeChanged, this, [this](const QString& mode) {
        m_session->settings().kiwiMode = mode;
        m_session->saveSettings();
    });
    connect(m_player, &KiwiPlayer::receiversChanged, this, [this]() {
        AppSettings& s = m_session->settings();
        s.kiwiReceivers = m_player->receivers();
        s.kiwiFavourites = m_player->favourites();
        s.kiwiCurrent = m_player->currentReceiver();
        m_session->saveSettings();
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
    m_rigText = new QLabel;   // only speaks up when the rig is not answering
    m_rigText->setObjectName(QStringLiteral("rigStatus"));
    m_sdrStatus = new QLabel;
    connect(m_player, &KiwiPlayer::statusChanged, this, [this](const QString& text) {
        m_sdrStatus->setText(text.isEmpty() ? QString() : tr("KiwiSDR: %1").arg(text));
    });
    m_dbStatus = new QLabel;
    m_dbStatus->setObjectName(QStringLiteral("dbStatus"));
    // audio in hand for the online receiver, in the corner while it plays
    m_bufferBar = new BufferBar;
    m_bufferBar->hide();
    statusBar()->addWidget(m_bufferBar);
    connect(m_player, &KiwiPlayer::audioInHandChanged, this, [this](double seconds) {
        if (m_player->isPlaying())
        {
            m_bufferBar->setSeconds(seconds);
            // A message in the status bar ("Checking for a new version")
            // takes the left corner for its time: the status bar hides the
            // bar and shows it again afterwards, so only show it when no
            // message is up.
            if (statusBar()->currentMessage().isEmpty())
                m_bufferBar->show();
        }
        else
        {
            m_bufferBar->clear();
            m_bufferBar->hide();
        }
    });
    statusBar()->addWidget(m_rigText);
    statusBar()->addWidget(m_sdrStatus, 1);
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

// The widgets that mirror a setting: after the start and after OK in Settings.
void MainWindow::applyViewSettings()
{
    const AppSettings& s = m_session->settings();
    const QSignalBlocker b1(m_tolerance), b2(m_onAirOnly), b3(m_onTopAction),
        b4(m_scaleAction), b5(m_tableAction);
    m_scaleAction->setChecked(s.showScale);
    m_scale->setVisible(s.showScale);
    m_scale->setSpanKHz(s.scaleSpanKHz);
    m_tableAction->setChecked(s.showTable);
    m_table->setVisible(s.showTable);
    {
        const QSignalBlocker b6(m_playerAction);
        m_playerAction->setChecked(s.showPlayer);
    }
    m_player->setVisible(s.showPlayer);
    if (s.showPlayer)
        if (m_session->kiwiDirectory()->receivers().isEmpty())
            m_session->kiwiDirectory()->refresh(true);   // first fill only
    m_tolerance->setValue(s.toleranceKHz);
    updateToleranceHint();
    m_onAirOnly->setChecked(s.onAirOnly);
    m_proxy->setOnAirOnly(s.onAirOnly);
    m_onTopAction->setChecked(s.alwaysOnTop);
    onAlwaysOnTopToggled(s.alwaysOnTop);
}

void MainWindow::updateHeader()
{
    m_freqLabel->setText(m_session->frequencyText());
    m_modeLabel->setText(m_session->modeText());

    const BandLine line = m_session->bandLine();
    QString band = line.text;
    m_bandLabel->setToolTip(line.tooltip.isEmpty()
                                ? tr("Allocation of the tuned frequency (ITU region set in Settings)")
                                : line.tooltip);
    if (band.isEmpty() && m_session->centreKHz() > 0.0)
        band = tr("no allocation listed");
    QString colour;
    if (!line.kind.isEmpty())
    {
        const QString& kind = line.kind;
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

void MainWindow::onCentreChanged(double kHz, Session::Origin)
{
    if (!m_fromEdit)
    {
        // the field says the same, unless it is what was just read
        const QSignalBlocker b(m_freqEdit);
        m_freqEdit->setText(QString::number(kHz, 'f', 3));
    }
    updateHeader();
    refreshLookup();
}

void MainWindow::refreshLookup()
{
    const double centre = m_session->centreKHz();
    const Session::Rows rows = m_session->rows(m_filter->text(), !m_model->isDialOrder());
    const bool dial = rows.kind == Session::Lookup::Dial;
    m_dialActive = dial;
    setSearching(rows.kind == Session::Lookup::Search);
    if (rows.same)
    {
        // the same search again, only the VFO moved: the list stays, the
        // distances follow
        m_model->setCentre(centre);
        updateCountLabel();
        updateScale();
        updatePlayer();
        return;
    }

    if (dial)
    {
        m_model->setHighlightKHz(m_tolerance->value());
        // enough blank rows to fill half a screen at either end
        const int rowH = qMax(1, m_table->verticalHeader()->defaultSectionSize());
        const QScreen* scr = screen();
        const int screenH = scr ? scr->availableGeometry().height() : 1200;
        m_model->setPadding(screenH / rowH / 2 + 2);
        m_model->setDialEntries(rows.list, centre);
        m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        centreOnMarker();
        QTimer::singleShot(0, this, &MainWindow::centreOnMarker);
    }
    else
    {
        m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_model->setEntries(rows.list, centre);
    }
    m_lastEvalMinute = QDateTime::currentDateTimeUtc().time().minute();
    updateCountLabel();
    updateScale();
    updatePlayer();
    if (!m_columnsFitted)
        QTimer::singleShot(0, this, [this]() { fitColumns(); centreOnMarker(); });
}

void MainWindow::refitColumns()
{
    // deferred: Qt resizes the one column at a divider right after the signal
    QTimer::singleShot(0, this, [this]() {
        m_columnsFitted = false;
        fitColumns();
        centreOnMarker();
    });
}

void MainWindow::setSearching(bool searching)
{
    // The dial view and the nearby list are in frequency order, the order
    // of the dial, and cannot be sorted. Search results can: a click on a
    // column title, once more for the other way round, a third time for
    // the usual order (on air first, then by frequency). Leaving the
    // search drops the sorting.
    QHeaderView* h = m_table->horizontalHeader();
    if (searching == m_searching)
        return;
    m_searching = searching;
    h->setSectionsClickable(searching);
    if (!searching)
    {
        m_sortColumn = -1;
        applySort();
    }
}

void MainWindow::onHeaderClicked(int column)
{
    if (!m_searching)
        return;
    if (column != m_sortColumn)
    {
        m_sortColumn = column;
        m_sortOrder = Qt::AscendingOrder;
    }
    else if (m_sortOrder == Qt::AscendingOrder)
        m_sortOrder = Qt::DescendingOrder;
    else
        m_sortColumn = -1;
    applySort();
}

void MainWindow::applySort()
{
    QHeaderView* h = m_table->horizontalHeader();
    // column -1 is the model's own order; descending would turn even that round
    m_proxy->sort(m_sortColumn, m_sortColumn < 0 ? Qt::AscendingOrder : m_sortOrder);
    h->setSortIndicatorShown(m_sortColumn >= 0);
    h->setSortIndicator(m_sortColumn, m_sortOrder);
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
    if (m_dialActive && index.model() == m_proxy)
    {
        m_anchorId = m_model->entryAt(src.row()).id;
        m_anchorY = m_table->rowViewportPosition(index.row());
        m_anchorKHz = kHz;
        m_anchorClock.start();
    }
    tuneTo(kHz, mode, m_model->entryAt(src.row()).id);
}

void MainWindow::tuneTo(double kHz, const QString& mode, qint64 flashId)
{
    if (!m_flashTimer)
    {
        m_flashTimer = new QTimer(this);
        m_flashTimer->setSingleShot(true);
        m_flashTimer->setInterval(350);
        connect(m_flashTimer, &QTimer::timeout, this, [this]() { m_model->setFlash(0); });
    }
    if (flashId != 0)
    {
        // a short blink on the row, as a receipt
        m_model->setFlash(flashId);
        m_flashTimer->start();
    }
    m_session->tuneTo(kHz, mode);
}

// The scale shows one label per frequency: the best entry on it (on air
// first), with the number of further entries.
void MainWindow::updateScale()
{
    if (m_scale->isHidden())   // switched off in the View menu
        return;
    m_scale->setCentre(m_session->centreKHz());
    m_scale->setHighlightKHz(m_tolerance->value());
    if (!m_dialActive)
        return;   // keep the last marks while a search is open
    // the same rows the list shows: "On air only" applies to the scale too
    QVector<DialMark> rows;
    const int count = m_proxy->rowCount();
    for (int pr = 0; pr < count; ++pr)
    {
        const int r = m_proxy->mapToSource(m_proxy->index(pr, 0)).row();
        if (r < 0 || m_model->isBlank(r))
            continue;
        const StationEntry& e = m_model->entryAt(r);
        DialMark m;
        m.kHz = e.kHz;
        m.name = m_session->db()->names().stationOf(e);
        m.rank = StationModel::rank(m_model->statusAt(r));
        rows.push_back(m);
    }
    m_scale->setMarks(groupDialMarks(rows));
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

void MainWindow::addMyStation()
{
    StationEntry e;
    e.kHz = m_session->centreKHz();
    e.mode = m_session->followRig() && m_session->rigAnswering() ? m_session->rigMode() : QString();
    StationEditDialog dlg(e, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    StationEntry fresh = dlg.entry();
    if (!m_session->db()->insertEntry(fresh))
        QMessageBox::warning(this, tr("My stations"), m_session->db()->lastError());
    m_session->forgetSearch();   // the data changed: search again
    refreshLookup();
}

void MainWindow::openMyStations()
{
    MyStationsDialog dlg(m_session->db(), m_session->centreKHz(),
                         m_session->rigAnswering() ? m_session->rigMode() : QString(), this);
    dlg.exec();
    m_session->forgetSearch();   // the data changed: search again
    refreshLookup();
}

void MainWindow::tableContextMenu(const QPoint& pos)
{
    QTableView* table = m_menuTable ? m_menuTable : m_table;
    StationDb* db = m_session->db();
    const QModelIndex proxyIdx = table->indexAt(pos);
    const QModelIndex idx = sourceIndex(proxyIdx);
    QMenu menu(this);
    if (idx.isValid() && !m_model->isBlank(idx.row()))
    {
        const StationEntry e = m_model->entryAt(idx.row());
        const QString station = db->names().stationOf(e);
        const QString mode = StationNames::modeOf(e);
        menu.addAction(tr("Tune to %1 kHz").arg(e.kHz), this, [this, proxyIdx]() {
            onRowActivated(proxyIdx);
        });
        menu.addSeparator();
        const QUrl modeUrl = SigidWiki::modeUrl(mode);
        if (!modeUrl.isEmpty())
            menu.addAction(tr("What does %1 sound like? (sigidwiki)").arg(mode), this, [modeUrl]() {
                QDesktopServices::openUrl(modeUrl);
            });
        menu.addAction(tr("Search sigidwiki for \"%1\"").arg(station), this, [station]() {
            QDesktopServices::openUrl(SigidWiki::searchUrl(station));
        });
        menu.addSeparator();
        if (e.source == userSourceId())
        {
            menu.addAction(tr("Edit my entry..."), this, [this, db, e]() {
                StationEditDialog dlg(e, this);
                if (dlg.exec() == QDialog::Accepted)
                {
                    db->updateEntry(dlg.entry());
                    m_session->forgetSearch();   // the data changed: search again
                    refreshLookup();
                }
            });
            menu.addAction(tr("Delete my entry"), this, [this, db, e]() {
                if (QMessageBox::question(this, tr("Delete"), tr("Delete \"%1\" on %2 kHz?")
                                                                    .arg(e.station).arg(e.kHz))
                    == QMessageBox::Yes)
                {
                    db->removeEntry(e.id);
                    m_session->forgetSearch();   // the data changed: search again
                    refreshLookup();
                }
            });
        }
        else
        {
            menu.addAction(tr("Copy to my stations..."), this, [this, db, e]() {
                // the listener's own entry, filled with what the list shows
                StationEntry copy = e;
                copy.id = 0;
                copy.source = userSourceId();
                copy.station = db->names().stationOf(e);
                copy.lang = db->names().languageOf(e);
                copy.site = db->names().siteOf(e);
                copy.target = db->names().targetOf(e);
                copy.days = db->names().daysOf(e);
                copy.mode = StationNames::modeOf(e);
                copy.remarks = StationNames::remarksOf(e);
                copy.power.clear();
                copy.azimuth.clear();
                copy.flag.clear();
                StationEditDialog dlg(copy, this);
                if (dlg.exec() == QDialog::Accepted)
                {
                    StationEntry fresh = dlg.entry();
                    db->insertEntry(fresh);
                    m_session->forgetSearch();   // the data changed: search again
                    refreshLookup();
                }
            });
        }
        menu.addSeparator();
    }
    menu.addAction(tr("Add station at %1 kHz...").arg(m_session->centreKHz(), 0, 'f', 3), this,
                   &MainWindow::addMyStation);
    menu.addAction(tr("My stations..."), this, &MainWindow::openMyStations);
    menu.exec(table->viewport()->mapToGlobal(pos));
}

void MainWindow::updateDbStatus()
{
    QString tip;
    m_dbStatus->setText(m_session->sourcesStatus(&tip));
    m_dbStatus->setToolTip(tip);
}

void MainWindow::onRigMode(const QString&)
{
    updateHeader();
    updatePlayer();
}

// The online receiver follows the tuned frequency, in the rig's mode when
// there is a rig, otherwise in AM.
void MainWindow::updatePlayer()
{
    const double centre = m_session->centreKHz();
    if (centre <= 0.0)
        return;
    // the online receiver follows the rig when there is one, never a
    // frequency merely typed for a look
    const bool rig = m_session->rigAnswering() && m_session->followRig() && m_session->rigHz() > 0;
    m_player->tune(rig ? m_session->rigHz() / 1000.0 : centre, rig ? m_session->rigMode() : QString());
}

void MainWindow::onRigState(bool, const QString& message)
{
    // The connection's ups and downs (reconnects, error reports from a
    // rigctld whose radio is off) only refresh the tooltip; whether the rig
    // counts as there is decided by RigClient::answeringChanged.
    m_rigText->setToolTip(message);
}

void MainWindow::onRigAnswering(bool answering)
{
    // (a rig that never answered: the Session has already put the
    // remembered frequency on the dial, through centreChanged)
    if (answering)
        m_rigText->clear();
    else if (m_session->followRig())
        m_rigText->setText(m_session->rigSilentText());
    updateHeader();
    updatePlayer();   // the mode box changes hands with the rig
}

void MainWindow::onFrequencyEdited()
{
    if (m_session->followRig())
        return;
    bool ok = false;
    const double kHz = QString(m_freqEdit->text()).replace(QLatin1Char(','), QLatin1Char('.')).toDouble(&ok);
    if (ok && kHz > 0.0)
    {
        m_fromEdit = true;   // what was typed stays as typed
        m_session->setManualKHz(kHz);
        m_fromEdit = false;
    }
}

void MainWindow::onFollowToggled(bool follow)
{
    m_session->setFollowRig(follow);
}

void MainWindow::onFollowRigChanged(bool follow)
{
    {
        const QSignalBlocker b(m_followRig);
        m_followRig->setChecked(follow);
    }
    if (!follow)
        m_rigText->clear();   // manual mode: no complaint about a silent rig
    else if (!m_session->rigAnswering() && m_rigText->text().isEmpty())
        m_rigText->setText(m_session->rigSilentText());
    updatePlayer();           // the mode box changes hands with the switch
    m_freqEdit->setReadOnly(follow);
    updateHeader();
    if (!follow)
    {
        m_freqEdit->setFocus();
        m_freqEdit->selectAll();
    }
}

void MainWindow::onToleranceChanged(double kHz)
{
    m_session->settings().toleranceKHz = kHz;
    m_session->saveSettingsSoon();
    m_model->setHighlightKHz(kHz);   // recolour only, the list stays put
    updateScale();
}

void MainWindow::updateToleranceHint()
{
    m_tolerance->setToolTip(tr("Entries this close to the tuned frequency are shown in red"));
}

void MainWindow::onOnAirOnlyToggled(bool on)
{
    m_session->settings().onAirOnly = on;
    m_session->saveSettings();
    m_proxy->setOnAirOnly(on);
    updateCountLabel();
    updateScale();
    // the rows changed under the view: put the VFO back in the middle
    centreOnMarker();
    QTimer::singleShot(0, this, &MainWindow::centreOnMarker);
}

void MainWindow::onAlwaysOnTopToggled(bool on)
{
    m_session->settings().alwaysOnTop = on;
    const bool visible = isVisible();
    setWindowFlag(Qt::WindowStaysOnTopHint, on);
    if (visible)
        show();
}

void MainWindow::onUpdateFinished(bool ok, const QString& message, bool databaseEmpty)
{
    m_updateAction->setEnabled(true);
    statusBar()->showMessage(message, ok ? 15000 : 0);
    updateDbStatus();
    if (ok)
    {
        refreshLookup();   // the data changed: the Session searches again
        updateHeader();    // the allocation table may have arrived
    }
    else if (databaseEmpty)
        QMessageBox::warning(this, tr("Database update failed"),
                             tr("%1\n\nYou can retry from File > Update databases now.").arg(message));
}

void MainWindow::openSettings()
{
    SettingsDialog dlg(m_session->settings(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    acceptSettings(dlg.settings(m_session->settings()));
}

void MainWindow::acceptSettings(const AppSettings& updated)
{
    m_session->acceptSettings(updated);
}

void MainWindow::onSettingsApplied()
{
    applyViewSettings();
    // a warning on screen names the address now in use; whether that one
    // answers is known after the next polls
    if (!m_rigText->text().isEmpty())
        m_rigText->setText(m_session->rigSilentText());
    updateHeader();
    updateDbStatus();
    refreshLookup();
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
        tr("<h3>On The Dial %1 <small style=\"font-weight:normal\">otd</small></h3>"
           "<p>Shows which shortwave stations are scheduled on the frequency your "
           "receiver is tuned to, following the VFO through Hamlib's rigctld.</p>"
           "<p>Schedule data:</p><ul>"
           "<li><a href=\"http://www.eibispace.de/\">EiBi</a> by Eike Bierwirth, free for third-party software</li>"
           "<li><a href=\"http://www.hfcc.org/data/\">HFCC</a> public data files</li>"
           "<li><a href=\"http://www1.s2.starcat.ne.jp/ndxc/\">Aoki / Bi Newsletter</a> by the Nagoya DXers Circle</li>"
           "<li><a href=\"https://avoindata.suomi.fi/data/en_GB/dataset/taajuusjakotaulukko\">Traficom</a> frequency allocation table</li>"
           "<li>Public <a href=\"http://kiwisdr.com/public/\">KiwiSDR</a> receivers, listed via <a href=\"http://rx.linkfanel.net/\">rx.linkfanel.net</a></li>"
           "</ul><p>Thank you to everyone compiling these lists.</p>"
           "<p>Rig control through <a href=\"https://hamlib.github.io/\">Hamlib</a>'s rigctld. "
           "Web page: <a href=\"https://otd.oh2gba.eu/\">otd.oh2gba.eu</a></p>"
           "<p>Data directory: %2</p>"
           "<p>Licensed under the GNU GPL v3 or later. Built with Qt %3.</p>")
            .arg(QLatin1String(OTD_VERSION), m_session->dataDir().toHtmlEscaped(),
                 QLatin1String(qVersion())));
}

void MainWindow::saveColumns()
{
    m_session->db()->setMeta(QStringLiteral("window.columns"),
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
    menu.addSeparator();
    menu.addAction(tr("Fit columns to the window"), this, &MainWindow::refitColumns);
    menu.exec(h->mapToGlobal(pos));
}

void MainWindow::centreOnMarker()
{
    if (!m_dialActive)
        return;
    // a model reset only schedules the layout; do it now so that the
    // scroll range is right before moving
    m_table->doItemsLayout();
    // just double-clicked: that row stays under the mouse (for a few
    // seconds, long enough for the rig to confirm the frequency)
    if (m_anchorId != 0)
    {
        if (m_anchorClock.elapsed() < 4000
            && qFuzzyCompare(m_session->centreKHz() + 1.0, m_anchorKHz + 1.0) && placeAnchor())
            return;
        m_anchorId = 0;
    }
    // The rows on the tuned frequency, as shown (On air only may hide some
    // of them), go to the middle as one block; without any, the seam
    // between "below the VFO" and "above it" does.
    const int rows = m_proxy->rowCount();
    int seam = rows, first = -1, last = -1;
    for (int r = 0; r < rows; ++r)
    {
        const QModelIndex idx = m_proxy->index(r, 0);
        if (qFuzzyIsNull(idx.data(StationModel::DeltaRole).toDouble())
            && !m_model->isBlank(m_proxy->mapToSource(idx).row()))
        {
            if (first < 0)
                first = r;
            last = r;
        }
        if (seam == rows && idx.data(StationModel::DialSideRole).toInt() > 0)
            seam = r;
    }
    QScrollBar* bar = m_table->verticalScrollBar();
    int y;
    if (first >= 0)
        y = (m_table->rowViewportPosition(first) + m_table->rowViewportPosition(last) + m_table->rowHeight(last)) / 2
            + bar->value();
    else
        y = seam < rows ? m_table->rowViewportPosition(seam) + bar->value() : m_table->verticalHeader()->length();
    bar->setValue(y - m_table->viewport()->height() / 2);
}

bool MainWindow::placeAnchor()
{
    for (int r = 0; r < m_proxy->rowCount(); ++r)
    {
        const int src = m_proxy->mapToSource(m_proxy->index(r, 0)).row();
        if (src < 0 || m_model->isBlank(src) || m_model->entryAt(src).id != m_anchorId)
            continue;
        QScrollBar* bar = m_table->verticalScrollBar();
        bar->setValue(bar->value() + m_table->rowViewportPosition(r) - m_anchorY);
        return true;
    }
    return false;
}

void MainWindow::refillDial()
{
    // The dial view holds a thousand entries either side of the tuned
    // frequency. Scrolled near an end of them, it loads the same window
    // around what is on screen instead, keeping the rows where they are,
    // so the list goes on as far as the database does. The tuning, the
    // seam and the scale stay as they are.
    if (!m_dialActive || m_refilling)
        return;
    const int rows = m_proxy->rowCount();
    int first = -1, last = -1;
    for (int r = 0; r < rows; ++r)
        if (!m_model->isBlank(m_proxy->mapToSource(m_proxy->index(r, 0)).row()))
        {
            if (first < 0)
                first = r;
            last = r;
        }
    if (first < 0)
        return;
    const int height = m_table->viewport()->height();
    int top = m_table->rowAt(0);
    int bottom = m_table->rowAt(height - 1);
    if (top < 0)
        top = 0;
    if (bottom < 0)
        bottom = rows - 1;
    const int margin = 100;
    if (top >= first + margin && bottom <= last - margin)
        return;   // well inside the loaded rows
    // the row in the middle of the screen keeps its place; among the blank
    // rows at an end, the nearest real one does
    int mid = m_table->rowAt(height / 2);
    if (mid < 0 || mid < first)
        mid = first;
    if (mid > last)
        mid = last;
    const StationEntry midEntry = m_model->entryAt(m_proxy->mapToSource(m_proxy->index(mid, 0)).row());
    const int midY = m_table->rowViewportPosition(mid);
    const StationList list = m_session->db()->around(midEntry.kHz, 1000, m_session->disabledSources());
    if (list.isEmpty())
        return;
    if (qFuzzyCompare(list.first().kHz, m_model->entryAt(m_proxy->mapToSource(m_proxy->index(first, 0)).row()).kHz)
        && qFuzzyCompare(list.last().kHz, m_model->entryAt(m_proxy->mapToSource(m_proxy->index(last, 0)).row()).kHz)
        && list.size() == last - first + 1)
        return;   // the database has no more in that direction
    m_refilling = true;
    m_model->setDialEntries(list, m_session->centreKHz());
    m_table->doItemsLayout();
    const int newRows = m_proxy->rowCount();
    for (int r = 0; r < newRows; ++r)
    {
        const int src = m_proxy->mapToSource(m_proxy->index(r, 0)).row();
        if (!m_model->isBlank(src) && m_model->entryAt(src).id == midEntry.id)
        {
            QScrollBar* bar = m_table->verticalScrollBar();
            bar->setValue(bar->value() + m_table->rowViewportPosition(r) - midY);
            break;
        }
    }
    m_refilling = false;
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    QTimer::singleShot(0, this, &MainWindow::centreOnMarker);
}

// The arrow keys. Without a rig they are the tuning knob: Up/Down 1 kHz,
// Page Up/Down 5 kHz, with Ctrl 0.1 kHz. With Shift, Down/Up go to the
// next station down or up the list among the rows shown, rig or no rig:
// the list runs upward in frequency, so Shift+Down is the next station up
// the band, as the eye reads it. Widgets that use the arrows themselves
// (lists, spin boxes) keep them.
bool MainWindow::tuneByKey(QKeyEvent* key)
{
    const double centre = m_session->centreKHz();
    if (centre <= 0.0)
        return false;
    const bool shift = key->modifiers() & Qt::ShiftModifier;
    double step = 0.0;
    switch (key->key())
    {
    case Qt::Key_Up:       step = 1.0;  break;
    case Qt::Key_Down:     step = -1.0; break;
    case Qt::Key_PageUp:   step = 5.0;  break;
    case Qt::Key_PageDown: step = -5.0; break;
    default: return false;
    }
    if (!shift && m_session->followRig())
        return false;   // the knob is for when no rig is followed
    if (shift && (key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown))
        return false;
    if (key->modifiers() & Qt::ControlModifier)
        step /= 10.0;
    if (QApplication::activePopupWidget())
        return false;   // an open popup (a menu, a list) takes the keys
    QWidget* focus = QApplication::focusWidget();
    if (qobject_cast<QComboBox*>(focus) || qobject_cast<QAbstractSpinBox*>(focus)
        || qobject_cast<QAbstractItemView*>(focus) || (focus && qobject_cast<QComboBox*>(focus->parentWidget())))
        return false;
    if (shift)
        tuneNeighbour(step > 0 ? -1 : 1);   // Down the list is up the band
    else
        m_session->setManualKHz(qMax(0.0, centre + step));
    return true;
}

// The next station up (direction 1) or down the band from the tuned
// frequency, among the rows on screen (so "On air only" and a search
// narrow the walk), with its mode: tuned as a double-click on it would.
// On a frequency with several entries the first shown (on air first) leads.
void MainWindow::tuneNeighbour(int direction)
{
    const double centre = m_session->centreKHz();
    if (centre <= 0.0)
        return;
    int best = -1;
    double bestKHz = 0.0;
    const int rows = m_proxy->rowCount();
    for (int pr = 0; pr < rows; ++pr)
    {
        const int r = m_proxy->mapToSource(m_proxy->index(pr, 0)).row();
        if (r < 0 || m_model->isBlank(r))
            continue;
        const double kHz = m_model->entryAt(r).kHz;
        if (direction > 0 ? kHz <= centre + 1e-6 : kHz >= centre - 1e-6)
            continue;
        if (best < 0 || (direction > 0 ? kHz < bestKHz : kHz > bestKHz))
        {
            best = r;
            bestKHz = kHz;
        }
    }
    if (best < 0)
        return;   // the end of what is shown
    const StationEntry e = m_model->entryAt(best);
    tuneTo(e.kHz, StationNames::modeOf(e), 0);   // no blink: that is the mouse's receipt
    // The dial recentres on the new frequency by itself. A search list
    // keeps its order while tuning, so there the table scrolls to the
    // station instead, keeping it in the middle.
    if (m_searching)
        for (int pr = 0; pr < m_proxy->rowCount(); ++pr)
        {
            const QModelIndex idx = m_proxy->index(pr, StationModel::ColStation);
            const int r = m_proxy->mapToSource(idx).row();
            if (r >= 0 && !m_model->isBlank(r) && m_model->entryAt(r).id == e.id)
            {
                m_table->scrollTo(idx, QAbstractItemView::PositionAtCenter);
                break;
            }
        }
}

// Mouse wheel over one digit of the big frequency: that digit goes up or
// down, like the tuning step of a radio display. With a rig followed the
// rig is tuned and the display follows it; without, the display alone.
bool MainWindow::wheelOnFrequency(QWheelEvent* wheel)
{
    const double centre = m_session->centreKHz();
    if (centre <= 0.0)
        return false;
    const int steps = wheel->angleDelta().y() / 120;
    if (steps == 0)
        return false;
    const QString text = m_freqLabel->text();
    const QFontMetrics fm(m_freqLabel->font());
    // the label draws its text at the left edge, with its margin
    const double x = wheel->position().x() - m_freqLabel->contentsRect().left();
    int idx = -1;
    double left = 0.0;
    for (int i = 0; i < text.size(); ++i)
    {
        const double w = fm.horizontalAdvance(text.at(i));
        if (x >= left && x < left + w)
        {
            idx = i;
            break;
        }
        left += w;
    }
    const double step = Format::digitStep(text, idx);
    if (step <= 0.0)
        return false;
    m_session->tuneTo(qMax(0.0, centre + steps * step), QString());
    return true;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_freqLabel && event->type() == QEvent::Wheel
        && wheelOnFrequency(static_cast<QWheelEvent*>(event)))
        return true;
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
        {StationModel::ColDelta,     fm.horizontalAdvance(QStringLiteral("▼ 999.9")) + pad},
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
    m_session->db()->setMeta(QStringLiteral("window.geometry"),
                             QString::fromLatin1(saveGeometry().toBase64()));
    saveColumns();
    AppSettings& s = m_session->settings();
    s.kiwiVolume = m_player->volume();
    s.kiwiReceivers = m_player->receivers();
    s.kiwiFavourites = m_player->favourites();
    s.kiwiCurrent = m_player->currentReceiver();
    m_session->saveSettings();
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
