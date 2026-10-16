#pragma once
// Embedded pages for `iw8-zonetool sim serve` (live free camera, frames
// rendered by the simulator) and `sim panorama` (static equirectangular
// walk-through that works from anywhere, e.g. published as an artifact).
namespace sim
{
std::string ToolsDirectory();

inline const char *kViewerHtml = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Map simulator</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root{--bg:#111;--fg:#ddd;--panel:#1c1c1ce6;--accent:#f0b429}
html,body{margin:0;height:100%;background:var(--bg);color:var(--fg);font:13px/1.4 system-ui,sans-serif;overflow:hidden}
#view{position:absolute;inset:0;width:100%;height:100%;object-fit:contain;background:#000;cursor:crosshair}
#hud{position:absolute;left:12px;top:12px;background:var(--panel);padding:10px 12px;border-radius:8px;max-width:360px}
#hud h1{font-size:14px;margin:0 0 6px;color:var(--accent)}
#hud label{display:block;margin:4px 0}
#hud select,#hud input,#hud button{font:inherit;background:#222;color:var(--fg);border:1px solid #444;border-radius:4px;padding:3px 6px}
#hud button{cursor:pointer}
#status{position:absolute;left:12px;bottom:12px;background:var(--panel);padding:8px 12px;border-radius:8px;font-family:ui-monospace,monospace;white-space:pre}
#pick{position:absolute;right:12px;top:12px;background:var(--panel);padding:10px 12px;border-radius:8px;max-width:420px;font-family:ui-monospace,monospace;white-space:pre-wrap;display:none}
#cross{position:absolute;left:50%;top:50%;width:14px;height:14px;margin:-7px;border:1px solid #fff8;border-radius:50%;pointer-events:none}
.row{display:flex;gap:6px;flex-wrap:wrap;align-items:center}
details{margin-top:6px}
</style></head><body>
<img id="view" alt="">
<div id="cross"></div>
<div id="hud">
<h1 id="title">Map simulator</h1>
<div class="row">Spawn <select id="spawn"></select> <button id="go">Go</button></div>
<div class="row">View <select id="mode">
<option value="exact">exact (zone shaders)</option><option value="material">material id</option>
<option value="surface">surface / instance id</option><option value="cull">static cull class</option>
<option value="normals">normals</option><option value="depth">depth</option></select>
 Exposure <input id="exposure" type="range" min="0.1" max="4" step="0.05" value="1" style="width:90px"><span id="expv">1.00</span></div>
<div class="row"><label><input type="checkbox" id="world" checked> world</label><label><input type="checkbox" id="models" checked> models</label>
<label><input type="checkbox" id="dynamic" checked> dynents</label><label><input type="checkbox" id="glass" checked> glass</label>
<label><input type="checkbox" id="flip"> flip winding</label> LOD <select id="lod"><option value="-1">auto</option><option>0</option><option>1</option><option>2</option><option>3</option></select></div>
<div class="row"><button id="pickbtn">Pick at crosshair (P)</button><button id="checkbtn">Run load checks</button><button id="shot">Save PNG</button></div>
<details><summary>Controls</summary>Click the image to capture the mouse. Mouse look; W/A/S/D move, Q/E down/up, Shift fast, Ctrl slow. Esc releases. Type a camera below and press Enter.
<div class="row">Camera <input id="cam" style="width:260px" placeholder="x,y,z,yaw,pitch"></div></details>
</div>
<div id="status"></div>
<div id="pick"></div>
<script>
const S={x:0,y:0,z:0,yaw:0,pitch:0,fov:65,keys:{},busy:false,dirty:true,lastT:0,info:null,fps:0};
const $=id=>document.getElementById(id);
const view=$('view');
function params(w,h){return `x=${S.x.toFixed(1)}&y=${S.y.toFixed(1)}&z=${S.z.toFixed(1)}&yaw=${S.yaw.toFixed(2)}&pitch=${S.pitch.toFixed(2)}&fov=${S.fov}&w=${w}&h=${h}&view=${$('mode').value}&exposure=${$('exposure').value}&lod=${$('lod').value}&world=${+$('world').checked}&models=${+$('models').checked}&dynamic=${+$('dynamic').checked}&glass=${+$('glass').checked}&flip=${+$('flip').checked}`}
let moving=false, idleTimer=null, lastUrl=null;
async function frame(){
  if(S.busy) return; S.busy=true;
  const w=moving?960:1600, h=moving?540:900;
  const t0=performance.now();
  try{
    const r=await fetch('/frame?'+params(w,h)+'&q='+(moving?70:88));
    const b=await r.blob();
    if(lastUrl) URL.revokeObjectURL(lastUrl);
    lastUrl=URL.createObjectURL(b); view.src=lastUrl;
    S.fps=Math.round(1000/(performance.now()-t0));
  }catch(e){ $('status').textContent='frame failed: '+e; }
  S.busy=false; S.dirty=false;
  status();
}
function status(){
  $('status').textContent=`pos ${S.x.toFixed(0)} ${S.y.toFixed(0)} ${S.z.toFixed(0)}   yaw ${S.yaw.toFixed(1)}  pitch ${S.pitch.toFixed(1)}   ${S.fps} fps${moving?' (preview)':''}`;
  $('cam').value=`${S.x.toFixed(0)},${S.y.toFixed(0)},${S.z.toFixed(0)},${S.yaw.toFixed(1)},${S.pitch.toFixed(1)}`;
}
function tick(t){
  const dt=Math.min((t-S.lastT)/1000,0.1); S.lastT=t;
  const k=S.keys; let speed=500*(k.shift?4:1)*(k.ctrl?0.25:1);
  const yaw=S.yaw*Math.PI/180, pitch=S.pitch*Math.PI/180;
  const f=[Math.cos(pitch)*Math.cos(yaw),Math.cos(pitch)*Math.sin(yaw),-Math.sin(pitch)];
  const r=[Math.sin(yaw),-Math.cos(yaw),0];
  let mv=false;
  const add=(v,s)=>{S.x+=v[0]*s;S.y+=v[1]*s;S.z+=v[2]*s;mv=true};
  if(k.w) add(f,speed*dt); if(k.s) add(f,-speed*dt); if(k.d) add(r,speed*dt); if(k.a) add(r,-speed*dt);
  if(k.e) S.z+=speed*dt,mv=true; if(k.q) S.z-=speed*dt,mv=true;
  if(mv){S.dirty=true; moving=true; clearTimeout(idleTimer); idleTimer=setTimeout(()=>{moving=false;S.dirty=true},250);}
  if(S.dirty) frame();
  requestAnimationFrame(tick);
}
view.addEventListener('click',()=>view.requestPointerLock());
document.addEventListener('mousemove',e=>{
  if(document.pointerLockElement!==view) return;
  S.yaw-=e.movementX*0.15; S.pitch=Math.max(-89,Math.min(89,S.pitch+e.movementY*0.15));
  S.dirty=true; moving=true; clearTimeout(idleTimer); idleTimer=setTimeout(()=>{moving=false;S.dirty=true},250);
});
document.addEventListener('keydown',e=>{
  if(e.target.tagName==='INPUT'||e.target.tagName==='SELECT') return;
  const key=e.key.toLowerCase(); S.keys[key]=true; S.keys.shift=e.shiftKey; S.keys.ctrl=e.ctrlKey;
  if(key==='p') pick();
});
document.addEventListener('keyup',e=>{const key=e.key.toLowerCase(); S.keys[key]=false; S.keys.shift=e.shiftKey; S.keys.ctrl=e.ctrlKey;});
for(const id of ['mode','exposure','world','models','dynamic','glass','flip','lod']) $(id).addEventListener('change',()=>{S.dirty=true;$('expv').textContent=(+$('exposure').value).toFixed(2)});
$('exposure').addEventListener('input',()=>{$('expv').textContent=(+$('exposure').value).toFixed(2)});
$('cam').addEventListener('keydown',e=>{ if(e.key!=='Enter') return; const v=$('cam').value.split(/[ ,]+/).map(Number); if(v.length>=5){[S.x,S.y,S.z,S.yaw,S.pitch]=v;S.dirty=true;} });
$('go').addEventListener('click',()=>{const s=S.info.spawns[+$('spawn').value]; if(!s) return; [S.x,S.y,S.z]=[s.origin[0],s.origin[1],s.origin[2]+60]; S.yaw=s.yaw; S.pitch=0; S.dirty=true;});
async function pick(){
  const rect=view.getBoundingClientRect(); const w=1600,h=900;
  const r=await fetch('/pick?'+params(w,h)+`&px=${w/2}&py=${h/2}`); const j=await r.json();
  $('pick').style.display='block'; $('pick').textContent=JSON.stringify(j,null,1); S.dirty=true;
}
$('pickbtn').addEventListener('click',pick);
$('checkbtn').addEventListener('click',async()=>{$('pick').style.display='block';$('pick').textContent='running load checks (ACTS, preload plans, script links)...';
  const r=await fetch('/check'); const t=await r.text(); try{$('pick').textContent=JSON.stringify(JSON.parse(t),null,1)}catch(e){$('pick').textContent=t}});
$('shot').addEventListener('click',()=>{const a=document.createElement('a');a.href='/frame?'+params(1920,1080)+'&q=95';a.download='sim.jpg';a.click();});
fetch('/info').then(r=>r.json()).then(info=>{
  S.info=info; $('title').textContent=info.map+' simulator';
  const sel=$('spawn'); info.spawns.forEach((s,i)=>{const o=document.createElement('option');o.value=i;o.textContent=`${i} ${s.classname} (${s.origin.map(v=>v.toFixed(0)).join(' ')})`;sel.appendChild(o);});
  if(info.spawns.length){const s=info.spawns[0];[S.x,S.y,S.z]=[s.origin[0],s.origin[1],s.origin[2]+60];S.yaw=s.yaw;}
  else {S.x=(info.boundsMin[0]+info.boundsMax[0])/2;S.y=(info.boundsMin[1]+info.boundsMax[1])/2;S.z=(info.boundsMin[2]+info.boundsMax[2])/2;}
  S.dirty=true; requestAnimationFrame(tick);
});
</script></body></html>
)HTML";

inline const char *kPanoramaHtml = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Map walk-through</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root{--bg:#111;--fg:#ddd;--panel:#1c1c1ce6;--accent:#f0b429}
html,body{margin:0;height:100%;background:var(--bg);color:var(--fg);font:13px/1.4 system-ui,sans-serif;overflow:hidden}
canvas#gl{position:absolute;inset:0;width:100%;height:100%;touch-action:none;cursor:grab}
#hud{position:absolute;left:12px;top:12px;background:var(--panel);padding:10px 12px;border-radius:8px;max-width:340px}
#hud h1{font-size:14px;margin:0 0 6px;color:var(--accent)}
#hud button,#hud select{font:inherit;background:#222;color:var(--fg);border:1px solid #444;border-radius:4px;padding:4px 8px;cursor:pointer}
#map{position:absolute;right:12px;bottom:12px;background:var(--panel);border-radius:8px;padding:6px}
#map canvas{display:block}
#status{position:absolute;left:12px;bottom:12px;background:var(--panel);padding:6px 10px;border-radius:8px;font-family:ui-monospace,monospace}
.row{display:flex;gap:6px;flex-wrap:wrap;align-items:center;margin-top:6px}
</style></head><body>
<canvas id="gl"></canvas>
<div id="hud"><h1 id="title">Map walk-through</h1>
<div id="label"></div>
<div class="row"><button id="prev">&larr; prev</button><select id="point"></select><button id="next">next &rarr;</button></div>
<div class="row">Drag to look around, wheel to zoom. Arrow keys / N and B switch points, or click a dot on the map.</div>
</div>
<div id="map"><canvas id="mapc" width="240" height="240"></canvas></div>
<div id="status"></div>
<script>
const $=id=>document.getElementById(id);
const canvas=$('gl'); const gl=canvas.getContext('webgl');
const vsrc=`attribute vec2 p;varying vec2 v;void main(){v=p;gl_Position=vec4(p,0.,1.);}`;
const fsrc=`precision highp float;varying vec2 v;uniform sampler2D tex;uniform float yaw,pitch,tanx,tany;
void main(){
 float cy=cos(yaw),sy=sin(yaw),cp=cos(pitch),sp=sin(pitch);
 vec3 f=vec3(cp*cy,cp*sy,-sp); vec3 r=vec3(sy,-cy,0.); vec3 u=cross(r,f);
 vec3 d=normalize(f+r*v.x*tanx+u*v.y*tany);
 float lon=atan(d.y,d.x), lat=asin(clamp(d.z,-1.,1.));
 gl_FragColor=texture2D(tex,vec2(lon/6.2831853+0.5,0.5-lat/3.14159265));
}`;
function shader(t,s){const o=gl.createShader(t);gl.shaderSource(o,s);gl.compileShader(o);return o;}
const prog=gl.createProgram();gl.attachShader(prog,shader(gl.VERTEX_SHADER,vsrc));gl.attachShader(prog,shader(gl.FRAGMENT_SHADER,fsrc));gl.linkProgram(prog);gl.useProgram(prog);
const buf=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,buf);gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,-1,1,1,1]),gl.STATIC_DRAW);
const loc=gl.getAttribLocation(prog,'p');gl.enableVertexAttribArray(loc);gl.vertexAttribPointer(loc,2,gl.FLOAT,false,0,0);
const U={yaw:gl.getUniformLocation(prog,'yaw'),pitch:gl.getUniformLocation(prog,'pitch'),tanx:gl.getUniformLocation(prog,'tanx'),tany:gl.getUniformLocation(prog,'tany')};
const tex=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,tex);
gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);
gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
gl.texImage2D(gl.TEXTURE_2D,0,gl.RGB,1,1,0,gl.RGB,gl.UNSIGNED_BYTE,new Uint8Array([20,20,20]));
const S={yaw:0,pitch:0,fov:80,index:0,points:[],loaded:false};
function resize(){const dpr=Math.min(devicePixelRatio||1,2);canvas.width=innerWidth*dpr;canvas.height=innerHeight*dpr;gl.viewport(0,0,canvas.width,canvas.height);draw();}
function draw(){
 const aspect=canvas.width/canvas.height; const tanx=Math.tan(S.fov*Math.PI/360);
 gl.uniform1f(U.yaw,S.yaw*Math.PI/180);gl.uniform1f(U.pitch,S.pitch*Math.PI/180);gl.uniform1f(U.tanx,tanx);gl.uniform1f(U.tany,tanx/aspect);
 gl.drawArrays(gl.TRIANGLE_STRIP,0,4);
 const p=S.points[S.index]; if(p) $('status').textContent=`${p.name}  (${p.origin.map(v=>v.toFixed(0)).join(' ')})   yaw ${S.yaw.toFixed(0)}  pitch ${S.pitch.toFixed(0)}`;
}
function load(i){
 if(!S.points.length) return; S.index=(i+S.points.length)%S.points.length; const p=S.points[S.index];
 $('point').value=S.index; S.yaw=p.yaw||0; S.pitch=0;
 const img=new Image(); img.onload=()=>{gl.bindTexture(gl.TEXTURE_2D,tex);gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL,false);gl.texImage2D(gl.TEXTURE_2D,0,gl.RGB,gl.RGB,gl.UNSIGNED_BYTE,img);draw();drawMap();};
 img.src=p.file; drawMap();
}
function drawMap(){
 const c=$('mapc'),ctx=c.getContext('2d'); ctx.fillStyle='#000';ctx.fillRect(0,0,c.width,c.height);
 if(!S.points.length) return;
 const xs=S.points.map(p=>p.origin[0]),ys=S.points.map(p=>p.origin[1]);
 const minx=Math.min(...xs)-200,maxx=Math.max(...xs)+200,miny=Math.min(...ys)-200,maxy=Math.max(...ys)+200;
 const sc=Math.min(c.width/(maxx-minx),c.height/(maxy-miny));
 const X=x=>(x-minx)*sc+(c.width-(maxx-minx)*sc)/2, Y=y=>c.height-((y-miny)*sc+(c.height-(maxy-miny)*sc)/2);
 S.points.forEach((p,i)=>{ctx.beginPath();ctx.arc(X(p.origin[0]),Y(p.origin[1]),i===S.index?6:4,0,7);ctx.fillStyle=i===S.index?'#f0b429':'#888';ctx.fill();});
 const p=S.points[S.index]; const a=S.yaw*Math.PI/180;
 ctx.strokeStyle='#f0b429';ctx.beginPath();ctx.moveTo(X(p.origin[0]),Y(p.origin[1]));ctx.lineTo(X(p.origin[0]+Math.cos(a)*300),Y(p.origin[1]+Math.sin(a)*300));ctx.stroke();
 c.onclick=e=>{const r=c.getBoundingClientRect();const mx=(e.clientX-r.left)*c.width/r.width,my=(e.clientY-r.top)*c.height/r.height;let best=0,bd=1e9;S.points.forEach((p,i)=>{const d=(X(p.origin[0])-mx)**2+(Y(p.origin[1])-my)**2;if(d<bd){bd=d;best=i;}});load(best);};
}
let drag=null;
canvas.addEventListener('pointerdown',e=>{drag={x:e.clientX,y:e.clientY,yaw:S.yaw,pitch:S.pitch};canvas.setPointerCapture(e.pointerId);});
canvas.addEventListener('pointermove',e=>{if(!drag) return;const k=0.15*S.fov/80;S.yaw=drag.yaw+(e.clientX-drag.x)*k;S.pitch=Math.max(-89,Math.min(89,drag.pitch+(e.clientY-drag.y)*k));draw();drawMap();});
canvas.addEventListener('pointerup',()=>drag=null);
canvas.addEventListener('wheel',e=>{S.fov=Math.max(30,Math.min(110,S.fov+Math.sign(e.deltaY)*5));draw();});
document.addEventListener('keydown',e=>{if(e.key==='ArrowRight'||e.key==='n') load(S.index+1); if(e.key==='ArrowLeft'||e.key==='b') load(S.index-1);});
$('prev').onclick=()=>load(S.index-1);$('next').onclick=()=>load(S.index+1);$('point').onchange=()=>load(+$('point').value);
addEventListener('resize',resize);
fetch('index.json').then(r=>r.json()).then(j=>{S.points=j.points;$('title').textContent=(j.map||'map')+' walk-through';$('label').textContent=j.label||'';
 j.points.forEach((p,i)=>{const o=document.createElement('option');o.value=i;o.textContent=p.name;$('point').appendChild(o);});resize();load(0);});
</script></body></html>
)HTML";
} // namespace sim
