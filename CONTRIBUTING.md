# Contributing to CANtrip

## Commit messages

For this, Claude said "Plain, professional, short and concise". I disagree. Have fun with it. Just don't forget to still describe what changed and why.

Prefer one commit per coherent change rather than a string of "fix typo", "azeazeazeaze", 
"actually fix it", "wip" commits  

## Naming conventions

There is the only hard naming convention for CANtrip: its vendor-neutral hardware interface is called **the AVlabs CAN
backend** (or "the AVlabs CAN backend interface"): prose and
code alike. The C++ type is `IAvlabsCanBackend` (defined in
`common/AVlabsCanBackend.h`).

## Adding a new vendor backend

1. Implement `IAvlabsCanBackend` (`common/AVlabsCanBackend.h`) in a new
   `common/YourVendorBackend.h/.cpp` pair, following `common/PeakBackend.h/
   .cpp` as the reference implementation.
2. Add one line to `common/CanBackendRegistry.cpp`'s `probeAvailableBackends()`.
3. Nothing else changes


## Verifying changes

Two layers: the automated unit tests, and checking against real behavior.
The tests cover logic that doesn't need hardware. They can't tell you a
vendor driver does what its documentation says, which is what the rest of
this section is about.

### Unit tests

The tests live in `test/unit`, one file per area, built with Qt Test and
run through CTest. They build with the app by default:

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

They take a few seconds and never open a window: anything with widgets
runs on Qt's offscreen platform. Run them before committing, and add to
them when you change something they could have caught:

- A bug fix gets a test that fails without the fix.
- New logic that doesn't need hardware (a model, a parser, a calculation)
  gets tests alongside it.
- To add a test file, create `test/unit/tst_<area>.cpp` and add one
  `cantrip_add_test(tst_<area>)` line to `test/unit/CMakeLists.txt`.

Everything in `app/` except `main.cpp` builds into the `cantrip_app`
static library, which both `cantrip.exe` and the tests link, so the tests
exercise the same code the app runs.

### Checking against real behavior

**The built-in synthetic test source (`CANtrip synthetic test source (no
hardware needed)`, always available as a channel option) plus
`test/sample.dbc` is fine for GUI/behavior/QoL/UX changes** - trace/graph
display, dialogs, ribbon layout, DBC import flow, and anything else where
the actual bytes on the wire aren't the point.

**It is not sufficient for anything low-level: CAN backend code
(`common/*Backend.cpp`), the extcap/serialization layer
(`extcap/can2pcap.cpp`), or performance/system-level changes (display
throttling, batching, anything touching how much work happens per frame).**

The synthetic source produces clean, predictable, low-volume traffic. It
cannot catch real vendor-driver quirks, wrong struct layouts, bit-timing
mismatches, or the kind of sustained real-world bus load that has actually
caused freezes/data-loss bugs in this project before. Several real bugs here were only ever exposed by real hardware or a genuinely busy bus, never by the synthetic source. Changes in this  category need to be field-tested: a real vehicle, a test bench, or a simulation/HIL setup actually outputting CAN traffic, driven through real hardware. 

If you can't field-test a low-level change yourself, say so explicitly rather than reporting it as verified.

For anything touching the capture pipeline (`extcap/can2pcap.cpp`,
`common/*Backend.cpp`, `app/TsharkCapture.cpp`), the fastest way to isolate
a bug is to test each layer independently rather than going straight
through the full GUI:

1. Build just `can2pcap.exe` (`cmake -DCANTRIP_BUILD_APP=OFF` skips Qt
   entirely, much faster iteration).
2. Run it directly: `can2pcap.exe --capture --fifo <file>
   --extcap-interface <id> <bitrate flags>` for a few seconds, then check
   the output file size (24 bytes = header only = zero frames captured;
   larger = frames flowing), so you know whether the
   backend/driver is the problem.
3. Feed that file through a real `tshark -r <file> -T ek` and inspect the
   JSON. This tells you whether the pcap serialization / Wireshark
   dissection layer is the problem, independent of CANtrip's own Qt code.

This isolates backend/driver issues from serialization issues from
dissection issues from Qt/UI issues, without ever needing to touch the GUI
or reproduce anything through drag-and-drop/dialogs. Add temporary
`fprintf(stderr, ...)` diagnostics directly in the backend if a status code
or event tag needs inspecting - cheap to add and remove, just make sure
they're fully reverted (`git diff` before committing) before shipping.

## Building the docs site locally

The [documentation site](https://avmolaei.github.io/CANtrip/) is built with
[MkDocs](https://www.mkdocs.org/) + the [Material theme](https://squidfunk.github.io/mkdocs-material/),
source in `docs/`, config in `mkdocs.yml` at the repo root. To preview
changes locally:

```powershell
pip install mkdocs-material
mkdocs serve
```

Then open `http://127.0.0.1:8000`. It auto-deploys to GitHub Pages via
`.github/workflows/docs.yml` on every push to `main` that touches `docs/**`
or `mkdocs.yml`.

## Build setup

See the README's "Building" section for the day-to-day CMake/Qt invocation.
Toolchain notes if setting up a machine from scratch:

- CMake >= 3.21, MSVC (Build Tools or full VS, C++ workload), Qt 6 with the
  **Charts** addon module specifically (`aqt install-qt ... -m qtcharts` if
  using aqtinstall - a base Qt install doesn't include it, and
  `app/CMakeLists.txt` requires `Widgets Charts`).
- A real Wireshark install (provides `tshark`/`dumpcap`). Npcap is *not*
  required; it only gates normal NIC capture, not extcap-based capture,
  which is CANtrip's only capture path.
- `git submodule update --init --recursive` for `third_party/dbcppp` (a
  real git submodule, not vendored files) before the first configure.
