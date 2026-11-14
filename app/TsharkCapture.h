// Wraps `tshark -T ek` as a QProcess and turns its EK (Elasticsearch bulk)
// JSON stdout into DecodedCanFrame values. tshark, not CANtrip, is doing the
// low-level CAN dissection here (via Wireshark's SocketCAN dissector reading
// from our can2pcap extcap) - this class just speaks tshark's line-oriented
// JSON protocol.
#pragma once

#include <cstdint>

#include <QByteArray>
#include <QObject>
#include <QProcess>

#include "../common/PcapCapture.h"
#include <QString>

namespace cantrip {

// Rx: arrived via live capture or log replay (the only source until Send
// Message). Tx: constructed by MessageSender and fed into the same
// onFrameReceived pipeline as a real capture, so logging/display/decode all
// pick it up without separate wiring.
enum class FrameDirection { Rx, Tx };

struct DecodedCanFrame {
    QString timestamp;
    FrameDirection direction = FrameDirection::Rx;
    uint32_t id = 0;
    bool extended = false;
    bool rtr = false;
    bool fd = false;
    bool brs = false;
    bool esi = false;
    uint8_t dlc = 0;
    QByteArray data;

    // True for a SocketCAN error frame rather than a data frame - when set,
    // only `timestamp` and `errorDescription` are meaningful; id/dlc/data
    // don't represent a real CAN identifier or payload (see
    // AVlabsCanBackend.h's CanFrame::error for the wire-level convention).
    bool error = false;
    QString errorDescription;
};

class TsharkCapture : public QObject {
    Q_OBJECT
public:
    explicit TsharkCapture(QObject* parent = nullptr);
    ~TsharkCapture() override;

    struct Config {
        QString tsharkPath = "tshark";
        QString interfaceId;
        uint32_t nominalBitrateBps = 500000;
        uint32_t dataBitrateBps = 2000000;
        bool fd = false;
        // Bit-timing tick values, used only when fd is true (see
        // CanBitTiming.h).
        uint32_t nomBrp = 0, nomTseg1 = 0, nomTseg2 = 0, nomSjw = 0;
        uint32_t dataBrp = 0, dataTseg1 = 0, dataTseg2 = 0, dataSjw = 0;
        // Join the channel without requesting exclusive bus-configuration
        // rights - for coexisting with another app that's already configured
        // it (see IAvlabsCanBackend::initialize's requestOwnership parameter
        // and CanControllerDialog's "Request bus configuration" checkbox).
        bool listenOnly = false;
    };

    // Opens the channel in a thread of this process (PcapPipeFeeder), which
    // serves what it reads as pcap on a named pipe, and starts
    // `tshark -i <that pipe> -T ek` to dissect it. Nothing has to be
    // installed into Wireshark for this: a stock tshark reads the pipe.
    void start(const Config& config);
    void stop();
    bool isRunning() const;

signals:
    void frameReceived(const DecodedCanFrame& frame);
    // Text from tshark's stderr or from the capture thread - surfaced as-is
    // rather than parsed, since it's meant for a human (e.g. "initialize
    // failed: ...").
    void errorOccurred(const QString& message);
    void stopped();

private slots:
    void onReadyReadStandardOutput();
    void onReadyReadStandardError();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);

private:
    void processLine(const QByteArray& line);

    QProcess process_;
    QByteArray pendingStdout_;
    QString tsharkPath_;
    // Declared after process_ so it is destroyed first: its thread must be
    // gone before anything it reports to is.
    PcapPipeFeeder feeder_;
};

} // namespace cantrip
