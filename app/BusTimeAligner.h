// Puts frames from several buses on one clock.
//
// Each bus is its own capture, and each capture counts time from its own
// first frame. Shown or logged together, two buses that started a second
// apart would both claim to start at t=0. This shifts every bus by when its
// first frame actually arrived, measured on one shared session clock.
//
// The shift is fixed per bus, so spacing between frames of the same bus
// stays exactly what the adapter reported. Only the alignment between buses
// depends on arrival time, which carries a few milliseconds of pipeline
// jitter. That is the accuracy to expect when comparing times across buses.
#pragma once

#include <map>

namespace cantrip {

class BusTimeAligner {
public:
    void reset() {
        offsets_.clear();
        haveOrigin_ = false;
    }

    // frameSec is the frame's own timestamp from its capture. sessionSec is
    // a clock shared by all buses, read when the frame arrived. Returns the
    // frame's time on the shared timeline, which starts at the first frame
    // seen on any bus.
    double align(int bus, double frameSec, double sessionSec) {
        if (!haveOrigin_) {
            origin_ = sessionSec - frameSec;
            haveOrigin_ = true;
        }
        auto it = offsets_.find(bus);
        if (it == offsets_.end()) it = offsets_.emplace(bus, (sessionSec - origin_) - frameSec).first;
        return frameSec + it->second;
    }

    // For events that did not come through a capture, such as a frame the
    // gateway transmitted. hasBus() is false until that bus has shown its
    // first frame; timeOn() turns a time on that bus's own clock into one on
    // the shared timeline, and fromSession() does the same for a reading of
    // the session clock.
    bool hasBus(int bus) const { return offsets_.count(bus) != 0; }
    double timeOn(int bus, double busSec) const { return busSec + offsets_.at(bus); }
    double fromSession(double sessionSec) const { return haveOrigin_ ? sessionSec - origin_ : 0.0; }

private:
    std::map<int, double> offsets_;
    double origin_ = 0.0;
    bool haveOrigin_ = false;
};

} // namespace cantrip
