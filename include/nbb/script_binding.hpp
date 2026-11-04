#pragma once
#include "nbb/core.hpp"

namespace nbb::scripts {
// NPC base fields stay process-scoped; script objects are owned by a loaded game.
void configure(const Json& plan, const Sequence& only, const std::filesystem::path& output);
void beforeLoad();
void afterLoad(const std::string& phase);
void sceneReady(); // retry missing bindings once after a loading screen
void check(const std::string& phase); // existing bindings only, never repairs
}
