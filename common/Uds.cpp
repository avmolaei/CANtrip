#include "Uds.h"

#include <cstdio>

namespace cantrip {

namespace {

std::string hexByte(uint8_t value) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%02X", value);
    return buf;
}

std::string hexWord(uint16_t value) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%04X", value);
    return buf;
}

std::string hexBytes(const std::vector<uint8_t>& bytes, size_t from) {
    std::string out;
    char buf[4];
    for (size_t i = from; i < bytes.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%02X", bytes[i]);
        if (!out.empty()) out += ' ';
        out += buf;
    }
    return out;
}

// Data shown as text when every byte is printable, which is how VINs, part
// numbers and serial numbers come back.
bool allPrintable(const std::vector<uint8_t>& bytes, size_t from) {
    if (from >= bytes.size()) return false;
    for (size_t i = from; i < bytes.size(); ++i) {
        if (bytes[i] < 0x20 || bytes[i] > 0x7E) return false;
    }
    return true;
}

std::string dataText(const std::vector<uint8_t>& bytes, size_t from) {
    if (from >= bytes.size()) return "no data";
    if (bytes.size() - from >= 4 && allPrintable(bytes, from)) {
        return "\"" + std::string(bytes.begin() + static_cast<long long>(from), bytes.end()) + "\"";
    }
    return hexBytes(bytes, from);
}

std::string didText(uint8_t high, uint8_t low) {
    const uint16_t did = static_cast<uint16_t>(high << 8 | low);
    const std::string name = udsDidName(did);
    return name.empty() ? hexWord(did) : hexWord(did) + " (" + name + ")";
}

std::string sessionName(uint8_t session) {
    switch (session & 0x7F) {
    case 0x01: return "default session";
    case 0x02: return "programming session";
    case 0x03: return "extended session";
    default: return "session " + hexByte(session & 0x7F);
    }
}

std::string resetName(uint8_t type) {
    switch (type & 0x7F) {
    case 0x01: return "hard reset";
    case 0x02: return "key off/on reset";
    case 0x03: return "soft reset";
    default: return "reset type " + hexByte(type & 0x7F);
    }
}

std::string describeRequest(const std::vector<uint8_t>& m) {
    const uint8_t sid = m[0];
    const std::string name = udsServiceName(sid);
    switch (sid) {
    case Uds::kSessionControl:
        return m.size() >= 2 ? name + ": " + sessionName(m[1]) : name;
    case Uds::kEcuReset:
        return m.size() >= 2 ? name + ": " + resetName(m[1]) : name;
    case Uds::kReadDataById:
        return m.size() >= 3 ? name + " " + didText(m[1], m[2]) : name;
    case Uds::kWriteDataById:
        return m.size() >= 3 ? name + " " + didText(m[1], m[2]) + " = " + dataText(m, 3) : name;
    case Uds::kSecurityAccess:
        if (m.size() < 2) return name;
        return (m[1] & 1) ? name + ": request seed, level " + std::to_string(m[1])
                          : name + ": send key, level " + std::to_string(m[1] - 1);
    case Uds::kReadDtc:
        if (m.size() >= 3 && m[1] == 0x02) return name + ": codes matching status mask " + hexByte(m[2]);
        if (m.size() >= 3 && m[1] == 0x01) return name + ": count of codes matching status mask " + hexByte(m[2]);
        return m.size() >= 2 ? name + ": report type " + hexByte(m[1]) : name;
    case Uds::kClearDtc:
        if (m.size() == 4 && m[1] == 0xFF && m[2] == 0xFF && m[3] == 0xFF) return name + ": all codes";
        return m.size() == 4 ? name + ": group " + hexBytes(m, 1) : name;
    case Uds::kRoutineControl:
        if (m.size() < 4) return name;
        return name + ": " + (m[1] == 1 ? "start" : m[1] == 2 ? "stop" : m[1] == 3 ? "results of" : "type " + hexByte(m[1])) +
               " routine " + hexWord(static_cast<uint16_t>(m[2] << 8 | m[3]));
    case Uds::kTesterPresent:
        return name;
    default:
        return m.size() > 1 ? name + ": " + hexBytes(m, 1) : name;
    }
}

std::string describeDtcList(const std::vector<uint8_t>& m) {
    // 59 02 <availability mask> then four bytes per code.
    const size_t count = (m.size() - 3) / 4;
    if (count == 0) return "no trouble codes stored";
    std::string out = std::to_string(count) + (count == 1 ? " trouble code: " : " trouble codes: ");
    for (size_t i = 0; i < count; ++i) {
        const size_t at = 3 + i * 4;
        if (i) out += ", ";
        out += udsDtcText(m[at], m[at + 1], m[at + 2]) + " [" + udsDtcStatusText(m[at + 3]) + "]";
    }
    return out;
}

std::string describePositive(const std::vector<uint8_t>& m) {
    const uint8_t sid = static_cast<uint8_t>(m[0] - Uds::kPositiveOffset);
    const std::string name = udsServiceName(sid);
    switch (sid) {
    case Uds::kSessionControl:
        return m.size() >= 2 ? "Now in " + sessionName(m[1]) : name + " accepted";
    case Uds::kEcuReset:
        return m.size() >= 2 ? "ECU accepted the " + resetName(m[1]) : name + " accepted";
    case Uds::kReadDataById:
        return m.size() >= 3 ? didText(m[1], m[2]) + " = " + dataText(m, 3) : name + " answered";
    case Uds::kWriteDataById:
        return m.size() >= 3 ? didText(m[1], m[2]) + " written" : name + " accepted";
    case Uds::kSecurityAccess:
        if (m.size() < 2) return name + " answered";
        if (m[1] & 1) {
            bool allZero = m.size() > 2;
            for (size_t i = 2; i < m.size(); ++i) allZero = allZero && m[i] == 0;
            return allZero ? "Already unlocked (seed is zero)" : "Seed: " + hexBytes(m, 2);
        }
        return "Unlocked, level " + std::to_string(m[1] - 1);
    case Uds::kReadDtc:
        if (m.size() >= 3 && m[1] == 0x02) return describeDtcList(m);
        if (m.size() >= 6 && m[1] == 0x01) return std::to_string(m[4] << 8 | m[5]) + " trouble code(s) match";
        return name + " answered: " + hexBytes(m, 1);
    case Uds::kClearDtc:
        return "Trouble codes cleared";
    case Uds::kRoutineControl:
        return m.size() >= 4 ? "Routine " + hexWord(static_cast<uint16_t>(m[2] << 8 | m[3])) + " done" +
                                   (m.size() > 4 ? ", result " + hexBytes(m, 4) : "")
                             : name + " answered";
    case Uds::kTesterPresent:
        return "ECU is there";
    default:
        return m.size() > 1 ? name + " answered: " + hexBytes(m, 1) : name + " answered";
    }
}

} // namespace

std::string udsServiceName(uint8_t serviceId) {
    switch (serviceId) {
    case 0x10: return "DiagnosticSessionControl";
    case 0x11: return "ECUReset";
    case 0x14: return "ClearDiagnosticInformation";
    case 0x19: return "ReadDTCInformation";
    case 0x22: return "ReadDataByIdentifier";
    case 0x23: return "ReadMemoryByAddress";
    case 0x27: return "SecurityAccess";
    case 0x28: return "CommunicationControl";
    case 0x2A: return "ReadDataByPeriodicIdentifier";
    case 0x2C: return "DynamicallyDefineDataIdentifier";
    case 0x2E: return "WriteDataByIdentifier";
    case 0x2F: return "InputOutputControlByIdentifier";
    case 0x31: return "RoutineControl";
    case 0x34: return "RequestDownload";
    case 0x35: return "RequestUpload";
    case 0x36: return "TransferData";
    case 0x37: return "RequestTransferExit";
    case 0x3D: return "WriteMemoryByAddress";
    case 0x3E: return "TesterPresent";
    case 0x85: return "ControlDTCSetting";
    case 0x87: return "LinkControl";
    default: return "service " + hexByte(serviceId);
    }
}

std::string udsNrcName(uint8_t nrc) {
    switch (nrc) {
    case 0x10: return "generalReject";
    case 0x11: return "serviceNotSupported";
    case 0x12: return "subFunctionNotSupported";
    case 0x13: return "incorrectMessageLengthOrInvalidFormat";
    case 0x14: return "responseTooLong";
    case 0x21: return "busyRepeatRequest";
    case 0x22: return "conditionsNotCorrect";
    case 0x24: return "requestSequenceError";
    case 0x25: return "noResponseFromSubnetComponent";
    case 0x26: return "failurePreventsExecutionOfRequestedAction";
    case 0x31: return "requestOutOfRange";
    case 0x33: return "securityAccessDenied";
    case 0x35: return "invalidKey";
    case 0x36: return "exceededNumberOfAttempts";
    case 0x37: return "requiredTimeDelayNotExpired";
    case 0x70: return "uploadDownloadNotAccepted";
    case 0x71: return "transferDataSuspended";
    case 0x72: return "generalProgrammingFailure";
    case 0x73: return "wrongBlockSequenceCounter";
    case 0x78: return "requestCorrectlyReceivedResponsePending";
    case 0x7E: return "subFunctionNotSupportedInActiveSession";
    case 0x7F: return "serviceNotSupportedInActiveSession";
    default: return "code " + hexByte(nrc);
    }
}

std::string udsNrcHint(uint8_t nrc) {
    switch (nrc) {
    case 0x11: return "This ECU does not have that service.";
    case 0x12: return "The service exists, but not with that sub-function.";
    case 0x13: return "The request has the wrong number of bytes.";
    case 0x22: return "The ECU is not in a state where it will do this.";
    case 0x24: return "Something has to be requested first, such as a seed before a key.";
    case 0x31: return "The ECU does not know that identifier, or the value is not allowed.";
    case 0x33: return "Unlock the ECU with Security Access first.";
    case 0x35: return "The key does not match the seed.";
    case 0x36: return "Too many wrong keys. The ECU is locked for a while.";
    case 0x37: return "The ECU is still in its delay after wrong keys.";
    case 0x78: return "The ECU is working on it and will answer later.";
    case 0x7E: return "Switch to another diagnostic session first.";
    case 0x7F: return "Switch to another diagnostic session first.";
    default: return {};
    }
}

std::string udsDidName(uint16_t did) {
    switch (did) {
    case 0xF186: return "active diagnostic session";
    case 0xF187: return "spare part number";
    case 0xF188: return "ECU software number";
    case 0xF189: return "ECU software version";
    case 0xF18A: return "system supplier";
    case 0xF18B: return "ECU manufacturing date";
    case 0xF18C: return "ECU serial number";
    case 0xF190: return "VIN";
    case 0xF191: return "ECU hardware number";
    case 0xF193: return "supplier hardware version";
    case 0xF195: return "supplier software version";
    case 0xF197: return "system name";
    default: return {};
    }
}

std::string udsDtcText(uint8_t high, uint8_t middle, uint8_t low) {
    static const char kSystem[4] = {'P', 'C', 'B', 'U'};
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%c%01X%01X%02X-%02X", kSystem[high >> 6], (high >> 4) & 0x3, high & 0x0F, middle, low);
    return buf;
}

std::string udsDtcStatusText(uint8_t status) {
    static const char* const kBits[8] = {"testFailed", "testFailedThisOperationCycle", "pendingDTC", "confirmedDTC",
                                         "testNotCompletedSinceLastClear", "testFailedSinceLastClear",
                                         "testNotCompletedThisOperationCycle", "warningIndicatorRequested"};
    std::string out;
    for (int bit = 0; bit < 8; ++bit) {
        if (!(status & (1 << bit))) continue;
        if (!out.empty()) out += ", ";
        out += kBits[bit];
    }
    return out.empty() ? "no status bits set" : out;
}

bool udsIsPendingResponseTo(const std::vector<uint8_t>& request, const std::vector<uint8_t>& response) {
    return !request.empty() && response.size() >= 3 && response[0] == Uds::kNegativeResponse &&
           response[1] == request[0] && response[2] == Uds::kNrcResponsePending;
}

bool udsIsFinalResponseTo(const std::vector<uint8_t>& request, const std::vector<uint8_t>& response) {
    if (request.empty() || response.empty()) return false;
    if (response[0] == Uds::kNegativeResponse) {
        return response.size() >= 3 && response[1] == request[0] && response[2] != Uds::kNrcResponsePending;
    }
    return response[0] == static_cast<uint8_t>(request[0] + Uds::kPositiveOffset);
}

bool udsSuppressesPositiveResponse(const std::vector<uint8_t>& request) {
    if (request.size() < 2 || !(request[1] & 0x80)) return false;
    // Only services whose second byte is a sub-function carry this bit.
    switch (request[0]) {
    case 0x10: case 0x11: case 0x28: case 0x31: case 0x3E: case 0x85: case 0x87:
        return true;
    default:
        return false;
    }
}

std::string udsDescribe(const std::vector<uint8_t>& m) {
    if (m.empty()) return "empty message";
    if (m[0] == Uds::kNegativeResponse) {
        if (m.size() < 3) return "Malformed negative response";
        return udsServiceName(m[1]) + " refused: " + udsNrcName(m[2]) + " (" + hexByte(m[2]) + ")";
    }
    // Bit 6 of the first byte marks a response.
    if (m[0] & 0x40) return describePositive(m);
    return describeRequest(m);
}

} // namespace cantrip
