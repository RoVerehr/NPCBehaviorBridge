# Building NPC Behavior Bridge

This checkout contains the production source for version 0.6.6. The C++ source, headers and CMake build definitions match the development version. It builds without the private development directory, Skyrim files, an MO2 profile or local test fixtures.

## Tools

- Windows x64.
- Visual Studio 2022 with Desktop development with C++ and a Windows SDK; the development build used SDK 10.0.26100.0.
- CMake 3.24 or later and Git.
- Python 3.9 or later for the optional dependency helper.

Dependencies are pinned in `.gitmodules` and `dependencies.lock.json`. Do not replace them with arbitrary latest versions when reproducing this release. The helper verifies commits and fetches CommonLib's nested OpenVR submodule. It refuses to replace a checkout at a different commit.

## Get dependency sources

For a Git checkout:

```sh
git submodule update --init --recursive
python tools/prepare_dependencies.py --check
```

GitHub's automatically generated source ZIP does not include submodule contents. After extracting it, use:

```sh
python tools/prepare_dependencies.py
```

Both paths obtain the same pinned sources. Source archive URLs are also recorded in the lock file. CommonLib's OpenVR dependency is initialized for a complete checkout; Bridge itself builds with VR disabled.

## Build

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DNBB_BUILD_TESTS=OFF
cmake --build build --config Release --target NPCBehaviorBridge nbb_cli --parallel 4
```

Outputs:

- `build/Release/NPCBehaviorBridge.dll`: the SKSE plugin.
- `build/Release/nbb_cli.exe`: the offline record-selection tool. Run it with `--fields` to list supported field names, or with manifest, configuration and report paths to generate an offline plan.

The default build disables local test targets. `NBB_BUILD_TESTS=ON` is for the development workspace and requires test files not shipped in this source-only repository. It is not required to build the plugin or CLI.

This build supplies its pinned dependencies directly; a separate vcpkg installation is not required. SE and AE support are enabled, VR and CommonLib's optional patch-safety decoder are disabled.

Install the DLL together with the matching release's JSON configuration and exclusion file under `SKSE/Plugins`. Configuration is not compiled into the DLL. Fully restart Skyrim through SKSE after replacing a DLL. No build command installs files or starts the game.

MSVC build timestamps, debug information and local paths can change the complete DLL hash. A build from this source is not promised to be byte-for-byte identical to the distributed DLL.

## Source and license distribution

When distributing covered binaries, provide the corresponding source under the included GPL terms, including these build files and the exact dependency sources. Keep the repository/release source link alongside the binary download. A repository clone must include submodules; a self-contained source archive must include their contents, not just GitHub's parent-repository ZIP.

Include `LICENSE`, `COPYING`, `EXCEPTIONS.md`, `THIRD_PARTY.md` and the applicable `licenses/` notices with binary distributions. These documents cover this software and its dependencies, not Skyrim or other mods' assets.
