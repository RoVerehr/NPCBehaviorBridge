#include "nbb/core.hpp"
#include "nbb/script_binding.hpp"
#include "nbb/runtime_policy.hpp"
#include "nbb/runtime_normalization.hpp"
#include "nbb/field_catalog.hpp"
#include "nbb/inheritance.hpp"
#include "nbb/diagnostics.hpp"
#include "nbb/report.hpp"
#include "nbb/load_capture.hpp"
#include <Windows.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <array>
#include <atomic>
#include <fstream>
#include <set>
#include <memory>
#include <utility>
#include <cmath>
#include <chrono>

namespace {
using nbb::Json;
using List=RE::BSSimpleList<RE::TESPackage*>;
static_assert(sizeof(List)==16 && sizeof(List::Node)==16);
static_assert(std::to_underlying(RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kStats)==0x02);
static_assert(std::to_underlying(RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kSpells)==0x08);
static_assert(std::to_underlying(RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kAIData)==0x10);
static_assert(std::to_underlying(RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kAIPackages)==0x20);
std::filesystem::path output;
Json plan,layout;
bool initialized=false;
bool debugOutput=false;
std::atomic<std::uint32_t> samplingReady{0};
std::atomic<std::uint64_t> samplingEpoch{0};
std::map<std::uint32_t,std::string> origins;
unsigned verification=0;
const nbb::runtime::Version* hostRuntime=nullptr;
std::map<std::string,const RE::TESFile*> loadedPluginsByName;
std::unordered_map<std::uint32_t,std::string> runtimeFormKeys;

void require(bool ok,const std::string& error) { if(!ok) throw std::runtime_error(error); }
std::uint64_t address(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }
const std::string& keyOf(const RE::TESForm* form) {
    static const std::string empty;
    if(!form) return empty;
    auto id=form->GetFormID(); auto index=(id>>24)==0xfe?(id>>12):(id>>24);
    if(auto cached=runtimeFormKeys.find(id);cached!=runtimeFormKeys.end())return cached->second;
    auto found=origins.find(index); require(found!=origins.end(),"Runtime form has no loaded origin");
    return runtimeFormKeys.emplace(id,nbb::formKey((id>>24)==0xfe?(id&0xfff):(id&0xffffff),{found->second})).first->second;
}
Json reference(const RE::TESForm* form) { return form?Json(keyOf(form)):Json(nullptr); }
RE::TESForm* resolve(const Json& key,RE::FormType type,std::initializer_list<RE::FormType> accepted={}) {
    if(key.is_null()) return nullptr;
    auto value=key.get<std::string>(); auto colon=value.rfind(':'); require(colon!=std::string::npos,"Malformed FormKey");
    auto name=value.substr(0,colon); auto local=static_cast<std::uint32_t>(std::stoul(value.substr(colon+1),nullptr,16));
    const auto found=loadedPluginsByName.find(name);
    const auto* file=found==loadedPluginsByName.end()?nullptr:found->second;
    require(file && file->compileIndex!=0xff,"Form origin not loaded: "+value);
    require(!file->IsLight() || local<=0xfff,"ESL FormKey exceeds local range: "+value);
    // Same SE/AE full/light ID construction as TESDataHandler::LookupFormID,
    // without its second linear plugin-name search. Keep all checks below.
    const auto formID=(std::uint32_t(file->compileIndex)<<24)+(std::uint32_t(file->smallFileCompileIndex)<<12)+local;
    auto* form=RE::TESForm::LookupByID(formID);
    bool typeMatches=form && (type==RE::FormType::None?
        (form->GetFormType()==RE::FormType::Spell||form->GetFormType()==RE::FormType::LeveledSpell||form->GetFormType()==RE::FormType::Shout):
        type==RE::FormType::Max?(form->IsInventoryObject() && form->GetFormType()!=RE::FormType::ConstructibleObject && form->GetFormType()!=RE::FormType::Note):form->GetFormType()==type);
    if(form && accepted.size()) typeMatches=std::find(accepted.begin(),accepted.end(),form->GetFormType())!=accepted.end();
    require(typeMatches && !form->IsDeleted(),"Runtime reference/type mismatch: "+value);
    require(keyOf(form)==value,"Runtime FormKey roundtrip failed: "+value);
    return form;
}
RE::BGSListForm*& listField(RE::TESNPC* npc,const std::string& field) {
    if(field=="SPOR") return npc->spectatorOverRidePackList;
    if(field=="ECOR") return npc->enterCombatOverRidePackList;
    if(field=="OCOR") return npc->observeCorpseOverRidePackList;
    if(field=="GWOR") return npc->guardWarnOverRidePackList;
    throw std::runtime_error("Unknown override list field");
}
std::uint32_t readWord(const void* ptr,unsigned bytes) {std::uint32_t v=0;std::memcpy(&v,ptr,bytes);return v;}
Json scalarValue(RE::TESNPC* npc,const nbb::ScalarField& f) {
    auto raw=readWord(reinterpret_cast<const char*>(npc)+f.runtimeOffset,f.runtimeBytes);
    return nbb::decodeScalarWord(f,raw);
}
RE::TESForm* resolveFunctionalReference(const Json& key,const std::string& field) {
    if(field=="Template") return resolve(key,RE::FormType::NPC,{RE::FormType::NPC,RE::FormType::LeveledNPC});
    if(field=="CrimeFaction") return resolve(key,RE::FormType::Faction);
    if(field=="DeathItem") return resolve(key,RE::FormType::LeveledItem);
    if(field=="Class") return resolve(key,RE::FormType::Class);
    return resolve(key,RE::FormType::FormList);
}
RE::TESForm*& functionalReference(RE::TESNPC* npc,const nbb::ReferenceField& f) {
    return *reinterpret_cast<RE::TESForm**>(reinterpret_cast<char*>(npc)+f.runtimeOffset);
}
void writeScalar(RE::TESNPC* npc,const nbb::ScalarField& f,const Json& value) {
    auto* ptr=reinterpret_cast<char*>(npc)+f.runtimeOffset;
    auto wanted=nbb::encodeScalarWord(f,readWord(ptr,f.runtimeBytes),value);
    std::memcpy(ptr,&wanted,f.runtimeBytes);
}
Json readPerks(RE::TESNPC* npc) {
    require(npc->perkCount<=4096 && (!npc->perkCount||npc->perks),"Invalid perk array");Json a=Json::array();
    for(unsigned i=0;i<npc->perkCount;++i) {auto& e=npc->perks[i];require(e.perk,"Null perk");a.push_back(Json::array({keyOf(e.perk),int(e.currentRank)}));}std::sort(a.begin(),a.end());return a;
}
Json readKeywords(RE::TESNPC* npc) {
    require(npc->numKeywords<=4096 && (!npc->numKeywords||npc->keywords),"Invalid keywords");Json a=Json::array();
    for(unsigned i=0;i<npc->numKeywords;++i) {require(npc->keywords[i],"Null keyword");a.push_back(keyOf(npc->keywords[i]));}std::sort(a.begin(),a.end());return a;
}
Json readAttacks(RE::TESNPC* npc) {
    Json result=Json::object();auto* map=npc->attackDataMap.get();if(!map)return result;
    require(map->attackDataMap.size()<=4096,"Excessive attack data");
    for(auto& [event,p]:map->attackDataMap) {
        require(bool(p),"Null attack data");const auto& d=p->data;
        result[event.c_str()]={{"damage_mult",d.damageMult},{"chance",d.attackChance},{"spell",reference(d.attackSpell)},{"flags",d.flags.underlying()},
            {"attack_angle",d.attackAngle},{"strike_angle",d.strikeAngle},{"stagger",d.staggerOffset},{"type",reference(d.attackType)},
            {"knockdown",d.knockDown},{"recovery",d.recoveryTime},{"stamina_mult",d.staminaMult}};
    }return result;
}
Json containerExtra(RE::ContainerItemExtra* extra) {
    if(!extra)return nullptr;Json condition=nullptr;
    if(extra->owner) {
        if(extra->owner->GetFormType()==RE::FormType::Faction) condition={{"rank",extra->conditional.rank}};
        else if(extra->owner->GetFormType()==RE::FormType::NPC) condition={{"global",reference(extra->conditional.global)}};
        else throw std::runtime_error("Invalid inventory owner type");
    }
    require(std::isfinite(extra->healthMult),"Nonfinite item health");
    return {{"owner",reference(extra->owner)},{"condition",condition},{"health",extra->healthMult}};
}
Json readField(RE::TESNPC* npc,const std::string& field) {
    if(field=="InheritedName") return std::string(npc->fullName.c_str());
    if(field=="InheritedShortName") return std::string(npc->shortName.c_str());
    if(const auto* f=nbb::scalarField(field)) return scalarValue(npc,*f);
    if(const auto* f=nbb::referenceField(field)) return reference(functionalReference(npc,*f));
    if(field=="SkillOffsets") return std::vector<std::uint8_t>(std::begin(npc->playerSkills.offsets),std::end(npc->playerSkills.offsets));
    if(field=="AttackRace") return reference(npc->attackDataMap?npc->attackDataMap->defaultDataRace:nullptr);
    if(field=="AttackData") return readAttacks(npc);
    if(field=="Perks") return readPerks(npc);
    if(field=="Keywords") return readKeywords(npc);
    if(field=="MagickaOffset") return npc->actorData.magickaOffset;
    if(field=="Protected") return npc->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kProtected);
    if(field=="UseAIPackages") return npc->actorData.templateUseFlags.all(RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kAIPackages);
    if(field=="CombatStyle") return reference(npc->combatStyle);
    if(field=="SkillValues") return std::vector<std::uint8_t>(std::begin(npc->playerSkills.values),std::end(npc->playerSkills.values));
    if(field=="Factions") {
        require(npc->factions.size()<=4096,"Excessive runtime factions");Json result=Json::array();
        for(auto& entry:npc->factions) {require(entry.faction,"Null faction");result.push_back(Json::array({keyOf(entry.faction),int(entry.rank)}));}
        std::sort(result.begin(),result.end());return result;
    }
    if(field=="Inventory") {
        require(npc->numContainerObjects<=4096 && (!npc->numContainerObjects||npc->containerObjects),"Invalid runtime inventory");
        Json result=Json::array();for(std::uint32_t i=0;i<npc->numContainerObjects;++i) {
            auto* item=npc->containerObjects[i];require(item&&item->obj,"Null inventory entry");
            result.push_back({{"item",keyOf(item->obj)},{"count",item->count},{"extra",containerExtra(item->itemExtra)}});
        }
        std::sort(result.begin(),result.end());return result;
    }
    if(field=="Magicka") return npc->playerSkills.magicka;
    if(field=="Stamina") return npc->playerSkills.stamina;
    if(field=="SPLO") {
        nbb::Sequence keys;
        auto* data=npc->actorEffects;
        if(data) {
            auto add=[&](auto** forms,std::uint32_t count) {
                require(count<=4096 && (!count||forms),"Invalid spell array");
                for(std::uint32_t i=0;i<count;++i) {require(forms[i],"Null spell entry");keys.push_back(keyOf(forms[i]));}
            };
            add(data->spells,data->numSpells);add(data->levSpells,data->numlevSpells);add(data->shouts,data->numShouts);
        }
        require(keys.size()<=4096,"Excessive combined spell list");
        std::ranges::sort(keys); keys.erase(std::unique(keys.begin(),keys.end()),keys.end()); return keys;
    }
    if(field=="PKID") {
        Json result=Json::array(); unsigned count=0;
        for(auto* package:npc->aiPackages.packages) {
            require(++count<=4096 && package,"Invalid runtime package list"); result.push_back(keyOf(package));
        }
        return result;
    }
    if(field=="Aggression") return std::to_underlying(npc->GetAggressionLevel());
    if(field=="Confidence") return std::to_underlying(npc->GetConfidenceLevel());
    if(field=="Assistance") return std::to_underlying(npc->GetAssistanceLevel());
    return reference(listField(npc,field));
}
using Snapshot=std::array<std::uint8_t,sizeof(RE::TESNPC)>;
using ActiveFields=std::map<std::string,const Json*>;
Snapshot protectedBytes(RE::TESNPC* npc,const ActiveFields& fields) {
    Snapshot bytes; std::memcpy(bytes.data(),npc,bytes.size());
    auto offset=[npc](const void* p){return address(p)-address(npc);};
    for(auto& [field,_]:fields) {
        if(const auto* f=nbb::scalarField(field)) {
            if(f->runtimeMask) {auto v=readWord(bytes.data()+f->runtimeOffset,f->runtimeBytes)&~f->runtimeMask;std::memcpy(bytes.data()+f->runtimeOffset,&v,f->runtimeBytes);}
            else std::fill_n(bytes.begin()+f->runtimeOffset,f->runtimeBytes,std::uint8_t(0));
        } else if(const auto* f=nbb::referenceField(field)) std::fill_n(bytes.begin()+f->runtimeOffset,8,std::uint8_t(0));
        else if(field=="InheritedName") std::fill_n(bytes.begin()+offset(&npc->fullName),8,std::uint8_t(0));
        else if(field=="InheritedShortName") std::fill_n(bytes.begin()+offset(&npc->shortName),8,std::uint8_t(0));
        else if(field=="AttackRace" || field=="AttackData") std::fill_n(bytes.begin()+offset(&npc->attackDataMap),8,std::uint8_t(0));
        else if(field=="SkillOffsets") std::fill_n(bytes.begin()+offset(&npc->playerSkills.offsets),18,std::uint8_t(0));
        else if(field=="Keywords") {std::fill_n(bytes.begin()+offset(&npc->keywords),8,std::uint8_t(0));std::fill_n(bytes.begin()+offset(&npc->numKeywords),4,std::uint8_t(0));}
        else if(field=="Perks") {std::fill_n(bytes.begin()+offset(&npc->perks),8,std::uint8_t(0));std::fill_n(bytes.begin()+offset(&npc->perkCount),4,std::uint8_t(0));}
        else if(field=="MagickaOffset") std::fill_n(bytes.begin()+offset(&npc->actorData.magickaOffset),2,std::uint8_t(0));
        else if(field=="Protected") bytes[offset(&npc->actorData.actorBaseFlags)+1]&=0xf7;
        else if(field=="UseAIPackages") bytes[offset(&npc->actorData.templateUseFlags)]&=0xdf;
        else if(field=="CombatStyle") std::fill_n(bytes.begin()+offset(&npc->combatStyle),sizeof(void*),std::uint8_t(0));
        else if(field=="SkillValues") std::fill_n(bytes.begin()+offset(&npc->playerSkills.values),18,std::uint8_t(0));
        else if(field=="Factions") std::fill_n(bytes.begin()+offset(&npc->factions),sizeof(npc->factions),std::uint8_t(0));
        else if(field=="Inventory") {
            std::fill_n(bytes.begin()+offset(&npc->containerObjects),sizeof(void*),std::uint8_t(0));
            std::fill_n(bytes.begin()+offset(&npc->numContainerObjects),sizeof(std::uint32_t),std::uint8_t(0));
        }
        else if(field=="Aggression") bytes[offset(&npc->aiData)] &= 0xfc;
        else if(field=="Confidence") bytes[offset(&npc->aiData)] &= 0xe3;
        else if(field=="Assistance") bytes[offset(&npc->aiData)+2] &= 0xf3;
        else if(field=="Magicka") std::fill_n(bytes.begin()+offset(&npc->playerSkills.magicka),2,std::uint8_t(0));
        else if(field=="Stamina") std::fill_n(bytes.begin()+offset(&npc->playerSkills.stamina),2,std::uint8_t(0));
        else if(field=="SPLO") std::fill_n(bytes.begin()+offset(&npc->actorEffects),sizeof(void*),std::uint8_t(0));
        else {
            auto start=field=="PKID"?offset(&npc->aiPackages.packages):offset(&listField(npc,field));
            auto count=field=="PKID"?sizeof(List):sizeof(void*);
            std::fill_n(bytes.begin()+start,count,std::uint8_t(0));
        }
    }
    return bytes;
}
Json descriptor(RE::TESNPC* npc,const std::string& field) {
    if(field=="InheritedName" || field=="InheritedShortName") return nullptr;
    if(field=="AttackData" || field=="AttackRace") return nullptr; // Native verification covers the ref-counted map; external sampling does not traverse it.
    if(const auto* f=nbb::scalarField(field)) return {{"kind","scalar"},{"address",address(npc)+f->runtimeOffset},{"bytes",f->runtimeBytes},{"mask",f->runtimeMask},{"shift",f->shift},{"signed",f->signedValue},{"boolean",f->boolean}};
    if(const auto* f=nbb::referenceField(field)) return {{"kind","reference"},{"address",address(npc)+f->runtimeOffset}};
    if(field=="SkillOffsets") return {{"kind","u8_array"},{"address",address(&npc->playerSkills.offsets)},{"count",18}};
    if(field=="Keywords") return {{"kind","keywords"},{"address",address(&npc->keywords)},{"count_address",address(&npc->numKeywords)}};
    if(field=="Perks") return {{"kind","perks"},{"address",address(&npc->perks)},{"count_address",address(&npc->perkCount)}};
    if(field=="MagickaOffset") return {{"kind","i16"},{"address",address(&npc->actorData.magickaOffset)}};
    if(field=="Protected") return {{"kind","flag"},{"address",address(&npc->actorData.actorBaseFlags)+1},{"mask",8}};
    if(field=="UseAIPackages") return {{"kind","flag"},{"address",address(&npc->actorData.templateUseFlags)},{"mask",32}};
    if(field=="CombatStyle") return {{"kind","reference"},{"address",address(&npc->combatStyle)}};
    if(field=="SkillValues") return {{"kind","u8_array"},{"address",address(&npc->playerSkills.values)},{"count",18}};
    if(field=="Factions") return {{"kind","factions"},{"address",address(&npc->factions)}};
    if(field=="Inventory") return {{"normalized_extras",true},{"kind","inventory"},{"address",address(&npc->containerObjects)},{"count_address",address(&npc->numContainerObjects)}};
    if(field=="Magicka") return {{"kind","u16"},{"address",address(&npc->playerSkills.magicka)}};
    if(field=="Stamina") return {{"kind","u16"},{"address",address(&npc->playerSkills.stamina)}};
    if(field=="SPLO") return {{"kind","spell_arrays"},{"address",address(&npc->actorEffects)}};
    if(field=="PKID") return {{"kind","list"},{"address",address(&npc->aiPackages.packages)}};
    if(field=="Aggression") return {{"kind","bits"},{"address",address(&npc->aiData)},{"mask",3},{"shift",0}};
    if(field=="Confidence") return {{"kind","bits"},{"address",address(&npc->aiData)},{"mask",28},{"shift",2}};
    if(field=="Assistance") return {{"kind","bits"},{"address",address(&npc->aiData)+2},{"mask",12},{"shift",2}};
    return {{"kind","reference"},{"address",address(&listField(npc,field))}};
}
Json manifestFromRuntime() {
    auto* data=RE::TESDataHandler::GetSingleton(); require(data,"TESDataHandler unavailable");
    loadedPluginsByName.clear();
    runtimeFormKeys.clear();
    std::set<const RE::TESFile*> loaded;
    for(auto* file:data->compiledFileCollection.files) { loaded.insert(file); origins[file->GetPartialIndex()]=nbb::lower(std::string(file->GetFilename())); }
    for(auto* file:data->compiledFileCollection.smallFiles) { loaded.insert(file); origins[file->GetPartialIndex()]=nbb::lower(std::string(file->GetFilename())); }
    Json plugins=Json::array(); unsigned full=0,light=0;
    for(auto* file:data->files) if(file && loaded.contains(file)) {
        if(file->IsLight()) require(file->smallFileCompileIndex==light++,"Light plugin order mismatch");
        else require(file->compileIndex==full++,"Full plugin order mismatch");
        auto name=std::string(file->GetFilename());
        loadedPluginsByName.emplace(nbb::lower(name),file);
        plugins.push_back({{"name",nbb::lower(name)},{"path",(std::filesystem::path("Data")/name).string()}});
    }
    require(plugins.size()==loaded.size(),"Runtime loaded plugin inventory incomplete");
    return {{"schema",1},{"plugins",plugins}};
}
void rememberReference(const Json& value,RE::FormType type) {
    if(value.is_null()) return;
    auto* form=resolve(value,type); // Validation is required even when sampling is off.
    if(debugOutput)layout["references"][std::to_string(address(form))]=value;
}
struct PreparedSpellList {
    RE::TESSpellList::SpellData* data{};
    PreparedSpellList()=default;
    PreparedSpellList(const PreparedSpellList&)=delete;
    PreparedSpellList& operator=(const PreparedSpellList&)=delete;
    ~PreparedSpellList() {
        if(data) {RE::free(data->spells);RE::free(data->levSpells);RE::free(data->shouts);std::destroy_at(data);RE::free(data);}
    }
    template<class T> static T** allocate(const std::vector<T*>& values) {
        if(values.empty()) return nullptr;
        auto** memory=RE::calloc<T*>(values.size());require(memory,"Spell array allocation failed");
        std::ranges::copy(values,memory);return memory;
    }
    void prepare(const Json& values) {
        std::vector<RE::SpellItem*> spells;std::vector<RE::TESLevSpell*> leveled;std::vector<RE::TESShout*> shouts;
        for(auto& value:values) {
            auto* f=resolve(value,RE::FormType::None);
            if(f->GetFormType()==RE::FormType::Spell) spells.push_back(static_cast<RE::SpellItem*>(f));
            else if(f->GetFormType()==RE::FormType::LeveledSpell) leveled.push_back(static_cast<RE::TESLevSpell*>(f));
            else shouts.push_back(static_cast<RE::TESShout*>(f));
        }
        auto* memory=static_cast<RE::TESSpellList::SpellData*>(RE::malloc(sizeof(RE::TESSpellList::SpellData)));
        require(memory,"Spell list allocation failed");data=std::construct_at(memory);
        data->spells=allocate(spells);data->numSpells=static_cast<std::uint32_t>(spells.size());
        data->levSpells=allocate(leveled);data->numlevSpells=static_cast<std::uint32_t>(leveled.size());
        data->shouts=allocate(shouts);data->numShouts=static_cast<std::uint32_t>(shouts.size());
    }
    void commit(RE::TESNPC* npc) {
        if(!data) return;
        if(!npc->actorEffects) {npc->actorEffects=std::exchange(data,nullptr);return;}
        auto old=*npc->actorEffects;*npc->actorEffects=*data;
        data->spells=nullptr;data->levSpells=nullptr;data->shouts=nullptr;
        RE::free(old.spells);RE::free(old.levSpells);RE::free(old.shouts);
    }
};

struct PreparedInventory {
    RE::ContainerObject** data{};std::uint32_t count{};
    PreparedInventory()=default;
    PreparedInventory(const PreparedInventory&)=delete;
    PreparedInventory& operator=(const PreparedInventory&)=delete;
    ~PreparedInventory() {if(data) {for(std::uint32_t i=0;i<count;++i) if(data[i]) {delete data[i]->itemExtra;delete data[i];}RE::free(data);}}
    void prepare(const Json& entries) {
        count=static_cast<std::uint32_t>(entries.size());if(!count) return;
        data=RE::calloc<RE::ContainerObject*>(count);require(data,"Inventory array allocation failed");
        for(std::uint32_t i=0;i<count;++i) {
            const auto& entry=entries.at(i);
            auto* item=static_cast<RE::TESBoundObject*>(resolve(entry.at("item"),RE::FormType::Max));
            data[i]=new RE::ContainerObject(item,entry.at("count").get<std::int32_t>());
            const auto& extra=entry.at("extra");if(!extra.is_null()) {
                auto* owner=resolve(extra.at("owner"),RE::FormType::NPC,{RE::FormType::NPC,RE::FormType::Faction});
                auto* e=new RE::ContainerItemExtra(owner);data[i]->itemExtra=e;e->healthMult=extra.at("health").get<float>();
                const auto& condition=extra.at("condition");
                if(owner && owner->GetFormType()==RE::FormType::Faction) e->conditional.rank=condition.at("rank").get<std::int32_t>();
                else if(owner) e->conditional.global=static_cast<RE::TESGlobal*>(resolve(condition.at("global"),RE::FormType::Global));
            }
        }
    }
    void commit(RE::TESNPC* npc) {
        auto** old=npc->containerObjects;auto n=npc->numContainerObjects;
        npc->containerObjects=std::exchange(data,nullptr);npc->numContainerObjects=count;
        for(std::uint32_t i=0;i<n;++i) {if(old[i]) delete old[i]->itemExtra;delete old[i];}RE::free(old);
    }
};
struct PreparedPerks {
    RE::PerkRankData* data{};std::uint32_t count{};bool ready=false;
    ~PreparedPerks(){RE::free(data);}
    void prepare(const Json& values) {
        count=static_cast<std::uint32_t>(values.size());data=count?RE::calloc<RE::PerkRankData>(count):nullptr;require(!count||data,"Perk allocation failed");
        for(unsigned i=0;i<count;++i) {data[i].perk=static_cast<RE::BGSPerk*>(resolve(values[i][0],RE::FormType::Perk));data[i].currentRank=values[i][1].get<std::int8_t>();}ready=true;
    }
    void commit(RE::TESNPC* npc) {if(!ready)return;auto* old=npc->perks;npc->perks=std::exchange(data,nullptr);npc->perkCount=count;RE::free(old);}
};
struct PreparedKeywords {
    RE::BGSKeyword** data{};std::uint32_t count{};bool ready=false;
    ~PreparedKeywords(){RE::free(data);}
    void prepare(const Json& values) {
        count=static_cast<std::uint32_t>(values.size());data=count?RE::calloc<RE::BGSKeyword*>(count):nullptr;require(!count||data,"Keyword allocation failed");
        for(unsigned i=0;i<count;++i) data[i]=static_cast<RE::BGSKeyword*>(resolve(values[i],RE::FormType::Keyword));ready=true;
    }
    void commit(RE::TESNPC* npc) {if(!ready)return;auto* old=npc->keywords;npc->keywords=std::exchange(data,nullptr);npc->numKeywords=count;RE::free(old);}
};
struct PreparedAttackMap {
    RE::NiPointer<RE::BGSAttackDataMap> data;
    void prepare(RE::TESNPC* npc,const ActiveFields& active) {
        auto* raw=RE::calloc<RE::BGSAttackDataMap>(1);require(raw,"Attack map allocation failed");
        // Construct the same base and container members as the native map. The
        // native vtable owns destruction after the NiPointer is committed.
        std::construct_at(static_cast<RE::NiRefObject*>(raw));
        std::construct_at(&raw->attackDataMap);
        RE::stl::emplace_vtable<RE::BGSAttackDataMap>(raw);
        data.reset(raw);
        data->defaultDataRace=npc->attackDataMap?npc->attackDataMap->defaultDataRace:nullptr;
        if(active.contains("AttackRace")) data->defaultDataRace=static_cast<RE::TESRace*>(resolve(active.at("AttackRace")->at("expected"),RE::FormType::Race));
        if(!active.contains("AttackData")) {
            if(npc->attackDataMap) for(auto& entry:npc->attackDataMap->attackDataMap) data->attackDataMap.insert(entry);
            return;
        }
        for(auto& [event,value]:active.at("AttackData")->at("expected").items()) {
            // Resolve every reference before invoking the native constructor.
            auto* spell=resolve(value.at("spell"),RE::FormType::Spell,{RE::FormType::Spell,RE::FormType::Shout});
            auto* type=resolve(value.at("type"),RE::FormType::Keyword);
            RE::NiPointer<RE::BGSAttackData> item(RE::BGSAttackData::Create());require(bool(item),"Attack allocation failed");item->event=event.c_str();auto& d=item->data;
            d.damageMult=value.at("damage_mult").get<float>();d.attackChance=value.at("chance").get<float>();d.attackSpell=static_cast<RE::SpellItem*>(spell);
            d.flags=static_cast<RE::AttackData::AttackFlag>(value.at("flags").get<std::uint32_t>());
            d.attackAngle=value.at("attack_angle").get<float>();d.strikeAngle=value.at("strike_angle").get<float>();d.staggerOffset=value.at("stagger").get<float>();
            d.attackType=static_cast<RE::BGSKeyword*>(type);d.knockDown=value.at("knockdown").get<float>();d.recoveryTime=value.at("recovery").get<float>();d.staminaMult=value.at("stamina_mult").get<float>();
            data->attackDataMap.insert({item->event,item});
        }
    }
};
// Frozen before any Bridge writes. Only proven old inheritance values relax conflicts.
Json frozenInheritance=Json::object(), projectedInheritance=Json::object();
std::map<std::string,RE::TESNPC*> inheritanceObjects;
std::map<std::string,std::size_t> inheritanceRows;
std::set<std::string> inheritanceScope;
std::set<std::string> fullSnapshotScope;
std::set<std::string> nativeFallbackScope;
bool selectiveNativeWork=false;
nbb::StatPredictionCache statPredictionCache;
std::unique_ptr<nbb::InheritanceContext> inheritanceContext;
nbb::FinalizedProviderValues providerValues;
Json statPrediction(RE::TESNPC* npc,const Json& values) {
    require(npc->race,"Automatic stats require a race");
    auto* cls=static_cast<RE::TESClass*>(resolve(values.at("Class"),RE::FormType::Class));require(cls,"Automatic stats require a class");
    auto* settings=RE::GameSettingCollection::GetSingleton();require(settings,"Game settings unavailable");
    auto setting=[&](const char* key){auto* s=settings->GetSetting(key);require(s,key);return s;};
    auto& race=npc->race->data;auto& c=cls->data;
    int level=values.at("Level");if(values.at("PCLevelMult").get<bool>()) {int minimum=values.at("CalcMinLevel"),maximum=values.at("CalcMaxLevel");level=std::max(minimum,std::min(maximum?maximum:65535,level/1000));}
    level=std::max(1,level);const auto skillStart=setting("iAVDSkillStart")->GetInteger();
    Json skills=std::vector<int>(18,skillStart);
    for(auto b:race.skillBoosts){auto skill=b.skill.underlying();if(skill>=6&&skill<24)skills[skill-6]=skills[skill-6].get<int>()+b.bonus;}
    Json attributes=Json::array();unsigned i=0;for(auto base:{race.startingHealth,race.startingMagicka,race.startingStamina}) {
        require(std::isfinite(base)&&base>=0&&base<=65535&&std::floor(base)==base,"Unsupported race starting attribute");
        auto field=std::array<const char*,3>{"HealthOffset","MagickaOffset","StaminaOffset"}[i++];attributes.push_back(int(base)+values.at(field).get<int>());
    }
    auto* sw=reinterpret_cast<const std::uint8_t*>(&c.skillWeights);auto* aw=reinterpret_cast<const std::uint8_t*>(&c.attributeWeights);
    return statPredictionCache.calculate({{"level",level},{"skills",skills},{"attributes",attributes},{"skill_weights",std::vector<std::uint8_t>(sw,sw+18)},
        {"attribute_weights",std::vector<std::uint8_t>(aw,aw+3)},{"skill_points",setting("iAVDSkillsLevelUp")->GetInteger()},
        {"attribute_points",setting("iAVDhmsLevelUp")->GetInteger()},{"health_bonus",setting("fNPCHealthLevelBonus")->GetFloat()}});
}
Json frozenField(const std::string& key,const std::string& field) {
    auto& values=frozenInheritance.at(key)["values"];
    if(!values.contains(field))values[field]=readField(inheritanceObjects.at(key),field);
    return values.at(field);
}
void freezeInheritance(const std::set<std::string>& only) {
    const auto freezeStarted=std::chrono::steady_clock::now();
    statPredictionCache.clear();fullSnapshotScope.clear();providerValues.clear();
    std::set<std::string> capture{"Template","InheritedName","InheritedShortName"};
    for(auto& f:plan.at("enabled_fields"))if((nbb::inheritanceBit(f.get<std::string>())||nbb::isTemplateField(f.get<std::string>()))&&f!="VMAD")capture.insert(f);
    capture.insert(nbb::statInputs.begin(),nbb::statInputs.end());for(auto f:{"SkillValues","Health","Magicka","Stamina"})capture.insert(f);
    for(std::size_t i=0;i<plan["npcs"].size();++i) {
        auto& row=plan["npcs"][i];auto k=row["npc"].get<std::string>();inheritanceRows[k]=i;
        if(k!="skyrim.esm:000007" && row.at("inheritance_record").at("writable").get<bool>()&&(only.empty()||only.contains(k)))inheritanceScope.insert(k);
        try {
            auto* npc=static_cast<RE::TESNPC*>(resolve(k,RE::FormType::NPC));inheritanceObjects[k]=npc;
            if(const auto* name=npc->GetName())row["display_name"]=name;
            Json node={{"flags",npc->actorData.templateUseFlags.underlying()},{"template",reference(npc->baseTemplateForm)},{"values",Json::object()}};
            // First discover every runtime relationship, before any Bridge writes.
            // Only candidate relationship fields are needed to project the new graph.
            for(auto& [f,s]:row["fields"].items())if(f=="Template"||nbb::isTemplateField(f))
                try{node["values"][f]=readField(npc,f);}catch(const std::exception& ex){node["errors"][f]=ex.what();}
            projectedInheritance[k]={{"flags",node.at("flags")},{"template",node.at("template")}};
            frozenInheritance[k]=std::move(node);
        } catch(const std::exception& ex) {row["inheritance_error"]=ex.what();}
    }
    for(auto& k:inheritanceScope) {
        if(!projectedInheritance.contains(k))continue;
        auto& row=plan["npcs"][inheritanceRows.at(k)];auto& node=projectedInheritance[k];
        const auto& values=frozenInheritance.at(k).at("values");
        for(auto& [f,s]:row["fields"].items()) {
            if(!values.contains(f)||!nbb::permitsValue(values.at(f),s))continue;
            if(f=="Template") {if(!nbb::templateSharedWithProtectedData(node["flags"].get<std::uint16_t>()))node["template"]=s["expected"];}
            else if(nbb::isTemplateField(f)) {
                unsigned mask=f=="UseAIPackages"?32:nbb::scalarField(f)->runtimeMask;
                auto flags=node["flags"].get<unsigned>();node["flags"]=(flags&~mask)|(s["expected"].get<bool>()?mask:0);
            }
        }
    }
    inheritanceContext=std::make_unique<nbb::InheritanceContext>(frozenInheritance,projectedInheritance,plan.at("enabled_fields"));
    const auto preflightStarted=std::chrono::steady_clock::now();
    selectiveNativeWork=nbb::load_capture::canPruneNativeWork();nativeFallbackScope.clear();
    std::size_t seedCount=0;
    if(selectiveNativeWork) {
        std::set<std::string> pending;
        for(auto& row:plan["npcs"]) {
            auto key=row.at("npc").get<std::string>();
            if(row.contains("inheritance_error")){pending.insert(key);continue;}
            if(!inheritanceScope.contains(key)||row.at("status")!="ready")continue;
            auto* npc=inheritanceObjects.at(key);const bool automatic=npc->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kAutoCalcStats);
            try {
                if(nbb::needsNativeFallback(row,automatic,inheritanceContext->frozen,inheritanceContext->projected,
                    [](const std::string& k,const std::string& f){return frozenField(k,f);},
                    [](const std::string& k){auto* parent=inheritanceObjects.at(k);return !parent->race||parent->attackDataMap!=parent->race->attackDataMap;}))pending.insert(key);
            }catch(const std::exception&){pending.insert(key);} // Original full path retains the diagnostic.
        }
        seedCount=pending.size();auto work=nbb::remainingInheritanceWork(pending,frozenInheritance,projectedInheritance);
        nativeFallbackScope=std::move(work.affected);fullSnapshotScope=std::move(work.capture);
    } else {fullSnapshotScope=nbb::inheritanceCaptureScope(plan.at("npcs"),frozenInheritance,projectedInheritance);nativeFallbackScope=fullSnapshotScope;}
    const auto captureStarted=std::chrono::steady_clock::now();
    std::size_t captured=0;
    for(auto& [k,npc]:inheritanceObjects)if(fullSnapshotScope.contains(k)) {
        auto& node=frozenInheritance[k];auto& values=node["values"];++captured;
        for(const auto& f:capture)try{if(!values.contains(f))values[f]=readField(npc,f);}catch(const std::exception& ex){node["errors"][f]=ex.what();}
        try{node["calculated"]=statPrediction(npc,values);}catch(const std::exception& ex){node["calculation_error"]=ex.what();}
    }
    SKSE::log::info("Snapshot scope: {} full NPC snapshots; {} relationship-only NPCs",captured,inheritanceObjects.size()-captured);
    if(selectiveNativeWork)SKSE::log::info("Remaining native work: {} pending seeds; {} affected NPCs; {} captured providers; stable branches skip repeated inheritance/calculation",seedCount,nativeFallbackScope.size(),captured);
    SKSE::log::info("Timing detail: native_graph_projection={:.3f} ms; remaining_work_preflight={:.3f} ms; native_snapshot_calculation={:.3f} ms (included in freeze_template_inheritance)",
        std::chrono::duration<double,std::milli>(preflightStarted-freezeStarted).count(),std::chrono::duration<double,std::milli>(captureStarted-preflightStarted).count(),
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-captureStarted).count());
}
Json providerValue(const std::string& key,const std::string& field,bool apply) {
    require(inheritanceObjects.contains(key),"Missing runtime template");
    return providerValues.get(key,field,[&]() -> Json {
        const auto& parent=plan["npcs"][inheritanceRows.at(key)];
        if(!apply&&parent.contains("field_results")&&parent["field_results"].contains(field)) {
            auto status=parent["field_results"][field].value("status",std::string());
            if(status=="audit_needs_changes"||status=="audit_already_matches")return parent["fields"][field]["expected"];
        }
        return readField(inheritanceObjects.at(key),field);
    });
}
void settleNativeFields(Json& row) {
    if(row.at("status")!="ready")return;
    auto key=row.at("npc").get<std::string>();auto* npc=inheritanceObjects.at(key);
    const bool automatic=npc->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kAutoCalcStats);
    nbb::settleInheritedFields(row,automatic,inheritanceContext->frozen,
        [](const std::string& k,const std::string& f){return frozenField(k,f);},
        [](const std::string& k){auto* parent=inheritanceObjects.at(k);return !parent->race||parent->attackDataMap!=parent->race->attackDataMap;});
}
void planInheritedFields(Json& row,bool apply) {
    auto key=row.at("npc").get<std::string>();if(!inheritanceObjects.contains(key))return;
    auto* npc=inheritanceObjects.at(key);
    if(nativeFallbackScope.contains(key))nbb::planInheritedFields(row,plan.at("enabled_fields"),frozenInheritance,projectedInheritance,
        [&](const std::string& k,const std::string& f){return providerValue(k,f,apply);},
        [&](const Json& values){return statPrediction(npc,values);},
        [&](const std::string& k){auto* parent=inheritanceObjects.at(k);return !parent->race || parent->attackDataMap!=parent->race->attackDataMap;},inheritanceContext.get());
    else if(selectiveNativeWork)settleNativeFields(row);
    else return;
    if(std::find(plan["enabled_fields"].begin(),plan["enabled_fields"].end(),Json("VMAD"))!=plan["enabled_fields"].end()) try {
        auto oldSource=inheritanceContext->frozen.provider(key,512),source=inheritanceContext->projected.provider(key,512);
        if(source==key)return; // Local VMAD was already selected by the core plan.
        auto& parent=plan["npcs"][inheritanceRows.at(source)];auto& oldParent=plan["npcs"][inheritanceRows.at(oldSource)];
        const auto& before=oldParent["inheritance_record"]["fields"]["VMAD"];
        const Json* value=&parent["inheritance_record"]["fields"]["VMAD"];
        if(parent["fields"].contains("VMAD")&&parent["fields"]["VMAD"]["status"]=="ready")value=&parent["fields"]["VMAD"]["expected"];
        if(source!=oldSource||*value!=before||row["fields"].contains("VMAD")) {
            require(!value->contains("unparsed"),"Inherited VMAD is unparsed");
            nbb::addInheritedField(row,"VMAD",*value,before,source);
            row["fields"]["VMAD"]["sources"]=parent["fields"].contains("VMAD")?parent["fields"]["VMAD"]["sources"]:Json::array({parent["winner"]});
        }
    }catch(const std::exception& ex){row["inheritance_notes"]["VMAD"]=ex.what();if(row["fields"].contains("VMAD")){row["fields"]["VMAD"]["status"]="skipped";row["fields"]["VMAD"]["reason"]="Unresolved inherited VMAD";}}
}

void prepareAndApply(Json& row,bool apply) {
    if(row.at("status")!="ready") return;
    auto* npc=static_cast<RE::TESNPC*>(resolve(row.at("npc"),RE::FormType::NPC));
    if(npc->GetFormID()==0x7) {row["runtime_status"]="skipped_player";row["reason"]="Player base record is always excluded";return;}
    const auto& fields=row.at("fields");Json before=Json::object(),results=Json::object(),normalizations=Json::object();ActiveFields active;
    for(auto& [field,spec]:fields.items()) {
        if(spec.at("status")!="ready") {results[field]={{"status","skipped_plan"},{"reason",spec.at("reason")}};continue;}
        if(field=="VMAD") {results[field]={{"status","pending_actor_binding"},{"reason","Separate Actor-instance adapter; see script binding reports"}};continue;}
        if(field=="Template" && nbb::templateSharedWithProtectedData(npc->actorData.templateUseFlags.underlying())) {
            results[field]={{"status","skipped_shared_template"},{"reason","Runtime template shared with appearance (Use Traits)"}};continue;
        }
        if(field=="Destructible") {results[field]={{"status","unsupported_runtime_adapter"},{"reason","Destruction-stage model ownership/preload lifetime is not implemented; tracked but not written"}};continue;}
        try {
            auto actual=readField(npc,field);before[field]=actual;
            bool defaultFill=false;
            if(field=="CombatStyle" && spec.at("disk").is_null() && !actual.is_null()) {
                auto* manager=RE::BGSDefaultObjectManager::GetSingleton();
                // This early object slot is stable across supported SE/AE versions;
                // validate the loaded form instead of relying on version-varying init flags.
                auto* defaultStyle=manager?manager->objects[std::to_underlying(RE::DEFAULT_OBJECT::kCombatStyle)]:nullptr;
                if(defaultStyle && defaultStyle->GetFormType()==RE::FormType::CombatStyle && !defaultStyle->IsDeleted())
                    defaultFill=nbb::isDefaultCombatStyleFill(field,spec.at("disk"),actual,reference(defaultStyle));
                if(defaultFill) normalizations[field]={{"reason","engine_default_for_unspecified"},{"actual",actual},{"disk_equivalent",nullptr}};
            }
            bool inheritedFill=spec.contains("inherited_old_value") && actual==spec.at("inherited_old_value");
            if(inheritedFill) normalizations[field]={{"reason","verified_old_inherited_or_calculated_value"},{"actual",actual}};
            if(actual!=spec.at("disk") && actual!=spec.at("expected") && !defaultFill && !inheritedFill) results[field]={{"status","skipped_runtime_conflict"},{"actual",actual}};
            else active[field]=&spec;
        } catch(const std::exception& e) {results[field]={{"status","read_error"},{"reason",e.what()}};}
    }
    // Publish snapshots once preparation is finished; avoid retaining duplicate trees.
    if(!normalizations.empty()) row["runtime_normalizations"]=std::move(normalizations);
    List replacement;PreparedSpellList spellReplacement;PreparedInventory inventory;PreparedPerks perks;PreparedKeywords keywords;PreparedAttackMap attacks;
    RE::BSTArray<RE::FACTION_RANK> factions;
    std::map<std::string,RE::TESForm*> resolvedReferences;
    std::map<std::string,RE::BSFixedString> inheritedNames;
    std::map<std::string,RE::BGSListForm*> lists;RE::TESCombatStyle* combatStyle=nullptr;
    bool replacePackages=false,replaceInventory=false,replaceFactions=false;
    std::vector<std::string> failed;
    for(auto& [field,specPointer]:active) {
        const auto& spec=*specPointer;
        try {
            const auto& expected=spec.at("expected");bool changed=before[field]!=expected;
            if(field=="InheritedName" || field=="InheritedShortName") {
                if(apply&&changed)inheritedNames.emplace(field,expected.get<std::string>().c_str());
            } else if(const auto* f=nbb::scalarField(field)) {
                const auto maximum=f->runtimeMask?(f->runtimeMask>>f->shift):f->runtimeBytes==4?UINT32_MAX:((1u<<(8*f->runtimeBytes))-1);
                if(!f->boolean && !f->signedValue) require(expected.get<std::uint64_t>()<=maximum,"Value exceeds runtime scalar storage");
            } else if(nbb::referenceField(field)) {
                auto* wanted=resolveFunctionalReference(expected,field);resolvedReferences[field]=wanted;
                for(const auto* value:std::array<const Json*,2>{&expected,&before[field]}) if(!value->is_null()) {auto* form=resolveFunctionalReference(*value,field);if(debugOutput)layout["references"][std::to_string(address(form))]=*value;}
            } else if(field=="Perks" || field=="Keywords") {
                auto type=field=="Perks"?RE::FormType::Perk:RE::FormType::Keyword;
                for(const auto* values:std::array<const Json*,2>{&expected,&before[field]}) for(const auto& e:*values) rememberReference(field=="Perks"?e.at(0):e,type);
                if(apply&&changed) {if(field=="Perks") perks.prepare(expected);else keywords.prepare(expected);}
            } else if(field=="AttackRace") {
                resolve(expected,RE::FormType::Race);
            } else if(field=="AttackData") {
                for(const auto& attack:expected) {resolve(attack.at("spell"),RE::FormType::Spell,{RE::FormType::Spell,RE::FormType::Shout});resolve(attack.at("type"),RE::FormType::Keyword);}
            } else if(field=="SPLO") {
                for(auto& v:expected) rememberReference(v,RE::FormType::None);
                for(auto& v:before[field]) rememberReference(v,RE::FormType::None);
                if(apply&&changed) spellReplacement.prepare(expected);
            } else if(field=="PKID") {
                for(auto& v:expected) rememberReference(v,RE::FormType::Package);
                for(auto& v:before[field]) rememberReference(v,RE::FormType::Package);
                if(apply&&changed) {
                    for(auto it=expected.rbegin();it!=expected.rend();++it) replacement.push_front(static_cast<RE::TESPackage*>(resolve(*it,RE::FormType::Package)));
                    replacePackages=true;
                }
            } else if(field=="Factions") {
                for(const auto* entries:std::array<const Json*,2>{&expected,&before[field]}) for(auto& e:*entries) rememberReference(e.at(0),RE::FormType::Faction);
                if(apply&&changed) {
                    factions.reserve(static_cast<std::uint32_t>(expected.size()));
                    for(auto& e:expected) factions.push_back(RE::FACTION_RANK{static_cast<RE::TESFaction*>(resolve(e.at(0),RE::FormType::Faction)),e.at(1).get<std::int8_t>()});
                    replaceFactions=true;
                }
            } else if(field=="Inventory") {
                for(const auto* entries:std::array<const Json*,2>{&expected,&before[field]}) for(auto& e:*entries) {
                    rememberReference(e.at("item"),RE::FormType::Max);const auto& extra=e.at("extra");
                    if(extra.is_object()) {
                        auto* owner=resolve(extra.at("owner"),RE::FormType::NPC,{RE::FormType::NPC,RE::FormType::Faction});
                        if(debugOutput&&owner) layout["references"][std::to_string(address(owner))]=extra.at("owner");
                        if(extra.at("condition").is_object() && extra["condition"].contains("global")) rememberReference(extra["condition"]["global"],RE::FormType::Global);
                    }
                }
                if(apply&&changed) {inventory.prepare(expected);replaceInventory=true;}
            } else if(field=="CombatStyle") {
                rememberReference(expected,RE::FormType::CombatStyle);rememberReference(before[field],RE::FormType::CombatStyle);
                combatStyle=static_cast<RE::TESCombatStyle*>(resolve(expected,RE::FormType::CombatStyle));
            } else if(field=="SPOR"||field=="ECOR"||field=="OCOR"||field=="GWOR") {
                rememberReference(expected,RE::FormType::FormList);rememberReference(before[field],RE::FormType::FormList);
                lists[field]=static_cast<RE::BGSListForm*>(resolve(expected,RE::FormType::FormList));
            }
        } catch(const std::exception& e) {failed.push_back(field);results[field]={{"status","preparation_error"},{"reason",e.what()}};}
    }
    for(auto& field:failed) active.erase(field);
    if(apply && ((active.contains("AttackRace") && before["AttackRace"]!=active.at("AttackRace")->at("expected")) ||
                 (active.contains("AttackData") && before["AttackData"]!=active.at("AttackData")->at("expected")))) {
        try {attacks.prepare(npc,active);} catch(const std::exception& e) {
            for(auto field:{"AttackRace","AttackData"}) if(active.contains(field)) {active.erase(field);results[field]={{"status","preparation_error"},{"reason",e.what()}};}
        }
    }
    if(active.empty()) {row["runtime_status"]=results.contains("VMAD") && results["VMAD"]["status"]=="pending_actor_binding"?"pending_actor_binding":"skipped_all_fields";row["field_results"]=std::move(results);row["runtime_before"]=std::move(before);return;}
    row["runtime_before"]=std::move(before);
    const bool needsWrite=std::any_of(active.begin(),active.end(),[&](const auto& e){return row["runtime_before"].at(e.first)!=e.second->at("expected");});
    const auto protectedBefore=needsWrite?protectedBytes(npc,active):Snapshot{};
    // Each complete replacement is allocated before committing any eligible field.
    if(apply&&needsWrite) {
        for(auto& [field,specPointer]:active) if(nbb::isTemplateField(field)) {
            const auto& spec=*specPointer;
            if(field=="UseAIPackages") {
                if(spec.at("expected").get<bool>()) npc->actorData.templateUseFlags.set(RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kAIPackages);
                else npc->actorData.templateUseFlags.reset(RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kAIPackages);
            } else writeScalar(npc,*nbb::scalarField(field),spec.at("expected"));
        }
        if(active.contains("Template")) functionalReference(npc,*nbb::referenceField("Template"))=resolvedReferences.at("Template");
        for(auto& [field,specPointer]:active) {
            const auto& spec=*specPointer;
            const auto& value=spec.at("expected");
            if(nbb::isTemplateField(field)||field=="Template") continue;
            if(field=="InheritedName") {if(inheritedNames.contains(field))npc->fullName=std::move(inheritedNames.at(field));}
            else if(field=="InheritedShortName") {if(inheritedNames.contains(field))npc->shortName=std::move(inheritedNames.at(field));}
            else if(const auto* f=nbb::scalarField(field)) writeScalar(npc,*f,value);
            else if(const auto* f=nbb::referenceField(field)) functionalReference(npc,*f)=resolvedReferences.at(field);
            else if(field=="SkillOffsets") for(unsigned i=0;i<18;++i) npc->playerSkills.offsets[i]=value.at(i).get<std::uint8_t>();
            else if(field=="Aggression") npc->SetAggressionLevel(static_cast<RE::ACTOR_AGGRESSION>(value.get<int>()));
            else if(field=="Confidence") npc->SetConfidenceLevel(static_cast<RE::ACTOR_CONFIDENCE>(value.get<int>()));
            else if(field=="Assistance") npc->SetAssistanceLevel(static_cast<RE::ACTOR_ASSISTANCE>(value.get<int>()));
            else if(field=="Magicka") npc->playerSkills.magicka=value.get<std::uint16_t>();
            else if(field=="Stamina") npc->playerSkills.stamina=value.get<std::uint16_t>();
            else if(field=="MagickaOffset") npc->actorData.magickaOffset=value.get<std::int16_t>();
            else if(field=="Protected") {
                if(value.get<bool>()) npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kProtected);
                else npc->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kProtected);
            } else if(field=="UseAIPackages") {
                if(!value.get<bool>()) npc->actorData.templateUseFlags.reset(RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kAIPackages);
            } else if(field=="SkillValues") for(unsigned i=0;i<18;++i) npc->playerSkills.values[i]=value.at(i).get<std::uint8_t>();
            else if(field=="CombatStyle") npc->combatStyle=combatStyle;
            else if(lists.contains(field)) listField(npc,field)=lists.at(field);
        }
        if(replacePackages && active.contains("PKID")) npc->aiPackages.packages=std::move(replacement);
        if(active.contains("SPLO")) spellReplacement.commit(npc);
        if(replaceFactions && active.contains("Factions")) npc->factions=std::move(factions);
        if(replaceInventory && active.contains("Inventory")) inventory.commit(npc);
        if(active.contains("Perks")) perks.commit(npc);
        if(active.contains("Keywords")) keywords.commit(npc);
        if(attacks.data && (active.contains("AttackRace") || active.contains("AttackData")))
            npc->attackDataMap=std::move(attacks.data);
    }
    row["non_target_object_bytes_unchanged"]=!needsWrite||protectedBefore==protectedBytes(npc,active);
    if(!needsWrite)row["no_native_write_required"]=true;
    require(row["non_target_object_bytes_unchanged"].get<bool>(),"Non-target NPC bytes changed during write");
    Json after=Json::object(),descriptors;bool matches=true;
    for(auto& [field,specPointer]:active) {
        const auto& spec=*specPointer;
        after[field]=readField(npc,field);bool match=after[field]==spec.at("expected");matches&=match;
        results[field]={{"status",apply?(match?"applied_verified":"apply_mismatch"):(match?"audit_already_matches":"audit_needs_changes")}};
        if(debugOutput) {
            auto d=descriptor(npc,field);
            if(!d.is_null()) {d["expected"]=spec.at("expected");d["baseline"]=after[field];descriptors[field]=std::move(d);}
            else results[field]["external_sampling"]="not_exported; verified by native reader";
        }
    }
    bool partial=active.size()!=fields.size();row["field_results"]=std::move(results);row["runtime_after"]=std::move(after);
    row["runtime_status"]=partial?(apply?"partial_apply":"partial_audit"):
        apply?(matches?"applied_verified":"apply_mismatch"):(matches?"audit_already_matches":"audit_needs_changes");
    if(!descriptors.empty()) layout["npcs"].push_back({{"npc",row.at("npc")},{"editor_id",row.at("editor_id")},{"address",address(npc)},
        {"identity_address",address(&npc->formID)},{"form_id",npc->GetFormID()}});
    if(!descriptors.empty())layout["npcs"].back()["fields"]=std::move(descriptors);
    require(!apply||matches,"Immediate post-write verification failed");
}

void debugReport(const char* name,const Json& data,bool append=false) {
    try {nbb::diagnostics::writeReport(debugOutput,output/name,data,append);}
    catch(const std::exception& e) {SKSE::log::error("Diagnostic output failed: {}",e.what());}
}

void verify(const std::string& phase) {
    if(plan.is_null()) return;
    Json rows=Json::array(); unsigned failures=0,checked=0;
    for(auto& row:plan.at("npcs")) {
        if(!row.contains("runtime_after") || row.at("runtime_after").empty()) continue;
        try {
            auto* npc=static_cast<RE::TESNPC*>(resolve(row.at("npc"),RE::FormType::NPC));
            for(auto& [field,baseline]:row.at("runtime_after").items()) {
                const auto& spec=row.at("fields").at(field);
                auto actual=readField(npc,field); ++checked;
                if(actual!=baseline) {
                    ++failures;
                    if(debugOutput) rows.push_back({{"npc",row.at("npc")},{"field",field},{"baseline",baseline},{"actual",actual},{"expected",spec.at("expected")}});
                    if(phase=="data_loaded") SKSE::log::error("Startup verification mismatch: NPC {} field {}; expected={}, actual={}",row.at("npc").get<std::string>(),field,baseline.dump(),actual.dump());
                }
            }
        } catch(const std::exception& e) {
            ++failures; rows.push_back({{"npc",row.at("npc")},{"error",e.what()}});
            SKSE::log::error("Verification {}: NPC {}: {}",phase,row.at("npc").get<std::string>(),e.what());
        }
    }
    Json event={{"phase",phase},{"sequence",++verification},{"fields_checked",checked},{"changed_or_errors",failures},{"details",rows},{"mode",plan.at("mode")}};
    debugReport("NPCBehaviorBridge-verification.jsonl",event,true);
    if(debugOutput) SKSE::log::info("Verification {}: {} fields, {} differences/read failures; read-only",phase,checked,failures);
    else if(phase=="data_loaded") SKSE::log::info("Startup verification: {} fields checked; {} differences/read failures",checked,failures);
}
class MenuEvents final:public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
        if(event && !event->opening && samplingReady.load() && (event->menuName=="Loading Menu" || event->menuName=="Sleep/Wait Menu")) {
            auto phase=std::string(event->menuName.c_str())+" closed";
            SKSE::GetTaskInterface()->AddTask([phase]{if(samplingReady.load()) {
                verify(phase);
                if(phase=="Loading Menu closed") nbb::scripts::sceneReady();
                else nbb::scripts::check(phase);
            }});
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};
MenuEvents menuEvents;
void initialize() {
    if(initialized) return; initialized=true;
    using Clock=std::chrono::steady_clock;
    const auto initializationStarted=Clock::now();
    auto stageStarted=initializationStarted;
    const auto timing=[&](const char* stage) {
        const auto ended=Clock::now();
        SKSE::log::info("Timing: {} = {:.3f} ms",stage,std::chrono::duration<double,std::milli>(ended-stageStarted).count());
        stageStarted=Clock::now();  // Stage measurements exclude their own timing-line write.
    };
    auto config=nbb::readJson("Data/SKSE/Plugins/NPCBehaviorBridge.json");
    nbb::validateConfig(config); debugOutput=config.value("debug",false);
    if(debugOutput) spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%l] [%s:%#] %v");
    nbb::diagnostics::resetReports(output,[](const std::string& error){SKSE::log::error("Cannot clear previous diagnostic report: {}",error);});
    SKSE::log::info("Mode: {}; debug: {}",config.at("mode").get<std::string>(),debugOutput);
    const auto exclusionPath=std::filesystem::path("Data/SKSE/Plugins/NPCBehaviorBridge.exclude.txt");
    auto exclusions=nbb::readExclusions(exclusionPath);
    timing("configuration_and_report_cleanup");
    auto manifest=manifestFromRuntime();
    timing("loaded_plugin_manifest");
    nbb::RunTimings coreTimings;
    auto capturedPlan=nbb::load_capture::takePlan(manifest,config,exclusions);
    const bool reusedCapturedPlan=capturedPlan.has_value();
    if(reusedCapturedPlan)plan=std::move(*capturedPlan);
    else plan=nbb::run(manifest,config,exclusions,&coreTimings);
    timing(reusedCapturedPlan?"captured_plan_handoff":"disk_fallback_core");
    nbb::load_capture::releasePlan();
    timing("capture_plan_release");
    if(!reusedCapturedPlan) {
        SKSE::log::info("Timing detail (included in disk_fallback_core): plugin_read_wall={:.3f} ms; baseline_selection_plan={:.3f} ms; plugins={}; workers={}",
            coreTimings.pluginReadWallMilliseconds,coreTimings.planMilliseconds,manifest.at("plugins").size(),coreTimings.parseWorkers);
        SKSE::log::info("Timing worker totals (overlapping): plugin_read_parse={:.3f} ms; plugin_sha256={:.3f} ms",coreTimings.readParseMilliseconds,coreTimings.hashMilliseconds);
    }
    stageStarted=Clock::now();
    SKSE::log::info("Baseline selection: {}",plan.at("baseline").dump());
    for(const auto& warning:plan.at("configuration_warnings")) SKSE::log::warn("Ignored protected field: {}",warning.dump());
    for(auto& entry:plan.at("exclusions")) if(entry.at("matched_npcs").empty())
        SKSE::log::warn("NPC exclusion did not match: {}",entry.at("rule").get<std::string>());
    FILETIME creation,exit,kernel,user; require(GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user)!=0,"Process identity unavailable");
    auto born=(std::uint64_t(creation.dwHighDateTime)<<32)|creation.dwLowDateTime;
    plan["runtime_session"]={{"pid",GetCurrentProcessId()},{"process_creation",born},{"runtime",hostRuntime->name},
        {"store",hostRuntime->store==1?"GOG":"Steam"},{"skse_store_type",hostRuntime->store},{"address_library_format",nbb::runtime::addressFormat(*hostRuntime)},
        {"configuration_sha256",nbb::hashFile("Data/SKSE/Plugins/NPCBehaviorBridge.json")}};
    plan["runtime_session"]["exclusions_sha256"]=std::filesystem::exists(exclusionPath)?Json(nbb::hashFile(exclusionPath)):Json(nullptr);
    if(debugOutput)layout={{"schema",1},{"pid",GetCurrentProcessId()},{"process_creation",born},{"runtime",hostRuntime->name},
        {"store",hostRuntime->store==1?"GOG":"Steam"},{"skse_store_type",hostRuntime->store},{"mode",plan.at("mode")},
        {"ready_address",address(&samplingReady)},{"epoch_address",address(&samplingEpoch)},{"references",Json::object()},{"npcs",Json::array()}};
    else layout=Json();
    const bool apply=config.value("mode",std::string("audit"))=="apply";
    std::set<std::string> only;
    for(auto& key:config.at("only_npcs")) only.insert(nbb::parseExclusions(key.get<std::string>()).at(0));
    timing("runtime_session_setup");
    freezeInheritance(only);
    timing("freeze_template_inheritance");
    std::map<std::string,unsigned> visited;
    std::function<void(const std::string&)> process=[&](const std::string& k) {
        if(visited[k]==2)return;
        require(visited[k]!=1,"Cyclic runtime template dependencies: "+k);visited[k]=1;
        auto& row=plan["npcs"][inheritanceRows.at(k)];
        if(projectedInheritance.contains(k)) {
            auto parent=projectedInheritance[k]["template"];
            if(!parent.is_null()&&inheritanceRows.contains(parent.get<std::string>())) process(parent.get<std::string>());
        }
        if(inheritanceScope.contains(k)) {
            planInheritedFields(row,apply);
            try {prepareAndApply(row,apply);} catch(const std::exception& e) {row["runtime_status"]="error";row["runtime_error"]=e.what();SKSE::log::error("{}: {}",k,e.what());}
            // Descendants use only the relationship that was actually committed.
            if(apply&&inheritanceObjects.contains(k)&&(!selectiveNativeWork||nativeFallbackScope.contains(k))) {auto* npc=inheritanceObjects.at(k);auto parent=reference(npc->baseTemplateForm);auto flags=npc->actorData.templateUseFlags.underlying();projectedInheritance[k]["template"]=parent;projectedInheritance[k]["flags"]=flags;inheritanceContext->projected.update(k,flags,parent);}
        } else if(!only.empty()&&!only.contains(k))row["runtime_status"]="outside_requested_scope";
        providerValues.finalize(k);visited[k]=2;
    };
    for(auto& [k,index]:inheritanceRows)try {process(k);} catch(const std::exception& e) {plan["npcs"][index]["inheritance_error"]=e.what();plan["npcs"][index]["runtime_status"]="skipped_inheritance_dependency";}
    timing("resolve_inheritance_and_restore_fields");
    SKSE::log::info("Stat prediction cache: {} reused; {} calculations attempted; {} successful entries",statPredictionCache.hits,statPredictionCache.misses,statPredictionCache.results.size());
    SKSE::log::info("Provider values: {} reused; {} reads; finalized parents only",providerValues.hits,providerValues.reads);
    frozenInheritance=Json();projectedInheritance=Json();inheritanceObjects.clear();inheritanceRows.clear();inheritanceScope.clear();
    fullSnapshotScope.clear();nativeFallbackScope.clear();statPredictionCache.clear();inheritanceContext.reset();providerValues.clear();
    timing("release_inheritance_snapshots");
    Json statuses=Json::object();
    unsigned restoredNpcs=0,restoredFields=0,errorNpcs=0;
    for(auto& row:plan.at("npcs")) {
        const auto result=nbb::diagnostics::npcResult(row);
        restoredFields+=result.restored;restoredNpcs+=result.restored!=0;errorNpcs+=!result.errors.empty();
        if(result.errors.empty()) SKSE::log::info("{}",result.line);
        else {SKSE::log::error("{}",result.line);for(const auto& error:result.errors) SKSE::log::error("NPC {}: {}",row.at("npc").get<std::string>(),error);}
        if(debugOutput) {
            auto status=row.value("runtime_status",row.at("status").get<std::string>());
            if(!statuses.contains(status)) statuses[status]=0;
            statuses[status]=statuses[status].get<unsigned>()+1;
        }
    }
    timing("npc_result_logs");
    if(debugOutput) {
    unsigned externalFields=0;for(const auto& n:layout.at("npcs")) externalFields+=static_cast<unsigned>(n.at("fields").size());
    layout["coverage"]={{"external_sampleable_fields",externalFields},{"scanned_npcs",plan.at("npcs").size()},{"sampleable_npcs",layout.at("npcs").size()},{"statuses",statuses}};
    Json fieldStatuses=Json::object();
    for(auto& row:plan.at("npcs")) for(auto& [field,spec]:row.at("fields").items()) {
        std::string status=spec.at("status");
        if(row.contains("field_results") && row.at("field_results").contains(field)) status=row.at("field_results").at(field).at("status").get<std::string>();
        else if(row.contains("runtime_status")) status=row.at("runtime_status").get<std::string>();
        else if(row.at("status")!="ready") status="skipped_npc";
        if(!fieldStatuses.contains(status)) fieldStatuses[status]=0;
        fieldStatuses[status]=fieldStatuses[status].get<unsigned>()+1;
    }
    layout["coverage"]["field_statuses"]=fieldStatuses;
    plan["runtime_coverage"]=layout.at("coverage");
    }
    timing("coverage_summary");
    debugReport("NPCBehaviorBridge-runtime.json",plan);
    timing("debug_runtime_json");
    try {nbb::report::write(debugOutput,output/"NPCBehaviorBridge-report.html",plan);}
    catch(const std::exception& e) {SKSE::log::error("HTML report failed: {}",e.what());}
    timing("debug_html_report");
    if(debugOutput) {
        layout["selected_sources"]=plan.at("selected_sources");
        layout["source_evidence"]=plan.at("plugins");
    }
    debugReport("NPCBehaviorBridge-sampling.json",layout);
    timing("sampling_report_setup_and_output");
    nbb::scripts::configure(plan,nbb::Sequence(only.begin(),only.end()),output);
    timing("script_target_configuration");
    if(auto* ui=RE::UI::GetSingleton()) ui->AddEventSink(&menuEvents);
    if(debugOutput) SKSE::log::info("{} runtime coverage: {}",apply?"Apply":"Audit",layout.at("coverage").dump());
    SKSE::log::info("{} complete: {} NPCs checked; {} fields restored on {} NPCs; {} base records with errors",apply?"Apply":"Audit",plan.at("npcs").size(),restoredFields,restoredNpcs,errorNpcs);
    timing("event_registration_and_completion_log");
    verify("data_loaded");
    timing("startup_verification");
    SKSE::log::info("Timing: initialization_total = {:.3f} ms (DataLoaded callback; includes timing logs; excludes later Actor script binding)",
        std::chrono::duration<double,std::milli>(Clock::now()-initializationStarted).count());
    SKSE::log::info("Timing: bridge_observed_startup_work = {:.3f} ms (early hook own work + pre-DataLoaded reports + initialization; native callbacks excluded; includes startup diagnostics)",
        nbb::load_capture::measuredEarlyMilliseconds()+std::chrono::duration<double,std::milli>(Clock::now()-initializationStarted).count());
}
void message(SKSE::MessagingInterface::Message* msg) {
    try {
        switch(msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            // Capture failures must never suppress the disk recovery path.
            try {nbb::load_capture::finish();}catch(const std::exception& e){SKSE::log::error("Load capture completion failed: {}",e.what());}
            initialize(); break;
        case SKSE::MessagingInterface::kPreLoadGame: nbb::scripts::beforeLoad(); samplingReady.store(0); samplingEpoch.fetch_add(1); break;
        case SKSE::MessagingInterface::kPostLoadGame:
            if(msg->data) {verify("post_load_game"); samplingEpoch.fetch_add(1); samplingReady.store(1);nbb::scripts::afterLoad("post_load_game");} break;
        case SKSE::MessagingInterface::kNewGame: verify("new_game"); samplingEpoch.fetch_add(1); samplingReady.store(1);nbb::scripts::afterLoad("new_game"); break;
        default: break;
        }
    } catch(const std::exception& e) { samplingReady.store(0); SKSE::log::error("Bridge stopped: {}",e.what()); }
}
}
extern "C" __declspec(dllexport) constinit SKSE::PluginVersionData SKSEPlugin_Version=[] {
    SKSE::PluginVersionData v; v.PluginVersion({0,7,0,0}); v.PluginName("NPCBehaviorBridge"); v.AuthorName("ROSVER");
    // As in WingFlight: generic/version-selected struct support, not the
    // post-629-only flag (older AE SKSE rejects that bit). V5 is set by CLib.
    v.UsesAddressLibrary(); v.UsesNoStructs();
    static_assert(nbb::runtime::supported.size()<std::size(v.compatibleVersions));
    for(std::size_t i=0;i<nbb::runtime::supported.size();++i) v.compatibleVersions[i]=nbb::runtime::pack(nbb::runtime::supported[i]);
    return v;
}();
extern "C" __declspec(dllexport) bool SKSEPlugin_Query(const SKSE::QueryInterface* skse,SKSE::PluginInfo* info) {
    if(!skse || !info) return false;
    info->infoVersion=SKSE::PluginInfo::kVersion; info->name="NPCBehaviorBridge"; info->version=REL::Version(0,7,0,0).pack();
    auto v=skse->RuntimeVersion();
    return !skse->IsEditor() && nbb::runtime::selectSKSE(v[0],v[1],v[2],v[3]);
}
extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* skse) {
    if(!skse || skse->IsEditor()) return false;
    auto v=skse->RuntimeVersion(); hostRuntime=nbb::runtime::selectSKSE(v[0],v[1],v[2],v[3]);
    if(!hostRuntime) return false;
    try {
        SKSE::Init(skse); auto directory=SKSE::log::log_directory(); if(!directory) return false; output=*directory;
        auto sink=std::make_shared<spdlog::sinks::basic_file_sink_mt>((output/"NPCBehaviorBridge.log").string(),true);
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("NPCBehaviorBridge",sink)); spdlog::flush_on(spdlog::level::info);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%l] %v");
        auto fileVersion=REL::Module::get().version();
        require(nbb::runtime::selectFile(fileVersion[0],fileVersion[1],fileVersion[2],fileVersion[3])==hostRuntime,
            "Executable file version and SKSE/store identity disagree");
        SKSE::log::info("NPC Behavior Bridge 0.7; runtime={} store={} SKSE-store-type={} Address-Library-format={}",
            hostRuntime->name,hostRuntime->store==1?"GOG":"Steam",hostRuntime->store,nbb::runtime::addressFormat(*hostRuntime));
        if(!SKSE::GetMessagingInterface()->RegisterListener(message))return false;
        try {nbb::load_capture::install(*hostRuntime);}catch(const std::exception& e){SKSE::log::error("Load capture installation failed: {}; disk recovery retained",e.what());}
        return true;
    } catch(const std::exception& e) {SKSE::log::error("Bridge load failed: {}",e.what());return false;}
      catch(...) {SKSE::log::error("Bridge load failed: unknown exception");return false;}
}
