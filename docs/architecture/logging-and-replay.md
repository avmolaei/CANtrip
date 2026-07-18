# Logging & Replay

## `ILogWriter`

Logging is built on a small interface, `ILogWriter`, with one
implementation per file format:

- **`AscLogWriter`** - a CANalyzer-compatible baseline ASC trace format.
- **`CsvLogWriter`** - a flat CSV, one row per frame (see
  [Logging Tab](../user-guide/logging-tab.md#format) for the exact column
  list).

`MainWindow::onFrameReceived()` calls `logWriter_->writeFrame()` for every
single frame, unconditionally, before any of the display-rate throttling
described in [Data Flow](data-flow.md) - logging is deliberately never
subject to that throttle, it exists purely to keep the UI responsive, never
to drop what gets written to disk.

Both writers handle the `Rx`/`Tx` direction on `DecodedCanFrame` the same
way: a frame [`MessageSender`](send-message-internals.md) transmitted logs
identically to one actually received, just tagged by direction.

## `LogReplaySource`

Log Replay loads a previously-saved file back and feeds it through
`MainWindow::onFrameReceived()` - **the exact same slot** a live
`TsharkCapture` and a live `MessageSender` feed. There is no separate
"replay mode" rendering path: Trace view, Graph view, and DBC decode all
behave completely identically whether the frame came from real hardware
seconds ago or a log file from last week. This is the same non-GUI-class
discipline followed throughout CANtrip - see
[Future: CLI & Headless Mode](../future/cli-and-headless-mode.md) for where
that discipline is heading next.

## Where signal-level decode still lives

DBC decode itself (`populateDecodedChildren()`, `messageById_`,
`resolveMessageName()`) currently lives inside `MainWindow`, not inside
`ILogWriter` or `LogReplaySource` - both of those only work with the raw
`DecodedCanFrame`, and rely on `MainWindow` having already been the one to
call into dbcppp. This is the one piece of otherwise-plain-class logic
that's still GUI-locked - see
[Future: CLI & Headless Mode](../future/cli-and-headless-mode.md#open-design-questions).
