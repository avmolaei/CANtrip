# Runes

A `.rune` file is CANtrip's answer to CANalyzer's `.cfg` files: a saved
snapshot of your whole environment, so you don't have to rebuild it by hand
every session. **Save Rune...** / **Load Rune...** live on the
[Home tab](home-tab.md#configuration-runes).

![Runes](../images/rune.png)

## What's saved

- **Channel** - matched back by display name (not the internal
  vendor-namespaced ID) when loading, so it survives things like a
  different USB port. If the named channel isn't currently available, the
  rune still loads, it just leaves the current channel selection alone and
  warns you.
- **Bus configuration** - bitrate, FD on/off, and full nominal/data timing
  values (see [Hardware Tab](hardware-tab.md#can-controller)).
- **Display mode and rate** - [Home tab's](home-tab.md#display) Waterfall/
  Periodic choice and display rate.
- **DBC path** - a path reference, not an embedded copy; re-imported from
  disk the same way [Import DBC](analysis-and-measurement-tab.md#import-dbc)
  does. An empty path means no DBC was loaded when the rune was saved.
- **Graph view layout** - every open graph window's full axis/signal setup
  (see [Graph view](analysis-and-measurement-tab.md#graph-view)) - not the
  plotted data itself, that's always live/session-only.
- **Stimulation messages** - every configured [transmit message](stimulation-tab.md#new-message).
- **Trace view column layout** - column order and widths, exactly as you
  left them.
- **Listen-only setting** - the [Request bus configuration](hardware-tab.md#request-bus-configuration)
  checkbox state.

## Backward compatibility

There's no file-format version number. Fields added after the `.rune`
format's first version just fall back to a sensible default if the JSON key
is missing (Qt's `QJsonValue` does this automatically) - a rune saved by an
older CANtrip loads fine in a newer one, it just leaves whatever-was-added-
since at its default (for example, an old rune with no saved column layout
just leaves the Trace view at its current column arrangement, rather than
erroring or resetting anything). See
[Architecture: Rune Persistence](../architecture/rune-persistence.md) for
the actual JSON shape and the one field that needed a real shape migration
rather than just a default.
