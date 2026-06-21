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
  d.csvText = text; d.alerts = computeAlerts(d); D = d; renderSidebar(); render(VIEW);
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
      if(CURSOR_CB && xs0===D.t) CURSOR_CB(i);
    }],
    setSelect:[uu=>{ selectionStats(uu, xs0, lines, selBox); }],
    draw:[uu=>{ if(bands&&bands.length) drawBands(uu,bands); drawDataAlerts(uu,xs0); if(useAnnot) drawAnnotations(uu); }],
  };
  const u = new uPlot({
    width: chartWidth(body), height, scales, series, axes, legend:{show:false},
    cursor:{ sync:{key:SYNC}, drag:{x:true,y:false}, points:{size:6} },
    hooks,
  }, data, body);
  u._h = height; u._xs = xs0; u._raw = lines; u._body = body;
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

/* compact list of session alerts (FC "Alert" column) for a view header */
function alertStrip(parent){
  if(!D || !D.alerts || !D.alerts.length) return;
  const wrap=el('div','alertstrip');
  D.alerts.forEach(a=>{ const c=el('span','achip'); c.style.borderColor=a.color;
    c.innerHTML=`<i style="background:${a.color}"></i><b>${fmtTime(a.t)}</b> ${a.label}`;
    wrap.append(c); });
  parent.append(wrap);
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
function clearMain(){ CHARTS=[]; const m=$('#main'); m.innerHTML=''; return m; }
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
  const dur=D.t[D.n-1]||0, dist=distanceKm(), wh=last('watt_hours');
  sectionTitle(m,'Session');
  const g=el('div','kpis');
  g.append(
    kpi('Duration', (dur/60).toFixed(1), 'min'),
    kpi('Distance', dist.toFixed(2), 'km'),
    kpi('Top speed', mx('speed_kmh').toFixed(1), 'km/h', null, C.warning),
    kpi('Avg speed', avg('speed_kmh').toFixed(1), 'km/h'),
    kpi('Peak duty', mx('duty_pct').toFixed(0), '%', null, mx('duty_pct')>90?C.error:C.highlight),
    kpi('Max A in', mx('curr_in_A').toFixed(0), 'A', null, C.wheel),
    kpi('Energy', wh.toFixed(0), 'Wh', dist>0?`${(wh/dist).toFixed(1)} Wh/km`:'', C.teal),
    kpi('Max FET', mx('temp_fet_C').toFixed(0), '°C', null, mx('temp_fet_C')>70?C.error:C.text),
    kpi('Max motor', mx('temp_mot_C').toFixed(0), '°C'),
    kpi('Max batt', has('temp_bat_C')&&mx('temp_bat_C')>0?mx('temp_bat_C').toFixed(0):'–', '°C'),
    kpi('Min cell', has('cell_min')?mn('cell_min').toFixed(3):(last('vcell_V')||0).toFixed(3), 'V', null, C.bran),
    kpi('Cell Δ max', has('cell_delta_mV')?mx('cell_delta_mV').toFixed(0):'–', 'mV', null, mx('cell_delta_mV')>100?C.warning:C.text),
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
  flags.forEach(fl=>{ const f=el('div','flag sev-'+fl.sev); f.innerHTML=
    `<span class="ico">${fl.ico}</span><div><div class="t">${fl.title}</div><div class="d">${fl.detail}</div></div><div class="when">${fl.when}</div>`;
    f.onclick=()=>switchView('timeline'); fc.append(f); });
  m.append(fc);
}

function viewTimeline() {
  const m=clearMain(); topbar(m,'Timeline','drag to zoom · hover for values');
  alertStrip(m);
  const lim = limitBands();
  makeChart(m,'Speed / Duty',[
    {label:'speed',col:'speed_kmh',color:C.warning,scale:'y',dec:1},
    {label:'duty %',col:'duty_pct',color:C.highlight,scale:'y2',dec:0},
  ],140);
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
  ]);
  makeChart(m,'Pitch / Setpoint',[
    {label:'pitch',col:'pitch_deg',color:C.wheel,dec:1},
    {label:'setpoint',col:'setpoint_deg',color:C.highlight,dec:1},
    {label:'roll',col:'roll_deg',color:C.muted,dec:1},
  ]);
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

  // ride-dynamics KPIs: lean + hard landings
  sectionTitle(m,'Dynamics');
  const g=el('div','kpis');
  const leanMax=Math.max(Math.abs(mx('roll_deg')), Math.abs(mn('roll_deg')));
  const gMax=mx('acc_z');
  // collect landing events (acc_z spikes), ranked
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
    const tbl=el('table','cfgtable');
    tbl.innerHTML='<tr><th>#</th><th>time</th><th>impact (g)</th></tr>';
    landings.slice(0,8).forEach((l,i)=>{ const tr=el('tr');
      tr.innerHTML=`<td class="mono">${i+1}</td><td class="mono">${fmtTime(l.t)}</td>`+
        `<td class="num mono" style="color:${l.g>3?C.error:C.warning}">${l.g.toFixed(2)}</td>`;
      tbl.append(tr); });
    m.append(tbl);
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
  makeChart(m,'Footpads (ADC)',[
    {label:'adc1',col:'adc1',color:C.gps,dec:2},
    {label:'adc2',col:'adc2',color:C.teal,dec:2},
  ]);
}

/* limit lines from mcconf (used as chart bands when a config is loaded) */
function limitBands(){
  const mc = CFG.mcconf; if(!mc) return { current:null, temp:null };
  const g = k => (mc[k] ?? mc[k?.toUpperCase?.()] );
  const bands = { current:[], temp:[] };
  const im = g('l_in_current_max'); if(im!=null) bands.current.push({value:+im, scale:'y', color:C.error, label:`l_in_current_max ${im}A`});
  const tf = g('l_temp_fet_start'); if(tf!=null) bands.temp.push({value:+tf, scale:'y', color:C.warning, label:`fet_start ${tf}°`});
  const te = g('l_temp_fet_end');   if(te!=null) bands.temp.push({value:+te, scale:'y', color:C.error, label:`fet_end ${te}°`});
  return { current:bands.current.length?bands.current:null, temp:bands.temp.length?bands.temp:null };
}

/* ---------- auto-flags (dataset-aware) ---------- */
function computeFlags(d) {
  const h = H(d), col = d.col, t = d.t, n = d.n;
  const firstWhen = pred => { for(let i=0;i<n;i++) if(pred(i)) return fmtTime(t[i]); return ''; };
  const countIf = pred => { let k=0; for(let i=0;i<n;i++) if(pred(i)) k++; return k; };
  const f=[];
  const duty=col.duty_pct, fet=col.temp_fet_C, delta=col.cell_delta_mV,
        fault=col.fault, accz=col.acc_z, vmin=col.cell_min;
  if(duty){ const dHi=countIf(i=>duty[i]>90);
    if(dHi) f.push({sev:h.mx('duty_pct')>=96?'err':'warn',ico:'!',title:`Duty cycle over 90% (${dHi}×)`,
      detail:`peak ${h.mx('duty_pct').toFixed(0)}% — approaching the limit`, when:firstWhen(i=>duty[i]>90)}); }
  if(fet){ const fHot=countIf(i=>fet[i]>70);
    if(fHot) f.push({sev:h.mx('temp_fet_C')>85?'err':'warn',ico:'▲',title:`Controller hot (max ${h.mx('temp_fet_C').toFixed(0)}°C)`,
      detail:`${fHot} samples over 70°C — check thermal headroom / l_temp_fet`, when:firstWhen(i=>fet[i]>70)}); }
  if(delta){ const dm=h.mx('cell_delta_mV'); if(dm>100) f.push({sev:dm>200?'err':'warn',ico:'≠',title:`Cell imbalance ${dm.toFixed(0)} mV`,
    detail:'pack drifting — consider a balance charge', when:firstWhen(i=>delta[i]>100)}); }
  if(fault){ const fc=countIf(i=>fault[i]>0); if(fc) f.push({sev:'err',ico:'✕',title:`Fault code raised`,
    detail:`mc_fault active in ${fc} samples`, when:firstWhen(i=>fault[i]>0)}); }
  if(accz){ const land=countIf(i=>accz[i]>2.2); if(land) f.push({sev:'info',ico:'↓',title:`${land} hard landing(s)`,
    detail:`acc Z spiked over 2.2g`, when:firstWhen(i=>accz[i]>2.2)}); }
  if(vmin){ const lv=h.mn('cell_min'); if(lv>0&&lv<3.3) f.push({sev:'warn',ico:'▽',title:`Low cell ${lv.toFixed(3)} V`,
    detail:'a cell dipped under 3.3V — reduce load / charge', when:firstWhen(i=>vmin[i]<3.3)}); }
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
