#include <QtTest>

#include "RuneFile.h"

using namespace cantrip;

class RuneFileTest : public QObject {
    Q_OBJECT

private slots:
    void everythingSavedComesBack() {
        RuneConfig saved;
        saved.channelDisplayName = "Vector XL Driver Library VN1640A Channel 1";
        saved.busConfig.nominalBitrateBps = 250000;
        saved.busConfig.dataBitrateBps = 4000000;
        saved.busConfig.fd = true;
        saved.busConfig.nominalTiming = {4, 63, 16, 16};
        saved.busConfig.dataTiming = {2, 7, 2, 2};
        saved.displayMode = RuneDisplayMode::Periodic;
        saved.displayRateMs = 100;
        saved.dbcPath = "C:/dbc/powertrain.dbc";
        saved.traceHeaderState = QByteArray::fromHex("00ff10203040");
        saved.listenOnly = true;

        GraphView::AxisLayout axis;
        axis.name = "Speed";
        axis.autoScale = false;
        axis.min = -12.5;
        axis.max = 8000.0;
        axis.hidden = true;
        axis.plottedSignals.push_back({"EngineData.EngineSpeed", QColor(10, 20, 30), GraphLineStyle::Scatter});
        axis.plottedSignals.push_back({"EngineData.EngineTemp", QColor(200, 100, 0), GraphLineStyle::Solid});
        saved.graphWindows = {{axis}, {}, {axis, axis}};

        TransmitMessage message;
        message.id = 0x18DAF110;
        message.extended = true;
        message.fd = true;
        message.brs = true;
        message.dlc = 12;
        message.data = QByteArray::fromHex("000102030405060708090A0B");
        message.cycleTimeMs = 250;
        message.paused = true;
        message.comment = "tester present, with \"quotes\"";
        saved.transmitMessages = {message, TransmitMessage{}};
        saved.extraBuses = {{2, "PEAK-System PCAN-Basic PCAN-USB FD", 250000}, {4, "Vector XL Driver Library VN1640A Channel 2", 1000000}};

        QTemporaryDir dir;
        const QString path = dir.filePath("config.rune");
        QString error;
        QVERIFY2(saveRuneFile(path, saved, &error), qPrintable(error));
        const std::optional<RuneConfig> loaded = loadRuneFile(path, &error);
        QVERIFY2(loaded.has_value(), qPrintable(error));

        QCOMPARE(loaded->channelDisplayName, saved.channelDisplayName);
        QCOMPARE(loaded->busConfig.nominalBitrateBps, 250000u);
        QCOMPARE(loaded->busConfig.dataBitrateBps, 4000000u);
        QCOMPARE(loaded->busConfig.fd, true);
        QCOMPARE(loaded->busConfig.nominalTiming.brp, 4u);
        QCOMPARE(loaded->busConfig.nominalTiming.tseg1, 63u);
        QCOMPARE(loaded->busConfig.nominalTiming.tseg2, 16u);
        QCOMPARE(loaded->busConfig.nominalTiming.sjw, 16u);
        QCOMPARE(loaded->busConfig.dataTiming.brp, 2u);
        QCOMPARE(loaded->busConfig.dataTiming.tseg1, 7u);
        QCOMPARE(loaded->busConfig.dataTiming.tseg2, 2u);
        QCOMPARE(loaded->busConfig.dataTiming.sjw, 2u);
        QVERIFY(loaded->displayMode == RuneDisplayMode::Periodic);
        QCOMPARE(loaded->displayRateMs, 100);
        QCOMPARE(loaded->dbcPath, saved.dbcPath);
        QCOMPARE(loaded->traceHeaderState, saved.traceHeaderState);
        QCOMPARE(loaded->listenOnly, true);

        QCOMPARE(loaded->graphWindows.size(), size_t(3));
        QCOMPARE(loaded->graphWindows[0].size(), size_t(1));
        QCOMPARE(loaded->graphWindows[1].size(), size_t(0));
        QCOMPARE(loaded->graphWindows[2].size(), size_t(2));
        const GraphView::AxisLayout& gotAxis = loaded->graphWindows[0][0];
        QCOMPARE(gotAxis.name, QString("Speed"));
        QCOMPARE(gotAxis.autoScale, false);
        QCOMPARE(gotAxis.min, -12.5);
        QCOMPARE(gotAxis.max, 8000.0);
        QCOMPARE(gotAxis.hidden, true);
        QCOMPARE(gotAxis.plottedSignals.size(), size_t(2));
        QCOMPARE(gotAxis.plottedSignals[0].qualifiedName, QString("EngineData.EngineSpeed"));
        QCOMPARE(gotAxis.plottedSignals[0].color, QColor(10, 20, 30));
        QVERIFY(gotAxis.plottedSignals[0].style == GraphLineStyle::Scatter);
        QVERIFY(gotAxis.plottedSignals[1].style == GraphLineStyle::Solid);

        QCOMPARE(loaded->extraBuses.size(), size_t(2));
        QCOMPARE(loaded->extraBuses[0].bus, 2);
        QCOMPARE(loaded->extraBuses[0].channelDisplayName, QString("PEAK-System PCAN-Basic PCAN-USB FD"));
        QCOMPARE(loaded->extraBuses[0].bitrateBps, 250000u);
        QCOMPARE(loaded->extraBuses[1].bus, 4);
        QCOMPARE(loaded->extraBuses[1].bitrateBps, 1000000u);

        QCOMPARE(loaded->transmitMessages.size(), size_t(2));
        const TransmitMessage& got = loaded->transmitMessages[0];
        QCOMPARE(got.id, 0x18DAF110u);
        QCOMPARE(got.extended, true);
        QCOMPARE(got.fd, true);
        QCOMPARE(got.brs, true);
        QCOMPARE(got.rtr, false);
        QCOMPARE(got.dlc, uint8_t(12));
        QCOMPARE(got.data, message.data);
        QCOMPARE(got.cycleTimeMs, 250);
        QCOMPARE(got.paused, true);
        QCOMPARE(got.comment, message.comment);
    }

    void aRuneFromBeforeNewerFieldsGetsDefaults() {
        QTemporaryDir dir;
        const QString path = dir.filePath("old.rune");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"json({"channelDisplayName": "CANtrip synthetic test source (no hardware needed)"})json");
        file.close();

        QString error;
        const std::optional<RuneConfig> loaded = loadRuneFile(path, &error);
        QVERIFY2(loaded.has_value(), qPrintable(error));
        QCOMPARE(loaded->displayRateMs, 33);
        QCOMPARE(loaded->listenOnly, false);
        QVERIFY(loaded->displayMode == RuneDisplayMode::Waterfall);
        QVERIFY(loaded->traceHeaderState.isEmpty());
        QVERIFY(loaded->transmitMessages.empty());
        QVERIFY(loaded->extraBuses.empty());
    }

    void brokenFilesFailWithAMessage() {
        QTemporaryDir dir;
        QString error;
        QVERIFY(!loadRuneFile(dir.filePath("missing.rune"), &error).has_value());
        QVERIFY(!error.isEmpty());

        const QString path = dir.filePath("broken.rune");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{ this is not json");
        file.close();
        error.clear();
        QVERIFY(!loadRuneFile(path, &error).has_value());
        QVERIFY(!error.isEmpty());
    }
};

QTEST_MAIN(RuneFileTest)
#include "tst_rune_file.moc"
