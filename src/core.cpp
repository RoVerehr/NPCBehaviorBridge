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
#include <chrono>
#include <atomic>
#include <thread>

namespace nbb {
const Sequence fieldNames=[] {auto names=legacyFieldNames;for(const auto& f:scalarFields) names.push_back(f.name);for(const auto& f:referenceFields) names.push_back(f.name);
    for(auto name:{"SkillOffsets","Perks","Keywords","AttackRace","AttackData","Destructible"}) names.push_back(name);return names;}();
namespace {
using Bytes = std::vector<std::uint8_t>;
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
        require(b.size()-i>=6,"Truncated subrecord header at payload_offset="+std::to_string(i)+"; remaining_bytes="+std::to_string(b.size()-i));
        auto type=tag(b,i); auto size=std::uint32_t(u16(b,i+4)); i+=6;
        if(type=="XXXX") {
            require(!extended && size==4 && b.size()-i>=4,"Malformed XXXX");
            extended=u32(b,i); i+=4; continue;
        }
        if(extended) { size=*extended; extended.reset(); }
        require(size<=b.size()-i,"Subrecord "+type+" exceeds record boundary at payload_offset="+std::to_string(i-6)+"; declared_bytes="+std::to_string(size)+"; remaining_bytes="+std::to_string(b.size()-i));
        out.push_back({type,b.subspan(i,size)}); i+=size;
    }
    require(!extended,"Dangling XXXX"); return out;
}
// Small record headers share a bounded read window instead of seeking/allocating
// for every header. Views last until the next read; decoded payloads own their bytes.
class PluginReader {
    std::ifstream stream;
    std::uint64_t length{},begin{};
    std::size_t available{};
    std::array<std::uint8_t,65536> window{};
    Bytes large;
public:
    explicit PluginReader(const std::filesystem::path& path):stream(path,std::ios::binary) {
        require(bool(stream),"Cannot open "+path.string());length=std::filesystem::file_size(path);
    }
    std::uint64_t size() const {return length;}
    std::span<const std::uint8_t> read(std::uint64_t offset,std::size_t count) {
        require(offset<=length && count<=length-offset,"File read failed at "+std::to_string(offset));
        if(offset>=begin && offset-begin<=available && count<=available-static_cast<std::size_t>(offset-begin))
            return std::span<const std::uint8_t>(window).subspan(static_cast<std::size_t>(offset-begin),count);
        stream.clear();stream.seekg(static_cast<std::streamoff>(offset));
        if(count>window.size()) {
            large.resize(count);stream.read(reinterpret_cast<char*>(large.data()),static_cast<std::streamsize>(count));
            require(bool(stream),"File read failed at "+std::to_string(offset));return large;
        }
        available=static_cast<std::size_t>(std::min<std::uint64_t>(window.size(),length-offset));begin=offset;
        stream.read(reinterpret_cast<char*>(window.data()),static_cast<std::streamsize>(available));
        require(bool(stream),"File read failed at "+std::to_string(offset));
        return std::span<const std::uint8_t>(window).first(count);
    }
};
std::span<const std::uint8_t> readAt(PluginReader& stream,std::uint64_t offset,std::size_t count) {return stream.read(offset,count);}
Bytes payload(PluginReader& stream,std::uint64_t p,std::uint32_t size,std::uint32_t flags) {
    auto data=readAt(stream,p,size);
    if(flags&0x40000) {
        const auto expected=u32(data,0);Bytes inflated;inflated.reserve(std::min<std::size_t>(expected,65536));
        z_stream decoder{};require(inflateInit(&decoder)==Z_OK,"Cannot initialize record decoder");
        struct End {z_stream& decoder;~End(){inflateEnd(&decoder);}} end{decoder};
        decoder.next_in=const_cast<Bytef*>(data.data()+4);decoder.avail_in=static_cast<uInt>(data.size()-4);
        std::array<std::uint8_t,65536> window{};int result=Z_OK;
        do {
            decoder.next_out=window.data();decoder.avail_out=static_cast<uInt>(window.size());
            result=inflate(&decoder,Z_NO_FLUSH);const auto produced=window.size()-decoder.avail_out;
            require((result==Z_OK||result==Z_STREAM_END)&&produced<=expected-inflated.size(),
                "Invalid compressed record at file_offset="+std::to_string(p)+"; declared_bytes="+std::to_string(expected)+
                "; decoded_bytes="+std::to_string(inflated.size()+produced)+"; zlib_result="+std::to_string(result));
            inflated.insert(inflated.end(),window.begin(),window.begin()+produced);
        }while(result!=Z_STREAM_END);
        require(inflated.size()==expected&&decoder.avail_in==0,
            "Compressed record size mismatch at file_offset="+std::to_string(p)+"; declared_bytes="+std::to_string(expected)+
            "; decoded_bytes="+std::to_string(inflated.size())+"; unread_compressed_bytes="+std::to_string(decoder.avail_in));
        return inflated;
    }
    return Bytes(data.begin(),data.end());
}
bool contains(const Sequence& s,const std::string& value) { return std::find(s.begin(),s.end(),value)!=s.end(); }
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
            auto count=number(4);require(count<=bytes.size()-pos,"VMAD array exceeds remaining payload");Json a=Json::array();
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
        if(type==5) {auto v=number(1);return bool(v);}
        throw std::runtime_error("Unsupported VMAD property type");
    }
    Json parse() {
        auto version=number(2);format=static_cast<std::uint16_t>(number(2));auto count=number(2);
        require((version==4||version==5) && (format==1||format==2),"Unsupported VMAD header");
        Json scripts=Json::object();
        while(count--) {
            auto name=string();auto status=number(1);auto n=number(2);
            Json props=Json::object();while(n--) {
                auto property=string();auto type=number(1);auto state=number(1);
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
Sequence inputFields(const std::string& tag) {
    Sequence result;
    for(const auto& f:scalarFields)if(tag==f.tag)result.push_back(f.name);
    for(const auto& f:referenceFields)if(tag==f.tag)result.push_back(f.name);
    const std::map<std::string,Sequence> extra{
        {"ACBS",{"MagickaOffset","Protected","UseAIPackages","Template"}},
        {"AIDT",{"Aggression","Confidence","Assistance"}},
        {"DNAM",{"SkillOffsets","Magicka","Stamina","SkillValues"}},
        {"PKID",{"PKID"}},{"SPLO",{"SPLO"}},{"SPCT",{"SPLO"}},
        {"SNAM",{"Factions"}},{"CNTO",{"Inventory"}},{"COED",{"Inventory"}},{"COCT",{"Inventory"}},
        {"PRKR",{"Perks"}},{"PRKZ",{"Perks"}},{"KWDA",{"Keywords"}},{"KSIZ",{"Keywords"}},
        {"ATKR",{"AttackRace"}},{"ATKD",{"AttackData"}},{"ATKE",{"AttackData"}},
        {"DEST",{"Destructible"}},{"DSTD",{"Destructible"}},{"DMDL",{"Destructible"}},
        {"DMDT",{"Destructible"}},{"DMDS",{"Destructible"}},{"VMAD",{"VMAD"}},
        {"SPOR",{"SPOR"}},{"ECOR",{"ECOR"}},{"OCOR",{"OCOR"}},{"GWOR",{"GWOR"}},
        {"ZNAM",{"CombatStyle"}}};
    if(auto it=extra.find(tag);it!=extra.end())result.insert(result.end(),it->second.begin(),it->second.end());
    return result;
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
    auto idx=raw>>24; require(idx<names.size(),"FormID has invalid master index: raw="+std::to_string(raw)+"; index="+std::to_string(idx)+"; master_table_entries="+std::to_string(names.size()));
    auto result=lower(names[idx]);result.push_back(':');
    constexpr char hex[]="0123456789abcdef";
    for(int shift=20;shift>=0;shift-=4)result.push_back(hex[(raw>>shift)&15]);
    return result;
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
Npc parseNpcRecord(std::span<const std::uint8_t> data,const std::string& key,const std::string& pluginName,
    const Sequence& names,std::uint32_t flags) {
    Npc npc; npc.key=key; npc.plugin=pluginName; npc.deleted=(flags&0x20)!=0; npc.ignored=(flags&0x1000)!=0;
    npc.fields=emptyFields(); npc.observed=Json::object();
    auto failure=[&](const Sequence& fields,const std::string& tag,std::size_t offset,const std::string& reason) {
        std::ostringstream text;text<<"plugin="<<pluginName<<"; npc="<<key<<"; editor_id="<<npc.editor
            <<"; subrecord="<<tag<<"; payload_offset=0x"<<std::hex<<offset<<std::dec<<"; reason="<<reason;
        for(const auto& field:fields) {
            if(npc.inputErrors.contains(field))npc.inputErrors[field]=npc.inputErrors[field].get<std::string>()+" | "+text.str();
            else npc.inputErrors[field]=text.str();
        }
    };
    std::vector<Sub> records;
    try {records=subrecords(data);}
    catch(const std::exception& e){failure(fieldNames,"<record>",0,e.what());return npc;}
    std::set<std::string> singleton;
    std::optional<std::uint32_t> spellCount;
    std::optional<std::uint32_t> itemCount,perkCount,keywordCount;
    Json pendingAttack=nullptr;
    for(auto& sub:records) {
        auto& t=sub.type; auto v=sub.data;
        try {
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
        if(t=="PRKZ") {require(v.size()==4,"Invalid PRKZ");singleton.insert(t);perkCount=u32(v,0);}
        if(t=="PRKR") {require(v.size()==8,"Invalid PRKR");auto k=formKey(u32(v,0),names);require(!k.empty(),"Null perk");npc.fields["Perks"].push_back(Json::array({k,static_cast<std::int8_t>(v[4])}));}
        if(t=="KSIZ") {require(v.size()==4,"Invalid KSIZ");singleton.insert(t);keywordCount=u32(v,0);}
        if(t=="KWDA") {require(v.size()%4==0,"Invalid KWDA");singleton.insert(t);for(unsigned j=0;j<v.size();j+=4){auto k=formKey(u32(v,j),names);require(!k.empty(),"Null keyword");npc.fields["Keywords"].push_back(k);}}
        auto refAt=[&](unsigned offset)->Json {auto k=formKey(u32(v,offset),names);return k.empty()?Json(nullptr):Json(k);};
        if(t=="ATKR") {require(v.size()==4,"Invalid ATKR");singleton.insert(t);npc.fields["AttackRace"]=refAt(0);}
        if(t=="ATKD") {
            require(v.size()==44 && pendingAttack.is_null(),"Invalid ATKD/event pair");
            pendingAttack={{"damage_mult",f32(v,0)},{"chance",f32(v,4)},{"spell",refAt(8)},{"flags",u32(v,12)},
                {"attack_angle",f32(v,16)},{"strike_angle",f32(v,20)},{"stagger",f32(v,24)},{"type",refAt(28)},
                {"knockdown",f32(v,32)},{"recovery",f32(v,36)},{"stamina_mult",f32(v,40)}};
        }
        if(t=="ATKE") {auto event=cstring(v);require(!pendingAttack.is_null(),"Unpaired attack event");npc.fields["AttackData"][event]=pendingAttack;pendingAttack=nullptr;}
        if(t=="DEST") {require(v.size()==8,"Invalid DEST");singleton.insert(t);npc.fields["Destructible"]={{"health",u32(v,0)},{"count",v[4]},{"flags",v[5]},{"stages",Json::array()}};}
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
        if(t=="SPCT") { require(v.size()==4,"Invalid SPCT size");singleton.insert(t); spellCount=u32(v,0); }
        if(t=="DNAM") {
            require(v.size()==52,"Unsupported DNAM size");singleton.insert(t);
            npc.fields["Magicka"]=u16(v,38); npc.fields["Stamina"]=u16(v,40);
            npc.fields["SkillValues"]=std::vector<std::uint8_t>(v.begin(),v.begin()+18);
        }
        if(t=="SNAM") {
            require(v.size()==8,"Invalid faction entry");auto ref=formKey(u32(v,0),names);require(!ref.empty(),"Null faction");
            npc.fields["Factions"].push_back(Json::array({ref,static_cast<std::int8_t>(v[4])}));
        }
        if(t=="COCT") {require(v.size()==4,"Invalid COCT");singleton.insert(t);itemCount=u32(v,0);}
        if(t=="CNTO") {
            require(v.size()==8,"Invalid inventory entry");auto ref=formKey(u32(v,0),names);require(!ref.empty(),"Null item");
            npc.fields["Inventory"].push_back({{"item",ref},{"count",static_cast<std::int32_t>(u32(v,4))},{"extra",nullptr}});
        }
        if(t=="COED") {
            require(v.size()==12 && !npc.fields["Inventory"].empty(),"Invalid container extra");

            auto raw=u32(v,4);Json candidate=nullptr;
            if(raw && (raw>>24)<names.size()) candidate=formKey(raw,names);
            npc.fields["Inventory"].back()["extra"]={{"owner",refAt(0)},{"condition_raw",raw},{"global_candidate",candidate},{"health",f32(v,8)}};
        }
        if(t=="VMAD") {
            singleton.insert(t);
            try {npc.fields["VMAD"]=VmadReader{v,names}.parse();}
            catch(const std::exception& e) {npc.fields["VMAD"]={{"unparsed",e.what()}};npc.observed["VMAD_parse_error"]=e.what();throw std::runtime_error(std::string("VMAD: ")+e.what());}
        }
        if(t=="SPOR"||t=="ECOR"||t=="OCOR"||t=="GWOR"||t=="DPLT"||t=="ZNAM"||t=="TPLT"||t=="RNAM") {
            require(v.size()==4,"Invalid size for "+t);singleton.insert(t);
            auto ref=formKey(u32(v,0),names); Json value=ref.empty()?Json(nullptr):Json(ref);
            if(t=="ZNAM") npc.fields["CombatStyle"]=value;
            else if(npc.fields.contains(t)) npc.fields[t]=value; else npc.observed[t]=value;
        }
        if(t=="AIDT") {
            require(v.size()==20,"Unsupported AIDT size");singleton.insert(t);
            // Preserve serialized AI values; runtime storage is checked only when writing.
            npc.fields["Aggression"]=v[0]; npc.fields["Confidence"]=v[1]; npc.fields["Assistance"]=v[5];
        }
        if(t=="ACBS") {
            require(v.size()==24,"Unsupported ACBS size");singleton.insert(t);npc.templateFlags=u16(v,18);
            npc.fields["MagickaOffset"]=static_cast<std::int16_t>(u16(v,4));npc.fields["Protected"]=(u32(v,0)&0x800)!=0;
            npc.fields["UseAIPackages"]=(npc.templateFlags&0x20)!=0;
        }
        } catch(const std::exception& e) {failure(inputFields(t),t,static_cast<std::size_t>(v.data()-data.data()),e.what());}
    }
    if(!npc.deleted && !npc.ignored)for(const auto* t:{"ACBS","AIDT","DNAM"})
        if(!singleton.contains(t))failure(inputFields(t),t,data.size(),"Required data block is absent");
    if(!pendingAttack.is_null())failure({"AttackData"},"ATKD",data.size(),"Unpaired ATKD");
    // Metadata counts do not veto readable serialized list entries. Lists are
    // selected whole, without repairing, merging or discarding repetitions.
    // Every list retains its serialized order and occurrence count, including
    // when deciding whether a later plugin differs from either baseline.
    return npc;
}
Plugin readPlugin(const std::filesystem::path& path,std::string name,double* hashMilliseconds) {
    Plugin plugin; plugin.name=lower(name); plugin.path=path;
    require(lower(path.filename().string())==plugin.name,"Manifest name/path mismatch");
    PluginReader stream(path);
    auto length=stream.size(); require(length>=24,"Missing TES4 header");
    auto head=readAt(stream,0,24); require(tag(head)=="TES4","First record is not TES4");
    auto size=u32(head,4); plugin.flags=u32(head,8);
    require(size<=length-24,"TES4 exceeds file boundary");
    auto data=payload(stream,24,size,plugin.flags);
    for(auto& sub:subrecords(data)) if(sub.type=="MAST") plugin.masters.push_back(lower(cstring(sub.data)));
    require(plugin.masters.size()<256,"Master table exceeds the serialized 8-bit index range");
    auto names=plugin.masters; names.push_back(plugin.name);
    std::vector<std::uint64_t> ends{length}; auto position=std::uint64_t(24)+size;
    while(position<length) {
        while(ends.size()>1 && position==ends.back()) ends.pop_back();
        require(ends.back()-position>=24,"Truncated group/record header");
        head=readAt(stream,position,24); auto type=tag(head); size=u32(head,4);
        if(type=="GRUP") {
            require(size>=24 && size<=ends.back()-position,"Invalid group boundary");
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
                Npc npc;
                try {data=payload(stream,position+24,size,flags);npc=parseNpcRecord(data,key,plugin.name,names,flags);}
                catch(const std::exception& e) {
                    npc.key=key;npc.plugin=plugin.name;npc.fields=emptyFields();npc.observed=Json::object();
                    npc.deleted=(flags&0x20)!=0;npc.ignored=(flags&0x1000)!=0;
                    for(const auto& f:fieldNames)npc.inputErrors[f]="plugin="+plugin.name+"; npc="+key+
                        "; file="+path.string()+"; file_offset="+std::to_string(position)+"; reason="+e.what();
                }
                // The last serialized definition is the one the game loaded.
                plugin.npcs.insert_or_assign(key,std::move(npc));
            }
        }
        position+=24+size;
    }
    require(position==length,"File boundary mismatch");
    if(!plugin.npcs.empty()) {
        const auto started=std::chrono::steady_clock::now();
        plugin.sha256=hashFile(path);
        if(hashMilliseconds) *hashMilliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    }
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
    const std::set<std::string> keys{"schema","mode","fields","only_npcs","baseline","debug"};
    for(auto& [key,_]:config.items()) require(keys.contains(key),"Unknown configuration key: "+key);
    require(!config.contains("debug") || config.at("debug").is_boolean(),"debug must be true or false");
    auto mode=config.at("mode").get<std::string>(); require(mode=="audit"||mode=="apply","mode must be audit or apply");
    auto baseline=lower(config.value("baseline",std::string()));
    require(baseline.empty() || ((baseline.ends_with(".esp")||baseline.ends_with(".esm")||baseline.ends_with(".esl")) && baseline.find_first_of("\\/:")==std::string::npos),"baseline must be a plugin filename or an empty string");
    require(config.at("fields").is_array(),"fields must be an array");
    std::set<std::string> fields;
    for(auto& f:config.at("fields")) {
        auto name=f.get<std::string>(); require(contains(fieldNames,name)||protectedField(name),"Unknown writable field: "+name);
        fields.insert(name);
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
Json makePlan(std::vector<Plugin> plugins,const Json& config,const Sequence& exclusions) {
    validateConfig(config);
    // COED's second word is either a signed faction rank or a file-local global
    // reference. Resolve that union using the complete loaded form-type table.
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
        std::map<std::string,Json> inputErrors;
        for(const auto* npc:chain)for(const auto& [field,error]:npc->inputErrors.items())if(enabled.contains(field)) {
            if(!inputErrors.contains(field))inputErrors[field]=Json::array();
            inputErrors[field].push_back(error);
        }
        std::map<std::string,const Npc*> selected;
        for(auto npc:chain) {
            if(npc->deleted || npc==chain.front() || npc==baseline || official.contains(npc->plugin)) continue;
            for(auto& field:enabled) {
                if(inputErrors.contains(field))continue;
                const auto& value=npc->fields.at(field);
                if(value!=baseline->fields.at(field) && value!=originalBaseline->fields.at(field)) selected[field]=npc;
            }
        }
        // A configured baseline also supplies the fallback when its correction
        // was reverted to the official value. A nonbaseline candidate still wins.
        if(customBaseline && !baseline->deleted) for(const auto& field:enabled) {
            if(inputErrors.contains(field))continue;
            if(baseline->fields.at(field)!=originalBaseline->fields.at(field) &&
               final->fields.at(field)==originalBaseline->fields.at(field))
                selected.try_emplace(field,baseline);
        }
        std::string reason;
        for(auto& entry:exclusionReport) {
            auto rule=entry.at("rule").get<std::string>();
            if(rule==key || (rule.starts_with("editorid:") && lower(final->editor)==rule.substr(9))) {
                entry["matched_npcs"].push_back(key); reason="Excluded by "+rule;
            }
        }

        if(key=="skyrim.esm:000007") reason="Player base record is always excluded";
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
                {"status",fieldReason.empty()?"ready":fieldReason.starts_with("Invalid ")||fieldReason.starts_with("Unparsed ")||fieldReason.starts_with("Container ownership")?"selection_error":"skipped"},{"reason",fieldReason}};
            if(fieldReason.empty()) {++applicable;++eligibleFields;changes+=differs;} else ++skippedFields;
        }
        for(const auto& [field,errors]:inputErrors) {
            fields[field]={{"baseline",nullptr},{"official_baseline",nullptr},{"disk",nullptr},{"expected",nullptr},
                {"sources",Json::array()},{"differs",false},{"status","input_error"},
                {"reason","Incomplete field history; no replacement value was selected"},{"input_errors",errors}};
            ++skippedFields;
        }
        auto status=excluded?"excluded":!reason.empty()?"skipped":fields.empty()?"no_source_changes":applicable?"ready":"skipped";
        if(reason.empty() && !fields.empty() && !applicable) reason="No applicable selected fields; see field reasons";
        Json row={{"npc",key},{"editor_id",final->editor},{"winner",final->plugin},{"baseline_plugin",baseline->plugin},
            {"baseline_kind",customBaseline?"configured_plugin":officialBaseline?"official":"first_definition"},
            {"official_baseline_plugin",originalBaseline->plugin},{"official_baseline_kind",officialBaseline?"official":"first_definition"},
            {"status",status},{"reason",reason},{"observed_only",final->observed},
            {"inheritance_record",{{"flags",final->templateFlags},{"fields",final->fields},{"writable",!excluded&&!final->deleted&&!baseline->deleted}}}};
        if(!inputErrors.empty())row["input_errors"]=inputErrors;
        row["fields"]=std::move(fields);rows.push_back(std::move(row));
        if(status==std::string("skipped")||excluded) ++skipped;
        else if(applicable) {++ready;if(changes) {++changedNpcs;changedFields+=changes;}}

    }
    Json result={{"schema",2},{"debug",config.value("debug",false)},{"strategy","last_nonbaseline_field"},{"baseline_comparison","official_and_configured"},{"configuration_warnings",configurationWarnings(config)},{"mode",config.at("mode")},
        {"runtime_adapter_limits",Json::array({{{"field","Destructible"},{"reason","Tracked, but destruction model/preload ownership is not yet implemented for runtime writes"}}})},{"baseline",baselineInfo},{"enabled_fields",enabled},{"requested_fields",config.at("fields")},{"selected_sources",sourceNames},{"exclusions",exclusionReport},{"plugins",evidence},
        {"summary",{{"scanned_npcs",rows.size()},{"eligible_npcs",ready},{"npcs_needing_changes",changedNpcs},{"fields_needing_changes",changedFields},{"skipped_or_excluded_npcs",skipped},{"eligible_fields",eligibleFields},{"skipped_fields",skippedFields}}}};
    result["npcs"]=std::move(rows);return result;
}
Json run(const Json& manifest,const Json& config,const Sequence& exclusions,RunTimings* timings,
    const std::function<void(const Plugin&)>& observer) {
    using Clock=std::chrono::steady_clock;
    if(timings) *timings={};
    validateConfig(config);
    require(manifest.at("schema")==1,"Unsupported manifest schema");
    const auto& entries=manifest.at("plugins");
    std::vector<Plugin> plugins(entries.size());
    std::vector<std::exception_ptr> failures(entries.size());
    std::vector<double> readTimes(entries.size()),hashTimes(entries.size()),observerTimes(entries.size());
    std::atomic_size_t next{0};
    const unsigned workers=static_cast<unsigned>(std::min<std::size_t>(entries.size(),std::clamp(std::thread::hardware_concurrency(),1u,4u)));
    const auto readStarted=Clock::now();
    const auto parse=[&] {
        for(auto i=next.fetch_add(1);i<entries.size();i=next.fetch_add(1)) {
            const auto started=Clock::now();
            try {
                const auto& p=entries.at(i);
                auto utf8=p.at("path").get<std::string>();
                auto path=std::filesystem::path(std::u8string(utf8.begin(),utf8.end()));
                plugins[i]=readPlugin(path,p.at("name").get<std::string>(),&hashTimes[i]);
                if(observer) {const auto observed=Clock::now();observer(plugins[i]);observerTimes[i]=std::chrono::duration<double,std::milli>(Clock::now()-observed).count();}
            } catch(...) {failures[i]=std::current_exception();}
            readTimes[i]=std::chrono::duration<double,std::milli>(Clock::now()-started).count()-hashTimes[i]-observerTimes[i];
        }
    };
    {
        // Workers touch only independent disk inputs. Slots retain load order;
        // no game objects, shared plan mutation, or engine calls run here.
        std::vector<std::jthread> threads;
        for(unsigned i=1;i<workers;++i) threads.emplace_back(parse);
        parse();
    }
    for(std::size_t i=0;i<failures.size();++i) if(failures[i]) {
        try {std::rethrow_exception(failures[i]);}
        catch(const std::exception& e) {throw std::runtime_error(entries.at(i).at("name").get<std::string>()+": file="+entries.at(i).at("path").get<std::string>()+"; phase=disk_input; reason="+e.what());}
    }
    const auto planStarted=Clock::now();
    if(timings) {
        timings->pluginReadWallMilliseconds=std::chrono::duration<double,std::milli>(planStarted-readStarted).count();timings->parseWorkers=workers;
        for(std::size_t i=0;i<entries.size();++i) {timings->readParseMilliseconds+=readTimes[i];timings->hashMilliseconds+=hashTimes[i];timings->observerMilliseconds+=observerTimes[i];}
    }
    auto result=makePlan(std::move(plugins),config,exclusions);
    if(timings) timings->planMilliseconds=std::chrono::duration<double,std::milli>(Clock::now()-planStarted).count();
    return result;
}
}
