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
                                                             PCAN-Basic.dll
                                                                    |
                                                              PEAK PCAN hardware
```

- **`extcap/pcan2pcap`** is a small Wireshark [extcap](https://www.wireshark.org/docs/wsdg_html_chunked/ChCaptureExtcap.html)
  program. It exposes PEAK-System PCAN-USB/PCAN-FD channels as capture
  interfaces to Wireshark/tshark, translating PCAN-Basic frames into
  SocketCAN-format pcapng records so Wireshark's built-in SocketCAN
  dissector decodes ID/DLC/data/FD flags.
- **`app/`** is the Qt desktop application: pick a PCAN channel, configure
  bus timing (usual presets or expert raw values), import a DBC per
  channel, and view live traffic in a table that unfolds into per-signal
  decoded values (via [dbcppp](https://github.com/xR3b0rn/dbcppp)).

Graphing, gateway mode, and message transmission are deferred to a later
phase.

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
