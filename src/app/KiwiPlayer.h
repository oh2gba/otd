// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/KiwiClient.h"
#include "core/KiwiDirectory.h"
#include "core/PcmResampler.h"

#include <QAudio>
#include <QWidget>

#include <memory>

class PcmQueue;
class SMeter;
class QAudioSink;
class QComboBox;
class QIODevice;
class QLabel;
class QLineEdit;
class QSlider;
class QTimer;
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
    // seconds of audio waiting to be heard (otd's queue and the sound
    // device's buffer); 0 when not playing
    double audioInHand() const;
    // rigMode empty: no rig is followed, the user's own mode choice applies
    void tune(double kHz, const QString& rigMode);
    QString manualMode() const { return m_manualMode; }   // the listener's own choice
    void setManualMode(const QString& mode);
    void stop();

    // What the sound device reports; a failure ends the session. Public for
    // the unit test, which has no sound device that could fail.
    void onSinkState(QAudio::State state, QAudio::Error error);

private:
    // opens the sink and the queue in this format; false (and nothing
    // left behind) when the device refuses it
    bool openSink(const class QAudioDevice& dev, const class QAudioFormat& fmt);

signals:
    void receiversChanged();
    void statusChanged(const QString& text);   // what the receiver is doing, empty when idle
    void manualModeChanged(const QString& mode);   // only when the listener picks one
    void playRequested();   // the user started a receiver
    // seconds of audio in hand, a few times a second while playing, and
    // 0 once playing has ended (isPlaying() tells the two apart)
    void audioInHandChanged(double seconds);

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
    QToolButton* m_open = nullptr;   // the receiver's own page, in the browser
    QStringList m_favourites;   // addresses marked with the star
    void updateStar();
    static QIcon starIcon(bool on);
    static QIcon globeIcon();
    QLineEdit* m_search = nullptr;
    QSlider* m_volume = nullptr;
    SMeter* m_meter = nullptr;
    QTimer* m_bufferTick = nullptr;
    int m_sinkBytesPerSecond = 0;
    QString m_statusText;
    QAudioSink* m_sink = nullptr;
    PcmQueue* m_queue = nullptr;
    std::unique_ptr<PcmResampler> m_resampler;   // when the device takes only its own format
    bool m_opening = false;                      // inside QAudioSink::start()
    quint64 m_audioFrames = 0;                   // for the log
    int m_sinkRate = 0;
    int m_sessionsEnded = 0;   // a late failure report for an ended session is dropped
    double m_kHz = 0.0;
    QString m_mode = QStringLiteral("AM");         // what the receiver demodulates now
    QString m_manualMode = QStringLiteral("AM");   // the listener's own choice, for when no rig leads
};
