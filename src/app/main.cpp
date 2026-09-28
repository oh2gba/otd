// SPDX-License-Identifier: GPL-3.0-or-later
#include "MainWindow.h"
#include "core/Logging.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QPalette>
#include <QStandardPaths>
#include <QStyle>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    // the organisation name is part of the data directory of installed
    // copies (e.g. AppData\onthedial\otd), so it keeps the old spelling
    QCoreApplication::setOrganizationName(QStringLiteral("onthedial"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("otd.oh2gba.eu"));
    QCoreApplication::setApplicationName(QStringLiteral("otd"));
    QApplication::setApplicationDisplayName(QStringLiteral("On The Dial"));
    QCoreApplication::setApplicationVersion(QStringLiteral(OTD_VERSION));
    // the installed desktop entry (otd, or eu.oh2gba.otd in the Flatpak)
    QApplication::setDesktopFileName(QStringLiteral(OTD_DESKTOP_ID));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Shortwave station identifier following your rig via Hamlib rigctld"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption dataDirOpt(
        {QStringLiteral("d"), QStringLiteral("data-dir")},
        QStringLiteral("Keep settings and the station database in <dir> instead of the "
                       "user's application data directory (portable mode)."),
        QStringLiteral("dir"));
    parser.addOption(dataDirOpt);
    QCommandLineOption freqOpt(
        {QStringLiteral("f"), QStringLiteral("frequency")},
        QStringLiteral("Start in manual mode at <kHz> instead of following the rig."),
        QStringLiteral("kHz"));
    parser.addOption(freqOpt);
    QCommandLineOption shotOpt(QStringLiteral("screenshot"),
                               QStringLiteral("Save a picture of the window to <file> after "
                                              "a few seconds and exit (for documentation)."),
                               QStringLiteral("file"));
    parser.addOption(shotOpt);
    QCommandLineOption paletteOpt(QStringLiteral("palette"),
                                  QStringLiteral("Use a light or dark colour scheme instead of the "
                                                 "desktop's (light|dark)."),
                                  QStringLiteral("scheme"));
    parser.addOption(paletteOpt);
    QCommandLineOption logOpt(QStringLiteral("log"),
                              QStringLiteral("Write what the program and Qt report to <file>, in detail "
                                             "(for looking into a problem)."),
                              QStringLiteral("file"));
    parser.addOption(logOpt);
    parser.process(app);
    if (parser.isSet(logOpt) && !Logging::toFile(parser.value(logOpt)))
        qWarning("could not open the log file %s", qPrintable(parser.value(logOpt)));
    qInfo("otd %s, Qt %s", OTD_VERSION, qVersion());

    if (parser.isSet(paletteOpt))
    {
        const bool dark = parser.value(paletteOpt).toLower() == QLatin1String("dark");
        app.setStyle(QStringLiteral("Fusion"));
        QPalette pal;
        if (dark)
        {
            const QColor bg(0x1e, 0x22, 0x27), panel(0x14, 0x18, 0x1d), text(0xe6, 0xe9, 0xec);
            pal.setColor(QPalette::Window, bg);
            pal.setColor(QPalette::WindowText, text);
            pal.setColor(QPalette::Base, panel);
            pal.setColor(QPalette::AlternateBase, QColor(0x1a, 0x1f, 0x25));
            pal.setColor(QPalette::Text, text);
            pal.setColor(QPalette::Button, bg);
            pal.setColor(QPalette::ButtonText, text);
            pal.setColor(QPalette::ToolTipBase, panel);
            pal.setColor(QPalette::ToolTipText, text);
            pal.setColor(QPalette::Highlight, QColor(0x2f, 0x81, 0xf7));
            pal.setColor(QPalette::HighlightedText, Qt::white);
            pal.setColor(QPalette::Mid, QColor(0x5a, 0x63, 0x6c));
            pal.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x94, 0x9d));
            pal.setColor(QPalette::Link, QColor(0x8a, 0xb4, 0xf8));
        }
        else
        {
            const QColor bg(0xf3, 0xf4, 0xf6), panel(Qt::white), text(0x1b, 0x22, 0x28);
            pal.setColor(QPalette::Window, bg);
            pal.setColor(QPalette::WindowText, text);
            pal.setColor(QPalette::Base, panel);
            pal.setColor(QPalette::AlternateBase, QColor(0xf4, 0xf6, 0xf8));
            pal.setColor(QPalette::Text, text);
            pal.setColor(QPalette::Button, bg);
            pal.setColor(QPalette::ButtonText, text);
            pal.setColor(QPalette::ToolTipBase, panel);
            pal.setColor(QPalette::ToolTipText, text);
            pal.setColor(QPalette::Highlight, QColor(0x1a, 0x5d, 0xc8));
            pal.setColor(QPalette::HighlightedText, Qt::white);
            pal.setColor(QPalette::Mid, QColor(0x9a, 0xa4, 0xad));
            pal.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x94, 0x9d));
            pal.setColor(QPalette::Link, QColor(0x1a, 0x5d, 0xc8));
        }
        app.setPalette(pal);
    }

    QString dataDir;
    if (parser.isSet(dataDirOpt))
    {
        dataDir = QDir(parser.value(dataDirOpt)).absolutePath();
        QDir().mkpath(dataDir);
    }
    else
    {
        dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }

    MainWindow w(dataDir, parser.value(freqOpt).toDouble());
    w.show();
    if (parser.isSet(shotOpt))
        w.screenshotTo(parser.value(shotOpt), 8000);
    return app.exec();
}
