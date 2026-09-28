// SPDX-License-Identifier: GPL-3.0-or-later
#include "Logging.h"

#include <QDateTime>
#include <QFile>
#include <QLoggingCategory>
#include <QMutex>
#include <QTextStream>

namespace
{
QFile* g_file = nullptr;
QMutex g_mutex;

void toLogFile(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    const char* kind = "debug";
    switch (type)
    {
    case QtInfoMsg:     kind = "info"; break;
    case QtWarningMsg:  kind = "warning"; break;
    case QtCriticalMsg: kind = "critical"; break;
    case QtFatalMsg:    kind = "fatal"; break;
    default: break;
    }
    QMutexLocker lock(&g_mutex);
    if (!g_file)
        return;
    QTextStream out(g_file);
    out << QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")) << ' '
        << (ctx.category ? ctx.category : "") << ' ' << kind << ": " << msg << '\n';
    out.flush();
    g_file->flush();   // every line lands, even if the program dies next
}
}

bool Logging::toFile(const QString& path)
{
    auto* file = new QFile(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        delete file;
        return false;
    }
    {
        QMutexLocker lock(&g_mutex);
        g_file = file;
    }
    QLoggingCategory::setFilterRules(QStringLiteral("otd.*=true\nqt.multimedia.*=true"));
    qInstallMessageHandler(toLogFile);
    qInfo("---- log started ----");
    return true;
}
