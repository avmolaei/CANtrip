# The AVlabs CAN Backend

There's no OS-level CAN abstraction on Windows the way SocketCAN provides
on Linux - every vendor ships its own proprietary DLL with its own API
shape. CANtrip's answer is a single vendor-neutral interface,
**`IAvlabsCanBackend`** (`common/AVlabsCanBackend.h`). Every piece of code
that needs to talk to CAN hardware, `can2pcap.exe`, `MessageSender`, talks
to this interface and never to a vendor SDK directly.

```cpp
class IAvlabsCanBackend {
public:
    virtual bool initialize(uint64_t channelId, const CanBitrateConfig& config,
                             bool requestOwnership, std::string* error) = 0;
    virtual void uninitialize(uint64_t channelId) = 0;
    virtual bool readFrame(uint64_t channelId, CanFrame* frame, std::string* error) = 0;
    virtual bool writeFrame(uint64_t channelId, const CanFrame& frame, std::string* error) = 0;
    // ...
};
```

- Each backend dynamically loads its vendor's DLL at runtime
  (`LoadLibrary`/`GetProcAddress`), so CANtrip builds and runs fine with
  only some (or none) of the vendor SDKs actually installed - a missing
  DLL just means that vendor's channels don't show up, not a build or
  launch failure.
- `readFrame()` is documented and used as **non-blocking**: it returns
  immediately whether or not a frame arrived. `can2pcap.exe`'s capture
  loop relies on this (see [Data Flow](data-flow.md)).
- `initialize()`'s `requestOwnership` parameter is what
  [listen-only mode](../user-guide/hardware-tab.md#request-bus-configuration)
  threads through to a backend - what it actually does is vendor-specific,
  see [Send Message Internals](send-message-internals.md).

## Reference implementations

- **`PeakBackend`** (`common/PeakBackend.cpp`) wraps PEAK-System's
  `PCANBasic.dll`. Classic CAN and CAN FD.
- **`VectorBackend`** (`common/VectorBackend.cpp`) wraps Vector
  Informatik's `vxlapi64.dll` (XL Driver Library), verified against a
  VN1640A and a VN7640. Classic CAN and CAN FD, with bit timing computed
  by `common/CanBitTiming.h/.cpp`.
- **`CanBackendRegistry.cpp`** is the single place that lists every
  backend CANtrip knows about (`probeAvailableBackends()`).

Adding support for another vendor (Kvaser's CANlib, ETAS's BOA, etc.) means
implementing `IAvlabsCanBackend` once against that vendor's real SDK header,
following `PeakBackend` as the reference shape, and adding one line to the
registry - see
[`CONTRIBUTING.md`](https://github.com/avmolaei/CANtrip/blob/main/CONTRIBUTING.md#adding-a-new-vendor-backend).

## The DLC code vs byte length trap

`CanFrame::dlc` stores the **raw DLC code** straight from the vendor
driver/CAN spec terminology (0-15), not a byte count. For classic frames
(dlc ≤ 8) the code and the byte length happen to be numerically identical,
which is exactly what makes this trap easy to miss - it only diverges for
CAN FD frames, where codes 9-15 map *non-linearly* to 12/16/20/24/32/48/64
bytes (ISO 11898-1):

```cpp
// code:   0  1  2  3  4  5  6  7  8   9  10  11  12  13  14  15
// bytes:  0  1  2  3  4  5  6  7  8  12  16  20  24  32  48  64
```

Everywhere in the app layer that isn't this raw `CanFrame` struct -
`DecodedCanFrame::dlc`, `TransmitMessage::dlc` - stores a real byte length
(0-64) instead, matching what a human actually means by "how many bytes."
The conversion between the two happens at exactly the boundaries where a
`CanFrame` gets constructed or consumed: `can2pcap.cpp`'s
`fdDlcCodeToLength()` (serializing into the SocketCAN wire format) and
`AscLogWriter.cpp`/`MessageSender.cpp`'s `dlcCodeFromByteLength()` (going
the other direction, into a real transmit).

**This is not a hypothetical foot-gun.** Missing this conversion in
`MessageSender::transmitOne()` sent a raw byte count (e.g. `64`) straight
into a real Vector adapter's DLC code field, and caused real bus errors
followed by a crash on real hardware, confirmed and fixed during Send
Message's development. If you're writing new code that touches a
`CanFrame` at a boundary with app-level code, check which convention
you're actually holding before assuming they're interchangeable.
