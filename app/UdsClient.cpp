#include "UdsClient.h"

#include <algorithm>
#include <cstring>

#include "../common/Uds.h"

namespace cantrip {

namespace {
std::vector<uint8_t> toVector(const QByteArray& bytes) {
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}
QByteArray toByteArray(const std::vector<uint8_t>& bytes) {
    return QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
}
} // namespace

UdsClient::UdsClient(QObject* parent) : QObject(parent) {
    clock_.start();
    setTarget(target_);
    connect(&tickTimer_, &QTimer::timeout, this, &UdsClient::tick);
    tickTimer_.setInterval(5);
    tickTimer_.setTimerType(Qt::PreciseTimer);
    connect(&testerPresentTimer_, &QTimer::timeout, this, &UdsClient::sendTesterPresent);
    testerPresentTimer_.setInterval(kTesterPresentPeriodMs);
}

void UdsClient::setTarget(const Target& target) {
    if (busy_) return;
    target_ = target;
    IsoTpConfig config;
    config.txId = target.requestId;
    config.rxId = target.responseId;
    config.extended = target.extended;
    transport_.setConfig(config);
}

bool UdsClient::flushFrames() {
    for (const CanFrame& frame : transport_.takeFrames()) {
        if (!router_ || !router_->send(target_.bus, frame)) return false;
    }
    return true;
}

bool UdsClient::request(const QByteArray& bytes, QString* error) {
    const auto refuse = [error](const QString& why) {
        if (error) *error = why;
        return false;
    };
    if (bytes.isEmpty()) return refuse("The request is empty.");
    if (bytes.size() > static_cast<qsizetype>(IsoTpChannel::kMaxPayload)) return refuse("A request can be at most 4095 bytes.");
    if (busy_) return refuse("Still waiting for the answer to the previous request.");

    transport_.reset();
    transport_.send(toVector(bytes), nowMs());
    if (!flushFrames()) {
        transport_.reset();
        return refuse(QString("CAN %1 is not being captured. Start a capture on it first.").arg(target_.bus));
    }

    pending_ = bytes;
    expectAnswer_ = !udsSuppressesPositiveResponse(toVector(bytes));
    deadlineMs_ = nowMs() + kResponseTimeoutMs;
    busy_ = true;
    tickTimer_.start();
    emit busyChanged(true);
    emit requestSent(bytes);
    return true;
}

void UdsClient::setTesterPresent(bool on) {
    if (on) testerPresentTimer_.start();
    else testerPresentTimer_.stop();
}

void UdsClient::sendTesterPresent() {
    if (busy_ || !router_) return;
    // 3E 80: tester present, and no answer please. Sent quietly: it would
    // otherwise fill the log every two seconds.
    CanFrame frame;
    frame.id = target_.requestId;
    frame.extended = target_.extended;
    frame.dlc = 8;
    const uint8_t bytes[8] = {0x02, 0x3E, 0x80, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    std::memcpy(frame.data, bytes, 8);
    router_->send(target_.bus, frame);
}

void UdsClient::cancel() {
    if (!busy_) return;
    transport_.reset();
    finish();
}

void UdsClient::finish() {
    busy_ = false;
    pending_.clear();
    tickTimer_.stop();
    emit busyChanged(false);
}

void UdsClient::fail(const QString& reason) {
    const QByteArray request = pending_;
    transport_.reset();
    finish();
    emit failed(request, reason);
}

void UdsClient::handleMessage(const std::vector<uint8_t>& message) {
    if (!busy_) return;
    const std::vector<uint8_t> request = toVector(pending_);
    if (udsIsPendingResponseTo(request, message)) {
        deadlineMs_ = nowMs() + kPendingTimeoutMs;
        emit responsePending(pending_);
        return;
    }
    // Anything else on the response ID that does not answer this request is
    // left alone: another tester may be talking to the same ECU.
    if (!udsIsFinalResponseTo(request, message)) return;
    const QByteArray answered = pending_;
    finish();
    emit responseReceived(answered, toByteArray(message));
}

void UdsClient::onFrame(const DecodedCanFrame& frame) {
    if (!busy_ || frame.error || frame.direction != FrameDirection::Rx || frame.bus != target_.bus) return;
    if (frame.id != target_.responseId || frame.extended != target_.extended || frame.fd) return;

    CanFrame raw;
    raw.id = frame.id;
    raw.extended = frame.extended;
    raw.rtr = frame.rtr;
    raw.dlc = static_cast<uint8_t>(std::min<qsizetype>(frame.data.size(), 8));
    std::memcpy(raw.data, frame.data.constData(), raw.dlc);
    transport_.onFrame(raw, nowMs());

    // A first frame is answered with flow control, which has to go out now.
    if (!flushFrames()) {
        fail(QString("CAN %1 stopped being captured.").arg(target_.bus));
        return;
    }
    for (const std::vector<uint8_t>& message : transport_.takeMessages()) handleMessage(message);
}

void UdsClient::tick() {
    if (!busy_) return;
    transport_.poll(nowMs());
    if (!flushFrames()) {
        fail(QString("CAN %1 stopped being captured.").arg(target_.bus));
        return;
    }
    const std::vector<std::string> errors = transport_.takeErrors();
    if (!errors.empty()) {
        fail("The transfer broke: " + QString::fromStdString(errors.front()) + ".");
        return;
    }
    // A request that asked for silence is done once its frames are out.
    if (!expectAnswer_ && !transport_.sending()) {
        const QByteArray request = pending_;
        finish();
        emit responseReceived(request, QByteArray());
        return;
    }
    // The clock only runs out while nothing is arriving: a long answer that
    // is still coming in is covered by the transport's own timeout.
    if (nowMs() >= deadlineMs_ && !transport_.receiving() && !transport_.sending()) {
        fail("No answer from the ECU.");
    }
}

} // namespace cantrip
