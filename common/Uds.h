// UDS (ISO 14229) in words: service names, negative response codes, trouble
// codes, and a one-line reading of any request or response. Plain functions
// on bytes, so the window, headless mode and the tests all read a message
// the same way.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cantrip {

namespace Uds {
// Service identifiers of the requests CANtrip has buttons for.
constexpr uint8_t kSessionControl = 0x10;
constexpr uint8_t kEcuReset = 0x11;
constexpr uint8_t kClearDtc = 0x14;
constexpr uint8_t kReadDtc = 0x19;
constexpr uint8_t kReadDataById = 0x22;
constexpr uint8_t kSecurityAccess = 0x27;
constexpr uint8_t kWriteDataById = 0x2E;
constexpr uint8_t kRoutineControl = 0x31;
constexpr uint8_t kTesterPresent = 0x3E;

constexpr uint8_t kNegativeResponse = 0x7F;
// A positive response's first byte is the request's plus this.
constexpr uint8_t kPositiveOffset = 0x40;
// "I am working on it, keep waiting": not a final answer.
constexpr uint8_t kNrcResponsePending = 0x78;
} // namespace Uds

// "ReadDataByIdentifier", or "service 0xAB" for one without a name here.
std::string udsServiceName(uint8_t serviceId);
// "requestOutOfRange", or "code 0xAB".
std::string udsNrcName(uint8_t nrc);
// A plain sentence on what a negative response code usually means, for
// someone who has not memorised the table. Empty for unknown codes.
std::string udsNrcHint(uint8_t nrc);
// The name of a standard data identifier such as 0xF190 (VIN). Empty if it
// is not one of the standardised ones.
std::string udsDidName(uint16_t did);
// A trouble code the way a scan tool prints it: "P0301-00".
std::string udsDtcText(uint8_t high, uint8_t middle, uint8_t low);
// The status byte's set bits by name: "testFailed, confirmedDTC".
std::string udsDtcStatusText(uint8_t status);

// True for the final answer to `request`: its positive response, or a
// negative response to it other than "response pending".
bool udsIsFinalResponseTo(const std::vector<uint8_t>& request, const std::vector<uint8_t>& response);
// True for the "response pending" negative response to `request`.
bool udsIsPendingResponseTo(const std::vector<uint8_t>& request, const std::vector<uint8_t>& response);
// True if the request asks the ECU not to answer when all goes well.
bool udsSuppressesPositiveResponse(const std::vector<uint8_t>& request);

// One line saying what a request or response is and what it carries.
std::string udsDescribe(const std::vector<uint8_t>& message);

} // namespace cantrip
