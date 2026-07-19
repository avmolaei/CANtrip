#include "DbcDecoder.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace cantrip {

bool DbcDecoder::loadFile(const QString& path, QString* error) {
    std::ifstream is(path.toStdString());
    if (!is) {
        if (error) *error = "Could not open file:\n" + path;
        return false;
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
        return false;
    }

    network_ = std::move(net);
    path_ = path;

    messageById_.clear();
    for (const dbcppp::IMessage& msg : network_->Messages()) {
        messageById_[static_cast<uint32_t>(msg.Id())] = &msg;
    }
    return true;
}

const dbcppp::IMessage* DbcDecoder::messageFor(const DecodedCanFrame& frame) const {
    if (!network_) return nullptr;
    // dbcppp requires the DBC message ID convention where extended-frame IDs
    // have the 0x80000000 bit set (the format .dbc files themselves use for
    // BO_ entries) - Wireshark's can.id field, in contrast, is just the bare
    // 11/29-bit numeric ID with the extended-ness reported separately, so we
    // OR the bit back in before matching.
    const uint32_t dbcId = frame.extended ? (frame.id | 0x80000000u) : frame.id;
    auto it = messageById_.find(dbcId);
    return it != messageById_.end() ? it->second : nullptr;
}

QString DbcDecoder::resolveMessageName(const DecodedCanFrame& frame) const {
    const dbcppp::IMessage* message = messageFor(frame);
    return message ? QString::fromStdString(message->Name()) : QString();
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
