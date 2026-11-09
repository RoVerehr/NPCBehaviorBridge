#pragma once
#include "nbb/core.hpp"
#include "nbb/runtime_normalization.hpp"
#include <algorithm>
#include <numeric>
#include <functional>
#include <cmath>
#include <stdexcept>
#include <set>
#include <unordered_map>
#include <array>
#include <bit>
#include <optional>
namespace nbb {
// Conservative connected closure of the OLD and PROPOSED runtime relationships.
// Include untouched ancestors/siblings/descendants: a child's empty local plan
// does not prove its final inherited contents will remain unchanged.
inline std::set<std::string> inheritanceCaptureScope(const Json& rows,const Json& oldGraph,const Json& newGraph) {
    std::map<std::string,std::set<std::string>> edges;
    std::set<std::string> scope;
    for(const auto& row:rows) {
        auto key=row.at("npc").get<std::string>();edges[key];
        if(!row.at("fields").empty()||row.contains("inheritance_error"))scope.insert(key);
    }
    for(const auto* graph:{&oldGraph,&newGraph}) for(auto& [key,node]:graph->items()) {
        edges[key];const auto& parent=node.at("template");
        if(parent.is_null())continue;
        auto target=parent.get<std::string>();edges[key].insert(target);edges[target].insert(key);
        if(!graph->contains(target))scope.insert(key); // Unresolved/LVLN branches retain full checks.
    }
    // Preserve full diagnostics for cycles and excessive chains as well.
    for(const auto* graph:{&oldGraph,&newGraph}) for(auto& [start,_]:graph->items()) {
        std::set<std::string> seen;auto key=start;
        for(unsigned depth=0;graph->contains(key);++depth) {
            if(!seen.insert(key).second){scope.insert(start);break;}
            const auto& parent=graph->at(key).at("template");if(parent.is_null())break;
            key=parent.get<std::string>();
        }
    }
    std::vector<std::string> pending(scope.begin(),scope.end());
    while(!pending.empty()) {
        auto key=std::move(pending.back());pending.pop_back();
        for(const auto& neighbor:edges[key])if(scope.insert(neighbor).second)pending.push_back(neighbor);
    }
    return scope;
}
struct RemainingInheritanceWork {
    std::set<std::string> affected,capture;
};
// Changes flow to descendants. Ancestors needed only as frozen providers do not
// make their other children dirty: expanding them bidirectionally repeats work.
inline RemainingInheritanceWork remainingInheritanceWork(const std::set<std::string>& pending,const Json& before,const Json& after) {
    std::map<std::string,std::set<std::string>> children,parents;
    for(const auto* graph:{&before,&after})for(auto& [key,node]:graph->items()) {
        const auto& parent=node.at("template");if(parent.is_null())continue;
        auto p=parent.get<std::string>();children[p].insert(key);parents[key].insert(p);
    }
    RemainingInheritanceWork work;work.affected=pending;
    std::vector<std::string> queue(pending.begin(),pending.end());
    for(std::size_t i=0;i<queue.size();++i)for(const auto& child:children[queue[i]])if(work.affected.insert(child).second)queue.push_back(child);
    work.capture=work.affected;queue.assign(work.capture.begin(),work.capture.end());
    for(std::size_t i=0;i<queue.size();++i)for(const auto& parent:parents[queue[i]])if(work.capture.insert(parent).second)queue.push_back(parent);
    return work;
}
inline unsigned inheritanceBit(const std::string& f) {
    static const std::map<std::string,unsigned> bits{
        {"DeathItem",0}, // Functional member of Traits; appearance remains excluded.
        {"Class",1},{"Level",1},{"PCLevelMult",1},{"CalcMinLevel",1},{"CalcMaxLevel",1},{"AutoCalcStats",1},
        {"SpeedMultiplier",1},{"MagickaOffset",1},{"StaminaOffset",1},{"HealthOffset",1},{"SkillValues",1},{"SkillOffsets",1},
        {"Health",1},{"Magicka",1},{"Stamina",1},{"BleedoutThreshold",1},{"BleedoutOverrideEnabled",1},
        {"Factions",2},{"CrimeFaction",2},{"SPLO",3},{"Perks",3},
        {"Aggression",4},{"Confidence",4},{"Assistance",4},{"Energy",4},{"Morality",4},{"Mood",4},
        {"AggroRadiusBehavior",4},{"AggroWarn",4},{"AggroWarnAttack",4},{"AggroAttack",4},{"CombatStyle",4},{"GiftFilter",4},
        {"PKID",5},{"Essential",7},{"Respawn",7},{"DoesntAffectStealthMeter",7},{"Protected",7},{"NoRumors",7},{"SimpleActor",7},
        {"InheritedName",7},{"InheritedShortName",7},{"Inventory",8},{"GearedUpWeapons",8},{"VMAD",9},
        {"DPLT",10},{"SPOR",10},{"ECOR",10},{"OCOR",10},{"GWOR",10},{"AttackRace",11},{"AttackData",11},{"Keywords",12}};
    auto it=bits.find(f);return it==bits.end()?0:1u<<it->second;
}
inline bool calculatedField(const std::string& f) {return f=="SkillValues"||f=="Health"||f=="Magicka"||f=="Stamina";}
inline const Sequence statInputs{"Class","Level","PCLevelMult","CalcMinLevel","CalcMaxLevel","AutoCalcStats","HealthOffset","MagickaOffset","StaminaOffset"};
// Resolve chains without calling engine initialization or choosing random LVLN entries.
inline std::string inheritedProvider(const std::string& start,unsigned bit,const Json& graph) {
    std::set<std::string> seen;auto key=start;
    for(;;) {
        if(!graph.contains(key)) throw std::runtime_error("Unresolved or leveled NPC template: "+key);
        if(!seen.insert(key).second) throw std::runtime_error("Cyclic template chain");
        const auto& n=graph.at(key);
        if(!(n.at("flags").get<unsigned>()&bit)||n.at("template").is_null())return key;
        key=n.at("template").get<std::string>();
    }
}
// Numeric edges avoid repeated FormKey/JSON lookups while walking a template chain.
// Only relationships are indexed; runtime field values still use the original snapshots.
class InheritanceIndex {
    static constexpr std::size_t absent=std::size_t(-1);
    struct Node {std::string key,parentKey;unsigned flags{};std::size_t parent=absent;bool hasParent{};};
    std::vector<Node> nodes;
    std::unordered_map<std::string,std::size_t> indices;
public:
    void build(const Json& graph) {
        nodes.clear();indices.clear();nodes.reserve(graph.size());indices.reserve(graph.size());
        for(auto& [key,_]:graph.items()){indices.emplace(key,nodes.size());nodes.push_back(Node{key});}
        for(auto& [key,node]:graph.items())update(key,node.at("flags").get<unsigned>(),node.at("template"));
    }
    void update(const std::string& key,unsigned flags,const Json& parent) {
        auto found=indices.find(key);if(found==indices.end())return;
        auto& node=nodes[found->second];node.flags=flags;node.hasParent=!parent.is_null();
        node.parentKey=node.hasParent?parent.get<std::string>():std::string();
        auto target=indices.find(node.parentKey);node.parent=target==indices.end()?absent:target->second;
    }
    std::string provider(const std::string& start,unsigned bit) const {
        auto found=indices.find(start);auto index=found==indices.end()?absent:found->second;
        const std::string* key=&start;std::set<std::size_t> seen;
        for(;;) {
            if(index==absent)throw std::runtime_error("Unresolved or leveled NPC template: "+*key);
            if(!seen.insert(index).second)throw std::runtime_error("Cyclic template chain");
            const auto& node=nodes[index];
            if(!(node.flags&bit)||!node.hasParent)return node.key;
            index=node.parent;key=&node.parentKey;
        }
        }
    void clear(){nodes.clear();indices.clear();}
};
struct PreparedInheritanceFields {
    struct Entry {std::string name;unsigned bit;bool inheritedName;};
    std::set<std::string> enabled;
    std::vector<Entry> entries;
    explicit PreparedInheritanceFields(const Json& fields) {
        for(const auto& field:fields)enabled.insert(field.get<std::string>());
        auto names=enabled;names.insert("InheritedName");names.insert("InheritedShortName");
        for(const auto& name:names)if(auto bit=inheritanceBit(name);bit&&name!="VMAD"&&!calculatedField(name))
            entries.push_back({name,bit,name=="InheritedName"||name=="InheritedShortName"});
    }
};
struct InheritanceContext {
    InheritanceIndex frozen,projected;
    PreparedInheritanceFields fields;
    InheritanceContext(const Json& before,const Json& after,const Json& enabled):fields(enabled){frozen.build(before);projected.build(after);}
};
inline Json distributeStats(Json values,const Json& weights,int points,int cap) {
    auto w=weights.get<std::vector<int>>();std::vector<unsigned> order(w.size());std::iota(order.begin(),order.end(),0);
    std::sort(order.begin(),order.end(),[&](auto a,auto b){return w[a]==w[b]?a>b:w[a]>w[b];});
    int total=std::accumulate(w.begin(),w.end(),0);if(!total||points<=0)return values;
    int q=points/total,rem=points%total,excess=0,capped=0;
    for(auto i:order){int v=values[i].get<int>()+q*w[i];excess+=std::max(0,v-cap);capped+=v>=cap;values[i]=std::min(v,cap);}
    while(excess&&capped<int(w.size())) {int amount=excess/(int(w.size())-capped);if(!amount)break;bool progress=false;
        for(auto i:order)if(w[i]>0&&values[i].get<int>()<cap){int gain=std::min(amount,cap-values[i].get<int>());values[i]=values[i].get<int>()+gain;excess-=gain;capped+=values[i].get<int>()>=cap;progress=true;}if(!progress)break;}
    rem+=excess;while(rem){bool progress=false;for(auto i:order){if(!rem)break;if(w[i]>0){--w[i];progress=true;if(values[i].get<int>()<cap){values[i]=values[i].get<int>()+1;--rem;}}}if(!progress)break;}return values;
}
inline Json calculateStats(const Json& input) {
    int level=input.at("level"), skillPoints=input.at("skill_points"),attributePoints=input.at("attribute_points");
    if(level<1||level>65535||skillPoints<0||skillPoints>10000||attributePoints<0||attributePoints>10000)throw std::runtime_error("Unsupported automatic-stat inputs");
    if(input.at("skills").size()!=18||input.at("skill_weights").size()!=18||input.at("attributes").size()!=3||input.at("attribute_weights").size()!=3||(!std::isfinite(input.at("health_bonus").get<double>())||input.at("health_bonus").get<double>()<0||input.at("health_bonus").get<double>()>10000))throw std::runtime_error("Malformed automatic-stat input");
    // Race base (0..65535) plus the signed ACBS offset (-32768..32767).
    // Negative intermediate values can become valid after level growth.
    for(auto v:input.at("attributes"))if(!v.is_number_integer()||v.get<std::int64_t>() < -32768||v.get<std::int64_t>() > 98302)
        throw std::runtime_error("Automatic attribute input outside representable race/offset range");
    auto skills=distributeStats(input.at("skills"),input.at("skill_weights"),(level-1)*skillPoints,100);
    auto hms=distributeStats(input.at("attributes"),input.at("attribute_weights"),(level-1)*attributePoints,65535);
    hms[0]=hms[0].get<int>()+int((level-1)*input.at("health_bonus").get<double>());
    for(auto v:hms)if(v.get<int>()<0||v.get<int>()>65535)
        throw std::runtime_error("Final automatic attribute outside writable range");
    return {{"SkillValues",skills},{"Health",hms[0]},{"Magicka",hms[1]},{"Stamina",hms[2]}};
}
// Per-initialization cache of successful calculations only. Keys contain every
// numeric input actually consumed by calculateStats, not NPC/race/class names.
struct StatPredictionCache {
    std::unordered_map<std::string,Json> results;
    std::size_t hits{},misses{};
    Json calculate(const Json& input) {
        auto key=input.dump();auto found=results.find(key);
        if(found!=results.end()){++hits;return found->second;}
        ++misses;auto result=calculateStats(input);results.emplace(std::move(key),result);return result;
    }
    void clear(){results.clear();hits=misses=0;}
};
// Only providers whose parent-first processing has finished are reusable. Never
// cache failed reads or pre-commit values; finalizing again invalidates old data.
struct FinalizedProviderValues {
    std::set<std::string> finalized;
    std::map<std::string,std::map<std::string,Json>> values;
    std::size_t hits{},reads{};
    void finalize(const std::string& key){values.erase(key);finalized.insert(key);}
    template<class Read> Json get(const std::string& key,const std::string& field,Read read) {
        if(!finalized.contains(key)){++reads;return read();}
        auto& fields=values[key];auto found=fields.find(field);
        if(found!=fields.end()){++hits;return found->second;}
        ++reads;auto value=read();return fields.emplace(field,std::move(value)).first->second;
    }
    void clear(){finalized.clear();values.clear();hits=reads=0;}
};
inline bool permitsValue(const Json& actual,const Json& spec) {
    return spec.value("status",std::string())=="ready" && (actual==spec.at("disk")||actual==spec.at("expected")||
        (spec.contains("inherited_old_value")&&actual==spec.at("inherited_old_value")));
}
inline void addInheritedField(Json& row,const std::string& field,const Json& expected,const Json& oldValue,const std::string& provider,const FieldEquality& same=identicalField) {
    if(row.contains("input_errors")&&row.at("input_errors").contains(field))return;
    const auto& disk=row["inheritance_record"]["fields"];
    auto spec=row["fields"].contains(field)?row["fields"][field]:Json{{"baseline",oldValue},{"official_baseline",oldValue},{"sources",Json::array()}};
    if(!spec.contains("local_selection")) spec["local_selection"]={{"expected",spec.value("expected",disk.value(field,oldValue))},{"status",spec.value("status",std::string("no_local_candidate"))}};
    spec["disk"]=disk.value(field,oldValue);spec["expected"]=expected;spec["differs"]=!same(field,spec["disk"],expected);spec["status"]="ready";spec["reason"]="";
    spec["inherited_old_value"]=oldValue;spec["inheritance_provider"]=provider;
    row["fields"][field]=std::move(spec);row["status"]="ready";row["reason"]="";
}
inline bool needsNativeFallback(const Json& row,bool automatic,const InheritanceIndex& oldGraph,const InheritanceIndex& newGraph,
    const std::function<Json(const std::string&,const std::string&)>& read,
    const std::function<bool(const std::string&)>& canCopyAttack,const FieldEquality& same=identicalField) {
    if(row.at("status")!="ready")return false;auto key=row.at("npc").get<std::string>();
    std::array<std::optional<std::string>,13> oldSources,newSources;
    auto provider=[&](unsigned bit,bool projected)->const std::string& {auto& cached=(projected?newSources:oldSources).at(std::countr_zero(bit));if(!cached)cached=(projected?newGraph:oldGraph).provider(key,bit);return *cached;};
    for(auto& [f,spec]:row.at("fields").items()) {
        if(spec.at("status")!="ready"||f=="VMAD"||f=="Destructible"||(automatic&&calculatedField(f)))continue;
        auto actual=read(key,f);Json effective=spec.at("expected");
        if(auto bit=inheritanceBit(f)) {
            const auto& source=provider(bit,false);const auto& next=provider(bit,true);
            if(source!=next)return true;
            if(source!=key) {
                if((f=="AttackRace"||f=="AttackData")&&!canCopyAttack(source))continue;
                effective=read(source,f);
            }
        }
        if(!same(f,actual,effective))return true;
    }
    return false;
}
// For branches proven to have no pending local or ancestor writes. Preserve
// inherited candidate metadata without recalculating already-final engine stats.
inline void settleInheritedFields(Json& row,bool automatic,const InheritanceIndex& graph,
    const std::function<Json(const std::string&,const std::string&)>& read,
    const std::function<bool(const std::string&)>& canCopyAttack,const FieldEquality& same=identicalField) {
    if(row.at("status")!="ready")return;auto key=row.at("npc").get<std::string>();
    std::array<std::optional<std::string>,13> sources;
    std::vector<std::string> fields;for(auto& [f,_]:row["fields"].items())fields.push_back(f);
    for(const auto& f:fields){auto& spec=row["fields"][f];if(spec.at("status")!="ready"||f=="VMAD"||f=="Destructible")continue;
        if(automatic&&calculatedField(f)){spec["status"]="skipped";spec["reason"]="Automatic stats; plugin cache not restored";continue;}
        auto bit=inheritanceBit(f);if(!bit)continue;
        auto& cached=sources.at(std::countr_zero(bit));if(!cached)cached=graph.provider(key,bit);const auto& source=*cached;if(source==key)continue;
        if((f=="AttackRace"||f=="AttackData")&&!canCopyAttack(source)) {
            row["inheritance_notes"][f]="Race-default attack map retained for child race";spec["status"]="skipped";spec["reason"]="Inherited attack map uses race default; child race retained";continue;
        }
        auto actual=read(key,f),parent=read(source,f);
        if(!same(f,actual,parent))throw std::runtime_error("Settled inheritance proof changed");
        addInheritedField(row,f,parent,parent,source,same);
    }
}
inline void planInheritedFields(Json& row,const Json& enabled,const Json& frozenInheritance,const Json& projectedInheritance,
    const std::function<Json(const std::string&,const std::string&)>& read,
    const std::function<Json(const Json&)>& calculate,const std::function<bool(const std::string&)>& canCopyAttack,
    const InheritanceContext* context=nullptr,const FieldEquality& same=identicalField) {
    auto k=row["npc"].get<std::string>();if(!frozenInheritance.contains(k))return;
    const auto& old=frozenInheritance.at(k);const auto& now=projectedInheritance.at(k);
    // Scoped to this NPC: the parent-first caller may commit graph changes between NPCs.
    std::array<std::optional<std::string>,13> oldProviders,newProviders;
    const auto provider=[&](unsigned bit,bool projected)->const std::string& {
        auto& cached=(projected?newProviders:oldProviders).at(std::countr_zero(bit));
        if(!cached)cached=context?(projected?context->projected:context->frozen).provider(k,bit):inheritedProvider(k,bit,projected?projectedInheritance:frozenInheritance);
        return *cached;
    };
    std::optional<PreparedInheritanceFields> localFields;if(!context)localFields.emplace(enabled);
    const auto& prepared=context?context->fields:*localFields;
    const auto& fields=prepared.enabled;
    // Names are only a consequence of a permitted template change, never config fields.
    const bool includeNames=now["template"]!=old["template"] || (now["flags"].get<unsigned>()&128);
    auto& selected=row["fields"];const auto& oldValues=old.at("values");
    const auto& diskValues=row.at("inheritance_record").at("fields");
    for(const auto& entry:prepared.entries) {
        if(entry.inheritedName&&!includeNames&&!fields.contains(entry.name))continue;
        const auto& f=entry.name;auto bit=entry.bit;
        try {
            const auto& oldSource=provider(bit,false);
            const auto& source=provider(bit,true);
            const auto& actual=oldValues.at(f);
            const auto& original=oldSource!=k?frozenInheritance.at(oldSource).at("values").at(f):actual;
            bool oldCopied=oldSource!=k&&same(f,actual,original);
            const auto local=selected.find(f);bool hasLocal=local!=selected.end()&&local->at("status")=="ready";
            if(source==k) {
                if(oldSource==k)continue;
                if(!hasLocal && !diskValues.contains(f)){row["inheritance_notes"][f]="Local value unavailable after detaching template";continue;}
                const auto& value=hasLocal?local->at("expected"):diskValues.at(f);
                if(oldCopied)addInheritedField(row,f,value,original,k,same);
                continue;
            }
            auto value=read(source,f);
            bool changedSource=source!=oldSource||!same(f,value,original);
            if(!hasLocal&&!changedSource)continue;
            if(f=="AttackRace"||f=="AttackData") {
                if(!canCopyAttack(source)) {row["inheritance_notes"][f]="Race-default attack map retained for child race";if(hasLocal){row["fields"][f]["status"]="skipped";row["fields"][f]["reason"]="Inherited attack map uses race default; child race retained";}continue;}
            }
            // A parent's actual verified contents supersede the child's local copy.
            // Never accept an unexplained child runtime modification as a baseline.
            const Json nullValue;auto diskIt=diskValues.find(f);const auto& disk=diskIt==diskValues.end()?nullValue:*diskIt;
            if(oldCopied || same(f,actual,disk) || same(f,actual,value)) addInheritedField(row,f,value,oldCopied?original:disk,source,same);
            else {row["inheritance_notes"][f]="Runtime value differs from old inherited contents; preserved";
                if(hasLocal)row["fields"][f]["status"]="skipped",row["fields"][f]["reason"]="Unexplained runtime change during inheritance synchronization";}
        } catch(const std::exception& ex) {
            row["inheritance_notes"][f]=ex.what();
            if(row["fields"].contains(f)) {row["fields"][f]["status"]="skipped";row["fields"][f]["reason"]="Unresolved inheritance: "+std::string(ex.what());}
        }
    }
    // Recalculate only when accepted stat inputs change, using the child's race.
    // The stat calculator only consumes these inputs, not inventories, spells or packages.
    Json values=Json::object();for(const auto& f:statInputs)if(old.at("values").contains(f))values[f]=old.at("values").at(f);
    bool changed=false;
    for(auto& f:nbb::statInputs) if(row["fields"].contains(f)&&values.contains(f)&&permitsValue(values[f],row["fields"][f])) {changed|=values[f]!=row["fields"][f]["expected"];values[f]=row["fields"][f]["expected"];}
    if(values.value("AutoCalcStats",false)) {
        try {
            // Race, class records and GMSTs are not modified by this synchronous pass.
            // Reuse a successful frozen prediction only while all calculation inputs agree.
            auto calculated=!changed&&old.contains("calculated")?old.at("calculated"):calculate(values);
            if(changed) for(auto f:{"SkillValues","Health","Magicka","Stamina"}) if(fields.contains(f)) {
                bool wasAuto=old.at("values").value("AutoCalcStats",false);
                auto proof=wasAuto?(old.contains("calculated")?old["calculated"].value(f,Json()):Json()):row["inheritance_record"]["fields"].value(f,Json());
                if(!old.at("values").contains(f)||old.at("values").at(f)!=proof)
                    throw std::runtime_error("Automatic stats have unrelated runtime changes; stat inputs retained");
            }
            for(auto f:{"SkillValues","Health","Magicka","Stamina"}) if(fields.contains(f)) {
                // Never write cached plugin values over auto-calculated values.
                if(row["fields"].contains(f)){row["fields"][f]["status"]="skipped";row["fields"][f]["reason"]="Automatic stats; plugin cache not restored";}
                if(changed)
                    addInheritedField(row,f,calculated.at(f),old["values"].at(f),k);
                else if(changed)row["inheritance_notes"][f]="Unexplained automatic-stat value; retained";
            }
        }catch(const std::exception& ex){row["inheritance_notes"]["automatic_stats"]=ex.what();
            // A derived-value calculation failure does not veto valid source inputs.
            for(const auto* f:{"SkillValues","Health","Magicka","Stamina"})if(fields.contains(f)&&
                (changed||row["fields"].contains(f))&&!(row.contains("input_errors")&&row["input_errors"].contains(f))) {
                if(!row["fields"].contains(f))row["fields"][f]={{"baseline",nullptr},{"official_baseline",nullptr},{"disk",nullptr},
                    {"expected",nullptr},{"sources",Json::array()},{"differs",false}};
                row["fields"][f]["status"]="calculation_error";row["fields"][f]["reason"]=std::string(ex.what());
            }
        }
    } else {
        // Leaving automatic stats restores local or inherited caches, but only
        // when the previous calculated value can be reproduced independently.
        for(auto f:{"SkillValues","Health","Magicka","Stamina"})if(fields.contains(f))try {
            const auto& source=provider(2,true);const auto& previous=provider(2,false);
            auto before=frozenInheritance.at(previous).at("values").at(f);
            bool leavingAuto=old.at("values").value("AutoCalcStats",false);
            if(leavingAuto) {if(!old.contains("calculated"))throw std::runtime_error("Old automatic value unavailable");before=old.at("calculated").at(f);}
            bool hasLocal=row["fields"].contains(f)&&row["fields"][f]["status"]=="ready";
            auto value=source!=k?read(source,f):hasLocal?row["fields"][f]["expected"]:row["inheritance_record"]["fields"].at(f);
            if(old["values"].at(f)==before && (source!=previous||leavingAuto||source!=k&&(value!=before||hasLocal)))addInheritedField(row,f,value,before,source);
            else if(old["values"].at(f)!=before && (source!=k||leavingAuto)) {
                row["inheritance_notes"][f]="Unexplained inherited/calculated stat cache";
                if(hasLocal){row["fields"][f]["status"]="skipped";row["fields"][f]["reason"]="Unexplained inherited/calculated stat cache";}
                if(leavingAuto)for(auto& input:statInputs)if(row["fields"].contains(input)){row["fields"][input]["status"]="skipped";row["fields"][input]["reason"]="Old automatic values cannot be verified";}
            }
        }catch(const std::exception& ex){row["inheritance_notes"][f]=ex.what();if(row["fields"].contains(f)){row["fields"][f]["status"]="skipped";row["fields"][f]["reason"]=ex.what();}}

    }
}

}
