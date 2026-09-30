// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/KiwiDirectory.h"
#include <QWidget>

class KiwiAudio;
class SMeter;
class QComboBox;
class QLabel;
class QLineEdit;
class QSlider;
class QToolButton;

// One row of controls: which KiwiSDR to listen to, play/stop, volume and
// a signal readout. Follows the frequency and mode it is given. The
// listening itself is KiwiAudio's.
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
    bool isPlaying() const;
    // seconds of audio waiting to be heard; 0 when not playing
    double audioInHand() const;

    // rigMode empty: no rig is followed, the user's own mode choice applies
    void tune(double kHz, const QString& rigMode);
    QString manualMode() const { return m_manualMode; }   // the listener's own choice
    void setManualMode(const QString& mode);
    void stop();

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
    void rememberCurrent();
    void rebuildList();
    void updateStar();
    QString pickReceiver() const;   // for a fresh install: a random free one
    static QString labelFor(const KiwiDirectory::Receiver& r);
    static QIcon starIcon(bool on);
    static QIcon globeIcon();

    KiwiAudio* m_audio = nullptr;
    QStringList m_custom;
    QList<KiwiDirectory::Receiver> m_directory;
    QString m_current;   // address
    QStringList m_favourites;   // addresses marked with the star
    QComboBox* m_receiver = nullptr;
    QLineEdit* m_search = nullptr;
    QToolButton* m_play = nullptr;
    QToolButton* m_add = nullptr;
    QToolButton* m_star = nullptr;
    QToolButton* m_open = nullptr;   // the receiver's own page, in the browser
    QComboBox* m_modeBox = nullptr;
    QSlider* m_volume = nullptr;
    SMeter* m_meter = nullptr;
    bool m_rigDriven = false;
    double m_kHz = 0.0;
    QString m_mode = QStringLiteral("AM");         // what the receiver demodulates now
    QString m_manualMode = QStringLiteral("AM");   // the listener's own choice, for when no rig leads
};
