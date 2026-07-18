!!! warning "Not implemented yet"
    Everything on this page describes the **planned design** for CANtrip
    v1.5's headless mode. None of it exists as working code today. This
    page is a design document (and will become the spec to implement
    against), not user instructions - don't follow it expecting it to work.

# CLI & Headless Mode

## Goals

A CLI-drivable way to start/stop a capture, configure the bus, send
messages, and log to a file - **without the GUI** - for CI pipelines and
automated test benches. The motivating case: running a bus-load or
regression check as part of an automated test rig, where nobody's watching
a window.

## Explicit non-goal

This is **not** a general scripting/automation language. That's a
separate, much larger piece of work slated for v3.0 (a CAPL-equivalent,
alongside UDS/diagnostics support) - see the project roadmap. Headless mode
is deliberately scoped to "drive the existing capture/log/send pipeline
from a command line," not "write arbitrary test logic."

## Why this is realistic for v1.5, not v3.0

Every piece this needs already exists as a plain, non-GUI class, following
the discipline established since `TsharkCapture`/`ILogWriter` were first
built:

- `TsharkCapture` - starts/stops a capture, no GUI dependency.
- `ILogWriter` (`AscLogWriter`/`CsvLogWriter`) - writes frames to a file.
- `MessageSender` - the cyclic transmit scheduler and both Send Message
  paths (see [Architecture: Send Message Internals](../architecture/send-message-internals.md)).
- Every `common/` backend - vendor hardware access.

`can2pcap.exe` is already living proof the underlying capture pipeline
works completely headless today - it's a console program with no Qt
dependency at all. Headless mode for `cantrip.exe` itself is "wire a CLI
onto what already exists," not a rebuild.

## Proposed CLI surface

A concrete idea worth locking in early: reuse [`.rune` files](../user-guide/runes.md)
directly as the headless config format, rather than inventing a second one.
A rune already carries channel selection, bus timing, and DBC path - the
exact things a headless run needs to know before it can start.

```
cantrip --headless --rune myconfig.rune --duration 60 --log out.asc
```

Rough shape of what such a flag set would cover:

| Flag | Purpose |
|---|---|
| `--headless` | Run without opening any window |
| `--rune <path>` | Load channel/bus config/DBC from an existing rune |
| `--duration <seconds>` | Capture for a fixed time, then exit cleanly |
| `--log <path>` | Log output path (format inferred from extension, `.asc`/`.csv`) |
| `--send <path>` | A rune (or its transmit-message subset) whose configured messages should also be sent during the run |

Not locked in - the exact flag names/shapes should get revisited once this
is actually scoped for real, this is a starting point, not a commitment.

## Open design questions

- **DBC decode extraction**: signal-level decode currently lives inside
  `MainWindow` (`messageById_`, `resolveMessageName()`,
  `populateDecodedChildren()`) - see
  [Architecture: Data Flow](../architecture/data-flow.md#where-each-responsibility-actually-lives).
  A headless run that wants decoded signals in its log (not just raw
  frames) needs this extracted into a standalone `DbcDecoder` class first.
  This is the one real prerequisite, not just a nice-to-have refactor.
- **Exit conditions beyond a fixed duration**: run until Ctrl+C? Run until
  a specific frame/error is seen? Not scoped yet.
- **Output format for a headless run's own status/errors**: stdout text,
  structured JSON, exit codes only? Matters if this is meant to be
  CI-pipeline-friendly.
- **Whether `--send` should support the full cyclic scheduler** or just a
  simpler "send these once at start" mode for the first version.

## See also

- [Architecture: Overview](../architecture/overview.md) for how the
  existing capture pipeline is structured.
- [Architecture: Rune Persistence](../architecture/rune-persistence.md) for
  the `.rune` JSON shape this would reuse.
