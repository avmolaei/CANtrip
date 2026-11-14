#include "LogReplaySource.h"

#include <algorithm>

#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

namespace cantrip {

namespace {

QByteArray parseSpaceHex(const QString& hex) {
    QByteArray out;
    const QStringList parts = hex.split(' ', Qt::SkipEmptyParts);
    out.reserve(parts.size());
    for (const QString& p : parts) {
        bool ok = false;
        out.append(static_cast<char>(p.toInt(&ok, 16)));
    }
    return out;
}

// Splits one CSV row, undoing CsvLogWriter's csvEscape(): a field wrapped in
// double quotes may contain commas, and "" inside one is a literal quote.
QStringList splitCsvRow(const QString& line) {
    QStringList fields;
    QString current;
    bool quoted = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (quoted) {
            if (c != '"') current += c;
            else if (i + 1 < line.size() && line[i + 1] == '"') { current += '"'; ++i; }
            else quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            fields << current;
            current.clear();
        } else {
            current += c;
        }
    }
    fields << current;
    return fields;
}

// AscLogWriter's ID field is uppercase hex with an optional trailing "x"
// for extended IDs (see AscLogWriter.cpp's formatFrameId()).
void parseFrameId(const QString& idToken, uint32_t* id, bool* extended) {
    QString s = idToken;
    *extended = s.endsWith('x', Qt::CaseInsensitive);
    if (*extended) s.chop(1);
    *id = s.toUInt(nullptr, 16);
}

} // namespace

LogReplaySource::LogReplaySource(QObject* parent) : QObject(parent) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &LogReplaySource::playNext);
}

bool LogReplaySource::start(const QString& path, QString* error) {
    frames_.clear();
    nextIndex_ = 0;
    running_ = false;

    const bool ok = path.endsWith(".csv", Qt::CaseInsensitive)
        ? loadCsv(path, error)
        : loadAsc(path, error);
    if (!ok) return false;

    if (frames_.isEmpty()) {
        if (error) *error = "No frames found in log file.";
        return false;
    }

    running_ = true;
    playNext();
    return true;
}

void LogReplaySource::stop() {
    timer_.stop();
    running_ = false;
}

bool LogReplaySource::isRunning() const {
    return running_;
}

void LogReplaySource::playNext() {
    if (nextIndex_ >= frames_.size()) {
        running_ = false;
        emit stopped();
        return;
    }

    const int justEmitted = nextIndex_;
    emit frameReceived(frames_[justEmitted]);
    ++nextIndex_;

    if (nextIndex_ >= frames_.size()) {
        running_ = false;
        emit stopped();
        return;
    }

    const double delta = frames_[nextIndex_].timestamp.toDouble() - frames_[justEmitted].timestamp.toDouble();
    timer_.start(std::max(0, static_cast<int>(delta * 1000.0)));
}

bool LogReplaySource::loadAsc(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = "Could not open " + path;
        return false;
    }

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        const QStringList tokens = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (tokens.size() < 6) continue;

        bool timestampOk = false;
        const double timestamp = tokens[0].toDouble(&timestampOk);
        if (!timestampOk) continue; // header/status/statistic/comment line, not a frame

        DecodedCanFrame frame;
        frame.timestamp = QString::number(timestamp, 'f', 6);

        if (tokens[1] == "CANFD") {
            // [0]=ts [1]=CANFD [2]=channel [3]=dir [4]=id [5]=name-or-brs ...
            if (tokens.size() < 9) continue;
            frame.fd = true;
            frame.bus = std::max(1, tokens[2].toInt());
            parseFrameId(tokens[4], &frame.id, &frame.extended);

            int idx = 5;
            // AscLogWriter omits the name token entirely (32 spaces collapse
            // under whitespace-splitting) when no DBC message matched - brs
            // is always exactly "0" or "1", so its absence is detectable.
            if (tokens[idx] != "0" && tokens[idx] != "1") ++idx; // skip name
            if (idx + 3 >= tokens.size()) continue;
            frame.brs = tokens[idx++] == "1";
            frame.esi = tokens[idx++] == "1";
            ++idx; // dlc code - byte length (next token) is authoritative, see AscLogWriter
            bool lenOk = false;
            const int dataLen = tokens[idx++].toInt(&lenOk);
            if (!lenOk) continue;
            frame.dlc = static_cast<uint8_t>(dataLen);
            frame.data = parseSpaceHex(tokens.mid(idx).join(' '));
        } else {
            // [0]=ts [1]=channel [2]=id [3]=dir [4]=frametype(d/r) [5]=dlc(hex) [6..]=data
            frame.bus = std::max(1, tokens[1].toInt());
            parseFrameId(tokens[2], &frame.id, &frame.extended);
            frame.rtr = tokens[4] == "r";
            frame.dlc = static_cast<uint8_t>(tokens[5].toInt(nullptr, 16));
            frame.data = frame.rtr ? QByteArray() : parseSpaceHex(tokens.mid(6).join(' '));
        }

        frames_.push_back(frame);
    }
    return true;
}

bool LogReplaySource::loadCsv(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = "Could not open " + path;
        return false;
    }

    QTextStream stream(&file);

    // Columns are looked up by name in the header row, never by position:
    // v1.4 added a Direction column in the middle, and reading by fixed
    // position then took every later field from its neighbor. Logs from
    // before and after that change both load this way.
    const QStringList header = splitCsvRow(stream.readLine());
    const int colTime = header.indexOf("Time");
    const int colChannel = header.indexOf("Channel");
    const int colId = header.indexOf("ID");
    const int colExtended = header.indexOf("Extended");
    const int colFd = header.indexOf("FD");
    const int colBrs = header.indexOf("BRS");
    const int colEsi = header.indexOf("ESI");
    const int colRtr = header.indexOf("RTR");
    const int colDlc = header.indexOf("DLC");
    const int colData = header.indexOf("Data");
    const int colError = header.indexOf("Error");
    if (colTime < 0 || colId < 0 || colDlc < 0 || colData < 0) {
        if (error) *error = "Not a CANtrip CSV log (no Time/ID/DLC/Data header row): " + path;
        return false;
    }

    while (!stream.atEnd()) {
        const QStringList f = splitCsvRow(stream.readLine());
        auto field = [&f](int column) { return column >= 0 && column < f.size() ? f[column] : QString(); };
        if (field(colTime).isEmpty()) continue;

        DecodedCanFrame frame;
        frame.timestamp = field(colTime);
        frame.bus = std::max(1, field(colChannel).toInt());

        if (!field(colError).isEmpty()) {
            frame.error = true;
            frame.errorDescription = field(colError);
            frames_.push_back(frame);
            continue;
        }

        frame.id = field(colId).toUInt(nullptr, 16);
        frame.extended = field(colExtended) == "1";
        frame.fd = field(colFd) == "1";
        frame.brs = field(colBrs) == "1";
        frame.esi = field(colEsi) == "1";
        frame.rtr = field(colRtr) == "1";
        frame.dlc = static_cast<uint8_t>(field(colDlc).toInt());
        frame.data = parseSpaceHex(field(colData));
        frames_.push_back(frame);
    }
    return true;
}

} // namespace cantrip
