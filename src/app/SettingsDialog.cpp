// SPDX-License-Identifier: GPL-3.0-or-later
#include "SettingsDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QCoreApplication>
#include <QDir>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLineEdit>
#include "core/RigctldLauncher.h"

#include <QComboBox>
#include <QFileDialog>
#include <QPushButton>
#include <QTabWidget>
#include <QSpinBox>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(const AppSettings& cur, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Settings"));

    auto* rigBox = new QGroupBox(tr("Connection to rigctld (or gqrx / SDR++ rigctl server)"));
    auto* rigForm = new QFormLayout(rigBox);
    m_host = new QLineEdit(cur.rigHost);
    m_port = new QSpinBox;
    m_port->setRange(1, 65535);
    m_port->setValue(cur.rigPort);
    m_poll = new QSpinBox;
    m_poll->setRange(100, 10000);
    m_poll->setSingleStep(100);
    m_poll->setSuffix(tr(" ms"));
    m_poll->setValue(cur.pollIntervalMs);
    rigForm->addRow(tr("Host:"), m_host);
    rigForm->addRow(tr("Port:"), m_port);
    rigForm->addRow(tr("Poll interval:"), m_poll);

    // ---- start rigctld from here ------------------------------------
    auto* launchBox = new QGroupBox(tr("Start rigctld for me"));
    auto* launchForm = new QFormLayout(launchBox);
    m_launch = new QCheckBox(tr("Start Hamlib's rigctld when the program starts"));
    m_launch->setChecked(cur.launchRigctld);
    m_model = new QComboBox;
    m_model->setEditable(false);
    m_model->setMinimumWidth(340);
    m_currentModel = cur.rigModel;
    auto* reload = new QPushButton(tr("Reload list"));
    connect(reload, &QPushButton::clicked, this, &SettingsDialog::reloadModels);
    auto* modelRow = new QHBoxLayout;
    modelRow->addWidget(m_model, 1);
    modelRow->addWidget(reload);
    m_device = new QComboBox;
    m_device->setEditable(true);
    m_device->addItems(RigctldLauncher::serialPortCandidates());
    m_device->setCurrentText(cur.rigDevice);
    m_device->lineEdit()->setPlaceholderText(tr("/dev/ttyUSB0, COM3, or host:port"));
    m_baud = new QComboBox;
    m_baud->addItem(tr("rig default"), 0);
    for (int b : {4800, 9600, 19200, 38400, 57600, 115200})
        m_baud->addItem(QString::number(b), b);
    m_baud->setCurrentIndex(qMax(0, m_baud->findData(cur.rigBaud)));
    m_extra = new QLineEdit(cur.rigctldExtra);
    m_extra->setPlaceholderText(tr("e.g. --set-conf=stop_bits=2 --civaddr=0x94"));
    m_launchNote = new QLabel;
    m_launchNote->setWordWrap(true);
    launchForm->addRow(m_launch);
    launchForm->addRow(tr("Rig model:"), modelRow);
    launchForm->addRow(tr("Device:"), m_device);
    launchForm->addRow(tr("Baud rate:"), m_baud);
    launchForm->addRow(tr("Extra arguments:"), m_extra);
    launchForm->addRow(m_launchNote);
    auto enableLaunch = [this](bool on) {
        for (QWidget* w : std::initializer_list<QWidget*>{m_model, m_device, m_baud, m_extra})
            w->setEnabled(on);
        // a rigctld started by otd always runs on this machine; port and
        // poll interval stay adjustable
        m_host->setEnabled(!on);
        if (on)
            m_host->setText(QStringLiteral("localhost"));
    };
    enableLaunch(cur.launchRigctld);
    connect(m_launch, &QCheckBox::toggled, this, enableLaunch);
    reloadModels();

    auto* viewBox = new QGroupBox(tr("Display"));
    auto* viewForm = new QFormLayout(viewBox);
    m_tolerance = new QDoubleSpinBox;
    m_tolerance->setRange(0.1, 500.0);
    m_tolerance->setDecimals(1);
    m_tolerance->setSuffix(tr(" kHz"));
    m_tolerance->setValue(cur.toleranceKHz);
    viewForm->addRow(tr("Highlight range (±):"), m_tolerance);
    m_region = new QComboBox;
    m_region->addItem(tr("Region 1: Europe, Africa, Middle East, Russia"), 1);
    m_region->addItem(tr("Region 2: the Americas"), 2);
    m_region->addItem(tr("Region 3: Asia and Pacific"), 3);
    m_region->setCurrentIndex(qMax(0, m_region->findData(cur.ituRegion)));
    m_region->setToolTip(tr("Band allocations differ slightly between the three ITU regions"));
    viewForm->addRow(tr("ITU region (band plan):"), m_region);

    // Downloaded data: one line per source, the box and the address in
    // two aligned columns
    auto* dataBox = new QGroupBox(tr("Downloaded data"));
    auto* grid = new QGridLayout(dataBox);
    grid->setColumnStretch(2, 1);
    int row = 0;
    // Each source: the switch, a line about it, and a link to the publisher's
    // page. The download addresses themselves are built in and not editable.
    auto addSource = [&](const QString& label, const QString& note, const QString& site,
                         const QString& siteText, bool enabled, QCheckBox** box) {
        *box = new QCheckBox(label);
        (*box)->setChecked(enabled);
        grid->addWidget(*box, row, 0);
        auto* about = new QLabel(QStringLiteral("%1 &nbsp;<a href=\"%2\">%3</a>")
                                     .arg(note.toHtmlEscaped(), site.toHtmlEscaped(), siteText.toHtmlEscaped()));
        about->setOpenExternalLinks(true);
        about->setTextInteractionFlags(Qt::TextBrowserInteraction);
        grid->addWidget(about, row, 1, 1, 2);
        ++row;
    };
    auto* schedules = new QLabel(tr("Schedules"));
    schedules->setStyleSheet(QStringLiteral("font-weight: bold;"));
    grid->addWidget(schedules, row++, 0, 1, 3);
    addSource(tr("EiBi"), tr("Eike Bierwirth's list."),
              QStringLiteral("http://www.eibispace.de/"), QStringLiteral("eibispace.de"),
              cur.eibiEnabled, &m_eibiOn);
    addSource(tr("HFCC"), tr("HFCC public data."),
              QStringLiteral("http://www.hfcc.org/data/"), QStringLiteral("hfcc.org"),
              cur.hfccEnabled, &m_hfccOn);
    addSource(tr("Aoki"), tr("Bi Newsletter, Nagoya DXers Circle."),
              QStringLiteral("http://www1.s2.starcat.ne.jp/ndxc/"), QStringLiteral("ndxc"),
              cur.aokiEnabled, &m_aokiOn);
    auto* tables = new QLabel(tr("Frequency allocation table"));
    tables->setStyleSheet(QStringLiteral("font-weight: bold;"));
    grid->addWidget(tables, row++, 0, 1, 3);
    addSource(tr("Traficom"),
              tr("Finland's detailed allocation table, replaces the built-in band plan."),
              QStringLiteral("https://avoindata.suomi.fi/data/en_GB/dataset/taajuusjakotaulukko"),
              QStringLiteral("avoindata.suomi.fi"), cur.traficomEnabled, &m_traficomOn);
    m_refreshDays = new QSpinBox;
    m_refreshDays->setRange(1, 90);
    m_refreshDays->setSuffix(tr(" days"));
    m_refreshDays->setValue(cur.refreshDays);
    grid->addWidget(new QLabel(tr("Check for new files when older than:")), row, 0, 1, 2, Qt::AlignRight);
    grid->addWidget(m_refreshDays, row, 2, Qt::AlignLeft);
    ++row;

    auto* programBox = new QGroupBox(tr("Program"));
    auto* programGrid = new QGridLayout(programBox);
    programGrid->setColumnStretch(1, 1);
    m_updateOn = new QCheckBox(tr("Check otd.oh2gba.eu for a new version"));
    m_updateOn->setChecked(cur.updateCheck);
    m_updateOn->setToolTip(tr("Once a day; only the program's version and platform are sent."));
    programGrid->addWidget(m_updateOn, 0, 0, 1, 2);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* rigTab = new QWidget;
    auto* rigLayout = new QVBoxLayout(rigTab);
    rigLayout->addWidget(rigBox);
    rigLayout->addWidget(launchBox);
    rigLayout->addStretch();
    auto* viewTab = new QWidget;
    auto* viewLayout = new QVBoxLayout(viewTab);
    viewLayout->addWidget(viewBox);
    viewLayout->addStretch();
    auto* dataTab = new QWidget;
    auto* dataLayout = new QVBoxLayout(dataTab);
    dataLayout->addWidget(dataBox);
    dataLayout->addWidget(programBox);
    dataLayout->addStretch();
    auto* tabs = new QTabWidget;
    tabs->addTab(rigTab, tr("Radio"));
    tabs->addTab(viewTab, tr("Display"));
    tabs->addTab(dataTab, tr("Data"));

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
}

void SettingsDialog::reloadModels()
{
    QString error;
    const QVector<RigctldLauncher::Model> models =
        RigctldLauncher::listModels(QString(), &error);
    m_model->clear();
    int selected = -1;
    for (const RigctldLauncher::Model& m : models)
    {
        m_model->addItem(m.label(), m.number);
        if (m.number == m_currentModel)
            selected = m_model->count() - 1;
    }
    if (models.isEmpty())
    {
        m_model->addItem(tr("%1  (rigctld not found, keeping model number)").arg(m_currentModel),
                         m_currentModel);
        // the packages bring rigctld along; a build from source uses the
        // distribution's Hamlib, which has to be there
        m_launchNote->setText(tr("rigctld not found (%1). The downloads include it; a build from "
                                 "source needs Hamlib installed, so that rigctld is on the PATH.")
                                 .arg(error));
    }
    else
    {
        const QString path = RigctldLauncher::defaultPath();
        const bool bundled = path.startsWith(QCoreApplication::applicationDirPath());
        m_launchNote->setText(tr("%1 rig models known to %2 (%3). Pick your radio, the port it is "
                                 "connected to and, if needed, the baud rate set in the radio's menu.")
                                 .arg(models.size())
                                 .arg(bundled ? tr("the included rigctld") : tr("the system's rigctld"),
                                      QDir::toNativeSeparators(path)));
        if (selected < 0)
            selected = m_model->findData(1);
    }
    m_model->setCurrentIndex(qMax(0, selected));
}


AppSettings SettingsDialog::settings(const AppSettings& base) const
{
    AppSettings s = base;
    s.rigHost = (m_launch->isChecked() || m_host->text().trimmed().isEmpty())
                    ? QStringLiteral("localhost")
                    : m_host->text().trimmed();
    s.rigPort = m_port->value();
    s.pollIntervalMs = m_poll->value();
    s.toleranceKHz = m_tolerance->value();
    s.ituRegion = m_region->currentData().toInt();
    s.updateCheck = m_updateOn->isChecked();

    s.refreshDays = m_refreshDays->value();

    s.eibiEnabled = m_eibiOn->isChecked();
    s.hfccEnabled = m_hfccOn->isChecked();
    s.aokiEnabled = m_aokiOn->isChecked();

    s.traficomEnabled = m_traficomOn->isChecked();
    s.launchRigctld = m_launch->isChecked();
    s.rigctldPath.clear();   // the bundled rigctld, or the one on the PATH, is always used
    s.rigModel = m_model->currentData().toInt();
    s.rigDevice = m_device->currentText().trimmed();
    s.rigBaud = m_baud->currentData().toInt();
    s.rigctldExtra = m_extra->text().trimmed();
    return s;
}
