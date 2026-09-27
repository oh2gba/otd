// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/KiwiClient.h"

#include <QtEndian>
#include <QtTest>

class TestKiwi : public QObject
{
    Q_OBJECT
private slots:
    void adpcmDecodesKnownBytes()
    {
        // reference values from the standard IMA ADPCM algorithm
        const QByteArray in = QByteArray::fromHex("0077f812ab34ff01");
        const QList<int> expected = {0, 0, 11, 41, 37, -19, 22, 44, -2, -33, 17, 63, -30, -229, -144, -118};
        KiwiClient::Adpcm d;
        const QByteArray pcm = d.decode(in);
        QCOMPARE(pcm.size(), in.size() * 4);
        for (int i = 0; i < expected.size(); ++i)
            QCOMPARE(int(qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(pcm.constData() + 2 * i))),
                     expected[i]);
        QCOMPARE(d.index, 34);
        QCOMPARE(d.prev, -118);
    }

    void adpcmHonoursPresetState()
    {
        KiwiClient::Adpcm d;
        d.index = 30;
        d.prev = 1000;
        const QByteArray pcm = d.decode(QByteArray::fromHex("2143"));
        const QList<int> expected = {1048, 1121, 1213, 1322};
        for (int i = 0; i < expected.size(); ++i)
            QCOMPARE(int(qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(pcm.constData() + 2 * i))),
                     expected[i]);
    }

    void silenceStaysNearZero()
    {
        KiwiClient::Adpcm d;
        const QByteArray pcm = d.decode(QByteArray(100, char(0x88)));   // -0 steps
        for (int i = 0; i < pcm.size(); i += 2)
            QVERIFY(qAbs(qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(pcm.constData() + i))) < 64);
    }
};

QTEST_GUILESS_MAIN(TestKiwi)
#include "test_kiwi.moc"
