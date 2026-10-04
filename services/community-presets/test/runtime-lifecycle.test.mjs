import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {build} from 'esbuild';
import {Miniflare} from 'miniflare';
const dir=path.dirname(fileURLToPath(import.meta.url));
const repo=path.resolve(dir,'../../..');
const entry=path.join(repo,'services/community-presets/src/index.js');
const bundled=await build({entryPoints:[entry],bundle:true,write:false,format:'esm',platform:'browser',target:'es2022'});
const mf=new Miniflare({modules:true,script:bundled.outputFiles[0].text,compatibilityDate:'2026-05-20',d1Databases:['DB'],r2Buckets:['PRESETS'],bindings:{ADMIN_TOKEN:'review-token',DEVICE_HASH_SECRET:'review-secret'}});
let checks=0;
try {
 const db=await mf.getD1Database('DB');
 for(const name of (await fs.readdir(path.join(repo,'services/community-presets/migrations'))).sort()){
  const sql=await fs.readFile(path.join(repo,'services/community-presets/migrations',name),'utf8');
  for(const stmt of sql.split(';').map(s=>s.trim()).filter(Boolean))await db.prepare(stmt).run();
 }
 const ini='[CrimsonWeatherPreset]\n[Meta]\nFormatVersion=6\n[Weather]\nRain=0.2500\n';
 async function call(method,url,{body,owner=false,admin=false,status=200}={}){
  const headers={};if(owner)headers['x-cw-client-id']='review-install';if(admin)headers.authorization='Bearer review-token';if(body!==undefined)headers['content-type']='application/json';
  const res=await mf.dispatchFetch('http://review.test'+url,{method,headers,body:body===undefined?undefined:JSON.stringify(body)});
  const raw=await res.text();assert.equal(res.status,status,`${method} ${url}: ${raw}`);checks++;return res.headers.get('content-type')?.includes('json')?JSON.parse(raw):raw;
 }
 let catalog=await call('GET','/api/v1/catalog');assert.equal(catalog.presets.length,0);
 const submitted=await call('POST','/api/v1/presets',{owner:true,body:{title:'Review',authorName:'Author',iniText:ini}});
 assert.equal(submitted.status,'pending');const id=submitted.id;
 await call('GET',`/api/v1/presets/${id}/download`,{status:404});
 await call('POST',`/api/v1/admin/submissions/${id}/approve`,{status:401});
 await call('POST',`/api/v1/admin/submissions/${id}/approve`,{admin:true});
 catalog=await call('GET','/api/v1/catalog');assert.equal(catalog.presets[0].author,'Author');assert.equal(catalog.presets[0].id,id);
 let liked=await call('POST',`/api/v1/presets/${id}/like`,{owner:true});assert.equal(liked.liked,true);assert.equal(liked.likes,1);
 liked=await call('POST',`/api/v1/presets/${id}/like`,{owner:true});assert.equal(liked.liked,false);assert.equal(liked.likes,0);
 assert.equal(await call('GET',`/api/v1/presets/${id}/download`,{owner:true}),ini);
 await call('GET',`/api/v1/presets/${id}/download`,{owner:true});
 assert.equal((await db.prepare('SELECT downloads FROM presets WHERE id=?').bind(id).first()).downloads,1);
 await Promise.all(Array.from({length:20},()=>call('POST',`/api/v1/presets/${id}/like`,{owner:true})));
 const counts=await db.prepare('SELECT likes,(SELECT COUNT(*) FROM preset_likes WHERE preset_id=?) AS rows FROM presets WHERE id=?').bind(id,id).first();assert.equal(counts.likes,0);assert.equal(counts.rows,0);
 await Promise.all(Array.from({length:12},()=>call('GET',`/api/v1/presets/${id}/download`,{owner:true})));
 assert.equal((await db.prepare('SELECT downloads FROM presets WHERE id=?').bind(id).first()).downloads,1);
 const edit=await call('PUT',`/api/v1/me/presets/${id}`,{owner:true,body:{title:'Changed',authorName:'Author',iniText:ini.replace('0.2500','0.7500')}});assert.equal(edit.status,'pending');
 let own=await call('GET','/api/v1/me/presets',{owner:true});assert.equal(own.presets[0].pending_update_id,edit.updateId);assert.equal(own.presets[0].author_name,'Author');
 await call('DELETE',`/api/v1/me/presets/${id}/update`,{owner:true});
 await call('DELETE',`/api/v1/me/presets/${id}/update`,{owner:true,status:404});
 const second=await call('PUT',`/api/v1/me/presets/${id}`,{owner:true,body:{title:'Changed',authorName:'Author',iniText:ini.replace('0.2500','0.7500')}});
 await call('POST',`/api/v1/presets/${id}/like`,{owner:true});
 await call('POST',`/api/v1/admin/submissions/${second.updateId}/approve`,{admin:true});
 assert.equal(await call('GET',`/api/v1/presets/${id}/download`),ini.replace('0.2500','0.7500'));
 await call('DELETE',`/api/v1/me/presets/${id}`,{owner:true});
 catalog=await call('GET','/api/v1/catalog');assert.equal(catalog.presets.length,0);
 await call('GET',`/api/v1/presets/${id}/download`,{status:404});
 await call('POST',`/api/v1/admin/presets/${id}/restore`,{admin:true});
 catalog=await call('GET','/api/v1/catalog');assert.equal(catalog.presets.length,1);
 await call('POST',`/api/v1/admin/presets/${id}/purge`,{admin:true});
 catalog=await call('GET','/api/v1/catalog');assert.equal(catalog.presets.length,0);
 console.log(`Independent real Workers/D1/R2 lifecycle: ${checks} route checks passed (${entry})`);
}finally{await mf.dispose();}
