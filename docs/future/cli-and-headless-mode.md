# CLI & Headless Mode

`cantrip.exe --headless` runs a capture (and optionally sends configured
messages) with **no GUI window at all** - for CI pipelines and automated
test benches. Built on `QCoreApplication`, not `QApplication`: nothing in
this path pulls in Qt Widgets.

```
cantrip --headless --channel "PEAK PCAN_USBBUS1" --duration 60 --log out.asc
cantrip --headless --rune myconfig.rune --send --duration 30 --log out.csv
```

## Explicit non-goal

This is **not** a general scripting/automation language. That's a
separate, much larger piece of work slated for v3.0 (a CAPL-equivalent,
alongside UDS/diagnostics support) - see the project roadmap. Headless
mode is deliberately scoped to "drive the existing capture/log/send
pipeline from a command line," not "write arbitrary test logic."

## Flags

| Flag | Meaning |
|---|---|
| `--headless` | Required to enable this mode at all (parsed in `main.cpp` before either `QApplication` or `QCoreApplication` is constructed) |
| `--rune <path>` | Load channel/bus config/DBC path/transmit messages from a `.rune` file |
| `--channel <name>` | Channel display name - same matching convention as `RuneConfig::channelDisplayName`, including the synthetic test source ("CANtrip synthetic test source (no hardware needed)"). Alternative to `--rune`, or overrides its channel |
| `--bitrate <bps>` | Classic bitrate (default 500000) - only applied without `--rune`, or as an explicit override on top of one |
| `--fd` / `--data-bitrate <bps>` | CAN FD on + data bitrate - same override rule |
| `--dbc <path>` | DBC to decode against - overrides/supplements a rune's own `dbcPath` |
| `--log <path>` | Output log file; format inferred from the extension (`.csv` → CSV, anything else → ASC) |
| `--send` | Also start the cyclic scheduler for the rune's configured transmit messages. Requires `--rune` (a bare `--channel` run has no messages configured to send) |
| `--duration <seconds>` | Run this long then exit cleanly; omit to run until Ctrl+C |

Config resolution: start from `--rune` if given, then any of
`--channel`/`--bitrate`/`--fd`/`--data-bitrate`/`--dbc` explicitly passed
overrides the corresponding rune value. A bad channel, unreadable rune, or
unparseable DBC all fail fast with a clear stderr message and a non-zero
exit, before the event loop ever starts - never starts capturing on a
config it isn't sure about.

## What it's built on

Every piece was already a plain, non-GUI class before this shipped -
`HeadlessRunner` (`app/HeadlessRunner.h/.cpp`) is genuinely just CLI
parsing and wiring on top of them, not new capture/log/send logic:

- `TsharkCapture` - starts/stops a capture.
- `ILogWriter` (`AscLogWriter`/`CsvLogWriter`) - writes frames to a file.
- `MessageSender` - the cyclic transmit scheduler and both Send Message
  paths (see [Architecture: Send Message Internals](../architecture/send-message-internals.md)).
- `DbcDecoder` (`app/DbcDecoder.h/.cpp`) - DBC load + signal decode,
  extracted out of `MainWindow` specifically to make this possible (it
  was the one real prerequisite - see
  [Architecture: Data Flow](../architecture/data-flow.md#where-each-responsibility-actually-lives),
  now updated to reflect the extraction).

`can2pcap.exe` was already living proof the underlying capture pipeline
works completely headless - this is `cantrip.exe` getting the same
property.

## Exit conditions

Whichever comes first:

- `--duration` elapses.
- Ctrl+C (or the console window closing) - handled via
  `SetConsoleCtrlHandler`, not `<csignal>`/`signal()` (that header didn't
  cooperate with this Qt/MSVC toolchain combination; `SetConsoleCtrlHandler`
  is the more correct native-Windows mechanism anyway, and this project is
  Windows-only). The handler runs on its own OS thread and only sets an
  atomic flag - `HeadlessRunner` polls it from a normal `QTimer` on the
  main thread rather than touching Qt state from the handler thread
  directly.
- A fatal capture error (`TsharkCapture` ending on its own rather than via
  a requested stop) → exit 1.

On any of these: capture stops, the transmit port closes if it was open,
the log file is flushed and closed, a one-line frame-count/duration
summary prints to stdout, then the process exits.

## Verified

- No-args error path: clear stderr message, exit 1, no window.
- Real capture + log against the synthetic source, both ASC and CSV,
  `--duration`-triggered clean exit - confirmed producing a complete,
  valid log file with the exact frame count reported.
- `--dbc` against the synthetic source - confirmed the log's MessageName
  column resolves correctly (`EngineData`, `TestWaveforms`, etc.) with no
  `MainWindow` in the process at all.
- **Not independently verified in this environment**: real OS-level
  Ctrl+C delivery specifically (console attachment doesn't carry over
  cleanly through this project's own test tooling). The shutdown path
  itself (`stopAndFinish()`) is exercised identically by the `--duration`
  path above and confirmed correct; only the *trigger* differs. `--send`
  against real hardware is also unverified here - needs the same
  real-hardware pass Send Message itself already went through.

## Still open

- Structured (JSON) status output - plain stdout/stderr text for now.
- `--send` as an independent second file/path, rather than tied to
  `--rune` only.
- Any exit condition beyond duration/Ctrl+C/fatal-error (e.g. "stop on
  seeing frame ID X") - arguably v3.0-scripting-shaped, not headless
  mode's job.

## See also

- [Architecture: Overview](../architecture/overview.md) for how the
  capture pipeline is structured.
- [Architecture: Rune Persistence](../architecture/rune-persistence.md)
  for the `.rune` JSON shape `--rune` loads.
