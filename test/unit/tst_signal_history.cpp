#include <QtTest>

#include <algorithm>
#include <cmath>

#include "SignalHistory.h"

using namespace cantrip;

class SignalHistoryTest : public QObject {
    Q_OBJECT

    // Deterministic noise in [0, 1), so a failure reproduces.
    static double noise(quint32& state) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) / 16777216.0;
    }

    static SignalSeries::MinMax bruteMinMax(const SignalSeries& s, size_t first, size_t last) {
        SignalSeries::MinMax mm{s.at(first).value, s.at(first).value};
        for (size_t i = first; i < last; ++i) {
            mm.min = std::min(mm.min, s.at(i).value);
            mm.max = std::max(mm.max, s.at(i).value);
        }
        return mm;
    }

private slots:
    void minMaxMatchesBruteForce() {
        // Sizes on both sides of every level boundary (8, 64, 512, 4096).
        for (size_t n : {1u, 7u, 8u, 9u, 63u, 64u, 65u, 511u, 512u, 513u, 4095u, 4097u, 20000u}) {
            SignalHistoryStore store;
            quint32 seed = static_cast<quint32>(n);
            for (size_t i = 0; i < n; ++i) store.recordSample("M.s", "u", i * 0.01, noise(seed) * 100.0 - 50.0);
            const SignalSeries& s = *store.series("M.s");
            QCOMPARE(s.size(), n);

            const auto whole = s.minMax(0, n);
            QCOMPARE(whole.min, s.valueMin());
            QCOMPARE(whole.max, s.valueMax());

            for (int k = 0; k < 200; ++k) {
                const size_t a = std::min(n - 1, static_cast<size_t>(noise(seed) * n));
                const size_t b = std::min(n, a + 1 + static_cast<size_t>(noise(seed) * (n - a)));
                const auto got = s.minMax(a, b);
                const auto want = bruteMinMax(s, a, b);
                if (got.min != want.min || got.max != want.max) {
                    QFAIL(qPrintable(QString("n=%1 range [%2, %3)").arg(n).arg(a).arg(b)));
                }
            }
        }
    }

    void lowerBoundFindsFirstSampleAtOrAfter() {
        SignalHistoryStore store;
        for (int i = 0; i < 1000; ++i) store.recordSample("M.s", "u", i * 0.5, i);
        const SignalSeries& s = *store.series("M.s");
        QCOMPARE(s.lowerBound(-1.0), size_t(0));
        QCOMPARE(s.lowerBound(0.0), size_t(0));
        QCOMPARE(s.lowerBound(0.25), size_t(1));
        QCOMPARE(s.lowerBound(10.0), size_t(20));
        QCOMPARE(s.lowerBound(499.5), size_t(999));
        QCOMPARE(s.lowerBound(1e9), size_t(1000));
    }

    void decimateReturnsRawSamplesWhenFew() {
        SignalHistoryStore store;
        for (int i = 0; i < 100; ++i) store.recordSample("M.s", "u", i * 0.25, i * 2.0);
        const SignalSeries& s = *store.series("M.s");

        QList<QPointF> out;
        s.decimate(0.0, 25.0, 1000, &out);
        QCOMPARE(out.size(), 100);
        for (int i = 0; i < 100; ++i) QCOMPARE(out[i], QPointF(i * 0.25, i * 2.0));

        // A window inside the data gets one sample either side, so the line
        // reaches the plot's edges: samples 10..20 are in range, 9 and 21 added.
        s.decimate(2.4, 5.1, 1000, &out);
        QCOMPARE(out.size(), 13);
        QCOMPARE(out.first(), QPointF(2.25, 18.0));
        QCOMPARE(out.last(), QPointF(5.25, 42.0));
    }

    void decimateIsBoundedAndKeepsTheEnvelope() {
        SignalHistoryStore store;
        quint32 seed = 7;
        const int n = 300000;
        for (int i = 0; i < n; ++i) {
            store.recordSample("M.s", "u", i * 0.001, std::sin(i * 0.0005) * 100.0 + noise(seed));
        }
        const SignalSeries& s = *store.series("M.s");

        for (const auto& [tMin, tMax] : {std::pair{0.0, 300.0}, std::pair{12.5, 250.25}, std::pair{100.0, 103.0}}) {
            QList<QPointF> out;
            s.decimate(tMin, tMax, 1000, &out);
            QVERIFY(out.size() > 1000);
            QVERIFY2(out.size() <= 2000, qPrintable(QString::number(out.size())));

            const size_t first = s.lowerBound(out.first().x());
            const size_t last = s.lowerBound(out.last().x()) + 1;
            const auto want = bruteMinMax(s, first, last);
            double lo = out.first().y(), hi = lo;
            for (qsizetype i = 0; i < out.size(); ++i) {
                lo = std::min(lo, out[i].y());
                hi = std::max(hi, out[i].y());
                if (i > 0) QVERIFY(out[i].x() >= out[i - 1].x());
            }
            QCOMPARE(lo, want.min);
            QCOMPARE(hi, want.max);
        }
    }

    void decimateKeepsASingleSpike() {
        SignalHistoryStore store;
        for (int i = 0; i < 100000; ++i) store.recordSample("M.s", "u", i * 0.001, i == 54321 ? 99.0 : 0.0);
        QList<QPointF> out;
        store.series("M.s")->decimate(0.0, 100.0, 800, &out);
        const auto spike = std::find_if(out.begin(), out.end(), [](const QPointF& p) { return p.y() == 99.0; });
        QVERIFY(spike != out.end());
        QVERIFY(std::abs(spike->x() - 54.321) < 100.0 / 800);
    }

    void decimateHandlesEmptyAndDegenerateRanges() {
        SignalHistoryStore store;
        store.recordSample("M.s", "u", 1.0, 5.0);
        QList<QPointF> out{QPointF(1, 1)};
        store.series("M.s")->decimate(3.0, 3.0, 100, &out);
        QVERIFY(out.isEmpty());
        store.series("M.s")->decimate(0.0, 2.0, 0, &out);
        QVERIFY(out.isEmpty());
        store.series("M.s")->decimate(0.0, 2.0, 100, &out);
        QCOMPARE(out.size(), 1);
    }

    void outOfOrderTimesStayInOrder() {
        SignalHistoryStore store;
        store.recordSample("M.s", "u", 5.0, 1.0);
        store.recordSample("M.s", "u", 3.0, 2.0);
        store.recordSample("M.s", "u", 6.0, 3.0);
        const SignalSeries& s = *store.series("M.s");
        QCOMPARE(s.at(1).timeSec, 5.0);
        QCOMPARE(s.at(1).value, 2.0);
        QCOMPARE(s.at(2).timeSec, 6.0);
        QCOMPARE(store.latestTimeSec(), 6.0);
    }

    void memoryLimitCoarsensOldestDataOnly() {
        SignalHistoryStore store;
        const qint64 limit = 256 * 1024;
        store.setMemoryLimitBytes(limit);
        for (int i = 0; i < 100; ++i) store.recordSample("Small.s", "u", i, i);

        const int n = 100000; // 1.6 MB of raw samples, far over the limit
        for (int i = 0; i < n; ++i) store.recordSample("Big.s", "u", i * 0.001, i == 10 ? 1e6 : std::sin(i * 0.01));
        const SignalSeries& big = *store.series("Big.s");

        QVERIFY(store.coarsened());
        QVERIFY2(store.memoryBytes() <= limit + 64 * 1024, qPrintable(QString::number(store.memoryBytes())));
        QVERIFY(big.size() < static_cast<size_t>(n));
        QCOMPARE(store.series("Small.s")->size(), size_t(100));

        // The spike sat in the very oldest data, coarsened many times over.
        QCOMPARE(big.valueMax(), 1e6);
        QCOMPARE(big.minMax(0, big.size()).max, 1e6);
        QVERIFY(big.at(0).timeSec < 0.1); // and the history still reaches back to the start

        // The newest samples are untouched, and the whole thing is still sorted.
        for (int k = 0; k < 1000; ++k) QCOMPARE(big.at(big.size() - 1 - k).timeSec, (n - 1 - k) * 0.001);
        for (size_t i = 1; i < big.size(); ++i) QVERIFY(big.at(i).timeSec >= big.at(i - 1).timeSec);

        // Levels were rebuilt to match what is left.
        const auto want = bruteMinMax(big, 100, big.size() - 100);
        QCOMPARE(big.minMax(100, big.size() - 100).min, want.min);
        QCOMPARE(big.minMax(100, big.size() - 100).max, want.max);
    }

    void underTheLimitNothingIsCoarsened() {
        SignalHistoryStore store;
        for (int i = 0; i < 50000; ++i) store.recordSample("M.s", "u", i * 0.001, i);
        QVERIFY(!store.coarsened());
        QCOMPARE(store.series("M.s")->size(), size_t(50000));
        QVERIFY(store.memoryBytes() >= 50000 * 16);
    }

    void resetClearsEverything() {
        SignalHistoryStore store;
        QSignalSpy added(&store, &SignalHistoryStore::signalAdded);
        store.recordSample("M.s", "rpm", 1.0, 2.0);
        store.recordSample("M.s", "rpm", 2.0, 3.0);
        QCOMPARE(added.count(), 1);
        QCOMPARE(store.unitFor("M.s"), QString("rpm"));
        QCOMPARE(store.signalNames(), QStringList{"M.s"});
        const quint64 oldRevision = store.series("M.s")->revision();

        store.reset();
        QVERIFY(store.series("M.s") == nullptr);
        QVERIFY(store.signalNames().isEmpty());
        QCOMPARE(store.memoryBytes(), qint64(0));
        QCOMPARE(store.latestTimeSec(), 0.0);

        // A series recorded after a reset must never look like the old one.
        store.recordSample("M.s", "rpm", 1.0, 2.0);
        QCOMPARE(added.count(), 2);
        QVERIFY(store.series("M.s")->revision() > oldRevision);
    }
};

QTEST_APPLESS_MAIN(SignalHistoryTest)
#include "tst_signal_history.moc"
