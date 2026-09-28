// SPDX-License-Identifier: GPL-3.0-or-later
// Manual check of the KiwiSDR client without a sound card:
//   kiwi_probe http://receiver:8073 [kHz] [mode] [seconds] [play]
// Prints the status lines, the signal level and how much audio arrived;
// with "play" the audio also goes to the default sound output.
#include "core/KiwiClient.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QCoreApplication>
#include <QIODevice>
#include <QMediaDevices>
#include <QTimer>

#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: kiwi_probe http://receiver:8073 [kHz] [mode] [seconds]\n");
        return 2;
    }
    const double kHz = argc > 2 ? QString::fromLocal8Bit(argv[2]).toDouble() : 9500.0;
    const QString mode = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("AM");
    const int seconds = argc > 4 ? QString::fromLocal8Bit(argv[4]).toInt() : 10;
    const bool play = argc > 5 && QString::fromLocal8Bit(argv[5]) == QLatin1String("play");
    QAudioSink* sink = nullptr;
    QIODevice* out = nullptr;

    KiwiClient client;
    qint64 bytes = 0;
    int frames = 0;
    double lastDbm = 0.0;
    QObject::connect(&client, &KiwiClient::stateChanged, [](const QString& s) {
        std::printf("state: %s\n", qPrintable(s));
        std::fflush(stdout);
    });
    QObject::connect(&client, &KiwiClient::closed, [&](const QString& r) {
        std::printf("closed: %s\n", r.isEmpty() ? "(requested)" : qPrintable(r));
        app.exit(r.isEmpty() ? 0 : 1);
    });
    QObject::connect(&client, &KiwiClient::audio, [&](const QByteArray& pcm) {
        bytes += pcm.size();
        ++frames;
        if (play && !sink)
        {
            QAudioFormat fmt;
            fmt.setSampleRate(client.sampleRate());
            fmt.setChannelCount(1);
            fmt.setSampleFormat(QAudioFormat::Int16);
            const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
            std::printf("audio device: %s, format supported: %s\n", qPrintable(dev.description()),
                        dev.isFormatSupported(fmt) ? "yes" : "no");
            sink = new QAudioSink(dev, fmt);
            sink->setBufferSize(client.sampleRate());   // half a second of 16-bit mono
            QObject::connect(sink, &QAudioSink::stateChanged, [&](QAudio::State st) {
                std::printf("sink state %d error %d\n", int(st), int(sink->error()));
                std::fflush(stdout);
            });
            out = sink->start();
            std::printf("sink started: %s, buffer %d bytes\n", out ? "yes" : "no", sink->bufferSize());
        }
        if (out && sink->bytesFree() >= pcm.size())
            out->write(pcm);
    });
    QObject::connect(&client, &KiwiClient::sMeter, [&](double d) { lastDbm = d; });
    client.tune(kHz, mode);
    client.open(QString::fromLocal8Bit(argv[1]));
    QTimer::singleShot(seconds * 1000, [&]() {
        std::printf("audio: %d frames, %lld bytes = %.1f s at %d Hz, last S-meter %.0f dBm\n", frames,
                    static_cast<long long>(bytes), bytes / 2.0 / client.sampleRate(), client.sampleRate(),
                    lastDbm);
        client.close();
        QTimer::singleShot(500, &app, [&]() { app.exit(bytes > 0 ? 0 : 1); });
    });
    return app.exec();
}
