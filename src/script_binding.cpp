#include "nbb/script_binding.hpp"
#include "nbb/script_lifecycle.hpp"
#include <atomic>
#include <fstream>
#include <set>
#undef GetObject

namespace nbb::scripts {
namespace {
using VM = RE::BSScript::Internal::VirtualMachine;
using Object = RE::BSTSmartPointer<RE::BSScript::Object>;
using Type = RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo>;
using Variable = RE::BSScript::Variable;
using Token = std::pair<RE::VMHandle, std::string>;
struct Target { std::string key; Json scripts; Json sources; std::set<std::string> diskScripts; };
struct Pending { Object object; bool prepared{}; };
std::map<RE::FormID, Target> targets;
std::map<std::uint32_t, std::string> origins;
// Only objects created here have a known intended owner when still unbound.
// Never borrow an instance from another Actor or a base-form handle.
std::map<Token, Pending> pending;
std::map<Token, std::string> failed;
std::filesystem::path reportPath;
std::atomic<bool> active{false};
std::atomic<std::uint64_t> generation{0};
bool apply = false;

void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }

std::string keyOf(const RE::TESForm* form) {
    auto id = form->GetFormID();
    auto index = (id >> 24) == 0xfe ? id >> 12 : id >> 24;
    auto it = origins.find(index);
    require(it != origins.end(), "Form has no loaded plugin origin");
    return formKey((id >> 24) == 0xfe ? id & 0xfff : id & 0xffffff, {it->second});
}

RE::TESForm* resolve(const Json& key) {
    if (key.is_null()) return nullptr;
    auto normalized = parseExclusions(key.get<std::string>());
    require(normalized.size() == 1 && !normalized[0].starts_with("editorid:"), "Expected a stable FormKey");
    const auto& text = normalized[0]; auto colon = text.rfind(':');
    auto name = text.substr(0, colon);
    auto local = static_cast<std::uint32_t>(std::stoul(text.substr(colon + 1), nullptr, 16));
    auto* data = RE::TESDataHandler::GetSingleton();
    auto* file = data->LookupModByName(name);
    require(file && file->compileIndex != 0xff && (!file->IsLight() || local <= 0xfff), "Script property origin is not loaded");
    auto* form = data->LookupForm(local, name);
    require(form && !form->IsDeleted() && keyOf(form) == text, "Script property FormKey did not resolve: " + text);
    return form;
}

const RE::BSScript::PropertyTypeInfo* propertyType(RE::BSScript::ObjectTypeInfo* type, const std::string& name) {
    for (auto* p = type; p; p = p->GetParent()) {
        require(p->IsLinkedValid(), "Script class is not linked");
        const auto* props = p->GetPropertyIter();
        for (std::uint32_t i = 0; i < p->GetNumProperties(); ++i)
            if (lower(props[i].name.c_str()) == lower(name)) return &props[i].info;
    }
    return nullptr;
}

// Do not synthesize OnInit calls or write VM initialization flags. The first
// adapter handles scripts such as CarryActorScript without an OnInit body.
// Other scripts stay explicitly unsupported until their initialization path
// has been verified, rather than silently missing or duplicating that event.
bool hasOnInit(RE::BSScript::ObjectTypeInfo* type) {
    auto matches = [](const auto& function) {
        return function && lower(function->GetName().c_str()) == "oninit" && !function->GetIsEmpty();
    };
    for (auto* p = type; p; p = p->GetParent()) {
        const auto* funcs = p->GetMemberFuncIter();
        for (std::uint32_t i = 0; i < p->GetNumMemberFuncs(); ++i) if (matches(funcs[i].func)) return true;
        const auto* states = p->GetNamedStateIter();
        for (std::uint32_t i = 0; i < p->GetNumNamedStates(); ++i) {
            const auto* stateFuncs = states[i].GetFuncIter();
            for (std::uint32_t j = 0; j < states[i].GetNumFuncs(); ++j) if (matches(stateFuncs[j].func)) return true;
        }
    }
    return false;
}

RE::BSScript::ObjectTypeInfo* nativeType(VM* vm, RE::BSScript::ObjectTypeInfo* type, RE::VMTypeID& id) {
    auto* found = nativeAncestor(type, [&](auto* candidate) {
        require(candidate->IsLinkedValid(), "Object property class is not linked");
        return vm->GetTypeIDForScriptObject(candidate->GetName(), id);
    }, [](auto* candidate) { return candidate->GetParent(); });
    require(found && id < static_cast<RE::VMTypeID>(RE::FormType::Max),
        "Object property has no native TESForm ancestor");
    return found;
}

Variable makeValue(VM* vm, const Json& spec, const RE::BSScript::TypeInfo& declared) {
    auto type = spec.at("type").get<int>();
    require(type >= 1 && type <= 5, "Array properties are not supported by the first script adapter");
    require(static_cast<int>(declared.GetUnmangledRawType()) == type, "VMAD property type differs from loaded PEX");
    const auto& value = spec.at("value");
    Variable result(declared);
    if (type == 1) {
        require(value.at("alias") == -1, "Quest alias property requires separate resolution");
        auto* form = resolve(value.at("form"));
        if (!form) return result; // retain the declared type of None
        auto* declaredClass = declared.GetTypeInfo();
        require(declaredClass, "Object property has no declared class");
        RE::VMTypeID nativeID{};
        auto* nativeClass = nativeType(vm, declaredClass, nativeID);
        auto* handles = vm->GetObjectHandlePolicy();
        auto handle = handles->GetHandleForObject(form->GetFormType(), form);
        require(handle != handles->EmptyHandle() && handles->HandleIsType(nativeID, handle), "Property object has the wrong native type");
        auto propertyObject = propertyBinding(nativeClass == declaredClass, [&] {
            Object existing;
            vm->FindBoundObject(handle, declaredClass->GetName(), existing);
            return existing;
        }, [&] {
            Variable wrapped;
            RE::BSScript::PackHandle(&wrapped, form, static_cast<RE::VMTypeID>(form->GetFormType()));
            require(wrapped.IsObject() && wrapped.GetObject(), "Could not wrap script property form");
            return Object(wrapped.GetObject());
        });
        require(propertyObject->GetHandle() == handle,
            "Property script instance belongs to a different Form");
        Object casted;
        Type destination(declaredClass);
        require(vm->CastObject(propertyObject, destination, casted) && casted && casted->GetHandle() == handle,
            "Property object cannot be cast to declared class on the same Form");
        result.SetObject(casted, declared.GetRawType());
    } else if (type == 2) result.SetString(value.get<std::string>());
    else if (type == 3) result.SetSInt(value.get<std::int32_t>());
    else if (type == 4) result.SetFloat(value.get<float>());
    else result.SetBool(value.is_boolean() ? value.get<bool>() : value.get<int>() != 0);
    return result;
}

Json inspectValue(VM* vm, const Variable& value) {
    if (value.IsInt()) return value.GetSInt();
    if (value.IsFloat()) return value.GetFloat();
    if (value.IsBool()) return value.GetBool();
    if (value.IsString()) return std::string(value.GetString());
    if (value.IsObject() || value.IsNoneObject()) {
        auto object = value.GetObject();
        if (!object) return nullptr;
        auto* type = object->GetTypeInfo(); RE::VMTypeID nativeID{};
        nativeType(vm, type, nativeID);
        auto* form = static_cast<RE::TESForm*>(object->Resolve(nativeID));
        require(form, "Property object cannot resolve to a native form");
        return keyOf(form);
    }
    return {{"unsupported_type", value.GetType().TypeAsString()}};
}

Json snapshot(VM* vm, Object& object, const Json& spec) {
    require(object && object->IsConstructed() && object->GetTypeInfo() && object->GetTypeInfo()->IsLinkedValid(),
        "Script object is not ready for property inspection; preserved without writes");
    Json props = Json::object(), defaults = Json::object(); bool match = true;
    for (auto& [name, property] : spec.at("properties").items()) {
        // On 1.6.1170 the generic property query returned an unchanged None
        // output for this live instance. Independent VM slot reads confirmed
        // the actual auto properties were populated. Inspect auto slots via
        // CommonLib's decoded layout; never execute a scripted getter here.
        const auto* slot = object->GetProperty(name.c_str());
        if (!slot) { props[name] = {{"error", "unreadable_auto_property"}}; match = false; continue; }
        Variable value(*slot);
        try {
            auto actual = inspectValue(vm, value); props[name] = actual;
            const auto& expected = property.at("type") == 1 ? property.at("value").at("form") : property.at("value");
            match &= actual == expected;
        } catch (const std::exception& e) { props[name] = {{"error", e.what()}}; match = false; }
    }
    // Also expose PEX defaults (e.g. CarryItemMiscCount = 1) without writing them.
    for (auto* p = object->GetTypeInfo(); p; p = p->GetParent()) {
        const auto* properties = p->GetPropertyIter();
        for (std::uint32_t i = 0; i < p->GetNumProperties(); ++i) {
            const auto& prop = properties[i]; std::string name = prop.name.c_str();
            if (spec.at("properties").contains(name) || prop.info.autoVarIndex == UINT32_MAX || defaults.contains(name)) continue;
            if (const auto* slot = object->GetProperty(name.c_str())) {
                Variable value(*slot);
                try { defaults[name] = inspectValue(vm, value); } catch (...) { defaults[name] = {{"error", "unresolved"}}; }
            }
        }
    }
    return {{"object_address", reinterpret_cast<std::uintptr_t>(object.get())}, {"handle", object->GetHandle()},
        {"constructed", object->IsConstructed()}, {"initialized", object->IsInitialized()}, {"valid", object->IsValid()},
        {"state", object->currentState.c_str()}, {"properties", props}, {"properties_match_source", match}, {"other_auto_properties", defaults}};
}

unsigned instanceCount(VM* vm, RE::VMHandle handle, const std::string& script) {
    unsigned count = 0;
    RE::BSSpinLockGuard guard(vm->attachedScriptsLock);
    auto it = vm->attachedScripts.find(handle);
    if (it != vm->attachedScripts.end()) for (auto& entry : it->second) {
        auto* obj = entry.get();
        if (obj && obj->GetTypeInfo() && lower(obj->GetTypeInfo()->GetName()) == lower(script)) ++count;
    }
    return count;
}

Json bindOne(VM* vm, RE::VMHandle handle, const std::string& script, const Json& spec, bool mayCreate) {
    Object existing;
    if (vm->FindBoundObject(handle, script.c_str(), existing) && existing) {
        auto result = snapshot(vm, existing, spec);
        result["instance_count"] = instanceCount(vm, handle, script);
        result["status"] = "existing_preserved";
        // Properties can legitimately change during play; report differences,
        // never restore old defaults onto a running/saved instance.
        return result;
    }
    const Token token{handle, lower(script)};
    if (!apply || !mayCreate) return {{"status", "missing"}, {"creation_allowed", apply && mayCreate}};
    if (failed.contains(token)) return {{"status", "previous_attempt_failed"}, {"reason", failed.at(token)}};
    if (vm->IsCompletelyFrozen()) return {{"status", "deferred_vm_frozen"}};
    try {
      auto binding = ensureBinding(true, [&] {
        Object found; vm->FindBoundObject(handle, script.c_str(), found); return found;
      }, [&] {
        Type type;
        require(vm->GetScriptObjectType(script.c_str(), type) && type && type->IsLinkedValid(), "Script PEX cannot be loaded/linked");
        // The engine checks the native ancestor against this handle. Looking
        // up the custom script name directly would reject valid Actor scripts.
        require(vm->TypeCanBeBound(script.c_str(), handle), "Engine rejected script/Actor type compatibility: " + script);
        require(spec.at("status").get<int>() == 0, "Unsupported VMAD script status");
        require(!hasOnInit(type.get()), "Custom OnInit requires a separately verified initialization adapter");
        std::map<std::string, Variable> values;
        for (auto& [name, property] : spec.at("properties").items()) {
            auto* info = propertyType(type.get(), name);
            require(info && info->autoVarIndex != UINT32_MAX, "Missing or non-auto script property: " + name);
            require(property.at("status").get<int>() == 1, "Unsupported VMAD property status");
            try { values.emplace(name, makeValue(vm, property, info->type)); }
            catch (const std::exception& e) { throw std::runtime_error("Property " + name + ": " + e.what()); }
        }
        auto& item = pending[token];
        if (!item.object) {
            require(vm->CreateObject(script.c_str(), item.object) && item.object, "VM could not create script object");
        }
        require(item.object->IsConstructed(), "Created object is not constructed");
        if (!item.prepared) {
            // Only touch the private, newly created object before its first bind.
            // Preflight all property slots before assigning any values.
            for (auto& [name, value] : values) require(item.object->GetProperty(name.c_str()), "Auto-property slot unavailable: " + name);
            for (auto& [name, value] : values) *item.object->GetProperty(name.c_str()) = value;
            item.prepared = true;
        }
        return item.object;
      }, [&](Object& object) {
        require(object->GetHandle() == vm->GetObjectHandlePolicy()->EmptyHandle(), "Pending object is already bound elsewhere");
        vm->GetObjectBindPolicy()->BindObject(object, handle);
      });
        auto bound = binding.object;
        require(bound->GetHandle() == handle, "Bound script handle differs from target Actor");
        auto result = snapshot(vm, bound, spec);
        result["instance_count"] = instanceCount(vm, handle, script);
        const bool verified = result.at("properties_match_source").get<bool>() && result.at("instance_count") == 1 &&
            bound->IsConstructed() && bound->IsInitialized() && bound->IsValid();
        result["status"] = binding.action == BindingAction::preserved ? "existing_preserved" :
            verified ? "created_bound_verified" : "created_bound_needs_review";
        // These checks verify the observed binding, not combat callbacks or save persistence.
        pending.erase(token);
        return result;
    } catch (const std::exception& e) {
        failed[token] = e.what();
        return {{"status", "binding_error"}, {"reason", e.what()}};
    }
}

void process(RE::Actor* actor, const std::string& phase, bool mayCreate) {
    if (!active.load() || !actor || actor->IsDeleted() || !actor->GetActorBase()) return;
    auto found = targets.find(actor->GetActorBase()->GetFormID());
    if (found == targets.end()) return;
    Json record{{"phase", phase}, {"generation", generation.load()}, {"mode", apply ? "apply" : "audit"},
        {"actor_form_id", actor->GetFormID()}, {"npc", found->second.key}, {"sources", found->second.sources}, {"scripts", Json::object()}};
    try {
        auto* vm = VM::GetSingleton(); require(vm && vm->GetObjectHandlePolicy() && vm->GetObjectBindPolicy(), "VM unavailable");
        auto* handles = vm->GetObjectHandlePolicy();
        auto handle = handles->GetHandleForObject(RE::FormType::ActorCharacter, actor);
        require(handle != handles->EmptyHandle() && handles->IsHandleObjectAvailable(handle), "Actor VM handle unavailable");
        record["actor_vm_handle"] = handle;
        for (auto& [script, spec] : found->second.scripts.items()) {
            try {
                // A script still declared by the winning NPC may be awaiting
                // the engine's own attachment. Do not race that normal path.
                bool declared = found->second.diskScripts.contains(lower(script));
                auto result = bindOne(vm, handle, script, spec, mayCreate && !declared);
                if (declared && result.at("status") == "missing") result["status"] = "pending_engine_attachment";
                result["declared_by_winning_npc"] = declared;
                record["scripts"][script] = std::move(result);
            }
            catch (const std::exception& e) { record["scripts"][script] = {{"status", "inspection_error"}, {"reason", e.what()}}; }
        }
    } catch (const std::exception& e) { record["error"] = e.what(); }
    std::ofstream stream(reportPath, std::ios::app); stream << record.dump() << '\n';
    SKSE::log::info("Script bindings {} actor {:08X}: {}", phase, actor->GetFormID(), record.at("scripts").dump());
}

void queueSweep(const std::string& phase, bool mayCreate) {
    auto stamp = generation.load();
    SKSE::GetTaskInterface()->AddTask([phase, mayCreate, stamp] {
        if (!active.load() || stamp != generation.load()) return;
        if (auto* lists = RE::ProcessLists::GetSingleton()) {
            std::set<RE::FormID> seen;
            lists->ForAllActors([&](RE::Actor* actor) {
                if (seen.insert(actor->GetFormID()).second) process(actor, phase, mayCreate);
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
    });
}

class ObjectEvents final : public RE::BSTEventSink<RE::TESObjectLoadedEvent> {
    RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override {
        if (event && event->loaded && active.load() && !targets.empty()) {
            auto stamp = generation.load(); auto id = event->formID;
            SKSE::GetTaskInterface()->AddTask([stamp, id] {
                if (!active.load() || stamp != generation.load()) return;
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
                process(actor, "actor_loaded", true);
            });
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};
ObjectEvents events;
}

void configure(const Json& plan, const Sequence& only, const std::filesystem::path& output) {
    targets.clear(); origins.clear(); apply = plan.at("mode") == "apply";
    reportPath = output / "NPCBehaviorBridge-script-bindings.jsonl";
    std::ofstream(reportPath, std::ios::trunc);
    auto* data = RE::TESDataHandler::GetSingleton();
    for (auto* file : data->files) if (file && file->compileIndex != 0xff) {
        auto index = file->IsLight() ? 0xfe000u | file->smallFileCompileIndex : file->compileIndex;
        origins[index] = lower(file->fileName);
    }
    Json configured = Json::array();
    for (const auto& row : plan.at("npcs")) {
        auto key = row.at("npc").get<std::string>();
        if (row.at("status") != "ready" || (!only.empty() && std::find(only.begin(), only.end(), key) == only.end())) continue;
        if (!row.at("fields").contains("VMAD")) continue;
        const auto& field = row.at("fields").at("VMAD");
        if (field.at("status") != "ready") continue;
        try {
            auto* form = resolve(key); require(form && form->Is(RE::FormType::NPC), "Script target is not an NPC base");
            std::set<std::string> diskScripts;
            for (const auto& [name, _] : field.at("disk").items()) diskScripts.insert(lower(name));
            targets.emplace(form->GetFormID(), Target{key, field.at("expected"), field.at("sources"), std::move(diskScripts)});
            configured.push_back({{"npc", key}, {"status", "pending_actor_binding"}, {"expected", field.at("expected")}, {"sources", field.at("sources")}});
        } catch (const std::exception& e) { configured.push_back({{"npc", key}, {"status", "configuration_error"}, {"reason", e.what()}}); }
    }
    writeJson(output / "NPCBehaviorBridge-script-targets.json", {{"mode", plan.at("mode")}, {"targets", configured},
        {"scope", "Missing Actor instances only; existing script state is preserved; NPC base VMAD metadata is not modified"}});
    RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink(&events);
}

void beforeLoad() {
    active.store(false); generation.fetch_add(1);
    pending.clear(); failed.clear();
}
void afterLoad(const std::string& phase) {
    beforeLoad(); active.store(true); queueSweep(phase, true);
}
void sceneReady() { if (active.load()) queueSweep("scene_ready", true); }
void check(const std::string& phase) { if (active.load()) queueSweep(phase, false); }
}
