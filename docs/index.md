# CANtrip

CANtrip is an open-source, free alternative to Vector CANalyzer for viewing
and decoding CAN / CAN-FD bus traffic on Windows, licensed GPLv3.

![CANtrip main window](images/hero.png)

This site is the full reference. If you just want to get running in five
minutes, start with [Getting Started](user-guide/getting-started.md). If
you're trying to understand how CANtrip actually works under the hood, or
you're contributing a change, go to [Architecture](architecture/overview.md).

## What's here

- **User Guide** - every ribbon tab and dialog, in full: [Home](user-guide/home-tab.md),
  [Hardware](user-guide/hardware-tab.md),
  [Analysis & Measurement](user-guide/analysis-and-measurement-tab.md),
  [Logging](user-guide/logging-tab.md), [Stimulation](user-guide/stimulation-tab.md),
  [About](user-guide/about-tab.md), [Runes](user-guide/runes.md), and a
  [Troubleshooting & FAQ](user-guide/troubleshooting.md) page built from this
  project's own real incidents.

- **Architecture** - how CANtrip is actually built: the
  [process architecture](architecture/overview.md), the exact
  [data flow](architecture/data-flow.md) from a bit on the wire to a decoded
  signal on screen, the [vendor-neutral backend abstraction](architecture/can-backend-abstraction.md),
  [how Send Message works](architecture/send-message-internals.md) for two
  very differently-behaved vendor SDKs, [logging & replay](architecture/logging-and-replay.md),
  the [Rune persistence format](architecture/rune-persistence.md), and
  [how bus load is actually calculated](architecture/bus-load-calculation.md).

- **[Headless Mode](headless-mode.md)** - `cantrip.exe --headless`: every
  flag, exit condition, and worked example for running a capture with no
  GUI window, for CI pipelines and test-bench automation.

## Quick links

- [GitHub repository](https://github.com/avmolaei/CANtrip)
- [Releases](https://github.com/avmolaei/CANtrip/releases)
- [CONTRIBUTING.md](https://github.com/avmolaei/CANtrip/blob/main/CONTRIBUTING.md)
  for code conventions and how changes get verified
- [RELEASING.md](https://github.com/avmolaei/CANtrip/blob/main/RELEASING.md)
  for the version naming scheme and how a release gets cut
