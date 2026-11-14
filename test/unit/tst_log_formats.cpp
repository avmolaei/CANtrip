#include <QtTest>

#include <memory>
#include <vector>

#include "AscLogWriter.h"
#include "CsvLogWriter.h"
#include "LogReplaySource.h"
#include "TestFrames.h"

using namespace cantrip;
using namespace cantrip::testing;

// Writes a log with the real writers, reads it back with the real replay
// source, and checks nothing changed on the way.
class LogFormatsTest : public QObject {
    Q_OBJECT

    static std::vector<DecodedCanFrame> sampleFrames() {
        std::vector<DecodedCanFrame> frames;
        frames.push_back(frame(0x123, 0.001));

        DecodedCanFrame extended = frame(0x18DAF110, 0.002, QByteArray::fromHex("0210C0"));
        extended.extended = true;
        frames.push_back(extended);

        DecodedCanFrame fd = frame(0x321, 0.003, QByteArray::fromHex("000102030405060708090A0B"));
        fd.fd = true;
        fd.brs = true;
        frames.push_back(fd);

        DecodedCanFrame remote = frame(0x7FF, 0.004, QByteArray());
        remote.rtr = true;
        remote.dlc = 4;
        frames.push_back(remote);

        DecodedCanFrame sent = frame(0x100, 0.005);
        sent.direction = FrameDirection::Tx;
        frames.push_back(sent);
        return frames;
    }

    static std::vector<DecodedCanFrame> writeAndReplay(ILogWriter& writer, const QString& path,
                                                       const std::vector<DecodedCanFrame>& frames) {
        QString error;
        if (!writer.open(path, &error)) qFatal("open failed: %s", qPrintable(error));
        for (const DecodedCanFrame& f : frames) writer.writeFrame(1, f);
        writer.close();

        std::vector<DecodedCanFrame> replayed;
        LogReplaySource replay;
        bool stopped = false;
        connect(&replay, &LogReplaySource::frameReceived, &replay, [&](const DecodedCanFrame& f) { replayed.push_back(f); });
        connect(&replay, &LogReplaySource::stopped, &replay, [&]() { stopped = true; });
        if (!replay.start(path, &error)) qFatal("replay failed: %s", qPrintable(error));
        if (!QTest::qWaitFor([&]() { return stopped; }, 5000)) qFatal("replay never finished");
        return replayed;
    }

    static void compareFrames(const DecodedCanFrame& got, const DecodedCanFrame& want) {
        QCOMPARE(got.error, want.error);
        QCOMPARE(got.timestamp.toDouble(), want.timestamp.toDouble());
        QCOMPARE(got.id, want.id);
        QCOMPARE(got.extended, want.extended);
        QCOMPARE(got.fd, want.fd);
        QCOMPARE(got.brs, want.brs);
        QCOMPARE(got.rtr, want.rtr);
        QCOMPARE(got.dlc, want.dlc);
        QCOMPARE(got.data.toHex(), want.data.toHex());
    }

private slots:
    void ascRoundTrips() {
        QTemporaryDir dir;
        AscLogWriter writer;
        const auto frames = sampleFrames();
        const auto replayed = writeAndReplay(writer, dir.filePath("log.asc"), frames);
        QCOMPARE(replayed.size(), frames.size());
        for (size_t i = 0; i < frames.size(); ++i) compareFrames(replayed[i], frames[i]);
    }

    void ascRoundTripsWithMessageNames() {
        // The name sits in the middle of an FD line, where a parser that
        // counts columns would trip over it.
        QTemporaryDir dir;
        AscLogWriter writer([](const DecodedCanFrame&) { return QString("EngineData"); });
        const auto frames = sampleFrames();
        const auto replayed = writeAndReplay(writer, dir.filePath("log.asc"), frames);
        QCOMPARE(replayed.size(), frames.size());
        for (size_t i = 0; i < frames.size(); ++i) compareFrames(replayed[i], frames[i]);
    }

    void ascLeavesBusErrorsOut() {
        QTemporaryDir dir;
        AscLogWriter writer;
        const auto replayed = writeAndReplay(writer, dir.filePath("log.asc"),
                                             {frame(0x123, 0.001), errorFrame(0.002, "Stuff Error"), frame(0x124, 0.003)});
        QCOMPARE(replayed.size(), size_t(2));
        QCOMPARE(replayed[1].id, 0x124u);
    }

    void csvRoundTrips() {
        QTemporaryDir dir;
        CsvLogWriter writer;
        const auto frames = sampleFrames();
        const auto replayed = writeAndReplay(writer, dir.filePath("log.csv"), frames);
        QCOMPARE(replayed.size(), frames.size());
        for (size_t i = 0; i < frames.size(); ++i) compareFrames(replayed[i], frames[i]);
    }

    void csvRoundTripsWithMessageNames() {
        // A named frame must not be mistaken for a bus error row.
        QTemporaryDir dir;
        CsvLogWriter writer([](const DecodedCanFrame&) { return QString("EngineData"); });
        const auto frames = sampleFrames();
        const auto replayed = writeAndReplay(writer, dir.filePath("log.csv"), frames);
        QCOMPARE(replayed.size(), frames.size());
        for (size_t i = 0; i < frames.size(); ++i) compareFrames(replayed[i], frames[i]);
    }

    void csvKeepsBusErrorsIncludingCommas() {
        QTemporaryDir dir;
        CsvLogWriter writer;
        const auto replayed = writeAndReplay(writer, dir.filePath("log.csv"),
                                             {frame(0x123, 0.001), errorFrame(0.002, "Bit Error, Form Error"), frame(0x124, 0.003)});
        QCOMPARE(replayed.size(), size_t(3));
        QVERIFY(!replayed[0].error);
        QVERIFY(replayed[1].error);
        QCOMPARE(replayed[1].errorDescription, QString("Bit Error, Form Error"));
        QVERIFY(!replayed[2].error);
        QCOMPARE(replayed[2].id, 0x124u);
    }

    void csvFromBeforeTheDirectionColumnStillLoads() {
        // The layout v1.3 wrote, one column fewer than today's.
        QTemporaryDir dir;
        const QString path = dir.filePath("old.csv");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("Time,Channel,ID,Extended,FD,BRS,ESI,RTR,DLC,Data,MessageName,Error\n"
                   "0.001000,1,18DAF110,1,0,0,0,0,3,02 10 C0,Diag,\n"
                   "0.002000,1,,,,,,,,,,No ACK\n");
        file.close();

        std::vector<DecodedCanFrame> replayed;
        LogReplaySource replay;
        bool stopped = false;
        connect(&replay, &LogReplaySource::frameReceived, &replay, [&](const DecodedCanFrame& f) { replayed.push_back(f); });
        connect(&replay, &LogReplaySource::stopped, &replay, [&]() { stopped = true; });
        QString error;
        QVERIFY2(replay.start(path, &error), qPrintable(error));
        QVERIFY(QTest::qWaitFor([&]() { return stopped; }, 5000));

        QCOMPARE(replayed.size(), size_t(2));
        QCOMPARE(replayed[0].id, 0x18DAF110u);
        QVERIFY(replayed[0].extended);
        QCOMPARE(replayed[0].data.toHex(), QByteArray("0210c0"));
        QVERIFY(replayed[1].error);
        QCOMPARE(replayed[1].errorDescription, QString("No ACK"));
    }

    void aFileThatIsNotALogFailsCleanly() {
        QTemporaryDir dir;
        const QString path = dir.filePath("notes.csv");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("name,price\nwidget,3\n");
        file.close();

        LogReplaySource replay;
        QString error;
        QVERIFY(!replay.start(path, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!replay.start(dir.filePath("missing.asc"), &error));
    }
};

QTEST_GUILESS_MAIN(LogFormatsTest)
#include "tst_log_formats.moc"
