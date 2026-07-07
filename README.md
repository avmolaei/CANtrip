# CANtrip

![CANtrip](app/resources/cantrip_source.png)

An open-source, free alternative to Vector CANalyzer for viewing CAN /
CAN-FD bus traffic on Windows, decoding it against DBC files, and (in a
later phase) graphing signals and sending/gatewaying messages.

## How it works

CANtrip does not reimplement CAN capture or low-level frame dissection.
Instead it reuses Wireshark's own capture pipeline:

```
CANtrip (Qt app) --launches--> tshark -T ek --reads from--> extcap: pcan2pcap
                                                                    |
                                                          AVlabs CAN backend
                                                          /       |        \
                                                   PeakBackend  VectorBackend  (Kvaser,
                                                       |            |           ETAS, ...)
                                                PCAN-Basic.dll  vxlapi64.dll
                                                       |            |
                                                 PEAK hardware  Vector VN-series hardware
```

- **`extcap/pcan2pcap`** is a small Wireshark [extcap](https://www.wireshark.org/docs/wsdg_html_chunked/ChCaptureExtcap.html)
  program. It exposes CAN channels from any available backend as capture
  interfaces to Wireshark/tshark, translating frames into SocketCAN-format
  pcapng records so Wireshark's built-in SocketCAN dissector decodes
  ID/DLC/data/FD flags.
- **`app/`** is the Qt desktop application: pick a channel from any
  installed vendor, configure bus timing (usual presets or expert raw
  values), import a DBC per channel, and view live traffic in a table that
  unfolds into per-signal decoded values (via [dbcppp](https://github.com/xR3b0rn/dbcppp)).

Graphing, gateway mode, and message transmission are deferred to a later
phase.

### Multi-vendor hardware support

CANtrip is not tied to one CAN adapter vendor. There's no OS-level CAN
abstraction on Windows (unlike Linux's SocketCAN, which is why Wireshark's
SocketCAN dissector already works for any Linux CAN device) - every vendor
ships its own proprietary DLL and API shape. CANtrip works around this with
a vendor-neutral interface, the **AVlabs CAN backend**
([`common/AVlabsCanBackend.h`](common/AVlabsCanBackend.h)) - one bus to sniff
them all: the extcap and app only ever talk to that interface, never to a
vendor SDK directly.

- Each backend dynamically loads its vendor's DLL at runtime
  (`LoadLibrary`/`GetProcAddress`), so CANtrip builds and runs fine with
  only some (or none) of the vendor SDKs installed - a backend whose DLL
  isn't found is simply omitted, not a startup error.
- [`common/PeakBackend.h/.cpp`](common/PeakBackend.cpp) wraps PEAK-System's
  `PCANBasic.dll`. Supports classic CAN and CAN FD.
- [`common/VectorBackend.h/.cpp`](common/VectorBackend.cpp) wraps Vector
  Informatik's `vxlapi64.dll` (XL Driver Library), verified against a real
  VN1640A. Supports classic CAN and CAN FD, with bit timing computed by
  [`common/CanBitTiming.h/.cpp`](common/CanBitTiming.cpp) (see the CAN
  Controller dialog on the Hardware ribbon tab).
- [`common/CanBackendRegistry.cpp`](common/CanBackendRegistry.cpp) is the
  single place that lists every backend CANtrip knows about.
- Adding support for another vendor (Kvaser's CANlib, ETAS's BOA, etc.)
  means implementing the AVlabs CAN backend interface once, using that
  vendor's real SDK header (never reconstructed from memory - a wrong
  struct layout when calling into a proprietary DLL is a silent
  memory-corruption bug, not a compile error - this bit us for real with an
  earlier hand-transcribed PEAK header, see git history) and adding one
  line to the registry. The extcap's pcap-serialization code and the app's
  decode/UI layer don't change.

## Prerequisites (Windows)

- [Wireshark](https://www.wireshark.org/) installed (provides `tshark`,
  `dumpcap`, and the extcap plugin folder).
- A driver package for whichever CAN hardware you're using - only one is
  needed, CANtrip just uses whichever it finds:
  - [PEAK-System PCAN-Basic](https://www.peak-system.com/PCAN-Basic.239.0.html)
    (provides `PCANBasic.dll`), or
  - Vector's XL Driver Library or any Vector driver package that installs
    `vxlapi64.dll` (e.g. Vector Driver Setup, CANoe/CANalyzer, Vector
    Hardware Manager).
- Qt 6 (msvc2019_64 or newer kit) and a matching MSVC toolchain.
- CMake >= 3.21.

## Building

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH="G:\Qt\6.7.3\msvc2019_64"
cmake --build build --config Debug
```

This produces:
- `build\extcap\Debug\pcan2pcap.exe`
- `build\app\Debug\cantrip.exe`

### Installing the extcap into Wireshark

Copy (or symlink) `pcan2pcap.exe` into Wireshark's personal extcap folder
so both Wireshark and `tshark` (and therefore CANtrip) can see it:

```powershell
copy build\extcap\Debug\pcan2pcap.exe "$env:APPDATA\Wireshark\extcap\"
```

Verify it's picked up:

```powershell
tshark -D
```

You should see a `pcan2pcap` interface listed.

## Running CANtrip

Prebuilt binaries are also published under
[Releases](https://github.com/avmolaei/CANtrip/releases) if you'd rather skip
building from source - grab the zip, extract it anywhere, and skip straight
to step 2 below (it already bundles `pcan2pcap.exe`, `cantrip.exe`, and every
DLL both need).

CANtrip's window is a ribbon, Office-style: each tab across the top shows a
different group of controls.

1. Install the extcap (see above) - CANtrip's app doesn't talk to hardware
   directly, it always goes through `tshark`, so `pcan2pcap.exe` has to be
   somewhere Wireshark/tshark can find it.
2. Launch `build\app\Debug\cantrip.exe` (or `cantrip.exe` from a Release
   zip).
3. On the **Hardware** tab, pick a channel from the "Network Hardware"
   dropdown. No CAN hardware or vendor driver installed yet? Pick
   **"CANtrip synthetic test source (no hardware needed)"** - it's always
   listed and fakes traffic so you can try everything below without owning
   a single wire.
4. Still on **Hardware**, click **CAN Controller...** to set the bitrate -
   classic `CAN` mode by default, or `ISO CAN FD`/`Expert CAN FD` for FD
   (not applicable to the synthetic source, which is classic-only for now).
   ISO mode computes real BRP/TSEG1/TSEG2/SJW register values live from a
   target bitrate and sample point; Expert mode lets you type those raw
   values directly.
5. On the **Analysis & Measurement** tab, click **Import DBC...** and load
   [`test/sample.dbc`](test/sample.dbc) - a small DBC whose four message IDs
   (`0x100`, `0x200`, `0x300`, `0x7E8`) deliberately match what the
   synthetic test source transmits, so you get fully decoded signals with
   zero hardware.
6. On the **Home** tab, click **Start**. Frames stream into the table as
   they arrive; click the arrow next to a row to unfold it into its decoded
   signals (name, physical value, unit) via dbcppp. Switch between
   "Waterfall" (newest first) and "Periodic" (one row per ID) display from
   the same tab; click **Stop** to end the capture.

## License

GPL-3.0. See [LICENSE](LICENSE). CANtrip statically/dynamically links
[dbcppp](https://github.com/xR3b0rn/dbcppp) (MPL-2.0) and shells out to
Wireshark's `tshark` (GPL-2.0) as a separate process — see individual
project licenses for details.

Vector, CANalyzer, and CANoe are trademarks of Vector Informatik GmbH.
CANtrip is an independent, unaffiliated open-source project and is not
endorsed by or affiliated with Vector Informatik.
