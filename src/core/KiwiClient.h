// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractSocket>
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>

class QNetworkAccessManager;
class QWebSocket;

// Audio client for a KiwiSDR receiver. Speaks the receiver's own
// WebSocket protocol, the same one its browser page and the KiwiSDR
// project's kiwiclient use: text commands in, "MSG" status lines and
// "SND" audio frames (IMA ADPCM) out. The receiver demodulates; this
// class only asks for a frequency and a mode and hands on 16-bit PCM.
//
// Each open() starts a fresh session on a fresh socket, so nothing a
// previous connection still reports can reach the new one. A session ends
// either by close(), which emits nothing, or otherwise (the receiver, the
// network, no answer in time, an unusable address), which is reported
// exactly once through closed() with the reason.
class KiwiClient : public QObject
{
    Q_OBJECT
public:
    explicit KiwiClient(QObject* parent = nullptr);
    ~KiwiClient() override;

    // receiver is the address as listed on kiwisdr.com, e.g.
    // http://example.ddns.net:8073, or as typed: host:port, IP:port, with
    // or without http:// or https://. An address that cannot be used ends
    // the session like any other failure, through closed(), after open()
    // has returned.
    void open(const QString& receiver, const QString& password = QString());
    void close();
    bool isOpen() const { return m_open; }
    bool isReady() const { return m_ready; }
    QUrl receiver() const { return m_receiver; }   // with a scheme and a host

    // How long a session may take to get going (the receiver's greeting)
    // before it is given up; 30 s unless a test sets it shorter.
    void setConnectTimeout(int ms);

    // mode as otd shows it: AM, USB, LSB, CW, NFM; anything else is AM
    void tune(double kHz, const QString& mode);
    int sampleRate() const { return m_rate; }

    // The WebSocket address for a receiver address. The port is the one
    // given; without one it is 8073, the KiwiSDR default (443 for https).
    static QUrl webSocketUrl(const QString& receiver, qint64 stamp);
    // The receiver address as a URL with a scheme; its host is empty when
    // the text is no usable address.
    static QUrl receiverUrl(const QString& receiver);

    // IMA ADPCM decoder state, exposed for the unit test
    struct Adpcm
    {
        int index = 0;
        int prev = 0;
        QByteArray decode(const QByteArray& nibbles);   // -> little-endian int16
    };

signals:
    void stateChanged(const QString& text);   // short human readable status
    void audio(const QByteArray& pcm16le);    // mono, sampleRate()
    void sMeter(double dBm);
    void closed(const QString& reason);       // the session ended without close(); never empty

private:
    void onConnected();
    void onDisconnected();
    void onSocketError(QAbstractSocket::SocketError error);
    void onText(const QString& text);
    void onBinary(const QByteArray& frame);
    void handleMsg(const QString& body);
    void handleSnd(const QByteArray& body);
    void send(const QString& command);
    void applyTuning();
    // end the session and report why, once
    void fail(const QString& reason);
    // detach and dispose of the current socket; nothing it still does is heard
    void dropSocket();
    // a fresh socket to this address, for open() and after a redirect
    void connectSocket(const QUrl& wsUrl);
    // the handshake was answered with a redirect (the proxy.kiwisdr.com
    // hosts do that): ask where to and connect there
    void followRedirect();
    // fetch the receiver's page and two of its icons, as a visitor's
    // browser does, so that the receiver counts otd as a listener
    void visitPage();
    // why the connection ended, as far as anyone said; empty for a plain hang-up
    QString endReason() const;

    QPointer<QWebSocket> m_ws;
    QTimer m_keepalive;
    QTimer m_connectTimer;   // gives up on a receiver that does not answer
    int m_session = 0;       // counts open() calls, to tell sessions apart
    QUrl m_receiver;
    QUrl m_wsUrl;              // the address the socket was opened at
    int m_redirects = 0;       // followed in this session
    QNetworkAccessManager* m_nam = nullptr;   // only for asking where a redirect goes
    QString m_password;
    bool m_open = false;
    bool m_ready = false;
    int m_rate = 12000;
    double m_kHz = 0.0;
    QString m_mode = QStringLiteral("AM");
    double m_maxKHz = 30000.0;
    double m_offsetKHz = 0.0;
    Adpcm m_adpcm;
    QString m_pendingReason;   // said by the receiver before it hangs up
    bool m_outOfRange = false; // the last tuning was outside the receiver's range
    quint64 m_sndFrames = 0;   // for the log
};
