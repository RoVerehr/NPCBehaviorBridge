#pragma once
#include <stdexcept>

namespace nbb::scripts {
enum class BindingAction { preserved, missing, created };
template <class Object> struct BindingResult { Object object; BindingAction action; };

// Native IDs belong to registered engine classes, not every Papyrus subclass.
template <class Type, class Lookup, class Parent>
Type* nativeAncestor(Type* type, Lookup lookup, Parent parent) {
    for (unsigned depth = 0; type && depth < 128; ++depth, type = parent(type))
        if (lookup(type)) return type;
    return nullptr;
}

// A custom script property refers to the owner's existing instance. Only a
// native property may obtain a plain engine wrapper when no binding exists.
template <class Find, class Wrap>
auto propertyBinding(bool nativeClass, Find find, Wrap wrap) {
    if (auto existing = find()) return existing;
    if (!nativeClass) throw std::runtime_error("Required custom property script is not bound to its Form");
    auto object = wrap();
    if (!object) throw std::runtime_error("Native property wrapper is unavailable");
    return object;
}

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
