# Headless Mode

`cantrip.exe --headless` runs a capture (and optionally sends configured
messages) with **no GUI window at all** - for CI pipelines and automated
test benches. Built on `QCoreApplication`, not `QApplication`: nothing in
this code path pulls in Qt Widgets, so there's no window to create even
transiently.

```
cantrip --headless --channel "PEAK PCAN_USBBUS1" --duration 60 --log out.asc
```

## Goal and non-goal

The goal: a CLI-drivable way to start a capture, configure the bus, send
messages, and log to a file, entirely from a command line, for automated
test rigs where nobody's watching a window.

The explicit non-goal: this is **not** a general scripting/automation
language. That is a separate, much larger piece of work planned for a
later release (a CAPL-equivalent, alongside UDS/diagnostics support).
Headless mode is deliberately scoped to "drive the existing capture/log/
send pipeline from a command line," not "write arbitrary test logic." If
you need conditional logic, retries, or multi-step orchestration, wrap
`cantrip --headless` invocations in whatever scripting environment your
CI already uses (PowerShell, Python, a Makefile, GitHub Actions steps) -
that's the intended integration point today, not something headless mode
tries to do internally.

## What it's built on

Every piece existed as a plain, non-GUI class before headless mode
shipped - `HeadlessRunner` (`app/HeadlessRunner.h/.cpp`) is genuinely
just CLI parsing and wiring on top of them, not new capture/log/send
logic:

- **`TsharkCapture`** - starts/stops a capture. Same class the GUI uses.
- **`ILogWriter`** (`AscLogWriter`/`CsvLogWriter`) - writes frames to a
  file, identical format to what the GUI's Logging tab produces.
- **`MessageSender`** - the cyclic transmit scheduler and both Send
  Message paths (see
  [Architecture: Send Message Internals](architecture/send-message-internals.md)).
- **`DbcDecoder`** - DBC load + signal decode, extracted out of
  `MainWindow` specifically to make headless mode possible (it was the
  one real prerequisite - decoding needs to work without a window to
  host it). See
  [Architecture: Data Flow](architecture/data-flow.md#where-each-responsibility-actually-lives).

`can2pcap.exe` was already living proof the underlying capture pipeline
works completely headless. Headless mode is `cantrip.exe` itself getting
that same property, rather than a parallel reimplementation.

## Synopsis

```
cantrip.exe --headless [OPTIONS]
```

`--headless` must be present, or CANtrip launches its normal GUI and
every other flag below is ignored. It's checked in `main.cpp` before
either `QApplication` or `QCoreApplication` is constructed - this is what
decides, at the very first line of `main()`, whether a window is even
possible for this run.

At minimum, one of `--channel` or `--rune` is required - a headless run
has to know what to capture from. Everything else is optional.

## Flag reference

### `--headless`

Type: switch (no value). **Required** to enable headless mode at all.
Without it, every flag below is silently ignored and CANtrip opens its
normal GUI window instead.

### `--rune <path>`

Type: file path. Optional.

Loads channel display name, bus timing (nominal/data bitrate, FD on/off,
full bit-timing register values), the DBC path, and any configured
transmit messages from an existing `.rune` file - the exact same file
format [Save Rune](user-guide/home-tab.md#configuration-runes) in the GUI
produces. See [Runes](user-guide/runes.md) for what a rune contains and
[Architecture: Rune Persistence](architecture/rune-persistence.md) for
its JSON shape.

```
cantrip.exe --headless --rune C:\bench\vn1640a-500k.rune --duration 30 --log out.asc
```

If the file doesn't exist or fails to parse, `HeadlessRunner` reports the
error to stderr and exits `1` immediately - no capture is attempted.
Every value a rune supplies can still be overridden by the flags below,
field by field (see [Config Resolution](#config-resolution-order)).

### `--channel <name>`

Type: string (exact match). Required if `--rune` isn't given; optional
(as an override) if it is.

The channel's **display name** - the same string shown in the GUI's
Network Hardware dropdown and stored in `RuneConfig::channelDisplayName`.
Matching is case-sensitive and exact, not a substring search. Two forms:

- The synthetic test source: exactly
  `"CANtrip synthetic test source (no hardware needed)"` - always
  available, needs no hardware or vendor driver installed. This is the
  only path fully testable without real CAN hardware.
- A real vendor channel: `"<vendor display name> <channel name>"`, e.g.
  `"PEAK PCAN_USBBUS1"` or `"Vector VN1640A Channel 1"` - built from
  whatever `probeAvailableBackends()` finds installed on the machine.
  Run the GUI once and check the Network Hardware dropdown to get the
  exact string if you're not sure.

```
cantrip.exe --headless --channel "CANtrip synthetic test source (no hardware needed)" --duration 10
```

If no channel resolves (typo, hardware unplugged, driver not installed),
`HeadlessRunner` reports `Channel not found: <name>` to stderr and exits
`1`.

### `--bitrate <bps>`

Type: unsigned integer, bits per second. Default: `500000` (500 kbit/s).

Classic CAN nominal bitrate. Only takes effect when not overridden by a
loaded rune, or explicitly to override one:

```
cantrip.exe --headless --channel "PEAK PCAN_USBBUS1" --bitrate 250000 --duration 10
```

### `--fd`

Type: switch (no value). Default: off (classic CAN).

Enables CAN FD. When combined with `--rune`, explicitly passing `--fd`
overrides the rune's own FD setting (even to turn it *off* if the rune
had it *on* - passing the flag at all counts as an explicit override,
tracked separately from the rune's loaded value).

```
cantrip.exe --headless --channel "PEAK PCAN_USBBUS1" --fd --data-bitrate 2000000 --duration 10
```

### `--data-bitrate <bps>`

Type: unsigned integer, bits per second. Default: `2000000` (2 Mbit/s).

CAN FD data-phase bitrate - only meaningful alongside `--fd`. Same
override rule as `--bitrate`.

### `--dbc <path>`

Type: file path. Optional.

Loads a DBC to decode against, via the same `DbcDecoder` the GUI uses.
Supplements or overrides a rune's own `dbcPath` (if `--dbc` is given, it
wins). Decoded message names appear in the log's `MessageName` column
(see [Logging Tab: Format](user-guide/logging-tab.md#format) for the
full CSV column list); this flag does **not** control what gets logged
otherwise (that's `--log`).

```
cantrip.exe --headless --channel "PEAK PCAN_USBBUS1" --dbc C:\dbc\vehicle.dbc --log out.csv --duration 60
```

If the DBC fails to parse, the actual dbcppp parser diagnostic is
included in the stderr message (same as the GUI's Import DBC error
dialog), and the run exits `1` without starting a capture.

### `--log <path>`

Type: file path. Optional (a run with no `--log` still captures and can
still `--send`, it just doesn't write anything to disk).

Output log file. Format is inferred purely from the extension: a path
ending in `.csv` (case-insensitive) produces `CsvLogWriter` output;
anything else produces `AscLogWriter` output (CANalyzer-compatible ASC).
There is no separate `--format` flag - name the file with the extension
you want.

```
cantrip.exe --headless --channel "..." --log C:\logs\run.asc --duration 300
cantrip.exe --headless --channel "..." --log C:\logs\run.csv --duration 300
```

If the file can't be opened for writing (bad path, permissions), the run
exits `1` before capturing anything. Both formats are byte-identical to
what the GUI's Logging tab produces - same `ILogWriter` implementations,
driven the same way. No headless-only log schema exists.

### `--send`

Type: switch (no value). Default: off.

Starts the cyclic transmit scheduler for whatever transmit messages the
loaded `--rune` contains, using the exact same `MessageSender` the GUI's
[Stimulation tab](user-guide/stimulation-tab.md) uses (both the Vector
listen-only-port path and PEAK's named-pipe path - see
[Architecture: Send Message Internals](architecture/send-message-internals.md)).

**Requires `--rune`.** A bare `--channel` run has no transmit messages
configured anywhere to send - passing `--send` without `--rune` is a
config error (exit `1`, reported before capturing starts).

```
cantrip.exe --headless --rune C:\bench\with-messages.rune --send --duration 30 --log out.asc
```

Timing detail worth knowing: the transmit port doesn't open immediately.
`HeadlessRunner` waits **2 seconds** after `capture_.start()` before
attempting to open it (a fixed delay, not configurable) - this mirrors
the GUI's own lazy-open behavior (`MainWindow::ensureSenderPortOpen()`),
which exists because opening the transmit port immediately can race
against `can2pcap.exe` still finishing its own startup. If opening the
port fails after that delay, the failure is reported to stderr but the
run **keeps capturing and logging** - a failed send never kills an
otherwise-working capture (same graceful-degradation philosophy as the
GUI).

### `--duration <seconds>`

Type: integer, seconds. Optional - omit to run until Ctrl+C.

```
cantrip.exe --headless --channel "..." --duration 120 --log out.asc
```

See [Exit Conditions](#exit-conditions) below for the full shutdown
sequence this triggers.

## Config resolution order

1. Start from `--rune <path>` if given - it supplies channel, full bus
   timing, DBC path, and transmit messages as the baseline.
2. Apply `--channel`, `--bitrate`, `--fd`, `--data-bitrate`, `--dbc`
   **only if each was explicitly passed** - each is tracked independently,
   so passing `--bitrate` alone overrides just the bitrate, leaving FD
   mode, data bitrate, and channel exactly as the rune specified them.
3. With no `--rune`, flags are the only source, and `--channel` becomes
   mandatory (there's nothing else to supply one). This was a deliberate
   design decision: an earlier draft effectively required a rune to exist
   first, which meant a genuinely fresh CI runner (one that's never had
   CANtrip's GUI touch it) couldn't configure a run at all. Plain flags
   fix that - both are first-class, neither is a fallback for the other.
4. Channel resolution happens last, after all overrides are applied, and
   is the first thing that can fail with "Channel not found."

Every config problem - bad channel, unreadable rune, unparseable DBC,
unopenable log file - fails fast with a clear stderr message and exit
`1`, **before** the event loop ever starts. A headless run never begins
capturing on a configuration it isn't sure about.

## Exit conditions

Whichever comes first:

- **`--duration` elapses** - a `QTimer::singleShot`-driven clean stop.
- **Ctrl+C, Ctrl+Break, or the console window closing** - handled via
  the Windows `SetConsoleCtrlHandler` API (not the portable C
  `<csignal>`/`signal()` API, which didn't cooperate with this project's
  Qt/MSVC toolchain combination; `SetConsoleCtrlHandler` is the more
  correct native-Windows mechanism regardless, and this project is
  Windows-only). The handler itself runs on a separate OS thread and
  only sets an atomic flag - `HeadlessRunner` polls that flag from an
  ordinary `QTimer` on the main thread every 200ms, rather than touching
  Qt/GUI-thread state directly from the handler thread.
- **A fatal capture error** - `TsharkCapture` stopping on its own (a real
  startup failure) rather than via a stop headless mode itself requested
  → exit code 1.

On any of these, in order: the transmit port closes if `--send` had
opened one, the capture stops, the log file (if any) is flushed and
closed, a one-line summary prints to stdout, and the process exits.

## Exit codes

| Code | Meaning |
|---|---|
| `0` | Clean stop - `--duration` elapsed, or Ctrl+C/Ctrl+Break/console close was handled |
| `1` | Either a config problem that prevented starting at all (bad channel, unreadable rune, unparseable DBC, unopenable log file, `--send` without `--rune`, an unrecognized flag), or a fatal capture error partway through a run |

## Output reference

Everything headless mode prints goes to stdout or stderr as plain text -
there is no structured (JSON) output format today. Every message is
prefixed `cantrip: `.

On successful start (stdout):

```
cantrip: capturing on '<channel display name>'[, logging to <path>]
```

The `, logging to <path>` clause only appears when `--log` was given. On
any exit, the final line (stdout):

```
cantrip: captured <N> frame(s) in <S>.<D>s
```

`<N>` is the total frame count (every frame that reached
`onFrameReceived`, both received and, if `--send` was active,
transmitted). `<S>.<D>` is wall-clock elapsed time since `capture_.start()`,
one decimal place.

Config errors (stderr, before any capture starts) are a single line,
followed immediately by exit `1` - for example:

```
cantrip: No channel specified - pass --channel <name> or --rune <path>.
```

```
cantrip: Unknown argument: --bogus

Usage: cantrip --headless [--rune <path>] [--channel <name>] [--bitrate <bps>] [--fd] [--data-bitrate <bps>] [--dbc <path>] [--log <path>] [--send] [--duration <seconds>]
```

Runtime warnings during a capture (tshark's own stderr chatter, e.g.
driver diagnostics) also go to stderr, prefixed the same way, but do
**not** by themselves stop the run or affect the exit code - only a
genuinely fatal capture error does that.

## Worked examples

**Minimal - synthetic source, no hardware needed, no logging:**

```
cantrip.exe --headless --channel "CANtrip synthetic test source (no hardware needed)" --duration 5
```

**Real hardware, classic CAN, log to ASC:**

```
cantrip.exe --headless --channel "PEAK PCAN_USBBUS1" --bitrate 500000 --log C:\logs\bench01.asc --duration 300
```

**Real hardware, CAN FD, log to CSV with decoded signal names:**

```
cantrip.exe --headless --channel "Vector VN1640A Channel 1" --fd --data-bitrate 2000000 --dbc C:\dbc\vehicle.dbc --log C:\logs\fd-run.csv --duration 120
```

**Reuse a saved rune's full config, override just the log destination:**

```
cantrip.exe --headless --rune C:\bench\standard-setup.rune --log C:\logs\nightly-2026-07-19.asc --duration 3600
```

**Reuse a rune, and also transmit its configured test messages:**

```
cantrip.exe --headless --rune C:\bench\with-messages.rune --send --log C:\logs\stimulus-run.csv --duration 60
```

**Run until manually stopped (no `--duration`), for an interactive bench
session you'll Ctrl+C when done:**

```
cantrip.exe --headless --channel "PEAK PCAN_USBBUS1" --log C:\logs\manual-session.asc
```

**In a CI script, checking the exit code (PowerShell):**

```powershell
& cantrip.exe --headless --channel "PEAK PCAN_USBBUS1" --duration 60 --log $env:LOG_PATH
if ($LASTEXITCODE -ne 0) {
    Write-Error "CANtrip capture failed"
    exit 1
}
```

## Verified vs. not yet independently verified

Confirmed working for real, in this environment:

- The no-args/bad-config error path (clean message, exit 1, no window
  ever appears).
- A full capture + log run against the synthetic source, both `.asc` and
  `.csv`, `--duration`-triggered clean exit - producing a complete, valid
  log file with the exact frame count reported in the summary.
- `--dbc` against the synthetic source - the log's MessageName column
  resolves correctly with zero `MainWindow` in the process at all.

Not independently verified in this environment:

- **Real OS-level Ctrl+C delivery specifically.** Console attachment
  doesn't carry over cleanly through this project's own test tooling.
  The shutdown logic itself (`stopAndFinish()`) is exercised identically
  by the `--duration` path above - confirmed correct - only the
  *trigger* differs for Ctrl+C. If you hit something odd here on a real
  terminal, that's the part to look at first.
- **`--send` against real hardware.** Reuses `MessageSender`, already
  verified working on real Vector and PEAK hardware for the GUI's own
  Send Message feature - but the headless wiring around it (the port-open
  timing specifically) is new and hasn't had the same real-hardware pass.

## Still open / not built yet

- Structured (JSON) status output.
- `--send` as an independent second file/path, rather than requiring the
  same `--rune` that supplies bus config.
- Any exit condition beyond duration/Ctrl+C/fatal-error - e.g. "stop on
  seeing frame ID X" is arguably shaped like the future scripting
  feature (v3.0), not something headless mode itself should grow.

## See also

- [Architecture: Overview](architecture/overview.md) for how the capture
  pipeline is structured.
- [Architecture: Rune Persistence](architecture/rune-persistence.md) for
  the `.rune` JSON shape `--rune` loads.
- [Runes](user-guide/runes.md) for how to actually produce a `.rune` file
  in the first place (today, always via the GUI's Save Rune).
