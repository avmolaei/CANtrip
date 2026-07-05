#include "CanBitTiming.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace cantrip {

namespace {

struct ClassicCandidate {
    CanTimingValues timing;
    double samplePoint = 0.0;
};

// Mirrors BitTiming._validate() + _restrict_to_minimum_range() (the latter
// is always applied here since python-can's iterate_from_sample_point always
// constructs candidates with strict=True).
bool classicValidStrict(uint32_t brp, int tseg1, int tseg2, int sjw, double effectiveBitrate) {
    if (tseg1 < 1 || tseg1 > 16) return false;
    if (tseg2 < 1 || tseg2 > 8) return false;
    if (sjw < 1 || sjw > 4) return false;
    if (sjw > tseg2) return false;

    double samplePoint = 100.0 * (1 + tseg1) / (1.0 + tseg1 + tseg2);
    if (samplePoint < 50.0) return false;

    int nbt = 1 + tseg1 + tseg2;
    if (nbt < 8 || nbt > 25) return false;
    if (brp < 1 || brp > 32) return false;
    if (effectiveBitrate < 5000.0 || effectiveBitrate > 1000000.0) return false;
    return true;
}

struct FdCandidate {
    CanTimingValues nominal;
    CanTimingValues data;
    double nomSamplePoint = 0.0;
    double dataSamplePoint = 0.0;
};

// Mirrors BitTimingFd._validate() + _restrict_to_minimum_range() (strict=True,
// as above).
bool fdValidStrict(uint32_t nomBrp, int nomTseg1, int nomTseg2, int nomSjw,
                    uint32_t dataBrp, int dataTseg1, int dataTseg2, int dataSjw,
                    double effectiveNomBitrate, double effectiveDataBitrate) {
    if (nomTseg1 < 0 || nomTseg2 < 0 || nomSjw < 0) return false;
    if (dataTseg1 < 0 || dataTseg2 < 0 || dataSjw < 0) return false;
    if (nomBrp < 1 || dataBrp < 1) return false;
    if (effectiveDataBitrate < effectiveNomBitrate) return false;
    if (nomSjw > nomTseg2) return false;
    if (dataSjw > dataTseg2) return false;

    double nomSamplePoint = 100.0 * (1 + nomTseg1) / (1.0 + nomTseg1 + nomTseg2);
    double dataSamplePoint = 100.0 * (1 + dataTseg1) / (1.0 + dataTseg1 + dataTseg2);
    if (nomSamplePoint < 50.0 || dataSamplePoint < 50.0) return false;

    int nbt = 1 + nomTseg1 + nomTseg2;
    int dbt = 1 + dataTseg1 + dataTseg2;
    if (nbt < 8 || nbt > 80) return false;
    if (dbt < 5 || dbt > 25) return false;
    if (dataTseg1 < 1 || dataTseg1 > 16) return false;
    if (dataTseg2 < 2 || dataTseg2 > 8) return false;
    if (dataSjw < 1 || dataSjw > 8) return false;

    if (nomBrp == dataBrp) {
        if (nomTseg1 < 2 || nomTseg1 > 128) return false;
        if (nomTseg2 < 2 || nomTseg2 > 32) return false;
        if (nomSjw < 1 || nomSjw > 32) return false;
    } else {
        if (nomTseg1 < 2 || nomTseg1 > 64) return false;
        if (nomTseg2 < 2 || nomTseg2 > 16) return false;
        if (nomSjw < 1 || nomSjw > 16) return false;
    }
    return true;
}

} // namespace

std::optional<CanTimingValues> calculateTiming(uint32_t fClockHz, uint32_t bitrateBps, double samplePointPercent) {
    if (samplePointPercent < 50.0 || bitrateBps == 0) return std::nullopt;

    std::vector<ClassicCandidate> candidates;
    for (uint32_t brp = 1; brp <= 64; ++brp) {
        double nbtF = static_cast<double>(fClockHz) / (static_cast<double>(bitrateBps) * brp);
        int nbt = static_cast<int>(nbtF); // truncation, matching python's int()
        if (nbt < 8) break;

        double effectiveBitrate = static_cast<double>(fClockHz) / (nbt * static_cast<double>(brp));
        if (std::abs(effectiveBitrate - bitrateBps) > bitrateBps / 256.0) continue;

        int tseg1 = static_cast<int>(std::lround(samplePointPercent / 100.0 * nbt)) - 1;
        tseg1 = std::min(tseg1, nbt - 2); // keep tseg2 at least 1 TQ
        int tseg2 = nbt - tseg1 - 1;
        int sjw = std::min(tseg2, 4);

        if (!classicValidStrict(brp, tseg1, tseg2, sjw, effectiveBitrate)) continue;

        ClassicCandidate c;
        c.timing = CanTimingValues{brp, static_cast<uint32_t>(tseg1), static_cast<uint32_t>(tseg2), static_cast<uint32_t>(sjw)};
        c.samplePoint = 100.0 * (1 + tseg1) / (1.0 + tseg1 + tseg2);
        candidates.push_back(c);
    }

    if (candidates.empty()) return std::nullopt;

    // python-can sorts sequentially by (brp, sample-point deviation) using a
    // stable sort; that collapses to a single lexicographic sort by
    // (deviation, brp) ascending.
    std::sort(candidates.begin(), candidates.end(), [&](const ClassicCandidate& a, const ClassicCandidate& b) {
        double devA = std::abs(a.samplePoint - samplePointPercent);
        double devB = std::abs(b.samplePoint - samplePointPercent);
        if (devA != devB) return devA < devB;
        return a.timing.brp < b.timing.brp;
    });

    return candidates.front().timing;
}

std::optional<CanFdTimingResult> calculateFdTiming(
    uint32_t fClockHz,
    uint32_t nomBitrateBps, double nomSamplePointPercent,
    uint32_t dataBitrateBps, double dataSamplePointPercent) {
    if (nomSamplePointPercent < 50.0 || dataSamplePointPercent < 50.0) return std::nullopt;
    if (nomBitrateBps == 0 || dataBitrateBps == 0) return std::nullopt;

    constexpr int kSyncSeg = 1;
    std::vector<FdCandidate> candidates;

    for (uint32_t nomBrp = 1; nomBrp <= 256; ++nomBrp) {
        double nbtF = static_cast<double>(fClockHz) / (static_cast<double>(nomBitrateBps) * nomBrp);
        int nbt = static_cast<int>(nbtF);
        if (nbt < 1) break;

        double effectiveNomBitrate = static_cast<double>(fClockHz) / (nbt * static_cast<double>(nomBrp));
        if (std::abs(effectiveNomBitrate - nomBitrateBps) > nomBitrateBps / 256.0) continue;

        int nomTseg1 = static_cast<int>(std::lround(nomSamplePointPercent / 100.0 * nbt)) - 1;
        nomTseg1 = std::min(nomTseg1, nbt - kSyncSeg - 2);
        int nomTseg2 = nbt - nomTseg1 - 1;
        int nomSjw = std::min(nomTseg2, 128);

        for (uint32_t dataBrp = 1; dataBrp <= 256; ++dataBrp) {
            double dbtF = static_cast<double>(fClockHz) / (static_cast<double>(dataBitrateBps) * dataBrp);
            int dbt = static_cast<int>(dbtF);
            if (dbt < 1) break;

            double effectiveDataBitrate = static_cast<double>(fClockHz) / (dbt * static_cast<double>(dataBrp));
            if (std::abs(effectiveDataBitrate - dataBitrateBps) > dataBitrateBps / 256.0) continue;

            int dataTseg1 = static_cast<int>(std::lround(dataSamplePointPercent / 100.0 * dbt)) - 1;
            dataTseg1 = std::min(dataTseg1, dbt - kSyncSeg - 2);
            int dataTseg2 = dbt - dataTseg1 - 1;
            int dataSjw = std::min(dataTseg2, 16);

            if (!fdValidStrict(nomBrp, nomTseg1, nomTseg2, nomSjw, dataBrp, dataTseg1, dataTseg2, dataSjw,
                                effectiveNomBitrate, effectiveDataBitrate)) {
                continue;
            }

            FdCandidate c;
            c.nominal = CanTimingValues{nomBrp, static_cast<uint32_t>(nomTseg1), static_cast<uint32_t>(nomTseg2), static_cast<uint32_t>(nomSjw)};
            c.data = CanTimingValues{dataBrp, static_cast<uint32_t>(dataTseg1), static_cast<uint32_t>(dataTseg2), static_cast<uint32_t>(dataSjw)};
            c.nomSamplePoint = 100.0 * (1 + nomTseg1) / (1.0 + nomTseg1 + nomTseg2);
            c.dataSamplePoint = 100.0 * (1 + dataTseg1) / (1.0 + dataTseg1 + dataTseg2);
            candidates.push_back(c);
        }
    }

    if (candidates.empty()) return std::nullopt;

    // Prefer solutions sharing one prescaler between phases, if any exist.
    std::vector<FdCandidate> samePrescaler;
    for (const auto& c : candidates) {
        if (c.nominal.brp == c.data.brp) samePrescaler.push_back(c);
    }
    std::vector<FdCandidate>& pool = samePrescaler.empty() ? candidates : samePrescaler;

    // Collapses python-can's three sequential stable sorts into one
    // lexicographic sort by (sample-point deviation sum, brp abs diff, brp sum).
    std::sort(pool.begin(), pool.end(), [&](const FdCandidate& a, const FdCandidate& b) {
        double devA = std::abs(a.nomSamplePoint - nomSamplePointPercent) + std::abs(a.dataSamplePoint - dataSamplePointPercent);
        double devB = std::abs(b.nomSamplePoint - nomSamplePointPercent) + std::abs(b.dataSamplePoint - dataSamplePointPercent);
        if (devA != devB) return devA < devB;
        int diffA = std::abs(static_cast<int>(a.nominal.brp) - static_cast<int>(a.data.brp));
        int diffB = std::abs(static_cast<int>(b.nominal.brp) - static_cast<int>(b.data.brp));
        if (diffA != diffB) return diffA < diffB;
        return (a.nominal.brp + a.data.brp) < (b.nominal.brp + b.data.brp);
    });

    const FdCandidate& best = pool.front();
    CanFdTimingResult result;
    result.nominal = best.nominal;
    result.data = best.data;
    result.achievedNomSamplePoint = best.nomSamplePoint;
    result.achievedDataSamplePoint = best.dataSamplePoint;
    return result;
}

} // namespace cantrip
