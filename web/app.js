/* ============================================================
   VESC Tuner dashboard — load session CSV, visualise telemetry.
   Buildless: uPlot + PapaParse via CDN. Theme = VibeWheel tokens.
   Core: parsing, dataset-aware helpers, chart factory, nav.
   Extra views live in views2.js (compare / tuning / diag / live).
   ============================================================ */
const C = {
  bran:'#06b6d4', wheel:'#38bdf8', gps:'#22c55e', target:'#a855f7',
  warning:'#f97316', error:'#ef4444', highlight:'#facc15', teal:'#14b8a6',
  text:'#f1f5f9', text2:'#94a3b8', muted:'#64748b', grid:'#1e293b', axis:'#334155',
};
const SYNC = 'tuner';
let D = null;            // active dataset
let CMP = null;          // comparison dataset (session B)
let CFG = { mcconf:null, appconf:null, suggestions:null }; // tuning context
let CHARTS = [];         // live uPlot instances (for resize)
let CURSOR_CB = null;    // optional hook(idx) fired on chart cursor move (Map uses it)
let XRANGE = null;       // persistent x-axis zoom window (carried across views)
let CURHUD = null;       // shared cursor readout element for the current view
let VIEW = 'overview';
let SMOOTH = { on:false, win:9 };                 // moving-average smoothing toggle
let ANNOT = {};                                   // { sessionName: [{t,label}] }  persisted
let THEME = localStorage.getItem('vesc_theme') || 'dark';
try { ANNOT = JSON.parse(localStorage.getItem('vesc_annot')||'{}'); } catch(e){ ANNOT = {}; }

/* moving average ignoring nulls (centred window) */
function movavg(a, win){
  if(!a || !SMOOTH.on) return a;
  const w=Math.max(1,win|0), half=w>>1, out=new Array(a.length);
  for(let i=0;i<a.length;i++){ let s=0,n=0;
    for(let j=i-half;j<=i+half;j++){ const v=a[j]; if(v!=null&&!isNaN(v)){s+=v;n++;} }
    out[i]= n? s/n : null;
  }
  return out;
}
function annotKey(){ return D ? D.name : '_'; }
function saveAnnot(){ try{ localStorage.setItem('vesc_annot', JSON.stringify(ANNOT)); }catch(e){} }

const $ = s => document.querySelector(s);
const el = (t, c) => { const e = document.createElement(t); if (c) e.className = c; return e; };

/* ---------- parsing ---------- */
function parseCSV(text, name) {
  const res = Papa.parse(text.trim(), { header: true, dynamicTyping: true, skipEmptyLines: true });
  const rows = res.data;
  if (!rows.length) return null;
  const fields = res.meta.fields;
  const col = {};
  for (const f of fields) col[f] = new Array(rows.length);
  for (let i = 0; i < rows.length; i++)
    for (const f of fields) { const v = rows[i][f]; col[f][i] = (v === '' || v == null) ? null : v; }
  const t0 = col.ts_ms ? col.ts_ms[0] : 0;
  const t = (col.ts_ms || rows.map((_, i) => i * 83)).map(v => (v - t0) / 1000);
  const cells = fields.filter(f => /^cell_\d+$/.test(f) && col[f].some(v => v != null));
  return { name: (name || 'session').replace('.csv',''), n: rows.length, fields, col, t, cells, source:'cardputer' };
}
/* ---------- import: detect source + adapt foreign formats ---------- */
// Float Control column -> internal column (+ optional transform). FC "SP-Carve"
// maps to our turn_tilt (carving = turn tilt). GPS/altitude/true-pitch pass through.
const FC_MAP = {
  'Time(s)':        {to:'ts_ms', fn:v=>v==null?null:Math.round(v*1000)},
  'Speed(km/h)':    {to:'speed_kmh'},
  'Duty%':          {to:'duty_pct', fn:v=>v==null?null:parseFloat(String(v))},  // "25%" -> 25
  'Voltage':        {to:'voltage_V'},
  'I-Battery':      {to:'curr_in_A'},
  'I-Motor':        {to:'curr_mot_A'},
  'I-FldWeak':      {to:'fldweak_A'},
  'Requested Amps': {to:'req_amps_A'},
  'Pitch':          {to:'pitch_deg'},
  'Roll':           {to:'roll_deg'},
  'Setpoint':       {to:'setpoint_deg'},
  'SP-ATR':         {to:'atr_deg'},
  'SP-Carve':       {to:'turntilt_deg'},
  'SP-TrqTlt':      {to:'torquetilt_deg'},
  'SP-BrkTlt':      {to:'braketilt_deg'},
  'SP-Remote':      {to:'remotetilt_deg'},
  'T-Mosfet':       {to:'temp_fet_C'},
  'T-Mot':          {to:'temp_mot_C'},
  'T-Batt':         {to:'temp_bat_C'},
  'ADC1':           {to:'adc1'},
  'ADC2':           {to:'adc2'},
  'Motor-Fault':    {to:'fault'},
  'Ah':             {to:'amp_hours'},
  'Ah Charged':     {to:'ah_charged'},
  'Wh':             {to:'watt_hours'},
  'Wh Charged':     {to:'wh_charged'},
  'ERPM':           {to:'rpm'},
  'Distance(km)':   {to:'odo_km'},
  'I-Booster':      {to:'booster_A'},
  'True Pitch':     {to:'true_pitch_deg'},
  'State(num)':     {to:'state'},
  'Altitude(m)':    {to:'altitude_m'},
  'GPS-Lat':        {to:'gps_lat'},
  'GPS-Long':       {to:'gps_lon'},
  'GPS-Accuracy':   {to:'gps_acc'},
  'T-BMS':          {to:'bms_temp_01'},
  'Alert':          {to:'alert_txt'},
};
/* alert text -> severity colour (drawn as vertical markers on every chart) */
function alertColor(txt){
  const s=String(txt).toLowerCase();
  if(s.includes('duty')) return C.error;
  if(s.includes('cell')||s.includes('voltage')||s.includes('temp')) return C.warning;
  if(s.includes('fault')) return C.error;
  return C.muted;   // GPS marker / watt-hours / informational
}
/* build sparse alert events {t,label,color} from an imported alert_txt column */
function computeAlerts(d){
  const a=d.col && d.col.alert_txt; if(!a) return [];
  const out=[];
  for(let i=0;i<a.length;i++){ const v=a[i];
    if(v==null||v==='') continue;
    const label=String(v).replace(/^Marker:\s*/,'').trim();
    out.push({ t:d.t[i], i, label, color:alertColor(v) });
  }
  return out;
}
/* derive ride dynamics from orientation angles when raw IMU is absent
   (Float Control logs only Pitch/Roll/True Pitch, no acc/gyro). pitch_rate
   ≈ gyro Y, roll_rate ≈ gyro X via central difference; gaps (>3s) skipped. */
function deriveDynamics(d){
  const t=d.t, n=d.n;
  const haveGyro = d.col.gyro_z && d.col.gyro_z.some(v=>v!=null);
  if(haveGyro) return;                       // raw IMU present — nothing to proxy
  const rate = src => { const a=d.col[src]; if(!a || !a.some(v=>v!=null)) return null;
    const out=new Array(n).fill(null);
    for(let i=1;i<n-1;i++){ const p=a[i-1], q=a[i+1], dt=t[i+1]-t[i-1];
      if(p==null||q==null||!(dt>0)||dt>3) continue; out[i]=(q-p)/dt; }
    return out; };
  const pr=rate('pitch_deg'), rr=rate('roll_deg');
  if(pr) d.col.pitch_rate=pr;
  if(rr) d.col.roll_rate=rr;
  if(pr||rr){ const ar=new Array(n).fill(null);
    for(let i=0;i<n;i++){ const a=pr?pr[i]:null, b=rr?rr[i]:null;
      if(a==null&&b==null) continue; ar[i]=Math.hypot(a||0,b||0); }
    d.col.ang_rate=ar;
  }
}
/* derive ride date/time from a Float Control filename (…_YYYYMMDDHHMMSS.csv) */
function parseDateFromName(name){
  const m=String(name||'').match(/(\d{4})(\d{2})(\d{2})(\d{2})?(\d{2})?(\d{2})?/);
  if(!m) return null;
  const date=`${m[1]}-${m[2]}-${m[3]}`;
  const time=m[4]?`${m[4]}:${m[5]||'00'}:${m[6]||'00'}`:null;
  return { date, time };
}
/* mean lat/lon of valid GPS samples (for a single weather query point) */
function gpsCentroid(d){
  const la=d.col.gps_lat, lo=d.col.gps_lon; if(!la||!lo) return null;
  let s1=0,s2=0,n=0;
  for(let i=0;i<d.n;i++){ if(la[i]!=null&&lo[i]!=null&&Math.abs(la[i])>1e-4){ s1+=la[i]; s2+=lo[i]; n++; } }
  return n?{lat:s1/n,lon:s2/n}:null;
}
/* fetch that-day weather from Open-Meteo archive (no key, CORS-ok); cached */
async function fetchWeather(date, lat, lon, d){
  if(d && d.weather) return d.weather;
  const key='wx:'+date+':'+lat.toFixed(2)+':'+lon.toFixed(2);
  try{ const c=localStorage.getItem(key); if(c){ const w=JSON.parse(c); if(d)d.weather=w; return w; } }catch(e){}
  const url=`https://archive-api.open-meteo.com/v1/archive?latitude=${lat.toFixed(4)}&longitude=${lon.toFixed(4)}`+
    `&start_date=${date}&end_date=${date}`+
    `&daily=temperature_2m_max,temperature_2m_min,temperature_2m_mean,relative_humidity_2m_mean,precipitation_sum,windspeed_10m_max&timezone=auto`;
  const res=await fetch(url); if(!res.ok) throw new Error('HTTP '+res.status);
  const j=await res.json(); const dd=j.daily||{};
  const g=a=>(dd[a]&&dd[a][0]!=null)?dd[a][0]:null;
  let tmean=g('temperature_2m_mean'); const tmax=g('temperature_2m_max'), tmin=g('temperature_2m_min');
  if(tmean==null && tmax!=null && tmin!=null) tmean=(tmax+tmin)/2;
  const w={ tmax, tmin, tmean, humidity:g('relative_humidity_2m_mean'),
            precip:g('precipitation_sum'), wind:g('windspeed_10m_max') };
  try{ localStorage.setItem(key, JSON.stringify(w)); }catch(e){}
  if(d) d.weather=w; return w;
}
/* render the Weather section into Overview (async fill; graceful offline) */
async function renderWeather(d, box, note){
  const dt = d.dateHint;
  if(!dt){ note.textContent='No date in this session — weather needs a dated Float Control file.'; return; }
  const ll = gpsCentroid(d);
  if(!ll){ note.textContent='No GPS in this session — weather lookup needs a location.'; return; }
  box.innerHTML='<div class="kpi"><div class="k">Weather</div><div class="v mono">…</div></div>';
  try{
    const w = await fetchWeather(dt, ll.lat, ll.lon, d);
    box.innerHTML='';
    box.append(
      kpi('Air temp', w.tmean!=null?w.tmean.toFixed(1):'–','°C',
          (w.tmin!=null&&w.tmax!=null)?`${w.tmin.toFixed(0)}–${w.tmax.toFixed(0)}° range`:'', C.warning),
      kpi('Humidity', w.humidity!=null?w.humidity.toFixed(0):'–','%', null, C.wheel),
      kpi('Precip', w.precip!=null?w.precip.toFixed(1):'–','mm', w.precip>0?'wet ride':'dry', w.precip>0?C.wheel:C.gps),
      kpi('Max wind', w.wind!=null?w.wind.toFixed(0):'–','km/h'),
    );
    note.innerHTML=`<span class="srctag" style="background:none;color:var(--muted);padding:0">that day · ${dt} · ${ll.lat.toFixed(3)},${ll.lon.toFixed(3)} · © Open-Meteo</span>`;
  }catch(e){
    box.innerHTML='';
    note.textContent='Weather unavailable ('+(navigator.onLine?e.message:'offline — connect once to cache')+').';
  }
}
function detectFormat(fields){
  if(fields.includes('ts_ms')) return 'cardputer';
  if(fields.includes('Time(s)') || fields.includes('Speed(km/h)') || fields.includes('Duty%')) return 'floatcontrol';
  return 'cardputer';   // best-effort: try the native parser
}
function parseFloatControl(text, name){
  const res = Papa.parse(text.trim(), { header:true, dynamicTyping:true, skipEmptyLines:true });
  const rows = res.data; if(!rows.length) return null;
  const active = res.meta.fields.filter(f => FC_MAP[f]);
  const col = {};
  for(const f of active){ const to=FC_MAP[f].to; if(!col[to]) col[to]=new Array(rows.length).fill(null); }
  for(let i=0;i<rows.length;i++) for(const f of active){ const m=FC_MAP[f]; let v=rows[i][f];
    if(v===''||v==null||v==='-') continue;
    v = m.fn ? m.fn(v) : (typeof v==='number'?v:(isNaN(+v)?v:+v));
    if(v==null||(typeof v==='number'&&isNaN(v))) continue;
    col[m.to][i]=v;
  }
  if(col.voltage_V && col.curr_in_A){ col.power_W=new Array(rows.length).fill(null);
    for(let i=0;i<rows.length;i++){ const V=col.voltage_V[i],I=col.curr_in_A[i]; if(V!=null&&I!=null) col.power_W[i]=V*I; } }
  const t0 = col.ts_ms ? col.ts_ms[0] : 0;
  const t = (col.ts_ms || rows.map((_,i)=>i*1000)).map(v=>(v-t0)/1000);
  return { name:(name||'session').replace(/\.(csv|txt)$/i,''), n:rows.length, fields:Object.keys(col), col, t, cells:[], source:'floatcontrol' };
}
function importCSV(text, name, forced, meta){
  let fmt = forced;
  if(!fmt){ const nl=text.indexOf('\n'); const head=(nl<0?text:text.slice(0,nl)).split(',').map(s=>s.trim()); fmt=detectFormat(head); }
  const d = fmt==='floatcontrol' ? parseFloatControl(text,name) : parseCSV(text,name);
  if(!d){ alert('Could not parse this file.'); return; }
  const dn = parseDateFromName(name);
  d.dateHint = (meta&&meta.date) || (dn&&dn.date) || null;
  d.timeHint = (meta&&meta.time) || (dn&&dn.time) || null;
  d.csvText = text; d.alerts = computeAlerts(d); deriveDynamics(d); D = d; XRANGE=null; renderSidebar(); render(VIEW);
}
function loadCSV(text, name) { importCSV(text, name); }   // auto-detect (drag-drop, sample, history)

/* ---------- dataset-aware helpers ----------
   H(d) returns a helper bundle bound to dataset d. Existing views do
   `const {has,mx,mn,last,avg}=H(D)`; compare view uses H(CMP) too.     */
function H(d){
  const c = n => d.col[n] || [];
  const has = n => d.col[n] && d.col[n].some(v => v != null);
  const mx = n => { let m=-Infinity; for(const v of c(n)) if(v!=null&&v>m) m=v; return m===-Infinity?0:m; };
  const mn = n => { let m= Infinity; for(const v of c(n)) if(v!=null&&v<m) m=v; return m=== Infinity?0:m; };
  const last = n => { const a=c(n); for(let i=a.length-1;i>=0;i--) if(a[i]!=null) return a[i]; return 0; };
  const avg = n => { let s=0,k=0; for(const v of c(n)) if(v!=null){s+=v;k++;} return k?s/k:0; };
  const p = (n,q) => { const a=c(n).filter(v=>v!=null).sort((x,y)=>x-y); if(!a.length) return 0;
    return a[Math.min(a.length-1, Math.floor(q*(a.length-1)))]; };
  const distanceKm = () => {
    if (has('odo_km')) { const first=c('odo_km').find(v=>v!=null)||0; const dd=last('odo_km')-first; if(dd>0) return dd; }
    let dd=0; const s=c('speed_kmh'), t=d.t;
    for (let i=1;i<d.n;i++) if(s[i]!=null) dd += s[i]*(t[i]-t[i-1])/3600;
    return dd;
  };
  return { d, has, mx, mn, last, avg, p, distanceKm };
}
// convenience: top-level helpers bound to the ACTIVE dataset (legacy call sites)
function has(c){ return H(D).has(c); }
function mx(c){ return H(D).mx(c); }
function mn(c){ return H(D).mn(c); }
function last(c){ return H(D).last(c); }
function avg(c){ return H(D).avg(c); }
function distanceKm(){ return H(D).distanceKm(); }

/* linear-interp a (times,values) series onto an arbitrary grid; null outside range */
function resample(times, values, grid){
  const out = new Array(grid.length).fill(null);
  let j = 0;
  for (let i=0;i<grid.length;i++){
    const g = grid[i];
    if (g < times[0] || g > times[times.length-1]) continue;
    while (j < times.length-1 && times[j+1] < g) j++;
    const t0=times[j], t1=times[j+1] ?? t0, v0=values[j], v1=values[j+1] ?? v0;
    if (v0==null || v1==null) { out[i] = v0!=null?v0:v1; continue; }
    out[i] = t1===t0 ? v0 : v0 + (v1-v0)*(g-t0)/(t1-t0);
  }
  return out;
}

/* ---------- sidebar ---------- */
function renderSidebar() {
  if (!D) return;
  $('#sess-name').textContent = D.name;
  const h = H(D), dur = D.t[D.n-1] || 0;
  const src = D.source==='floatcontrol' ? 'Float Control' : 'Cardputer';
  $('#sess-meta').innerHTML =
    `<span class="srctag">${src}</span><br>` +
    `<b>${(dur/60).toFixed(1)}</b> min · <b>${D.n}</b> samples<br>` +
    `<b>${h.distanceKm().toFixed(2)}</b> km · <b>${D.fields.length}</b> channels`;
}

/* ---------- chart factory ---------- */
const fmtTime = s => { s=Math.max(0,s); const m=Math.floor(s/60); return m+':'+String(Math.floor(s%60)).padStart(2,'0'); };
const axisBase = { stroke:C.muted, grid:{stroke:C.grid,width:1}, ticks:{stroke:C.axis,width:1}, font:'11px -apple-system,sans-serif' };
const axX = () => ({ ...axisBase, values:(u,v)=>v.map(fmtTime), space:60 });
const axY = (scale,side)=>({ ...axisBase, scale, side, size:46 });

function chartCard(parent, title) {
  const card=el('div','chart-card'), head=el('div','chart-head'), ct=el('div','ct'), leg=el('div','legend-pills');
  ct.textContent=title; head.append(ct,leg); card.append(head);
  const body=el('div'); card.append(body); parent.append(card);
  return { body, leg };
}

/* real drawable width of a card body (avoids spilling past the rounded frame:
   #main padding + card padding/border are already excluded by using the body). */
function chartWidth(body){ const w=body.clientWidth; return (w&&w>40)?w:(($('#main').clientWidth||800)-82); }

/* low-level: lines = [{label,color,xs,ys,scale,width,dec,dash}] sharing grid `xs0`.
   opts.annot=false disables annotation markers (e.g. live/scatter charts).      */
function plot(parent, title, xs0, lines, height=128, bands, opts={}) {
  const { body, leg } = chartCard(parent, title);
  const pills = lines.map(d => {
    const lp=el('div','lp'), sw=el('span','sw'); sw.style.background=d.color;
    if (d.dash) sw.style.opacity='.55';
    const tx=document.createElement('span'); tx.textContent=d.label;
    const b=document.createElement('b'); b.textContent='–';
    lp.append(sw,tx,b); leg.append(lp); return b;
  });
  const selBox = el('div','selstats'); body.parentElement.append(selBox);
  // smoothing applied to plotted values; raw kept for stats
  const plotted = lines.map(d => movavg(d.ys, SMOOTH.win));
  const data = [xs0, ...plotted];
  const scales = { x:{ time:false } };
  lines.forEach(d => scales[d.scale||'y'] = scales[d.scale||'y'] || {});
  const series = [{}].concat(lines.map(d => ({
    label:d.label, stroke:d.color, width:d.width||1.5, scale:d.scale||'y',
    points:{show:false}, spanGaps:true, dash:d.dash?[4,4]:undefined,
  })));
  const axes = [ axX(), axY('y',3) ];
  if (lines.some(d => (d.scale||'y')==='y2')) axes.push(axY('y2',1));
  const useAnnot = opts.annot !== false;
  const hooks = {
    setCursor:[uu=>{ const i=uu.cursor.idx;
      lines.forEach((d,k)=>{ pills[k].textContent=(i!=null&&plotted[k]&&plotted[k][i]!=null)?(+plotted[k][i]).toFixed(d.dec??1):'–'; });
      if(xs0===D.t){ if(CURSOR_CB) CURSOR_CB(i); if(CURHUD) writeCursorHud(i); }
    }],
    setSelect:[uu=>{ selectionStats(uu, xs0, lines, selBox); }],
    setScale:[(uu,key)=>{ if(key!=='x' || xs0!==D.t) return; const s=uu.scales.x;
      const full = s.min<=D.t[0]+1e-6 && s.max>=D.t[D.n-1]-1e-6;
      XRANGE = full ? null : {min:s.min, max:s.max}; refreshHudZoom();
    }],
    draw:[uu=>{ if(bands&&bands.length) drawBands(uu,bands); drawDataAlerts(uu,xs0); if(useAnnot) drawAnnotations(uu); }],
  };
  const u = new uPlot({
    width: chartWidth(body), height, scales, series, axes, legend:{show:false},
    cursor:{ sync:{key:SYNC}, drag:{x:true,y:false}, points:{size:6} },
    hooks,
  }, data, body);
  u._h = height; u._xs = xs0; u._raw = lines; u._body = body;
  if(XRANGE && xs0===D.t){ try{ u.setScale('x', XRANGE); }catch(e){} }   // carry zoom across views
  if (useAnnot) body.addEventListener('dblclick', e=>{
    const rect=body.getBoundingClientRect();
    const xv=u.posToVal(e.clientX-rect.left, 'x'); if(xv==null) return;
    const label=prompt('Marker label @ '+fmtTime(xv)); if(label==null) return;
    (ANNOT[annotKey()]=ANNOT[annotKey()]||[]).push({t:+xv.toFixed(2), label}); saveAnnot();
    CHARTS.forEach(c=>c.redraw(false,false));
  });
  CHARTS.push(u);
  return u;
}

/* window stats for a brushed selection (drag) — avg / min / max per line */
function selectionStats(u, xs0, lines, box){
  const sel=u.select; if(!sel || sel.width<3){ box.innerHTML=''; return; }
  const x0=u.posToVal(sel.left,'x'), x1=u.posToVal(sel.left+sel.width,'x');
  let i0=0,i1=xs0.length-1;
  for(let i=0;i<xs0.length;i++){ if(xs0[i]>=x0){i0=i;break;} }
  for(let i=xs0.length-1;i>=0;i--){ if(xs0[i]<=x1){i1=i;break;} }
  const parts=[`<b>${fmtTime(x0)}–${fmtTime(x1)}</b> (${(x1-x0).toFixed(1)}s)`];
  lines.forEach(d=>{ let s=0,n=0,mn=Infinity,mx=-Infinity;
    for(let i=i0;i<=i1;i++){ const v=d.ys[i]; if(v==null||isNaN(v))continue; s+=v;n++; if(v<mn)mn=v; if(v>mx)mx=v; }
    if(n) parts.push(`<span style="color:${d.color}">${d.label}</span> avg ${(s/n).toFixed(d.dec??1)} · ${mn.toFixed(d.dec??1)}–${mx.toFixed(d.dec??1)}`);
  });
  box.innerHTML = parts.join('&nbsp;&nbsp;');
}

/* vertical annotation markers for the active session */
function drawAnnotations(u){
  const arr=ANNOT[annotKey()]; if(!arr||!arr.length) return;
  const ctx=u.ctx; ctx.save();
  ctx.strokeStyle=C.target; ctx.fillStyle=C.target; ctx.lineWidth=1; ctx.font='10px -apple-system';
  for(const a of arr){ const x=u.valToPos(a.t,'x',true);
    if(x<u.bbox.left||x>u.bbox.left+u.bbox.width) continue;
    ctx.setLineDash([3,3]); ctx.beginPath(); ctx.moveTo(x,u.bbox.top); ctx.lineTo(x,u.bbox.top+u.bbox.height); ctx.stroke();
    ctx.setLineDash([]); ctx.fillText(a.label||'•', x+3, u.bbox.top+10);
  }
  ctx.restore();
}

/* data-driven alert markers (FC "Alert" column) — vertical ticks on every
   chart of the ACTIVE session timeline. xs0 closure guards against compare/live. */
function drawDataAlerts(u, xs0){
  if(!D || !D.alerts || !D.alerts.length || xs0!==D.t) return;
  const ctx=u.ctx, top=u.bbox.top, bot=u.bbox.top+u.bbox.height; ctx.save();
  ctx.font='9px -apple-system';
  for(const a of D.alerts){ const x=u.valToPos(a.t,'x',true);
    if(x<u.bbox.left||x>u.bbox.left+u.bbox.width) continue;
    ctx.strokeStyle=a.color; ctx.fillStyle=a.color; ctx.lineWidth=1;
    ctx.globalAlpha=.5; ctx.setLineDash([2,3]);
    ctx.beginPath(); ctx.moveTo(x,top); ctx.lineTo(x,bot); ctx.stroke();
    ctx.setLineDash([]); ctx.globalAlpha=1;
    // small downward triangle flag at the top of the plot area
    ctx.beginPath(); ctx.moveTo(x,top+6); ctx.lineTo(x-4,top); ctx.lineTo(x+4,top); ctx.closePath(); ctx.fill();
    // label runs vertically downward so neighbouring alerts don't collide
    ctx.save(); ctx.translate(x+3, top+8); ctx.rotate(Math.PI/2); ctx.globalAlpha=.8;
    ctx.fillText(a.label.length>24?a.label.slice(0,23)+'…':a.label, 0, 0); ctx.restore();
  }
  ctx.restore();
}

/* compact list of session alerts (FC "Alert" column) for a view header.
   Chips are clickable → jump to Timeline, zoom to the moment, show context. */
function alertStrip(parent){
  if(!D || !D.alerts || !D.alerts.length) return;
  const wrap=el('div','alertstrip');
  D.alerts.forEach(a=>{ const c=el('span','achip'); c.style.borderColor=a.color;
    c.innerHTML=`<i style="background:${a.color}"></i><b>${fmtTime(a.t)}</b> ${a.label}`;
    c.title='Click → zoom Timeline to this moment';
    c.onclick=()=>gotoAlert(a);
    wrap.append(c); });
  parent.append(wrap);
}
/* zoom every live chart's x-axis to a window around time t (seconds) */
let FOCUS_T = null;   // pending time to zoom Timeline to on next render
function zoomToTime(t, pad){ pad=pad||15;
  CHARTS.forEach(u=>{ try{ u.setScale('x',{min:Math.max(0,t-pad), max:t+pad}); }catch(e){} }); }
function gotoAlert(a){ FOCUS_T = a.t; switchView('timeline'); }
/* one-line readout of telemetry at time t (nearest sample) + nearest alert */
function alertContextBanner(parent, t){
  let bi=0, bd=Infinity; for(let i=0;i<D.n;i++){ const dd=Math.abs(D.t[i]-t); if(dd<bd){bd=dd;bi=i;} }
  const g=n=>{ const a=D.col[n]; return (a&&a[bi]!=null)?a[bi]:null; };
  const f=v=>v==null?'–':(+v).toFixed(1);
  const al=(D.alerts||[]).reduce((b,a)=> Math.abs(a.t-t)<Math.abs((b?b.t:1e9)-t)?a:b, null);
  const bar=el('div','alertctx');
  bar.innerHTML=`<b>@ ${fmtTime(t)}</b>`+
    (al&&Math.abs(al.t-t)<2?` <span style="color:${al.color};font-weight:700">${al.label}</span>`:'')+
    ` · speed <b>${f(g('speed_kmh'))}</b> km/h · duty <b>${f(g('duty_pct'))}</b>% · <b>${f(g('voltage_V'))}</b>V`+
    ` · FET <b>${f(g('temp_fet_C'))}</b>° · Imot <b>${f(g('curr_mot_A'))}</b>A`+
    ` <a class="ctxreset" title="reset zoom">reset</a>`;
  bar.querySelector('.ctxreset').onclick=()=>{ CHARTS.forEach(u=>{ try{ u.setScale('x',{min:D.t[0],max:D.t[D.n-1]}); }catch(e){} }); };
  parent.append(bar);
}

/* horizontal limit lines (e.g. l_current_max) drawn on scale y */
function drawBands(u, bands){
  const ctx = u.ctx; ctx.save();
  for (const b of bands){
    const sc = b.scale || 'y';
    if (!u.scales[sc]) continue;
    const y = u.valToPos(b.value, sc, true);
    if (y < u.bbox.top || y > u.bbox.top + u.bbox.height) continue;
    ctx.strokeStyle = b.color || C.error; ctx.lineWidth = 1; ctx.setLineDash([6,4]);
    ctx.beginPath(); ctx.moveTo(u.bbox.left, y); ctx.lineTo(u.bbox.left+u.bbox.width, y); ctx.stroke();
    ctx.setLineDash([]); ctx.fillStyle = b.color || C.error; ctx.font='10px -apple-system';
    ctx.fillText(b.label||'', u.bbox.left+6, y-3);
  }
  ctx.restore();
}

/* a column padded/truncated to exactly D.n (nulls if missing) — safe for uPlot */
function colN(name){ const a=D.col[name];
  if(a && a.length===D.n) return a;
  const o=new Array(D.n).fill(null); if(a) for(let i=0;i<a.length&&i<D.n;i++) o[i]=a[i]; return o; }
/* convenience: build lines from the ACTIVE dataset (legacy makeChart signature) */
function makeChart(parent, title, defs, height=128, bands) {
  const lines = defs.map(d => ({ ...d, xs:D.t, ys:colN(d.col) }));
  return plot(parent, title, D.t, lines, height, bands);
}

/* ---------- views (core 4) ---------- */
function clearMain(){ CHARTS=[]; CURHUD=null; const m=$('#main'); m.innerHTML=''; return m; }

/* shared cursor readout + zoom indicator, used by chart-heavy views */
function cursorHud(parent){
  const h=el('div','curhud');
  h.innerHTML='<span class="ch-val">hover charts to read values at a time</span><span class="ch-zoom"></span>';
  CURHUD=h; parent.append(h); refreshHudZoom();
}
function writeCursorHud(i){ if(!CURHUD||i==null) return;
  const g=n=>{ const a=D.col[n]; return (a&&a[i]!=null)?a[i]:null; }; const f=x=>x==null?'–':(+x).toFixed(1);
  CURHUD.querySelector('.ch-val').innerHTML=
    `<b>@ ${fmtTime(D.t[i])}</b> · speed ${f(g('speed_kmh'))} · duty ${f(g('duty_pct'))}% · ${f(g('voltage_V'))}V · FET ${f(g('temp_fet_C'))}° · Imot ${f(g('curr_mot_A'))}A`;
}
function refreshHudZoom(){ if(!CURHUD) return; const z=CURHUD.querySelector('.ch-zoom');
  if(XRANGE){ z.innerHTML=`zoom ${fmtTime(XRANGE.min)}–${fmtTime(XRANGE.max)} <a class="ctxreset">reset</a>`;
    z.querySelector('.ctxreset').onclick=resetZoom; } else z.textContent=''; }
function resetZoom(){ XRANGE=null; CHARTS.forEach(u=>{ try{ u.setScale('x',{min:D.t[0],max:D.t[D.n-1]}); }catch(e){} }); refreshHudZoom(); }
function topbar(m, title, hint){ const t=el('div','topbar'); const h=el('h1'); h.textContent=title;
  const s=el('div','hint'); s.textContent=hint||''; t.append(h,s); m.append(t); return t; }
function sectionTitle(m, t){ const s=el('div','section-title'); s.textContent=t; m.append(s); }

function kpi(label, value, unit, sub, color) {
  const k=el('div','kpi'); k.innerHTML =
    `<div class="k">${label}</div><div class="v mono" ${color?`style="color:${color}"`:''}>${value}${unit?`<small>${unit}</small>`:''}</div>`+
    (sub?`<div class="sub">${sub}</div>`:'');
  return k;
}

function viewOverview() {
  const m=clearMain(); topbar(m, 'Overview', D.name);
  alertStrip(m);
  if(typeof assessmentCard==='function') assessmentCard(m, D);
  const dur=D.t[D.n-1]||0, dist=distanceKm(), wh=last('watt_hours');
  const NV=(k,dflt)=>(typeof norm==='function'?norm(k).value:dflt);   // resolved threshold (config→reference)
  sectionTitle(m,'Session');
  const g=el('div','kpis');
  g.append(
    kpi('Duration', (dur/60).toFixed(1), 'min'),
    kpi('Distance', dist.toFixed(2), 'km'),
    kpi('Top speed', mx('speed_kmh').toFixed(1), 'km/h', null, C.warning),
    kpi('Avg speed', avg('speed_kmh').toFixed(1), 'km/h'),
    kpi('Peak duty', mx('duty_pct').toFixed(0), '%', null, mx('duty_pct')>NV('duty_crit',90)?C.error:C.highlight),
    kpi('Max A in', mx('curr_in_A').toFixed(0), 'A', null, C.wheel),
    kpi('Energy', wh.toFixed(0), 'Wh', dist>0?`${(wh/dist).toFixed(1)} Wh/km`:'', C.teal),
    kpi('Max FET', mx('temp_fet_C').toFixed(0), '°C', null, mx('temp_fet_C')>NV('fet_warn',70)?C.error:C.text),
    kpi('Max motor', mx('temp_mot_C').toFixed(0), '°C'),
    kpi('Max batt', has('temp_bat_C')&&mx('temp_bat_C')>0?mx('temp_bat_C').toFixed(0):'–', '°C'),
    kpi('Min cell', has('cell_min')?mn('cell_min').toFixed(3):(last('vcell_V')||0).toFixed(3), 'V', null, C.bran),
    kpi('Cell Δ max', has('cell_delta_mV')?mx('cell_delta_mV').toFixed(0):'–', 'mV', null, mx('cell_delta_mV')>NV('imbal_warn',100)?C.warning:C.text),
  );
  m.append(g);
  if(D.dateHint || H(D).has('gps_lat')){
    sectionTitle(m,'Weather (that day)');
    const wx=el('div','kpis'); m.append(wx);
    const wnote=el('div','hint'); wnote.style.margin='4px 2px 0'; m.append(wnote);
    renderWeather(D, wx, wnote);
  }
  sectionTitle(m,'Auto-flags');
  const flags=computeFlags(D);
  const fc=el('div','flags');
  if(!flags.length){ const f=el('div','flag ok'); f.innerHTML=`<span class="ico">✓</span><div><div class="t">No issues detected</div><div class="d">duty, temps, balance and faults all nominal</div></div>`; fc.append(f); }
  const tag = fl => (fl.src && typeof tierTag==='function') ? ' '+tierTag(fl.tier, fl.src) : '';
  flags.forEach(fl=>{ const f=el('div','flag sev-'+fl.sev); f.innerHTML=
    `<span class="ico">${fl.ico}</span><div><div class="t">${fl.title}</div><div class="d">${fl.detail}${tag(fl)}</div></div><div class="when">${fl.when}</div>`;
    f.onclick=()=>switchView('timeline'); fc.append(f); });
  m.append(fc);
}

function viewTimeline() {
  const m=clearMain(); topbar(m,'Timeline','drag to zoom · hover for values · click an alert chip to focus');
  alertStrip(m);
  cursorHud(m);
  const focus=FOCUS_T; FOCUS_T=null;
  if(focus!=null) alertContextBanner(m, focus);
  const lim = limitBands();
  makeChart(m,'Speed / Duty',[
    {label:'speed',col:'speed_kmh',color:C.warning,scale:'y',dec:1},
    {label:'duty %',col:'duty_pct',color:C.highlight,scale:'y2',dec:0},
  ],140, lim.duty);
  makeChart(m,'Power / Current',[
    {label:'A in',col:'curr_in_A',color:C.wheel,dec:1},
    {label:'A motor',col:'curr_mot_A',color:C.bran,dec:1},
    {label:'kW',col:'power_W',color:C.target,scale:'y2',dec:0},
  ],128, lim.current);
  makeChart(m,'Temperatures',[
    {label:'FET',col:'temp_fet_C',color:C.error,dec:1},
    {label:'motor',col:'temp_mot_C',color:C.warning,dec:1},
    {label:'batt',col:'temp_bat_C',color:C.highlight,dec:1},
  ],128, lim.temp);
  makeChart(m,'Voltage',[
    {label:'pack V',col:'voltage_V',color:C.teal,dec:1},
    {label:'V/cell',col:'vcell_V',color:C.bran,scale:'y2',dec:3},
  ],128, lim.volt);
  makeChart(m,'Pitch / Setpoint',[
    {label:'pitch',col:'pitch_deg',color:C.wheel,dec:1},
    {label:'setpoint',col:'setpoint_deg',color:C.highlight,dec:1},
    {label:'roll',col:'roll_deg',color:C.muted,dec:1},
  ]);
  if(focus!=null) zoomToTime(focus, 15);
}

function viewBattery() {
  const m=clearMain(); topbar(m,'Battery', `${D.cells.length}S · per-cell`);
  sectionTitle(m,'Cells (latest)');
  const lastVals = D.cells.map(c=>last(c));
  const cmin=Math.min(...lastVals), cmax=Math.max(...lastVals);
  const grid=el('div','cellgrid');
  D.cells.forEach((c,i)=>{ const v=lastVals[i];
    const box=el('div','cellbox'+(v<=cmin+1e-6?' low':v>=cmax-1e-6?' high':''));
    const f=Math.max(0,Math.min(1,(v-3.0)/1.25))*100;
    const col=v<=cmin+1e-6?C.warning:v>=cmax-1e-6?C.bran:C.gps;
    box.innerHTML=`<div class="cn">CELL ${String(i+1).padStart(2,'0')}</div><div class="cv">${v.toFixed(3)}</div>`+
      `<div class="bar"><i style="width:${f}%;background:${col}"></i></div>`;
    grid.append(box);
  });
  m.append(grid);
  sectionTitle(m,'Over time');
  const cellDefs=D.cells.map((c,i)=>({label:'',col:c,color:`hsl(${190+i*7},70%,60%)`,width:1,dec:3}));
  makeChart(m,'Cell voltages', cellDefs, 150);
  makeChart(m,'Cell imbalance',[
    {label:'Δ mV',col:'cell_delta_mV',color:C.warning,dec:0},
  ]);
  makeChart(m,'Battery % / Wh left',[
    {label:'batt %',col:'batt_pct',color:C.gps,dec:0},
    {label:'Wh left',col:'batt_wh',color:C.teal,scale:'y2',dec:0},
  ]);
  if (has('temp_bat_C') && mx('temp_bat_C')>0)
    makeChart(m,'Battery temperature',[{label:'BAT °C',col:'temp_bat_C',color:C.highlight,dec:1}]);
}

function viewImu() {
  const m=clearMain(); topbar(m,'Balance / IMU','ride dynamics');
  const rawImu = has('acc_z') || has('gyro_z');
  const derived = !rawImu && (has('pitch_rate') || has('roll_rate'));

  sectionTitle(m,'Dynamics');
  const g=el('div','kpis');
  const leanMax=Math.max(Math.abs(mx('roll_deg')), Math.abs(mn('roll_deg')));

  if(rawImu){
    const gMax=mx('acc_z');
    const landings=[];
    if(has('acc_z')){ const a=D.col.acc_z; let cool=0;
      for(let i=0;i<D.n;i++){ if(a[i]!=null && a[i]>2.0 && cool<=0){ landings.push({t:D.t[i],g:a[i]}); cool=8; } if(cool>0)cool--; } }
    landings.sort((x,y)=>y.g-x.g);
    g.append(
      kpi('Max lean', leanMax.toFixed(0),'°','roll', leanMax>40?C.warning:C.text),
      kpi('Hard landings', String(landings.length),'', '>2.0 g', landings.length?C.warning:C.gps),
      kpi('Peak impact', landings.length?landings[0].g.toFixed(2):gMax.toFixed(2),'g', landings.length?('@'+fmtTime(landings[0].t)):'', gMax>3?C.error:C.text),
      kpi('Max gyro', Math.max(Math.abs(mx('gyro_z')),Math.abs(mn('gyro_z'))).toFixed(0),'°/s','yaw rate'),
    );
    m.append(g);
    if(landings.length){
      sectionTitle(m,'Hardest landings');
      const tbl=el('table','cfgtable'); tbl.innerHTML='<tr><th>#</th><th>time</th><th>impact (g)</th></tr>';
      landings.slice(0,8).forEach((l,i)=>{ const tr=el('tr');
        tr.innerHTML=`<td class="mono">${i+1}</td><td class="mono">${fmtTime(l.t)}</td>`+
          `<td class="num mono" style="color:${l.g>3?C.error:C.warning}">${l.g.toFixed(2)}</td>`; tbl.append(tr); });
      m.append(tbl);
    }
  } else if(derived){
    const prMax=Math.max(Math.abs(mx('pitch_rate')), Math.abs(mn('pitch_rate')));
    const rrMax=Math.max(Math.abs(mx('roll_rate')),  Math.abs(mn('roll_rate')));
    const spikes=[]; if(has('ang_rate')){ const a=D.col.ang_rate; let cool=0;
      for(let i=0;i<D.n;i++){ if(a[i]!=null && a[i]>40 && cool<=0){ spikes.push({t:D.t[i],r:a[i]}); cool=4; } if(cool>0)cool--; } }
    spikes.sort((x,y)=>y.r-x.r);
    g.append(
      kpi('Max lean', leanMax.toFixed(0),'°','roll', leanMax>40?C.warning:C.text),
      kpi('Max pitch rate', prMax.toFixed(0),'°/s','≈ gyro Y (derived)', C.wheel),
      kpi('Max roll rate', rrMax.toFixed(0),'°/s','≈ gyro X (derived)', C.warning),
      kpi('Rate spikes', String(spikes.length),'', '>40°/s · proxy', spikes.length?C.warning:C.gps),
    );
    m.append(g);
    const note=el('div','hint'); note.style.margin='-4px 2px 6px';
    note.innerHTML='No raw accel/gyro here (Float Control import) — showing <b>derived</b> pitch/roll rates from angles. Raw 6-axis IMU comes only from Cardputer logs.';
    m.append(note);
    if(spikes.length){
      sectionTitle(m,'Sharpest corrections (proxy)');
      const tbl=el('table','cfgtable'); tbl.innerHTML='<tr><th>#</th><th>time</th><th>angular rate (°/s)</th></tr>';
      spikes.slice(0,8).forEach((s,i)=>{ const tr=el('tr');
        tr.innerHTML=`<td class="mono">${i+1}</td><td class="mono">${fmtTime(s.t)}</td>`+
          `<td class="num mono" style="color:${s.r>80?C.error:C.warning}">${s.r.toFixed(0)}</td>`; tbl.append(tr); });
      m.append(tbl);
    }
  } else {
    g.append(kpi('Max lean', leanMax.toFixed(0),'°','roll', leanMax>40?C.warning:C.text));
    m.append(g);
    const note=el('div','hint'); note.textContent='No accelerometer/gyroscope or pitch/roll data in this session.'; m.append(note);
  }

  sectionTitle(m,'Charts');
  makeChart(m,'Pitch vs Setpoint',[
    {label:'pitch',col:'pitch_deg',color:C.wheel,dec:1},
    {label:'true pitch',col:'true_pitch_deg',color:C.muted,dec:1},
    {label:'setpoint',col:'setpoint_deg',color:C.highlight,dec:1},
  ]);
  makeChart(m,'Setpoint contributions (SP-*)',[
    {label:'ATR',col:'atr_deg',color:C.bran,dec:2},
    {label:'torque-tilt',col:'torquetilt_deg',color:C.target,dec:2},
    {label:'turn-tilt',col:'turntilt_deg',color:C.gps,dec:2},
    {label:'brake-tilt',col:'braketilt_deg',color:C.warning,dec:2},
    {label:'remote',col:'remotetilt_deg',color:C.highlight,dec:2},
  ]);
  if(rawImu){
    makeChart(m,'Accelerometer (g)',[
      {label:'acc X',col:'acc_x',color:C.warning,dec:2},
      {label:'acc Y',col:'acc_y',color:C.wheel,dec:2},
      {label:'acc Z',col:'acc_z',color:C.target,dec:2},
    ]);
    makeChart(m,'Gyroscope (°/s)',[
      {label:'gyro X',col:'gyro_x',color:C.warning,dec:1},
      {label:'gyro Y',col:'gyro_y',color:C.wheel,dec:1},
      {label:'gyro Z',col:'gyro_z',color:C.target,dec:1},
    ]);
  } else if(derived){
    makeChart(m,'Pitch / roll rate (derived ≈ gyro)',[
      {label:'pitch rate',col:'pitch_rate',color:C.wheel,dec:1},
      {label:'roll rate',col:'roll_rate',color:C.warning,dec:1},
    ]);
  }
  makeChart(m,'Footpads (ADC)',[
    {label:'adc1',col:'adc1',color:C.gps,dec:2},
    {label:'adc2',col:'adc2',color:C.teal,dec:2},
  ]);
}

/* limit lines for chart bands: mcconf current limit + Refloat pushback/haptic
   thresholds (duty/HV/LV/temps) so every Timeline chart shows the buzz line. */
function limitBands(){
  const bands = { current:[], temp:[], duty:[], volt:[] };
  const mc = CFG.mcconf;
  if(mc){ const g = k => (mc[k] ?? mc[k?.toUpperCase?.()]);
    const im = g('l_in_current_max'); if(im!=null) bands.current.push({value:+im, scale:'y', color:C.error, label:`l_in_current_max ${im}A`});
  }
  if(typeof pbCfg==='function' && D){ const c = pbCfg();
    bands.duty.push({value:c.tiltback_duty, scale:'y2', color:C.error, label:`buzz ${c.tiltback_duty}%`});
    bands.volt.push({value:c.hv, scale:'y', color:C.warning, label:`HV ${c.hv}`},
                    {value:c.lv, scale:'y', color:C.highlight, label:`LV ${c.lv}`});
    bands.temp.push({value:c.fet_max, scale:'y', color:C.error, label:`FET ${c.fet_max}°`},
                    {value:c.mot_max, scale:'y', color:C.warning, label:`motor ${c.mot_max}°`});
  }
  const nn = a => a.length?a:null;
  return { current:nn(bands.current), temp:nn(bands.temp), duty:nn(bands.duty), volt:nn(bands.volt) };
}

/* ---------- auto-flags (dataset-aware) ---------- */
function computeFlags(d) {
  const h = H(d), col = d.col, t = d.t, n = d.n;
  const firstWhen = pred => { for(let i=0;i<n;i++) if(pred(i)) return fmtTime(t[i]); return ''; };
  const countIf = pred => { let k=0; for(let i=0;i<n;i++) if(pred(i)) k++; return k; };
  // thresholds + provenance from the norm resolver (device config first, else reference)
  const N = (typeof norm==='function') ? norm : (()=>({value:null,tier:'',source:''}));
  const f=[];
  const duty=col.duty_pct, fet=col.temp_fet_C, mot=col.temp_mot_C, delta=col.cell_delta_mV,
        fault=col.fault, accz=col.acc_z, vmin=col.cell_min, bt=col.temp_bat_C;
  if(duty){ const w=N('duty_warn'), cr=N('duty_crit'); const dHi=countIf(i=>duty[i]>w.value);
    if(dHi){ const peak=h.mx('duty_pct'); f.push({sev:peak>=cr.value?'err':'warn',ico:'!',
      title:`Duty over ${w.value}% (${dHi}×)`, detail:`peak ${peak.toFixed(0)}% · critical ${cr.value}%`,
      when:firstWhen(i=>duty[i]>w.value), src:w.source, tier:w.tier}); } }
  if(fet){ const w=N('fet_warn'), cr=N('fet_crit'); const fHot=countIf(i=>fet[i]>w.value);
    if(fHot){ const mxf=h.mx('temp_fet_C'); f.push({sev:mxf>cr.value?'err':'warn',ico:'▲',
      title:`Controller hot (max ${mxf.toFixed(0)}°C)`, detail:`${fHot} samples over ${w.value}°C`,
      when:firstWhen(i=>fet[i]>w.value), src:w.source, tier:w.tier}); } }
  if(mot){ const w=N('mot_warn'); const mHot=countIf(i=>mot[i]>w.value);
    if(mHot) f.push({sev:'warn',ico:'▲',title:`Motor hot (max ${h.mx('temp_mot_C').toFixed(0)}°C)`,
      detail:`${mHot} samples over ${w.value}°C`, when:firstWhen(i=>mot[i]>w.value), src:w.source, tier:w.tier}); }
  if(bt && h.mx('temp_bat_C')>0){ const w=N('batt_warn'), cr=N('batt_crit'); const bHot=countIf(i=>bt[i]>w.value);
    if(bHot) f.push({sev:h.mx('temp_bat_C')>cr.value?'err':'warn',ico:'▲',title:`Battery warm (max ${h.mx('temp_bat_C').toFixed(0)}°C)`,
      detail:`over ${w.value}°C`, when:firstWhen(i=>bt[i]>w.value), src:w.source, tier:w.tier}); }
  if(delta){ const w=N('imbal_warn'), cr=N('imbal_crit'); const dm=h.mx('cell_delta_mV');
    if(dm>w.value) f.push({sev:dm>cr.value?'err':'warn',ico:'≠',title:`Cell imbalance ${dm.toFixed(0)} mV`,
      detail:`over ${w.value} mV — consider a balance charge`, when:firstWhen(i=>delta[i]>w.value), src:w.source, tier:w.tier}); }
  if(fault){ const fc=countIf(i=>fault[i]>0); if(fc) f.push({sev:'err',ico:'✕',title:`Fault code raised`,
    detail:`mc_fault active in ${fc} samples`, when:firstWhen(i=>fault[i]>0), src:'VESC firmware', tier:'firmware'}); }
  if(accz){ const w=N('landing_g'); const land=countIf(i=>accz[i]>w.value); if(land) f.push({sev:'info',ico:'↓',title:`${land} hard landing(s)`,
    detail:`acc Z over ${w.value}g`, when:firstWhen(i=>accz[i]>w.value), src:w.source, tier:w.tier}); }
  if(vmin){ const w=N('cell_warn'); const lv=h.mn('cell_min'); if(lv>0&&lv<w.value) f.push({sev:'warn',ico:'▽',title:`Low cell ${lv.toFixed(3)} V`,
    detail:`under ${w.value}V — reduce load / charge`, when:firstWhen(i=>vmin[i]<w.value), src:w.source, tier:w.tier}); }
  return f;
}

/* ---------- nav / wiring ---------- */
const VIEWS={ overview:viewOverview, timeline:viewTimeline, battery:viewBattery, imu:viewImu };
function render(v){
  if(!D && !['live','history'].includes(v)){ $('#main').innerHTML='<div class="empty">No session loaded — drop a CSV or click “Load session CSV”.</div>'; return; }
  (VIEWS[v]||viewOverview)();
}
function switchView(v){
  if(v!=='live' && typeof liveDisconnect==='function') liveDisconnect();
  if(v!=='replay' && typeof replayStop==='function') replayStop();
  if(v!=='map' && typeof leafmapStop==='function') leafmapStop();
  VIEW=v; document.querySelectorAll('.navitem').forEach(b=>b.classList.toggle('active',b.dataset.v===v)); render(v);
}
document.querySelectorAll('.navitem').forEach(b=> b.onclick=()=>switchView(b.dataset.v));
let IMPORT_SRC = null;   // which import button was clicked (forces the parser)
const impCard=$('#imp-card'), impFc=$('#imp-fc');
if(impCard) impCard.onclick=()=>{ IMPORT_SRC='cardputer';    $('#file').click(); };
if(impFc)   impFc.onclick  =()=>{ IMPORT_SRC='floatcontrol'; $('#file').click(); };
$('#file').onchange=e=>{ const f=e.target.files[0];
  if(f){ const r=new FileReader(); r.onload=()=>importCSV(r.result,f.name,IMPORT_SRC); r.readAsText(f); }
  e.target.value=''; IMPORT_SRC=null; };

// drag & drop (active session)
const dz=$('#drop');
window.addEventListener('dragover',e=>{ e.preventDefault(); dz.classList.add('on'); });
window.addEventListener('dragleave',e=>{ if(e.relatedTarget===null) dz.classList.remove('on'); });
window.addEventListener('drop',e=>{ e.preventDefault(); dz.classList.remove('on');
  const f=e.dataTransfer.files[0]; if(f){ const r=new FileReader(); r.onload=()=>loadCSV(r.result,f.name); r.readAsText(f); } });

// resize
let rt; window.addEventListener('resize',()=>{ clearTimeout(rt); rt=setTimeout(()=>{
  CHARTS.forEach(u=>u.setSize({width:chartWidth(u._body||u.root.parentElement),height:u._h})); },120); });

// auto-load bundled sample
fetch('sample_session.csv').then(r=>r.ok?r.text():Promise.reject()).then(t=>loadCSV(t,'sample_session.csv')).catch(()=>render('overview'));
