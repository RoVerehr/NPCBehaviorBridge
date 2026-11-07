#pragma once
#include "nbb/core.hpp"
#include <string_view>
#include <unordered_map>

namespace nbb {
// Scalar definitions describe disk serialization and the supported TESNPC layout.
// Shared words are written with masks; excluded flags are never cleared wholesale.
struct ScalarField {
    const char* name; const char* tag; unsigned diskOffset, diskBytes;
    unsigned runtimeOffset, runtimeBytes;
    std::uint32_t diskMask=0, runtimeMask=0; unsigned shift=0;
    bool signedValue=false, boolean=false;
};
inline const std::vector<ScalarField> scalarFields=[] {
    std::vector<ScalarField> v;
    auto flag=[&](const char* name,unsigned bit) {v.push_back({name,"ACBS",0,4,0x38,4,1u<<bit,1u<<bit,bit,false,true});};
    flag("Essential",1);flag("Respawn",3);flag("AutoCalcStats",4);flag("Unique",5);
    flag("DoesntAffectStealthMeter",6);flag("PCLevelMult",7);flag("UsesTemplate",8);flag("CalcForAllTemplates",9);
    flag("NoRumors",13);flag("Summonable",14);flag("DoesntBleed",16);flag("BleedoutOverrideEnabled",18);
    flag("SimpleActor",20);flag("LoopedScript",21);flag("NoActivation",23);flag("Ghost",29);flag("Invulnerable",31);
    auto templ=[&](const char* name,unsigned bit) {v.push_back({name,"ACBS",18,2,0x4a,2,1u<<bit,1u<<bit,bit,false,true});};
    templ("UseStats",1);templ("UseFactions",2);templ("UseSpellList",3);templ("UseAIData",4);
    templ("UseInventory",8);templ("UseScript",9);templ("UseDefPackList",10);templ("UseAttackData",11);templ("UseKeywords",12);
    v.push_back({"StaminaOffset","ACBS",6,2,0x3e,2,0,0,0,true});
    v.push_back({"HealthOffset","ACBS",20,2,0x4c,2,0,0,0,true});
    v.push_back({"Level","ACBS",8,2,0x40,2});v.push_back({"CalcMinLevel","ACBS",10,2,0x42,2});
    v.push_back({"CalcMaxLevel","ACBS",12,2,0x44,2});v.push_back({"SpeedMultiplier","ACBS",14,2,0x46,2});
    v.push_back({"BaseDisposition","ACBS",16,2,0x48,2,0,0,0,true});v.push_back({"BleedoutThreshold","ACBS",22,2,0x4e,2});
    v.push_back({"Health","DNAM",36,2,0x1b4,2});v.push_back({"GearedUpWeapons","DNAM",48,1,0x241,1});
    v.push_back({"Energy","AIDT",2,1,0xb8,4,0,0x1fe0,5});
    v.push_back({"Morality","AIDT",3,1,0xb8,4,0,0x6000,13});
    v.push_back({"Mood","AIDT",4,1,0xb8,4,0,0x38000,15});
    v.push_back({"AggroRadiusBehavior","AIDT",6,1,0xb8,4,0,0x100000,20,false,true});
    v.push_back({"AggroWarn","AIDT",8,4,0xbc,2});v.push_back({"AggroWarnAttack","AIDT",12,4,0xbe,2});
    v.push_back({"AggroAttack","AIDT",16,4,0xc0,2});
    v.push_back({"SoundLevel","NAM8",0,4,0x245,1});
    return v;
}();
inline const ScalarField* scalarField(std::string_view name) {
    static const auto index=[] {std::unordered_map<std::string_view,const ScalarField*> result;for(const auto& f:scalarFields)result.emplace(f.name,&f);return result;}();
    auto found=index.find(name);return found==index.end()?nullptr:found->second;
}
inline Json decodeScalarWord(const ScalarField& field,std::uint32_t raw) {
    if(field.runtimeMask) raw=(raw&field.runtimeMask)>>field.shift;
    if(field.boolean) return bool(raw);
    if(field.signedValue) return static_cast<std::int16_t>(raw);
    return raw;
}
inline std::uint32_t encodeScalarWord(const ScalarField& field,std::uint32_t original,const Json& value) {
    std::uint32_t raw=field.boolean?(value.get<bool>()?1u:0u):field.signedValue?std::uint16_t(value.get<std::int16_t>()):value.get<std::uint32_t>();
    return field.runtimeMask?(original&~field.runtimeMask)|((raw<<field.shift)&field.runtimeMask):raw;
}
struct ReferenceField { const char* name; const char* tag; unsigned runtimeOffset; Sequence types; };
inline const std::vector<ReferenceField> referenceFields{
    {"Template","TPLT",0x60,{"NPC_","LVLN"}}, {"CrimeFaction","CRIF",0x230,{"FACT"}},
    {"DeathItem","INAM",0x50,{"LVLI"}}, {"DPLT","DPLT",0x228,{"FLST"}},
    {"Class","CNAM",0x1c0,{"CLAS"}}, {"GiftFilter","GNAM",0x1d0,{"FLST"}}
};
inline const ReferenceField* referenceField(std::string_view name) {
    for(const auto& f:referenceFields) if(name==f.name) return &f;
    return nullptr;
}
inline bool isTemplateField(std::string_view name) {
    const auto* f=scalarField(name);return name=="UseAIPackages" || (f && f->diskOffset==18 && std::string_view(f->tag)=="ACBS");
}
inline bool protectedField(std::string_view name) {
    static const Sequence names{"RNAM","Race","Female","CharGenFacePreset","IsCharGenFacePreset","UseTraits","UseBaseData","UseModelAnimation",
        "VTCK","Voice","VoiceType","CSDT","CSDI","CSDC","CSCR","CreatureSounds","LoopedAudio","OppositeGenderAnims",
        "FULL","SHRT","Name","ShortName","DOFT","SOFT","DefaultOutfit","SleepOutfit","NAM9","NAMA","PNAM","HCLF","Tint","TintLayers",
        "TINI","TINC","TINV","TIAS","FTST","QNAM","WNAM","ANAM","NAM6","NAM7","Height","Weight","Skin","Face","HeadParts","HairColor",
        "FarAwayModelDistance","EDID","EditorID","OBND"};
    for(const auto& n:names) if(name==n) return true;
    return false;
}
// Template-derived names may change; Traits still protects appearance.
inline bool templateSharedWithProtectedData(std::uint16_t flags) { return (flags&0x01)!=0; }
}
