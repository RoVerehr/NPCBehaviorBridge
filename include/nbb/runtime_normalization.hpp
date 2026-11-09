#pragma once
#include "nbb/core.hpp"
#include <string_view>
#include <array>

namespace nbb {
using FieldEquality = std::function<bool(std::string_view,const Json&,const Json&)>;
inline bool identicalField(std::string_view,const Json& a,const Json& b) {return a==b;}
// SPLO is serialized as one sequence but the engine stores three arrays. This
// projection is only for runtime comparison/export, never source selection or
// mutation: preserve order and every occurrence within each native array.
template<class Classify> Json spellRuntimeOrder(const Json& values,Classify classify) {
    std::array<Json,3> groups{Json::array(),Json::array(),Json::array()};
    for(const auto& value:values) groups.at(classify(value)).push_back(value);
    Json result=Json::array();
    for(const auto& group:groups)for(const auto& value:group)result.push_back(value);
    return result;
}
// A missing ZNAM is filled by TESNPC::InitItemImpl with the engine's current
// default combat style. Only that exact substitution is equivalent to null.
inline bool isDefaultCombatStyleFill(std::string_view field, const Json& disk,
    const Json& actual, const Json& engineDefault) {
    return field=="CombatStyle" && disk.is_null() && !engineDefault.is_null() && actual==engineDefault;
}
}
