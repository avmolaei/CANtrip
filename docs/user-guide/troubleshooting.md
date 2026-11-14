# Troubleshooting & FAQ

Built from CANtrip's own real incidents, not hypothetical ones - each of
these actually happened during development and cost real debugging time.

## The extcap deploy trap

This one no longer applies. CANtrip used to capture through a
`can2pcap.exe` that had to be copied by hand into Wireshark's extcap
folder, and a stale copy there made fixes look like they weren't working.
CANtrip now reads the hardware itself, so there is nothing to copy and
nothing that can go stale.

## PEAK: capture stops immediately after Start

Two different real causes have produced this exact symptom:

1. **A second process is still holding the channel.** PCAN-Basic (PEAK's
   driver) does not allow a second handle to initialize a channel another
   process already has open - another tool using the adapter, or a
   second CANtrip window on the same channel. Close whatever else has the
   channel open and try again.
2. **Something else on the machine already has the channel open** with
   exclusive configuration rights, and [Request bus configuration](hardware-tab.md#request-bus-configuration)
   is checked. Uncheck it to join as listen-only instead, assuming that
   other application has already configured the bus the way you need.

If you ever see a raw `PCAN_ERROR_INITIALIZE` in a log or error message,
it's this same situation: PCAN-Basic refusing a second handle on an
already-active channel.

## Why does listen-only behave differently between Vector and PEAK?

They genuinely support it differently at the driver level. Vector's XL
Driver Library has a real permission-mask concept: opening a port with a
zero permission mask joins a channel without requesting configuration
rights, cleanly, as a second independent handle. PCAN-Basic has no
equivalent - there's no second-handle path onto an already-active channel
at all, full stop. This is why [Send Message](stimulation-tab.md) had to
be built completely differently for the two vendors under the hood. See
[Architecture: Send Message Internals](../architecture/send-message-internals.md)
for the real mechanism on each side.

## Can I send on a channel I don't have configuration rights on?

At the moment, this is deliberately left as an open question rather than
a documented guarantee either way - a real collision risk exists if two
uncoordinated applications both try to transmit on the same channel at
once. Don't rely on undefined behavior here; if you need to both monitor
and transmit, do so from the application that actually holds configuration
rights on that channel.

## The Trace/Graph view UI froze on a busy bus

This was a real bug, root-caused and fixed: a sufficiently busy bus could
produce frames faster than decode-and-repaint could keep up, backing up
until the UI stopped responding. Fixed by the
[Display Rate](home-tab.md#display-rate) throttle - if you're on an old
build without it, or you've selected "Unlimited" and have a genuinely busy
bus, switch to 30/10/5 Hz.
