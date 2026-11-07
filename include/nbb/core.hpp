#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace nbb {
using Json = nlohmann::json;
using Sequence = std::vector<std::string>;
inline const Sequence legacyFieldNames{"PKID", "SPOR", "ECOR", "OCOR", "GWOR", "Aggression", "Confidence", "Assistance", "SPLO", "Magicka", "Stamina",
    "MagickaOffset", "Protected", "Factions", "Inventory", "CombatStyle", "SkillValues", "UseAIPackages", "VMAD"};
inline const Sequence inventoryTypes{"SCRL","ARMO","BOOK","INGR","LIGH","MISC","APPA","WEAP","AMMO","KEYM","ALCH","SLGM","LVLI"};
extern const Sequence fieldNames;
Json configurationWarnings(const Json& config);
struct Npc {
    std::string key, editor, plugin;
    Json fields, observed;
    std::uint16_t templateFlags{};
    bool deleted{}, ignored{};
};
struct Plugin {
    std::string name;
    std::filesystem::path path;
    Sequence masters;
    std::uint32_t flags{};
    std::map<std::string, Npc> npcs;
    std::map<std::string, std::string> types;
    std::string sha256;
};
std::string lower(std::string text);
std::string formKey(std::uint32_t raw, const Sequence& mastersAndSelf);
std::string hashFile(const std::filesystem::path& path);
Json readJson(const std::filesystem::path& path);
void writeJson(const std::filesystem::path& path, const Json& data);
// Decode an already decompressed NPC record; performs no file or engine access.
Npc parseNpcRecord(std::span<const std::uint8_t> data, const std::string& key, const std::string& pluginName,
    const Sequence& mastersAndSelf, std::uint32_t flags);
Plugin readPlugin(const std::filesystem::path& path, std::string name, double* hashMilliseconds = nullptr);
Sequence parseExclusions(std::string text);
Sequence readExclusions(const std::filesystem::path& path);
Json makePlan(std::vector<Plugin> plugins, const Json& config, const Sequence& exclusions = {});
struct RunTimings {
    double readParseMilliseconds{}, hashMilliseconds{}, planMilliseconds{};
    double pluginReadWallMilliseconds{};
    unsigned parseWorkers{};
    double observerMilliseconds{};
};
Json run(const Json& manifest, const Json& config, const Sequence& exclusions = {}, RunTimings* timings = nullptr,
    const std::function<void(const Plugin&)>& observer = {});
void validateConfig(const Json& config);
}
