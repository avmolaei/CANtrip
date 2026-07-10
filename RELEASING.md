# Releasing CANtrip

## Version naming

Tags look like `vMAJOR.MINOR.PATCH_codename`, e.g. `v1.1.0_yukari`.

**The codename rule: one name per major version line, not per release.**
Every `v1.x` release uses **yukari** — `v1.0.0_yukari`, `v1.1.0_yukari`,
`v1.2.0_yukari`, and so on, all share the name. Only bumping the major
version (`v2.0.0`) gets a new codename. This is the same idea as Android's
dessert names or Ubuntu's animal names: the codename identifies the release
*line*, not each individual point release.

This differs from CANtrip's own pre-1.0 history, where every release got a
distinct name (`v0.1_mugi`, `v0.2_dawn`, `v0.3_miyuki`, `v0.4_mai`,
`v0.5_Sawako`). That per-release-naming approach is retired as of v1.0;
don't follow it for new releases.

Names are anime girl names, picked when a new major version starts. No
fixed list - pick something when you get there.

| Tag | Codename introduced |
|---|---|
| v0.1_mugi ... v0.5_Sawako | one-off names, pre-1.0 only |
| v1.0.0_yukari, v1.1.0_yukari, ... | yukari (all of v1.x) |

## Cutting a release

1. **Bump the version deliberately** - decide MAJOR.MINOR.PATCH based on
   what changed since the last tag (`git log <last-tag>..HEAD --oneline`).
   New features → bump MINOR. Fixes only → bump PATCH. Breaking/architecture
   changes → bump MAJOR (and pick a new codename).

2. **Build Release configuration:**
   ```powershell
   cmake --build build --config Release
   ```

3. **Stage the package contents** into a folder (matches what the app
   needs to run standalone):
   - `build\app\Release\*` (the exe + all Qt/dbcppp DLLs windeployqt copied
     alongside it)
   - `build\extcap\Release\can2pcap.exe`
   - `test\sample.dbc`
   - `README.md`
   - `LICENSE`

4. **Zip it**, named `CANtrip-vMAJOR.MINOR.PATCH_codename.zip`.

5. **Verify before shipping**: extract the zip fresh to a *different*
   folder than the one you built from, launch `cantrip.exe` from there, and
   confirm it starts and stays running. This has caught real packaging bugs
   before (missing DLLs, stale files). Don't skip it just because the build
   succeeded.

6. **Tag and push:**
   ```
   git tag -a vMAJOR.MINOR.PATCH_codename -m "CANtrip vMAJOR.MINOR.PATCH codename"
   git push origin vMAJOR.MINOR.PATCH_codename
   ```

7. **Write release notes** covering what's new since the last tag. A couple of paragraphs plus bullet points is fine, mention *why* a fix
   mattered if it was found against real hardware/data, not just what
   changed.

8. **Create the GitHub Release and upload the zip**, Attach the zip from step 4 and paste in the notes from step 7.

## Notes

- The `release/` folder (build output + zip + release notes drafts) is
  gitignored - it's scratch space for cutting a release, not tracked
  history. If you want release notes preserved in git, copy them somewhere
  tracked before the folder gets cleaned out.
- Don't force-push or rewrite an already-pushed tag. If a release build was
  wrong, cut a new patch version instead.
