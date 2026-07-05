#include "TsharkCapture.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace cantrip {

namespace {

QByteArray parseColonHex(const QString& hex) {
    QByteArray out;
    const QStringList parts = hex.split(':', Qt::SkipEmptyParts);
    out.reserve(parts.size());
    for (const QString& p : parts) {
        bool ok = false;
        out.append(static_cast<char>(p.toInt(&ok, 16)));
    }
    return out;
}

} // namespace

TsharkCapture::TsharkCapture(QObject* parent) : QObject(parent) {
    connect(&process_, &QProcess::readyReadStandardOutput, this, &TsharkCapture::onReadyReadStandardOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, &TsharkCapture::onReadyReadStandardError);
    connect(&process_, &QProcess::finished, this, &TsharkCapture::onFinished);
}

void TsharkCapture::start(const Config& config) {
    if (process_.state() != QProcess::NotRunning) return;
    pendingStdout_.clear();

    QStringList args;
    args << "-i" << config.interfaceId << "-T" << "ek";

    // Preference key = the extcap's --call-name with the leading dashes and
    // every internal dash stripped (confirmed against a real tshark: our
    // pcan2pcap's --data-bitrate option shows up as extcap.<if>.databitrate
    // in `tshark -G defaultprefs`, not extcap.<if>.data-bitrate).
    auto setOpt = [&](const QString& key, const QString& value) {
        args << "-o" << QString("extcap.%1.%2:%3").arg(config.interfaceId, key, value);
    };
    setOpt("bitrate", QString::number(config.nominalBitrateBps));
    if (config.fd) {
        // boolflag options only appear in the extcap's argv when set to the
        // literal lowercase string "true" - "TRUE" is silently ignored.
        setOpt("fd", "true");
        setOpt("databitrate", QString::number(config.dataBitrateBps));
    }
    if (!config.expertInitString.isEmpty()) {
        setOpt("expertstring", config.expertInitString);
    }

    process_.start(config.tsharkPath, args);
}

void TsharkCapture::stop() {
    if (process_.state() == QProcess::NotRunning) return;
    process_.terminate();
    if (!process_.waitForFinished(2000)) {
        process_.kill();
        process_.waitForFinished(2000);
    }
}

bool TsharkCapture::isRunning() const {
    return process_.state() != QProcess::NotRunning;
}

void TsharkCapture::onReadyReadStandardOutput() {
    pendingStdout_ += process_.readAllStandardOutput();
    int newlineIdx;
    while ((newlineIdx = pendingStdout_.indexOf('\n')) >= 0) {
        QByteArray line = pendingStdout_.left(newlineIdx);
        pendingStdout_.remove(0, newlineIdx + 1);
        processLine(line);
    }
}

void TsharkCapture::onReadyReadStandardError() {
    const QByteArray err = process_.readAllStandardError();
    if (!err.isEmpty()) {
        emit errorOccurred(QString::fromLocal8Bit(err));
    }
}

void TsharkCapture::onFinished(int exitCode, QProcess::ExitStatus status) {
    Q_UNUSED(exitCode);
    Q_UNUSED(status);
    emit stopped();
}

void TsharkCapture::processLine(const QByteArray& line) {
    if (line.trimmed().isEmpty()) return;

    const QJsonDocument doc = QJsonDocument::fromJson(line);
    if (!doc.isObject()) return;

    const QJsonObject root = doc.object();
    // EK format alternates an {"index": ...} bulk-header line with a data
    // line for every packet; the header lines have no "layers" key.
    if (!root.contains("layers")) return;
    const QJsonObject layers = root["layers"].toObject();

    // Wireshark's SocketCAN dissector names the layer "can" for classic
    // frames (16-byte records) and "canfd" for CAN FD ones (72-byte
    // records) - which one pcan2pcap emits follows directly from
    // CanFrame::fd, see serializeFrame() in pcan2pcap.cpp.
    const bool fd = layers.contains("canfd");
    const QJsonObject can = layers.value(fd ? "canfd" : "can").toObject();
    if (can.isEmpty()) return;

    DecodedCanFrame frame;
    frame.fd = fd;
    frame.id = can.value("can_can_id").toString().toUInt();
    frame.dlc = static_cast<uint8_t>(can.value("can_can_len").toString().toUInt());
    frame.extended = can.value("can_can_flags_xtd").toBool();
    if (fd) {
        frame.brs = can.value("can_canfd_flags_brs").toBool();
        frame.esi = can.value("can_canfd_flags_esi").toBool();
    } else {
        frame.rtr = can.value("can_can_flags_rtr").toBool();
    }

    // The raw payload always lands in a generic "data" layer regardless of
    // frame type - verified directly (a hand-crafted FD frame with a
    // realistic non-zero CAN ID decodes to layers.data.data_data_data, same
    // as classic frames). Don't use canfd's own "can_can_padding" field:
    // Wireshark's CAN-ID-based sub-dissector table (e.g. autosar-nm's
    // default match on ID 0) can claim the payload into a completely
    // different, unpredictable layer instead - "data" is the one path that
    // held up under that test.
    frame.data = parseColonHex(layers.value("data").toObject().value("data_data_data").toString());

    frame.timestamp = layers.value("frame").toObject().value("frame_frame_time_relative").toString();

    emit frameReceived(frame);
}

} // namespace cantrip
