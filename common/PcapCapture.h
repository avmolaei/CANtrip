// Reads a CAN channel through its backend and writes what it sees as a pcap
// stream of SocketCAN records (LINKTYPE 227), the format Wireshark's own
// dissector reads.
//
// This is what feeds tshark. CANtrip runs it in a thread of its own process
// (PcapPipeFeeder, over a named pipe), so capturing needs nothing installed
// into Wireshark. can2pcap.exe runs the very same loop as a Wireshark extcap,
// for anyone who wants to capture from Wireshark's own window.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "AVlabsCanBackend.h"

namespace cantrip {

constexpr uint16_t kPcapLinktypeCanSocketcan = 227;
// The channel that needs no hardware: a few DBC-decodable frames, test
// waveforms and the occasional bus error (see test/sample.dbc).
constexpr const char* kSyntheticInterfaceId = "cantrip_test";

// Called with human-readable trouble that doesn't stop the capture, such as
// a read error. May be called from the capture's own thread.
using CaptureMessageCallback = std::function<void(const std::string&)>;

// Interfaces are namespaced by backend so identically-named channels from
// two different vendors can't collide, e.g. "peak_pcan_usbbus1".
std::string interfaceIdFor(const IAvlabsCanBackend& backend, const CanChannelInfo& channel);
// Finds the backend and channel an interface ID refers to. Returns the owning
// backend and sets *channelId; null if no available channel has that ID.
std::unique_ptr<IAvlabsCanBackend> resolveInterface(const std::string& interfaceId, uint64_t* channelId);

// One frame in the SocketCAN on-wire layout. Returns the byte length: 16 for
// classic frames and bus errors, 72 for CAN FD. buf needs room for 72.
size_t serializeFrame(const CanFrame& frame, uint8_t* buf);
// Both return false once whatever reads `out` has gone away.
bool writePcapHeader(FILE* out);
bool writePcapRecord(FILE* out, const CanFrame& frame);

// Both capture loops run until `stop` becomes true or `out` stops accepting
// data, whichever comes first.
void runSyntheticCapture(FILE* out, const std::atomic<bool>& stop);
// Initializes the channel, captures, and releases the channel again before
// returning. Returns false with *error set if it could not be initialized.
// On PEAK it also serves the transmit pipe MessageSender sends through,
// since only the thread holding the PEAK channel can transmit on it.
bool runBackendCapture(IAvlabsCanBackend& backend, uint64_t channelId, const std::string& interfaceId,
                       const CanBitrateConfig& config, bool requestOwnership, FILE* out,
                       const std::atomic<bool>& stop, const CaptureMessageCallback& onMessage, std::string* error);

// Runs a capture in its own thread and serves it on a named pipe for tshark
// to read (`tshark -i <pipeName()>`).
class PcapPipeFeeder {
public:
    struct Config {
        std::string interfaceId;
        CanBitrateConfig bitrate;
        bool requestOwnership = true;
    };

    PcapPipeFeeder() = default;
    ~PcapPipeFeeder();
    PcapPipeFeeder(const PcapPipeFeeder&) = delete;
    PcapPipeFeeder& operator=(const PcapPipeFeeder&) = delete;

    // Creates the pipe, so pipeName() is valid as soon as this returns, and
    // starts the thread that waits for a reader and then feeds it. onMessage
    // is called from that thread.
    bool start(const Config& config, CaptureMessageCallback onMessage, std::string* error);
    const std::string& pipeName() const { return pipeName_; }

    // Asks the thread to finish. Returns at once.
    void requestStop() { stop_ = true; }
    // Waits for the thread. Once this returns the hardware has been released.
    // A thread blocked writing to a reader that has stalled only wakes when
    // that reader exits, so end the reader first if it might be stuck.
    void join();

private:
    void run(Config config, CaptureMessageCallback onMessage, void* pipe);

    std::string pipeName_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

} // namespace cantrip
