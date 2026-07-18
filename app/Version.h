// CANtrip's version/codename, shown in the About dialog title. Per
// RELEASING.md, the codename is per-major-version-line, not per-release
// (all of v1.x is "Yukari") - bump kVersion with every release; only
// change kCodename at the next major-version bump. Kept as a manually
// maintained constant (not derived from `git describe` at build time)
// since the release process itself is already manual per RELEASING.md.
#pragma once

namespace cantrip {

constexpr const char* kVersion = "1.4.2";
constexpr const char* kCodename = "Yukari";

} // namespace cantrip
