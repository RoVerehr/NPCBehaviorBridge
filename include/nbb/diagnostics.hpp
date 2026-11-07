#pragma once
#include "nbb/core.hpp"
#include <array>
#include <fstream>
#include <sstream>
#include <set>

namespace nbb::diagnostics {
inline constexpr std::array reportNames{
    "NPCBehaviorBridge-runtime.json", "NPCBehaviorBridge-sampling.json",
    "NPCBehaviorBridge-script-bindings.jsonl", "NPCBehaviorBridge-script-targets.json",
    "NPCBehaviorBridge-verification.jsonl", "NPCBehaviorBridge-report.html",
    // Remove leftover files from pre-release capture experiments, too.
    "NPCBehaviorBridge-load-probe.jsonl", "NPCBehaviorBridge-early-prototype.json",
    "NPCBehaviorBridge-early-all.json", "NPCBehaviorBridge-early-all-audit.json",
    "NPCBehaviorBridge-load-stream.json"};

template<class Error> void resetReports(const std::filesystem::path& directory, Error error) {
    for (const auto* name : reportNames) {
        std::error_code ec;
        std::filesystem::remove(directory / name, ec);
        if (ec) error(std::string(name) + ": " + ec.message());
    }
}

inline void writeReport(bool debug, const std::filesystem::path& path, const Json& data, bool append = false) {
    if (!debug) return;
    if (!append) { writeJson(path, data); return; }
    std::ofstream stream(path, std::ios::app);
    stream << data.dump() << '\n'; stream.flush();
    if (!stream) throw std::runtime_error("Cannot append diagnostic report: " + path.string());
}

inline bool isError(const std::string& status) {
    return status == "error" || status.find("error") != std::string::npos ||
        status == "apply_mismatch" || status == "created_bound_needs_review" || status == "previous_attempt_failed";
}

struct NpcResult {
    unsigned restored{}, unchanged{}, skipped{}, pending{}, wouldRestore{};
    std::vector<std::string> errors;
    std::string line;
};

inline std::string skipReason(const std::string& status) {
    if(status=="no_source_changes") return "No qualifying changes";
    if(status=="outside_requested_scope") return "Outside only_npcs selection";
    if(status=="skipped_runtime_conflict") return "Runtime value differs from plugin data";
    return status;
}

inline NpcResult npcResult(const Json& row) {
    NpcResult result;
    std::map<std::string, std::string> skipped;
    const auto status = row.value("runtime_status", row.value("status", std::string("unknown")));
    for (const auto* name : {"runtime_error", "inheritance_error"})
        if (row.contains(name)) result.errors.push_back(row.at(name).get<std::string>());
    if (row.contains("non_target_object_bytes_unchanged") && !row.at("non_target_object_bytes_unchanged").get<bool>())
        result.errors.push_back("Non-target NPC bytes changed");
    for (const auto& [field, spec] : row.at("fields").items()) {
        const auto& entry = row.contains("field_results") && row.at("field_results").contains(field) ? row.at("field_results").at(field) : spec;
        const auto state = entry.value("status", status);
        if (isError(state)) {
            result.errors.push_back(field + ": " + state + "; " + entry.value("reason", std::string("post-write mismatch")));
        } else if (state == "applied_verified" || state == "audit_already_matches") {
            if (row.contains("runtime_before") && row.contains("runtime_after") &&
                row.at("runtime_before").contains(field) && row.at("runtime_after").contains(field) &&
                row.at("runtime_before").at(field) != row.at("runtime_after").at(field)) ++result.restored;
            else ++result.unchanged;
        } else if (state == "audit_needs_changes") ++result.wouldRestore;
        else if (state == "pending_actor_binding") ++result.pending;
        else {
            ++result.skipped;
            auto reason = entry.value("reason", std::string());
            if (reason.empty()) reason = skipReason(state == "ready" ? status : state);
            if (!skipped[reason].empty()) skipped[reason] += ",";
            skipped[reason] += field;
        }
    }
    if (isError(status) && result.errors.empty()) result.errors.push_back(status);
    std::ostringstream text;
    text << "NPC " << row.value("editor_id", std::string()) << " [" << row.at("npc").get<std::string>() << "]: ";
    text << (!result.errors.empty() ? "ERROR" : result.restored ? "RESTORED" : result.wouldRestore ? "AUDIT" :
        result.unchanged ? "UNCHANGED" : result.pending ? "PENDING SCRIPTS" : "SKIPPED");
    text << "; restored=" << result.restored << ", unchanged=" << result.unchanged << ", skipped=" << result.skipped;
    if (result.pending) text << ", pending_scripts=" << result.pending;
    if (result.wouldRestore) text << ", would_restore=" << result.wouldRestore;
    if (auto reason = row.value("reason", std::string()); !reason.empty()) text << "; " << reason;
    else if (row.at("fields").empty()) text << "; " << skipReason(status);
    for (const auto& [reason, fields] : skipped) text << "; skipped " << fields << " (" << reason << ")";
    if (row.contains("inheritance_notes")) {
        std::set<std::string> notes;
        for (const auto& [field, reason] : row.at("inheritance_notes").items()) notes.insert(reason.get<std::string>());
        for(const auto& note:notes) if(!skipped.contains(note)) text << "; inheritance: " << note;
    }
    result.line = text.str();
    return result;
}

inline std::vector<std::string> scriptErrors(const Json& record) {
    std::vector<std::string> errors;
    if (record.contains("error")) errors.push_back(record.at("error").get<std::string>());
    for (const auto& [name, spec] : record.at("scripts").items()) {
        const auto status = spec.value("status", std::string());
        if (isError(status)) errors.push_back(name + ": " + status + "; " + spec.value("reason", std::string()));
        for (const auto* group : {"properties", "other_auto_properties"}) if (spec.contains(group))
            for (const auto& [property, value] : spec.at(group).items())
                if (value.is_object() && value.contains("error"))
                    errors.push_back(name + "." + property + ": " + value.at("error").get<std::string>());
                else if(value.is_object() && value.contains("unsupported_type"))
                    errors.push_back(name + "." + property + ": unsupported property type " + value.at("unsupported_type").dump());
    }
    return errors;
}
}
