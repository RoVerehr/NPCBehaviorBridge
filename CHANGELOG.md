# Changelog

## 0.7.1

- Fix repeated AI package references stopping restoration.
- Preserve the selected source list order and every repeated entry.
- Remove unnecessary content and size restrictions without changing the restoration rules.
- Isolate errors per field so other fields and NPCs can still be processed.
- Add detailed input, restoration, and load-capture fallback errors to logs and debug reports.

## 0.7

- Reduce startup overhead by capturing NPC records during the game's own loading and sharing one recovery plan.
- Enable the loading optimization across all 14 supported Steam/GOG runtimes, with automatic disk fallback when capture cannot be used safely.
- Reduce repeated template snapshots and stat calculations while keeping the existing restoration rules and script-state preservation.
- Remove development probes and test-only reports; retain normal logging, timing, and optional debug/HTML reports.

## 0.6.8

- Always exclude the player from NPC restoration and script binding.
- Add a `debug` option, disabled by default. Normal logging shows each NPC's result and keeps all errors; detailed diagnostic files are generated only when enabled.
- Add a searchable HTML startup report with field comparisons when debug mode is enabled.
- Start a fresh log each time the game launches and clear outdated diagnostic reports.

## 0.6.7

- Restore configured-baseline corrections when later plugins revert a field to the official value and no nonbaseline candidate exists. Qualifying gameplay changes still take priority.
- Use the engine's inheritance-aware script binding check. Resolve native ancestors for script-valued properties and reuse the target Form's existing custom script instance.
- Improve property diagnostics without resetting existing script state.
