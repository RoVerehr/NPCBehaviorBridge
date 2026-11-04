#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
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
Plugin readPlugin(const std::filesystem::path& path, std::string name);
Sequence parseExclusions(std::string text);
Sequence readExclusions(const std::filesystem::path& path);
Json makePlan(const std::vector<Plugin>& plugins, const Json& config, const Sequence& exclusions = {});
Json run(const Json& manifest, const Json& config, const Sequence& exclusions = {});
void validateConfig(const Json& config);
}
