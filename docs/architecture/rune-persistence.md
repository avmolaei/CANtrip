# Rune Persistence

A `.rune` file (`app/RuneFile.h/.cpp`) is plain JSON. `RuneConfig` is the
in-memory shape; `saveRuneFile()`/`loadRuneFile()` convert it to and from
disk.

```json
{
  "channelDisplayName": "...",
  "busConfig": { "nominalBitrateBps": 500000, "fd": false, "nominalTiming": {...}, "dataTiming": {...} },
  "displayMode": "Waterfall",
  "displayRateMs": 33,
  "dbcPath": "...",
  "graph": { "windows": [ { "axes": [...] } ] },
  "transmitMessages": [ { "id": 0, "extended": false, "fd": false, "dlc": 8, "data": "hex...", "cycleTimeMs": 0, ... } ],
  "traceHeaderState": "hex-encoded QHeaderView::saveState() bytes",
  "listenOnly": false
}
```

## No version field, by design

Rather than a version number gating what gets parsed, every field just
tolerates its own absence. `QJsonObject::operator[]` on a missing key
returns a null `QJsonValue`, and `QJsonValue::toBool(default)`/`toString()`/
`toArray()` etc. all convert that null value to a sensible default rather
than erroring. So a rune saved before a given field existed just loads with
that field at its default - `listenOnly` (added for a `.rune` predating it)
defaults to `false`, matching the behavior that already existed before the
field did.

```cpp
config.listenOnly = root["listenOnly"].toBool(false);
```

## `graphWindows`: the one field that needed a real migration

Everything above is a simple "missing key → default" story. Graph layout
is different, because its *shape itself* changed (single-window → up to 6
windows), not just whether a key exists:

```cpp
if (graph.contains("windows")) {
    // current: array of window objects, each with its own "axes" array
} else if (graph.contains("axes")) {
    // pre-multi-window: one flat "axes" array directly under "graph" -
    // load it as a single window rather than rejecting the file or
    // silently dropping the layout
}
```

This is the pattern to reach for if a future field needs to change shape
rather than just start existing: branch on which shape is actually present
(`contains()`), don't bump a global version number for one field.

## Byte-for-byte persistence: `traceHeaderState`

Trace view column order/widths are Qt's own opaque `QHeaderView::saveState()`
byte blob, not hand-rolled per-column JSON - stored as a hex string the same
way `TransmitMessage::data` (frame payload bytes) already was, and restored
via `QHeaderView::restoreState()`, which is a documented no-op on an
empty/invalid byte array. That's what makes an old rune with no saved
header state safe to load: `restoreState()` on the empty default just
leaves the header exactly as it currently is.
