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
[Headless Mode](../headless-mode.md), which relies on exactly this.

## Where signal-level decode lives

Neither `ILogWriter` nor `LogReplaySource` decode DBC signals themselves -
both only work with the raw `DecodedCanFrame`. The `MessageName` column
`CsvLogWriter`/`AscLogWriter` write comes from a small injected callback
(`MessageNameResolver`, a `std::function`), backed by `DbcDecoder`'s
`resolveMessageName()` (`app/DbcDecoder.h/.cpp`, see
[Data Flow](data-flow.md#where-each-responsibility-actually-lives)) -
either `MainWindow`'s own instance or, headless,
[`HeadlessRunner`](../headless-mode.md)'s. Full per-signal
decode (`DbcDecoder::decodeSignals()`, used for the Trace view's expandable
rows and Graph view plotting) is a separate call `MainWindow` makes for
its own UI needs - logging was never coupled to that heavier path, and
never needed `MainWindow` to have "already decoded" anything first.
