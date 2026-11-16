// Central place that knows about every backend CANtrip ships. Kept separate
// from any one backend's .cpp so this file's include list is the map of
// "which vendors does CANtrip currently support" - add a new backend by
// implementing the AVlabs CAN backend interface (see PeakBackend.h/.cpp)
// and adding one more load attempt below.
#include "AVlabsCanBackend.h"
#include "PeakBackend.h"
#include "VectorBackend.h"

#include <map>

namespace cantrip {

void disambiguateChannelNames(std::vector<CanChannelInfo>& channels, const std::vector<std::string>& suffixes) {
    std::map<std::string, int> uses;
    for (const CanChannelInfo& ch : channels) ++uses[ch.name];
    for (size_t i = 0; i < channels.size() && i < suffixes.size(); ++i) {
        if (uses[channels[i].name] > 1) channels[i].name += suffixes[i];
    }
    // Same model, same serial (or no serial at all): fall back to numbering
    // whatever is still doubled, in enumeration order.
    std::map<std::string, int> after, seen;
    for (const CanChannelInfo& ch : channels) ++after[ch.name];
    for (CanChannelInfo& ch : channels) {
        if (after[ch.name] > 1) {
            const std::string base = ch.name;
            ch.name += " #" + std::to_string(++seen[base]);
        }
    }
}

std::vector<std::unique_ptr<IAvlabsCanBackend>> probeAvailableBackends() {
    std::vector<std::unique_ptr<IAvlabsCanBackend>> backends;

    std::string error;
    if (auto peak = PeakBackend::load(&error)) {
        backends.push_back(std::move(peak));
    }
    if (auto vector = VectorBackend::load(&error)) {
        backends.push_back(std::move(vector));
    }

    // Future vendors slot in here the same way, e.g.:
    //   if (auto kvaser = KvaserBackend::load(&error)) backends.push_back(std::move(kvaser));
    // Each fails closed (omitted, not an error) when its vendor DLL isn't
    // installed, so users only ever see backends relevant to their setup.

    return backends;
}

} // namespace cantrip
