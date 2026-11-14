#include <QtTest>

#include <vector>

#include "AscLogWriter.h"
#include "BusTimeAligner.h"
#include "CsvLogWriter.h"
#include "LogReplaySource.h"
#include "TestFrames.h"

using namespace cantrip;
using namespace cantrip::testing;

class MultiBusTest : public QObject {
    Q_OBJECT

    static std::vector<DecodedCanFrame> replay(ILogWriter& writer, const QString& path, const std::vector<DecodedCanFrame>& frames) {
        QString error;
        if (!writer.open(path, &error)) qFatal("open failed: %s", qPrintable(error));
        for (const DecodedCanFrame& f : frames) writer.writeFrame(f.bus, f);
        writer.close();

        std::vector<DecodedCanFrame> replayed;
        LogReplaySource source;
        bool stopped = false;
        connect(&source, &LogReplaySource::frameReceived, &source, [&](const DecodedCanFrame& f) { replayed.push_back(f); });
        connect(&source, &LogReplaySource::stopped, &source, [&]() { stopped = true; });
        if (!source.start(path, &error)) qFatal("replay failed: %s", qPrintable(error));
        if (!QTest::qWaitFor([&]() { return stopped; }, 5000)) qFatal("replay never finished");
        return replayed;
    }

    static std::vector<DecodedCanFrame> framesOnThreeBuses() {
        DecodedCanFrame a = frame(0x100, 0.001);
        DecodedCanFrame b = frame(0x100, 0.002);
        b.bus = 2;
        DecodedCanFrame c = frame(0x321, 0.003, QByteArray::fromHex("000102030405060708090A0B"));
        c.fd = true;
        c.bus = 4;
        return {a, b, c};
    }

private slots:
    void framesAreOnBusOneUnlessToldOtherwise() {
        QCOMPARE(DecodedCanFrame{}.bus, 1);
    }

    void busNumbersSurviveAnAscLog() {
        QTemporaryDir dir;
        AscLogWriter writer;
        const auto replayed = replay(writer, dir.filePath("log.asc"), framesOnThreeBuses());
        QCOMPARE(replayed.size(), size_t(3));
        QCOMPARE(replayed[0].bus, 1);
        QCOMPARE(replayed[1].bus, 2);
        QCOMPARE(replayed[2].bus, 4);
        QCOMPARE(replayed[1].id, 0x100u); // same ID on two buses stays two frames
    }

    void busNumbersSurviveACsvLog() {
        QTemporaryDir dir;
        CsvLogWriter writer;
        const auto replayed = replay(writer, dir.filePath("log.csv"), framesOnThreeBuses());
        QCOMPARE(replayed.size(), size_t(3));
        QCOMPARE(replayed[0].bus, 1);
        QCOMPARE(replayed[1].bus, 2);
        QCOMPARE(replayed[2].bus, 4);
    }

    void aSingleBusKeepsItsOwnTimes() {
        BusTimeAligner aligner;
        // First frame arrives 0.6 s into the session, stamped 0 by its capture.
        QCOMPARE(aligner.align(1, 0.0, 0.6), 0.0);
        QCOMPARE(aligner.align(1, 0.25, 0.9), 0.25); // arrival jitter does not leak in
        QCOMPARE(aligner.align(1, 10.5, 11.3), 10.5);
    }

    void aBusThatStartsLaterIsShiftedByHowMuchLater() {
        BusTimeAligner aligner;
        QCOMPARE(aligner.align(1, 0.0, 0.6), 0.0);
        // Bus 2's capture saw its first frame 1.5 s after bus 1's.
        QCOMPARE(aligner.align(2, 0.0, 2.1), 1.5);
        // From then on bus 2 keeps its own spacing, on the shared timeline.
        QCOMPARE(aligner.align(2, 0.010, 2.5), 1.510);
        QCOMPARE(aligner.align(1, 2.0, 2.61), 2.0);
    }

    void resetStartsANewTimeline() {
        BusTimeAligner aligner;
        aligner.align(1, 0.0, 5.0);
        aligner.align(2, 0.0, 6.0);
        aligner.reset();
        QCOMPARE(aligner.align(2, 0.0, 100.0), 0.0);
        QCOMPARE(aligner.align(1, 0.0, 100.5), 0.5);
    }
};

QTEST_GUILESS_MAIN(MultiBusTest)
#include "tst_multi_bus.moc"
