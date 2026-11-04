#pragma once
#include "nbb/diagnostics.hpp"

namespace nbb::report {
inline Json model(const Json& plan) {
    Json records=Json::array();unsigned restored=0,changedNPCs=0,errorNPCs=0;
    for(const auto& row:plan.at("npcs")) {
        const auto summary=diagnostics::npcResult(row);
        restored+=summary.restored;changedNPCs+=summary.restored!=0;errorNPCs+=!summary.errors.empty();
        auto status=!summary.errors.empty()?"error":summary.restored?"restored":summary.wouldRestore?"audit":
            summary.unchanged?"unchanged":summary.pending?"pending":"skipped";
        Json fields=Json::array();
        for(const auto& [name,spec]:row.at("fields").items()) {
            const auto result=row.contains("field_results")&&row.at("field_results").contains(name)?row.at("field_results").at(name):spec;
            const auto state=result.value("status",std::string("skipped"));
            Json field={{"name",name},{"status",state},{"reason",result.value("reason",spec.value("reason",std::string()))},
                {"sources",spec.value("sources",Json::array())},{"values",Json::object()}};
            for(const auto* key:{"official_baseline","baseline","disk","expected"})
                if(spec.contains(key))field["values"][key]=spec.at(key);
            for(const auto* key:{"runtime_before","runtime_after"})
                if(row.contains(key)&&row.at(key).contains(name))field["values"][key]=row.at(key).at(name);
            if(state=="applied_verified") {
                const auto& values=field.at("values");
                field["status"]=values.contains("runtime_before")&&values.contains("runtime_after")&&values.at("runtime_before")!=values.at("runtime_after")?"restored":"unchanged";
            }
            if(spec.contains("inheritance_provider")) {
                field["selection"]="Template inheritance or automatic-stat synchronization";
                field["provider"]=spec.at("inheritance_provider");
            } else if(spec.contains("baseline")&&spec.contains("official_baseline")&&spec.contains("expected")&&
                spec.at("expected")==spec.at("baseline")&&spec.at("baseline")!=spec.at("official_baseline")&&
                spec.value("sources",Json::array())==Json::array({row.value("baseline_plugin",std::string())}))
                field["selection"]="Configured baseline fallback after an official-value revert";
            else field["selection"]="Last qualifying value different from both baselines";
            fields.push_back(std::move(field));
        }
        records.push_back({{"key",row.at("npc")},{"editor",row.value("editor_id",std::string())},
            {"name",row.value("display_name",row.value("editor_id",std::string()))},{"status",status},
            {"restored",summary.restored},{"unchanged",summary.unchanged},{"skipped",summary.skipped},{"pending",summary.pending},
            {"winner",row.value("winner",std::string())},{"baseline",row.value("baseline_plugin",std::string())},
            {"reason",row.value("reason",std::string())},{"summary",summary.line},{"errors",summary.errors},
            {"notes",row.value("inheritance_notes",Json::object())},{"fields",fields}});
    }
    return {{"mode",plan.value("mode",std::string("unknown"))},{"baseline",plan.value("baseline",Json::object())},
        {"session",plan.value("runtime_session",Json::object())},
        {"totals",{{"npcs",records.size()},{"restored_npcs",changedNPCs},{"restored_fields",restored},{"error_npcs",errorNPCs}}},
        {"warnings",plan.value("configuration_warnings",Json::array())},{"records",records}};
}

inline std::string embeddedJson(const Json& value) {
    const auto json=value.dump();std::string safe;safe.reserve(json.size());
    for(char c:json) {if(c=='<')safe+="\\u003c";else if(c=='>')safe+="\\u003e";else if(c=='&')safe+="\\u0026";else safe+=c;}
    return safe;
}

inline void write(bool debug,const std::filesystem::path& path,const Json& plan) {
    if(!debug)return;
    const auto data=model(plan);
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    if(!out)throw std::runtime_error("Cannot write HTML report: "+path.string());
    out << R"NBB(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; base-uri 'none'">
<title>NPC Behavior Bridge — Results</title>
<style>
:root{color-scheme:dark;--bg:#111923;--panel:#1b2734;--line:#354556;--text:#edf3f8;--muted:#b0c0cf;--accent:#9acbbd}*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.55 system-ui,sans-serif}main{max-width:1240px;margin:auto;padding:36px 26px}h1{font-size:32px;letter-spacing:-.6px;margin:0 0 5px}h2{font-size:18px}p{margin:8px 0}a{color:var(--accent)}.muted,.meta{color:var(--muted)}.eyebrow{color:var(--accent);font-size:12px;letter-spacing:2px;text-transform:uppercase}.cards{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin:24px 0}.card{padding:16px 20px;background:var(--panel);border:1px solid var(--line);border-radius:10px}.card strong{display:block;font-size:28px}.card span{color:var(--muted)}.toolbar{display:flex;gap:12px;align-items:end;flex-wrap:wrap;margin:24px 0 12px}label{display:flex;flex-direction:column;gap:5px}.search{flex:1;min-width:240px}input,select,button{font:inherit;color:var(--text);background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:10px}input:focus,select:focus,button:focus-visible,summary:focus-visible{outline:2px solid var(--accent);outline-offset:2px}button{cursor:pointer}button:disabled{opacity:.4;cursor:default}#results>details{border:1px solid var(--line);border-radius:9px;background:var(--panel);margin:10px 0;overflow:hidden}summary{cursor:pointer;padding:15px 18px}summary::marker{color:var(--muted)}.npcTitle{font-size:17px;font-weight:650}.badge{display:inline-block;border-radius:4px;padding:2px 8px;margin-left:10px;font-size:12px;letter-spacing:.4px;background:#344658;color:#e5eef7}.restored{background:#244a40;color:#b9efda}.error{background:#622f38;color:#ffd1d7}.skipped,.audit{background:#52442c;color:#f4dda6}.pending{background:#40375b;color:#dfd2ff}.meta{font-size:13px;overflow-wrap:anywhere}.body{padding:0 18px 18px}.field{border:1px solid var(--line);border-radius:7px;margin-top:14px;padding:14px}.fieldHead{display:flex;align-items:center;flex-wrap:wrap;gap:6px}.fieldHead strong{font-size:16px}.values{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px;margin-top:14px}.value{min-width:0;background:#121c26;border:1px solid #2b3c4d;border-radius:5px;padding:10px}.value h3{font-size:12px;color:var(--muted);margin:0 0 8px;font-weight:500}.value pre{white-space:pre-wrap;overflow-wrap:anywhere;font:12px/1.6 ui-monospace,Consolas,monospace;margin:0;max-height:210px;overflow:auto}.missing{color:#8c9cab;font-style:italic}.notice{border-left:3px solid var(--accent);padding:8px 14px;background:#172530;margin-top:12px}.errorText{color:#ffb8c3}.pager{display:flex;gap:12px;align-items:center;margin:20px 0;justify-content:space-between}.foot{color:var(--muted);font-size:12px;margin-top:25px}@media(max-width:700px){main{padding:20px 14px}h1{font-size:25px}.cards{grid-template-columns:repeat(2,1fr)}.values{grid-template-columns:1fr}.toolbar label{width:100%}.pager{flex-wrap:wrap}}
</style></head><body><main>
<div class="eyebrow">NPC Behavior Bridge · Startup report</div><h1>See what Bridge restored</h1>
<p class="muted" id="session"></p>
<p class="notice">This is a startup snapshot, not a live monitor. Later save, quest and runtime changes are not shown here. Script attachments marked pending are checked after loading actors; see the script-binding log for their outcomes.</p>
<div class="cards" id="cards"></div><div id="warnings"></div>
<div class="toolbar"><label class="search">Search NPC, FormKey, plugin or field<input id="search" type="search" placeholder="For example: Ysolda, 013bab, AI Overhaul, Stamina"></label>
<label>Show<select id="filter"><option value="attention">Changes and items to review</option><option value="all">All NPCs</option><option value="restored">Restored</option><option value="skipped">Skipped / partially skipped</option><option value="error">Errors</option><option value="pending">Pending scripts</option><option value="unchanged">Already correct</option></select></label>
<button id="reset">Reset</button></div><p class="meta" id="count" role="status" aria-live="polite"></p><div id="results"></div>
<div class="pager"><button id="prev">Previous</button><span id="page"></span><button id="next">Next</button></div>
<p class="foot">Lists retain their order. “Not read” means there was no captured runtime value; “None” is an actual null reference. Restored counts include verified before/after changes, not fields that were already correct. No external resources or network requests are used.</p>
</main><script type="application/json" id="report-data">)NBB";
    out << embeddedJson(data);
    out << R"NBB(</script><script>
'use strict';
const data=JSON.parse(document.getElementById('report-data').textContent),$=id=>document.getElementById(id);
function el(tag,text,cls){const e=document.createElement(tag);if(text!==undefined)e.textContent=text;if(cls)e.className=cls;return e;}
const fmt=v=>v===null?'None':Array.isArray(v)?(v.length?v.map((x,i)=>`${i+1}. ${typeof x==='object'?JSON.stringify(x):String(x)}`).join('\n'):'Empty list'):typeof v==='object'?JSON.stringify(v,null,2):String(v);
const titles={PKID:'Daily packages',SPLO:'Spells',SPOR:'Spectator override',ECOR:'Combat override',OCOR:'Observe-dead override',GWOR:'Guard-warning override',VMAD:'Script attachments',DPLT:'Default package list',HealthOffset:'Health offset',StaminaOffset:'Stamina offset',MagickaOffset:'Magicka offset',SkillValues:'Skill values',UseAIPackages:'Inherit packages',DeathItem:'Death loot',Template:'Template reference'};
const statusLabel=s=>({restored:'Restored',unchanged:'Already correct',pending_actor_binding:'Pending actor binding',audit_already_matches:'Already correct',audit_needs_changes:'Would restore (audit)',ready:'Planned',skipped_plan:'Skipped',skipped_runtime_conflict:'Skipped: runtime change'}[s]||s.replaceAll('_',' '));
function badge(status){const b=el('span',statusLabel(status),'badge');b.classList.add(status.includes('error')||status.includes('mismatch')?'error':status.includes('pending')?'pending':status.includes('skip')?'skipped':status==='restored'?'restored':'other');return b;}
$('session').textContent=`Mode: ${data.mode} · Runtime: ${data.session.runtime||'not recorded'} ${data.session.store||''} · Baseline: ${data.baseline.active||'official / first definition'}${data.session.pid?' · Process '+data.session.pid:''}`;
for(const [key,label] of [['npcs','NPCs reviewed'],['restored_npcs','NPCs restored'],['restored_fields','Fields changed'],['error_npcs','NPCs with errors']]){const c=el('div',undefined,'card');c.append(el('strong',Number(data.totals[key]).toLocaleString()),el('span',label));$('cards').append(c);}
for(const w of data.warnings)$('warnings').append(el('p',JSON.stringify(w),'notice'));
const searchIndex=data.records.map(r=>[r.key,r.editor,r.name,r.winner,r.baseline,...r.fields.flatMap(f=>[f.name,...f.sources])].join(' ').toLowerCase());
let page=0,filtered=[],timer;const pageSize=40;
function matches(r,filter){if(filter==='all')return true;if(filter==='attention')return r.status==='restored'||r.status==='error'||r.status==='audit'||r.pending>0||r.skipped>0||r.reason!=='';if(filter==='pending')return r.pending>0;if(filter==='skipped')return r.status==='skipped'||r.skipped>0;return r.status===filter;}
function fillBody(body,r){
 body.append(el('p',`Final plugin: ${r.winner} · Configured/available baseline: ${r.baseline}`,'meta'));
 if(r.reason)body.append(el('p',r.reason,'notice'));
 for(const error of r.errors)body.append(el('p',error,'errorText'));
 for(const note of new Set(Object.values(r.notes)))body.append(el('p',note,'meta'));
 if(!r.fields.length)body.append(el('p','No field recovery was selected for this NPC.','muted'));
 for(const f of r.fields){
  const box=el('section',undefined,'field'),head=el('div',undefined,'fieldHead');head.append(el('strong',titles[f.name]?`${titles[f.name]} (${f.name})`:f.name),badge(f.status));box.append(head);
  box.append(el('p',`Selected source: ${f.sources.length?f.sources.join(', '):'Computed / no direct source'}${f.provider?' · Template/stat provider: '+f.provider:''}`,'meta'));
  box.append(el('p',f.selection,'meta'));if(f.reason)box.append(el('p',f.reason,'notice'));
  const values=el('div',undefined,'values');
  for(const [key,title] of [['official_baseline','Official / first-definition baseline'],['baseline','Configured / available baseline'],['disk','Final plugin value'],['expected','Selected target'],['runtime_before','Runtime before Bridge'],['runtime_after','Runtime after Bridge']]){
   const cell=el('div',undefined,'value');cell.append(el('h3',title));const exists=Object.hasOwn(f.values,key);cell.append(el('pre',exists?fmt(f.values[key]):key.startsWith('runtime')?'Not read':'Not available',exists?'':'missing'));values.append(cell);
  }box.append(values);body.append(box);
 }
}
function render(){
 const total=filtered.length,pages=Math.max(1,Math.ceil(total/pageSize));page=Math.min(page,pages-1);$('results').replaceChildren();
 $('count').textContent=`${total.toLocaleString()} matching NPCs · Expand an NPC to compare its fields.`;
 for(const r of filtered.slice(page*pageSize,(page+1)*pageSize)){
  const d=el('details'),summary=el('summary');summary.append(el('span',r.name||r.editor||r.key,'npcTitle'),badge(r.status));summary.append(el('div',`${r.editor} · ${r.key} · restored ${r.restored}, already correct ${r.unchanged}, skipped ${r.skipped}${r.pending?', pending scripts '+r.pending:''}`,'meta'));d.append(summary);let loaded=false;
  d.addEventListener('toggle',()=>{if(d.open&&!loaded){loaded=true;const body=el('div',undefined,'body');fillBody(body,r);d.append(body);}});$('results').append(d);
 }
 if(!total)$('results').append(el('p','No matching NPCs. Try another search or choose All NPCs.','notice'));
 $('page').textContent=`Page ${page+1} of ${pages}`;$('prev').disabled=page===0;$('next').disabled=page+1>=pages;
}
function apply(){const term=$('search').value.trim().toLowerCase(),filter=$('filter').value;filtered=data.records.filter((r,i)=>matches(r,filter)&&(!term||searchIndex[i].includes(term)));page=0;render();}
$('search').addEventListener('input',()=>{clearTimeout(timer);timer=setTimeout(apply,150);});$('filter').addEventListener('change',apply);
$('reset').onclick=()=>{$('search').value='';$('filter').value='attention';apply();};$('prev').onclick=()=>{page--;render();};$('next').onclick=()=>{page++;render();};
apply();
</script></body></html>)NBB";
    out.flush();if(!out)throw std::runtime_error("HTML report write failed: "+path.string());
}
}
