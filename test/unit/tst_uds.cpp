// UDS: the words put on requests and responses, the simulated ECU, and the
// client talking to that ECU through a real synthetic capture loop.
#include <atomic>
#include <thread>

#include <fcntl.h>
#include <io.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include "../../common/PcapCapture.h"
#include "../../common/SimulatedEcu.h"
#include "../../common/Uds.h"
#include "DiagnosticsView.h"
#include "RuneFile.h"
#include "UdsClient.h"

using namespace cantrip;

namespace {

using Bytes = std::vector<uint8_t>;

UdsClient::Target ecuTarget() {
    UdsClient::Target target;
    target.requestId = SimulatedEcu::kRequestId;
    target.responseId = SimulatedEcu::kResponseId;
    return target;
}

QByteArray q(std::initializer_list<int> values) {
    QByteArray out;
    for (int v : values) out.append(static_cast<char>(v));
    return out;
}

// The synthetic source running for real in a thread, with the client's
// frames going in through the router and the ECU's answers read back out of
// the pcap stream the way tshark would, minus tshark.
class SyntheticBus {
public:
    std::shared_ptr<GatewayRouter> router = std::make_shared<GatewayRouter>();

    bool start(UdsClient* client) {
        client_ = client;
        if (_pipe(fds_, 1 << 16, _O_BINARY) != 0) return false;
        FILE* out = _fdopen(fds_[1], "wb");
        capture_ = std::thread([this, out]() {
            writePcapHeader(out);
            runSyntheticCapture(out, stop_, router.get(), 1);
            fclose(out);
        });
        reader_ = std::thread([this]() { readLoop(); });
        return true;
    }
    void finish() {
        stop_ = true;
        capture_.join();
        reader_.join();
    }

private:
    bool readExact(char* buf, int count) {
        int got = 0;
        while (got < count) {
            const int n = _read(fds_[0], buf + got, static_cast<unsigned>(count - got));
            if (n <= 0) return false;
            got += n;
        }
        return true;
    }
    void readLoop() {
        char header[24];
        if (!readExact(header, 24)) return;
        while (true) {
            char record[16];
            if (!readExact(record, 16)) break;
            uint32_t length;
            memcpy(&length, record + 8, 4);
            QByteArray body(static_cast<qsizetype>(length), 0);
            if (!readExact(body.data(), static_cast<int>(length))) break;
            // SocketCAN layout: big-endian ID with flags on top, length, data from byte 8.
            const uint32_t word = qFromBigEndian<uint32_t>(body.constData());
            DecodedCanFrame frame;
            frame.error = word & 0x20000000u;
            frame.extended = word & 0x80000000u;
            frame.id = word & 0x1FFFFFFFu;
            frame.dlc = static_cast<uint8_t>(body[4]);
            frame.data = body.mid(8, frame.dlc);
            QMetaObject::invokeMethod(client_, [client = client_, frame]() { client->onFrame(frame); }, Qt::QueuedConnection);
        }
        _close(fds_[0]);
    }

    UdsClient* client_ = nullptr;
    int fds_[2] = {-1, -1};
    std::atomic<bool> stop_{false};
    std::thread capture_, reader_;
};

} // namespace

class UdsTest : public QObject {
    Q_OBJECT

private slots:
    // ---- words

    void requestsReadAsWhatTheyAsk() {
        QCOMPARE(udsDescribe({0x22, 0xF1, 0x90}), std::string("ReadDataByIdentifier 0xF190 (VIN)"));
        QCOMPARE(udsDescribe({0x22, 0x01, 0x00}), std::string("ReadDataByIdentifier 0x0100"));
        QCOMPARE(udsDescribe({0x10, 0x03}), std::string("DiagnosticSessionControl: extended session"));
        QCOMPARE(udsDescribe({0x11, 0x01}), std::string("ECUReset: hard reset"));
        QCOMPARE(udsDescribe({0x27, 0x01}), std::string("SecurityAccess: request seed, level 1"));
        QCOMPARE(udsDescribe({0x27, 0x02, 1, 2, 3, 4}), std::string("SecurityAccess: send key, level 1"));
        QCOMPARE(udsDescribe({0x19, 0x02, 0xFF}), std::string("ReadDTCInformation: codes matching status mask 0xFF"));
        QCOMPARE(udsDescribe({0x14, 0xFF, 0xFF, 0xFF}), std::string("ClearDiagnosticInformation: all codes"));
        QCOMPARE(udsDescribe({0x3E, 0x00}), std::string("TesterPresent"));
        QCOMPARE(udsDescribe({0x31, 0x01, 0xFF, 0x00}), std::string("RoutineControl: start routine 0xFF00"));
        QCOMPARE(udsDescribe({0xBA, 0x01}), std::string("service 0xBA: 01"));
    }

    void responsesReadAsWhatCameBack() {
        Bytes vin = {0x62, 0xF1, 0x90};
        for (char c : std::string("CANTR1PSYNTH00001")) vin.push_back(static_cast<uint8_t>(c));
        QCOMPARE(udsDescribe(vin), std::string("0xF190 (VIN) = \"CANTR1PSYNTH00001\""));
        QCOMPARE(udsDescribe({0x62, 0x01, 0x00, 0x03, 0x20}), std::string("0x0100 = 03 20"));
        QCOMPARE(udsDescribe({0x50, 0x03, 0x00, 0x32, 0x01, 0xF4}), std::string("Now in extended session"));
        QCOMPARE(udsDescribe({0x67, 0x01, 0xDE, 0xAD, 0xBE, 0xEF}), std::string("Seed: DE AD BE EF"));
        QCOMPARE(udsDescribe({0x67, 0x01, 0, 0, 0, 0}), std::string("Already unlocked (seed is zero)"));
        QCOMPARE(udsDescribe({0x67, 0x02}), std::string("Unlocked, level 1"));
        QCOMPARE(udsDescribe({0x54}), std::string("Trouble codes cleared"));
        QCOMPARE(udsDescribe({0x7F, 0x22, 0x31}), std::string("ReadDataByIdentifier refused: requestOutOfRange (0x31)"));
        QCOMPARE(udsDescribe({0x59, 0x02, 0xFF}), std::string("no trouble codes stored"));
        QCOMPARE(udsDescribe({0x59, 0x02, 0xFF, 0x03, 0x01, 0x00, 0x09, 0xC1, 0x00, 0x87, 0x08}),
                 std::string("2 trouble codes: P0301-00 [testFailed, confirmedDTC], U0100-87 [confirmedDTC]"));
    }

    void troubleCodesPrintLikeAScanTool() {
        QCOMPARE(udsDtcText(0x03, 0x01, 0x00), std::string("P0301-00"));
        QCOMPARE(udsDtcText(0x40, 0x35, 0x11), std::string("C0035-11"));
        QCOMPARE(udsDtcText(0x9A, 0xBC, 0x4D), std::string("B1ABC-4D"));
        QCOMPARE(udsDtcText(0xC1, 0x00, 0x87), std::string("U0100-87"));
        QCOMPARE(udsDtcStatusText(0x00), std::string("no status bits set"));
        QCOMPARE(udsDtcStatusText(0x24), std::string("pendingDTC, testFailedSinceLastClear"));
    }

    void answersAreMatchedToTheirRequest() {
        const Bytes request = {0x22, 0xF1, 0x90};
        QVERIFY(udsIsFinalResponseTo(request, {0x62, 0xF1, 0x90, 0x41}));
        QVERIFY(udsIsFinalResponseTo(request, {0x7F, 0x22, 0x31}));
        QVERIFY(!udsIsFinalResponseTo(request, {0x7F, 0x22, 0x78})); // still working on it
        QVERIFY(udsIsPendingResponseTo(request, {0x7F, 0x22, 0x78}));
        QVERIFY(!udsIsFinalResponseTo(request, {0x50, 0x03}));        // an answer to something else
        QVERIFY(!udsIsFinalResponseTo(request, {0x7F, 0x10, 0x12}));
        QVERIFY(udsSuppressesPositiveResponse({0x3E, 0x80}));
        QVERIFY(!udsSuppressesPositiveResponse({0x3E, 0x00}));
        QVERIFY(!udsSuppressesPositiveResponse({0x22, 0xF1, 0x90})); // F1 has the top bit set, but is no sub-function
    }

    void hexInputIsForgivingAboutSpacingOnly() {
        bool ok = false;
        QCOMPARE(DiagnosticsView::parseHex("22 F1 90", &ok), q({0x22, 0xF1, 0x90}));
        QVERIFY(ok);
        QCOMPARE(DiagnosticsView::parseHex("22f190", &ok), q({0x22, 0xF1, 0x90}));
        QCOMPARE(DiagnosticsView::parseHex("0x22, 0xF1, 0x90", &ok), q({0x22, 0xF1, 0x90}));
        DiagnosticsView::parseHex("22 F1 9", &ok);
        QVERIFY(!ok);
        DiagnosticsView::parseHex("read vin", &ok);
        QVERIFY(!ok);
        DiagnosticsView::parseHex("", &ok);
        QVERIFY(!ok);
    }

    // ---- the simulated ECU's services

    void ecuReadsIdentifiers() {
        SimulatedEcu ecu;
        const Bytes vin = ecu.handle({0x22, 0xF1, 0x90}, 0);
        QCOMPARE(vin.size(), size_t(20));
        QCOMPARE(std::string(vin.begin() + 3, vin.end()), std::string("CANTR1PSYNTH00001"));
        QCOMPARE(ecu.handle({0x22, 0xF1, 0x86}, 0), Bytes({0x62, 0xF1, 0x86, 0x01}));
        QCOMPARE(ecu.handle({0x22, 0xAB, 0xCD}, 0), Bytes({0x7F, 0x22, 0x31}));
        QCOMPARE(ecu.handle({0x22, 0xF1}, 0), Bytes({0x7F, 0x22, 0x13}));
        QVERIFY(ecu.handle({0x22, 0x01, 0x00}, 0) != ecu.handle({0x22, 0x01, 0x00}, 5000)); // engine speed moves
    }

    void ecuRefusesWhatItDoesNotKnow() {
        SimulatedEcu ecu;
        QCOMPARE(ecu.handle({0xBA}, 0), Bytes({0x7F, 0xBA, 0x11}));
        QCOMPARE(ecu.handle({0x10, 0x55}, 0), Bytes({0x7F, 0x10, 0x12}));
        QCOMPARE(ecu.handle({0x10}, 0), Bytes({0x7F, 0x10, 0x13}));
    }

    void ecuSessionsLapseWithoutTesterPresent() {
        SimulatedEcu ecu;
        QCOMPARE(ecu.handle({0x10, 0x03}, 0), Bytes({0x50, 0x03, 0x00, 0x32, 0x01, 0xF4}));
        QCOMPARE(ecu.handle({0x22, 0xF1, 0x86}, 4000), Bytes({0x62, 0xF1, 0x86, 0x03}));
        QVERIFY(ecu.handle({0x3E, 0x80}, 8000).empty()); // kept alive, silently
        QCOMPARE(ecu.handle({0x22, 0xF1, 0x86}, 12000), Bytes({0x62, 0xF1, 0x86, 0x03}));
        QCOMPARE(ecu.handle({0x22, 0xF1, 0x86}, 17001), Bytes({0x62, 0xF1, 0x86, 0x01})); // 5 s of silence
    }

    void ecuUnlocksWithTheRightKeyOnly() {
        SimulatedEcu ecu;
        QCOMPARE(ecu.handle({0x27, 0x01}, 0), Bytes({0x7F, 0x27, 0x7F})); // not in the default session
        ecu.handle({0x10, 0x03}, 0);
        QCOMPARE(ecu.handle({0x27, 0x02, 1, 2, 3, 4}, 1), Bytes({0x7F, 0x27, 0x24})); // key before seed

        Bytes seed = ecu.handle({0x27, 0x01}, 2);
        QCOMPARE(seed.size(), size_t(6));
        QCOMPARE(ecu.handle({0x27, 0x02, 0, 0, 0, 0}, 3), Bytes({0x7F, 0x27, 0x35}));
        QCOMPARE(ecu.handle({0x27, 0x02, 0, 0, 0, 0}, 4), Bytes({0x7F, 0x27, 0x24})); // a wrong key spends the seed

        seed = ecu.handle({0x27, 0x01}, 5);
        const uint32_t value = static_cast<uint32_t>(seed[2]) << 24 | static_cast<uint32_t>(seed[3]) << 16 |
                               static_cast<uint32_t>(seed[4]) << 8 | seed[5];
        const uint32_t key = value ^ SimulatedEcu::kKeyMask;
        QCOMPARE(ecu.handle({0x27, 0x02, uint8_t(key >> 24), uint8_t(key >> 16), uint8_t(key >> 8), uint8_t(key)}, 6),
                 Bytes({0x67, 0x02}));
        QCOMPARE(ecu.handle({0x27, 0x01}, 7), Bytes({0x67, 0x01, 0, 0, 0, 0})); // already unlocked
    }

    void ecuWritesTheVinOnlyWhenAllowed() {
        SimulatedEcu ecu;
        Bytes write = {0x2E, 0xF1, 0x90};
        for (char c : std::string("WVWZZZ1JZXW000001")) write.push_back(static_cast<uint8_t>(c));
        QCOMPARE(ecu.handle(write, 0), Bytes({0x7F, 0x2E, 0x7F})); // default session
        ecu.handle({0x10, 0x03}, 0);
        QCOMPARE(ecu.handle(write, 1), Bytes({0x7F, 0x2E, 0x33})); // locked
        const Bytes seed = ecu.handle({0x27, 0x01}, 2);
        const uint32_t key = (static_cast<uint32_t>(seed[2]) << 24 | static_cast<uint32_t>(seed[3]) << 16 |
                              static_cast<uint32_t>(seed[4]) << 8 | seed[5]) ^ SimulatedEcu::kKeyMask;
        ecu.handle({0x27, 0x02, uint8_t(key >> 24), uint8_t(key >> 16), uint8_t(key >> 8), uint8_t(key)}, 3);
        Bytes tooShort(write.begin(), write.end() - 1);
        QCOMPARE(ecu.handle(tooShort, 4), Bytes({0x7F, 0x2E, 0x13}));
        QCOMPARE(ecu.handle(write, 5), Bytes({0x6E, 0xF1, 0x90}));
        const Bytes vin = ecu.handle({0x22, 0xF1, 0x90}, 6);
        QCOMPARE(std::string(vin.begin() + 3, vin.end()), std::string("WVWZZZ1JZXW000001"));
    }

    void ecuTroubleCodesCanBeReadClearedAndComeBackOnReset() {
        SimulatedEcu ecu;
        QCOMPARE(ecu.handle({0x19, 0x02, 0xFF}, 0).size(), size_t(3 + 3 * 4));
        QCOMPARE(ecu.handle({0x19, 0x02, 0x08}, 0).size(), size_t(3 + 2 * 4)); // only the confirmed ones
        QCOMPARE(ecu.handle({0x19, 0x01, 0xFF}, 0), Bytes({0x59, 0x01, 0xFF, 0x01, 0x00, 0x03}));
        QCOMPARE(ecu.handle({0x14, 0xFF, 0xFF, 0xFF}, 0), Bytes({0x54}));
        QCOMPARE(ecu.handle({0x19, 0x02, 0xFF}, 0), Bytes({0x59, 0x02, 0xFF}));
        QCOMPARE(ecu.handle({0x11, 0x01}, 0), Bytes({0x51, 0x01}));
        QCOMPARE(ecu.handle({0x19, 0x02, 0xFF}, 0).size(), size_t(3 + 3 * 4));
    }

    // ---- the client against the ECU, through the synthetic capture loop

    void clientReadsTheVinAcrossSeveralFrames() {
        UdsClient client;
        SyntheticBus bus;
        client.setTransmitter(bus.router);
        client.setTarget(ecuTarget());
        QSignalSpy responses(&client, &UdsClient::responseReceived);
        QSignalSpy failures(&client, &UdsClient::failed);
        QVERIFY(bus.start(&client));

        QString error;
        QTRY_VERIFY2(client.busy() || client.request(q({0x22, 0xF1, 0x90}), &error), qPrintable(error));
        QVERIFY(!client.request(q({0x3E, 0x00}), &error)); // one at a time
        QTRY_COMPARE(responses.size(), 1);
        bus.finish();

        QCOMPARE(failures.size(), 0);
        QCOMPARE(responses[0][0].toByteArray(), q({0x22, 0xF1, 0x90}));
        const QByteArray response = responses[0][1].toByteArray();
        QCOMPARE(response.size(), 20); // 3 frames' worth: the client had to send flow control
        QCOMPARE(response.mid(3), QByteArray("CANTR1PSYNTH00001"));
        QVERIFY(!client.busy());
    }

    void clientGetsNegativeResponsesAsAnswers() {
        UdsClient client;
        SyntheticBus bus;
        client.setTransmitter(bus.router);
        client.setTarget(ecuTarget());
        QSignalSpy responses(&client, &UdsClient::responseReceived);
        QVERIFY(bus.start(&client));
        QString error;
        QTRY_VERIFY(client.busy() || client.request(q({0x22, 0xAB, 0xCD}), &error));
        QTRY_COMPARE(responses.size(), 1);
        bus.finish();
        QCOMPARE(responses[0][1].toByteArray(), q({0x7F, 0x22, 0x31}));
    }

    void clientWaitsThroughResponsePending() {
        UdsClient client;
        SyntheticBus bus;
        client.setTransmitter(bus.router);
        client.setTarget(ecuTarget());
        QSignalSpy pending(&client, &UdsClient::responsePending);
        QSignalSpy responses(&client, &UdsClient::responseReceived);
        QVERIFY(bus.start(&client));
        QString error;
        QTRY_VERIFY(client.busy() || client.request(q({0x31, 0x01, 0xFF, 0x00}), &error));
        QTRY_COMPARE(responses.size(), 1);
        bus.finish();
        QCOMPARE(pending.size(), 1);
        QCOMPARE(responses[0][1].toByteArray(), q({0x71, 0x01, 0xFF, 0x00}));
    }

    void clientSendsALongRequestAndTheEcuTakesIt() {
        // Writing the VIN is 20 bytes: the client sends a first frame, waits
        // for the ECU's flow control, then the rest.
        UdsClient client;
        SyntheticBus bus;
        client.setTransmitter(bus.router);
        client.setTarget(ecuTarget());
        QSignalSpy responses(&client, &UdsClient::responseReceived);
        QVERIFY(bus.start(&client));
        QString error;
        QByteArray write = q({0x2E, 0xF1, 0x90}) + "WVWZZZ1JZXW000001";
        QTRY_VERIFY(client.busy() || client.request(write, &error));
        QTRY_COMPARE(responses.size(), 1);
        bus.finish();
        // Refused, since nothing was unlocked, but refused by the service:
        // all 20 bytes arrived for it to look at.
        QCOMPARE(responses[0][1].toByteArray(), q({0x7F, 0x2E, 0x7F}));
    }

    void clientReportsAnEcuThatIsNotThere() {
        UdsClient client;
        SyntheticBus bus;
        client.setTransmitter(bus.router);
        client.setTarget(ecuTarget());
        UdsClient::Target target;
        target.requestId = 0x700; // nobody listens here
        target.responseId = 0x708;
        client.setTarget(target);
        QSignalSpy failures(&client, &UdsClient::failed);
        QVERIFY(bus.start(&client));
        QString error;
        QTRY_VERIFY(client.busy() || client.request(q({0x3E, 0x00}), &error));
        QTRY_COMPARE_WITH_TIMEOUT(failures.size(), 1, 3000);
        bus.finish();
        QCOMPARE(failures[0][1].toString(), QString("No answer from the ECU."));
        QVERIFY(!client.busy());
    }

    void clientSaysSoWhenNothingIsCapturing() {
        UdsClient client;
        client.setTransmitter(std::make_shared<GatewayRouter>());
        QString error;
        QVERIFY(!client.request(q({0x3E, 0x00}), &error));
        QCOMPARE(error, QString("CAN 1 is not being captured. Start a capture on it first."));
        QVERIFY(!client.busy());
    }

    void runesRememberTheEcu() {
        QTemporaryDir dir;
        const QString path = dir.filePath("diag.rune");
        RuneConfig config;
        config.graphWindows.push_back({});
        config.diagBus = 2;
        config.diagRequestId = 0x18DA10F1;
        config.diagResponseId = 0x18DAF110;
        config.diagExtended = true;
        QString error;
        QVERIFY2(saveRuneFile(path, config, &error), qPrintable(error));
        const std::optional<RuneConfig> loaded = loadRuneFile(path, &error);
        QVERIFY2(loaded.has_value(), qPrintable(error));
        QCOMPARE(loaded->diagBus, 2);
        QCOMPARE(loaded->diagRequestId, 0x18DA10F1u);
        QCOMPARE(loaded->diagResponseId, 0x18DAF110u);
        QVERIFY(loaded->diagExtended);
    }
};

QTEST_MAIN(UdsTest)
#include "tst_uds.moc"
