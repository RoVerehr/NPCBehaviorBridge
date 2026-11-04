#pragma once
#include <stdexcept>

namespace nbb::scripts {
enum class BindingAction { preserved, missing, created };
template <class Object> struct BindingResult { Object object; BindingAction action; };

// Shared by the native adapter and focused tests. Recheck after preparation:
// the engine may have supplied a binding while the PEX/properties were resolved.
template <class Find, class Prepare, class Bind>
auto ensureBinding(bool allowCreate, Find find, Prepare prepare, Bind bind) {
    using Object = decltype(find());
    if (auto existing = find()) return BindingResult<Object>{existing, BindingAction::preserved};
    if (!allowCreate) return BindingResult<Object>{{}, BindingAction::missing};
    auto prepared = prepare();
    if (!prepared) throw std::runtime_error("VM returned an empty prepared script");
    if (auto existing = find()) return BindingResult<Object>{existing, BindingAction::preserved};
    bind(prepared);
    auto bound = find();
    if (!bound || bound != prepared) throw std::runtime_error("VM binding did not resolve to the prepared object");
    return BindingResult<Object>{bound, BindingAction::created};
}
}
