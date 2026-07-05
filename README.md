# CANtrip

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
                                                             /      |      \
                                                        PeakBackend  (Vector, Kvaser,
                                                            |         ETAS, ... later)
                                                     PCAN-Basic.dll
                                                            |
                                                      PEAK PCAN hardware
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
- [`common/PeakBackend.h/.cpp`](common/PeakBackend.cpp) is the reference
  implementation, wrapping PEAK-System's `PCANBasic.dll`.
- [`common/CanBackendRegistry.cpp`](common/CanBackendRegistry.cpp) is the
  single place that lists every backend CANtrip knows about.
- Adding support for another vendor (Vector's XL Driver Library, Kvaser's
  CANlib, ETAS's BOA, etc.) means implementing the AVlabs CAN backend
  interface once, using that vendor's real SDK header (never reconstructed
  from memory - a wrong struct layout when calling into a proprietary DLL
  is a silent memory-corruption bug, not a compile error) and adding one
  line to the registry. The extcap's pcap-serialization code and the app's
  decode/UI layer don't change.

## Prerequisites (Windows)

- [Wireshark](https://www.wireshark.org/) installed (provides `tshark`,
  `dumpcap`, and the extcap plugin folder).
- [PEAK-System PCAN-Basic](https://www.peak-system.com/PCAN-Basic.239.0.html)
  driver package installed (provides `PCANBasic.dll`).
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

## License

GPL-3.0. See [LICENSE](LICENSE). CANtrip statically/dynamically links
[dbcppp](https://github.com/xR3b0rn/dbcppp) (MPL-2.0) and shells out to
Wireshark's `tshark` (GPL-2.0) as a separate process — see individual
project licenses for details.
