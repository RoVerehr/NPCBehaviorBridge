#pragma once
#include "nbb/core.hpp"
#include <algorithm>
#include <numeric>
#include <functional>
#include <cmath>
#include <stdexcept>
namespace nbb {
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
    for(unsigned depth=0;depth<128;++depth) {
        if(!graph.contains(key)) throw std::runtime_error("Unresolved or leveled NPC template: "+key);
        if(!seen.insert(key).second) throw std::runtime_error("Cyclic template chain");
        const auto& n=graph.at(key);
        if(!(n.at("flags").get<unsigned>()&bit)||n.at("template").is_null())return key;
        key=n.at("template").get<std::string>();
    }
    throw std::runtime_error("Excessive template chain");
}
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
inline bool permitsValue(const Json& actual,const Json& spec) {
    return spec.value("status",std::string())=="ready" && (actual==spec.at("disk")||actual==spec.at("expected")||
        (spec.contains("inherited_old_value")&&actual==spec.at("inherited_old_value")));
}
inline void addInheritedField(Json& row,const std::string& field,const Json& expected,const Json& oldValue,const std::string& provider) {
    const auto& disk=row["inheritance_record"]["fields"];
    auto spec=row["fields"].contains(field)?row["fields"][field]:Json{{"baseline",oldValue},{"official_baseline",oldValue},{"sources",Json::array()}};
    if(!spec.contains("local_selection")) spec["local_selection"]={{"expected",spec.value("expected",disk.value(field,oldValue))},{"status",spec.value("status",std::string("no_local_candidate"))}};
    spec["disk"]=disk.value(field,oldValue);spec["expected"]=expected;spec["differs"]=spec["disk"]!=expected;spec["status"]="ready";spec["reason"]="";
    spec["inherited_old_value"]=oldValue;spec["inheritance_provider"]=provider;
    row["fields"][field]=std::move(spec);row["status"]="ready";row["reason"]="";
}
inline void planInheritedFields(Json& row,const Json& enabled,const Json& frozenInheritance,const Json& projectedInheritance,
    const std::function<Json(const std::string&,const std::string&)>& read,
    const std::function<Json(const Json&)>& calculate,const std::function<bool(const std::string&)>& canCopyAttack) {
    auto k=row["npc"].get<std::string>();if(!frozenInheritance.contains(k))return;
    const auto& old=frozenInheritance.at(k);const auto& now=projectedInheritance.at(k);
    std::set<std::string> fields;for(auto& f:enabled)fields.insert(f);
    // Names are only a consequence of a permitted template change, never config fields.
    if(now["template"]!=old["template"] || (now["flags"].get<unsigned>()&128)) {fields.insert("InheritedName");fields.insert("InheritedShortName");}
    for(auto& f:fields) {
        auto bit=nbb::inheritanceBit(f);if(!bit||f=="VMAD"||nbb::calculatedField(f))continue;
        try {
            auto oldSource=nbb::inheritedProvider(k,bit,frozenInheritance);
            auto source=nbb::inheritedProvider(k,bit,projectedInheritance);
            auto actual=old.at("values").at(f);auto original=old.at("values").at(f);
            if(oldSource!=k) original=frozenInheritance.at(oldSource).at("values").at(f);
            bool oldCopied=oldSource!=k&&actual==original;
            bool hasLocal=row["fields"].contains(f)&&row["fields"][f]["status"]=="ready";
            if(source==k) {
                if(oldSource==k)continue;
                if(!hasLocal && !row["inheritance_record"]["fields"].contains(f)){row["inheritance_notes"][f]="Local value unavailable after detaching template";continue;}
                auto value=hasLocal?row["fields"][f]["expected"]:row["inheritance_record"]["fields"][f];
                if(oldCopied)addInheritedField(row,f,value,original,k);
                continue;
            }
            auto value=read(source,f);
            bool changedSource=source!=oldSource||value!=original;
            if(!hasLocal&&!changedSource)continue;
            if(f=="AttackRace"||f=="AttackData") {
                if(!canCopyAttack(source)) {row["inheritance_notes"][f]="Race-default attack map retained for child race";if(hasLocal){row["fields"][f]["status"]="skipped";row["fields"][f]["reason"]="Inherited attack map uses race default; child race retained";}continue;}
            }
            // A parent's actual verified contents supersede the child's local copy.
            // Never accept an unexplained child runtime modification as a baseline.
            auto disk=row["inheritance_record"]["fields"].value(f,Json());
            if(oldCopied || actual==disk || actual==value) addInheritedField(row,f,value,oldCopied?original:disk,source);
            else {row["inheritance_notes"][f]="Runtime value differs from old inherited contents; preserved";
                if(hasLocal)row["fields"][f]["status"]="skipped",row["fields"][f]["reason"]="Unexplained runtime change during inheritance synchronization";}
        } catch(const std::exception& ex) {
            row["inheritance_notes"][f]=ex.what();
            if(row["fields"].contains(f)) {row["fields"][f]["status"]="skipped";row["fields"][f]["reason"]="Unresolved inheritance: "+std::string(ex.what());}
        }
    }
    // Recalculate only when accepted stat inputs change, using the child's race.
    auto values=old.at("values");bool changed=false;
    for(auto& f:nbb::statInputs) if(row["fields"].contains(f)&&values.contains(f)&&permitsValue(values[f],row["fields"][f])) {changed|=values[f]!=row["fields"][f]["expected"];values[f]=row["fields"][f]["expected"];}
    if(values.value("AutoCalcStats",false)) {
        try {
            auto calculated=calculate(values);
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
            // Do not alter inputs if we cannot compute the dependent automatic values.
            for(auto& [f,spec]:row["fields"].items())if(std::find(nbb::statInputs.begin(),nbb::statInputs.end(),f)!=nbb::statInputs.end()||nbb::calculatedField(f)){spec["status"]="skipped";spec["reason"]="Automatic calculation unavailable or unrelated runtime change";}}
    } else {
        // Leaving automatic stats restores local or inherited caches, but only
        // when the previous calculated value can be reproduced independently.
        for(auto f:{"SkillValues","Health","Magicka","Stamina"})if(fields.contains(f))try {
            auto source=nbb::inheritedProvider(k,2,projectedInheritance);auto previous=nbb::inheritedProvider(k,2,frozenInheritance);
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
