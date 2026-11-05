# Better Dofus maintenance

- This project is **Better Dofus**, https://github.com/vDAKK/better-dofus (`origin`),
  shown in game as « par DAKK ». It was renamed on 2026-11-05 from vDAKK/dofus-enhanced
  (GitHub redirects the old name), a fork of Altansar's Dofus Enhanced, first named
  Dofus Miniatures. Keep Altansar credited as the mod's creator, with
  https://github.com/Altansar69/dofus-enhanced, which the user can only read and
  which is not an update source any more: its releases keep the former names.
- Always build with `build.ps1`: its clean build is required, because incremental
  builds miss header changes with this localized compiler.
- The user requested automatic delivery: after an authorized mod update, increment
  the version, build, test, package, commit/push the reviewed sources and publish
  a GitHub release. Do not leave a completed revision local-only.
- Never add Claude or Anthropic attribution (Co-Authored-By trailers, "Generated
  with" lines) to commits, release notes or documentation.

## Layout

- `src/<area>/`: the mod's sources, which include each other as `"<area>/<file>"`
  from `src/`. Areas: core, loader, ui, appearance, animations, glow, souls,
  metamob, maps, travel, treasure, notebook, jobs, social, stats.
- `tests/<area>/`: the offline tests, one program per feature, registered with
  `dofus_test()` in `CMakeLists.txt`. `tests/support/` holds the fake native
  runtime fixture and `tests/update/` the updater's PowerShell tests.
- `package/`: files shipped as they are in the release archive
  (`Update-GitHub.ps1`, and `LISEZ-MOI.txt` with the version history).
- `docs/`: the detailed guide and the resource data notes. `tools/`: packaging,
  publishing and data generation. `third_party/`: MinHook (submodule),
  nlohmann/json and the WebView2 SDK.
- The shipped names are part of the release format, and installed loaders and
  updaters depend on them: `BetterDofus.dll`, `BetterDofusLoader.exe`,
  `BetterDofusAutoLoader.exe`, `BetterDofusTreasure.exe`, `Update-GitHub.ps1`, the
  `BetterDofus-X.Y.Z` folders, the `BetterDofus-X.Y.Z.zip` asset, the
  `<!-- better-dofus game=... metadata=... -->` comment of the release notes and the
  shared `BetterDofus-settings.ini`. Never rename them.
- Two old names must stay, because data saved under them would be lost: the Metamob
  key's entropy `DofusEnhanced/Metamob/v1` and the game's saved panel key
  `DofusMiniatures.SoulStonePanel`. `MigrateLegacyData` and the `DofusMiniatures` run
  value and DLL checks exist for installs from before 2.0.0.

## Build, test and release

- Use `build.ps1`, then `ctest --test-dir build --output-on-failure --output-junit test-results.xml`.
  Package with `python tools/package-release.py --output <absolute output directory>`.
  Commit/push before `python tools/publish-release.py --package <X.Y.Z-package.json> --notes <notes.md>`.
  Keep versions immutable. Do not overwrite a published asset or reuse a tag.
- The version X.Y.Z is the only name of a release. A new one sets `ModSemanticVersion`,
  `ModVersion` and `ModRevision` (X*1000000 + Y*1000 + Z) in `src/core/version.hpp`.
  The releases before 2.0.0 keep their tags and `DofusMiniatures-rN-...` assets;
  installs below 2.0.0 cannot update by themselves and need the README's manual step.
- Never publish game assemblies/metadata, installed game assets, user settings,
  logs, personal paths, credentials or analysis snapshots. The runtime archive
  has nine explicitly allowed entries. Keep all third-party licenses.
- The bootstrap is version-independent, but each module and its own loader must
  still pin the exact supported binary AND metadata hashes. Never bypass these
  checks, the native guards or the ABI verification to accept a game update.
- A new game build can require a native port: start with `tools/port/README.md`.
  Validate method signatures, field
  layouts and interior callsites against that exact build before publication.
  The updater distributes a port; it does not generate one.
- Every native method goes through a 16-byte guard in `src/core/signatures.hpp`
  and `FindVerified`; every native field through a checked offset.
- Do not click, type, focus, inject into, terminate or restart an already open
  game without a fresh explicit request. Offline tests and read-only file checks
  are the default. The watcher ignores every game open when it starts.
- Updater changes require regression checks for incompatible clients, corrupt
  assets, partial downloads, traversal, preference retention and offline fallback.
  Verify the published archive through the real unauthenticated updater in an
  isolated directory. No game injection is needed for this check.
- Clearly distinguish offline validation from live initialization/rendering.
