#include "MessageSender.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

namespace cantrip {

namespace {

// Must match can2pcap.cpp's sanitizeId()/interfaceIdFor() and
// MainWindow.cpp's own copy exactly - see MainWindow.cpp's comment on why
// this is duplicated rather than shared (it's how the app tells any backend
// consumer which vendor+channel an interfaceId string refers to).
std::string sanitizeId(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        } else if (!out.empty() && out.back() != '_') {
            out += '_';
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

QString interfaceIdFor(const IAvlabsCanBackend& backend, const CanChannelInfo& channel) {
    return QString::fromStdString(backend.id() + "_" + sanitizeId(channel.name));
}

// Fine enough for cyclic test messages (10/20/50/100 ms are the common real
// cases) without spinning a timer per message - matches the single-shared-
// timer pattern already used for displayFlushTimer_/staleTimer_ elsewhere.
constexpr int kSchedulerTickMs = 5;

// CanFrame::dlc (common/AVlabsCanBackend.h) is a raw DLC *code* for FD
// frames (0-15), not a byte length - confirmed against can2pcap.cpp's own
// receive path (fdDlcCodeToLength()) and AscLogWriter.cpp's identical
// table (dlcCodeFromByteLength()), both of which this must match exactly.
// TransmitMessage::dlc, by contrast, is a real byte length throughout this
// file and the UI (matching DecodedCanFrame::dlc's convention) - this
// converts at the one point it actually needs to become a CanFrame for a
// real writeFrame() call. Missing this conversion sent a raw byte count
// (e.g. 64) straight into the driver's DLC code field - confirmed for real
// (2026-07-13, user's Vector hardware) to cause bus errors and a crash.
uint8_t dlcCodeFromByteLength(int len) {
    if (len <= 8) return static_cast<uint8_t>(len);
    if (len <= 12) return 9;
    if (len <= 16) return 10;
    if (len <= 20) return 11;
    if (len <= 24) return 12;
    if (len <= 32) return 13;
    if (len <= 48) return 14;
    return 15;
}

} // namespace

MessageSender::MessageSender(QObject* parent) : QObject(parent) {
    connect(&timer_, &QTimer::timeout, this, &MessageSender::onTimerTick);
}

bool MessageSender::openPort(const QString& interfaceId, const CanBitrateConfig& config, QString* error) {
    closePort();

    for (auto& candidate : probeAvailableBackends()) {
        for (const CanChannelInfo& ch : candidate->enumerateChannels()) {
            if (interfaceIdFor(*candidate, ch) != interfaceId) continue;

            if (candidate->id() == "peak") {
                // PCAN-Basic has no listen-only equivalent - a second
                // handle can't join a channel can2pcap.exe already
                // initialized. Connect to the named pipe it creates for
                // exactly this instead (see extcap/can2pcap.cpp's
                // createTxPipe()) - it transmits on its own handle.
                //
                // capture_.isRunning() (checked before this is ever called)
                // only reflects tshark's own process state - can2pcap.exe is
                // a further child process tshark spawns for the actual
                // extcap capture, and it still needs its own real time to
                // load the vendor DLL, resolve the channel, call
                // CAN_Initialize, and only then create this pipe. Retrying
                // for a couple of seconds absorbs that startup latency
                // instead of failing on the very first attempt.
                const std::string pipeName = "\\\\.\\pipe\\cantrip_tx_" + interfaceId.toStdString();
                HANDLE pipe = INVALID_HANDLE_VALUE;
                for (int attempt = 0; attempt < 20; ++attempt) {
                    pipe = CreateFileA(pipeName.c_str(), GENERIC_WRITE, 0, nullptr,
                                        OPEN_EXISTING, 0, nullptr);
                    if (pipe != INVALID_HANDLE_VALUE) break;
                    Sleep(100);
                }
                if (pipe == INVALID_HANDLE_VALUE) {
                    if (error) *error = "Could not connect to CANtrip's PEAK transmit pipe. "
                                         "Is a capture actually running on this channel?";
                    return false;
                }
                sendMode_ = SendMode::NamedPipe;
                txPipe_ = pipe;
                portOpen_ = true;
                portClock_.start();
                return true;
            }

            // Listen-only: this channel is already configured (by
            // can2pcap.exe's own capture port, opened moments before this
            // call) - request no configuration rights, just join it.
            std::string initError;
            if (!candidate->initialize(ch.channelId, config, /*requestOwnership=*/false, &initError)) {
                if (error) *error = QString::fromStdString(initError);
                return false;
            }
            sendMode_ = SendMode::DirectBackend;
            backend_ = std::move(candidate);
            channelId_ = ch.channelId;
            portOpen_ = true;
            portClock_.start();
            return true;
        }
    }

    if (error) *error = "Interface not found: " + interfaceId;
    return false;
}

void MessageSender::closePort() {
    stop();
    if (sendMode_ == SendMode::NamedPipe) {
        if (txPipe_ != INVALID_HANDLE_VALUE) CloseHandle(txPipe_);
        txPipe_ = INVALID_HANDLE_VALUE;
    } else if (backend_ && portOpen_) {
        backend_->uninitialize(channelId_);
    }
    backend_.reset();
    portOpen_ = false;
}

int MessageSender::addMessage(const TransmitMessage& message) {
    messages_.push_back(message);
    nextDueMs_.push_back(0);
    return static_cast<int>(messages_.size()) - 1;
}

void MessageSender::updateMessage(int index, const TransmitMessage& message) {
    if (index < 0 || index >= static_cast<int>(messages_.size())) return;
    messages_[static_cast<size_t>(index)] = message;
    nextDueMs_[static_cast<size_t>(index)] = 0;
}

void MessageSender::removeMessages(const std::vector<int>& indices) {
    // Remove highest-index-first so earlier indices in the list stay valid
    // as later ones are erased.
    std::vector<int> sorted = indices;
    std::sort(sorted.rbegin(), sorted.rend());
    for (int index : sorted) {
        if (index < 0 || index >= static_cast<int>(messages_.size())) continue;
        messages_.erase(messages_.begin() + index);
        nextDueMs_.erase(nextDueMs_.begin() + index);
    }
}

void MessageSender::clearMessages() {
    messages_.clear();
    nextDueMs_.clear();
}

void MessageSender::start() {
    if (!portOpen_ || sending_) return;
    sending_ = true;
    for (size_t i = 0; i < messages_.size(); ++i) {
        nextDueMs_[i] = portClock_.elapsed();
    }
    timer_.start(kSchedulerTickMs);
}

void MessageSender::stop() {
    sending_ = false;
    timer_.stop();
}

void MessageSender::sendNow(const std::vector<int>& indices) {
    if (!portOpen_) return;
    for (int index : indices) {
        if (index < 0 || index >= static_cast<int>(messages_.size())) continue;
        transmitOne(index);
    }
}

void MessageSender::onTimerTick() {
    if (!portOpen_ || !sending_) return;
    const qint64 now = portClock_.elapsed();

    for (size_t i = 0; i < messages_.size(); ++i) {
        const TransmitMessage& msg = messages_[i];
        if (msg.paused) continue;
        if (now < nextDueMs_[i]) continue;

        transmitOne(static_cast<int>(i));

        if (msg.cycleTimeMs <= 0) {
            // One-shot: fired once when sending started, don't reschedule -
            // push its due time far out so it doesn't fire again this session.
            nextDueMs_[i] = std::numeric_limits<qint64>::max();
        } else {
            nextDueMs_[i] = now + msg.cycleTimeMs;
        }
    }
}

void MessageSender::transmitOne(int index) {
    if (index < 0 || index >= static_cast<int>(messages_.size())) return;
    const TransmitMessage& msg = messages_[static_cast<size_t>(index)];

    CanFrame frame;
    frame.id = msg.id;
    frame.extended = msg.extended;
    frame.rtr = msg.rtr;
    frame.fd = msg.fd;
    frame.brs = msg.brs;
    // msg.dlc is a real byte length (0-64); CanFrame::dlc must be the raw
    // DLC code (0-15) for FD frames, the two are only numerically the same
    // for classic frames (dlc <= 8) - see dlcCodeFromByteLength() above.
    frame.dlc = msg.fd ? dlcCodeFromByteLength(msg.dlc) : msg.dlc;
    std::memcpy(frame.data, msg.data.constData(), std::min<size_t>(sizeof(frame.data), static_cast<size_t>(msg.data.size())));

    std::string error;
    if (!writeFrameOut(frame, &error)) {
        emit sendFailed(msg, QString::fromStdString(error));
        return;
    }

    emit frameSent(toDecodedFrame(msg));
}

bool MessageSender::writeFrameOut(const CanFrame& frame, std::string* error) {
    if (sendMode_ == SendMode::NamedPipe) {
        DWORD bytesWritten = 0;
        if (!WriteFile(txPipe_, &frame, sizeof(CanFrame), &bytesWritten, nullptr) ||
            bytesWritten != sizeof(CanFrame)) {
            if (error) *error = "Could not write to the PEAK transmit pipe.";
            return false;
        }
        return true;
    }
    return backend_->writeFrame(channelId_, frame, error);
}

DecodedCanFrame MessageSender::toDecodedFrame(const TransmitMessage& message) const {
    DecodedCanFrame frame;
    frame.direction = FrameDirection::Tx;
    frame.timestamp = QString::number(portClock_.elapsed() / 1000.0, 'f', 6);
    frame.id = message.id;
    frame.extended = message.extended;
    frame.rtr = message.rtr;
    frame.fd = message.fd;
    frame.brs = message.brs;
    frame.dlc = message.dlc;
    frame.data = message.data;
    return frame;
}

} // namespace cantrip
