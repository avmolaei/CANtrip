// can2pcap - Wireshark extcap bridge for CANtrip's CAN hardware backends.
// Bridges *any* IAvlabsCanBackend CANtrip has probed as available - see
// ../common/AVlabsCanBackend.h for the vendor-neutral interface and
// ../common/CanBackendRegistry.cpp for the list of backends it tries.
// (Named pcan2pcap prior to CANtrip 1.1 - a leftover from a PEAK-only
// prototype, before the vendor-neutral backend abstraction existed.)
//
// Implements the four extcap CLI contracts Wireshark/tshark invoke:
//   --extcap-interfaces           list capture interfaces
//   --extcap-dlts                 list link-layer types per interface
//   --extcap-config               list configurable capture options
//   --capture --fifo <path>       stream frames as classic pcap records
//
// Frames are written using libpcap's LINKTYPE_CAN_SOCKETCAN (227) layout,
// which Wireshark's built-in SocketCAN dissector already understands:
// each record is a `struct can_frame` (16 bytes) or `struct canfd_frame`
// (72 bytes) with the entire struct in network (big-endian) byte order.
// This gets CAN ID/DLC/data/FD-flag decoding for free from Wireshark;
// CANtrip's own DBC layer adds signal-level decode on top later.
#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <iostream>
#include <chrono>
#include <thread>

#include "../common/AVlabsCanBackend.h"
#include "../common/PcapCapture.h"

namespace {

using cantrip::CanBitrateConfig;
using cantrip::CanChannelInfo;
using cantrip::CanFrame;
using cantrip::IAvlabsCanBackend;

using cantrip::interfaceIdFor;
using cantrip::kPcapLinktypeCanSocketcan;
constexpr const char* kTestInterfaceId = cantrip::kSyntheticInterfaceId;

std::vector<std::string> collectArgs(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return args;
}

bool hasFlag(const std::vector<std::string>& args, const std::string& flag) {
    for (auto& a : args) if (a == flag) return true;
    return false;
}

std::string getOption(const std::vector<std::string>& args, const std::string& name, const std::string& def = "") {
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == name && i + 1 < args.size()) return args[i + 1];
    }
    return def;
}

void printExtcapInterfaces() {
    std::printf("extcap {version=1.0}{help=https://github.com/}{display=CANtrip CAN bridge}\n");

    for (auto& backend : cantrip::probeAvailableBackends()) {
        for (const CanChannelInfo& ch : backend->enumerateChannels()) {
            std::string id = interfaceIdFor(*backend, ch);
            std::printf("interface {value=%s}{display=%s %s}\n",
                        id.c_str(), backend->displayName().c_str(), ch.name.c_str());
        }
    }
    // Always offer the synthetic test interface so the pipeline can be
    // verified end-to-end without any CAN hardware or vendor drivers
    // installed.
    std::printf("interface {value=%s}{display=CANtrip synthetic test source (no hardware needed)}\n",
                 kTestInterfaceId);
}

void printExtcapDlts(const std::string& /*interfaceId*/) {
    std::printf("dlt {number=%u}{name=CAN_SOCKETCAN}{display=SocketCAN}\n", kPcapLinktypeCanSocketcan);
}

void printExtcapConfig(const std::string& /*interfaceId*/) {
    // "Usual" bitrate presets as a selector; FD bit-timing (tick values, not
    // a free-text expert string) as plain integer fields. The Qt app talks
    // to can2pcap the same way (invoking with --bitrate/--fd/--data-bitrate/
    // --nom-*/--data-*), this config listing is what makes the tool also
    // usable stand-alone from within Wireshark's own capture-options dialog.
    // Values come from CANtrip's bit-timing calculator (see CanBitTiming.h);
    // manually-entered "expert" values work the same way from either side.
    std::printf("arg {number=0}{call=--bitrate}{display=Bitrate}{type=selector}"
                "{tooltip=Classic CAN nominal bitrate}\n");
    std::printf("value {arg=0}{value=125000}{display=125 kbit/s}{default=false}\n");
    std::printf("value {arg=0}{value=250000}{display=250 kbit/s}{default=false}\n");
    std::printf("value {arg=0}{value=500000}{display=500 kbit/s}{default=true}\n");
    std::printf("value {arg=0}{value=1000000}{display=1 Mbit/s}{default=false}\n");

    std::printf("arg {number=1}{call=--fd}{display=Enable CAN FD}{type=boolflag}{default=false}\n");

    std::printf("arg {number=2}{call=--data-bitrate}{display=FD data bitrate}{type=selector}"
                "{tooltip=Only used when CAN FD is enabled}\n");
    std::printf("value {arg=2}{value=2000000}{display=2 Mbit/s}{default=true}\n");
    std::printf("value {arg=2}{value=5000000}{display=5 Mbit/s}{default=false}\n");

    std::printf("arg {number=3}{call=--nom-brp}{display=Nominal BRP}{type=integer}{default=2}"
                "{tooltip=FD nominal phase bitrate prescaler}\n");
    std::printf("arg {number=4}{call=--nom-tseg1}{display=Nominal TSEG1}{type=integer}{default=63}"
                "{tooltip=FD nominal phase time segment 1}\n");
    std::printf("arg {number=5}{call=--nom-tseg2}{display=Nominal TSEG2}{type=integer}{default=16}"
                "{tooltip=FD nominal phase time segment 2}\n");
    std::printf("arg {number=6}{call=--nom-sjw}{display=Nominal SJW}{type=integer}{default=16}"
                "{tooltip=FD nominal phase sync jump width}\n");
    std::printf("arg {number=7}{call=--data-brp}{display=Data BRP}{type=integer}{default=2}"
                "{tooltip=FD data phase bitrate prescaler}\n");
    std::printf("arg {number=8}{call=--data-tseg1}{display=Data TSEG1}{type=integer}{default=15}"
                "{tooltip=FD data phase time segment 1}\n");
    std::printf("arg {number=9}{call=--data-tseg2}{display=Data TSEG2}{type=integer}{default=4}"
                "{tooltip=FD data phase time segment 2}\n");
    std::printf("arg {number=10}{call=--data-sjw}{display=Data SJW}{type=integer}{default=4}"
                "{tooltip=FD data phase sync jump width}\n");

    std::printf("arg {number=11}{call=--test-mode}{display=Synthetic test source}{type=boolflag}"
                "{default=false}{tooltip=Generate fake frames instead of reading real hardware}\n");

    std::printf("arg {number=12}{call=--listen-only}{display=Listen-only (no bus configuration)}"
                "{type=boolflag}{default=false}{tooltip=Join this channel without requesting "
                "exclusive configuration rights, for use alongside another app that already "
                "configured the bus}\n");
}

int runCapture(const std::vector<std::string>& args, const std::string& interfaceId) {
    std::string fifoPath = getOption(args, "--fifo");
    if (fifoPath.empty()) {
        std::fprintf(stderr, "can2pcap: --capture requires --fifo <path>\n");
        return 1;
    }

    bool testMode = hasFlag(args, "--test-mode") || interfaceId == kTestInterfaceId;

    FILE* fifo = std::fopen(fifoPath.c_str(), "wb");
    if (!fifo) {
        std::fprintf(stderr, "can2pcap: failed to open fifo '%s'\n", fifoPath.c_str());
        return 1;
    }

    if (!cantrip::writePcapHeader(fifo)) {
        std::fclose(fifo);
        return 1;
    }

    // Never set: as an extcap this process runs until dumpcap kills it.
    const std::atomic<bool> stop{false};
    const auto report = [](const std::string& message) { std::fprintf(stderr, "can2pcap: %s\n", message.c_str()); };

    if (testMode) {
        cantrip::runSyntheticCapture(fifo, stop);
        std::fclose(fifo);
        return 0;
    }

    auto getUint = [&](const char* name, const char* def) {
        return static_cast<uint32_t>(std::strtol(getOption(args, name, def).c_str(), nullptr, 10));
    };

    // Listen-only: join this channel without requesting exclusive
    // bus-configuration rights, assuming another app has already configured
    // it (see IAvlabsCanBackend::initialize's requestOwnership parameter).
    const bool requestOwnership = !hasFlag(args, "--listen-only");

    CanBitrateConfig config;
    config.fd = hasFlag(args, "--fd");
    config.nominalBitrateBps = getUint("--bitrate", "500000");
    config.dataBitrateBps = getUint("--data-bitrate", "2000000");
    config.nominalTiming.brp = getUint("--nom-brp", "2");
    config.nominalTiming.tseg1 = getUint("--nom-tseg1", "63");
    config.nominalTiming.tseg2 = getUint("--nom-tseg2", "16");
    config.nominalTiming.sjw = getUint("--nom-sjw", "16");
    config.dataTiming.brp = getUint("--data-brp", "2");
    config.dataTiming.tseg1 = getUint("--data-tseg1", "15");
    config.dataTiming.tseg2 = getUint("--data-tseg2", "4");
    config.dataTiming.sjw = getUint("--data-sjw", "4");

    uint64_t channelId = 0;
    std::unique_ptr<IAvlabsCanBackend> backend = cantrip::resolveInterface(interfaceId, &channelId);
    if (!backend) {
        std::fprintf(stderr, "can2pcap: unknown or unavailable interface '%s'\n", interfaceId.c_str());
        std::fclose(fifo);
        return 1;
    }

    std::string err;
    const bool ok = cantrip::runBackendCapture(*backend, channelId, interfaceId, config, requestOwnership, fifo, stop, report, &err);
    if (!ok) std::fprintf(stderr, "can2pcap: %s\n", err.c_str());
    std::fclose(fifo);
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args = collectArgs(argc, argv);
    std::string interfaceId = getOption(args, "--extcap-interface");

    if (hasFlag(args, "--extcap-interfaces")) {
        printExtcapInterfaces();
        return 0;
    }
    if (hasFlag(args, "--extcap-dlts")) {
        printExtcapDlts(interfaceId);
        return 0;
    }
    if (hasFlag(args, "--extcap-config") && !hasFlag(args, "--capture")) {
        printExtcapConfig(interfaceId);
        return 0;
    }
    if (hasFlag(args, "--capture")) {
        return runCapture(args, interfaceId);
    }

    std::fprintf(stderr,
        "can2pcap: Wireshark extcap bridge for CANtrip's CAN hardware backends.\n"
        "This program is meant to be invoked by Wireshark/tshark/dumpcap,\n"
        "not run directly. See --extcap-interfaces.\n");
    return 1;
}
