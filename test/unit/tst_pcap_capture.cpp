#include <QtTest>

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "../../common/PcapCapture.h"

using namespace cantrip;

namespace {

// Stands in for a vendor backend, so the capture loop can be driven without
// hardware: hands out queued frames and records what was asked of it.
class FakeBackend : public IAvlabsCanBackend {
public:
    std::string backendId = "fake";
    std::deque<CanFrame> incoming;
    bool endless = false;       // never run out of frames
    bool failInitialize = false;
    std::atomic<bool>* stopWhenDrained = nullptr;
    std::atomic<int> initialized{0};
    std::atomic<int> uninitialized{0};
    std::atomic<int> transmitted{0};
    bool lastRequestOwnership = false;
    CanFrame lastTransmitted;

    std::string id() const override { return backendId; }
    std::string displayName() const override { return "Fake backend"; }
    std::vector<CanChannelInfo> enumerateChannels() const override { return {{1, "PCAN-USB FD", true}}; }

    bool initialize(uint64_t, const CanBitrateConfig&, bool requestOwnership, std::string* error) override {
        lastRequestOwnership = requestOwnership;
        if (failInitialize) {
            *error = "channel is busy";
            return false;
        }
        ++initialized;
        return true;
    }
    void uninitialize(uint64_t) override { ++uninitialized; }

    bool readFrame(uint64_t, CanFrame* out, std::string*) override {
        if (endless) {
            *out = CanFrame{};
            out->id = 0x123;
            out->dlc = 8;
            return true;
        }
        if (incoming.empty()) {
            if (stopWhenDrained) *stopWhenDrained = true;
            return false;
        }
        *out = incoming.front();
        incoming.pop_front();
        return true;
    }
    bool writeFrame(uint64_t, const CanFrame& frame, std::string*) override {
        lastTransmitted = frame;
        ++transmitted;
        return true;
    }
};

CanFrame classicFrame(uint32_t id, uint64_t timestampUs) {
    CanFrame f;
    f.id = id;
    f.dlc = 8;
    for (int i = 0; i < 8; ++i) f.data[i] = static_cast<uint8_t>(0x10 + i);
    f.timestampUs = timestampUs;
    return f;
}

uint32_t readLe32(const QByteArray& bytes, int offset) {
    uint32_t v;
    memcpy(&v, bytes.constData() + offset, 4);
    return v;
}

} // namespace

class PcapCaptureTest : public QObject {
    Q_OBJECT

private slots:
    void interfaceIdsAreSafeForCommandLines() {
        FakeBackend backend;
        QCOMPARE(interfaceIdFor(backend, {1, "PCAN-USB FD", true}), std::string("fake_pcan_usb_fd"));
        QCOMPARE(interfaceIdFor(backend, {1, "VN1640A Channel 1", true}), std::string("fake_vn1640a_channel_1"));
        QCOMPARE(interfaceIdFor(backend, {1, "  odd -- name!! ", true}), std::string("fake_odd_name"));
    }

    void classicFramesSerializeAsSocketCan() {
        uint8_t buf[72] = {};
        QCOMPARE(serializeFrame(classicFrame(0x123, 0), buf), size_t(16));
        QCOMPARE(QByteArray(reinterpret_cast<char*>(buf), 16).toHex(), QByteArray("0000012308000000" "1011121314151617"));

        CanFrame extended = classicFrame(0x18DAF110, 0);
        extended.extended = true;
        serializeFrame(extended, buf);
        QCOMPARE(QByteArray(reinterpret_cast<char*>(buf), 4).toHex(), QByteArray("98daf110")); // EFF flag set

        CanFrame remote = classicFrame(0x7FF, 0);
        remote.rtr = true;
        serializeFrame(remote, buf);
        QCOMPARE(QByteArray(reinterpret_cast<char*>(buf), 4).toHex(), QByteArray("400007ff")); // RTR flag set
    }

    void fdFramesCarryTheirRealLength() {
        CanFrame fd = classicFrame(0x321, 0);
        fd.fd = true;
        fd.brs = true;
        fd.esi = true;
        fd.dlc = 9; // the DLC code for 12 bytes
        uint8_t buf[72] = {};
        QCOMPARE(serializeFrame(fd, buf), size_t(72));
        QCOMPARE(buf[4], uint8_t(12));
        QCOMPARE(buf[5], uint8_t(0x03)); // BRS | ESI
        QCOMPARE(buf[8], uint8_t(0x10));

        fd.dlc = 15;
        serializeFrame(fd, buf);
        QCOMPARE(buf[4], uint8_t(64));
    }

    void busErrorsSerializeAsErrorFrames() {
        CanFrame error;
        error.error = true;
        error.id = CanErr::kProt;
        error.dlc = 8;
        error.data[2] = CanErr::kProtStuff;
        uint8_t buf[72] = {};
        QCOMPARE(serializeFrame(error, buf), size_t(16));
        QCOMPARE(QByteArray(reinterpret_cast<char*>(buf), 4).toHex(), QByteArray("20000008")); // ERR flag + class
        QCOMPARE(buf[10], uint8_t(CanErr::kProtStuff));
    }

    void captureWritesEveryFrameThenReleasesTheChannel() {
        QTemporaryDir dir;
        const QString path = dir.filePath("capture.pcap");
        FILE* out = nullptr;
        QCOMPARE(fopen_s(&out, path.toLocal8Bit().constData(), "wb"), 0);

        std::atomic<bool> stop{false};
        FakeBackend backend;
        backend.stopWhenDrained = &stop;
        backend.incoming.push_back(classicFrame(0x100, 1500000));
        CanFrame fd = classicFrame(0x200, 2000001);
        fd.fd = true;
        fd.dlc = 9;
        backend.incoming.push_back(fd);
        CanFrame error;
        error.error = true;
        error.id = CanErr::kProt;
        error.timestampUs = 3000000;
        backend.incoming.push_back(error);

        QVERIFY(writePcapHeader(out));
        std::string error2;
        QVERIFY(runBackendCapture(backend, 1, "fake_ch", CanBitrateConfig{}, /*requestOwnership=*/false, out, stop, {}, &error2));
        fclose(out);

        QCOMPARE(backend.initialized.load(), 1);
        QCOMPARE(backend.uninitialized.load(), 1);
        QCOMPARE(backend.lastRequestOwnership, false);

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QCOMPARE(bytes.size(), 24 + (16 + 16) + (16 + 72) + (16 + 16));
        QCOMPARE(readLe32(bytes, 0), 0xa1b2c3d4u);
        QCOMPARE(readLe32(bytes, 20), uint32_t(kPcapLinktypeCanSocketcan));
        // First record: 1.5 s, 16 bytes.
        QCOMPARE(readLe32(bytes, 24), 1u);
        QCOMPARE(readLe32(bytes, 28), 500000u);
        QCOMPARE(readLe32(bytes, 32), 16u);
        // Second record starts after it: 2.000001 s, 72 bytes.
        QCOMPARE(readLe32(bytes, 56), 2u);
        QCOMPARE(readLe32(bytes, 60), 1u);
        QCOMPARE(readLe32(bytes, 64), 72u);
    }

    void aChannelThatWillNotOpenIsReported() {
        QTemporaryDir dir;
        FILE* out = nullptr;
        QCOMPARE(fopen_s(&out, dir.filePath("capture.pcap").toLocal8Bit().constData(), "wb"), 0);
        std::atomic<bool> stop{false};
        FakeBackend backend;
        backend.failInitialize = true;
        std::string error;
        QVERIFY(!runBackendCapture(backend, 1, "fake_ch", CanBitrateConfig{}, true, out, stop, {}, &error));
        fclose(out);
        QCOMPARE(error, std::string("initialize failed: channel is busy"));
        QCOMPARE(backend.uninitialized.load(), 0);
    }

    void captureEndsAndReleasesTheChannelWhenTheReaderGoesAway() {
        // What happens when tshark exits or is killed mid-capture.
        int fds[2];
        QCOMPARE(_pipe(fds, 4096, _O_BINARY), 0);
        _close(fds[0]); // nobody will ever read
        FILE* out = _fdopen(fds[1], "wb");
        QVERIFY(out != nullptr);

        std::atomic<bool> stop{false}; // never set
        FakeBackend backend;
        backend.endless = true;
        std::string error;
        QVERIFY(runBackendCapture(backend, 1, "fake_ch", CanBitrateConfig{}, true, out, stop, {}, &error));
        fclose(out);
        QCOMPARE(backend.uninitialized.load(), 1);
    }

    void peakTransmitsWhatArrivesOnItsPipe() {
        // PEAK can only transmit from the thread that holds the channel, so
        // MessageSender hands its frames to the capture loop through a pipe.
        QTemporaryDir dir;
        FILE* out = nullptr;
        QCOMPARE(fopen_s(&out, dir.filePath("capture.pcap").toLocal8Bit().constData(), "wb"), 0);

        std::atomic<bool> stop{false};
        FakeBackend backend;
        backend.backendId = "peak";
        const std::string interfaceId = "peak_unit_test_" + std::to_string(GetCurrentProcessId());
        std::thread capture([&]() {
            std::string error;
            runBackendCapture(backend, 1, interfaceId, CanBitrateConfig{}, true, out, stop, {}, &error);
        });

        const std::string pipeName = "\\\\.\\pipe\\cantrip_tx_" + interfaceId;
        HANDLE client = INVALID_HANDLE_VALUE;
        QVERIFY(QTest::qWaitFor([&]() {
            client = CreateFileA(pipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            return client != INVALID_HANDLE_VALUE;
        }, 3000));

        const CanFrame sent = classicFrame(0x7E0, 0);
        DWORD written = 0;
        QVERIFY(WriteFile(client, &sent, sizeof(sent), &written, nullptr));
        QCOMPARE(written, DWORD(sizeof(sent)));
        const bool transmitted = QTest::qWaitFor([&]() { return backend.transmitted.load() == 1; }, 3000);

        stop = true;
        capture.join();
        CloseHandle(client);
        fclose(out);

        QVERIFY(transmitted);
        QCOMPARE(backend.lastTransmitted.id, 0x7E0u);
        QCOMPARE(backend.lastTransmitted.data[7], uint8_t(0x17));
        QCOMPARE(backend.uninitialized.load(), 1);
    }

    void feederServesTheSyntheticSourceOnItsPipe() {
        // The same pipe tshark reads from, read here by the test instead.
        PcapPipeFeeder feeder;
        std::string error;
        QVERIFY2(feeder.start({kSyntheticInterfaceId, CanBitrateConfig{}, true}, {}, &error), error.c_str());
        QVERIFY(feeder.pipeName().rfind("\\\\.\\pipe\\cantrip_capture_", 0) == 0);

        HANDLE client = INVALID_HANDLE_VALUE;
        QVERIFY(QTest::qWaitFor([&]() {
            client = CreateFileA(feeder.pipeName().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            return client != INVALID_HANDLE_VALUE;
        }, 3000));

        QByteArray bytes(24 + 32, '\0');
        int total = 0;
        while (total < bytes.size()) {
            DWORD got = 0;
            QVERIFY(ReadFile(client, bytes.data() + total, static_cast<DWORD>(bytes.size() - total), &got, nullptr));
            total += static_cast<int>(got);
        }
        QCOMPARE(readLe32(bytes, 0), 0xa1b2c3d4u);
        QCOMPARE(readLe32(bytes, 20), uint32_t(kPcapLinktypeCanSocketcan));
        QCOMPARE(readLe32(bytes, 32), 16u);                                      // first record: a classic frame
        QCOMPARE(bytes.mid(40, 4).toHex(), QByteArray("00000100"));              // ID 0x100, EngineData

        feeder.requestStop();
        feeder.join();
        CloseHandle(client);
    }

    void feederStopsEvenIfNothingEverConnects() {
        // tshark missing or failing to start must not leave a thread behind.
        PcapPipeFeeder feeder;
        std::string error;
        QVERIFY(feeder.start({kSyntheticInterfaceId, CanBitrateConfig{}, true}, {}, &error));
        QElapsedTimer timer;
        timer.start();
        feeder.requestStop();
        feeder.join();
        QVERIFY(timer.elapsed() < 1000);
    }

    void feederReportsAnInterfaceThatDoesNotExist() {
        std::mutex mutex;
        std::string message;
        PcapPipeFeeder feeder;
        std::string error;
        QVERIFY(feeder.start({"no_such_interface", CanBitrateConfig{}, true},
                             [&](const std::string& m) { std::lock_guard<std::mutex> lock(mutex); message = m; }, &error));
        HANDLE client = INVALID_HANDLE_VALUE;
        QVERIFY(QTest::qWaitFor([&]() {
            client = CreateFileA(feeder.pipeName().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            return client != INVALID_HANDLE_VALUE;
        }, 3000));
        feeder.join(); // ends by itself: there is nothing to capture
        CloseHandle(client);
        std::lock_guard<std::mutex> lock(mutex);
        QCOMPARE(message, std::string("unknown or unavailable interface 'no_such_interface'"));
    }
};

QTEST_APPLESS_MAIN(PcapCaptureTest)
#include "tst_pcap_capture.moc"
