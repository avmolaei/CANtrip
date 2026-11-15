// The gateway: which frames a rule forwards, and that two capture loops
// really pass frames to each other through it.
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

#include <QTemporaryDir>
#include <QtTest>

#include "../../common/GatewayRouter.h"
#include "../../common/PcapCapture.h"
#include "GatewayText.h"
#include "RuneFile.h"

using namespace cantrip;

namespace {

CanFrame frameWithId(uint32_t id) {
    CanFrame f;
    f.id = id;
    f.dlc = 8;
    for (int i = 0; i < 8; ++i) f.data[i] = static_cast<uint8_t>(0xA0 + i);
    return f;
}

// A channel whose received frames are fed by the test and whose transmitted
// frames are kept, both from other threads than the capture loop's.
class WireBackend : public IAvlabsCanBackend {
public:
    bool refuseTransmit = false;

    void receive(const CanFrame& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        incoming_.push_back(frame);
    }
    std::vector<CanFrame> transmitted() {
        std::lock_guard<std::mutex> lock(mutex_);
        return transmitted_;
    }

    std::string id() const override { return "wire"; }
    std::string displayName() const override { return "Wire"; }
    std::vector<CanChannelInfo> enumerateChannels() const override { return {{1, "wire", true}}; }
    bool initialize(uint64_t, const CanBitrateConfig&, bool, std::string*) override { return true; }
    void uninitialize(uint64_t) override {}
    bool readFrame(uint64_t, CanFrame* out, std::string*) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (incoming_.empty()) return false;
        *out = incoming_.front();
        incoming_.pop_front();
        return true;
    }
    bool writeFrame(uint64_t, const CanFrame& frame, std::string* error) override {
        if (refuseTransmit) {
            *error = "bus is listen only";
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        transmitted_.push_back(frame);
        return true;
    }

private:
    std::mutex mutex_;
    std::deque<CanFrame> incoming_;
    std::vector<CanFrame> transmitted_;
};

// Runs runBackendCapture for one bus in a thread until told to stop.
struct RunningBus {
    WireBackend backend;
    std::atomic<bool> stop{false};
    std::thread thread;
    FILE* out = nullptr;

    void start(const QString& pcapPath, GatewayRouter* gateway, int bus) {
        fopen_s(&out, pcapPath.toLocal8Bit().constData(), "wb");
        thread = std::thread([this, gateway, bus]() {
            std::string error;
            runBackendCapture(backend, 1, "wire", CanBitrateConfig{}, true, out, stop, {}, &error, gateway, bus);
        });
    }
    void finish() {
        stop = true;
        thread.join();
        fclose(out);
    }
};

} // namespace

class GatewayTest : public QObject {
    Q_OBJECT

private slots:
    void aRuleForwardsItsBusAndItsIdsOnly() {
        GatewayRouter router;
        router.attach(1);
        router.attach(2);
        router.setRules({{1, 2, 0x100, 0x1FF}});

        router.route(1, frameWithId(0x0FF)); // below the range
        router.route(1, frameWithId(0x100));
        router.route(1, frameWithId(0x1FF));
        router.route(1, frameWithId(0x200)); // above it
        router.route(2, frameWithId(0x150)); // right ID, wrong bus

        CanFrame out;
        int from = 0;
        QVERIFY(router.take(2, &out, &from));
        QCOMPARE(out.id, 0x100u);
        QCOMPARE(from, 1);
        QVERIFY(router.take(2, &out));
        QCOMPARE(out.id, 0x1FFu);
        QVERIFY(!router.take(2, &out));
        QVERIFY(!router.take(1, &out));
        QCOMPARE(router.forwardedCounts(), std::vector<uint64_t>({2}));
    }

    void noRulesMeansNothingMoves() {
        GatewayRouter router;
        router.attach(1);
        router.attach(2);
        QVERIFY(!router.active());
        router.route(1, frameWithId(0x100));
        CanFrame out;
        QVERIFY(!router.take(2, &out));
    }

    void busErrorsAreNotForwarded() {
        GatewayRouter router;
        router.attach(1);
        router.attach(2);
        router.setRules({{1, 2}});
        CanFrame error = frameWithId(CanErr::kProt);
        error.error = true;
        router.route(1, error);
        CanFrame out;
        QVERIFY(!router.take(2, &out));
    }

    void aFrameReachesADestinationOnceHoweverManyRulesMatch() {
        GatewayRouter router;
        for (int bus : {1, 2, 3}) router.attach(bus);
        router.setRules({{1, 2}, {1, 2, 0x100, 0x100}, {1, 3}});
        router.route(1, frameWithId(0x100));
        CanFrame out;
        QVERIFY(router.take(2, &out));
        QVERIFY(!router.take(2, &out));
        QVERIFY(router.take(3, &out));
    }

    void aRuleFromABusToItselfIsDropped() {
        GatewayRouter router;
        router.setRules({{2, 2}, {1, 2}});
        QCOMPARE(router.rules().size(), size_t(1));
        QCOMPARE(router.rules()[0].fromBus, 1);
    }

    void nothingQueuesForABusNobodyServes() {
        GatewayRouter router;
        router.attach(1);
        router.setRules({{1, 2}});
        for (int i = 0; i < 10000; ++i) router.route(1, frameWithId(0x100));
        QCOMPARE(router.dropped(), uint64_t(0));
        router.attach(2);
        CanFrame out;
        QVERIFY(!router.take(2, &out)); // what came before it attached is gone
    }

    void aDestinationThatStopsTransmittingLosesFramesInsteadOfMemory() {
        GatewayRouter router;
        router.attach(1);
        router.attach(2);
        router.setRules({{1, 2}});
        for (int i = 0; i < 5000; ++i) router.route(1, frameWithId(0x100));
        QCOMPARE(router.dropped(), uint64_t(5000 - 4096));
    }

    void twoWayRulesDoNotBounceAFrameBack() {
        // A frame forwarded to bus 2 is transmitted there, not received
        // there, so the 2 to 1 rule never sees it.
        QTemporaryDir dir;
        GatewayRouter router;
        router.setRules({{1, 2}, {2, 1}});
        RunningBus bus1, bus2;
        bus1.start(dir.filePath("1.pcap"), &router, 1);
        bus2.start(dir.filePath("2.pcap"), &router, 2);

        bus1.backend.receive(frameWithId(0x111));
        bus2.backend.receive(frameWithId(0x222));
        QTRY_COMPARE(bus2.backend.transmitted().size(), size_t(1));
        QTRY_COMPARE(bus1.backend.transmitted().size(), size_t(1));
        QTest::qWait(50); // time for a bounce to show, if there were one
        bus1.finish();
        bus2.finish();

        QCOMPARE(bus2.backend.transmitted().size(), size_t(1));
        QCOMPARE(bus2.backend.transmitted()[0].id, 0x111u);
        QCOMPARE(bus2.backend.transmitted()[0].data[7], uint8_t(0xA7));
        QCOMPARE(bus1.backend.transmitted().size(), size_t(1));
        QCOMPARE(bus1.backend.transmitted()[0].id, 0x222u);

        const std::vector<GatewaySent> sent = router.drainSent();
        QCOMPARE(sent.size(), size_t(2));
        QVERIFY(router.drainSent().empty());
    }

    void forwardingIsFast() {
        // The destination loop is woken by the router instead of finishing a
        // sleep, so a frame is out well inside one Windows timer tick.
        QTemporaryDir dir;
        GatewayRouter router;
        router.setRules({{1, 2}});
        RunningBus bus1, bus2;
        bus1.start(dir.filePath("1.pcap"), &router, 1);
        bus2.start(dir.filePath("2.pcap"), &router, 2);
        QTest::qWait(50);

        QElapsedTimer timer;
        qint64 worstUs = 0;
        for (size_t i = 1; i <= 20; ++i) {
            timer.start();
            bus1.backend.receive(frameWithId(0x100));
            while (bus2.backend.transmitted().size() < i && timer.elapsed() < 2000) std::this_thread::yield();
            worstUs = std::max(worstUs, timer.nsecsElapsed() / 1000);
            QTest::qWait(5);
        }
        bus1.finish();
        bus2.finish();
        QCOMPARE(bus2.backend.transmitted().size(), size_t(20));
        qInfo("worst forwarding delay: %lld us", static_cast<long long>(worstUs));
        QVERIFY2(worstUs < 40000, qPrintable(QString::number(worstUs)));
    }

    void aRefusedTransmitIsCountedWithItsReason() {
        QTemporaryDir dir;
        GatewayRouter router;
        router.setRules({{1, 2}});
        RunningBus bus1, bus2;
        bus2.backend.refuseTransmit = true;
        bus1.start(dir.filePath("1.pcap"), &router, 1);
        bus2.start(dir.filePath("2.pcap"), &router, 2);
        bus1.backend.receive(frameWithId(0x100));
        QTRY_COMPARE(router.failed(), uint64_t(1));
        bus1.finish();
        bus2.finish();
        QCOMPARE(router.lastError(), std::string("bus is listen only"));
        QVERIFY(router.drainSent().empty());
    }

    void sendPutsAFrameOnABusWithoutAnyRule() {
        // How Send Message reaches a bus beyond the first.
        QTemporaryDir dir;
        GatewayRouter router;
        QVERIFY(!router.send(2, frameWithId(0x700))); // nobody serves bus 2 yet
        RunningBus bus2;
        bus2.start(dir.filePath("2.pcap"), &router, 2);
        // Not QTRY_VERIFY: it evaluates its expression more than once.
        bool queued = false;
        for (int i = 0; i < 400 && !queued; ++i) {
            queued = router.send(2, frameWithId(0x701));
            if (!queued) QTest::qWait(5);
        }
        QVERIFY(queued);
        QTRY_COMPARE(bus2.backend.transmitted().size(), size_t(1));
        bus2.finish();
        QCOMPARE(bus2.backend.transmitted()[0].id, 0x701u);
        const std::vector<GatewaySent> sent = router.drainSent();
        QCOMPARE(sent.size(), size_t(1));
        QCOMPARE(sent[0].bus, 2);
        QCOMPARE(sent[0].fromBus, 0);
    }

    void idRangesReadAndPrintTheSameWay() {
        uint32_t low = 1, high = 1;
        QVERIFY(parseGatewayIdRange("", &low, &high));
        QCOMPARE(low, 0u);
        QCOMPARE(high, kGatewayMaxId);
        QVERIFY(parseGatewayIdRange("All", &low, &high));
        QVERIFY(parseGatewayIdRange(" 0x100 ", &low, &high));
        QCOMPARE(low, 0x100u);
        QCOMPARE(high, 0x100u);
        QVERIFY(parseGatewayIdRange("0x100 - 0x1ff", &low, &high));
        QCOMPARE(high, 0x1FFu);
        QVERIFY(parseGatewayIdRange("256-511", &low, &high));
        QCOMPARE(low, 256u);
        QVERIFY(!parseGatewayIdRange("0x200-0x100", &low, &high));
        QVERIFY(!parseGatewayIdRange("engine", &low, &high));
        QVERIFY(!parseGatewayIdRange("0x20000000", &low, &high));
        QVERIFY(!parseGatewayIdRange("1-2-3", &low, &high));

        QCOMPARE(gatewayIdRangeText({1, 2}), QString("all"));
        QCOMPARE(gatewayIdRangeText({1, 2, 0x100, 0x100}), QString("0x100"));
        QCOMPARE(gatewayIdRangeText({1, 2, 0x100, 0x1FF}), QString("0x100-0x1FF"));
    }

    void aForwardedFrameBecomesATxRowOnItsDestination() {
        GatewaySent sent;
        sent.bus = 2;
        sent.fromBus = 1;
        sent.frame = frameWithId(0x321);
        sent.frame.fd = true;
        sent.frame.dlc = 9; // the DLC code for 12 bytes

        const DecodedCanFrame row = decodedFromGatewaySent(sent, 9.97);
        QCOMPARE(row.bus, 2);
        QVERIFY(row.direction == FrameDirection::Tx);
        QCOMPARE(row.id, 0x321u);
        QCOMPARE(row.data.size(), 12);
        QCOMPARE(int(row.dlc), 12);
        QCOMPARE(row.timestamp, QString("9.970000"));
    }

    void aForwardedFrameIsTimedOnItsBusNotOnWhenTheWindowSawIt() {
        // tshark is two seconds behind at the start of a capture: bus 2's
        // first frame, read at session time 1.0, only shows up at 3.0.
        GatewayRouter router;
        router.attach(1);
        router.attach(2);
        router.setRules({{1, 2}});
        router.route(2, frameWithId(0x10)); // bus 2's clock starts now
        QTest::qWait(100);
        router.route(1, frameWithId(0x100));
        CanFrame out;
        int from = 0;
        QVERIFY(router.take(2, &out, &from));
        router.reportSent(2, from, out, true, {});

        BusTimeAligner aligner;
        GatewaySentPump pump;
        // Bus 2 has shown nothing yet, so the row waits.
        QVERIFY(pump.pump(router, aligner, 1.2).empty());

        aligner.align(2, 0.0, 3.0); // bus 2's first frame arrives: timeline 0
        const std::vector<DecodedCanFrame> rows = pump.pump(router, aligner, 3.0);
        QCOMPARE(rows.size(), size_t(1));
        // About 0.1 s after bus 2's first frame, whatever the session clock says.
        const double t = rows[0].timestamp.toDouble();
        QVERIFY2(t > 0.09 && t < 0.2, qPrintable(rows[0].timestamp));
    }

    void runesKeepTheRules() {
        QTemporaryDir dir;
        const QString path = dir.filePath("gateway.rune");
        RuneConfig config;
        config.graphWindows.push_back({});
        config.gatewayEnabled = true;
        config.gatewayRules = {{1, 2}, {3, 1, 0x100, 0x1FF}};
        QString error;
        QVERIFY2(saveRuneFile(path, config, &error), qPrintable(error));

        const std::optional<RuneConfig> loaded = loadRuneFile(path, &error);
        QVERIFY2(loaded.has_value(), qPrintable(error));
        QVERIFY(loaded->gatewayEnabled);
        QCOMPARE(loaded->gatewayRules.size(), size_t(2));
        QCOMPARE(loaded->gatewayRules[1].fromBus, 3);
        QCOMPARE(loaded->gatewayRules[1].toBus, 1);
        QCOMPARE(loaded->gatewayRules[1].idLow, 0x100u);
        QCOMPARE(loaded->gatewayRules[1].idHigh, 0x1FFu);
        QCOMPARE(loaded->gatewayRules[0].idHigh, kGatewayMaxId);
    }
};

QTEST_MAIN(GatewayTest)
#include "tst_gateway.moc"
