#include "AscLogWriter.h"

#include <QDateTime>

namespace cantrip {

namespace {

// A plausible-looking CANalyzer version string for the "// version" header
// comment - deliberately not copied from any real trace, just a reasonable
// stand-in so the header has the same line present.
constexpr const char* kAscVersionComment = "// version 12.0.0";

QString formatHeaderDate(const QDateTime& dt) {
    // "ddd MMM d hh:mm:ss.zzz ap yyyy" -> e.g. "Wed Jul 8 02:20:57.104 pm 2026":
    // 12-hour clock with lowercase am/pm and a non-zero-padded day, which is
    // how real CANalyzer writes it (Qt's "d" is already non-padded; "hh"
    // paired with "ap" in the same format string gives a zero-padded
    // 12-hour hour; "ap" gives lowercase am/pm).
    return dt.toString("ddd MMM d hh:mm:ss.zzz ap yyyy");
}

// Right-justifies a timestamp (seconds, 6 decimal places) to CANalyzer's
// real field width of 11 characters - confirmed against a real trace by
// measuring both a "0.000000"-style short timestamp and a much longer one
// later in the same file; both land on an 11-character field.
QString formatTimestampField(double seconds) {
    return QString("%1").arg(seconds, 11, 'f', 6);
}

QString formatFrameId(uint32_t id, bool extended) {
    QString s = QString::number(id, 16).toUpper();
    if (extended) s += "x";
    return s;
}

QString formatDataHex(const QByteArray& data) {
    return QString::fromLatin1(data.toHex(' ').toUpper());
}

// CAN FD's DLC code (0-15) doesn't equal the byte count above 8 bytes (9-15
// map non-linearly to 12/16/20/24/32/48/64) - DecodedCanFrame::dlc is
// already the real byte count (see TsharkCapture.cpp, sourced from
// Wireshark's can_can_len/canfd_len), so this reverses that mapping back to
// the DLC code real ASC output shows. Standard ISO 11898-1 FD table, not
// vendor-specific - safe to hardcode, same category as the SocketCAN
// CAN_ERR_* constants already hardcoded elsewhere in this codebase.
int dlcCodeFromByteLength(int len) {
    if (len <= 8) return len;
    if (len <= 12) return 9;
    if (len <= 16) return 10;
    if (len <= 20) return 11;
    if (len <= 24) return 12;
    if (len <= 32) return 13;
    if (len <= 48) return 14;
    return 15;
}

} // namespace

AscLogWriter::AscLogWriter(MessageNameResolver resolver) : messageNameResolver_(std::move(resolver)) {}

bool AscLogWriter::open(const QString& path, QString* error) {
    file_.setFileName(path);
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = "Could not open " + path + " for writing.";
        return false;
    }
    stream_.setDevice(&file_);

    const QDateTime now = QDateTime::currentDateTime();
    const QString dateStr = formatHeaderDate(now);
    stream_ << "date " << dateStr << "\n";
    stream_ << "base hex  timestamps absolute\n";
    stream_ << "internal events logged\n";
    stream_ << kAscVersionComment << "\n";
    stream_ << "Begin TriggerBlock " << dateStr << "\n";
    stream_ << formatTimestampField(0.0) << " Start of measurement\n";
    return true;
}

void AscLogWriter::writeFrame(int channel, const DecodedCanFrame& frame) {
    // Bus-error frames aren't written for v1 - real CANalyzer's chip-status
    // line represents transceiver chip state, not SocketCAN protocol-
    // violation classes, and there's no verified mapping between the two;
    // writing the wrong thing would be worse than omitting it (see
    // AscLogWriter.h's header comment and CONTRIBUTING.md's verification
    // guidance).
    if (frame.error) return;

    const QString line = frame.fd ? formatFdLine(channel, frame) : formatClassicLine(channel, frame);
    stream_ << line << "\n";
}

QString AscLogWriter::formatClassicLine(int channel, const DecodedCanFrame& frame) const {
    const QString id = formatFrameId(frame.id, frame.extended);
    const QString dir = "Rx"; // CANtrip only ever receives today - no Tx path yet (Send Message, not this pass)

    QString dtype;
    QString data;
    if (frame.rtr) {
        dtype = QString("r %1").arg(static_cast<int>(frame.dlc), 0, 16);
        data.clear();
    } else {
        dtype = QString("d %1").arg(static_cast<int>(frame.dlc), 0, 16);
        data = formatDataHex(frame.data);
    }

    return QString("%1 %2  %3 %4 %5 %6")
        .arg(formatTimestampField(frame.timestamp.toDouble()))
        .arg(channel)
        .arg(id, -15)
        .arg(dir, -4)
        .arg(dtype)
        .arg(data);
}

QString AscLogWriter::formatFdLine(int channel, const DecodedCanFrame& frame) const {
    const QString id = formatFrameId(frame.id, frame.extended);
    const QString name = messageNameResolver_ ? messageNameResolver_(frame) : QString();
    const int dlcCode = dlcCodeFromByteLength(frame.dlc);

    return QString("%1 CANFD %2 %3 %4  %5 %6 %7 %8 %9 %10")
        .arg(formatTimestampField(frame.timestamp.toDouble()))
        .arg(channel, 3)
        .arg(QStringLiteral("Rx"), -4)
        .arg(id, 8)
        .arg(name, -32)
        .arg(frame.brs ? 1 : 0)
        .arg(frame.esi ? 1 : 0)
        .arg(dlcCode, 0, 16)
        .arg(static_cast<int>(frame.dlc), 2)
        .arg(formatDataHex(frame.data));
}

void AscLogWriter::close() {
    if (!file_.isOpen()) return;
    const QString dateStr = formatHeaderDate(QDateTime::currentDateTime());
    stream_ << "End TriggerBlock " << dateStr << "\n";
    stream_.flush();
    file_.close();
}

} // namespace cantrip
