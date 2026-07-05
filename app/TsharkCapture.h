// Wraps `tshark -T ek` as a QProcess and turns its EK (Elasticsearch bulk)
// JSON stdout into DecodedCanFrame values. tshark, not CANtrip, is doing the
// low-level CAN dissection here (via Wireshark's SocketCAN dissector reading
// from our pcan2pcap extcap) - this class just speaks tshark's line-oriented
// JSON protocol.
#pragma once

#include <cstdint>

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>

namespace cantrip {

struct DecodedCanFrame {
    QString timestamp;
    uint32_t id = 0;
    bool extended = false;
    bool rtr = false;
    bool fd = false;
    bool brs = false;
    bool esi = false;
    uint8_t dlc = 0;
    QByteArray data;
};

class TsharkCapture : public QObject {
    Q_OBJECT
public:
    explicit TsharkCapture(QObject* parent = nullptr);

    struct Config {
        QString tsharkPath = "tshark";
        QString interfaceId;
        uint32_t nominalBitrateBps = 500000;
        uint32_t dataBitrateBps = 2000000;
        bool fd = false;
        QString expertInitString;
    };

    // Starts `tshark -i <interfaceId> -T ek`, forwarding config via
    // `-o extcap.<interfaceId>.<option>:<value>` - tshark's CLI does NOT
    // accept an extcap's declared options as its own long options directly
    // (verified against a real install: `tshark -i x --fd` fails with
    // "unrecognized option"). The `-o extcap.*` preference route is the only
    // one that actually reaches the extcap subprocess's argv.
    void start(const Config& config);
    void stop();
    bool isRunning() const;

signals:
    void frameReceived(const DecodedCanFrame& frame);
    // Raw stderr text from tshark/the extcap - surfaced as-is rather than
    // parsed, since it's meant for a human (e.g. "PCANBasic.dll not found").
    void errorOccurred(const QString& message);
    void stopped();

private slots:
    void onReadyReadStandardOutput();
    void onReadyReadStandardError();
    void onFinished(int exitCode, QProcess::ExitStatus status);

private:
    void processLine(const QByteArray& line);

    QProcess process_;
    QByteArray pendingStdout_;
};

} // namespace cantrip
