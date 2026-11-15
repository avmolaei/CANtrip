#include <QtTest>

#include "DbcDecoder.h"
#include "TestFrames.h"

using namespace cantrip;
using namespace cantrip::testing;

class DbcDecoderTest : public QObject {
    Q_OBJECT

    // A one-message DBC: `id` is called `name` and has one 8 bit signal.
    static QString writeDbc(const QTemporaryDir& dir, const QString& fileName, int id, const QString& name) {
        const QString path = dir.filePath(fileName);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) qFatal("cannot write %s", qPrintable(path));
        file.write(QString("VERSION \"\"\n\nNS_ :\n\nBS_:\n\nBU_: ECU1\n\nBO_ %1 %2: 8 ECU1\n"
                           " SG_ %2Value : 0|8@1+ (1,0) [0|255] \"u\" Vector__XXX\n\n").arg(id).arg(name).toUtf8());
        return path;
    }

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

    void severalFilesDecodeTogether() {
        QTemporaryDir dir;
        DbcDecoder decoder;
        QString error;
        QVERIFY2(decoder.addFile(sampleDbcPath(), DbcDecoder::kAllBuses, &error), qPrintable(error));
        QVERIFY2(decoder.addFile(writeDbc(dir, "body.dbc", 0x555, "DoorStatus"), DbcDecoder::kAllBuses, &error), qPrintable(error));

        QCOMPARE(decoder.files().size(), size_t(2));
        QCOMPARE(decoder.files()[1].messageCount, 1);
        QCOMPARE(decoder.resolveMessageName(frame(0x100, 0)), QString("EngineData"));
        QCOMPARE(decoder.resolveMessageName(frame(0x555, 0)), QString("DoorStatus"));
        QCOMPARE(decoder.decodeSignals(frame(0x555, 0)).front().name, QString("DoorStatusValue"));
        QVERIFY(decoder.conflicts().isEmpty());
    }

    void theFirstFileWinsAnIdBothDefine() {
        QTemporaryDir dir;
        DbcDecoder decoder;
        QVERIFY(decoder.addFile(sampleDbcPath(), DbcDecoder::kAllBuses, nullptr));
        QVERIFY(decoder.addFile(writeDbc(dir, "other.dbc", 0x100, "Impostor"), DbcDecoder::kAllBuses, nullptr));
        QCOMPARE(decoder.resolveMessageName(frame(0x100, 0)), QString("EngineData"));

        const QStringList conflicts = decoder.conflicts();
        QCOMPARE(conflicts.size(), 1);
        QVERIFY2(conflicts.first().contains("0x100") && conflicts.first().contains("sample.dbc") && conflicts.first().contains("other.dbc"),
                 qPrintable(conflicts.first()));

        // Take the winner away and the other file's definition applies.
        decoder.removeFile(0);
        QCOMPARE(decoder.resolveMessageName(frame(0x100, 0)), QString("Impostor"));
        QCOMPARE(decoder.signalCount(frame(0x100, 0)), 1);
        QVERIFY(decoder.conflicts().isEmpty());
    }

    void aFileCanBelongToOneBus() {
        QTemporaryDir dir;
        DbcDecoder decoder;
        QVERIFY(decoder.addFile(writeDbc(dir, "bus1.dbc", 0x100, "OnBusOne"), 1, nullptr));
        QVERIFY(decoder.addFile(writeDbc(dir, "bus2.dbc", 0x100, "OnBusTwo"), 2, nullptr));

        DecodedCanFrame onBus2 = frame(0x100, 0);
        onBus2.bus = 2;
        DecodedCanFrame onBus3 = frame(0x100, 0);
        onBus3.bus = 3;
        QCOMPARE(decoder.resolveMessageName(frame(0x100, 0)), QString("OnBusOne"));
        QCOMPARE(decoder.resolveMessageName(onBus2), QString("OnBusTwo"));
        QVERIFY(decoder.resolveMessageName(onBus3).isEmpty());
        QVERIFY(decoder.conflicts().isEmpty()); // different buses never meet

        // A file for every bus does overlap with a per-bus one.
        QVERIFY(decoder.addFile(writeDbc(dir, "all.dbc", 0x100, "Everywhere"), DbcDecoder::kAllBuses, nullptr));
        QCOMPARE(decoder.resolveMessageName(onBus3), QString("Everywhere"));
        QCOMPARE(decoder.resolveMessageName(onBus2), QString("OnBusTwo"));
        QCOMPARE(decoder.conflicts().size(), 2);
    }

    void addingABadFileChangesNothing() {
        DbcDecoder decoder;
        QVERIFY(decoder.addFile(sampleDbcPath(), DbcDecoder::kAllBuses, nullptr));
        QString error;
        QVERIFY(!decoder.addFile("Z:/no/such/file.dbc", 2, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(decoder.files().size(), size_t(1));
        decoder.clear();
        QVERIFY(!decoder.isLoaded());
        QVERIFY(decoder.resolveMessageName(frame(0x100, 0)).isEmpty());
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
