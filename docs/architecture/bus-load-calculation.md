# Bus Load Calculation

[`BusLoadTracker`](../user-guide/hardware-tab.md#bus-load-statistics)
(`app/BusLoadTracker.h/.cpp`) is a plain, non-GUI class - same pattern as
`TsharkCapture`/`MessageSender` - that turns the same frame stream the
Trace/Graph views see into a live bus load percentage and running
statistics. `BusLoadView` only displays what it emits.

## Why this needs real bit-level timing

A naive "bytes per second ÷ bitrate" estimate is wrong: every CAN frame
carries substantial overhead beyond its data payload - arbitration ID,
control bits, CRC, delimiters, ACK, EOF, inter-frame spacing - and that
overhead is fixed cost regardless of payload size, so it dominates load
on a bus full of short frames. Getting a number worth trusting means
estimating real bits-on-the-wire per frame, not just data bytes.

## The per-frame bit-count formula

Classic CAN frame overhead, before bit-stuffing, standard vs. extended ID
(`app/BusLoadTracker.cpp`):

```cpp
// standard (11-bit): SOF1 + ID11 + RTR1 + IDE1 + r0_1 + DLC4
//                     + CRC15 + CRCdelim1 + ACK1 + ACKdelim1 + EOF7 = 44
// extended (29-bit):  SOF1 + ID11 + SRR1 + IDE1 + IDext18 + RTR1 + r1_1
//                     + r0_1 + DLC4 + CRC15 + CRCdelim1 + ACK1
//                     + ACKdelim1 + EOF7 = 64
```

CAN FD adds BRS/ESI/res control bits and widens the CRC to 17 bits
(payload ≤ 16 bytes) or 21 bits (payload > 16 bytes) per ISO
11898-1:2015 - the code carries four overhead constants (standard/
extended × short/long CRC) for this.

**Bit stuffing** (a stuff bit inserted after every 5 consecutive
same-polarity bits, from SOF through the CRC field) genuinely depends on
the real bit pattern of the ID and data - not knowable without simulating
the exact bits. Rather than bit-simulate every frame, a flat multiplier
(`kStuffingFactor = 1.15`) is applied to the stuffed region only, the
same kind of practical approximation other bus-load tools use. The fixed
tail after the stuffed region (CRC delimiter, ACK, ACK delimiter, EOF -
10 bits, never stuffed by design) and inter-frame spacing are added
unstuffed.

## Dual-rate accounting for CAN FD + BRS

A frame with the bit-rate-switch flag set genuinely transmits at two
different speeds: arbitration/control at the nominal bitrate, then data +
CRC at the (faster) data bitrate, then the fixed tail back at nominal.
`frameSeconds()` computes real wall-clock occupancy accordingly:

```cpp
const double nominalPortion = (overhead - kUnstuffedTailBits - crcFieldBits) * kStuffingFactor;
const double dataPortion = (dataFieldBits + crcFieldBits) * kStuffingFactor;
return (nominalPortion / nominalRate) + (dataPortion / dataRate)
     + ((kUnstuffedTailBits + kInterFrameSpaceBits) / nominalRate);
```

A classic frame, or an FD frame without BRS set, stays entirely at the
nominal rate - same shape as the classic-frame calculation, just with
FD's wider overhead constants.

## What gets tracked from there

Every call to `recordFrame()` (connected directly to
`TsharkCapture::frameReceived` and `MessageSender::frameSent` - **not**
`LogReplaySource`, a replayed log isn't occupying the bus right now, so
it must never count toward live load) appends a `{time, busSeconds}`
event to a rolling one-second window. A 250ms tick then:

- Sums the window's `busSeconds` ÷ window duration → **instant load %**.
- Feeds one decimated sample per second into a **dedicated**
  `SignalHistoryStore` (not `MainWindow`'s shared one, so this synthetic
  `"BusLoad.Percent"` pseudo-signal never pollutes the general Graph
  view's real signal list) - this is what lets the History graph plot it
  with zero new charting code, via `GraphView`'s existing simple mode
  (see [Send Message Internals](send-message-internals.md) for the same
  "reuse the existing pipeline" spirit applied elsewhere).
- Updates running max/min (+ timestamps) and a cumulative mean.
- Updates frame rate, total frames/bytes, error-frame rate, peak burst
  rate, idle-time percentage, and **per-CAN-ID** load share - tracked as
  real accumulated bus-seconds per ID (`BusLoadPerIdStats::totalBusSeconds`),
  not a frame-count approximation, so an ID with fewer-but-larger frames
  is correctly weighted against one with many small frames.

Runs continuously regardless of which tab is showing - not gated behind
"is the Bus Load view currently visible" the way the Stimulation
received pane is, since max/min/mean are meant to describe the whole
session.

## Honest caveat

The stuffing multiplier and FD overhead constants are a documented,
practical approximation, not exact bit-level simulation of every frame.
Cross-check against a reference tool on real hardware before treating the
exact percentage as gospel - the relative shape (load rising, which IDs
dominate) is far more load-bearing than the last decimal place.
