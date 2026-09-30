// SPDX-License-Identifier: GPL-3.0-or-later
#include "MyStationsDialog.h"
#include "StationEditDialog.h"
#include "core/StationModel.h"
#include "core/StationDb.h"
#include "core/UserList.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

MyStationsDialog::MyStationsDialog(StationDb* db, double currentKHz, const QString& currentMode,
                                   QWidget* parent)
    : QDialog(parent)
    , m_db(db)
    , m_currentKHz(currentKHz)
    , m_currentMode(currentMode)
{
    setWindowTitle(tr("My stations"));
    resize(900, 420);

    m_model = new StationModel(db, this);
    m_table = new QTableView;
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->setAlternatingRowColors(true);
    for (int c : {StationModel::ColDelta, StationModel::ColLanguage, StationModel::ColTarget,
                  StationModel::ColLastHeard, StationModel::ColSource})
        m_table->setColumnHidden(c, true);
    m_table->horizontalHeader()->setSectionResizeMode(StationModel::ColStation, QHeaderView::Stretch);
    connect(m_table, &QTableView::doubleClicked, this, &MyStationsDialog::editEntry);

    auto* add = new QPushButton(tr("Add..."));
    auto* edit = new QPushButton(tr("Edit..."));
    auto* del = new QPushButton(tr("Delete"));
    auto* imp = new QPushButton(tr("Import..."));
    auto* exp = new QPushButton(tr("Export..."));
    connect(add, &QPushButton::clicked, this, &MyStationsDialog::addEntry);
    connect(edit, &QPushButton::clicked, this, &MyStationsDialog::editEntry);
    connect(del, &QPushButton::clicked, this, &MyStationsDialog::removeEntry);
    connect(imp, &QPushButton::clicked, this, &MyStationsDialog::importCsv);
    connect(exp, &QPushButton::clicked, this, &MyStationsDialog::exportCsv);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    for (QPushButton* b : {add, edit, del, imp, exp})
        buttons->addButton(b, QDialogButtonBox::ActionRole);

    auto* note = new QLabel(tr("Your own identifications. They show up in the station list like the "
                               "downloaded ones, with source \"Mine\", and are kept in the same database file."));
    note->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note);
    layout->addWidget(m_table, 1);
    layout->addWidget(buttons);
    reload();
}

void MyStationsDialog::reload()
{
    m_model->setEntries(m_db->entriesOf(userSourceId()), 0.0);
}

int MyStationsDialog::selectedRow() const
{
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    return rows.isEmpty() ? -1 : rows.first().row();
}

void MyStationsDialog::addEntry()
{
    StationEntry e;
    e.kHz = m_currentKHz;
    e.mode = m_currentMode;
    StationEditDialog dlg(e, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    StationEntry fresh = dlg.entry();
    if (!m_db->insertEntry(fresh))
        QMessageBox::warning(this, tr("My stations"), m_db->lastError());
    reload();
}

void MyStationsDialog::editEntry()
{
    const int row = selectedRow();
    if (row < 0)
        return;
    StationEditDialog dlg(m_model->entryAt(row), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (!m_db->updateEntry(dlg.entry()))
        QMessageBox::warning(this, tr("My stations"), m_db->lastError());
    reload();
}

void MyStationsDialog::removeEntry()
{
    const int row = selectedRow();
    if (row < 0)
        return;
    const StationEntry& e = m_model->entryAt(row);
    if (QMessageBox::question(this, tr("Delete"), tr("Delete \"%1\" on %2 kHz?").arg(e.station).arg(e.kHz))
        != QMessageBox::Yes)
        return;
    m_db->removeEntry(e.id);
    reload();
}

void MyStationsDialog::importCsv()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Import my stations"), QString(),
                                                      tr("Station lists (*.csv *.txt);;All files (*)"));
    if (file.isEmpty())
        return;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
    {
        QMessageBox::warning(this, tr("Import"), f.errorString());
        return;
    }
    int skipped = 0;
    StationList entries = UserList::fromCsv(f.readAll(), &skipped);
    int added = 0;
    for (StationEntry& e : entries)
        if (m_db->insertEntry(e))
            ++added;
    reload();
    QMessageBox::information(this, tr("Import"),
                             tr("%1 entries imported, %2 lines skipped.").arg(added).arg(skipped));
}

void MyStationsDialog::exportCsv()
{
    const QString file = QFileDialog::getSaveFileName(this, tr("Export my stations"),
                                                      QStringLiteral("my-stations.csv"),
                                                      tr("Station lists (*.csv)"));
    if (file.isEmpty())
        return;
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        QMessageBox::warning(this, tr("Export"), f.errorString());
        return;
    }
    f.write(UserList::toCsv(m_db->entriesOf(userSourceId())));
}
