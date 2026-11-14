#include "BusAutoDetector.h"

#include <memory>
#include <string>

#include <QElapsedTimer>
#include <QThread>

#include "../common/PcapCapture.h"

namespace cantrip {

namespace {

const uint32_t kCandidateBitrates[] = {125000, 250000, 500000, 1000000};

} // namespace

bool BusAutoDetector::probeBitrate(const QString& interfaceId, uint32_t bitrateBps, int probeMs) {
    uint64_t channelId = 0;
    const std::unique_ptr<IAvlabsCanBackend> backend = resolveInterface(interfaceId.toStdString(), &channelId);
    if (!backend) return false;

    CanBitrateConfig config;
    config.nominalBitrateBps = bitrateBps;
    std::string error;
    if (!backend->initialize(channelId, config, /*requestOwnership=*/true, &error)) return false;

    // At the wrong bitrate a live bus shows up as bus errors, or as nothing
    // at all. Only clean data frames count as a match.
    int dataFrames = 0;
    int errorFrames = 0;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < probeMs) {
        CanFrame frame;
        error.clear();
        if (backend->readFrame(channelId, &frame, &error)) {
            if (frame.error) ++errorFrames;
            else ++dataFrames;
        } else {
            QThread::msleep(1);
        }
    }
    backend->uninitialize(channelId);

    return dataFrames > 0 && errorFrames == 0;
}

std::optional<uint32_t> BusAutoDetector::detectClassicBitrate(const QString& interfaceId) {
    constexpr int kProbeMs = 300;
    for (uint32_t bitrate : kCandidateBitrates) {
        if (probeBitrate(interfaceId, bitrate, kProbeMs)) {
            return bitrate;
        }
    }
    return std::nullopt;
}

} // namespace cantrip
