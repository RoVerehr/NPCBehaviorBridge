#pragma once
#include <RE/Skyrim.h>

namespace nbb {
// Both adapters run after the engine has resolved the NPC's reference slots.
inline RE::TESForm* defaultCombatStyle() {
    auto* manager=RE::BGSDefaultObjectManager::GetSingleton();
    auto* style=manager?manager->objects[std::to_underlying(RE::DEFAULT_OBJECT::kCombatStyle)]:nullptr;
    return style && style->GetFormType()==RE::FormType::CombatStyle && !style->IsDeleted()?style:nullptr;
}
}
