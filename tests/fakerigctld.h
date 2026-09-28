// SPDX-License-Identifier: GPL-3.0-or-later
// A small fake rigctld on localhost for the tests: it answers the plain
// rigctl protocol ("f", "m", "F", "M") the way Hamlib does, and can be told
// to behave like a rigctld whose radio is switched off.
#pragma once

#include <QObject>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>

class FakeRigctld : public QObject
{
public:
    qint64 hz = 7125000;
    QString mode = QStringLiteral("USB");
    bool radioOn = true;           // off: every command answers "RPRT -5"
    bool zeroWhenOff = false;      // off: "f" answers "0" instead, as some rigs do
    bool modeFails = false;        // "m" answers "RPRT -11" while "f" still works
    QStringList received;          // commands as they arrived, in order

    FakeRigctld()
    {
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* s = server.nextPendingConnection())
            {
                clients << s;
                connect(s, &QTcpSocket::readyRead, this, [this, s]() { serve(s); });
            }
        });
        server.listen(QHostAddress::LocalHost);
    }
    quint16 port() const { return server.serverPort(); }
    // Cut every connection, as a rigctld that is stopped would.
    void dropClients()
    {
        for (QTcpSocket* s : clients)
            s->abort();
        clients.clear();
    }

private:
    void serve(QTcpSocket* s)
    {
        // the socket keeps a partial line until the rest arrives
        while (s->canReadLine())
        {
            const QString cmd = QString::fromLatin1(s->readLine()).trimmed();
            received << cmd;
            if (!radioOn)
            {
                if (zeroWhenOff && cmd == QLatin1String("f"))
                    s->write("0\n");
                else
                    s->write("RPRT -5\n");
                continue;
            }
            if (cmd == QLatin1String("f"))
                s->write(QByteArray::number(hz) + '\n');
            else if (cmd == QLatin1String("m"))
            {
                if (modeFails)
                    s->write("RPRT -11\n");
                else
                    s->write(mode.toLatin1() + "\n2400\n");
            }
            else if (cmd.startsWith(QLatin1String("F ")))
            {
                hz = cmd.mid(2).toLongLong();
                s->write("RPRT 0\n");
            }
            else if (cmd.startsWith(QLatin1String("M ")))
            {
                mode = cmd.section(QLatin1Char(' '), 1, 1);
                s->write("RPRT 0\n");
            }
            else
                s->write("RPRT -1\n");
        }
    }
    QTcpServer server;
    QList<QTcpSocket*> clients;
};
