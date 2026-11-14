#include <QtTest>

#include <cmath>

#include "../../common/CanBitTiming.h"

using namespace cantrip;

class CanBitTimingTest : public QObject {
    Q_OBJECT

    // What any valid timing has to satisfy, whatever values were picked.
    static void verifyTiming(const CanTimingValues& t, uint32_t clockHz, uint32_t bitrate, double samplePointPercent) {
        QVERIFY(t.brp >= 1);
        QVERIFY(t.tseg1 >= 1);
        QVERIFY(t.tseg2 >= 1);
        QVERIFY(t.sjw >= 1);
        QVERIFY(t.sjw <= t.tseg2);
        const uint32_t quantaPerBit = 1 + t.tseg1 + t.tseg2;
        QCOMPARE(clockHz % (t.brp * quantaPerBit), 0u);
        QCOMPARE(clockHz / (t.brp * quantaPerBit), bitrate);
        // The sample point can only sit on a time quantum boundary, so the
        // best any timing can do is land within one quantum of the request.
        const double samplePoint = 100.0 * (1 + t.tseg1) / quantaPerBit;
        QVERIFY2(std::abs(samplePoint - samplePointPercent) <= 100.0 / quantaPerBit, qPrintable(QString::number(samplePoint)));
    }

private slots:
    void classicTimingHitsTheBitrate_data() {
        QTest::addColumn<uint>("clockHz");
        QTest::addColumn<uint>("bitrate");
        for (uint clock : {8000000u, 16000000u, 80000000u}) {
            for (uint bitrate : {125000u, 250000u, 500000u, 1000000u}) {
                QTest::addRow("%u Hz clock, %u bit/s", clock, bitrate) << clock << bitrate;
            }
        }
    }
    void classicTimingHitsTheBitrate() {
        QFETCH(uint, clockHz);
        QFETCH(uint, bitrate);
        const auto timing = calculateTiming(clockHz, bitrate, 80.0);
        QVERIFY(timing.has_value());
        verifyTiming(*timing, clockHz, bitrate, 80.0);
    }

    void fdTimingHitsBothBitrates() {
        const auto timing = calculateFdTiming(80000000, 500000, 80.0, 2000000, 80.0);
        QVERIFY(timing.has_value());
        verifyTiming(timing->nominal, 80000000, 500000, 80.0);
        verifyTiming(timing->data, 80000000, 2000000, 80.0);
        QVERIFY(std::abs(timing->achievedNomSamplePoint - 80.0) < 5.0);
        QVERIFY(std::abs(timing->achievedDataSamplePoint - 80.0) < 5.0);
    }

    void aBitrateTheClockCannotDivideToHasNoTiming() {
        // 8 MHz has no whole number of time quanta per bit at 3 Mbit/s.
        QVERIFY(!calculateTiming(8000000, 3000000, 80.0).has_value());
    }
};

QTEST_APPLESS_MAIN(CanBitTimingTest)
#include "tst_can_bit_timing.moc"
