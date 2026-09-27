// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

// Audio client for a KiwiSDR receiver. Speaks the receiver's own
// WebSocket protocol, the same one its browser page and the KiwiSDR
// project's kiwiclient use: text commands in, "MSG" status lines and
// "SND" audio frames (IMA ADPCM) out. The receiver demodulates; this
// class only asks for a frequency and a mode and hands on 16-bit PCM.
class KiwiClient : public QObject
{
    Q_OBJECT
public:
    explicit KiwiClient(QObject* parent = nullptr);

    // receiver is the address as listed on kiwisdr.com, e.g.
    // http://example.ddns.net:8073 (a plain host:port works too)
    void open(const QUrl& receiver, const QString& password = QString());
    void close();
    bool isOpen() const { return m_open; }
    bool isReady() const { return m_ready; }
    QUrl receiver() const { return m_receiver; }

    // mode as otd shows it: AM, USB, LSB, CW, NFM; anything else is AM
    void tune(double kHz, const QString& mode);
    int sampleRate() const { return m_rate; }

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
    void closed(const QString& reason);       // empty reason = closed on request

private:
    void onConnected();
    void onDisconnected();
    void onText(const QString& text);
    void onBinary(const QByteArray& frame);
    void handleMsg(const QString& body);
    void handleSnd(const QByteArray& body);
    void send(const QString& command);
    void applyTuning();

    QWebSocket m_ws;
    QTimer m_keepalive;
    QUrl m_receiver;
    QString m_password;
    bool m_open = false;
    bool m_ready = false;
    int m_rate = 12000;
    double m_kHz = 0.0;
    QString m_mode = QStringLiteral("AM");
    double m_maxKHz = 30000.0;
    double m_offsetKHz = 0.0;
    Adpcm m_adpcm;
    QString m_closeReason;
};
