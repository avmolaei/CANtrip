// Forwards frames from one bus to another while a capture runs.
//
// The capture threads do the work themselves: the thread reading bus A hands
// each frame to route(), and the thread that owns bus B picks it up with
// take() and transmits it on the channel it already holds. Nothing waits on
// tshark or on the window, so a forwarded frame leaves within about a
// millisecond of arriving.
//
// One router is shared by every capture of a session. All of it is safe to
// call from any thread.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "AVlabsCanBackend.h"

namespace cantrip {

// Frames seen on fromBus with an ID from idLow to idHigh go out on toBus.
struct GatewayRule {
    int fromBus = 1;
    int toBus = 2;
    uint32_t idLow = 0;
    uint32_t idHigh = 0x1FFFFFFFu; // the whole range: every ID

    bool matches(int bus, const CanFrame& frame) const {
        return bus == fromBus && frame.id >= idLow && frame.id <= idHigh;
    }
};

// A frame the gateway put on a bus, for whoever shows or logs traffic.
struct GatewaySent {
    int bus = 0;     // the bus it was transmitted on
    int fromBus = 0; // the bus it came from, 0 for a frame sent with send()
    CanFrame frame;
    std::chrono::steady_clock::time_point when;
    // When it left, in seconds since the first frame received on clockBus:
    // the clock that bus's own received frames are stamped with. clockBus is
    // the destination if it has received anything, else the source, else 0
    // for no such clock.
    int clockBus = 0;
    double clockSec = 0.0;
};

class GatewayRouter {
public:
    // Rules can change while captures run. A rule from a bus to itself is
    // dropped: it would repeat every frame on its own bus forever.
    void setRules(std::vector<GatewayRule> rules);
    std::vector<GatewayRule> rules() const;
    // Cheap check for the capture loops: false means route() does nothing.
    bool active() const { return active_.load(std::memory_order_relaxed); }

    // A capture thread says which bus it serves, for as long as it runs.
    // Frames for a bus nobody serves are not queued.
    void attach(int bus);
    void detach(int bus);

    // Called by the thread reading fromBus for every frame it receives.
    // A frame goes to each destination bus once, however many rules match.
    void route(int fromBus, const CanFrame& frame);
    // Queues one frame for `bus` whatever the rules say: how Send Message
    // transmits on a bus beyond the first. False if nobody serves that bus.
    bool send(int bus, const CanFrame& frame);
    // Called by the thread serving `bus`: the next frame to transmit on it.
    bool take(int bus, CanFrame* out, int* fromBus = nullptr);
    // Sleeps until there is a frame for `bus`, at most `timeout`.
    void waitForWork(int bus, std::chrono::milliseconds timeout);
    // The serving thread reports what became of a frame it took.
    void reportSent(int bus, int fromBus, const CanFrame& frame, bool ok, const std::string& error);

    // Everything transmitted since the last call, oldest first.
    std::vector<GatewaySent> drainSent();

    // Frames forwarded under each rule, in the order of rules().
    std::vector<uint64_t> forwardedCounts() const;
    // Frames given up on because a destination bus could not keep up.
    uint64_t dropped() const;
    // Transmits a destination bus refused, and the most recent reason.
    uint64_t failed() const;
    std::string lastError() const;
    // Empties the queues and zeroes the counters. Rules stay.
    void reset();

private:
    struct Pending {
        int fromBus;
        CanFrame frame;
    };

    mutable std::mutex mutex_;
    std::condition_variable work_;
    std::atomic<bool> active_{false};
    std::vector<GatewayRule> rules_;
    std::vector<uint64_t> forwarded_; // parallel to rules_
    std::set<int> attached_;
    std::map<int, std::chrono::steady_clock::time_point> firstFrameAt_;
    std::map<int, std::deque<Pending>> queues_;
    std::deque<GatewaySent> sent_;
    uint64_t dropped_ = 0;
    uint64_t failed_ = 0;
    std::string lastError_;
};

} // namespace cantrip
