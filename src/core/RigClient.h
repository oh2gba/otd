// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QQueue>
#include <QTcpSocket>
#include <QTimer>

// Polls a rigctl-protocol server (Hamlib rigctld on port 4532, gqrx, SDR++)
// for VFO frequency and mode. Reconnects automatically.
class RigClient : public QObject
{
    Q_OBJECT
public:
    explicit RigClient(QObject* parent = nullptr);
    ~RigClient() override;

    void setEndpoint(const QString& host, quint16 port);
    void setPollInterval(int ms);

    QString host() const { return m_host; }
    quint16 port() const { return m_port; }
    bool isConnected() const { return m_socket.state() == QAbstractSocket::ConnectedState; }
    // True while the rig actually answers frequency polls. A connection to
    // rigctld alone does not count: with the radio switched off rigctld
    // accepts the connection but answers every poll with an error report.
    bool isAnswering() const { return m_answer == Answer::Yes; }
    // How long without a valid frequency answer before the rig counts as
    // silent; 0 (the default) means three poll intervals, at least 5 s.
    void setSilenceTimeout(int ms);
    qint64 frequencyHz() const { return m_frequencyHz; }
    QString mode() const { return m_mode; }
    int passbandHz() const { return m_passband; }

public slots:
    void start();
    void stop();
    // Tune the rig ("F"/"M" commands); no-ops when disconnected.
    void setFrequency(qint64 hz);
    void setMode(const QString& mode);
    // Drop the connection and retry shortly (e.g. after rigctld was started).
    void reconnectSoon();

signals:
    // Only when the value changes; after a silence the next answer is
    // reported again even if the frequency is the same.
    void frequencyChanged(qint64 hz);
    // The rig started answering (true), or has been silent for the silence
    // timeout (false). Emitted on changes only, and once for a rig that has
    // never answered since start(). A new endpoint that stays silent is
    // reported again, even when the old one was silent too.
    void answeringChanged(bool answering);
    void modeChanged(const QString& mode, int passbandHz);
    void stateChanged(bool connected, const QString& message);

private slots:
    void connectNow();
    void onConnected();
    void onDisconnected();
    void onError(QAbstractSocket::SocketError error);
    void onReadyRead();
    void poll();

private:
    void handleLine(const QByteArray& line);
    void scheduleReconnect();
    void onSilence();
    void armSilenceTimer();

    enum class Answer { Unknown, Yes, No };
    Answer m_answer = Answer::Unknown;
    QTimer m_silenceTimer;
    int m_silenceTimeoutMs = 0;

    enum Expect { ExFreq, ExMode, ExPassband, ExReport };
    QQueue<Expect> m_expect;   // answers still owed by the server, in order

    QTcpSocket m_socket;
    QTimer m_pollTimer;
    QTimer m_reconnectTimer;
    QByteArray m_buffer;
    QString m_host = QStringLiteral("localhost");
    quint16 m_port = 4532;
    bool m_enabled = false;
    int m_missedPolls = 0;
    qint64 m_frequencyHz = 0;
    QString m_mode;
    int m_passband = 0;
};
