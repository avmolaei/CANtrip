// Central place that knows about every backend CANtrip ships. Kept separate
// from any one backend's .cpp so this file's include list is the map of
// "which vendors does CANtrip currently support" - add a new backend by
// implementing ICanBackend (see PeakBackend.h/.cpp) and adding one more
// load attempt below.
#include "AVlabsCanBackend.h"
#include "PeakBackend.h"

namespace cantrip {

std::vector<std::unique_ptr<ICanBackend>> probeAvailableBackends() {
    std::vector<std::unique_ptr<ICanBackend>> backends;

    std::string error;
    if (auto peak = PeakBackend::load(&error)) {
        backends.push_back(std::move(peak));
    }

    // Future vendors slot in here the same way, e.g.:
    //   if (auto vector = VectorBackend::load(&error)) backends.push_back(std::move(vector));
    //   if (auto kvaser = KvaserBackend::load(&error)) backends.push_back(std::move(kvaser));
    // Each fails closed (omitted, not an error) when its vendor DLL isn't
    // installed, so users only ever see backends relevant to their setup.

    return backends;
}

} // namespace cantrip
