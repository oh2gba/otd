// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class StationDb;

// The list of public KiwiSDR receivers, as mirrored for the receiver map
// at rx.linkfanel.net (generated from kiwisdr.com/public). Fetched at
// most once a day with If-Modified-Since, kept in the database.
class KiwiDirectory : public QObject
{
    Q_OBJECT
public:
    struct Receiver
    {
        QString url;        // http://host:port
        QString name;
        QString location;
        QString grid;
        int users = 0;
        int usersMax = 0;
        double lowKHz = 0.0;    // frequency coverage
        double highKHz = 30000.0;
        bool offline = false;
    };

    KiwiDirectory(StationDb* db, QNetworkAccessManager* nam, QObject* parent = nullptr);

    QList<Receiver> receivers() const { return m_receivers; }
    QDateTime fetched() const;
    bool isStale(int maxAgeHours = 24) const;
    static QUrl defaultUrl();

    // Parses the JavaScript file ("var kiwisdr_com = [ ... ];") or plain JSON.
    static QList<Receiver> parse(const QByteArray& data, QString* error = nullptr);

public slots:
    void refresh(bool force = false);

signals:
    void updated();
    void failed(const QString& reason);

private:
    void load();
    void store(const QByteArray& json, const QString& lastModified);

    StationDb* m_db;
    QNetworkAccessManager* m_nam;
    QList<Receiver> m_receivers;
    bool m_busy = false;
};
