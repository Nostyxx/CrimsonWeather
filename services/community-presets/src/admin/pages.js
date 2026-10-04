export const ADMIN_LOGIN_HTML = `<!doctype html>
<html><head><meta charset="utf-8"><title>Crimson Weather Admin Login</title>
<style>body{font-family:Segoe UI,Arial,sans-serif;margin:24px;background:#111;color:#eee}input,button{font:inherit;padding:8px;margin:4px 0}input{width:min(520px,100%);background:#0c0c0c;color:#eee;border:1px solid #333}button{display:block}.err{color:#ff9b9b}</style></head>
<body><h1>Crimson Weather Admin</h1>
<p>Paste your Worker ADMIN_TOKEN to create a local browser admin session.</p>
<input id="token" type="password" autocomplete="current-password" autofocus>
<button onclick="login()">Log In</button>
<p id="msg" class="err"></p>
<script>
const loginKey=new URLSearchParams(location.search).get('key')||'';
async function login(){const token=document.getElementById('token').value; const r=await fetch('/api/v1/admin/login',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({token,loginKey})}); if(r.ok){location.href='/admin';return;} document.getElementById('msg').textContent=await r.text();}
</script></body></html>`;

export const ADMIN_HTML = `<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Crimson Weather Admin</title>
<style>
:root{color-scheme:dark;--bg:#0f1013;--panel:#181a1f;--panel2:#20232a;--line:#343842;--text:#f2f4f8;--muted:#aeb6c2;--accent:#5d7fc4;--good:#5fb980;--warn:#d3aa55;--bad:#c76565}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 Segoe UI,Arial,sans-serif}button,input,select,textarea{font:inherit}button{border:1px solid #6684bf;background:#344f82;color:#fff;padding:7px 11px;cursor:pointer}button.secondary{background:#242832;border-color:#4b5362}button.danger{background:#6b2929;border-color:#a84f4f}button.good{background:#2e6845;border-color:#58a375}button:disabled{opacity:.45;cursor:not-allowed}input,select,textarea{background:#101218;color:var(--text);border:1px solid var(--line);padding:7px 9px}main{max-width:1480px;margin:0 auto;padding:18px}.top{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:14px}.top h1{font-size:22px;margin:0}.tabs{display:flex;gap:6px;flex-wrap:wrap;margin:12px 0 16px}.tab{background:#1d2129;border-color:#3d4656}.tab.active{background:#3a568c;border-color:#6f8ccc}.toolbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:10px 0}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}.card,.panel{background:var(--panel);border:1px solid var(--line);padding:12px}.card b{font-size:24px;display:block}.muted{color:var(--muted);font-size:12px}.message{min-height:20px;color:#ffcf8a;margin:8px 0}.table{width:100%;border-collapse:collapse;background:var(--panel)}th,td{border-bottom:1px solid var(--line);padding:8px;text-align:left;vertical-align:top}th{background:var(--panel2);color:#dfe6ef;position:sticky;top:0}tr:hover td{background:#1b1f27}.pill{display:inline-block;border:1px solid #555f70;background:#252a34;padding:1px 6px;margin-left:5px;color:#dce4ef}.pending{border-color:var(--warn);color:#ffd991}.approved{border-color:var(--good);color:#a9e8bd}.rejected,.deleted{border-color:var(--bad);color:#ffb0b0}.drawer{position:fixed;inset:20px;background:#14161b;border:1px solid #5d6574;box-shadow:0 18px 70px #000;z-index:5;padding:16px;overflow:auto}.drawer[hidden],section[hidden]{display:none}.drawer header{display:flex;justify-content:space-between;align-items:flex-start;gap:12px}.split{display:grid;grid-template-columns:minmax(280px,420px) 1fr;gap:12px}pre{white-space:pre-wrap;overflow:auto;background:#0b0d11;border:1px solid #303541;padding:10px;max-height:520px}.right{text-align:right}.empty{padding:18px;color:var(--muted)}.nowrap{white-space:nowrap}.search{min-width:280px}.dangerText{color:#ffb0b0}.clientHash{font-family:Consolas,monospace;font-size:12px;color:#c8d4e4}
</style></head>
<body><main>
<div class="top"><h1>Crimson Weather Community Admin</h1><div><button class="secondary" data-action="rebuild">Rebuild Catalog</button> <a href="/api/v1/admin/submissions/export.zip"><button class="secondary">Download Pending ZIP</button></a> <button class="secondary" data-action="logout">Log Out</button></div></div>
<div class="tabs">
  <button class="tab active" data-tab="dashboard">Dashboard</button>
  <button class="tab" data-tab="review">Review Queue</button>
  <button class="tab" data-tab="presets">Presets</button>
  <button class="tab" data-tab="clients">Clients</button>
  <button class="tab" data-tab="update">Update</button>
  <button class="tab" data-tab="trash">Trash</button>
  <button class="tab" data-tab="audit">Audit</button>
</div>
<div id="message" class="message"></div>

<section id="dashboard">
  <div id="cards" class="grid"></div>
  <div class="split" style="margin-top:12px">
    <div class="panel"><h2>Newest Pending</h2><div id="dashPending"></div></div>
    <div class="panel"><h2>Top Presets</h2><div id="dashTop"></div></div>
  </div>
</section>

<section id="review" hidden>
  <div class="toolbar"><button class="good" data-bulk="approve">Approve Selected</button><button class="secondary" data-bulk="reject">Reject Selected</button><button class="danger" data-bulk="delete">Delete Selected</button><button class="secondary" data-action="refresh">Refresh</button></div>
  <div id="reviewTable"></div>
</section>

<section id="presets" hidden>
  <div class="toolbar"><input id="presetSearch" class="search" placeholder="Search title, author, id, client..."><select id="presetStatus"><option value="">All active</option><option value="approved">Approved</option><option value="pending">Pending</option><option value="rejected">Rejected</option></select><select id="presetSort"><option value="updated">Updated</option><option value="created">Created</option><option value="downloads">Downloads</option><option value="likes">Likes</option><option value="title">Title</option></select><button class="secondary" data-action="loadPresets">Search</button></div>
  <div id="presetTable"></div>
</section>

<section id="clients" hidden>
  <div class="toolbar"><input id="clientSearch" class="search" placeholder="Search client hash, label, author..."><select id="clientSort"><option value="last">Last upload</option><option value="uploads">Uploads</option><option value="downloads">Downloads</option><option value="likes">Likes</option></select><button class="secondary" data-action="loadClients">Search</button></div>
  <div class="panel"><h2>Whitelist Client</h2><div class="toolbar"><input id="wlClientId" class="search" placeholder="Raw ClientId or submitter hash"><input id="wlLabel" placeholder="Label"><input id="wlNote" placeholder="Note"><button data-action="addWhitelist">Whitelist</button></div></div>
  <div id="clientTable" style="margin-top:12px"></div>
</section>

<section id="update" hidden>
  <div class="panel">
    <h2>Addon Update Notice</h2>
    <p class="muted">This controls what the addon sees in the overlay header. Saving here updates /api/v1/update without changing addon code.</p>
    <div class="toolbar">
      <label>Latest Version <input id="updateVersion" placeholder="0.6.6"></label>
      <label>Published <input id="updatePublished" placeholder="2026-05-22"></label>
      <label><input id="updateCritical" type="checkbox"> Critical</label>
    </div>
    <div class="toolbar">
      <label style="flex:1">Download URL <input id="updateUrl" style="width:100%" placeholder="https://www.nexusmods.com/crimsondesert/mods/632?tab=files"></label>
    </div>
    <div class="toolbar">
      <label style="flex:1">Direct .addon64 <input id="updateArtifact" type="file" accept=".addon64,application/octet-stream" style="width:100%"></label>
      <button class="secondary" data-action="uploadUpdateArtifact">Upload .addon64</button>
    </div>
    <div id="updateArtifactInfo" class="muted"></div>
    <textarea id="updateChangelog" spellcheck="false" style="width:100%;min-height:520px;font-family:Consolas,monospace"></textarea>
    <div class="toolbar">
      <button class="good" data-action="saveUpdate">Save Update Notice</button>
      <button class="secondary" data-action="loadUpdate">Reload</button>
      <span id="updateSource" class="muted"></span>
    </div>
  </div>
</section>

<section id="trash" hidden>
  <div class="toolbar"><input id="trashSearch" class="search" placeholder="Search trash..."><button class="secondary" data-action="loadTrash">Search</button><button class="secondary" data-bulk="restore">Restore Selected</button><button class="danger" data-bulk="purge">Purge Selected</button></div>
  <div id="trashTable"></div>
</section>

<section id="audit" hidden>
  <div class="toolbar"><button class="secondary" data-action="loadAudit">Refresh</button></div>
  <div id="auditTable"></div>
</section>

<section id="detail" class="drawer" hidden>
  <header><div><h2 id="detailTitle"></h2><div id="detailMeta" class="muted"></div></div><button class="secondary" data-action="closeDetail">Close</button></header>
  <div class="split"><div><h3>Metadata</h3><pre id="detailJson"></pre><h3>Scan</h3><pre id="detailScan"></pre></div><div><h3>INI Content</h3><pre id="detailIni"></pre></div></div>
</section>
</main>
<script>
const state={tab:'dashboard',selected:new Set()};
const qs=(s)=>document.querySelector(s);
const qsa=(s)=>Array.from(document.querySelectorAll(s));
async function api(path,opts){const r=await fetch(path,opts); if(!r.ok) throw new Error(await r.text()); return r.json();}
function esc(s){return String(s??'').replace(/[&<>"]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c];});}
function msg(text){qs('#message').textContent=text||'';}
function pill(row){const cls=row.deleted_at?'deleted':row.status;return '<span class="pill '+cls+'">'+esc(row.deleted_at?'deleted':row.status)+'</span>';}
function client(row){return row.client_fingerprint?'<span class="clientHash">'+esc(row.client_fingerprint)+'</span>':'';}
function selectCell(row){return '<input type="checkbox" class="rowcheck" data-id="'+esc(row.id)+'">';}
function actionButton(action,id,label,cls){return '<button class="'+(cls||'secondary')+'" data-action="'+action+'" data-id="'+esc(id)+'">'+label+'</button>';}
function selectedIds(){return qsa('.rowcheck:checked').map(function(x){return x.dataset.id;});}
function table(headers,rows,empty){if(!rows.length)return '<div class="empty">'+esc(empty||'No rows.')+'</div>';return '<table class="table"><thead><tr>'+headers.map(function(h){return '<th>'+h+'</th>';}).join('')+'</tr></thead><tbody>'+rows.join('')+'</tbody></table>';}
async function setTab(tab){state.tab=tab;qsa('.tab').forEach(function(b){b.classList.toggle('active',b.dataset.tab===tab);});qsa('main>section').forEach(function(s){if(s.id!=='detail')s.hidden=s.id!==tab;});msg('');await loadActive();}
async function loadActive(){if(state.tab==='dashboard')return loadDashboard();if(state.tab==='review')return loadReview();if(state.tab==='presets')return loadPresets();if(state.tab==='clients')return loadClients();if(state.tab==='update')return loadUpdate();if(state.tab==='trash')return loadTrash();if(state.tab==='audit')return loadAudit();}
async function loadDashboard(){const d=await api('/api/v1/admin/overview');const cards=[['Pending',d.counts.pending],['Approved',d.counts.approved],['Rejected',d.counts.rejected],['Trash',d.counts.deleted],['Clients',d.counts.clients],['Downloads',d.counts.downloads],['Likes',d.counts.likes]];qs('#cards').innerHTML=cards.map(function(c){return '<div class="card"><span class="muted">'+c[0]+'</span><b>'+Number(c[1]||0)+'</b></div>';}).join('');qs('#dashPending').innerHTML=miniPresetList(d.newestPending||[]);qs('#dashTop').innerHTML=miniPresetList(d.topPresets||[]);}
function miniPresetList(rows){return rows.length?rows.map(function(r){return '<div class="row"><b>'+esc(r.title)+'</b> '+pill(r)+'<div class="muted">'+esc(r.id)+' by '+esc(r.author_name||'')+' '+client(r)+'</div><div class="muted">'+Number(r.downloads||0)+' downloads | '+Number(r.likes||0)+' likes</div></div>';}).join(''):'<div class="empty">Nothing here.</div>';}
async function loadReview(){const d=await api('/api/v1/admin/presets?status=pending&deleted=active&sort=created&limit=300');qs('#reviewTable').innerHTML=presetTable(d.presets||[],true);}
async function loadPresets(){const q=encodeURIComponent(qs('#presetSearch').value);const status=encodeURIComponent(qs('#presetStatus').value);const sort=encodeURIComponent(qs('#presetSort').value);const d=await api('/api/v1/admin/presets?q='+q+'&status='+status+'&sort='+sort+'&deleted=active&limit=300');qs('#presetTable').innerHTML=presetTable(d.presets||[],false);}
async function loadTrash(){const q=encodeURIComponent(qs('#trashSearch').value);const d=await api('/api/v1/admin/presets?q='+q+'&deleted=trash&sort=delete_after&limit=300');qs('#trashTable').innerHTML=presetTable(d.presets||[],true);}
function presetTable(rows,checks){return table([(checks?'<input type="checkbox" data-action="toggleAll">':''),'Preset','Client','Stats','Dates','Actions'],rows.map(function(r){const actions=[actionButton('view',r.id,'View'),r.status==='pending'&&!r.deleted_at?actionButton('approve',r.id,'Approve','good'):'',r.status==='pending'&&!r.deleted_at?actionButton('reject',r.id,'Reject'):'',!r.deleted_at?actionButton('whitelistPreset',r.id,'Whitelist'):'',!r.deleted_at?actionButton('delete',r.id,'Delete','danger'):'',r.deleted_at?actionButton('restore',r.id,'Restore','good'):'',r.deleted_at?actionButton('purge',r.id,'Purge','danger'):''].filter(Boolean).join(' ');return '<tr><td>'+(checks?selectCell(r):'')+'</td><td><b>'+esc(r.title)+'</b> '+pill(r)+(r.update_of?'<div class="muted">Update for '+esc(r.update_of)+'</div>':'')+'<div class="muted">'+esc(r.id)+' by '+esc(r.author_name||'')+'</div><div>'+esc(r.description||'')+'</div></td><td>'+client(r)+'</td><td class="nowrap">'+Number(r.downloads||0)+' downloads<br>'+Number(r.likes||0)+' likes</td><td class="muted">Created '+esc(r.created_at||'')+'<br>Updated '+esc(r.updated_at||'')+(r.deleted_at?'<br><span class="dangerText">Purge after '+esc(r.delete_after||'')+'</span>':'')+'</td><td class="nowrap">'+actions+'</td></tr>'; }),'No presets found.');}
async function loadClients(){const q=encodeURIComponent(qs('#clientSearch').value);const sort=encodeURIComponent(qs('#clientSort').value);const d=await api('/api/v1/admin/clients?q='+q+'&sort='+sort+'&limit=300');qs('#clientTable').innerHTML=table(['Client','Whitelist','Uploads','Stats','Actions'],(d.clients||[]).map(function(c){return '<tr><td><div class="clientHash">'+esc(c.client_fingerprint)+'</div><div class="muted">'+esc(c.submitter_hash)+'</div></td><td><b>'+esc(c.label||'(unlabeled)')+'</b><div class="muted">'+(Number(c.auto_approve||0)?'Auto approve':'Not trusted')+'</div><div>'+esc(c.note||'')+'</div></td><td>Approved '+Number(c.approved_count||0)+'<br>Pending '+Number(c.pending_count||0)+'<br>Rejected '+Number(c.rejected_count||0)+'<br>Deleted '+Number(c.deleted_count||0)+'</td><td>'+Number(c.total_downloads||0)+' downloads<br>'+Number(c.total_likes||0)+' likes<br><span class="muted">'+esc(c.last_upload||'')+'</span></td><td>'+actionButton('clientUploads',c.submitter_hash,'View Uploads')+' '+actionButton('removeWhitelist',c.submitter_hash,'Remove Trust','danger')+'</td></tr>'; }),'No clients found.');}
async function loadUpdate(){const d=await api('/api/v1/admin/update');const u=d.update||{};qs('#updateVersion').value=u.latestVersion||'';qs('#updateUrl').value=u.downloadPageUrl||'';qs('#updatePublished').value=u.publishedAt||'';qs('#updateCritical').checked=!!u.critical;qs('#updateChangelog').value=u.changelog||'';qs('#updateArtifactInfo').textContent=u.addonSha256?('Direct artifact: '+Number(u.addonSizeBytes||0)+' bytes | sha256 '+u.addonSha256+' | '+(u.addonR2Key||'')):'Direct artifact: not uploaded';qs('#updateSource').textContent='Source: '+(u.source||'default');}
async function saveUpdate(){const body={latestVersion:qs('#updateVersion').value,downloadPageUrl:qs('#updateUrl').value,publishedAt:qs('#updatePublished').value,critical:qs('#updateCritical').checked,changelog:qs('#updateChangelog').value};await api('/api/v1/admin/update',{method:'PUT',headers:{'content-type':'application/json'},body:JSON.stringify(body)});msg('Update notice saved');await loadUpdate();}
async function uploadUpdateArtifact(){const file=qs('#updateArtifact').files[0];if(!file){msg('Choose a .addon64 file first.');return;}const version=qs('#updateVersion').value;if(!version){msg('Set Latest Version before uploading.');return;}const r=await fetch('/api/v1/admin/update/artifact?version='+encodeURIComponent(version),{method:'PUT',headers:{'content-type':'application/octet-stream'},body:file});if(!r.ok)throw new Error(await r.text());const d=await r.json();msg('Uploaded .addon64 '+d.version+' sha256 '+d.addonSha256);qs('#updateArtifact').value='';await loadUpdate();}
async function loadAudit(){const d=await api('/api/v1/admin/audit?limit=200');qs('#auditTable').innerHTML=table(['Time','Action','Preset','Admin','Note'],(d.audit||[]).map(function(a){return '<tr><td>'+esc(a.created_at)+'</td><td>'+esc(a.action)+'</td><td>'+esc(a.preset_id||'')+'</td><td>'+esc(a.admin_email_or_token||'')+'</td><td>'+esc(a.note||'')+'</td></tr>'; }),'No audit entries.');}
async function viewPreset(id){const d=await api('/api/v1/admin/presets/'+encodeURIComponent(id));qs('#detailTitle').textContent=d.preset.title+' by '+d.preset.author_name;qs('#detailMeta').textContent=d.preset.id+' | '+d.preset.status+' | '+(d.preset.client_fingerprint||'no client')+' | '+d.preset.r2_key;qs('#detailJson').textContent=JSON.stringify(d.preset,null,2);qs('#detailIni').textContent=d.iniText||'(missing file)';qs('#detailScan').textContent=JSON.stringify(d.scan,null,2);qs('#detail').hidden=false;}
function closeDetail(){qs('#detail').hidden=true;}
async function approve(id){await api('/api/v1/admin/submissions/'+encodeURIComponent(id)+'/approve',{method:'POST'});msg('Approved '+id);await loadActive();}
async function rejectOne(id){const note=prompt('Reject note?')||'';await api('/api/v1/admin/submissions/'+encodeURIComponent(id)+'/reject',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({note})});msg('Rejected '+id);await loadActive();}
async function deleteOne(id){if(!confirm('Hide '+id+' now? It will move to Trash and auto-purge after 7 days.'))return;await api('/api/v1/admin/presets/'+encodeURIComponent(id),{method:'DELETE',headers:{'content-type':'application/json'},body:JSON.stringify({reason:'admin'})});msg('Moved to Trash '+id);closeDetail();await loadActive();}
async function restoreOne(id){await api('/api/v1/admin/presets/'+encodeURIComponent(id)+'/restore',{method:'POST'});msg('Restored '+id);await loadActive();}
async function purgeOne(id){if(!confirm('Permanently purge '+id+'? This deletes DB and R2 data now.'))return;await api('/api/v1/admin/presets/'+encodeURIComponent(id)+'/purge',{method:'POST'});msg('Purged '+id);closeDetail();await loadActive();}
async function whitelistPreset(id){const label=prompt('Trusted label?')||'';await api('/api/v1/admin/whitelist/from-preset/'+encodeURIComponent(id),{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({label})});msg('Submitter whitelisted');await loadActive();}
async function addWhitelist(){const value=qs('#wlClientId').value;const label=qs('#wlLabel').value;const note=qs('#wlNote').value;const body=/^[a-f0-9]{64}$/i.test(value.trim())?{submitterHash:value.trim(),label,note,autoApprove:true}:{clientId:value,label,note,autoApprove:true};await api('/api/v1/admin/whitelist',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)});qs('#wlClientId').value='';msg('Client whitelisted');await loadClients();}
async function removeWhitelist(hash){if(!confirm('Remove trusted client?'))return;await api('/api/v1/admin/whitelist/'+encodeURIComponent(hash),{method:'DELETE'});msg('Trusted client removed');await loadClients();}
async function bulk(action){const ids=selectedIds();if(!ids.length){msg('Select at least one row.');return;}if(action==='delete'&&!confirm('Move '+ids.length+' preset(s) to Trash for 7 days?'))return;if(action==='purge'&&!confirm('Permanently purge '+ids.length+' preset(s)?'))return;const body={};body[action]=ids;await api('/api/v1/admin/submissions/batch',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)});msg('Batch '+action+' complete');await loadActive();}
async function rebuild(){await api('/api/v1/admin/catalog/rebuild',{method:'POST'});msg('Catalog rebuilt');await loadDashboard();}
async function logout(){await api('/api/v1/admin/logout',{method:'POST'});location.reload();}
document.addEventListener('click',function(e){const el=e.target.closest('button,input[type=checkbox]');if(!el)return;const a=el.dataset.action;if(el.dataset.tab){setTab(el.dataset.tab).catch(function(err){msg(err.message);});return;}if(el.dataset.bulk){bulk(el.dataset.bulk).catch(function(err){msg(err.message);});return;}if(a==='toggleAll'){qsa('.rowcheck').forEach(function(c){c.checked=el.checked;});return;}const id=el.dataset.id;if(a==='refresh')loadActive().catch(function(err){msg(err.message);});else if(a==='loadPresets')loadPresets().catch(function(err){msg(err.message);});else if(a==='loadClients')loadClients().catch(function(err){msg(err.message);});else if(a==='loadUpdate')loadUpdate().catch(function(err){msg(err.message);});else if(a==='saveUpdate')saveUpdate().catch(function(err){msg(err.message);});else if(a==='uploadUpdateArtifact')uploadUpdateArtifact().catch(function(err){msg(err.message);});else if(a==='loadTrash')loadTrash().catch(function(err){msg(err.message);});else if(a==='loadAudit')loadAudit().catch(function(err){msg(err.message);});else if(a==='view')viewPreset(id).catch(function(err){msg(err.message);});else if(a==='approve')approve(id).catch(function(err){msg(err.message);});else if(a==='reject')rejectOne(id).catch(function(err){msg(err.message);});else if(a==='delete')deleteOne(id).catch(function(err){msg(err.message);});else if(a==='restore')restoreOne(id).catch(function(err){msg(err.message);});else if(a==='purge')purgeOne(id).catch(function(err){msg(err.message);});else if(a==='whitelistPreset')whitelistPreset(id).catch(function(err){msg(err.message);});else if(a==='clientUploads'){setTab('presets').then(function(){qs('#presetSearch').value=id.slice(0,12);qs('#presetStatus').value='';return loadPresets();}).catch(function(err){msg(err.message);});}else if(a==='removeWhitelist')removeWhitelist(id).catch(function(err){msg(err.message);});else if(a==='addWhitelist')addWhitelist().catch(function(err){msg(err.message);});else if(a==='rebuild')rebuild().catch(function(err){msg(err.message);});else if(a==='logout')logout().catch(function(err){msg(err.message);});else if(a==='closeDetail')closeDetail();});
setTab('dashboard').catch(function(err){msg(err.message);});
</script></body></html>`;
