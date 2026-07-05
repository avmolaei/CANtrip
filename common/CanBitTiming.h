// CAN / CAN FD bit-timing calculator: turns a target bitrate + sample point
// into the BRP/TSEG1/TSEG2/SJW register values a controller actually wants.
#pragma once

#include <cstdint>
#include <optional>

namespace cantrip {

struct CanTimingValues {
    uint32_t brp = 0;
    uint32_t tseg1 = 0;
    uint32_t tseg2 = 0;
    uint32_t sjw = 0;
};

struct CanFdTimingResult {
    CanTimingValues nominal;
    CanTimingValues data;
    double achievedNomSamplePoint = 0.0;
    double achievedDataSamplePoint = 0.0;
};

// Classic CAN bit timing for a target bitrate/sample point, ported from
// python-can's BitTiming.iterate_from_sample_point/from_sample_point
// (can/bit_timing.py) - verified against that real, actively-maintained
// implementation rather than derived from scratch. Returns nullopt if no
// ISO-11898-compliant solution exists for the given clock/bitrate/sample
// point combination.
std::optional<CanTimingValues> calculateTiming(uint32_t fClockHz, uint32_t bitrateBps, double samplePointPercent);

// CAN FD bit timing for the nominal (arbitration) and data phases, ported
// from python-can's BitTimingFd.iterate_from_sample_point/from_sample_point.
std::optional<CanFdTimingResult> calculateFdTiming(
    uint32_t fClockHz,
    uint32_t nomBitrateBps, double nomSamplePointPercent,
    uint32_t dataBitrateBps, double dataSamplePointPercent);

} // namespace cantrip
