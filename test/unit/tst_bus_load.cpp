#include <QtTest>

#include <cmath>

#include "BusLoadTracker.h"
#include "TestFrames.h"

using namespace cantrip;
using namespace cantrip::testing;

// The numbers here are the worked examples from
// docs/architecture/bus-load-calculation.md.
class BusLoadTest : public QObject {
    Q_OBJECT

    static BusLoadStats statsAfter(const CanBitrateConfig& config, const std::vector<DecodedCanFrame>& frames) {
        BusLoadTracker tracker;
        tracker.setBusConfig(config);
        tracker.reset();
        BusLoadStats latest;
        connect(&tracker, &BusLoadTracker::statsUpdated, &tracker, [&](const BusLoadStats& s) { latest = s; });
        for (const DecodedCanFrame& f : frames) tracker.recordFrame(f);
        if (!QTest::qWaitFor([&]() { return latest.totalFrames + latest.totalErrorFrames >= frames.size(); }, 3000)) {
            qFatal("no statistics arrived");
        }
        return latest;
    }

    static double busMicroseconds(const BusLoadStats& stats, uint32_t id) {
        for (const BusLoadPerIdStats& row : stats.perId) {
            if (row.id == id) return row.totalBusSeconds * 1e6 / static_cast<double>(row.frameCount);
        }
        return -1.0;
    }

private slots:
    void classicFrameTakesItsBitTime() {
        CanBitrateConfig config;
        config.nominalBitrateBps = 500000;
        const BusLoadStats stats = statsAfter(config, {frame(0x100, 0.0), frame(0x100, 0.1), frame(0x200, 0.2)});

        QCOMPARE(stats.totalFrames, uint64_t(3));
        QCOMPARE(stats.totalBytes, uint64_t(24));
        QCOMPARE(stats.perId.size(), size_t(2));
        // 8 data bytes, standard ID: 125.7 bit times at 500 kbit/s.
        QVERIFY2(std::abs(busMicroseconds(stats, 0x100) - 251.4) < 0.05, qPrintable(QString::number(busMicroseconds(stats, 0x100))));
        QVERIFY(std::abs(busMicroseconds(stats, 0x200) - 251.4) < 0.05);
    }

    void aSlowerBusMakesTheSameFrameLonger() {
        CanBitrateConfig config;
        config.nominalBitrateBps = 125000;
        const BusLoadStats stats = statsAfter(config, {frame(0x100, 0.0)});
        QVERIFY(std::abs(busMicroseconds(stats, 0x100) - 251.4 * 4) < 0.2);
    }

    void bitRateSwitchShortensAnFdFrame() {
        CanBitrateConfig config;
        config.nominalBitrateBps = 500000;
        config.dataBitrateBps = 2000000;
        config.fd = true;

        DecodedCanFrame slow = frame(0x300, 0.0, QByteArray(64, '\x55'));
        slow.fd = true;
        DecodedCanFrame fast = slow;
        fast.id = 0x301;
        fast.brs = true;

        const BusLoadStats stats = statsAfter(config, {slow, fast});
        const double withoutSwitch = busMicroseconds(stats, 0x300);
        const double withSwitch = busMicroseconds(stats, 0x301);
        QVERIFY2(std::abs(withoutSwitch - 1302.0) < 1.0, qPrintable(QString::number(withoutSwitch)));
        QVERIFY2(std::abs(withSwitch - 383.0) < 1.0, qPrintable(QString::number(withSwitch)));
    }

    void busErrorsAreCountedButTakeNoBusTime() {
        CanBitrateConfig config;
        const BusLoadStats stats = statsAfter(config, {frame(0x100, 0.0), errorFrame(0.1, "Stuff Error")});
        QCOMPARE(stats.totalFrames, uint64_t(1));
        QCOMPARE(stats.totalErrorFrames, uint64_t(1));
        QCOMPARE(stats.perId.size(), size_t(1));
    }
};

QTEST_GUILESS_MAIN(BusLoadTest)
#include "tst_bus_load.moc"
