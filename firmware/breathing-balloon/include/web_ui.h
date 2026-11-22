#ifndef WEB_UI_H
#define WEB_UI_H

// 控制页面 HTML —— 保存在 PROGMEM（Flash）中，不占用 RAM
const char WEB_PAGE[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>创客星辰·呼吸气球控制系统</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{font-family:-apple-system,sans-serif;background:#0f172a;color:#e2e8f0;min-height:100vh;padding:16px;padding-bottom:80px}
.header{text-align:center;padding:16px 0}
.header h1{font-size:1.5em;color:#38bdf8}
.section{background:#1e293b;border-radius:16px;padding:16px;margin-top:16px}
.section h3{font-size:1em;color:#38bdf8;margin-bottom:12px;display:flex;align-items:center;gap:8px}
.row{display:flex;gap:10px;margin-bottom:10px}
.row:last-child{margin-bottom:0}
.btn{flex:1;padding:14px 10px;border:none;border-radius:12px;font-size:1em;font-weight:600;cursor:pointer;position:relative;transition:all 0.15s;min-height:64px;display:flex;flex-direction:column;align-items:center;justify-content:center}
.btn:active{transform:scale(0.96)}
.btn small{font-size:0.7em;font-weight:400;opacity:0.8;margin-top:2px}
.btn-inflate{background:#16a34a;color:#fff}
.btn-deflate{background:#2563eb;color:#fff}
.btn-auto{background:#8b5cf6;color:#fff}
.btn-stop{background:#dc2626;color:#fff;width:100%;padding:16px;font-size:1.1em}
.btn-stop:active{background:#b91c1c}
.btn-set{background:#334155;color:#e2e8f0}
.label{display:block;font-size:0.85em;color:#94a3b8;margin-bottom:6px}
.range-row{display:flex;align-items:center;gap:10px}
.range-row input[type=range]{flex:1}
.range-row input[type=number]{width:70px;background:#334155;border:1px solid #475569;color:#e2e8f0;border-radius:8px;padding:8px;text-align:center;font-size:16px}
input[type=range]{-webkit-appearance:none;height:6px;background:#334155;border-radius:3px;outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:22px;height:22px;border-radius:50%;background:#38bdf8;cursor:pointer}
.detail-row{display:flex;gap:10px;margin-top:10px}
.detail-row>div{flex:1}
.section h4{font-size:0.9em;color:#94a3b8;margin:12px 0 8px}
</style>
</head>
<body>

<div class="header">
  <h1>🎈 创客星辰·呼吸气球控制系统</h1>
</div>

<div class="section">
  <h3>🎮 控制</h3>
  <div class="row">
    <button class="btn btn-inflate" onclick="doCmd('inflate')">
      持续呼气<small>气球变大</small>
    </button>
    <button class="btn btn-deflate" onclick="doCmd('deflate')">
      持续吸气<small>气球变小</small>
    </button>
  </div>
  <div class="row">
    <button class="btn btn-auto" onclick="doCmd('auto')">
      开始自动呼吸
    </button>
  </div>
  <button class="btn btn-stop" onclick="doCmd('stop')">⏹ 停止</button>
</div>

<div class="section">
  <h3>🫁 呼吸参数</h3>
  <label class="label">呼吸频率：<span id="freqVal">12.5</span> 次/分钟</label>
  <div class="range-row">
    <input type="range" id="freqSlider" min="6" max="30" step="0.5" value="12.5">
    <input type="number" id="freqInput" min="6" max="30" step="0.5" value="12.5">
  </div>
  <h4>精细调节</h4>
  <div class="detail-row">
    <div>
      <label class="label">充气时间（秒）</label>
      <input type="number" id="inflateTime" min="0.1" max="60" step="0.1" value="2.8" style="width:100%;background:#334155;border:1px solid #475569;color:#e2e8f0;border-radius:8px;padding:8px;font-size:16px">
    </div>
    <div>
      <label class="label">抽气时间（秒）</label>
      <input type="number" id="deflateTime" min="0.1" max="60" step="0.1" value="2" style="width:100%;background:#334155;border:1px solid #475569;color:#e2e8f0;border-radius:8px;padding:8px;font-size:16px">
    </div>
  </div>
  <div class="row" style="margin-top:12px">
    <button class="btn btn-set" onclick="applySettings()">应用参数</button>
    <button class="btn btn-set" onclick="saveSettings()">保存参数</button>
    <button class="btn btn-set" onclick="resetSettings()">恢复默认</button>
  </div>
  <div id="pendingMsg" style="display:none;margin-top:8px;padding:8px;background:#854d0e;border-radius:8px;font-size:0.85em;text-align:center;color:#fef3c7">⏳ 参数待下周期生效</div>
</div>

<script>
let pollTimer=null;
let settingsDirty=false;
let editInflateMs=2800;
let editDeflateMs=2000;

const $=id=>document.getElementById(id);
function getNum(id){let v=parseFloat($(id).value);return isNaN(v)?null:v}

// 从内部毫秒值刷新页面显示（仅显示，不触发计算）
function refreshDisplay(){
  $('inflateTime').value=(editInflateMs/1000).toFixed(1);
  $('deflateTime').value=(editDeflateMs/1000).toFixed(1);
  let f=editInflateMs>0&&editDeflateMs>0?60000/(editInflateMs+editDeflateMs):12.5;
  if(f<6)f=6;if(f>30)f=30;
  $('freqSlider').value=f;
  $('freqInput').value=f.toFixed(1);
  $('freqVal').textContent=f.toFixed(1);
}

// 频率滑块/输入框改变 → 保持当前比例，精确计算毫秒
function syncFromFreq(){
  settingsDirty=true;
  let f=getNum('freqInput');
  if(f===null||f<6||f>30)return;
  let totalMs=Math.round(60000/f);
  let ratioI=editInflateMs,ratioD=editDeflateMs;
  if(ratioI<1||ratioD<1){ratioI=2800;ratioD=2000;}
  let newInflateMs=Math.round(totalMs*ratioI/(ratioI+ratioD));
  let newDeflateMs=totalMs-newInflateMs;
  editInflateMs=newInflateMs;
  editDeflateMs=newDeflateMs;
  $('inflateTime').value=(editInflateMs/1000).toFixed(1);
  $('deflateTime').value=(editDeflateMs/1000).toFixed(1);
  $('freqVal').textContent=f;
}

// 精细调节充气/抽气时间 → 重新计算频率
function syncFromDetail(){
  settingsDirty=true;
  let i=getNum('inflateTime'),d=getNum('deflateTime');
  if(i===null||d===null||i<=0||d<=0)return;
  editInflateMs=Math.round(i*1000);
  editDeflateMs=Math.round(d*1000);
  let f=60000/(editInflateMs+editDeflateMs);
  if(f<6)f=6;if(f>30)f=30;
  $('freqSlider').value=f;
  $('freqInput').value=f.toFixed(1);
  $('freqVal').textContent=f.toFixed(1);
}

function updateUI(s){
  if(!s)return;
  $('pendingMsg').style.display=s.pendingUpdate?'block':'none';
  if(!settingsDirty){
    editInflateMs=s.inflate_ms;
    editDeflateMs=s.deflate_ms;
    refreshDisplay();
  }
}

function api(path,body){return fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body||{})}).then(r=>r.json())}

function doCmd(cmd){
  api('/api/control',{action:cmd}).then(r=>{
    if(!r.ok)alert('操作失败: '+(r.error||'未知'));
  }).catch(()=>{});
}

function applySettings(){
  let body={
    inflate_ms:editInflateMs,
    deflate_ms:editDeflateMs
  };
  api('/api/settings',body).then(r=>{
    if(!r.ok)alert('参数错误: '+(r.error||''));
    else settingsDirty=false;
  });
}

function saveSettings(){if(!confirm('确认保存参数？断电后将保留。'))return;api('/api/save').then(r=>{if(!r.ok)alert('保存失败: '+(r.error||''))})}

function resetSettings(){
  if(!confirm('恢复默认参数？'))return;
  editInflateMs=2800;editDeflateMs=2000;
  refreshDisplay();
  let body={inflate_ms:2800,deflate_ms:2000};
  api('/api/settings',body).then(r=>{
    if(!r.ok)alert('参数错误: '+(r.error||''));
    else settingsDirty=false;
  });
}

function poll(){
  fetch('/api/status').then(r=>r.json()).then(s=>{
    updateUI(s);
  }).catch(()=>{});
}

// event bindings
$('freqSlider').addEventListener('input',()=>{$('freqInput').value=$('freqSlider').value;syncFromFreq()});
$('freqInput').addEventListener('input',()=>{$('freqSlider').value=$('freqInput').value;syncFromFreq()});
$('inflateTime').addEventListener('input',syncFromDetail);
$('deflateTime').addEventListener('input',syncFromDetail);

pollTimer=setInterval(poll,1000);
poll();
</script>
</body>
</html>)rawhtml";

#endif
