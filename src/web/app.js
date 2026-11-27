// Shared page script. Served pre-gzipped: scripts/embed_assets.py packs it into assets_gz.h at build time.
// Show/hide button for password fields, placed inside the input (.pw in style.css)
document.querySelectorAll('input[type=password]').forEach(function(i){var w=document.createElement('span'),b=document.createElement('button');w.className='pw';b.type='button';b.innerHTML='&#128065;';b.onclick=function(){i.type=i.type=='password'?'text':'password'};i.before(w);w.append(i,b)});
// confirm() for forms with data-confirm
document.querySelectorAll('form[data-confirm]').forEach(function(f){f.onsubmit=function(){return confirm(f.dataset.confirm)}});
// On/off switches (.sw, toggleRow in web.cpp): the form is sent as soon as the switch moves
document.querySelectorAll('.sw input').forEach(function(i){i.onchange=function(){i.disabled=true;i.form.submit()}});
// Restart page (#rs): poll until the board answers, then open "/"
if(document.getElementById('rs'))setTimeout(function(){setInterval(function(){fetch('/',{cache:'no-store'}).then(function(){location='/'},function(){})},2000)},3000);
// Live values: elements with data-k get their text from /api/status every 5 s (only while the tab is visible).
// A value [text, cls] also sets the colour class.
var K=document.querySelectorAll('[data-k]');
if(document.querySelector('main [data-k]'))setInterval(function(){if(!document.hidden)fetch('/api/status',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){K.forEach(function(e){var v=j[e.dataset.k];if(v==null)return;if(v.pop){e.classList.remove('ok','warn','bad','mut');e.classList.add(v[1]);v=v[0]}e.textContent=v})},function(){})},5000);
// DNS query log (#lg): filter + pages of 20 in the browser, so the search covers the whole log.
// lgf(0) = first page (new filter), lgf(-1)/lgf(1) = newer/older; texts come from #lgn data-*
var lgP=0;function lgf(d){var q=document.getElementById('lf').value.toLowerCase(),o=document.getElementById('lb').checked,m=[],t=document.getElementById('lgn');document.querySelectorAll('#lg tr[data-d]').forEach(function(r){r.style.display='none';if(r.dataset.d.indexOf(q)>=0&&(!o||r.classList.contains('b')))m.push(r)});var n=Math.max(1,Math.ceil(m.length/20));lgP=d?Math.min(n-1,Math.max(0,lgP+d)):0;m.slice(lgP*20,lgP*20+20).forEach(function(r){r.style.display=''});t.textContent=t.dataset.a+(lgP+1)+t.dataset.b+n+t.dataset.c+m.length;document.getElementById('lgp').style.display=n>1?'':'none'}
if(document.getElementById('lg'))lgf(0);
// History charts (section[data-hist], cards from histStart/chart in web.cpp): /api/hist on load, every 30 s
// and when the tab becomes visible. Each .ch gets an inline SVG: 360 slots (1 h), newest on the right.
var H=document.querySelector('[data-hist]');
function hs(){if(!document.hidden)fetch('/api/hist?set='+H.dataset.hist,{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){H.querySelectorAll('.ch').forEach(function(c){chr(c,j)})},function(){})}
if(H){hs();setInterval(hs,30000);document.addEventListener('visibilitychange',hs)}
function chr(c,j){var d=H.dataset,ks=c.dataset.s.split(','),L=c.dataset.l.split('|'),v=[],P='',hd='';
ks.forEach(function(k){j[k].forEach(function(x){if(x!=null)v.push(x)})});
var mn=v.length?Math.min.apply(0,v):0,mx=v.length?Math.max.apply(0,v):1,lo=mn<0?mn-3:0,hi=mn<0?mx+3:Math.max(mx*1.15,1);
ks.forEach(function(k,i){var a=j[k],o=360-a.length,g='',sx,ex,cur=a[a.length-1],sm=0;
if(c.dataset.u){a.forEach(function(x){sm+=x||0});cur='Σ'+sm}
function e(){if(g)P+='<path class="a c'+i+'" d="M'+sx+' 100'+g+'L'+ex+' 100Z"/><path class=c'+i+' d="M'+g.slice(1)+'"/>';g=''}
a.forEach(function(x,n){if(x==null)return e();if(!g)sx=o+n;ex=o+n;g+='L'+ex+' '+(100-(x-lo)/(hi-lo)*100).toFixed(1)});e();
hd+='<b class=c'+i+'>'+(L[i]?L[i]+': ':'')+(cur==null?'—':cur)+'</b>'});
c.innerHTML='<div class=chh><span>'+c.dataset.t+'</span><span>'+hd+'</span></div><svg viewBox="0 0 359 100" preserveAspectRatio=none>'+P+'</svg><div class="chh chf"><span>'+d.a+'</span><span>'+(v.length?'↓'+mn+' ↑'+mx:'')+'</span><span>'+d.b+'</span></div>'}
// Online firmware update (#fw, Settings): while a job runs ([data-r]) poll /api/update, reload when it ends.
// Preset lists (#bl, DNS): while a list waits or downloads, poll and reload when the state code (j.bl) changes.
// #fwr = firmware installed: restart through the restart page.
var F=document.querySelector('#fw[data-r],#bl[data-r]');
if(F)setInterval(function(){fetch('/api/update',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){if(F.id=='bl'?j.bl!=F.dataset.r:j.state!='run')location.reload();else{var p=F.querySelector('[data-p]');if(p)p.textContent=j.progress+'%'}},function(){})},1000);
if(document.getElementById('fwr'))document.getElementById('fwr').submit();
