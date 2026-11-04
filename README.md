# NPC Behavior Bridge 0.6.7

NPC Behavior Bridge is an SKSE plugin that restores selected NPC base-record gameplay changes when later plugins revert them to baseline values.

It follows three steps:

1. Read the configured fields and compare their vanilla and configured baseline values.
2. Follow load order and select each field's last value that differs from both baselines. If none exists and the final value reverts to vanilla, use the configured baseline when it differs from vanilla.
3. Restore that value when later records revert to either baseline.

It does not invent schedules or combat behavior. AI Overhaul and USSEP are optional; no mod-name whitelist is used. Lists are selected whole rather than merged entry by entry. The default configuration uses USSEP as an additional baseline, falling back when unavailable.

The plugin handles enabled gameplay fields, template inheritance, related stat calculation and supported script attachments. It protects appearance, race, voice and outfits; restoring a gameplay template may change an inherited name. Base fields are processed once after game data loads. Subsequent save/load, waiting and location checks do not repeatedly force the values back.

This repository contains the source corresponding to **0.6.7**, build instructions, pinned dependency submodules and license notices. Local test fixtures, game data, sampling reports, deployment paths, runtime configuration and binary packages are not included. Use the configuration files supplied with the matching mod release.

See [CHANGELOG.md](CHANGELOG.md) for release notes.

## Building

Requires Windows, Visual Studio 2022 with C++ tools and a Windows SDK, CMake 3.24 or later, and Git. Python 3.9 or later is used by the dependency helper.

Clone this repository with `--recurse-submodules`, or initialize dependencies after cloning:

```sh
git submodule update --init --recursive
python tools/prepare_dependencies.py --check
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DNBB_BUILD_TESTS=OFF
cmake --build build --config Release --target NPCBehaviorBridge nbb_cli --parallel 4
```

If using a downloaded source ZIP, run `python tools/prepare_dependencies.py` first instead of the submodule command. See [BUILDING-PUBLIC.md](BUILDING-PUBLIC.md) for details.

## Runtime support

- Steam: 1.5.97; 1.6.317, 1.6.318, 1.6.323, 1.6.342, 1.6.353, 1.6.629, 1.6.640, 1.6.1130, 1.6.1170; 1.7.99 and 1.7.104.
- GOG: 1.6.659 and 1.6.1179.

Requires matching SKSE and Address Library installations and the Microsoft Visual C++ x64 runtime. VR and unlisted versions are unsupported. The 14-version address/layout and loader checks are offline validation; in-game sampling was performed on Steam 1.6.1170.

Destructible data is tracked but not written. Unresolved leveled-template branches and invalid references are skipped. Existing script-instance preservation has been observed in game. In 0.6.7, Lydia's missing WIDeadBodyCleanupScript was created and bound successfully, its properties were verified, and the single instance persisted after saving, fully restarting the game and loading that save on Steam 1.6.1170. This does not establish support for arbitrary scripts, custom OnInit handlers or array properties; execution of the death-cleanup event was not tested.

## License

Maintained by ROSVER.

Code is licensed under **GPL-3.0-or-later** with the included Modding Exception and GPL-3.0 Linking Exception (with Corresponding Source). See [LICENSE](LICENSE), [COPYING](COPYING) and [EXCEPTIONS.md](EXCEPTIONS.md).

The pinned alandtse/CommonLibSSE-NG revision uses these GPL terms; it is not an MIT-only dependency. Original MIT notices and all other dependency notices remain included. See [THIRD_PARTY.md](THIRD_PARTY.md).
