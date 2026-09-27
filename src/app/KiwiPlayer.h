// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/KiwiClient.h"
#include "core/KiwiDirectory.h"

#include <QWidget>

class PcmQueue;
class SMeter;
class QAudioSink;
class QComboBox;
class QIODevice;
class QLabel;
class QLineEdit;
class QSlider;
class QToolButton;

// One row of controls: which KiwiSDR to listen to, play/stop, volume and
// a signal readout. Follows the frequency and mode it is given.
class KiwiPlayer : public QWidget
{
    Q_OBJECT
public:
    explicit KiwiPlayer(QWidget* parent = nullptr);
    ~KiwiPlayer() override;

    // the user's own entries (plain addresses) and the public directory
    QStringList receivers() const;
    QStringList favourites() const { return m_favourites; }
    void setReceivers(const QStringList& custom, const QStringList& favourites, const QString& current);
    void setDirectory(const QList<KiwiDirectory::Receiver>& list);
    QString currentReceiver() const;   // the address behind the current choice
    int volume() const;            // 0..100
    void setVolume(int percent);

    bool isPlaying() const { return m_client.isOpen(); }
    // rigMode empty: no rig is followed, the user's own mode choice applies
    void tune(double kHz, const QString& rigMode);
    QString manualMode() const;
    void setManualMode(const QString& mode);
    void stop();

signals:
    void receiversChanged();
    void statusChanged(const QString& text);   // what the receiver is doing, empty when idle
    void manualModeChanged(const QString& mode);

private:
    void togglePlay();
    void onAudio(const QByteArray& pcm);
    void onClosed(const QString& reason);
    void setStatus(const QString& text);
    void rememberCurrent();
    void rebuildList();
    static QString labelFor(const KiwiDirectory::Receiver& r);

    QStringList m_custom;
    QList<KiwiDirectory::Receiver> m_directory;
    QString m_current;   // address

    KiwiClient m_client;
    QComboBox* m_receiver = nullptr;
    QToolButton* m_play = nullptr;
    QToolButton* m_add = nullptr;
    QComboBox* m_modeBox = nullptr;
    bool m_rigDriven = false;
    QToolButton* m_star = nullptr;
    QStringList m_favourites;   // addresses marked with the star
    void updateStar();
    static QIcon starIcon(bool on);
    QLineEdit* m_search = nullptr;
    QSlider* m_volume = nullptr;
    SMeter* m_meter = nullptr;
    QString m_statusText;
    QAudioSink* m_sink = nullptr;
    PcmQueue* m_queue = nullptr;
    int m_sinkRate = 0;
    double m_kHz = 0.0;
    QString m_mode = QStringLiteral("AM");
};
