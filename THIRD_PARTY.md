# Third-party dependencies

Exact upstream URLs, commit IDs and source archive URLs are recorded in [dependencies.lock.json](dependencies.lock.json). The six direct dependencies are pinned Git submodules. Their source remains under the original authors' licenses.

| Component | License / notices | Use |
|---|---|---|
| alandtse/CommonLibSSE-NG | GPL-3.0-or-later with Modding Exception and GPL-3.0 Linking Exception (with Corresponding Source); [GPL text](licenses/CommonLibSSE-NG.txt), [exceptions](licenses/CommonLibSSE-NG-EXCEPTIONS.md), [original MIT notices](licenses/CommonLibSSE-NG-LICENSE-MIT.txt) | SKSE integration and game structures |
| zlib 1.3.1 | [zlib license](licenses/zlib.txt) | Reading compressed plugin records |
| nlohmann/json 3.11.3 | [MIT](licenses/json.txt) | Configuration and reports |
| spdlog 1.14.1 | [MIT](licenses/spdlog.txt) | Logging |
| fmt, bundled with the pinned spdlog | [MIT and upstream optional exception](licenses/fmt.txt) | Formatting used by spdlog |
| rapidcsv | [BSD 3-Clause](licenses/rapidcsv.txt) | CommonLib address-library support |
| DirectXTK | [MIT](licenses/DirectXTK.txt) | Dependency required by this CommonLib build; Bridge has no rendering subsystem |
| OpenVR, nested in CommonLib | [BSD 3-Clause](licenses/OpenVR.txt) | Upstream dependency source; Bridge's VR target is disabled |
| hde64 | [BSD notice](licenses/CommonLibSSE-NG-LICENSE-hde64.txt) | Notice retained from CommonLib; the optional patch-safety decoder is disabled in this build |

The CommonLib license declaration is available at the [exact pinned revision](https://github.com/alandtse/CommonLibSSE-NG/blob/94faaed0c60eddd8347767f2d4d29a97c93bde8c/README.md#license). Do not interpret its retained original MIT notices as licensing the whole current fork under MIT.

NPC Behavior Bridge's original code uses the license in [LICENSE](LICENSE), with [COPYING](COPYING) and [EXCEPTIONS.md](EXCEPTIONS.md). Third-party copyright and license notices remain applicable.

No AI Overhaul, Lawless, appearance-mod, Skyrim plugin, script or art assets are included. Those mods and their own dependencies must remain installed when their behavior or resources are needed.
