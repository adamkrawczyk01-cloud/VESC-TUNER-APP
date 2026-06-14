/* ============================================================
   VESC Tuner dashboard — load session CSV, visualise telemetry.
   Buildless: uPlot + PapaParse via CDN. Theme = VibeWheel tokens.
   ============================================================ */
const C = {
  bran:'#06b6d4', wheel:'#38bdf8', gps:'#22c55e', target:'#a855f7',
  warning:'#f97316', error:'#ef4444', highlight:'#facc15', teal:'#14b8a6',
  text:'#f1f5f9', text2:'#94a3b8', muted:'#64748b', grid:'#1e293b', axis:'#334155',
};
const SYNC = uPlot.sync ? 'tuner' : 'tuner';
let D = null;            // dataset
let CHARTS = [];         // live uPlot instances (for resize)
let VIEW = 'overview';

const $ = s => document.querySelector(s);
const el = (t, c) => { const e = document.createElement(t); if (c) e.className = c; return e; };

/* ---------- parsing ---------- */
function loadCSV(text, name) {
  const res = Papa.parse(text.trim(), { header: true, dynamicTyping: true, skipEmptyLines: true });
  const rows = res.data;
  if (!rows.length) return;
  const fields = res.meta.fields;
  const col = {};
  for (const f of fields) col[f] = new Array(rows.length);
  for (let i = 0; i < rows.length; i++)
    for (const f of fields) { const v = rows[i][f]; col[f][i] = (v === '' || v == null) ? null : v; }
  const t0 = col.ts_ms ? col.ts_ms[0] : 0;
  const t = (col.ts_ms || rows.map((_, i) => i * 83)).map(v => (v - t0) / 1000);
  // detect cell columns present
  const cells = fields.filter(f => /^cell_\d+$/.test(f) && col[f].some(v => v != null));
  D = { name: name || 'session', n: rows.length, fields, col, t, cells };
  renderSidebar(); render(VIEW);
}

function has(c){ return D.col[c] && D.col[c].some(v => v != null); }
function mx(c){ let m=-Infinity; for(const v of D.col[c]||[]) if(v!=null&&v>m) m=v; return m===-Infinity?0:m; }
function mn(c){ let m= Infinity; for(const v of D.col[c]||[]) if(v!=null&&v<m) m=v; return m=== Infinity?0:m; }
function last(c){ const a=D.col[c]||[]; for(let i=a.length-1;i>=0;i--) if(a[i]!=null) return a[i]; return 0; }
function avg(c){ let s=0,n=0; for(const v of D.col[c]||[]) if(v!=null){s+=v;n++;} return n?s/n:0; }
function distanceKm(){
  if (has('odo_km')) { const d = last('odo_km') - (D.col.odo_km.find(v=>v!=null)||0); if (d>0) return d; }
  let d=0; const s=D.col.speed_kmh, t=D.t;
  for (let i=1;i<D.n;i++) if(s[i]!=null) d += s[i]*(t[i]-t[i-1])/3600;
  return d;
}

/* ---------- sidebar ---------- */
function renderSidebar() {
  $('#sess-name').textContent = D.name.replace('.csv','');
  const dur = D.t[D.n-1] || 0;
  $('#sess-meta').innerHTML =
    `<b>${(dur/60).toFixed(1)}</b> min · <b>${D.n}</b> samples<br>` +
    `<b>${distanceKm().toFixed(2)}</b> km · <b>${D.fields.length}</b> channels`;
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

function makeChart(parent, title, defs, height=128) {
  const { body, leg } = chartCard(parent, title);
  const pills = defs.map(d => {
    const lp=el('div','lp'), sw=el('span','sw'); sw.style.background=d.color;
    const tx=document.createElement('span'); tx.textContent=d.label;
    const b=document.createElement('b'); b.textContent='–';
    lp.append(sw,tx,b); leg.append(lp); return b;
  });
  const data = [D.t, ...defs.map(d => D.col[d.col] || [])];
  const scales = { x:{ time:false } };
  defs.forEach(d => scales[d.scale||'y'] = scales[d.scale||'y'] || {});
  const series = [{}].concat(defs.map(d => ({
    label:d.label, stroke:d.color, width:d.width||1.5, scale:d.scale||'y',
    points:{show:false}, spanGaps:true,
  })));
  const axes = [ axX(), axY('y',3) ];
  if (defs.some(d => (d.scale||'y')==='y2')) axes.push(axY('y2',1));
  const u = new uPlot({
    width: parent.clientWidth-30, height, scales, series, axes, legend:{show:false},
    cursor:{ sync:{key:SYNC}, drag:{x:true,y:false}, points:{size:6} },
    hooks:{ setCursor:[uu=>{ const i=uu.cursor.idx;
      defs.forEach((d,k)=>{ const a=D.col[d.col]; pills[k].textContent=(i!=null&&a&&a[i]!=null)?(+a[i]).toFixed(d.dec??1):'–'; });
    }] },
  }, data, body);
  u._h = height;
  CHARTS.push(u);
  return u;
}

/* ---------- views ---------- */
function clearMain(){ CHARTS=[]; const m=$('#main'); m.innerHTML=''; return m; }
function topbar(m, title, hint){ const t=el('div','topbar'); const h=el('h1'); h.textContent=title;
  const s=el('div','hint'); s.textContent=hint||''; t.append(h,s); m.append(t); }
function sectionTitle(m, t){ const s=el('div','section-title'); s.textContent=t; m.append(s); }

function kpi(label, value, unit, sub, color) {
  const k=el('div','kpi'); k.innerHTML =
    `<div class="k">${label}</div><div class="v mono" ${color?`style="color:${color}"`:''}>${value}${unit?`<small>${unit}</small>`:''}</div>`+
    (sub?`<div class="sub">${sub}</div>`:'');
  return k;
}

function viewOverview() {
  const m=clearMain(); topbar(m, 'Overview', D.name);
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
  sectionTitle(m,'Auto-flags');
  const flags=computeFlags();
  const fc=el('div','flags');
  if(!flags.length){ const f=el('div','flag ok'); f.innerHTML=`<span class="ico">✓</span><div><div class="t">No issues detected</div><div class="d">duty, temps, balance and faults all nominal</div></div>`; fc.append(f); }
  flags.forEach(fl=>{ const f=el('div','flag sev-'+fl.sev); f.innerHTML=
    `<span class="ico">${fl.ico}</span><div><div class="t">${fl.title}</div><div class="d">${fl.detail}</div></div><div class="when">${fl.when}</div>`;
    f.onclick=()=>switchView('timeline'); fc.append(f); });
  m.append(fc);
}

function viewTimeline() {
  const m=clearMain(); topbar(m,'Timeline','drag to zoom · hover for values');
  makeChart(m,'Speed / Duty',[
    {label:'speed',col:'speed_kmh',color:C.warning,scale:'y',dec:1},
    {label:'duty %',col:'duty_pct',color:C.highlight,scale:'y2',dec:0},
  ],140);
  makeChart(m,'Power / Current',[
    {label:'A in',col:'curr_in_A',color:C.wheel,dec:1},
    {label:'A motor',col:'curr_mot_A',color:C.bran,dec:1},
    {label:'kW',col:'power_W',color:C.target,scale:'y2',dec:0},
  ]);
  makeChart(m,'Temperatures',[
    {label:'FET',col:'temp_fet_C',color:C.error,dec:1},
    {label:'motor',col:'temp_mot_C',color:C.warning,dec:1},
    {label:'batt',col:'temp_bat_C',color:C.highlight,dec:1},
  ]);
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
  // current cell snapshot (last sample)
  sectionTitle(m,'Cells (latest)');
  const lo=mn('cell_min'), hi=mx('cell_max');
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
  // cell voltage lines
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
  makeChart(m,'Pitch vs Setpoint',[
    {label:'pitch',col:'pitch_deg',color:C.wheel,dec:1},
    {label:'setpoint',col:'setpoint_deg',color:C.highlight,dec:1},
  ]);
  makeChart(m,'Tilt contributions',[
    {label:'ATR',col:'atr_deg',color:C.bran,dec:2},
    {label:'torque-tilt',col:'torquetilt_deg',color:C.target,dec:2},
    {label:'turn-tilt',col:'turntilt_deg',color:C.gps,dec:2},
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

/* ---------- auto-flags ---------- */
function firstWhen(pred){ for(let i=0;i<D.n;i++) if(pred(i)) return fmtTime(D.t[i]); return ''; }
function countIf(pred){ let n=0; for(let i=0;i<D.n;i++) if(pred(i)) n++; return n; }
function computeFlags() {
  const f=[];
  const duty=D.col.duty_pct, fet=D.col.temp_fet_C, delta=D.col.cell_delta_mV,
        fault=D.col.fault, accz=D.col.acc_z, vmin=D.col.cell_min;
  const dHi=countIf(i=>duty&&duty[i]>90);
  if(dHi) f.push({sev:duty&&mx('duty_pct')>=96?'err':'warn',ico:'!',title:`Duty cycle over 90% (${dHi}×)`,
    detail:`peak ${mx('duty_pct').toFixed(0)}% — approaching the limit`, when:firstWhen(i=>duty[i]>90)});
  const fHot=countIf(i=>fet&&fet[i]>70);
  if(fHot) f.push({sev:mx('temp_fet_C')>85?'err':'warn',ico:'▲',title:`Controller hot (max ${mx('temp_fet_C').toFixed(0)}°C)`,
    detail:`${fHot} samples over 70°C — check thermal headroom / l_temp_fet`, when:firstWhen(i=>fet[i]>70)});
  if(delta){ const dm=mx('cell_delta_mV'); if(dm>100) f.push({sev:dm>200?'err':'warn',ico:'≠',title:`Cell imbalance ${dm.toFixed(0)} mV`,
    detail:'pack drifting — consider a balance charge', when:firstWhen(i=>delta[i]>100)}); }
  if(fault){ const fc=countIf(i=>fault[i]>0); if(fc) f.push({sev:'err',ico:'✕',title:`Fault code raised`,
    detail:`mc_fault active in ${fc} samples`, when:firstWhen(i=>fault[i]>0)}); }
  if(accz){ const land=countIf(i=>accz[i]>2.2); if(land) f.push({sev:'info',ico:'↓',title:`${land} hard landing(s)`,
    detail:`acc Z spiked over 2.2g`, when:firstWhen(i=>accz[i]>2.2)}); }
  if(vmin){ const lv=mn('cell_min'); if(lv>0&&lv<3.3) f.push({sev:'warn',ico:'▽',title:`Low cell ${lv.toFixed(3)} V`,
    detail:'a cell dipped under 3.3V — reduce load / charge', when:firstWhen(i=>vmin[i]<3.3)}); }
  return f;
}

/* ---------- nav / wiring ---------- */
const VIEWS={ overview:viewOverview, timeline:viewTimeline, battery:viewBattery, imu:viewImu };
function render(v){ if(!D){ $('#main').innerHTML='<div class="empty">No session loaded — drop a CSV or click “Load session CSV”.</div>'; return; } (VIEWS[v]||viewOverview)(); }
function switchView(v){ VIEW=v; document.querySelectorAll('.navitem').forEach(b=>b.classList.toggle('active',b.dataset.v===v)); render(v); }
document.querySelectorAll('.navitem').forEach(b=> b.onclick=()=>switchView(b.dataset.v));
$('#load').onclick=()=>$('#file').click();
$('#file').onchange=e=>{ const f=e.target.files[0]; if(f){ const r=new FileReader(); r.onload=()=>loadCSV(r.result,f.name); r.readAsText(f); } };

// drag & drop
const dz=$('#drop');
window.addEventListener('dragover',e=>{ e.preventDefault(); dz.classList.add('on'); });
window.addEventListener('dragleave',e=>{ if(e.relatedTarget===null) dz.classList.remove('on'); });
window.addEventListener('drop',e=>{ e.preventDefault(); dz.classList.remove('on');
  const f=e.dataTransfer.files[0]; if(f){ const r=new FileReader(); r.onload=()=>loadCSV(r.result,f.name); r.readAsText(f); } });

// resize
let rt; window.addEventListener('resize',()=>{ clearTimeout(rt); rt=setTimeout(()=>{
  const w=$('#main').clientWidth-30; CHARTS.forEach(u=>u.setSize({width:w,height:u._h})); },120); });

// auto-load bundled sample
fetch('sample_session.csv').then(r=>r.ok?r.text():Promise.reject()).then(t=>loadCSV(t,'sample_session.csv')).catch(()=>render('overview'));
