// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/KiwiClient.h"
#include "core/KiwiDirectory.h"

#include <QWidget>

class PcmQueue;
class QAudioSink;
class QComboBox;
class QIODevice;
class QLabel;
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
    void setReceivers(const QStringList& custom, const QString& current);
    void setDirectory(const QList<KiwiDirectory::Receiver>& list);
    QString currentReceiver() const;   // the address behind the current choice
    int volume() const;            // 0..100
    void setVolume(int percent);

    bool isPlaying() const { return m_client.isOpen(); }
    void tune(double kHz, const QString& mode);
    void stop();

signals:
    void receiversChanged();

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
    QSlider* m_volume = nullptr;
    QLabel* m_meter = nullptr;
    QLabel* m_status = nullptr;
    QAudioSink* m_sink = nullptr;
    PcmQueue* m_queue = nullptr;
    int m_sinkRate = 0;
    double m_kHz = 0.0;
    QString m_mode = QStringLiteral("AM");
};
