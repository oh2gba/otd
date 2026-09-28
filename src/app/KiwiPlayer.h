// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/KiwiClient.h"
#include "core/KiwiDirectory.h"

#include <QAudio>
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
    QString manualMode() const { return m_manualMode; }   // the listener's own choice
    void setManualMode(const QString& mode);
    void stop();

    // What the sound device reports; a failure ends the session. Public for
    // the unit test, which has no sound device that could fail.
    void onSinkState(QAudio::State state, QAudio::Error error);

signals:
    void receiversChanged();
    void statusChanged(const QString& text);   // what the receiver is doing, empty when idle
    void manualModeChanged(const QString& mode);   // only when the listener picks one
    void playRequested();   // the user started a receiver

private:
    void togglePlay();
    void onAudio(const QByteArray& pcm);
    void onClosed(const QString& reason);
    void stopWith(const QString& status);
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
    int m_sessionsEnded = 0;   // a late failure report for an ended session is dropped
    double m_kHz = 0.0;
    QString m_mode = QStringLiteral("AM");         // what the receiver demodulates now
    QString m_manualMode = QStringLiteral("AM");   // the listener's own choice, for when no rig leads
};
