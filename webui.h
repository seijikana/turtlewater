#pragma once
#include <pgmspace.h>

// Web UI — スマホ対応 (max-width 480px)
// 3秒ポーリング、5ステップ進捗バー、センサー表示、スケジュール/タイマー/SwitchBot/WiFi設定
const char HTML_PAGE[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html lang="ja"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>亀水槽 自動水替え</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:sans-serif;background:#1a1a2e;color:#eee;max-width:480px;margin:0 auto;padding:10px}
h2{color:#00d4ff;margin:10px 0 6px;font-size:.95rem;letter-spacing:.05em}
.card{background:#16213e;border-radius:8px;padding:12px;margin-bottom:10px}
#status-text{font-size:1.1rem;font-weight:bold;margin-bottom:8px;min-height:1.4em}
.prog-wrap{background:#0f3460;border-radius:4px;height:18px;margin-bottom:8px;position:relative}
.prog-bar{background:#00d4ff;height:100%;border-radius:4px;transition:width .5s;min-width:0}
.prog-label{position:absolute;right:6px;top:0;height:100%;display:flex;align-items:center;font-size:.7rem;color:#fff}
.sensors{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:10px}
.snsr{background:#0f3460;padding:4px 10px;border-radius:4px;font-size:.82rem;transition:background .3s}
.snsr.on{background:#00d4ff;color:#000;font-weight:bold}
.btn-row{display:flex;gap:8px;flex-wrap:wrap}
button{border:none;padding:10px 14px;border-radius:6px;cursor:pointer;font-size:.88rem;font-weight:bold;touch-action:manipulation}
.b-auto{background:#00d4ff;color:#000}.b-fill{background:#28a745;color:#fff}
.b-drain{background:#fd7e14;color:#fff}.b-stop{background:#dc3545;color:#fff}
.b-sm{background:#0f3460;color:#eee;padding:7px 11px;font-size:.8rem}
label{display:block;margin-bottom:4px;font-size:.82rem}
input[type=text],input[type=password],input[type=number],input[type=time]{
  width:100%;background:#0f3460;border:1px solid #334;color:#eee;
  padding:6px 8px;border-radius:4px;margin-bottom:8px;font-size:.85rem}
.srow{display:flex;gap:6px;align-items:center;margin-bottom:6px;flex-wrap:wrap}
.dow-wrap{display:flex;gap:4px;flex-wrap:wrap}
.dow-wrap label{display:flex;align-items:center;gap:2px;font-size:.78rem;margin:0;cursor:pointer}
.save-btn{background:#00d4ff;color:#000;width:100%;padding:8px;border-radius:5px;margin-top:4px;font-size:.88rem}
table{width:100%;border-collapse:collapse;font-size:.78rem}
th,td{text-align:left;padding:4px 5px;border-bottom:1px solid #0f3460}
th{color:#00d4ff}
.ok{color:#5dde7a}.err{color:#ff6b6b}.warn{color:#ffa94d}
.msg{color:#5dde7a;font-size:.8rem;min-height:1.1em;margin-top:2px}
.sep{border-top:1px solid #0f3460;margin:8px 0}
</style>
</head><body>

<div class="card">
  <div id="status-text">接続中…</div>
  <div class="prog-wrap">
    <div class="prog-bar" id="prog-bar" style="width:0%"></div>
    <span class="prog-label" id="prog-label"></span>
  </div>
  <div class="sensors">
    <span class="snsr" id="s-ah">A_H ⬜</span>
    <span class="snsr" id="s-al">A_L ⬜</span>
    <span class="snsr" id="s-bh">B_H ⬜</span>
    <span class="snsr" id="s-bl">B_L ⬜</span>
  </div>
  <div class="btn-row">
    <button class="b-auto" onclick="apiPost('/api/start')">▶ 自動</button>
    <button class="b-fill" onclick="apiPost('/api/fill')">💧 給水</button>
    <button class="b-drain" onclick="apiPost('/api/drain')">🚿 排水</button>
    <button class="b-stop" onclick="apiPost('/api/stop')">⛔ 緊急停止</button>
  </div>
</div>

<div class="card">
  <h2>手動制御</h2>
  <div class="btn-row">
    <button class="b-sm" onclick="manual('v1',1)">V1 ON</button>
    <button class="b-sm" onclick="manual('v1',0)">V1 OFF</button>
    <button class="b-sm" onclick="manual('v2',1)">V2 ON</button>
    <button class="b-sm" onclick="manual('v2',0)">V2 OFF</button>
    <button class="b-sm" onclick="manual('pump',1)">Pump ON</button>
    <button class="b-sm" onclick="manual('pump',0)">Pump OFF</button>
  </div>
</div>

<div class="card">
  <h2>実行履歴</h2>
  <table><thead><tr><th>日時</th><th>結果</th><th>時間</th><th>契機</th></tr></thead>
  <tbody id="hist"></tbody></table>
</div>

<div class="card">
  <h2>スケジュール</h2>
  <div id="sched-wrap"></div>
  <div class="msg" id="m-sched"></div>
  <button class="save-btn" onclick="saveSched()">保存</button>
</div>

<div class="card">
  <h2>タイムアウト設定（秒）</h2>
  <label>排水最大時間<input type="number" id="t-drain" min="10" max="3600"></label>
  <label>移送最大時間<input type="number" id="t-transfer" min="10" max="3600"></label>
  <label>給水最大時間<input type="number" id="t-fill" min="10" max="3600"></label>
  <label>低水位補給 追加給水時間（AL&amp;BL ON後）<input type="number" id="t-extra" min="0" max="300"></label>
  <label>センサーデバウンス時間（秒）<input type="number" id="t-sdb" min="0" max="30"></label>
  <div class="msg" id="m-timer"></div>
  <button class="save-btn" onclick="saveTimers()">保存</button>
</div>

<div class="card">
  <h2>SwitchBot 設定</h2>
  <label>Token<input type="password" id="sb-tok" placeholder="(未変更)"></label>
  <label>Secret<input type="password" id="sb-sec" placeholder="(未変更)"></label>
  <label>Device ID<input type="text" id="sb-dev"></label>
  <div class="msg" id="m-sb"></div>
  <button class="save-btn" onclick="saveSB()">保存</button>
</div>

<div class="card">
  <h2>Wi-Fi 設定</h2>
  <label>SSID<input type="text" id="wf-ssid"></label>
  <label>Password<input type="password" id="wf-pass"></label>
  <div class="msg" id="m-wf"></div>
  <button class="save-btn" onclick="saveWifi()">保存して再起動</button>
</div>

<script>
const DOW=['日','月','火','水','木','金','土'];
let _sched=null;

function buildSchedUI(){
  const w=document.getElementById('sched-wrap');
  w.innerHTML='';
  for(let i=0;i<4;i++){
    const s=_sched?_sched[i]:{enabled:false,dow:0,hour:8,minute:0};
    const dow=DOW.map((d,j)=>`<label><input type="checkbox" class="sd${i}" data-j="${j}"${(s.dow>>j)&1?' checked':''}>${d}</label>`).join('');
    const hh=String(s.hour).padStart(2,'0'),mm=String(s.minute).padStart(2,'0');
    w.innerHTML+=`<div class="srow">
      <input type="checkbox" id="se${i}"${s.enabled?' checked':''}> スロット${i+1}
      <div class="dow-wrap">${dow}</div>
      <input type="time" id="st${i}" value="${hh}:${mm}" style="width:auto">
    </div>`;
  }
}

async function poll(){
  try{
    const d=await(await fetch('/api/status')).json();
    const running=d.running;
    document.getElementById('status-text').textContent=
      running?`実行中 Step ${d.step}`:
      (d.status==='error'?'⚠ エラー（要確認）':'✅ 待機中');
    const p=d.progress||0;
    document.getElementById('prog-bar').style.width=p+'%';
    document.getElementById('prog-label').textContent=p>0?p+'%':'';
    const S=d.sensors||{};
    ['ah','al','bh','bl'].forEach(k=>{
      const el=document.getElementById('s-'+k);
      const v=S[k];
      el.textContent=k.toUpperCase()+(v?' 💧':' ⬜');
      el.className='snsr'+(v?' on':'');
    });
    const tb=document.getElementById('hist');
    tb.innerHTML=(d.history||[]).map(h=>{
      const dt=new Date(h.ts*1000);
      const ts=`${dt.getMonth()+1}/${dt.getDate()} ${String(dt.getHours()).padStart(2,'0')}:${String(dt.getMinutes()).padStart(2,'0')}`;
      const cls=h.result==='completed'?'ok':h.result==='stopped'?'warn':'err';
      return`<tr><td>${ts}</td><td class="${cls}">${h.result}</td><td>${h.duration}s</td><td>${h.trigger}</td></tr>`;
    }).join('');
  }catch(e){}
}

async function apiPost(url){
  try{const r=await fetch(url,{method:'POST'});if(!r.ok)alert('Error: '+r.status);}catch(e){}
  poll();
}
async function manual(dev,val){
  try{await fetch(`/api/m/${dev}/${val}`,{method:'POST'});}catch(e){}
}

async function loadTimers(){
  try{const d=await(await fetch('/api/timers')).json();
    document.getElementById('t-drain').value=d.drain_max;
    document.getElementById('t-transfer').value=d.transfer_max;
    document.getElementById('t-fill').value=d.fill_max;
    document.getElementById('t-extra').value=d.fill_extra;
    document.getElementById('t-sdb').value=d.sensor_db;
  }catch(e){}
}
async function saveTimers(){
  const body=JSON.stringify({
    drain_max:+document.getElementById('t-drain').value,
    transfer_max:+document.getElementById('t-transfer').value,
    fill_max:+document.getElementById('t-fill').value,
    fill_extra:+document.getElementById('t-extra').value,
    sensor_db:+document.getElementById('t-sdb').value
  });
  const r=await fetch('/api/timers',{method:'POST',headers:{'Content-Type':'application/json'},body});
  document.getElementById('m-timer').textContent=r.ok?'✔ 保存しました':'✘ エラー';
}

async function loadSched(){
  try{_sched=await(await fetch('/api/sched')).json();buildSchedUI();}catch(e){buildSchedUI();}
}
async function saveSched(){
  const slots=[];
  for(let i=0;i<4;i++){
    let dow=0;
    document.querySelectorAll(`.sd${i}`).forEach(cb=>{if(cb.checked)dow|=(1<<+cb.dataset.j);});
    const t=document.getElementById(`st${i}`).value.split(':');
    slots.push({enabled:document.getElementById(`se${i}`).checked,dow,hour:+t[0],minute:+t[1]});
  }
  const r=await fetch('/api/sched',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(slots)});
  document.getElementById('m-sched').textContent=r.ok?'✔ 保存しました':'✘ エラー';
}

async function loadSB(){
  try{const d=await(await fetch('/api/config')).json();
    document.getElementById('sb-tok').placeholder=d.switchbot_token||'(未設定)';
    document.getElementById('sb-dev').value=d.switchbot_device_id||'';
  }catch(e){}
}
async function saveSB(){
  const body=JSON.stringify({
    switchbot_token:document.getElementById('sb-tok').value,
    switchbot_secret:document.getElementById('sb-sec').value,
    switchbot_device_id:document.getElementById('sb-dev').value
  });
  const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body});
  document.getElementById('m-sb').textContent=r.ok?'✔ 保存しました':'✘ エラー';
  if(r.ok){document.getElementById('sb-tok').value='';document.getElementById('sb-sec').value='';}
}

async function saveWifi(){
  const ssid=document.getElementById('wf-ssid').value;
  if(!ssid){alert('SSIDを入力してください');return;}
  const body=JSON.stringify({ssid,pass:document.getElementById('wf-pass').value});
  const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body});
  document.getElementById('m-wf').textContent=r.ok?'✔ 保存 → 再起動中…':'✘ エラー';
}

poll();loadTimers();loadSched();loadSB();
setInterval(poll,3000);
</script>
</body></html>)rawhtml";
