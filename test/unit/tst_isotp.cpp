// ISO-TP: messages split into CAN frames and put back together, with a
// tester channel and an ECU channel wired to each other.
#include <QtTest>

#include "../../common/IsoTp.h"

using namespace cantrip;

namespace {

std::vector<uint8_t> counting(size_t size) {
    std::vector<uint8_t> out(size);
    for (size_t i = 0; i < size; ++i) out[i] = static_cast<uint8_t>(i * 7 + 1);
    return out;
}

QByteArray hex(const CanFrame& frame) {
    return QByteArray(reinterpret_cast<const char*>(frame.data), frame.dlc).toHex();
}

// Two channels facing each other. run() carries frames back and forth until
// both are quiet, advancing the clock a millisecond per round.
struct Link {
    IsoTpChannel tester;
    IsoTpChannel ecu;
    uint64_t nowMs = 0;
    std::vector<CanFrame> testerSent, ecuSent;

    Link() {
        IsoTpConfig t, e;
        t.txId = e.rxId = 0x7E0;
        t.rxId = e.txId = 0x7E8;
        tester.setConfig(t);
        ecu.setConfig(e);
    }
    void configureEcu(uint8_t blockSize, uint8_t stMinMs) {
        IsoTpConfig e = ecu.config();
        e.blockSize = blockSize;
        e.stMinMs = stMinMs;
        ecu.setConfig(e);
    }
    void run(int rounds = 2000) {
        for (int i = 0; i < rounds; ++i) {
            tester.poll(nowMs);
            ecu.poll(nowMs);
            const std::vector<CanFrame> fromTester = tester.takeFrames();
            const std::vector<CanFrame> fromEcu = ecu.takeFrames();
            for (const CanFrame& f : fromTester) { testerSent.push_back(f); ecu.onFrame(f, nowMs); }
            for (const CanFrame& f : fromEcu) { ecuSent.push_back(f); tester.onFrame(f, nowMs); }
            if (fromTester.empty() && fromEcu.empty() && !tester.sending() && !ecu.sending()) return;
            ++nowMs;
        }
    }
};

} // namespace

class IsoTpTest : public QObject {
    Q_OBJECT

private slots:
    void aShortMessageIsOnePaddedFrame() {
        IsoTpChannel channel;
        QVERIFY(channel.send({0x22, 0xF1, 0x90}, 0));
        const std::vector<CanFrame> frames = channel.takeFrames();
        QCOMPARE(frames.size(), size_t(1));
        QCOMPARE(frames[0].id, 0x7E0u);
        QCOMPARE(hex(frames[0]), QByteArray("0322f190cccccccc"));
        QVERIFY(!channel.sending());
    }

    void paddingCanBeTurnedOff() {
        IsoTpConfig config;
        config.padding = false;
        IsoTpChannel channel(config);
        channel.send({0x3E, 0x00}, 0);
        QCOMPARE(hex(channel.takeFrames()[0]), QByteArray("023e00"));
    }

    void sevenBytesStillFitOneFrameEightDoNot() {
        IsoTpChannel channel;
        channel.send(counting(7), 0);
        QCOMPARE(channel.takeFrames().size(), size_t(1));
        QVERIFY(!channel.sending());
        channel.send(counting(8), 0);
        const std::vector<CanFrame> frames = channel.takeFrames();
        QCOMPARE(frames.size(), size_t(1));
        QCOMPARE(hex(frames[0]).left(4), QByteArray("1008")); // first frame, 8 bytes in all
        QVERIFY(channel.sending()); // waiting for flow control
    }

    void emptyAndOversizedMessagesAreRefused() {
        IsoTpChannel channel;
        QVERIFY(!channel.send({}, 0));
        QVERIFY(!channel.send(counting(4096), 0));
        QVERIFY(channel.send(counting(4095), 0));
        QVERIFY(!channel.send({0x3E, 0x00}, 0)); // one message at a time
    }

    void aLongMessageCrossesInBothDirections() {
        Link link;
        const std::vector<uint8_t> request = counting(20);
        QVERIFY(link.tester.send(request, link.nowMs));
        link.run();
        std::vector<std::vector<uint8_t>> got = link.ecu.takeMessages();
        QCOMPARE(got.size(), size_t(1));
        QCOMPARE(got[0], request);
        // First frame, then the ECU's flow control, then two consecutive frames.
        QCOMPARE(link.testerSent.size(), size_t(3));
        QCOMPARE(hex(link.testerSent[0]).left(4), QByteArray("1014"));
        QCOMPARE(hex(link.testerSent[1]).left(2), QByteArray("21"));
        QCOMPARE(hex(link.testerSent[2]).left(2), QByteArray("22"));
        QCOMPARE(link.ecuSent.size(), size_t(1));
        QCOMPARE(hex(link.ecuSent[0]), QByteArray("300000cccccccccc"));

        const std::vector<uint8_t> response = counting(300);
        QVERIFY(link.ecu.send(response, link.nowMs));
        link.run();
        got = link.tester.takeMessages();
        QCOMPARE(got.size(), size_t(1));
        QCOMPARE(got[0], response);
        QVERIFY(link.tester.takeErrors().empty());
        QVERIFY(link.ecu.takeErrors().empty());
    }

    void theLongestMessageArrivesIntact() {
        // 4095 bytes wrap the 4-bit sequence number many times over.
        Link link;
        const std::vector<uint8_t> payload = counting(4095);
        link.tester.send(payload, 0);
        link.run();
        const std::vector<std::vector<uint8_t>> got = link.ecu.takeMessages();
        QCOMPARE(got.size(), size_t(1));
        QCOMPARE(got[0], payload);
    }

    void blockSizeMakesTheSenderStopAndAsk() {
        Link link;
        link.configureEcu(/*blockSize=*/2, /*stMinMs=*/0);
        const std::vector<uint8_t> payload = counting(6 + 7 * 5); // first frame + 5 consecutive
        link.tester.send(payload, 0);
        link.run();
        QCOMPARE(link.ecu.takeMessages()[0], payload);
        // One flow control at the start, and one after each full block of 2
        // while more is to come: after frames 2 and 4.
        QCOMPARE(link.ecuSent.size(), size_t(3));
        QCOMPARE(hex(link.ecuSent[0]).left(6), QByteArray("300200"));
    }

    void theSenderKeepsTheGapItWasAskedFor() {
        Link link;
        link.configureEcu(0, /*stMinMs=*/10);
        link.tester.send(counting(6 + 7 * 4), 0);
        link.run();
        QCOMPARE(link.ecu.takeMessages().size(), size_t(1));
        // Four consecutive frames, the last three each 10 ms after the one before.
        QVERIFY2(link.nowMs >= 30, qPrintable(QString::number(link.nowMs)));
        QVERIFY2(link.nowMs < 45, qPrintable(QString::number(link.nowMs)));
    }

    void noFlowControlEndsTheTransfer() {
        IsoTpChannel channel;
        channel.send(counting(20), 0);
        channel.takeFrames();
        channel.poll(999);
        QVERIFY(channel.sending());
        channel.poll(1000);
        QVERIFY(!channel.sending());
        QCOMPARE(channel.takeErrors(), std::vector<std::string>({"no flow control frame came back"}));
        QVERIFY(channel.send({0x3E, 0x00}, 1001)); // and it can be used again
    }

    void overflowFromTheOtherSideEndsTheTransfer() {
        IsoTpChannel channel;
        channel.send(counting(20), 0);
        CanFrame fc;
        fc.id = 0x7E8;
        fc.dlc = 3;
        fc.data[0] = 0x32;
        channel.onFrame(fc, 1);
        QVERIFY(!channel.sending());
        QCOMPARE(channel.takeErrors().size(), size_t(1));
    }

    void waitFramesBuyTimeButNotForever() {
        IsoTpChannel channel;
        channel.send(counting(20), 0);
        CanFrame wait;
        wait.id = 0x7E8;
        wait.dlc = 3;
        wait.data[0] = 0x31;
        for (int i = 1; i <= 10; ++i) {
            channel.onFrame(wait, static_cast<uint64_t>(i) * 900); // each within the timeout of the last
            channel.poll(static_cast<uint64_t>(i) * 900);
            QVERIFY(channel.sending());
        }
        channel.onFrame(wait, 9900);
        QVERIFY(!channel.sending());
    }

    void aMessageThatStopsArrivingIsGivenUp() {
        Link link;
        link.ecu.send(counting(50), 0);
        // Only the first frame reaches the tester.
        link.tester.onFrame(link.ecu.takeFrames()[0], 0);
        QVERIFY(link.tester.receiving());
        link.tester.poll(1000);
        QVERIFY(!link.tester.receiving());
        QCOMPARE(link.tester.takeErrors(), std::vector<std::string>({"the message stopped arriving partway through"}));
        QVERIFY(link.tester.takeMessages().empty());
    }

    void aSkippedFrameIsNoticed() {
        Link link;
        link.ecu.send(counting(30), 0);
        link.tester.onFrame(link.ecu.takeFrames()[0], 0);
        for (const CanFrame& fc : link.tester.takeFrames()) link.ecu.onFrame(fc, 0);
        const std::vector<CanFrame> consecutive = link.ecu.takeFrames();
        QVERIFY(consecutive.size() >= 3);
        link.tester.onFrame(consecutive[0], 1);
        link.tester.onFrame(consecutive[2], 2); // the second one never came
        QVERIFY(!link.tester.receiving());
        QCOMPARE(link.tester.takeErrors(), std::vector<std::string>({"a consecutive frame arrived out of order"}));
    }

    void otherTrafficIsIgnored() {
        IsoTpChannel channel;
        CanFrame f;
        f.dlc = 8;
        f.data[0] = 0x02;
        f.data[1] = 0x7E;
        f.id = 0x123; // not our response ID
        channel.onFrame(f, 0);
        f.id = 0x7E8;
        f.extended = true; // right number, wrong kind of ID
        channel.onFrame(f, 0);
        f.extended = false;
        f.fd = true;
        channel.onFrame(f, 0);
        f.fd = false;
        f.error = true;
        channel.onFrame(f, 0);
        QVERIFY(channel.takeMessages().empty());
        f.error = false;
        channel.onFrame(f, 0);
        QCOMPARE(channel.takeMessages().size(), size_t(1));
    }

    void extendedIdsWork() {
        IsoTpConfig config;
        config.txId = 0x18DA10F1;
        config.rxId = 0x18DAF110;
        config.extended = true;
        IsoTpChannel channel(config);
        channel.send({0x3E, 0x00}, 0);
        const CanFrame sent = channel.takeFrames()[0];
        QCOMPARE(sent.id, 0x18DA10F1u);
        QVERIFY(sent.extended);
    }

    void malformedSingleFramesAreDropped() {
        IsoTpChannel channel;
        CanFrame f;
        f.id = 0x7E8;
        f.dlc = 3;
        f.data[0] = 0x05; // claims 5 bytes, frame only has 2 after the first
        channel.onFrame(f, 0);
        f.data[0] = 0x00; // claims none
        channel.onFrame(f, 0);
        QVERIFY(channel.takeMessages().empty());
        QVERIFY(channel.takeErrors().empty());
    }
};

QTEST_APPLESS_MAIN(IsoTpTest)
#include "tst_isotp.moc"
