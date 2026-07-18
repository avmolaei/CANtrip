# Troubleshooting & FAQ

Built from CANtrip's own real incidents, not hypothetical ones - each of
these actually happened during development and cost real debugging time.

## The extcap deploy trap

**Symptom**: you rebuilt CANtrip (or updated to a new release) and a fix
that should be there "still doesn't work", with no obvious error.

**Cause**: `can2pcap.exe`, the process Wireshark/`tshark` actually invokes
for capture, lives at `%APPDATA%\Wireshark\extcap\can2pcap.exe` - a
manually-installed copy (see [Getting Started](getting-started.md#installing)).
Rebuilding CANtrip only updates the copy in the build output folder (or a
new release zip); the deployed copy in Wireshark's extcap folder is a
**separate file that never updates itself**. `cantrip.exe` is launched
directly from wherever you extracted/built it, so it's always current -
this specifically only bites `can2pcap.exe`-side changes.

**Fix**: re-copy the fresh `can2pcap.exe` over the one in
`%APPDATA%\Wireshark\extcap\` after every update, same as the
[install step](getting-started.md#installing). If in doubt, compare file
modified timestamps between the two copies.

## PEAK: capture stops immediately after Start

Two different real causes have produced this exact symptom:

1. **A second process is still holding the channel.** PCAN-Basic (PEAK's
   driver) does not allow a second handle to initialize a channel another
   process already has open - a leftover `can2pcap.exe`/`tshark.exe`/
   `dumpcap.exe` process from a previous session (for example, one that
   got orphaned by force-killing `cantrip.exe` rather than clicking Stop
   first) can hold the handle open indefinitely. Check for and close any
   lingering `can2pcap.exe`/`tshark.exe`/`dumpcap.exe` processes and try
   again.
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
