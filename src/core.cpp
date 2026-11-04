#include "nbb/core.hpp"
#include "nbb/field_catalog.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <cmath>

namespace nbb {
const Sequence fieldNames=[] {auto names=legacyFieldNames;for(const auto& f:scalarFields) names.push_back(f.name);for(const auto& f:referenceFields) names.push_back(f.name);
    for(auto name:{"SkillOffsets","Perks","Keywords","AttackRace","AttackData","Destructible"}) names.push_back(name);return names;}();
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t maxRecord = 32 * 1024 * 1024;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t p) {
    require(p <= b.size() && b.size() - p >= 4, "Truncated uint32");
    return b[p] | (std::uint32_t(b[p+1])<<8) | (std::uint32_t(b[p+2])<<16) | (std::uint32_t(b[p+3])<<24);
}
std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t p) {
    require(p <= b.size() && b.size() - p >= 2, "Truncated uint16");
    return std::uint16_t(b[p] | (std::uint16_t(b[p+1])<<8));
}
std::string tag(std::span<const std::uint8_t> b, std::size_t p = 0) {
    require(p <= b.size() && b.size()-p >= 4, "Truncated tag");
    return std::string(reinterpret_cast<const char*>(b.data()+p),4);
}
std::string cstring(std::span<const std::uint8_t> b) {
    require(!b.empty() && b.back()==0, "Unterminated plugin string");
    return std::string(reinterpret_cast<const char*>(b.data()),b.size()-1);
}
struct Sub { std::string type; std::span<const std::uint8_t> data; };
std::vector<Sub> subrecords(std::span<const std::uint8_t> b) {
    std::vector<Sub> out;
    std::optional<std::uint32_t> extended;
    for (std::size_t i=0; i<b.size();) {
        require(b.size()-i>=6,"Truncated subrecord header");
        auto type=tag(b,i); auto size=std::uint32_t(u16(b,i+4)); i+=6;
        if(type=="XXXX") {
            require(!extended && size==4 && b.size()-i>=4,"Malformed XXXX");
            extended=u32(b,i); i+=4; continue;
        }
        if(extended) { size=*extended; extended.reset(); }
        require(size<=b.size()-i,"Subrecord exceeds record boundary");
        out.push_back({type,b.subspan(i,size)}); i+=size;
    }
    require(!extended,"Dangling XXXX"); return out;
}
Bytes readAt(std::ifstream& stream, std::uint64_t offset, std::size_t count) {
    Bytes result(count); stream.seekg(static_cast<std::streamoff>(offset));
    stream.read(reinterpret_cast<char*>(result.data()),static_cast<std::streamsize>(count));
    require(bool(stream),"File read failed at "+std::to_string(offset)); return result;
}
Bytes payload(std::ifstream& stream,std::uint64_t p,std::uint32_t size,std::uint32_t flags) {
    require(size<=maxRecord,"Record exceeds safety limit");
    auto data=readAt(stream,p,size);
    if(flags&0x40000) {
        auto expected=u32(data,0); require(expected<=maxRecord,"Inflated record exceeds limit");
        Bytes inflated(expected); uLongf actual=expected; uLong compressed=static_cast<uLong>(data.size()-4);
        auto result=uncompress2(inflated.data(),&actual,data.data()+4,&compressed);
        require(result==Z_OK && actual==expected && compressed==data.size()-4,"Invalid compressed record");
        return inflated;
    }
    return data;
}
bool contains(const Sequence& s,const std::string& value) { return std::find(s.begin(),s.end(),value)!=s.end(); }
void uniqueSequence(const Sequence& values) {
    require(values.size()<=4096,"Excessive package list");
    require(std::set<std::string>(values.begin(),values.end()).size()==values.size(),"Duplicate package reference");
}
struct VmadReader {
    std::span<const std::uint8_t> bytes; const Sequence& names; std::size_t pos{}; std::uint16_t format{};
    std::uint32_t number(unsigned n) {
        require(pos<=bytes.size() && n<=bytes.size()-pos,"Truncated VMAD");
        auto v=n==1?bytes[pos]:n==2?u16(bytes,pos):u32(bytes,pos);pos+=n;return v;
    }
    std::string string() {
        auto n=number(2);require(n<=bytes.size()-pos,"Truncated VMAD string");
        std::string value(reinterpret_cast<const char*>(bytes.data()+pos),n);pos+=n;return value;
    }
    Json value(unsigned type) {
        if(type>=11 && type<=15) {
            auto count=number(4);require(count<=4096,"Excessive VMAD array");Json a=Json::array();
            while(count--) a.push_back(value(type-10));return a;
        }
        if(type==1) {
            require(bytes.size()-pos>=8,"Truncated VMAD object");
            auto raw=u32(bytes,pos+(format==2?4:0));auto alias=static_cast<std::int16_t>(u16(bytes,pos+(format==2?2:6)));pos+=8;
            auto key=formKey(raw,names);return {{"form",key.empty()?Json(nullptr):Json(key)},{"alias",alias}};
        }
        if(type==2) return string();
        if(type==3) return static_cast<std::int32_t>(number(4));
        if(type==4) {auto bits=number(4);float f;std::memcpy(&f,&bits,4);require(std::isfinite(f),"Nonfinite VMAD float");return f;}
        if(type==5) {auto v=number(1);require(v<=1,"Invalid VMAD bool");return bool(v);}
        throw std::runtime_error("Unsupported VMAD property type");
    }
    Json parse() {
        auto version=number(2);format=static_cast<std::uint16_t>(number(2));auto count=number(2);
        require((version==4||version==5) && (format==1||format==2) && count<=1024,"Unsupported VMAD header");
        Json scripts=Json::object();
        while(count--) {
            auto name=string();auto status=number(1);auto n=number(2);require(n<=4096 && !scripts.contains(name),"Invalid VMAD scripts/properties");
            Json props=Json::object();while(n--) {
                auto property=string();auto type=number(1);auto state=number(1);require(!props.contains(property),"Duplicate VMAD property");
                props[property]={{"type",type},{"status",state},{"value",value(type)}};
            }
            scripts[name]={{"status",status},{"properties",props}};
        }
        require(pos==bytes.size(),"Unsupported trailing NPC VMAD data");return scripts;
    }
};
float f32(std::span<const std::uint8_t> bytes,unsigned offset) { auto raw=u32(bytes,offset);float v;std::memcpy(&v,&raw,4);require(std::isfinite(v),"Nonfinite functional field");return v; }
bool relevantType(const std::string& t) {
    static const Sequence refs{"NPC_","PACK","FLST","SPEL","LVSP","SHOU","FACT","CSTY","LVLN","CLAS","KYWD","PERK","RACE","GLOB","EXPL","DEBR","TXST"};
    return contains(refs,t)||contains(inventoryTypes,t);
}
Json emptyFields() {
    Json f={{"PKID",Json::array()},{"SPOR",nullptr},{"ECOR",nullptr},{"OCOR",nullptr},{"GWOR",nullptr},
        {"Aggression",nullptr},{"Confidence",nullptr},{"Assistance",nullptr},
        {"SPLO",Json::array()},{"Magicka",nullptr},{"Stamina",nullptr},
        {"MagickaOffset",0},{"Protected",false},{"Factions",Json::array()},{"Inventory",Json::array()},
        {"CombatStyle",nullptr},{"SkillValues",Json::array()},{"UseAIPackages",false},{"VMAD",Json::object()}};
    for(const auto& x:scalarFields) f[x.name]=x.boolean?Json(false):Json(0);
    for(const auto& x:referenceFields) f[x.name]=nullptr;
    f["SkillOffsets"]=std::vector<unsigned>(18,0);f["Perks"]=Json::array();f["Keywords"]=Json::array();
    f["AttackRace"]=nullptr;f["AttackData"]=Json::object();f["Destructible"]=nullptr;
    return f;
}
}
std::string lower(std::string text) { for(auto& c:text) if(c>='A'&&c<='Z') c=char(c-'A'+'a'); return text; }
std::string formKey(std::uint32_t raw,const Sequence& names) {
    if(raw==0) return {};
    auto idx=raw>>24; require(idx<names.size(),"FormID has invalid master index");
    std::ostringstream out; out<<lower(names[idx])<<':'<<std::hex<<std::setw(6)<<std::setfill('0')<<(raw&0xffffff);
    return out.str();
}
std::string hashFile(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"SHA256 provider unavailable");
    struct Cleanup { BCRYPT_ALG_HANDLE& a; BCRYPT_HASH_HANDLE& h; ~Cleanup(){ if(h) BCryptDestroyHash(h); if(a) BCryptCloseAlgorithmProvider(a,0); } } cleanup{algorithm,hash};
    require(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0,"SHA256 initialization failed");
    std::ifstream in(path,std::ios::binary); require(bool(in),"Cannot hash "+path.string());
    std::array<char,65536> chunk{};
    while(in) { in.read(chunk.data(),chunk.size()); auto count=in.gcount();
        if(count) require(BCryptHashData(hash,reinterpret_cast<PUCHAR>(chunk.data()),static_cast<ULONG>(count),0)>=0,"SHA256 update failed"); }
    require(in.eof(),"Hash read failed"); std::array<unsigned char,32> digest{};
    require(BCryptFinishHash(hash,digest.data(),32,0)>=0,"SHA256 finish failed");
    std::ostringstream out; for(auto b:digest) out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b); return out.str();
}
Json readJson(const std::filesystem::path& path) {
    std::ifstream in(path); require(bool(in),"Cannot read "+path.string()); Json result; in>>result; return result;
}
void writeJson(const std::filesystem::path& path,const Json& data) {
    if(!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path,std::ios::trunc); require(bool(out),"Cannot write "+path.string());
    out<<data.dump(2)<<'\n'; out.flush(); require(bool(out),"JSON write failed");
}
Plugin readPlugin(const std::filesystem::path& path,std::string name) {
    Plugin plugin; plugin.name=lower(name); plugin.path=path;
    require(lower(path.filename().string())==plugin.name,"Manifest name/path mismatch");
    std::ifstream stream(path,std::ios::binary); require(bool(stream),"Cannot open "+path.string());
    auto length=std::filesystem::file_size(path); require(length>=24,"Missing TES4 header");
    auto head=readAt(stream,0,24); require(tag(head)=="TES4","First record is not TES4");
    auto size=u32(head,4); plugin.flags=u32(head,8);
    require(size<=length-24,"TES4 exceeds file boundary");
    auto data=payload(stream,24,size,plugin.flags);
    for(auto& sub:subrecords(data)) if(sub.type=="MAST") plugin.masters.push_back(lower(cstring(sub.data)));
    require(plugin.masters.size()<254,"Unsupported master count"); uniqueSequence(plugin.masters);
    auto names=plugin.masters; names.push_back(plugin.name);
    std::vector<std::uint64_t> ends{length}; auto position=std::uint64_t(24)+size;
    while(position<length) {
        while(ends.size()>1 && position==ends.back()) ends.pop_back();
        require(ends.back()-position>=24,"Truncated group/record header");
        head=readAt(stream,position,24); auto type=tag(head); size=u32(head,4);
        if(type=="GRUP") {
            require(size>=24 && size<=ends.back()-position,"Invalid group boundary");
            require(ends.size()<64,"Excessive group nesting");
            auto groupType=u32(head,12); auto label=tag(head,8);
            if(groupType==0 && !relevantType(label)) { position+=size; continue; }
            ends.push_back(position+size); position+=24; continue;
        }
        require(size<=ends.back()-position-24,"Record exceeds group boundary");
        auto flags=u32(head,8);
        if(relevantType(type)) {
            auto key=formKey(u32(head,12),names); require(!key.empty(),"Zero major FormID");
            if(!(flags&0x1000)) plugin.types[key]=(flags&0x20)?"DELETED":type;
            if(type=="NPC_") {
                require(!plugin.npcs.contains(key),"Duplicate NPC record in plugin");
                Npc npc; npc.key=key; npc.plugin=plugin.name; npc.deleted=(flags&0x20)!=0; npc.ignored=(flags&0x1000)!=0;
                npc.fields=emptyFields(); npc.observed=Json::object();
                data=payload(stream,position+24,size,flags);
                std::set<std::string> singleton;
                std::optional<std::uint32_t> spellCount;
                std::optional<std::uint32_t> itemCount,perkCount,keywordCount;
                Json pendingAttack=nullptr;
                for(auto& sub:subrecords(data)) {
                    auto& t=sub.type; auto v=sub.data;
                    for(const auto& sf:scalarFields) if(t==sf.tag) {
                        require(v.size()>=sf.diskOffset+sf.diskBytes,"Truncated scalar "+std::string(sf.name));
                        std::uint32_t raw=sf.diskBytes==4?u32(v,sf.diskOffset):sf.diskBytes==2?u16(v,sf.diskOffset):v[sf.diskOffset];
                        if(sf.diskMask) npc.fields[sf.name]=bool(raw&sf.diskMask);
                        else if(sf.boolean) npc.fields[sf.name]=bool(raw);
                        else if(sf.signedValue) npc.fields[sf.name]=static_cast<std::int16_t>(raw);
                        else npc.fields[sf.name]=raw;
                    }
                    for(const auto& rf:referenceFields) if(t==rf.tag) {
                        require(v.size()==4,"Invalid reference field");auto k=formKey(u32(v,0),names);npc.fields[rf.name]=k.empty()?Json(nullptr):Json(k);
                    }
                    if(t=="DNAM") { require(v.size()==52,"Invalid DNAM");npc.fields["SkillOffsets"]=std::vector<std::uint8_t>(v.begin()+18,v.begin()+36); }
                    if(t=="PRKZ") {require(v.size()==4 && singleton.insert(t).second,"Invalid PRKZ");perkCount=u32(v,0);}
                    if(t=="PRKR") {require(v.size()==8,"Invalid PRKR");auto k=formKey(u32(v,0),names);require(!k.empty(),"Null perk");npc.fields["Perks"].push_back(Json::array({k,static_cast<std::int8_t>(v[4])}));}
                    if(t=="KSIZ") {require(v.size()==4 && singleton.insert(t).second,"Invalid KSIZ");keywordCount=u32(v,0);}
                    if(t=="KWDA") {require(v.size()%4==0 && singleton.insert(t).second,"Invalid KWDA");for(unsigned j=0;j<v.size();j+=4){auto k=formKey(u32(v,j),names);require(!k.empty(),"Null keyword");npc.fields["Keywords"].push_back(k);}}
                    auto refAt=[&](unsigned offset)->Json {auto k=formKey(u32(v,offset),names);return k.empty()?Json(nullptr):Json(k);};
                    if(t=="ATKR") {require(v.size()==4 && singleton.insert(t).second,"Invalid ATKR");npc.fields["AttackRace"]=refAt(0);}
                    if(t=="ATKD") {
                        require(v.size()==44 && pendingAttack.is_null(),"Invalid ATKD/event pair");
                        pendingAttack={{"damage_mult",f32(v,0)},{"chance",f32(v,4)},{"spell",refAt(8)},{"flags",u32(v,12)},
                            {"attack_angle",f32(v,16)},{"strike_angle",f32(v,20)},{"stagger",f32(v,24)},{"type",refAt(28)},
                            {"knockdown",f32(v,32)},{"recovery",f32(v,36)},{"stamina_mult",f32(v,40)}};
                    }
                    if(t=="ATKE") {auto event=cstring(v);require(!pendingAttack.is_null() && !npc.fields["AttackData"].contains(event),"Unpaired/duplicate attack event");npc.fields["AttackData"][event]=pendingAttack;pendingAttack=nullptr;}
                    if(t=="DEST") {require(v.size()==8 && singleton.insert(t).second,"Invalid DEST");npc.fields["Destructible"]={{"health",u32(v,0)},{"count",v[4]},{"flags",v[5]},{"stages",Json::array()}};}
                    if(t=="DSTD") {require(v.size()==20 && !npc.fields["Destructible"].is_null(),"Invalid destruction stage");
                        npc.fields["Destructible"]["stages"].push_back({{"health_percent",v[0]},{"index",v[1]},{"model_stage",v[2]},{"flags",v[3]},
                            {"self_damage",u32(v,4)},{"explosion",refAt(8)},{"debris",refAt(12)},{"debris_count",u32(v,16)},{"model",Json::array()}});}
                    if(t=="DMDL"||t=="DMDT"||t=="DMDS") {
                        require(!npc.fields["Destructible"].is_null() && !npc.fields["Destructible"]["stages"].empty(),"Orphan destruction model");
                        npc.fields["Destructible"]["stages"].back()["model"].push_back({{"tag",t},{"raw",std::vector<std::uint8_t>(v.begin(),v.end())}});
                    }
                    if(t=="EDID") npc.editor=cstring(v);
                    if(t=="PKID") { require(v.size()==4,"Invalid PKID"); auto ref=formKey(u32(v,0),names); require(!ref.empty(),"Null package"); npc.fields[t].push_back(ref); }
                    if(t=="SPLO") { require(v.size()==4,"Invalid SPLO"); auto ref=formKey(u32(v,0),names); require(!ref.empty(),"Null spell"); npc.fields[t].push_back(ref); }
                    if(t=="SPCT") { require(v.size()==4 && singleton.insert(t).second,"Invalid/duplicate SPCT"); spellCount=u32(v,0); }
                    if(t=="DNAM") {
                        require(v.size()==52 && singleton.insert(t).second,"Unsupported/duplicate DNAM");
                        npc.fields["Magicka"]=u16(v,38); npc.fields["Stamina"]=u16(v,40);
                        npc.fields["SkillValues"]=std::vector<std::uint8_t>(v.begin(),v.begin()+18);
                    }
                    if(t=="SNAM") {
                        require(v.size()==8,"Invalid faction entry");auto ref=formKey(u32(v,0),names);require(!ref.empty(),"Null faction");
                        npc.fields["Factions"].push_back(Json::array({ref,static_cast<std::int8_t>(v[4])}));
                    }
                    if(t=="COCT") {require(v.size()==4 && singleton.insert(t).second,"Invalid COCT");itemCount=u32(v,0);}
                    if(t=="CNTO") {
                        require(v.size()==8,"Invalid inventory entry");auto ref=formKey(u32(v,0),names);require(!ref.empty(),"Null item");
                        npc.fields["Inventory"].push_back({{"item",ref},{"count",static_cast<std::int32_t>(u32(v,4))},{"extra",nullptr}});
                    }
                    if(t=="COED") {
                        require(v.size()==12 && !npc.fields["Inventory"].empty(),"Invalid container extra");
                        require(npc.fields["Inventory"].back()["extra"].is_null(),"Duplicate container extra");
                        auto raw=u32(v,4);Json candidate=nullptr;
                        if(raw && (raw>>24)<names.size()) candidate=formKey(raw,names);
                        npc.fields["Inventory"].back()["extra"]={{"owner",refAt(0)},{"condition_raw",raw},{"global_candidate",candidate},{"health",f32(v,8)}};
                    }
                    if(t=="VMAD") {
                        require(singleton.insert(t).second,"Duplicate VMAD");
                        try {npc.fields["VMAD"]=VmadReader{v,names}.parse();}
                        catch(const std::exception& e) {npc.fields["VMAD"]={{"unparsed",e.what()}};npc.observed["VMAD_parse_error"]=e.what();}
                    }
                    if(t=="SPOR"||t=="ECOR"||t=="OCOR"||t=="GWOR"||t=="DPLT"||t=="ZNAM"||t=="TPLT"||t=="RNAM") {
                        require(v.size()==4 && singleton.insert(t).second,"Invalid/duplicate "+t);
                        auto ref=formKey(u32(v,0),names); Json value=ref.empty()?Json(nullptr):Json(ref);
                        if(t=="ZNAM") npc.fields["CombatStyle"]=value;
                        else if(npc.fields.contains(t)) npc.fields[t]=value; else npc.observed[t]=value;
                    }
                    if(t=="AIDT") {
                        require(v.size()==20 && singleton.insert(t).second,"Unsupported/duplicate AIDT");
                        require(v[0]<=3 && v[1]<=4 && v[5]<=2,"Invalid AI parameter");
                        npc.fields["Aggression"]=v[0]; npc.fields["Confidence"]=v[1]; npc.fields["Assistance"]=v[5];
                    }
                    if(t=="ACBS") {
                        require(v.size()==24 && singleton.insert(t).second,"Unsupported/duplicate ACBS");npc.templateFlags=u16(v,18);
                        npc.fields["MagickaOffset"]=static_cast<std::int16_t>(u16(v,4));npc.fields["Protected"]=(u32(v,0)&0x800)!=0;
                        npc.fields["UseAIPackages"]=(npc.templateFlags&0x20)!=0;
                    }
                }
                if(!npc.deleted && !npc.ignored) require(singleton.contains("ACBS") && singleton.contains("AIDT") && singleton.contains("DNAM"),"NPC lacks ACBS/AIDT/DNAM");
                require(pendingAttack.is_null(),"Unpaired ATKD");
                require(perkCount.value_or(0)==npc.fields["Perks"].size(),"PRKZ/PRKR count mismatch");
                require(keywordCount.value_or(0)==npc.fields["Keywords"].size(),"KSIZ/KWDA count mismatch");
                if(!npc.fields["Destructible"].is_null()) require(npc.fields["Destructible"]["count"]==npc.fields["Destructible"]["stages"].size(),"DEST stage count mismatch");
                for(auto f:{"Keywords","Perks"}) {require(npc.fields[f].size()<=4096,"Excessive functional list");std::sort(npc.fields[f].begin(),npc.fields[f].end());}
                uniqueSequence(npc.fields["PKID"].get<Sequence>());
                require(npc.fields["SPLO"].size()<=4096,"Excessive spell list");
                require(spellCount.value_or(0)==npc.fields["SPLO"].size(),"SPCT/SPLO count mismatch");
                auto spells=npc.fields["SPLO"].get<Sequence>(); std::ranges::sort(spells);
                spells.erase(std::unique(spells.begin(),spells.end()),spells.end()); npc.fields["SPLO"]=spells;
                require(itemCount.value_or(0)==npc.fields["Inventory"].size(),"COCT/CNTO count mismatch");
                for(auto field:{"Factions","Inventory"}) {auto& list=npc.fields[field];require(list.size()<=4096,"Excessive list");std::sort(list.begin(),list.end());}
                plugin.npcs.emplace(key,std::move(npc));
            }
        }
        position+=24+size;
    }
    require(position==length,"File boundary mismatch");
    if(!plugin.npcs.empty()) plugin.sha256=hashFile(path);
    return plugin;
}

Sequence parseExclusions(std::string text) {
    if(text.starts_with("\xef\xbb\xbf")) text.erase(0,3);
    Sequence rules; std::istringstream input(text); std::string line; unsigned number=0;
    while(std::getline(input,line)) {
        ++number;
        auto first=line.find_first_not_of(" \t\r"); if(first==std::string::npos) continue;
        line=line.substr(first,line.find_last_not_of(" \t\r")-first+1);
        if(line[0]=='#'||line[0]==';') continue;
        auto value=lower(line);
        auto error="Invalid NPC exclusion at line "+std::to_string(number)+": "+line;
        if(value.starts_with("editorid:")) {
            auto name=value.substr(9);
            require(!name.empty() && name.find_first_of(" \t\r:")==std::string::npos,error);
        } else {
            auto colon=value.rfind(':'); require(colon!=std::string::npos,error);
            auto file=value.substr(0,colon); auto id=value.substr(colon+1);
            require((file.ends_with(".esm")||file.ends_with(".esp")||file.ends_with(".esl")) && file.find_first_of("\\/:")==std::string::npos,error);
            require(!id.empty() && id.size()<=6 && id.find_first_not_of("0123456789abcdef")==std::string::npos,error);
            auto local=static_cast<std::uint32_t>(std::stoul(id,nullptr,16)); require(local!=0,error);
            value=formKey(local,{file});
        }
        if(!contains(rules,value)) rules.push_back(value);
    }
    return rules;
}
Sequence readExclusions(const std::filesystem::path& path) {
    if(!std::filesystem::exists(path)) return {};
    std::ifstream in(path,std::ios::binary); require(bool(in),"Cannot read exclusions: "+path.string());
    std::string text((std::istreambuf_iterator<char>(in)),{});
    require(!in.bad(),"Exclusion file read failed"); return parseExclusions(text);
}
void validateConfig(const Json& config) {
    require(config.at("schema")==2,"Config schema 2 required: replace the old AIO-specific configuration");
    const std::set<std::string> keys{"schema","mode","fields","only_npcs","baseline"};
    for(auto& [key,_]:config.items()) require(keys.contains(key),"Unknown configuration key: "+key);
    auto mode=config.at("mode").get<std::string>(); require(mode=="audit"||mode=="apply","mode must be audit or apply");
    auto baseline=lower(config.value("baseline",std::string()));
    require(baseline.empty() || ((baseline.ends_with(".esp")||baseline.ends_with(".esm")||baseline.ends_with(".esl")) && baseline.find_first_of("\\/:")==std::string::npos),"baseline must be a plugin filename or an empty string");
    require(config.at("fields").is_array(),"fields must be an array");
    std::set<std::string> fields;
    for(auto& f:config.at("fields")) {
        auto name=f.get<std::string>(); require(contains(fieldNames,name)||protectedField(name),"Unknown writable field: "+name);
        require(fields.insert(name).second,"Duplicate writable field");
    }
    require(!fields.empty(),"At least one writable field is required");
    require(config.at("only_npcs").is_array(),"only_npcs must be an array");
    for(auto& key:config.at("only_npcs")) {
        auto rules=parseExclusions(key.get<std::string>());
        require(rules.size()==1 && !rules[0].starts_with("editorid:"),"only_npcs requires plugin:localID");
    }
}
Json configurationWarnings(const Json& config) {
    Json warnings=Json::array();for(const auto& name:config.at("fields")) if(protectedField(name.get<std::string>()))
        warnings.push_back({{"field",name},{"reason","Protected appearance/race/voice/name/outfit field ignored"}});
    return warnings;
}
Json makePlan(const std::vector<Plugin>& inputs,const Json& config,const Sequence& exclusions) {
    validateConfig(config);
    // COED's second word is either a signed faction rank or a file-local global
    // reference. Resolve that union using the complete loaded form-type table.
    auto plugins=inputs;
    std::map<std::string,std::string> ownerTypes;for(const auto& plugin:plugins) for(const auto& [key,type]:plugin.types) ownerTypes[key]=type;
    for(auto& plugin:plugins) for(auto& [key,npc]:plugin.npcs) if(npc.fields.contains("Inventory")) {
        for(auto& item:npc.fields["Inventory"]) {
            auto& extra=item["extra"];if(!extra.is_object()||!extra.contains("condition_raw"))continue;
            auto owner=extra.at("owner");auto raw=extra.at("condition_raw").get<std::uint32_t>();
            std::string type=owner.is_null()?"NONE":ownerTypes[owner.get<std::string>()];
            Json condition;
            if(type=="FACT") condition={{"rank",static_cast<std::int32_t>(raw)}};
            else if(type=="NPC_") {if(raw && extra["global_candidate"].is_null())condition={{"invalid",raw}};else condition={{"global",extra["global_candidate"]}};}
            else if(type=="NONE") condition=nullptr;
            else condition={{"invalid_owner",owner}};
            extra={{"owner",owner},{"condition",condition},{"health",extra.at("health")}};
        }
        if(npc.fields["Inventory"].is_array()) std::sort(npc.fields["Inventory"].begin(),npc.fields["Inventory"].end());
    }
    const std::set<std::string> official{"skyrim.esm","update.esm","dawnguard.esm","hearthfires.esm","dragonborn.esm"};
    std::map<std::string,std::size_t> indices;
    std::map<std::string,std::vector<const Npc*>> history;
    std::map<std::string,std::string> types;
    Json evidence=Json::array();
    for(std::size_t i=0;i<plugins.size();++i) {
        auto& p=plugins[i]; require(indices.emplace(p.name,i).second,"Duplicate loaded plugin");
        for(auto& master:p.masters) require(indices.contains(master)&&indices.at(master)<i,"Missing/out-of-order master: "+p.name+" -> "+master);
        for(auto& [key,npc]:p.npcs) if(!npc.ignored) history[key].push_back(&npc);
        for(auto& [key,type]:p.types) types[key]=type;
        if(!p.sha256.empty()) evidence.push_back({{"plugin",p.name},{"sha256",p.sha256},{"npc_records",p.npcs.size()}});
    }
    auto requestedBaseline=lower(config.value("baseline",std::string()));
    auto activeBaseline=indices.contains(requestedBaseline)?requestedBaseline:std::string();
    Json baselineInfo={{"requested",requestedBaseline},{"active",activeBaseline.empty()?Json(nullptr):Json(activeBaseline)},
        {"status",requestedBaseline.empty()?"official_requested":activeBaseline.empty()?"not_loaded_using_official":"plugin_loaded"}};
    std::set<std::string> enabled,sourceNames;
    for(auto& f:config.at("fields")) if(!protectedField(f.get<std::string>())) enabled.insert(f.get<std::string>());
    Json exclusionReport=Json::array();
    for(auto& rule:exclusions) exclusionReport.push_back({{"rule",rule},{"matched_npcs",Json::array()}});
    Json rows=Json::array(); unsigned changedNpcs=0,changedFields=0,skipped=0,ready=0,skippedFields=0,eligibleFields=0;
    for(auto& [key,chain]:history) {
        auto final=chain.back(); const Npc* baseline=chain.front();
        bool officialBaseline=false;
        for(auto npc:chain) if(official.contains(npc->plugin)) {baseline=npc;officialBaseline=true;}
        const Npc* originalBaseline=baseline;
        bool customBaseline=false;
        for(auto npc:chain) if(!activeBaseline.empty() && npc->plugin==activeBaseline) {baseline=npc;customBaseline=true;}
        std::map<std::string,const Npc*> selected;
        for(auto npc:chain) {
            if(npc->deleted || npc==chain.front() || npc==baseline || official.contains(npc->plugin)) continue;
            for(auto& field:enabled) {
                const auto& value=npc->fields.at(field);
                if(value!=baseline->fields.at(field) && value!=originalBaseline->fields.at(field)) selected[field]=npc;
            }
        }
        std::string reason;
        for(auto& entry:exclusionReport) {
            auto rule=entry.at("rule").get<std::string>();
            if(rule==key || (rule.starts_with("editorid:") && lower(final->editor)==rule.substr(9))) {
                entry["matched_npcs"].push_back(key); reason="Excluded by "+rule;
            }
        }

        bool excluded=!reason.empty();
        if(reason.empty() && final->deleted) reason="Final NPC is deleted";
        if(reason.empty() && baseline->deleted) reason="Baseline NPC is deleted";
        Json fields=Json::object(); unsigned changes=0,applicable=0;
        for(auto& [field,npc]:selected) {
            const auto& value=npc->fields.at(field);std::string fieldReason=reason;
            if(fieldReason.empty() && field=="Template" && templateSharedWithProtectedData(final->templateFlags))
                fieldReason="Template shared with appearance (Use Traits); skipped";
            if(fieldReason.empty() && field=="VMAD" && (value.contains("unparsed") || final->fields.at(field).contains("unparsed")))
                fieldReason="Unparsed VMAD cannot be used for script binding";
            if(fieldReason.empty() && field=="Inventory") {
                for(auto* list:{&value,&final->fields.at(field)}) for(auto& entry:*list) if(!entry.at("extra").is_null()) {
                    const auto& extra=entry.at("extra");
                    if(!extra.is_object() || !extra.contains("condition") || (extra["condition"].is_object() &&
                        (extra["condition"].contains("invalid") || extra["condition"].contains("invalid_owner"))))
                        fieldReason="Container ownership/extra data could not be resolved";
                }
            }
            if(fieldReason.empty() && field=="Factions") {
                std::set<std::string> seen;
                for(auto& entry:value) if(!seen.insert(entry.at(0).get<std::string>()).second) fieldReason="Duplicate faction entries require manual resolution";
            }
            auto checkRef=[&](const Json& ref,const Sequence& accepted) {
                if(ref.is_null() || !fieldReason.empty()) return;
                auto target=ref.get<std::string>();
                if(!types.contains(target)||!contains(accepted,types.at(target))) fieldReason="Invalid "+field+" reference: "+target;
            };
            if(const auto* ref=referenceField(field)) checkRef(value,ref->types);
            if(field=="Keywords") for(auto& ref:value) checkRef(ref,{"KYWD"});
            if(field=="Perks") for(auto& entry:value) checkRef(entry.at(0),{"PERK"});
            if(field=="AttackRace") checkRef(value,{"RACE"});
            if(field=="PKID") for(auto& ref:value) checkRef(ref,{"PACK"});
            else if(field=="SPLO") for(auto& ref:value) checkRef(ref,{"SPEL","LVSP","SHOU"});
            else if(field=="Factions") for(auto& entry:value) checkRef(entry.at(0),{"FACT"});
            else if(field=="Inventory") for(auto& entry:value) {
                checkRef(entry.at("item"),inventoryTypes);const auto& extra=entry.at("extra");
                if(extra.is_object() && extra.contains("owner")) {checkRef(extra.at("owner"),{"NPC_","FACT"});
                    if(extra.contains("condition") && extra["condition"].is_object() && extra["condition"].contains("global")) checkRef(extra["condition"]["global"],{"GLOB"});}
            }
            if(field=="AttackData") for(const auto& entry:value) {checkRef(entry.at("spell"),{"SPEL","SHOU"});checkRef(entry.at("type"),{"KYWD"});}
            else if(field=="CombatStyle") checkRef(value,{"CSTY"});
            else if(field=="SPOR"||field=="ECOR"||field=="OCOR"||field=="GWOR") checkRef(value,{"FLST"});
            bool differs=final->fields.at(field)!=value;sourceNames.insert(npc->plugin);
            fields[field]={{"baseline",baseline->fields.at(field)},{"official_baseline",originalBaseline->fields.at(field)},{"disk",final->fields.at(field)},
                {"expected",value},{"sources",Json::array({npc->plugin})},{"differs",differs},
                {"status",fieldReason.empty()?"ready":"skipped"},{"reason",fieldReason}};
            if(fieldReason.empty()) {++applicable;++eligibleFields;changes+=differs;} else ++skippedFields;
        }
        auto status=excluded?"excluded":!reason.empty()?"skipped":fields.empty()?"no_source_changes":applicable?"ready":"skipped";
        if(reason.empty() && !fields.empty() && !applicable) reason="No applicable selected fields; see field reasons";
        rows.push_back({{"npc",key},{"editor_id",final->editor},{"winner",final->plugin},{"baseline_plugin",baseline->plugin},
            {"baseline_kind",customBaseline?"configured_plugin":officialBaseline?"official":"first_definition"},
            {"official_baseline_plugin",originalBaseline->plugin},{"official_baseline_kind",officialBaseline?"official":"first_definition"},
            {"status",status},{"reason",reason},{"fields",fields},{"observed_only",final->observed},
            {"inheritance_record",{{"flags",final->templateFlags},{"fields",final->fields},{"writable",!excluded&&!final->deleted&&!baseline->deleted}}}});
        if(status==std::string("skipped")||excluded) ++skipped;
        else if(applicable) {++ready;if(changes) {++changedNpcs;changedFields+=changes;}}

    }
    return {{"schema",2},{"strategy","last_nonbaseline_field"},{"baseline_comparison","official_and_configured"},{"configuration_warnings",configurationWarnings(config)},{"mode",config.at("mode")},
        {"runtime_adapter_limits",Json::array({{{"field","Destructible"},{"reason","Tracked, but destruction model/preload ownership is not yet implemented for runtime writes"}}})},{"baseline",baselineInfo},{"enabled_fields",enabled},{"requested_fields",config.at("fields")},{"selected_sources",sourceNames},{"exclusions",exclusionReport},{"plugins",evidence},{"npcs",rows},
        {"summary",{{"scanned_npcs",rows.size()},{"eligible_npcs",ready},{"npcs_needing_changes",changedNpcs},{"fields_needing_changes",changedFields},{"skipped_or_excluded_npcs",skipped},{"eligible_fields",eligibleFields},{"skipped_fields",skippedFields}}}};
}
Json run(const Json& manifest,const Json& config,const Sequence& exclusions) {
    validateConfig(config);
    require(manifest.at("schema")==1,"Unsupported manifest schema");
    std::vector<Plugin> plugins;
    for(auto& p:manifest.at("plugins")) {
        auto utf8=p.at("path").get<std::string>();
        auto path=std::filesystem::path(std::u8string(utf8.begin(),utf8.end()));
        try { plugins.push_back(readPlugin(path,p.at("name").get<std::string>())); }
        catch(const std::exception& e) { throw std::runtime_error(p.at("name").get<std::string>()+": "+e.what()); }
    }
    return makePlan(plugins,config,exclusions);
}
}
