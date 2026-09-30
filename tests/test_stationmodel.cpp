// SPDX-License-Identifier: GPL-3.0-or-later
// What the station list shows for an entry. The rows keep the codes the
// sources publish (EiBi's, HFCC's); the list and the tooltip show them
// through the lookup tables.
#include "core/StationModel.h"
#include "core/StationDb.h"

#include <QTemporaryDir>
#include <QtTest>

class TestStationModel : public QObject
{
    Q_OBJECT

    static StationEntry eibi(double kHz, const QString& name, const QString& itu, const QString& lang,
                             const QString& site)
    {
        StationEntry e;
        e.source = QStringLiteral("eibi");
        e.kHz = kHz;
        e.station = name;
        e.itu = itu;
        e.lang = lang;
        e.site = site;
        return e;
    }

    // the model's text for the row of this station (as the list names it)
    static QString shown(const StationModel& model, const QString& station, int column, int role = Qt::DisplayRole)
    {
        for (int r = 0; r < model.rowCount(); ++r)
            if (model.data(model.index(r, StationModel::ColStation), Qt::DisplayRole).toString() == station)
                return model.data(model.index(r, column), role).toString();
        return QStringLiteral("(no row %1)").arg(station);
    }

private slots:
    // The list shows the short language name and the site; the tooltip
    // keeps EiBi's whole description of the language, where it is spoken.
    void languageAndSiteAsShown()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        EibiParser::CodeTables t;
        t.languages.insert(QStringLiteral("FI"), QStringLiteral("Finnish: Finland (5m)"));
        t.languages.insert(QStringLiteral("E"), QStringLiteral("English: UK (60m), USA (225m)"));
        t.languages.insert(QStringLiteral("F"), QStringLiteral("French: France (53m), Canada (7m)"));
        t.countries.insert(QStringLiteral("FIN"), QStringLiteral("Finland"));
        t.countries.insert(QStringLiteral("D"), QStringLiteral("Germany"));
        t.sites.insert(QStringLiteral("FIN/hv"), QStringLiteral("Harjavalta"));
        t.sites.insert(QStringLiteral("D/n"), QStringLiteral("Nauen"));
        QVERIFY(db.storeCodes(t));
        QVERIFY(db.replaceSource(QStringLiteral("eibi"),
                                 {eibi(7400, QStringLiteral("Radio Hiekka"), QStringLiteral("FIN"), QStringLiteral("FI"), QStringLiteral("hv")),
                                  eibi(7410, QStringLiteral("Two tongues"), QStringLiteral("USA"), QStringLiteral("F,E"), QStringLiteral("/D-n")),
                                  eibi(7420, QStringLiteral("Unknown"), QStringLiteral("XYZ"), QStringLiteral("QQ"), QStringLiteral("zz"))}));
        HfccParser::Tables h;
        h.broadcasters.insert(QStringLiteral("YLE"), QStringLiteral("Yleisradio"));
        h.languages.insert(QStringLiteral("Fin"), QStringLiteral("Finnish"));
        h.sites.insert(QStringLiteral("PRI"), {QStringLiteral("Pori"), QStringLiteral("FIN")});
        h.admins.insert(QStringLiteral("FIN"), QStringLiteral("Finland"));
        QVERIFY(db.storeHfccTables(h));
        StationEntry hfcc;
        hfcc.source = QStringLiteral("hfcc");
        hfcc.kHz = 7430;
        hfcc.station = QStringLiteral("YLE");
        hfcc.itu = QStringLiteral("FIN");
        hfcc.lang = QStringLiteral("Fin");
        hfcc.site = QStringLiteral("PRI");
        hfcc.power = QStringLiteral("500");
        hfcc.azimuth = QStringLiteral("210");
        hfcc.mode = QStringLiteral("D");
        QVERIFY(db.replaceSource(QStringLiteral("hfcc"), {hfcc}));

        StationModel model(&db);
        model.setEntries(db.lookup(7415, 20), 7415);
        QCOMPARE(model.rowCount(), 4);
        QCOMPARE(shown(model, QStringLiteral("Radio Hiekka"), StationModel::ColLanguage), QStringLiteral("Finnish"));
        QCOMPARE(shown(model, QStringLiteral("Radio Hiekka"), StationModel::ColSite), QStringLiteral("Harjavalta"));
        QCOMPARE(shown(model, QStringLiteral("Radio Hiekka"), StationModel::ColMode), QStringLiteral("AM"));   // worked out
        QCOMPARE(shown(model, QStringLiteral("Two tongues"), StationModel::ColLanguage), QStringLiteral("French, English"));
        QCOMPARE(shown(model, QStringLiteral("Two tongues"), StationModel::ColSite), QStringLiteral("Nauen (Germany)"));
        QCOMPARE(shown(model, QStringLiteral("Unknown"), StationModel::ColLanguage), QStringLiteral("QQ"));
        QCOMPARE(shown(model, QStringLiteral("Unknown"), StationModel::ColSite), QStringLiteral("zz"));
        // HFCC: the broadcaster's name for its code, and the rest
        QCOMPARE(shown(model, QStringLiteral("Yleisradio"), StationModel::ColLanguage), QStringLiteral("Finnish"));
        QCOMPARE(shown(model, QStringLiteral("Yleisradio"), StationModel::ColSite), QStringLiteral("Pori"));
        QCOMPARE(shown(model, QStringLiteral("Yleisradio"), StationModel::ColCountry), QStringLiteral("Finland"));
        QCOMPARE(shown(model, QStringLiteral("Yleisradio"), StationModel::ColMode), QStringLiteral("AM"));
        QCOMPARE(shown(model, QStringLiteral("Yleisradio"), StationModel::ColRemarks), QStringLiteral("500 kW, az 210°"));

        const QString hiekka = shown(model, QStringLiteral("Radio Hiekka"), StationModel::ColStation, Qt::ToolTipRole);
        QVERIFY2(hiekka.contains(QLatin1String("Language: Finnish: Finland (5m)")), qPrintable(hiekka));
        QVERIFY2(hiekka.contains(QLatin1String("Transmitter: Harjavalta")), qPrintable(hiekka));
        const QString unknown = shown(model, QStringLiteral("Unknown"), StationModel::ColStation, Qt::ToolTipRole);
        QVERIFY2(unknown.contains(QLatin1String("Language: QQ")), qPrintable(unknown));
        const QString yle = shown(model, QStringLiteral("Yleisradio"), StationModel::ColStation, Qt::ToolTipRole);
        QVERIFY2(yle.contains(QLatin1String("<b>Yleisradio</b>")), qPrintable(yle));
        QVERIFY2(yle.contains(QLatin1String("Language: Finnish<")), qPrintable(yle));
        const QString two = shown(model, QStringLiteral("Two tongues"), StationModel::ColStation, Qt::ToolTipRole);
        QVERIFY2(two.contains(QLatin1String("Language: French: France (53m), Canada (7m); English: UK (60m), USA (225m)")),
                 qPrintable(two));
    }

    // Targets: EiBi's codes and HFCC's CIRAF zones as names in the list,
    // with what they stand for in the tooltip.
    void targetsAsShown()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        EibiParser::CodeTables t;
        t.targets.insert(QStringLiteral("Eu"), QStringLiteral("Europe (often including North Africa/Middle East)"));
        t.targets.insert(QStringLiteral("N.."), QStringLiteral("North .."));
        t.countries.insert(QStringLiteral("CHN"), QStringLiteral("China"));
        QVERIFY(db.storeCodes(t));
        StationEntry north = eibi(7400, QStringLiteral("North"), QStringLiteral("FIN"), QString(), QString());
        north.target = QStringLiteral("NEu");
        StationEntry europe = eibi(7401, QStringLiteral("Europe"), QStringLiteral("D"), QString(), QString());
        europe.target = QStringLiteral("Eu");
        StationEntry china = eibi(7402, QStringLiteral("China"), QStringLiteral("CHN"), QString(), QString());
        china.target = QStringLiteral("CHN");
        StationEntry odd = eibi(7403, QStringLiteral("Odd"), QStringLiteral("D"), QString(), QString());
        odd.target = QStringLiteral("Tas");
        QVERIFY(db.replaceSource(QStringLiteral("eibi"), {north, europe, china, odd}));
        StationEntry hfcc;
        hfcc.source = QStringLiteral("hfcc");
        hfcc.kHz = 7404;
        hfcc.station = QStringLiteral("Zones");
        hfcc.target = QStringLiteral("18,40E");
        StationEntry unknown = hfcc;
        unknown.kHz = 7405;
        unknown.station = QStringLiteral("Zone 99");
        unknown.target = QStringLiteral("99");
        QVERIFY(db.replaceSource(QStringLiteral("hfcc"), {hfcc, unknown}));

        StationModel model(&db);
        model.setEntries(db.lookup(7402, 10), 7402);
        QCOMPARE(model.rowCount(), 6);
        QCOMPARE(shown(model, QStringLiteral("North"), StationModel::ColTarget), QStringLiteral("North Europe"));
        QCOMPARE(shown(model, QStringLiteral("Europe"), StationModel::ColTarget), QStringLiteral("Europe"));
        QCOMPARE(shown(model, QStringLiteral("China"), StationModel::ColTarget), QStringLiteral("China"));
        QCOMPARE(shown(model, QStringLiteral("Odd"), StationModel::ColTarget), QStringLiteral("Tas"));
        QCOMPARE(shown(model, QStringLiteral("Zone 99"), StationModel::ColTarget), QStringLiteral("99"));
        QCOMPARE(shown(model, QStringLiteral("Zones"), StationModel::ColTarget),
                 QStringLiteral("Nordic countries, Iran, Afghanistan (east)"));

        const QString eu = shown(model, QStringLiteral("Europe"), StationModel::ColStation, Qt::ToolTipRole);
        QVERIFY2(eu.contains(QLatin1String("Target: Europe (often including North Africa/Middle East)")), qPrintable(eu));
        const QString zones = shown(model, QStringLiteral("Zones"), StationModel::ColStation, Qt::ToolTipRole);
        QVERIFY2(zones.contains(QLatin1String("Target: CIRAF 18: Denmark, Finland, Norway, Sweden; 40 east: Afghanistan, Iran")),
                 qPrintable(zones));
        const QString oddTip = shown(model, QStringLiteral("Odd"), StationModel::ColStation, Qt::ToolTipRole);
        QVERIFY2(oddTip.contains(QLatin1String("Target: Tas")), qPrintable(oddTip));
    }
};

QTEST_MAIN(TestStationModel)
#include "test_stationmodel.moc"
