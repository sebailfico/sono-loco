#ifndef SETUP_PAGE_H
#define SETUP_PAGE_H

// The setup network's page (D19): served from flash by the node a phone joins,
// and nothing but a view of the mesh. Every change it makes is a command sent
// with `@<mac>` through /api/cmd, the same that a node on USB would send, and
// what it shows is what each node answered -- never what was sent.
//
// One file, no build step and nothing fetched from the internet: a phone on
// this network has none. Names go in through textContent, never innerHTML.

static const char SETUP_PAGE[] = R"PAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SonoLoco setup</title>
<style>
:root{--bg:#f6f5f2;--card:#fff;--ink:#1d1d1b;--dim:#6b6a66;--line:#e2e0da;--acc:#2f6f5e;--bad:#a33}
@media (prefers-color-scheme:dark){:root{--bg:#161614;--card:#21211f;--ink:#ecebe6;--dim:#9a9890;--line:#34332f;--acc:#6fbfa6;--bad:#e77}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.4 system-ui,sans-serif}
main{max-width:680px;margin:0 auto;padding:16px}h1{font-size:20px;margin:4px 0 2px}h2{font-size:15px;margin:0 0 8px}
.sub{color:var(--dim);font-size:13px}.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px;margin:12px 0}
.node{border-top:1px solid var(--line);padding:10px 0}.node:first-child{border-top:0}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:6px 0}
input[type=text],input[type=password]{flex:1;min-width:0;padding:7px 8px;border:1px solid var(--line);border-radius:6px;background:var(--bg);color:var(--ink);font:inherit}
input[type=range]{flex:1}button{padding:7px 12px;border:1px solid var(--acc);border-radius:6px;background:var(--acc);color:#fff;font:inherit}
button.q{background:none;color:var(--acc)}.st{font-size:12px;color:var(--dim);min-height:16px}.err{color:var(--bad)}
input{accent-color:var(--acc)}label{display:flex;gap:6px;align-items:center}.name{font-weight:600}.mono{font:12px ui-monospace,monospace;color:var(--dim)}
</style></head><body><main>
<h1 id="title">SonoLoco</h1><div class="sub" id="who"></div>
<div class="card" id="lock" hidden><h2>First, a password for this network</h2>
<div class="sub">Anyone nearby can join it until it has one, so nothing else is here until then. 8 to 63 characters. The network then restarts with it, and you join it again with the password.</div>
<div class="row"><input id="appass" type="password" autocomplete="new-password" placeholder="New password"><button class="q" id="appassShow" type="button">Show</button><button id="appassGo">Set</button></div><div class="st" id="appassSt"></div></div>
<div class="card" id="meshCard" hidden><div class="row" style="justify-content:space-between"><h2>Speakers</h2><button class="q" id="refresh">Refresh</button></div>
<div class="st" id="meshSt">Asking the mesh&hellip;</div><div id="nodes"></div></div>
<div class="card" id="wifiCard" hidden><h2>Home WiFi, for updates</h2>
<div class="sub">This node uses it only to take new firmware (update mode). Stored here, on this node.</div>
<div class="row"><input id="ssid" type="text" placeholder="Network name"></div>
<div class="row"><input id="wpass" type="password" placeholder="Password"><button class="q" id="wpassShow" type="button">Show</button><button id="wifiGo">Save</button></div><div class="st" id="wifiSt"></div></div>
<div class="row"><button class="q" id="close">Close the setup network</button></div>
</main><script>
const $=id=>document.getElementById(id);
async function post(url,body){const r=await fetch(url,{method:'POST',body});const t=await r.text();if(!r.ok)throw new Error(t.trim()||r.status);return t}
// What one node said, from the lines of an @ question: '[@name] line', and '[@name]+ rest' continuing one.
function answers(text){const out=[];let last=null;for(const l of text.split('\n')){const m=l.match(/^\[@([^\]]+)\](\+?) (.*)$/);if(!m)continue;
 if(m[2]&&last){last.line+=m[3]}else{last={name:m[1],line:m[3]};out.push(last)}}return out}
function fields(line){const f={};for(const m of line.matchAll(/(\w+)=(\S+)/g))f[m[1]]=m[2];return f}
async function ask(mac,cmd){const t=await post('/api/cmd','@'+mac+' '+cmd);const a=answers(t);
 if(!a.length)throw new Error(/retry/.test(t)?'no answer, after retries':'no answer');
 const last=a[a.length-1].line;if(/\berror\b/.test(last))throw new Error(last.replace(/^\[\w+\] /,''));return fields(last)}
function nodeCard(n){
 const d=document.createElement('div');d.className='node';
 const head=document.createElement('div');head.className='row';
 const nm=document.createElement('input');nm.type='text';nm.value=n.name;nm.maxLength=31;
 const save=document.createElement('button');save.className='q';save.textContent='Rename';head.append(nm,save);
 const info=document.createElement('div');info.className='mono';
 info.textContent=n.mac+'  '+n.mode+(n.src&&n.src!=='-'?' from '+n.src:'')+'  lost '+n.lost+'  fw '+n.fw;
 const trimRow=document.createElement('div');trimRow.className='row';
 const tl=document.createElement('span');tl.style.minWidth='72px';
 const tr=document.createElement('input');tr.type='range';tr.min=-40;tr.max=12;tr.step=1;tr.value=parseInt(n.trim);
 const show=()=>tl.textContent='Trim '+tr.value+' dB';show();tr.oninput=show;trimRow.append(tl,tr);
 const sw=document.createElement('div');sw.className='row';
 const mute=document.createElement('input');mute.type='checkbox';mute.checked=n.mute==='1';
 const mono=document.createElement('input');mono.type='checkbox';mono.checked=n.mono==='1';
 const l1=document.createElement('label');l1.append(mute,'Mute');const l2=document.createElement('label');l2.append(mono,'Mono (one speaker)');
 // Which box is this: two ding-dongs from that speaker, over its music and through a mute.
 const ring=document.createElement('button');ring.className='q';ring.textContent='Ring';ring.style.marginLeft='auto';
 sw.append(l1,l2,ring);
 const st=document.createElement('div');st.className='st';
 d.append(head,info,trimRow,sw,st);
 // A control shows what the node said: put back as it was when nobody answered, or the node refused.
 async function set(cmd,apply,revert){st.className='st';st.textContent='Asking '+n.name+'…';
  try{const f=await ask(n.mac,cmd);apply(f);st.textContent=n.name+' says '+Object.entries(f).map(e=>e.join('=')).join(' ')}
  catch(e){revert();st.className='st err';st.textContent=String(e.message||e)}}
 tr.onchange=()=>set('v'+tr.value,f=>{if(f.trim)n.trim=f.trim;tr.value=parseInt(n.trim);show()},()=>{tr.value=parseInt(n.trim);show()});
 mute.onchange=()=>set('m'+(mute.checked?1:0),f=>{if(f.mute)n.mute=f.mute;mute.checked=n.mute==='1'},()=>{mute.checked=n.mute==='1'});
 mono.onchange=()=>set('M'+(mono.checked?1:0),f=>{if(f.mono)n.mono=f.mono;mono.checked=n.mono==='1'},()=>{mono.checked=n.mono==='1'});
 save.onclick=()=>set('I'+nm.value.trim(),f=>{if(f.name)n.name=f.name;nm.value=n.name},()=>{nm.value=n.name});
 ring.onclick=()=>set('h',()=>{},()=>{});
 return d}
async function refresh(){$('meshSt').className='st';$('meshSt').textContent='Asking the mesh…';
 try{const t=await post('/api/cmd','@* N');const list=answers(t).map(a=>a.line).filter(l=>l.startsWith('[NODE]')).map(fields);
  const box=$('nodes');box.replaceChildren(...list.map(nodeCard));
  $('meshSt').textContent=list.length?list.length+' answered. A speaker that is off, out of range or in another mesh does not.':'Nobody answered.'}
 catch(e){$('meshSt').className='st err';$('meshSt').textContent=String(e.message||e)}}
let ssid='this setup network';
// Open: the password and nothing else, and the node refuses the rest too. Protected: everything.
async function info(){try{const r=await fetch('/api/info');const i=await r.json();$('title').textContent=i.name;ssid=i.name+'-setup';
 $('who').textContent='Mesh '+i.meshname+' ('+i.mesh+'), firmware '+i.fw;
 $('lock').hidden=i.protected;$('meshCard').hidden=!i.protected;$('wifiCard').hidden=!i.protected;
 if(i.wifi)$('ssid').value=i.wifi;if(i.protected)refresh()}catch(e){}}
// Show/Hide without closing the phone's keyboard: the button never takes the focus (a tap focuses
// on its pointerdown/mousedown, which is cancelled), and the field keeps it and its caret.
function reveal(input,btn){const keep=e=>e.preventDefault();btn.addEventListener('pointerdown',keep);btn.addEventListener('mousedown',keep);
 btn.onclick=()=>{const f=$(input),had=document.activeElement===f,a=f.selectionStart,b=f.selectionEnd,hid=f.type==='password';
  f.type=hid?'text':'password';btn.textContent=hid?'Hide':'Show';
  if(had){f.focus({preventScroll:true});try{f.setSelectionRange(a,b)}catch(e){}}}}
reveal('appass',$('appassShow'));reveal('wpass',$('wpassShow'));
async function form(id,url,body,done){const s=$(id);s.className='st';s.textContent='…';
 try{s.textContent=(await post(url,body)).trim();if(done)done()}catch(e){s.className='st err';s.textContent=String(e.message||e)}}
// After Set: the network restarts with the password and drops the phone. Count down to that, then
// ask the node every 2 s until it answers -- the phone back on, with the password -- and reload.
// A blind reload would land on the browser's own error page while the phone is off the network.
function el(tag,cls,text){return Object.assign(document.createElement(tag),{className:cls,textContent:text})}
async function backOn(){try{const c=new AbortController();setTimeout(()=>c.abort(),1500);
 const r=await fetch('/api/info',{cache:'no-store',signal:c.signal});return r.ok&&(await r.json()).protected}catch(e){return false}}
function afterPassword(ms){const now=el('div','st','');
 $('lock').replaceChildren(el('h2','','Password set'),
  el('div','sub','When '+ssid+' restarts, your phone is dropped from it and may go back to its usual WiFi. Open the WiFi settings and join '+ssid+' again, with the password.'),now,
  el('div','sub','Joined, and the page does not come back? Turn the phone’s WiFi off and on, join again, and open 192.168.1.1. A phone can keep sending everything over mobile data to a network with no internet.'));
 const end=Date.now()+ms;
 const tick=setInterval(()=>{const left=Math.ceil((end-Date.now())/1000);
  if(left>0){now.textContent='Restarting in '+left+' s…';return}
  clearInterval(tick);now.textContent='Restarted. Waiting for you to join it again — this page reloads by itself.';
  const poll=setInterval(async()=>{if(await backOn()){clearInterval(poll);now.textContent='Back on. Reloading…';location.reload()}},2000)},250)}
$('appassGo').onclick=async()=>{const s=$('appassSt');s.className='st';s.textContent='…';
 try{const t=await post('/api/appass',$('appass').value);$('appass').value='';
  const m=t.match(/restart_ms=(\d+)/);afterPassword(m?+m[1]:3000)}
 catch(e){s.className='st err';s.textContent=String(e.message||e)}};
$('wifiGo').onclick=()=>form('wifiSt','/api/wifi',$('ssid').value+'\n'+$('wpass').value,()=>{$('wpass').value='';$('wpass').type='password';$('wpassShow').textContent='Show'});
$('close').onclick=async()=>{try{await post('/api/close','')}catch(e){}document.body.textContent='The setup network is closing. You can leave this page.'};
$('refresh').onclick=refresh;info();
</script></body></html>
)PAGE";

#endif  // SETUP_PAGE_H
