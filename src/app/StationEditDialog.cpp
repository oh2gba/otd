// SPDX-License-Identifier: GPL-3.0-or-later
#include "StationEditDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimeEdit>

StationEditDialog::StationEditDialog(const StationEntry& entry, QWidget* parent)
    : QDialog(parent)
    , m_entry(entry)
{
    setWindowTitle(entry.id > 0 ? tr("Edit my station") : tr("Add to my stations"));

    m_station = new QLineEdit(entry.station);
    m_station->setPlaceholderText(tr("What you heard, e.g. \"Buzzer-like tone, 3 pips\""));
    m_kHz = new QDoubleSpinBox;
    m_kHz->setRange(0.001, 100000.0);
    m_kHz->setDecimals(3);
    m_kHz->setSuffix(tr(" kHz"));
    m_kHz->setValue(entry.kHz > 0 ? entry.kHz : 1000.0);
    m_mode = new QComboBox;
    m_mode->setEditable(true);
    m_mode->addItems({QStringLiteral("AM"), QStringLiteral("USB"), QStringLiteral("LSB"), QStringLiteral("CW"),
                      QStringLiteral("DRM"), QStringLiteral("RTTY"), QStringLiteral("FAX"), QStringLiteral("HFDL"),
                      QStringLiteral("ALE"), QStringLiteral("STANAG"), QStringLiteral("OTH"), QString()});
    m_mode->setCurrentText(entry.mode);

    const bool allDay = entry.startMin == 0 && entry.endMin >= 1440;
    m_allDay = new QCheckBox(tr("all day"));
    m_allDay->setChecked(allDay || entry.id == 0);
    m_start = new QTimeEdit(QTime(entry.startMin / 60, entry.startMin % 60));
    m_end = new QTimeEdit(QTime((entry.endMin % 1440) / 60, entry.endMin % 60));
    for (QTimeEdit* t : {m_start, m_end})
    {
        t->setDisplayFormat(QStringLiteral("HH:mm"));
        t->setEnabled(!m_allDay->isChecked());
    }
    connect(m_allDay, &QCheckBox::toggled, this, [this](bool on) {
        m_start->setEnabled(!on);
        m_end->setEnabled(!on);
    });
    auto* timeRow = new QHBoxLayout;
    timeRow->addWidget(m_allDay);
    timeRow->addWidget(m_start);
    timeRow->addWidget(new QLabel(tr("to")));
    timeRow->addWidget(m_end);
    timeRow->addWidget(new QLabel(tr("UTC")));
    timeRow->addStretch();

    m_days = new QLineEdit(entry.days);
    m_days->setPlaceholderText(tr("empty = daily; Mo-Fr, SaSu, 1245, irr, 1.Sa ..."));
    m_country = new QLineEdit(entry.itu);
    m_country->setPlaceholderText(tr("ITU code, e.g. RUS, G, USA"));
    m_country->setMaxLength(3);
    m_language = new QLineEdit(entry.lang);
    m_site = new QLineEdit(entry.site);
    m_notes = new QPlainTextEdit(entry.remarks);
    m_notes->setPlaceholderText(tr("Sound, callsign, what it might be, links ..."));
    m_notes->setMaximumHeight(90);

    auto* form = new QFormLayout;
    form->addRow(tr("Station / description:"), m_station);
    form->addRow(tr("Frequency:"), m_kHz);
    form->addRow(tr("Mode:"), m_mode);
    form->addRow(tr("Time:"), timeRow);
    form->addRow(tr("Days:"), m_days);
    form->addRow(tr("Country:"), m_country);
    form->addRow(tr("Language:"), m_language);
    form->addRow(tr("Transmitter / site:"), m_site);
    form->addRow(tr("Notes:"), m_notes);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        if (m_station->text().trimmed().isEmpty())
        {
            m_station->setFocus();
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
    m_station->setFocus();
}

StationEntry StationEditDialog::entry() const
{
    StationEntry e = m_entry;
    e.source = userSourceId();
    e.persistence = 1;
    e.station = m_station->text().trimmed();
    e.kHz = m_kHz->value();
    e.mode = m_mode->currentText().trimmed().toUpper();
    if (m_allDay->isChecked())
    {
        e.startMin = 0;
        e.endMin = 1440;
    }
    else
    {
        e.startMin = m_start->time().hour() * 60 + m_start->time().minute();
        e.endMin = m_end->time().hour() * 60 + m_end->time().minute();
    }
    e.days = m_days->text().trimmed();
    e.itu = m_country->text().trimmed().toUpper();
    e.lang = m_language->text().trimmed();
    e.site = m_site->text().trimmed();
    e.remarks = m_notes->toPlainText().trimmed();
    e.lang.clear();
    e.site.clear();
    e.target.clear();
    return e;
}
