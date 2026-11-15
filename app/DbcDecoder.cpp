#include "DbcDecoder.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include <QFileInfo>

namespace cantrip {

std::unique_ptr<DbcDecoder::File> DbcDecoder::parse(const QString& path, int bus, QString* error) {
    std::ifstream is(path.toStdString());
    if (!is) {
        if (error) *error = "Could not open file:\n" + path;
        return nullptr;
    }
    // dbcppp's boost::spirit x3 grammar writes its only diagnostic (which
    // line/token it choked on) to std::cerr, with no way to retrieve it
    // through LoadDBCFromIs's return value - redirect cerr for the duration
    // of the call so a parse failure can show *why*, not just that it failed.
    std::ostringstream parseLog;
    std::streambuf* prevCerr = std::cerr.rdbuf(parseLog.rdbuf());
    auto net = dbcppp::INetwork::LoadDBCFromIs(is);
    std::cerr.rdbuf(prevCerr);
    if (!net) {
        if (error) {
            QString detail = QString::fromStdString(parseLog.str()).trimmed();
            *error = "Failed to parse DBC file:\n" + path;
            if (!detail.isEmpty()) *error += "\n\n" + detail;
        }
        return nullptr;
    }

    auto file = std::make_unique<File>();
    file->info.path = path;
    file->info.bus = bus;
    file->network = std::move(net);
    for (const dbcppp::IMessage& msg : file->network->Messages()) {
        file->messageById[static_cast<uint32_t>(msg.Id())] = &msg;
    }
    file->info.messageCount = static_cast<int>(file->messageById.size());
    return file;
}

bool DbcDecoder::loadFile(const QString& path, QString* error) {
    std::unique_ptr<File> file = parse(path, kAllBuses, error);
    if (!file) return false;
    clear();
    for (const auto& [id, msg] : file->messageById) {
        QStringList& names = signalNames_[msg];
        for (const dbcppp::ISignal& sig : msg->Signals()) names << QString::fromStdString(sig.Name());
    }
    files_.push_back(std::move(file));
    return true;
}

bool DbcDecoder::addFile(const QString& path, int bus, QString* error) {
    std::unique_ptr<File> file = parse(path, bus, error);
    if (!file) return false;
    for (const auto& [id, msg] : file->messageById) {
        QStringList& names = signalNames_[msg];
        for (const dbcppp::ISignal& sig : msg->Signals()) names << QString::fromStdString(sig.Name());
    }
    files_.push_back(std::move(file));
    return true;
}

void DbcDecoder::removeFile(int index) {
    if (index < 0 || index >= static_cast<int>(files_.size())) return;
    for (const auto& [id, msg] : files_[index]->messageById) signalNames_.erase(msg);
    files_.erase(files_.begin() + index);
}

void DbcDecoder::clear() {
    files_.clear();
    signalNames_.clear();
}

std::vector<DbcDecoder::FileInfo> DbcDecoder::files() const {
    std::vector<FileInfo> out;
    for (const auto& file : files_) out.push_back(file->info);
    return out;
}

QStringList DbcDecoder::conflicts() const {
    QStringList out;
    for (size_t later = 1; later < files_.size(); ++later) {
        for (size_t earlier = 0; earlier < later; ++earlier) {
            const File& a = *files_[earlier];
            const File& b = *files_[later];
            // Two files for two different buses never meet.
            if (a.info.bus != kAllBuses && b.info.bus != kAllBuses && a.info.bus != b.info.bus) continue;
            std::vector<uint32_t> shared;
            for (const auto& [id, msg] : b.messageById) {
                if (a.messageById.count(id)) shared.push_back(id);
            }
            std::sort(shared.begin(), shared.end());
            for (uint32_t id : shared) {
                const bool extended = (id & 0x80000000u) != 0;
                out << QString("0x%1 (%2) is in both %3 and %4. %3 is used.")
                           .arg(id & 0x1FFFFFFFu, extended ? 8 : 3, 16, QChar('0'))
                           .arg(QString::fromStdString(a.messageById.at(id)->Name()),
                                QFileInfo(a.info.path).fileName(), QFileInfo(b.info.path).fileName());
            }
        }
    }
    return out;
}

const dbcppp::IMessage* DbcDecoder::messageFor(const DecodedCanFrame& frame) const {
    // dbcppp requires the DBC message ID convention where extended-frame IDs
    // have the 0x80000000 bit set (the format .dbc files themselves use for
    // BO_ entries) - Wireshark's can.id field, in contrast, is just the bare
    // 11/29-bit numeric ID with the extended-ness reported separately, so we
    // OR the bit back in before matching.
    const uint32_t dbcId = frame.extended ? (frame.id | 0x80000000u) : frame.id;
    for (const auto& file : files_) {
        if (file->info.bus != kAllBuses && file->info.bus != frame.bus) continue;
        auto it = file->messageById.find(dbcId);
        if (it != file->messageById.end()) return it->second;
    }
    return nullptr;
}

QString DbcDecoder::resolveMessageName(const DecodedCanFrame& frame) const {
    const dbcppp::IMessage* message = messageFor(frame);
    return message ? QString::fromStdString(message->Name()) : QString();
}

int DbcDecoder::signalCount(const DecodedCanFrame& frame) const {
    const QStringList* names = signalNames(frame);
    return names ? static_cast<int>(names->size()) : 0;
}

const QStringList* DbcDecoder::signalNames(const DecodedCanFrame& frame) const {
    auto it = signalNames_.find(messageFor(frame));
    return it != signalNames_.end() ? &it->second : nullptr;
}

std::vector<DbcDecoder::DecodedSignal> DbcDecoder::decodeSignals(const DecodedCanFrame& frame) const {
    std::vector<DecodedSignal> result;
    const dbcppp::IMessage* message = messageFor(frame);
    if (!message) return result;

    // ISignal::Decode() requires at least 8 bytes and reads up to the
    // signal's own byte range - pad rather than pass frame.data directly,
    // since a short classic frame (dlc < 8) or a short FD frame both give a
    // buffer smaller than that minimum.
    uint8_t buf[64] = {};
    std::memcpy(buf, frame.data.constData(), static_cast<size_t>(std::min<qsizetype>(frame.data.size(), 64)));

    const QString messageName = QString::fromStdString(message->Name());
    for (const dbcppp::ISignal& sig : message->Signals()) {
        const auto raw = sig.Decode(buf);
        DecodedSignal decoded;
        decoded.name = QString::fromStdString(sig.Name());
        decoded.qualifiedName = messageName + "." + decoded.name;
        decoded.unit = QString::fromStdString(sig.Unit());
        decoded.physicalValue = sig.RawToPhys(raw);
        result.push_back(std::move(decoded));
    }
    return result;
}

} // namespace cantrip
