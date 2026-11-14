#include <QtTest>

#include "DbcDecoder.h"
#include "TestFrames.h"

using namespace cantrip;
using namespace cantrip::testing;

class DbcDecoderTest : public QObject {
    Q_OBJECT

private slots:
    void decodesSignalsWithScaleAndOffset() {
        DbcDecoder decoder;
        QString error;
        QVERIFY2(decoder.loadFile(sampleDbcPath(), &error), qPrintable(error));
        QVERIFY(decoder.isLoaded());

        const DecodedCanFrame f = frame(0x100, 0.0);
        QCOMPARE(decoder.resolveMessageName(f), QString("EngineData"));

        const auto decoded = decoder.decodeSignals(f);
        QCOMPARE(decoded.size(), size_t(2));
        QCOMPARE(decoded[0].name, QString("EngineSpeed"));
        QCOMPARE(decoded[0].qualifiedName, QString("EngineData.EngineSpeed"));
        QCOMPARE(decoded[0].unit, QString("rpm"));
        QCOMPARE(decoded[0].physicalValue, 1418.0); // 0x2C50 * 0.125
        QCOMPARE(decoded[1].name, QString("EngineTemp"));
        QCOMPARE(decoded[1].physicalValue, 90.0); // 0x82 - 40
    }

    void signalNamesAndCountMatchDecode() {
        DbcDecoder decoder;
        QVERIFY(decoder.loadFile(sampleDbcPath(), nullptr));
        const DecodedCanFrame f = frame(0x100, 0.0);
        QCOMPARE(decoder.signalCount(f), 2);
        QVERIFY(decoder.signalNames(f) != nullptr);
        QCOMPARE(*decoder.signalNames(f), (QStringList{"EngineSpeed", "EngineTemp"}));
    }

    void unknownIdDecodesToNothing() {
        DbcDecoder decoder;
        QVERIFY(decoder.loadFile(sampleDbcPath(), nullptr));
        const DecodedCanFrame f = frame(0x555, 0.0);
        QVERIFY(decoder.resolveMessageName(f).isEmpty());
        QVERIFY(decoder.decodeSignals(f).empty());
        QCOMPARE(decoder.signalCount(f), 0);
        QVERIFY(decoder.signalNames(f) == nullptr);
    }

    void extendedFlagIsPartOfTheId() {
        // 0x100 in the DBC is a standard ID: an extended frame with the same
        // number is a different message.
        DbcDecoder decoder;
        QVERIFY(decoder.loadFile(sampleDbcPath(), nullptr));
        DecodedCanFrame f = frame(0x100, 0.0);
        f.extended = true;
        QVERIFY(decoder.resolveMessageName(f).isEmpty());
    }

    void shortFramesDecodeWithoutReadingPastTheirData() {
        DbcDecoder decoder;
        QVERIFY(decoder.loadFile(sampleDbcPath(), nullptr));
        const auto decoded = decoder.decodeSignals(frame(0x100, 0.0, QByteArray::fromHex("502C")));
        QCOMPARE(decoded.size(), size_t(2));
        QCOMPARE(decoded[0].physicalValue, 1418.0);
        QCOMPARE(decoded[1].physicalValue, -40.0); // the missing byte reads as 0
    }

    void withoutADbcNothingDecodes() {
        DbcDecoder decoder;
        QVERIFY(!decoder.isLoaded());
        QVERIFY(decoder.resolveMessageName(frame(0x100, 0.0)).isEmpty());
        QVERIFY(decoder.decodeSignals(frame(0x100, 0.0)).empty());
    }

    void aBadFileFailsAndKeepsTheLoadedDbc() {
        DbcDecoder decoder;
        QVERIFY(decoder.loadFile(sampleDbcPath(), nullptr));

        QString error;
        QVERIFY(!decoder.loadFile("Z:/no/such/file.dbc", &error));
        QVERIFY(!error.isEmpty());

        QTemporaryDir dir;
        const QString garbage = dir.filePath("garbage.dbc");
        QFile file(garbage);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("this is not a dbc file\n");
        file.close();
        error.clear();
        QVERIFY(!decoder.loadFile(garbage, &error));
        QVERIFY(!error.isEmpty());

        QCOMPARE(decoder.resolveMessageName(frame(0x100, 0.0)), QString("EngineData"));
    }
};

QTEST_APPLESS_MAIN(DbcDecoderTest)
#include "tst_dbc_decoder.moc"
