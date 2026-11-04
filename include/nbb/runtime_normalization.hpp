#pragma once
#include "nbb/core.hpp"
#include <string_view>

namespace nbb {
// A missing ZNAM is filled by TESNPC::InitItemImpl with the engine's current
// default combat style. Only that exact substitution is equivalent to null.
inline bool isDefaultCombatStyleFill(std::string_view field, const Json& disk,
    const Json& actual, const Json& engineDefault) {
    return field=="CombatStyle" && disk.is_null() && !engineDefault.is_null() && actual==engineDefault;
}
}
