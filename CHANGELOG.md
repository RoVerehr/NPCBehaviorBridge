# Changelog

## 0.6.7

- Restore configured-baseline corrections when later plugins revert a field to the official value and no nonbaseline candidate exists. Qualifying gameplay changes still take priority.
- Use the engine's inheritance-aware script binding check. Resolve native ancestors for script-valued properties and reuse the target Form's existing custom script instance.
- Improve property diagnostics without resetting existing script state.
