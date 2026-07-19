#include "BusLoadTracker.h"

#include <algorithm>

#include <QDateTime>

namespace cantrip {

namespace {

// Frame overhead in bits, excluding the DLC-dependent data field, before
// bit-stuffing is applied - SOF, arbitration, control, CRC field, CRC
// delimiter, ACK slot+delimiter, EOF. Classic CAN, both ID lengths:
//   standard (11-bit): SOF1 + ID11 + RTR1 + IDE1 + r0_1 + DLC4
//                       + CRC15 + CRCdelim1 + ACK1 + ACKdelim1 + EOF7 = 44
//   extended (29-bit): SOF1 + ID11 + SRR1 + IDE1 + IDext18 + RTR1 + r1_1
//                       + r0_1 + DLC4 + CRC15 + CRCdelim1 + ACK1
//                       + ACKdelim1 + EOF7 = 64
constexpr double kClassicStdOverheadBits = 44.0;
constexpr double kClassicExtOverheadBits = 64.0;

// CAN FD adds BRS/ESI/res bits to the control field and widens the CRC to
// 17 bits (payload <= 16 bytes) or 21 bits (payload > 16 bytes) per ISO
// 11898-1:2015. Approximate overhead, same spirit as the classic constants
// above - the exact fixed-stuff-bit count FD's CRC field uses isn't worth
// bit-simulating for a load estimate.
constexpr double kFdStdOverheadBitsShortCrc = 49.0;
constexpr double kFdStdOverheadBitsLongCrc = 53.0;
constexpr double kFdExtOverheadBitsShortCrc = 69.0;
constexpr double kFdExtOverheadBitsLongCrc = 73.0;

constexpr double kInterFrameSpaceBits = 3.0;

// Bit stuffing only applies to the portion from SOF through the CRC field
// itself (a stuff bit is inserted after 5 consecutive same-polarity bits);
// the CRC delimiter/ACK/EOF are fixed-form fields with no stuffing by
// design. Real stuffing depends on the actual bit pattern of the ID+data,
// which isn't practical to simulate here - this flat multiplier is the
// same kind of practical approximation other CAN tools (this one modeled
// after included) use for a load estimate rather than exact accounting.
constexpr double kStuffingFactor = 1.15;
// Bits after the stuffed region: CRC delimiter(1) + ACK(1) + ACK
// delimiter(1) + EOF(7) = 10, common to classic and FD.
constexpr double kUnstuffedTailBits = 10.0;

} // namespace

BusLoadTracker::BusLoadTracker(QObject* parent) : QObject(parent) {
    connect(&tickTimer_, &QTimer::timeout, this, &BusLoadTracker::onTick);
    minPercent_ = 0.0;
}

void BusLoadTracker::setBusConfig(const CanBitrateConfig& config) {
    busConfig_ = config;
}

void BusLoadTracker::reset() {
    clock_.restart();
    tickTimer_.start(kTickMs);

    recentEvents_.clear();
    framesSinceLastTick_ = 0;

    hasSample_ = false;
    maxPercent_ = 0.0;
    minPercent_ = 0.0;
    maxTimestamp_.clear();
    minTimestamp_.clear();
    sumPercentSamples_ = 0.0;
    percentSampleCount_ = 0;

    totalFrames_ = 0;
    totalBytes_ = 0;
    totalErrorFrames_ = 0;
    totalBusSecondsAllTime_ = 0.0;
    peakBurstRate_ = 0.0;
    idleTicks_ = 0;
    totalTicks_ = 0;

    perIdStats_.clear();

    history_.reset();
    lastHistorySampleSec_ = -1.0;
}

double BusLoadTracker::frameSeconds(const DecodedCanFrame& frame) const {
    if (frame.error) return 0.0;

    const double nominalRate = busConfig_.nominalBitrateBps > 0
        ? static_cast<double>(busConfig_.nominalBitrateBps) : 500000.0;
    const double dataRate = busConfig_.dataBitrateBps > 0
        ? static_cast<double>(busConfig_.dataBitrateBps) : nominalRate;

    const double dataFieldBits = 8.0 * frame.dlc;

    if (!frame.fd) {
        const double overhead = frame.extended ? kClassicExtOverheadBits : kClassicStdOverheadBits;
        // Stuffed region: everything from SOF through the CRC field
        // itself, i.e. overhead minus the unstuffed tail, plus the data.
        const double stuffedBits = (overhead - kUnstuffedTailBits) + dataFieldBits;
        const double totalBits = stuffedBits * kStuffingFactor + kUnstuffedTailBits + kInterFrameSpaceBits;
        return totalBits / nominalRate;
    }

    const bool longCrc = frame.dlc > 16;
    double overhead = frame.extended
        ? (longCrc ? kFdExtOverheadBitsLongCrc : kFdExtOverheadBitsShortCrc)
        : (longCrc ? kFdStdOverheadBitsLongCrc : kFdStdOverheadBitsShortCrc);

    if (!frame.brs) {
        // FD without a bit rate switch: whole frame stays at the nominal
        // rate, same shape as the classic-frame math above.
        const double stuffedBits = (overhead - kUnstuffedTailBits) + dataFieldBits;
        const double totalBits = stuffedBits * kStuffingFactor + kUnstuffedTailBits + kInterFrameSpaceBits;
        return totalBits / nominalRate;
    }

    // BRS: arbitration/control (up through the bit rate switch point)
    // stays at the nominal rate; data + CRC field transmit at the data
    // rate, then the fixed tail (CRC delimiter/ACK/EOF) drops back to
    // nominal - approximated here as: nominal-rate portion is the
    // overhead minus the CRC field itself (roughly 21 bits including the
    // long/short split already folded into `overhead`), data-rate portion
    // is the data field plus that CRC field, both stuffed at the same
    // flat factor.
    const double crcFieldBits = longCrc ? 21.0 : 17.0;
    const double nominalPortion = (overhead - kUnstuffedTailBits - crcFieldBits) * kStuffingFactor;
    const double dataPortion = (dataFieldBits + crcFieldBits) * kStuffingFactor;
    return (nominalPortion / nominalRate) + (dataPortion / dataRate)
         + ((kUnstuffedTailBits + kInterFrameSpaceBits) / nominalRate);
}

void BusLoadTracker::recordFrame(const DecodedCanFrame& frame) {
    if (!tickTimer_.isActive()) return; // no capture running - see reset()/onTick()

    const double now = clock_.elapsed() / 1000.0;
    const double busSeconds = frameSeconds(frame);

    recentEvents_.push_back({now, busSeconds});
    ++framesSinceLastTick_;

    if (frame.error) {
        ++totalErrorFrames_;
        return;
    }

    ++totalFrames_;
    totalBytes_ += static_cast<uint64_t>(frame.dlc);
    totalBusSecondsAllTime_ += busSeconds;

    const uint64_t key = (frame.extended ? (uint64_t{1} << 32) : 0) | frame.id;
    auto& entry = perIdStats_[key];
    entry.id = frame.id;
    entry.extended = frame.extended;
    ++entry.frameCount;
    entry.totalBusSeconds += busSeconds;
}

void BusLoadTracker::onTick() {
    const double now = clock_.elapsed() / 1000.0;

    while (!recentEvents_.empty() && now - recentEvents_.front().timeSec > kInstantWindowSec) {
        recentEvents_.pop_front();
    }

    double windowBusSeconds = 0.0;
    for (const auto& e : recentEvents_) windowBusSeconds += e.busSeconds;
    const double instantPercent = std::min(100.0, (windowBusSeconds / kInstantWindowSec) * 100.0);

    const double instantFrameRate = static_cast<double>(recentEvents_.size()) / kInstantWindowSec;
    const double tickBurstRate = framesSinceLastTick_ / (kTickMs / 1000.0);
    peakBurstRate_ = std::max(peakBurstRate_, tickBurstRate);

    ++totalTicks_;
    if (framesSinceLastTick_ == 0) ++idleTicks_;
    framesSinceLastTick_ = 0;

    const QString nowStamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    if (!hasSample_) {
        hasSample_ = true;
        maxPercent_ = instantPercent;
        minPercent_ = instantPercent;
        maxTimestamp_ = nowStamp;
        minTimestamp_ = nowStamp;
    } else {
        if (instantPercent > maxPercent_) { maxPercent_ = instantPercent; maxTimestamp_ = nowStamp; }
        if (instantPercent < minPercent_) { minPercent_ = instantPercent; minTimestamp_ = nowStamp; }
    }
    sumPercentSamples_ += instantPercent;
    ++percentSampleCount_;

    // One history point per second, decimated from the 250ms tick, so the
    // Bus Load History graph stays compact over a long capture (matches
    // SignalHistoryStore's own bounded-samples-per-signal cap).
    if (now - lastHistorySampleSec_ >= 1.0) {
        history_.recordSample("BusLoad.Percent", "%", now, instantPercent);
        lastHistorySampleSec_ = now;
    }

    BusLoadStats stats;
    stats.hasData = hasSample_;
    stats.instantPercent = instantPercent;
    stats.maxPercent = maxPercent_;
    stats.minPercent = minPercent_;
    stats.maxTimestamp = maxTimestamp_;
    stats.minTimestamp = minTimestamp_;
    stats.meanPercent = percentSampleCount_ > 0 ? sumPercentSamples_ / percentSampleCount_ : 0.0;

    stats.instantFrameRate = instantFrameRate;
    stats.averageFrameRate = now > 0.0 ? static_cast<double>(totalFrames_) / now : 0.0;
    stats.totalFrames = totalFrames_;
    stats.totalBytes = totalBytes_;
    stats.errorFrameRate = now > 0.0 ? static_cast<double>(totalErrorFrames_) / now : 0.0;
    stats.totalErrorFrames = totalErrorFrames_;
    stats.peakBurstRate = peakBurstRate_;
    stats.idlePercent = totalTicks_ > 0 ? (100.0 * idleTicks_ / totalTicks_) : 0.0;

    stats.perId.reserve(perIdStats_.size());
    for (const auto& [key, entry] : perIdStats_) {
        (void)key;
        BusLoadPerIdStats row = entry;
        row.percentOfBus = totalBusSecondsAllTime_ > 0.0
            ? 100.0 * entry.totalBusSeconds / totalBusSecondsAllTime_
            : 0.0;
        stats.perId.push_back(row);
    }
    std::sort(stats.perId.begin(), stats.perId.end(),
              [](const BusLoadPerIdStats& a, const BusLoadPerIdStats& b) {
                  return a.percentOfBus > b.percentOfBus;
              });

    emit statsUpdated(stats);
}

} // namespace cantrip
