#include "GatewayRouter.h"

#include <algorithm>

namespace cantrip {

namespace {
// A destination that stops transmitting (bus off, cable out) must not grow a
// queue without end, and nobody reading drainSent() must not either.
constexpr size_t kMaxQueuedPerBus = 4096;
constexpr size_t kMaxSentKept = 16384;
} // namespace

void GatewayRouter::setRules(std::vector<GatewayRule> rules) {
    rules.erase(std::remove_if(rules.begin(), rules.end(),
                               [](const GatewayRule& rule) { return rule.fromBus == rule.toBus; }),
                rules.end());
    std::lock_guard<std::mutex> lock(mutex_);
    rules_ = std::move(rules);
    forwarded_.assign(rules_.size(), 0);
    active_ = !rules_.empty();
}

std::vector<GatewayRule> GatewayRouter::rules() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rules_;
}

void GatewayRouter::attach(int bus) {
    std::lock_guard<std::mutex> lock(mutex_);
    attached_.insert(bus);
    firstFrameAt_.erase(bus); // a new capture counts time from its own first frame
}

void GatewayRouter::detach(int bus) {
    std::lock_guard<std::mutex> lock(mutex_);
    attached_.erase(bus);
    queues_.erase(bus);
}

void GatewayRouter::route(int fromBus, const CanFrame& frame) {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Every frame counts here, rules or not: this is the instant the
        // bus's own clock reads zero.
        if (!firstFrameAt_.count(fromBus)) firstFrameAt_[fromBus] = std::chrono::steady_clock::now();
        if (rules_.empty() || frame.error) return;
        // Small and on the stack: the buses this frame was already sent to.
        int done[16];
        int doneCount = 0;
        for (size_t i = 0; i < rules_.size(); ++i) {
            const GatewayRule& rule = rules_[i];
            if (!rule.matches(fromBus, frame)) continue;
            if (!attached_.count(rule.toBus)) continue;
            ++forwarded_[i];
            if (std::find(done, done + doneCount, rule.toBus) != done + doneCount) continue;
            if (doneCount < 16) done[doneCount++] = rule.toBus;
            std::deque<Pending>& queue = queues_[rule.toBus];
            if (queue.size() >= kMaxQueuedPerBus) {
                ++dropped_;
                continue;
            }
            queue.push_back({fromBus, frame});
            queued = true;
        }
    }
    if (queued) work_.notify_all();
}

bool GatewayRouter::send(int bus, const CanFrame& frame) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!attached_.count(bus)) return false;
        std::deque<Pending>& queue = queues_[bus];
        if (queue.size() >= kMaxQueuedPerBus) {
            ++dropped_;
            return true;
        }
        queue.push_back({0, frame});
    }
    work_.notify_all();
    return true;
}

bool GatewayRouter::take(int bus, CanFrame* out, int* fromBus) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = queues_.find(bus);
    if (it == queues_.end() || it->second.empty()) return false;
    *out = it->second.front().frame;
    if (fromBus) *fromBus = it->second.front().fromBus;
    it->second.pop_front();
    return true;
}

void GatewayRouter::waitForWork(int bus, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    work_.wait_for(lock, timeout, [this, bus]() {
        const auto it = queues_.find(bus);
        return it != queues_.end() && !it->second.empty();
    });
}

void GatewayRouter::reportSent(int bus, int fromBus, const CanFrame& frame, bool ok, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ok) {
        ++failed_;
        lastError_ = error;
        return;
    }
    if (sent_.size() >= kMaxSentKept) sent_.pop_front();
    GatewaySent sent{bus, fromBus, frame, std::chrono::steady_clock::now()};
    for (int clockBus : {bus, fromBus}) {
        const auto it = firstFrameAt_.find(clockBus);
        if (it == firstFrameAt_.end()) continue;
        sent.clockBus = clockBus;
        sent.clockSec = std::chrono::duration<double>(sent.when - it->second).count();
        break;
    }
    sent_.push_back(sent);
}

std::vector<GatewaySent> GatewayRouter::drainSent() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<GatewaySent> out(sent_.begin(), sent_.end());
    sent_.clear();
    return out;
}

std::vector<uint64_t> GatewayRouter::forwardedCounts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return forwarded_;
}

uint64_t GatewayRouter::dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

uint64_t GatewayRouter::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
}

std::string GatewayRouter::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

void GatewayRouter::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    queues_.clear();
    sent_.clear();
    firstFrameAt_.clear();
    forwarded_.assign(rules_.size(), 0);
    dropped_ = 0;
    failed_ = 0;
    lastError_.clear();
}

} // namespace cantrip
