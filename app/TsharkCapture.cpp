#include "TsharkCapture.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
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

// Field names straight from Wireshark's own SocketCAN error-frame decode -
// verified against a real tshark (a hand-crafted error frame with
// CAN_ERR_PROT | CAN_ERR_PROT_STUFF set decoded to exactly these field
// names, e.g. "can_can_err_prot_type_stuff": true).
QString describeBusError(const QJsonObject& can) {
    QStringList parts;
    if (can.value("can_can_err_prot_type_bit").toBool()) parts << "Bit Error";
    if (can.value("can_can_err_prot_type_form").toBool()) parts << "Form Error";
    if (can.value("can_can_err_prot_type_stuff").toBool()) parts << "Stuff Error";
    if (can.value("can_can_err_prot_type_bit0").toBool()) parts << "Bit0 Error";
    if (can.value("can_can_err_prot_type_bit1").toBool()) parts << "Bit1 Error";
    if (can.value("can_can_err_prot_type_overload").toBool()) parts << "Overload";
    if (can.value("can_can_err_prot_type_active").toBool()) parts << "Active Error State";
    if (can.value("can_can_err_prot_type_tx").toBool()) parts << "While Transmitting";
    if (can.value("can_can_err_lostarb").toBool()) parts << "Lost Arbitration";
    if (can.value("can_can_err_ctrl").toBool()) parts << "Controller Error";
    if (can.value("can_can_err_trx").toBool()) parts << "Transceiver Error";
    if (can.value("can_can_err_ack").toBool()) parts << "No ACK";
    if (can.value("can_can_err_busoff").toBool()) parts << "Bus Off";
    if (can.value("can_can_err_buserror").toBool()) parts << "Bus Error";
    if (can.value("can_can_err_restarted").toBool()) parts << "Controller Restarted";
    if (can.value("can_can_err_tx_timeout").toBool()) parts << "TX Timeout";
    return parts.isEmpty() ? QStringLiteral("Unspecified Error") : parts.join(", ");
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
    // -l: flush tshark's stdout after every packet. Without it, tshark
    // buffers its EK JSON output since it's writing to a pipe rather than a
    // terminal, so packets arrive in bursts instead of as they're captured -
    // this showed up as wildly inconsistent measured periods in Periodic
    // mode (0.2-0.7s instead of the synthetic source's actual steady 0.4s
    // per ID) even though the underlying capture timing was fine.
    args << "-i" << config.interfaceId << "-T" << "ek" << "-l";

    // Wireshark's autosar-nm dissector claims CAN ID 0 by default
    // (autosar-nm.can_id/can_id_mask default to 0/0xffffffff), which is a
    // real automotive convention for network-management frames but not a
    // universal one - confirmed against a real production DBC where ID 0
    // is a plain sensor message instead. When it claims a frame, the
    // payload never reaches the generic "data" layer processLine() reads
    // from at all, so every signal in that message silently decodes as if
    // every byte were zero. CANtrip does its own DBC-based decoding and has
    // no use for any of Wireshark's higher-level CAN sub-protocol
    // dissectors, so disable this one outright rather than special-case ID
    // 0. Verified: the same message with this disabled reports its real
    // payload bytes instead of being swallowed into "autosar-nm".
    args << "--disable-protocol" << "autosar-nm";

    // Preference key = the extcap's --call-name with the leading dashes and
    // every internal dash stripped (confirmed against a real tshark: our
    // can2pcap's --data-bitrate option shows up as extcap.<if>.databitrate
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
        setOpt("nombrp", QString::number(config.nomBrp));
        setOpt("nomtseg1", QString::number(config.nomTseg1));
        setOpt("nomtseg2", QString::number(config.nomTseg2));
        setOpt("nomsjw", QString::number(config.nomSjw));
        setOpt("databrp", QString::number(config.dataBrp));
        setOpt("datatseg1", QString::number(config.dataTseg1));
        setOpt("datatseg2", QString::number(config.dataTseg2));
        setOpt("datasjw", QString::number(config.dataSjw));
    }
    if (config.listenOnly) {
        setOpt("listenonly", "true");
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

    // Draining every buffered line unconditionally here - each of which
    // triggers a full decode + UI mutation via frameReceived - is exactly
    // how a real hang was reproduced: any nested Windows message loop
    // (a modal dialog, dragging the window itself, a QDrag operation -
    // confirmed via ProcDump + cdb to be any of these, not any one
    // specific action) lets tshark's pipe buffer keep growing underneath
    // it while this handler can't run, then dumps the entire backlog into
    // one uninterruptible burst the moment it's called again. On a busy
    // real bus that backlog can be tens of thousands of lines. Capping how
    // many get processed per call and re-queuing the rest (rather than
    // looping until pendingStdout_ is empty) lets the event loop - and
    // whatever nested loop is currently running - get a turn in between
    // batches, so the UI stays responsive while catching up instead of
    // freezing solid. Nothing is dropped, just spread across more
    // iterations.
    constexpr int kMaxLinesPerBatch = 200;
    int processed = 0;
    int newlineIdx;
    while (processed < kMaxLinesPerBatch && (newlineIdx = pendingStdout_.indexOf('\n')) >= 0) {
        QByteArray line = pendingStdout_.left(newlineIdx);
        pendingStdout_.remove(0, newlineIdx + 1);
        processLine(line);
        ++processed;
    }

    if (pendingStdout_.indexOf('\n') >= 0) {
        QMetaObject::invokeMethod(this, &TsharkCapture::onReadyReadStandardOutput, Qt::QueuedConnection);
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
    // records) - which one can2pcap emits follows directly from
    // CanFrame::fd, see serializeFrame() in can2pcap.cpp.
    const bool fd = layers.contains("canfd");
    const QJsonObject can = layers.value(fd ? "canfd" : "can").toObject();
    if (can.isEmpty()) return;

    DecodedCanFrame frame;

    // Error frames (always classic-length, never under "canfd") carry no
    // real CAN ID/DLC/payload - can_can_flags_err is Wireshark's own flag
    // for "this is a SocketCAN error frame", so branch off before touching
    // any of the normal id/dlc/data fields, which are absent or meaningless
    // here (e.g. there's no can_can_id field at all on an error frame).
    if (can.value("can_can_flags_err").toBool()) {
        frame.error = true;
        frame.errorDescription = describeBusError(can);
        frame.timestamp = layers.value("frame").toObject().value("frame_frame_time_relative").toString();
        emit frameReceived(frame);
        return;
    }

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
