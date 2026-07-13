#include "CsvLogWriter.h"

namespace cantrip {

namespace {

// RFC 4180-style quoting: only wrap in quotes (and double any embedded
// quotes) when needed - errorDescription is the one field that routinely
// contains commas (e.g. "Bit Error, Form Error", see
// TsharkCapture.cpp's describeBusError()), everything else here is plain.
QString csvEscape(const QString& field) {
    if (!field.contains(',') && !field.contains('"') && !field.contains('\n')) {
        return field;
    }
    QString escaped = field;
    escaped.replace('"', "\"\"");
    return "\"" + escaped + "\"";
}

QString formatDataHex(const QByteArray& data) {
    return QString::fromLatin1(data.toHex(' ').toUpper());
}

} // namespace

CsvLogWriter::CsvLogWriter(MessageNameResolver resolver) : messageNameResolver_(std::move(resolver)) {}

bool CsvLogWriter::open(const QString& path, QString* error) {
    file_.setFileName(path);
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = "Could not open " + path + " for writing.";
        return false;
    }
    stream_.setDevice(&file_);
    stream_ << "Time,Channel,ID,Direction,Extended,FD,BRS,ESI,RTR,DLC,Data,MessageName,Error\n";
    return true;
}

void CsvLogWriter::writeFrame(int channel, const DecodedCanFrame& frame) {
    if (frame.error) {
        stream_ << frame.timestamp << "," << channel << ",,,,,,,,,,," << csvEscape(frame.errorDescription) << "\n";
        return;
    }

    const QString name = messageNameResolver_ ? messageNameResolver_(frame) : QString();
    const QString dir = frame.direction == FrameDirection::Tx ? "Tx" : "Rx";

    stream_ << frame.timestamp << ","
            << channel << ","
            << QString::number(frame.id, 16).toUpper() << ","
            << dir << ","
            << (frame.extended ? "1" : "0") << ","
            << (frame.fd ? "1" : "0") << ","
            << (frame.brs ? "1" : "0") << ","
            << (frame.esi ? "1" : "0") << ","
            << (frame.rtr ? "1" : "0") << ","
            << static_cast<int>(frame.dlc) << ","
            << formatDataHex(frame.data) << ","
            << csvEscape(name) << ","
            << "\n";
}

void CsvLogWriter::close() {
    if (!file_.isOpen()) return;
    stream_.flush();
    file_.close();
}

} // namespace cantrip
