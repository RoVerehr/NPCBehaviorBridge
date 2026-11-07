#include "nbb/load_capture.hpp"
#include "nbb/load_stream.hpp"
#include "nbb/core.hpp"
#include "nbb/field_catalog.hpp"
#include "nbb/inheritance.hpp"
#include "nbb/runtime_normalization.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_set>
#include <thread>
#include <charconv>
namespace nbb::load_capture {
namespace {
constexpr std::size_t packageLimit=64,factionLimit=128;
constexpr std::array<const char*,10> refNames{"Template","Class","CombatStyle","DeathItem","Race","SPOR","DPLT","ECOR","OCOR","GWOR"};
constexpr std::array<std::size_t,10> refOffsets{0x60,0x1C0,0x1D8,0x50,0x158,0x168,0x228,0x180,0x170,0x178};
struct FileContext {
    std::uint64_t address{};std::array<char,260> name{};
    std::uint32_t count{},selfPrefix{};bool valid{};
    std::array<std::uint32_t,256> masters{};std::array<bool,256> masterValid{};
};
struct Ref { std::uint64_t raw{};std::uint32_t observedID{};std::uint8_t observedType{};bool readable{}; };
struct Faction { Ref ref;std::int8_t rank{}; };
static_assert(sizeof(RE::FACTION_RANK)==16&&sizeof(RE::BSTArray<RE::FACTION_RANK>)==24);
struct Snapshot {
    bool readable{};std::array<std::uint8_t,0x268> bytes{};
    std::array<Ref,refNames.size()> refs{};
    std::array<Ref,packageLimit> packages{};
    std::uint32_t packageCount{};bool packageReadFailed{},packageTruncated{};
    std::array<Faction,factionLimit> factions{};std::uint32_t factionCount{};
    bool factionReadFailed{},factionTruncated{};
};

std::atomic<bool> active{false};
using LoadFn=bool(*)(RE::TESNPC*,RE::TESFile*);
using InitFn=void(*)(RE::TESNPC*);
LoadFn originalLoad{};InitFn originalInit{};
using LookupFn=RE::TESForm*(*)(std::uint32_t);
LookupFn lookup=+[](std::uint32_t id){return RE::TESForm::LookupByID(id);};
Json captureConfig,earlyResults=Json::array();Sequence captureExclusions;
std::atomic<DWORD> initThread{0};bool nativePruningAllowed=false;
using Clock=std::chrono::steady_clock;
std::atomic<std::uint64_t> loadOwnNs{0},initOwnNs{0};
std::uint64_t planNs{},applyNs{},finishNs{};
struct OwnTimer {
    std::atomic<std::uint64_t>& total;Clock::time_point began=Clock::now();std::uint64_t native{};
    template<class F> auto invoke(F&& f) {
        auto start=Clock::now();
        if constexpr(std::is_void_v<std::invoke_result_t<F>>) {f();native+=static_cast<std::uint64_t>((Clock::now()-start).count());}
        else {auto result=f();native+=static_cast<std::uint64_t>((Clock::now()-start).count());return result;}
    }
    ~OwnTimer(){total.fetch_add(static_cast<std::uint64_t>((Clock::now()-began).count())-native,std::memory_order_relaxed);}
};
static_assert(Clock::period::num==1&&Clock::period::den==1000000000);

void applyAll(RE::TESNPC*) noexcept;
std::atomic<bool> streamEnabled{false},streamFrozen{false};
std::atomic<unsigned> streamInFlight{0},streamLateLoads{0};
struct StreamFlight {bool enabled=streamEnabled.load();StreamFlight(){if(enabled)++streamInFlight;}~StreamFlight(){if(enabled)--streamInFlight;}};
load_stream::Payload captureStreamSafe(RE::TESFile*) noexcept;
void collectStream(RE::TESNPC*,RE::TESFile*,bool,load_stream::Payload) noexcept;
Sequence earlyFields() {
    return {"PKID","CombatStyle","SPOR","ECOR","OCOR","GWOR","Aggression","Confidence","Assistance","Energy","Morality","Mood","AggroRadiusBehavior","Protected","Factions",
        "Template","UseStats","UseFactions","UseSpellList","UseAIData","UseAIPackages","UseInventory","UseScript","UseDefPackList","UseAttackData","UseKeywords"};
}
int referenceIndex(std::string_view field) {for(unsigned i=0;i<refNames.size();++i)if(field==refNames[i])return static_cast<int>(i);return -1;}
const ScalarField* earlyScalar(std::string_view name) {
    static const std::array<ScalarField,5> extra{{
        {"Aggression","AIDT",0,1,0xB8,4,0,3,0},
        {"Confidence","AIDT",1,1,0xB8,4,0,28,2},
        {"Assistance","AIDT",5,1,0xB8,4,0,0xC0000,18},
        {"Protected","ACBS",0,4,0x38,4,0,0x800,11,false,true},
        {"UseAIPackages","ACBS",18,2,0x4A,2,0,32,5,false,true}}};
    for(const auto& f:extra)if(name==f.name)return &f;
    return scalarField(name);
}

bool read(std::uintptr_t source,void* target,std::size_t size) noexcept {
    SIZE_T got=0;
    return source && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(source),target,size,&got) && got==size;
}
template<class T> T value(const Snapshot& s,std::size_t offset) noexcept {
    T result{};std::memcpy(&result,s.bytes.data()+offset,sizeof(T));return result;
}
Json scalar(const Snapshot& s,const ScalarField& f) {
    std::uint32_t word{};std::memcpy(&word,s.bytes.data()+f.runtimeOffset,f.runtimeBytes);return decodeScalarWord(f,word);
}
bool prefix(RE::TESFile* file,std::uint32_t& out) noexcept {
    std::uint8_t full{};std::uint16_t light{};std::uint32_t flags{};const auto p=reinterpret_cast<std::uintptr_t>(file);
    if(!read(p+offsetof(RE::TESFile,compileIndex),&full,sizeof(full))||full==0xFF||
       !read(p+offsetof(RE::TESFile,smallFileCompileIndex),&light,sizeof(light))||
       !read(p+offsetof(RE::TESFile,recordFlags),&flags,sizeof(flags)))return false;
    if(flags&0x200){if(light>0xFFF)return false;out=0xFE000000u|(std::uint32_t(light)<<12);}
    else out=std::uint32_t(full)<<24;
    return true;
}
FileContext context(RE::TESFile* file) noexcept {
    FileContext c;c.address=reinterpret_cast<std::uintptr_t>(file);if(!file)return c;
    std::uintptr_t pointers{};
    if(!read(c.address+offsetof(RE::TESFile,fileName),c.name.data(),c.name.size())||
       !read(c.address+offsetof(RE::TESFile,masterCount),&c.count,sizeof(c.count))||c.count>255||
       !read(c.address+offsetof(RE::TESFile,masterPtrs),&pointers,sizeof(pointers))||!prefix(file,c.selfPrefix))return c;
    c.name.back()=0;c.valid=true;
    for(std::uint32_t i=0;i<c.count;++i){RE::TESFile* master{};
        if(read(pointers+sizeof(void*)*i,&master,sizeof(master)))c.masterValid[i]=prefix(master,c.masters[i]);}
    return c;
}
bool mapID(std::uint64_t raw,const FileContext& c,std::uint32_t& id) noexcept {
    if(raw>0xFFFFFFFFULL)return false;
    if(raw<=0x7FF){id=static_cast<std::uint32_t>(raw);return true;}
    if(!c.valid)return false;
    auto index=static_cast<std::uint32_t>(raw>>24);if(index>c.count)return false;
    if(index<c.count&&!c.masterValid[index])return false;
    const auto p=index<c.count?c.masters[index]:c.selfPrefix;
    id=p|(static_cast<std::uint32_t>(raw)&((p>>24)==0xFE?0xFFF:0xFFFFFF));return true;
}
RE::TESFile* lastFile(RE::TESNPC* npc) noexcept {
    // Guarded equivalent of TESForm::GetFile(-1); no engine getter or file seek.
    std::uintptr_t array{},data{};std::uint32_t size{};RE::TESFile* file{};
    if(!read(reinterpret_cast<std::uintptr_t>(npc)+0x08,&array,8)||!array||!read(array,&data,8)||
       !read(array+8,&size,4)||!size||size>4096)return nullptr;
    read(data+sizeof(void*)*(size-1),&file,sizeof(file));return file;
}
Ref reference(std::uint64_t raw) noexcept {
    Ref r;r.raw=raw;
    // Load-time references can still be encoded IDs. Never call TESForm methods
    // on them. These header observations are evidence only, not resolved identities.
    std::array<std::uint8_t,0x20> header{};
    if(raw>0xFFFFFFFFULL && read(raw,header.data(),header.size())) {
        r.readable=true;std::memcpy(&r.observedID,header.data()+0x14,4);r.observedType=header[0x1A];
    }
    return r;
}
Snapshot capture(RE::TESNPC* npc) noexcept {
    Snapshot s;s.readable=read(reinterpret_cast<std::uintptr_t>(npc),s.bytes.data(),s.bytes.size());
    if(!s.readable)return s;
    for(std::size_t i=0;i<refOffsets.size();++i)s.refs[i]=reference(value<std::uint64_t>(s,refOffsets[i]));
    const auto factionData=value<std::uint64_t>(s,0x70);const auto factionSize=value<std::uint32_t>(s,0x80);
    s.factionTruncated=factionSize>factionLimit;
    for(unsigned i=0;i<std::min<std::size_t>(factionSize,factionLimit);++i){
        std::array<std::uint8_t,16> bytes{};
        if(!factionData||!read(factionData+i*16,bytes.data(),bytes.size())){s.factionReadFailed=true;break;}
        std::uint64_t raw{};std::memcpy(&raw,bytes.data(),8);auto& f=s.factions[s.factionCount++];f.ref=reference(raw);std::memcpy(&f.rank,bytes.data()+8,1);
    }
    // Embedded BSSimpleList head. Bound traversal and use guarded reads because
    // these are intermediate loader states, not initialized runtime records.
    auto item=value<std::uint64_t>(s,0xC8),next=value<std::uint64_t>(s,0xD0);
    for(std::size_t n=0;n<packageLimit;++n) {
        if(item)s.packages[s.packageCount++]=reference(item);
        if(!next)return s;
        if(n+1==packageLimit){s.packageTruncated=true;return s;}
        std::array<std::uint64_t,2> node{};
        if(!read(next,node.data(),sizeof(node))){s.packageReadFailed=true;return s;}
        item=node[0];next=node[1];
    }
    return s;
}

bool load(RE::TESNPC* npc,RE::TESFile* file) {
    if(!active.load(std::memory_order_relaxed))return originalLoad(npc,file);
    OwnTimer own{loadOwnNs};StreamFlight flight;
    auto payload=flight.enabled?captureStreamSafe(file):load_stream::Payload{};
    const auto result=own.invoke([&]{return originalLoad(npc,file);});
    if(flight.enabled)collectStream(npc,file,result,std::move(payload));
    return result;
}
void init(RE::TESNPC* npc) {
    if(!active.load(std::memory_order_relaxed)){originalInit(npc);return;}
    OwnTimer own{initOwnNs};own.invoke([&]{originalInit(npc);});applyAll(npc);
}
void need(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
struct EarlyEnvironment {
    std::vector<Plugin> plugins;
    std::map<std::string,std::size_t> pluginIndices;
    std::map<std::uint32_t,std::string> origins;
    std::map<std::string,std::uint32_t> ids;
    Json key(std::uint32_t id) {
        if(!id)return nullptr;
        const bool light=(id>>24)==0xFE;const auto prefix=id&(light?0xFFFFF000u:0xFF000000u);
        need(origins.contains(prefix),"Unknown early reference origin");
        auto result=formKey(id&(light?0xFFF:0xFFFFFF),{origins.at(prefix)});ids[result]=id;return result;
    }
};
EarlyEnvironment environment() {
    EarlyEnvironment env;auto* data=RE::TESDataHandler::GetSingleton();need(data,"Early data handler unavailable");
    std::set<const RE::TESFile*> loaded;
    auto remember=[&](const RE::TESFile* file){loaded.insert(file);env.origins[file->IsLight()?0xFE000000u|(std::uint32_t(file->smallFileCompileIndex)<<12):std::uint32_t(file->compileIndex)<<24]=lower(std::string(file->GetFilename()));};
    for(auto* f:data->compiledFileCollection.files)remember(f);
    for(auto* f:data->compiledFileCollection.smallFiles)remember(f);
    for(auto* f:data->files)if(f&&loaded.contains(f)){Plugin p;p.name=lower(std::string(f->GetFilename()));env.pluginIndices[p.name]=env.plugins.size();env.plugins.push_back(std::move(p));}
    need(env.plugins.size()==loaded.size(),"Incomplete early plugin order");return env;
}

// Raw inputs were verified against disk in ALL3. ALL4 builds the full plan once
// here, shares its 26 supported early fields, then moves it to DataLoaded.
struct StreamRecord {load_stream::Payload payload;std::string key;bool nativeSuccess{};};
struct StreamFile {FileContext context;Sequence names;std::map<std::uint32_t,StreamRecord> records;};
std::map<std::string,StreamFile> streamFiles;
std::mutex streamMutex;
std::unique_ptr<EarlyEnvironment> streamEnvironment;
bool streamReady=false,streamReused=false;
std::string streamBuildError;
std::size_t streamParsedRecords{},streamInputNpcs{};
Json streamEvidence=Json::array();
Sequence streamPluginOrder;
double streamParseMs{},streamTypesMs{},streamPlanMs{};
std::atomic<std::size_t> streamBytes{0},streamCalls{0};
std::size_t streamFailures{},streamDuplicates{};
Json streamIssues=Json::array();
std::map<std::string,unsigned> streamSources;
std::unordered_set<DWORD> streamThreads;
std::atomic<std::uint64_t> streamCaptureNs{0};
constexpr std::size_t streamByteLimit=256*1024*1024,streamRecordLimit=250000;
load_stream::Payload captureStreamSafe(RE::TESFile* file) noexcept {
    const auto started=Clock::now();
    struct Timer {Clock::time_point began;~Timer(){streamCaptureNs.fetch_add((Clock::now()-began).count());}} timer{started};
    try {
        if(streamBytes.load()>=streamByteLimit || streamCalls.load()>=streamRecordLimit) {
            load_stream::Payload result;result.error="Full capture memory/record limit";return result;
        }
        return load_stream::capture(reinterpret_cast<std::uintptr_t>(file),read);
    }catch(const std::exception& e){load_stream::Payload result;result.error=e.what();return result;}
     catch(...){return {};}
}
void collectStream(RE::TESNPC* npc,RE::TESFile* file,bool success,load_stream::Payload payload) noexcept {
    const auto started=Clock::now();
    struct Timer {Clock::time_point began;~Timer(){streamCaptureNs.fetch_add((Clock::now()-began).count());}} timer{started};
    ++streamCalls;
    try {
        // A compressed buffer can be created lazily by native Load. Retry only
        // observation after it returns; never request decompression ourselves.
        if(!payload.complete && streamBytes.load()<streamByteLimit && streamCalls.load()<=streamRecordLimit)payload=load_stream::capture(reinterpret_cast<std::uintptr_t>(file),read);
        RE::FORM current{};need(read(reinterpret_cast<std::uintptr_t>(file)+offsetof(RE::TESFile,currentform),&current,sizeof(current)),"Full capture header unreadable");
        auto ctx=context(file);need(ctx.valid,"Full capture context unavailable");
        const auto name=lower(std::string(ctx.name.data()));
        std::lock_guard lock(streamMutex);
        streamThreads.insert(GetCurrentThreadId());
        auto issue=[&](const std::string& error){++streamFailures;if(streamIssues.size()<100)streamIssues.push_back({{"plugin",name},{"form_id",current.formID},{"reason",error}});};
        if(streamFrozen.load()){++streamLateLoads;issue("Load completed after full capture freeze");return;}
        if(!payload.complete){issue(payload.error.empty()?"Full capture incomplete":payload.error);return;}
        if(payload.id!=current.formID || payload.flags!=current.flags || payload.recordSize!=current.length ||
            (!success && !(payload.flags&(0x20|0x1000))) || (success && (!npc||npc->formID!=payload.id))) {
            issue("Full capture identity or native Load result mismatch");return;
        }
        if(streamCalls.load()>streamRecordLimit || payload.bytes.size()>streamByteLimit-streamBytes.load()) {issue("Full capture memory/record limit");return;}
        auto& dest=streamFiles[name];if(dest.records.empty())dest.context=ctx;
        if(dest.records.contains(payload.id)){++streamDuplicates;issue("Duplicate full NPC record in plugin");return;}
        streamBytes.fetch_add(payload.bytes.size());++streamSources[payload.source];
        dest.records.emplace(payload.id,StreamRecord{std::move(payload),{},success});
    }catch(const std::exception& e){std::lock_guard lock(streamMutex);++streamFailures;if(streamIssues.size()<100)streamIssues.push_back({{"reason",e.what()}});}
     catch(...){std::lock_guard lock(streamMutex);++streamFailures;}
}
void prepareStreamInput(EarlyEnvironment seed) {
    streamFrozen.store(true);
    std::lock_guard lock(streamMutex);
    need(streamInFlight.load()==0,"Native Load still in flight at DataLoaded; full capture audit disabled");
    streamEnvironment=std::make_unique<EarlyEnvironment>(std::move(seed));
    auto& env=*streamEnvironment;
    streamPluginOrder.clear();for(const auto& p:env.plugins)streamPluginOrder.push_back(p.name);
    for(auto& [name,file]:streamFiles) {
        need(env.pluginIndices.contains(name),"Full capture plugin absent from loaded order");
        for(unsigned i=0;i<file.context.count;++i) {
            need(file.context.masterValid[i]&&env.origins.contains(file.context.masters[i]),"Full capture master missing");
            file.names.push_back(env.origins.at(file.context.masters[i]));
        }
        env.plugins.at(env.pluginIndices.at(name)).masters=file.names;
        file.names.push_back(name);
        for(auto& [id,record]:file.records)record.key=env.key(id).get<std::string>();
    }
    need(streamFailures==0 && streamDuplicates==0,"Incomplete full record history");
}
std::string fullRuntimeType(std::uint32_t id) {
    auto* form=lookup(id);return !form||form->formID!=id?"INVALID":form->IsDeleted()?"DELETED":std::string(RE::FormTypeToString(form->GetFormType()));
}
void populateStreamTypes(EarlyEnvironment& env,const std::function<std::string(std::uint32_t)>& typeOf) {
    std::map<std::string,std::uint32_t> prefixes;for(const auto& [prefix,name]:env.origins)prefixes[name]=prefix;
    auto& types=env.plugins.back().types;
    std::function<void(const Json&)> references=[&](const Json& value) {
        if(value.is_structured()){for(const auto& child:value)references(child);return;}
        if(!value.is_string())return;
        const auto& key=value.get_ref<const std::string&>();auto colon=key.rfind(':');
        if(colon==std::string::npos||key.size()-colon!=7||types.contains(key))return;
        auto origin=prefixes.find(key.substr(0,colon));if(origin==prefixes.end())return;
        std::uint32_t local{};auto parsed=std::from_chars(key.data()+colon+1,key.data()+key.size(),local,16);
        if(parsed.ec!=std::errc()||parsed.ptr!=key.data()+key.size())return;
        const auto prefix=origin->second;
        if((prefix>>24)==0xFE && local>0xFFF){types[key]="INVALID";return;}
        const auto id=prefix|local;env.ids[key]=id;
        types[key]=typeOf(id);
    };
    for(const auto& plugin:env.plugins)for(const auto& [key,npc]:plugin.npcs)references(npc.fields);
 }
Json createStreamPlan(EarlyEnvironment& env,const std::function<std::string(std::uint32_t)>& typeOf) {
    const auto began=Clock::now();
    std::vector<const StreamFile*> inputs;std::vector<std::size_t> indices;
    for(const auto& [name,input]:std::as_const(streamFiles)){inputs.push_back(&input);indices.push_back(env.pluginIndices.at(name));}
    std::vector<std::exception_ptr> failures(inputs.size());std::vector<std::size_t> counts(inputs.size());
    std::atomic_size_t next{0};
    const auto parse=[&] {
        for(auto i=next.fetch_add(1);i<inputs.size();i=next.fetch_add(1))try {
            const auto& input=*inputs[i];auto& plugin=env.plugins.at(indices[i]);
            for(const auto& [id,record]:input.records) {
                need(record.payload.complete,"Incomplete captured record");
                auto npc=parseNpcRecord(record.payload.bytes,record.key,plugin.name,input.names,record.payload.flags);
                need(plugin.npcs.emplace(record.key,std::move(npc)).second,"Duplicate captured NPC");++counts[i];
            }
        }catch(...){failures[i]=std::current_exception();}
    };
    {std::vector<std::jthread> threads;const auto workers=std::min<std::size_t>(inputs.size(),std::clamp(std::thread::hardware_concurrency(),1u,4u));
        for(unsigned i=1;i<workers;++i)threads.emplace_back(parse);parse();}
    for(const auto& failure:failures)if(failure)std::rethrow_exception(failure);
    for(auto n:counts)streamParsedRecords+=n;
    const auto typed=Clock::now();streamParseMs=std::chrono::duration<double,std::milli>(typed-began).count();
    populateStreamTypes(env,typeOf);
    const auto planned=Clock::now();streamTypesMs=std::chrono::duration<double,std::milli>(planned-typed).count();
    streamEvidence=Json::array();for(const auto& p:env.plugins)if(!p.npcs.empty())streamEvidence.push_back({{"plugin",p.name},{"npc_records",p.npcs.size()},{"input","engine_load_capture"}});
    auto result=makePlan(std::move(env.plugins),captureConfig,captureExclusions);
    streamPlanMs=std::chrono::duration<double,std::milli>(Clock::now()-planned).count();streamInputNpcs=result.at("npcs").size();
    // Provenance describes what was actually used; no invented disk hash.
    result["plugins"]=streamEvidence;result["input_source"]="engine_load_capture";
    return result;
}
void validateParent(RE::TESNPC* child,RE::TESForm* parent) {
    std::set<std::uint32_t> seen{child->formID};unsigned depth=0;
    while(parent){need(++depth<=128,"Early template chain too deep");need(parent->GetFormType()==RE::FormType::NPC&&!parent->IsDeleted(),"Early template must be a nondeleted NPC");
        need(seen.insert(parent->formID).second,"Early template cycle");auto* p=static_cast<RE::TESNPC*>(parent);const auto s=capture(p);need(s.readable,"Early parent unreadable");const auto& r=s.refs[0];
        if(!r.raw)break;
        if(r.readable){parent=lookup(r.observedID);need(reinterpret_cast<std::uintptr_t>(parent)==r.raw,"Early template pointer mismatch");}
        else {std::uint32_t id{};need(mapID(r.raw,context(lastFile(p)),id),"Early parent reference unresolved");parent=lookup(id);need(parent,"Early parent missing");}
    }
}
void earlyApplyImpl(RE::TESNPC* npc,EarlyEnvironment& env,const Json& input) {
    const auto key=env.key(npc->formID).get<std::string>();Json result={{"npc",key},{"status","skipped"}};
    auto store=[&](const char* reason){result["reason"]=reason;earlyResults.push_back(result);if(captureConfig.value("debug",false))SKSE::log::info("Early recovery {}: {}",key,reason);};
    if(key=="skyrim.esm:000007"){store("Player excluded");return;}
    if(captureConfig.value("mode",std::string())!="apply"){store("Audit mode; no early writes");return;}
    if(!captureConfig.at("only_npcs").empty()){bool allowed=false;for(const auto& k:captureConfig.at("only_npcs"))allowed|=parseExclusions(k.get<std::string>()).at(0)==key;if(!allowed){store("Outside requested NPC scope");return;}}
    const auto before=capture(npc);need(before.readable,"Early pre-write snapshot failed");
    if(value<std::uint16_t>(before,0x4A)&0x8000){store("Template copying already occurred; regular recovery retained");return;}
    const auto* editor=npc->GetFormEditorID();
    if((!editor||!*editor)&&std::any_of(captureExclusions.begin(),captureExclusions.end(),[](const auto& x){return x.starts_with("editorid:");})){store("EditorID exclusion cannot be checked early; regular recovery retained");return;}
    auto row=input;
    if(row.at("status")!="ready"){store("Selection not ready or excluded");return;}
    need(lower(std::string(context(lastFile(npc)).name.data()))==row.at("winner").get<std::string>(),"Early winner/source context mismatch");
        bool deferTemplateGroup=false;
        if(row["fields"].contains("Template")) {
            const auto& spec=row["fields"]["Template"];
            deferTemplateGroup=spec.at("status")!="ready"||templateSharedWithProtectedData(value<std::uint16_t>(before,0x4A));
            if(!deferTemplateGroup&&spec.at("expected").is_null()&&!spec.at("disk").is_null()) {
                deferTemplateGroup=true;result["deferred_template_reason"]="Template detachment remains in regular recovery";
            }
            if(!deferTemplateGroup&&!spec.at("expected").is_null())try{auto* parent=lookup(env.ids.at(spec.at("expected").get<std::string>()));need(parent,"Early template missing");validateParent(npc,parent);}
            catch(const std::exception& e){deferTemplateGroup=true;result["deferred_template_reason"]=e.what();}
        }
        for(auto it=row["fields"].begin();it!=row["fields"].end();) {
            if(it.value().at("status")!="ready"||(deferTemplateGroup&&(it.key()=="Template"||isTemplateField(it.key()))))it=row["fields"].erase(it);else ++it;
        }
        if(row["fields"].empty()){store("No safe early fields; regular recovery retained");return;}
    Json actual=Json::object(),desired=Json::object();
    auto runtimeRef=[&](const Ref& r)->Json {if(!r.raw)return nullptr;need(r.readable,"Field has not been resolved by InitItem");auto* f=lookup(r.observedID);need(reinterpret_cast<std::uintptr_t>(f)==r.raw&&!f->IsDeleted(),"Untrusted early runtime reference");return env.key(r.observedID);};
    auto fieldValue=[&](const Snapshot& s,const std::string& f)->Json {
        if(f=="PKID"){need(!s.packageReadFailed&&!s.packageTruncated,"Runtime package snapshot incomplete");Json a=Json::array();for(unsigned i=0;i<s.packageCount;++i)a.push_back(runtimeRef(s.packages[i]));return a;}
        if(f=="Factions"){need(!s.factionReadFailed&&!s.factionTruncated,"Runtime faction snapshot incomplete");Json a=Json::array();for(unsigned i=0;i<s.factionCount;++i){auto key=runtimeRef(s.factions[i].ref);need(!key.is_null(),"Null runtime faction");a.push_back(Json::array({key,int(s.factions[i].rank)}));}std::sort(a.begin(),a.end());return a;}
        if(auto ri=referenceIndex(f);ri>=0)return runtimeRef(s.refs[ri]);
        const auto* sf=earlyScalar(f);need(sf,"Unknown early scalar read");return scalar(s,*sf);
    };
    std::map<std::string,RE::TESForm*> pointers;
    std::map<std::size_t,unsigned> wordWrites;
    auto encoded=before.bytes;std::array<std::uint8_t,0x268> allowedBits{};
    using List=RE::BSSimpleList<RE::TESPackage*>;List replacement,oldPackages;bool packagesChanged=false,factionsChanged=false;
    RE::BSTArray<RE::FACTION_RANK> newFactions,oldFactions;
    auto allow=[&](std::size_t start,std::size_t size){std::fill_n(allowedBits.begin()+start,size,0xFF);};
    for(auto& [f,spec]:row.at("fields").items()){
        if(spec.at("status")!="ready"){store("Candidate contains skipped fields; transaction not applied");return;}
        actual[f]=fieldValue(before,f);
        bool allowed=permitsValue(actual[f],spec);
        if(!allowed&&f=="CombatStyle"){auto* m=RE::BGSDefaultObjectManager::GetSingleton();auto* d=m?m->objects[std::to_underlying(RE::DEFAULT_OBJECT::kCombatStyle)]:nullptr;
            if(d&&d->GetFormType()==RE::FormType::CombatStyle&&!d->IsDeleted())allowed=isDefaultCombatStyleFill(f,spec.at("disk"),actual[f],env.key(d->formID));}
        if(!allowed){result["runtime_before"]=actual;store("Unknown runtime change; regular recovery retained");return;}
        desired[f]=spec.at("expected");
        auto resolve=[&](const Json& v,RE::FormType t){if(v.is_null())return static_cast<RE::TESForm*>(nullptr);auto* p=lookup(env.ids.at(v.get<std::string>()));need(p&&p->GetFormType()==t&&!p->IsDeleted()&&env.key(p->formID)==v,"Early target reference failed validation");return p;};
        if(actual[f]==desired[f])continue;
        if(f=="PKID"){for(auto it=desired[f].rbegin();it!=desired[f].rend();++it){auto* p=resolve(*it,RE::FormType::Package);need(p,"Null early package");replacement.push_front(static_cast<RE::TESPackage*>(p));}packagesChanged=true;allow(0xC8,16);}
        else if(f=="Factions"){need(desired[f].size()<=factionLimit,"Too many early factions");newFactions.reserve(static_cast<std::uint32_t>(desired[f].size()));for(const auto& e:desired[f]){auto* p=resolve(e.at(0),RE::FormType::Faction);need(p,"Null early faction");newFactions.push_back(RE::FACTION_RANK{static_cast<RE::TESFaction*>(p),e.at(1).get<std::int8_t>()});}factionsChanged=true;allow(0x70,sizeof(RE::BSTArray<RE::FACTION_RANK>));}
        else if(auto ri=referenceIndex(f);ri>=0){
            pointers[f]=resolve(desired[f],f=="Template"?RE::FormType::NPC:f=="CombatStyle"?RE::FormType::CombatStyle:RE::FormType::FormList);
            if(f=="Template"){validateParent(npc,pointers[f]);}allow(refOffsets[ri],8);
        } else {const auto* sf=earlyScalar(f);need(sf,"Unknown early scalar write");
            std::uint32_t word{};std::memcpy(&word,encoded.data()+sf->runtimeOffset,sf->runtimeBytes);word=encodeScalarWord(*sf,word,desired[f]);std::memcpy(encoded.data()+sf->runtimeOffset,&word,sf->runtimeBytes);
            wordWrites[sf->runtimeOffset]=sf->runtimeBytes;
            for(unsigned b=0;b<sf->runtimeBytes;++b)allowedBits[sf->runtimeOffset+b]|=sf->runtimeMask?static_cast<std::uint8_t>(sf->runtimeMask>>(8*b)):0xFF;
        }
    }
    result["runtime_before"]=actual;result["expected"]=desired;
    // Allocate every list and validate every reference before touching the NPC.
    // Retain old list ownership until readback succeeds, permitting local rollback.
    if(packagesChanged){oldPackages=std::move(npc->aiPackages.packages);npc->aiPackages.packages=std::move(replacement);}
    if(factionsChanged){oldFactions=std::move(npc->factions);npc->factions=std::move(newFactions);}
    for(const auto& [f,p]:pointers)std::memcpy(reinterpret_cast<std::uint8_t*>(npc)+refOffsets[referenceIndex(f)],&p,sizeof(p));
    for(const auto& [offset,bytes]:wordWrites)std::memcpy(reinterpret_cast<std::uint8_t*>(npc)+offset,encoded.data()+offset,bytes);
    try {
        const auto after=capture(npc);need(after.readable,"Early readback failed");
        for(const auto& [f,v]:desired.items())need(fieldValue(after,f)==v,"Early field readback mismatch");
        for(std::size_t b=0;b<before.bytes.size();++b)need(((before.bytes[b]^after.bytes[b])&~allowedBits[b])==0,"Early protected bits changed");
    }catch(...){
        if(packagesChanged)npc->aiPackages.packages=std::move(oldPackages);
        if(factionsChanged)npc->factions=std::move(oldFactions);
        for(const auto& [f,p]:pointers){auto ri=referenceIndex(f);std::memcpy(reinterpret_cast<std::uint8_t*>(npc)+refOffsets[ri],&before.refs[ri].raw,8);}
        for(const auto& [offset,bytes]:wordWrites)std::memcpy(reinterpret_cast<std::uint8_t*>(npc)+offset,before.bytes.data()+offset,bytes);
        throw std::runtime_error("Early readback/protection failed; transaction fields rolled back");
    }
    unsigned changed=0;for(auto& [f,v]:desired.items())changed+=actual.at(f)!=v;
    result["status"]=changed?"applied_verified":"already_matches";result["changed_fields"]=changed;result["protected_bytes_equal"]=true;result["phase"]="after_native_init_before_template_copy";
    earlyResults.push_back(std::move(result));
}

std::mutex collectionMutex;
std::unordered_set<std::uint32_t> attemptedNPCs;
std::atomic<bool> collectionClosed{false},allDisabled{false};
std::atomic<unsigned> foreignInitThreads{0};
std::unique_ptr<EarlyEnvironment> globalEnvironment;Json globalPlan;
std::unordered_map<std::uint32_t,std::size_t> globalRows;std::size_t globalPlanBuilds{};
void buildAll() {
    const auto began=Clock::now();collectionClosed.store(true);
    std::lock_guard lock(collectionMutex);++globalPlanBuilds;
    try {
        prepareStreamInput(environment());
        globalPlan=createStreamPlan(*streamEnvironment,fullRuntimeType);
        globalEnvironment=std::move(streamEnvironment);streamReady=true;
        SKSE::log::info("Load-stream full plan: {} records / {} NPCs; parse={:.3f} ms, references={:.3f} ms, selection={:.3f} ms; no ESP reads",streamParsedRecords,streamInputNpcs,streamParseMs,streamTypesMs,streamPlanMs);
    }catch(const std::exception& e) {
        streamBuildError=e.what();streamEnvironment.reset();streamReady=false;
        SKSE::log::error("Load-stream input unavailable: {}; disk fallback retained",e.what());
        allDisabled.store(true);globalEnvironment=std::make_unique<EarlyEnvironment>(environment());
        globalPlan={{"npcs",Json::array()},{"enabled_fields",captureConfig.at("fields")}};
    }
    // Raw byte ownership ends once the sole full plan owns decoded values.
    streamFiles.clear();
    for(std::size_t i=0;i<globalPlan.at("npcs").size();++i)globalRows.emplace(globalEnvironment->ids.at(globalPlan["npcs"][i]["npc"].get<std::string>()),i);
    planNs=static_cast<std::uint64_t>((Clock::now()-began).count());
    SKSE::log::info("Early recovery plan: {} captured records, {} NPCs, {} collection deferrals; {:.3f} ms",streamParsedRecords,globalRows.size(),streamFailures,planNs/1e6);
}
Json earlyProjection(const Json& full) {
    static const auto names=[] {auto fields=earlyFields();return std::set<std::string>(fields.begin(),fields.end());}();
    Json fields=Json::object();bool ready=false;
    for(const auto& [field,spec]:full.at("fields").items())if(names.contains(field)){fields[field]=spec;ready|=spec.at("status")=="ready";}
    if(full.at("status")!="ready"||!ready)return Json();
    return {{"npc",full.at("npc")},{"winner",full.at("winner")},{"status","ready"},{"fields",std::move(fields)}};
}
void applyAll(RE::TESNPC* npc) noexcept {
    if(allDisabled.load()||streamLateLoads.load())return;
    DWORD unset=0;initThread.compare_exchange_strong(unset,GetCurrentThreadId());
    if(initThread.load()!=GetCurrentThreadId()){++foreignInitThreads;return;}
    try {
        if(!collectionClosed.load())buildAll();
        if(!npc||npc->formID==7||!globalRows.contains(npc->formID)||!attemptedNPCs.insert(npc->formID).second)return;
        auto row=earlyProjection(globalPlan["npcs"][globalRows.at(npc->formID)]);
        if(row.is_null())return;
        const auto began=Clock::now();
        try{earlyApplyImpl(npc,*globalEnvironment,row);}
        catch(const std::exception& e){earlyResults.push_back({{"npc",row.at("npc")},{"status","error"},{"reason",e.what()}});SKSE::log::error("Early recovery {}: {}; regular recovery retained",row.at("npc").get<std::string>(),e.what());}
        applyNs+=static_cast<std::uint64_t>((Clock::now()-began).count());
    }catch(const std::exception& e){allDisabled.store(true);SKSE::log::error("Early recovery disabled: {}; regular recovery retained",e.what());}
     catch(...){allDisabled.store(true);SKSE::log::error("Early recovery disabled after unknown exception; regular recovery retained");}
}
std::string streamReuseFailure(const Json& manifest,const Json& config,const Sequence& exclusions,const std::vector<std::uint32_t>& actualNPCs) {
    if(!streamReady||globalPlan.is_null())return streamBuildError.empty()?"Full captured plan unavailable":streamBuildError;
    if(!streamFrozen.load()||streamInFlight.load())return "Record capture not frozen or Load still in flight";
    if(streamFailures||streamDuplicates)return "Incomplete or duplicate captured record history";
    if(allDisabled.load()||streamLateLoads.load()||foreignInitThreads.load())return "Unsupported early loading lifecycle";
    if(config!=captureConfig||exclusions!=captureExclusions)return "Configuration or exclusions changed during loading";
    Sequence order;for(const auto& p:manifest.at("plugins"))order.push_back(lower(p.at("name").get<std::string>()));
    if(order!=streamPluginOrder)return "Loaded plugin order changed during loading";
    for(auto id:actualNPCs)if(id&&(id>>24)!=0xFF&&!globalRows.contains(id))return "Static NPC absent from captured plan";
    return {};
}
bool executable(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info))||info.State!=MEM_COMMIT||info.Protect&PAGE_GUARD)return false;
    const auto p=info.Protect&0xFF;return p==PAGE_EXECUTE||p==PAGE_EXECUTE_READ||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;
}
}
std::optional<Json> takePlan(const Json& manifest,const Json& config,const Sequence& exclusions) {
    if(!streamEnabled.load())return std::nullopt;
    std::string failure;std::vector<std::uint32_t> current;
    try {
        auto* data=RE::TESDataHandler::GetSingleton();need(data,"Data handler unavailable for capture coverage check");
        for(auto* npc:data->GetFormArray<RE::TESNPC>())if(npc)current.push_back(npc->formID);
        failure=streamReuseFailure(manifest,config,exclusions,current);
    }catch(const std::exception& e){failure=e.what();}
    if(!failure.empty()){SKSE::log::error("Load-stream disk fallback: {}",failure);return std::nullopt;}
    globalPlan["load_capture"]={{"records",streamParsedRecords},{"npcs",streamInputNpcs},{"plan_builds",globalPlanBuilds},{"capture_failures",streamFailures},{"sources",streamSources}};
    if(config.value("debug",false))globalPlan["load_capture"]["early_results"]=earlyResults;
    streamReused=true;SKSE::log::info("Load-stream plan reused: {} records / {} NPCs; no disk plugin scan, no repeated selection",streamParsedRecords,streamInputNpcs);
    return std::move(globalPlan);
}
bool canPruneNativeWork(){return nativePruningAllowed;}
double measuredEarlyMilliseconds(){return (loadOwnNs.load()+initOwnNs.load()+finishNs)/1e6;}
void releasePlan() {
    nativePruningAllowed=streamReused&&!allDisabled.load()&&streamLateLoads.load()==0&&foreignInitThreads.load()==0&&captureConfig.value("mode",std::string())=="apply";
    globalPlan=Json();globalRows.clear();globalEnvironment.reset();attemptedNPCs.clear();earlyResults=Json::array();streamFiles.clear();
}
bool install(const runtime::Version& host) {
    const auto* layout=runtime::captureLayout(host);if(!layout)return false;
    auto v=REL::Module::get().version();
    if(runtime::selectFile(v[0],v[1],v[2],v[3])!=runtime::selectSKSE(host.major,host.minor,host.build,host.store))return false;
    captureConfig=readJson("Data/SKSE/Plugins/NPCBehaviorBridge.json");validateConfig(captureConfig);
    captureExclusions=readExclusions("Data/SKSE/Plugins/NPCBehaviorBridge.exclude.txt");
    REL::Relocation<std::uintptr_t> primary{RE::VTABLE_TESNPC[0]};
    std::uintptr_t previousLoad{},previousInit{};
    need(read(primary.address()+layout->loadSlot*sizeof(void*),&previousLoad,sizeof(previousLoad))&&executable(previousLoad),"NPC Load callback unavailable");
    need(read(primary.address()+layout->initSlot*sizeof(void*),&previousInit,sizeof(previousInit))&&executable(previousInit),"NPC initialization callback unavailable");
    originalLoad=reinterpret_cast<LoadFn>(previousLoad);originalInit=reinterpret_cast<InitFn>(previousInit);
    // Preserve existing hook chains; do not patch native function instructions.
    primary.write_vfunc(layout->loadSlot,&load);primary.write_vfunc(layout->initSlot,&init);
    streamEnabled=true;active.store(true);
    SKSE::log::info("Load capture enabled for {} {}: 75 input fields, 26 early-recovery fields",host.name,host.store?"GOG":"Steam");
    return true;
}
void finish() {
    const auto began=Clock::now();
    if(!active.exchange(false))return;
    unsigned fields=0,npcs=0,errors=0,skips=0;
    for(const auto& r:earlyResults){fields+=r.value("changed_fields",0u);npcs+=r.value("changed_fields",0u)!=0;errors+=r.value("status",std::string())=="error";skips+=r.value("status",std::string())=="skipped";}
    SKSE::log::info("Early recovery: {} fields on {} NPCs; {} deferred transactions; {} errors",fields,npcs,skips,errors);
    SKSE::log::info("Load capture: {} records, {} failures, {} late loads; plan builds={}",streamCalls.load(),streamFailures,streamLateLoads.load(),globalPlanBuilds);
    finishNs=static_cast<std::uint64_t>((Clock::now()-began).count());
}
}
