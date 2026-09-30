// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/AppSettings.h"
#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QLabel;

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(const AppSettings& current, QWidget* parent = nullptr);
    // Returns a copy of the given settings with the dialog fields applied.
    AppSettings settings(const AppSettings& base) const;

private:
    QLineEdit* m_host;
    QSpinBox* m_port;
    QSpinBox* m_poll;
    QDoubleSpinBox* m_tolerance;
    QComboBox* m_region;
    QCheckBox* m_updateOn;
    QSpinBox* m_refreshDays;
    QCheckBox* m_eibiOn;
    QCheckBox* m_hfccOn;
    QCheckBox* m_aokiOn;
    QCheckBox* m_traficomOn;

    QCheckBox* m_launch;
    QComboBox* m_model;
    QComboBox* m_device;
    QComboBox* m_baud;
    QLineEdit* m_extra;
    QLabel* m_launchNote;
    int m_currentModel = 1;

private slots:
    void reloadModels();
};
