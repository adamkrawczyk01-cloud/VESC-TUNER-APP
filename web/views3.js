/* ============================================================
   VESC Tuner — analysis views (batch 2):
   Histograms, Segments, Efficiency, Motor/FOC, Faults, Statistics,
   Replay/scrubber, History (IndexedDB).
   Depends on app.js + views2.js globals.
   ============================================================ */

/* ---------- shared small helpers ---------- */
function scatterChart(parent, title, xs, ys, color, xLabel, yLabel, height=240){
  const { body } = chartCard(parent, title);
  const pairs = []; for(let i=0;i<xs.length;i++){ if(xs[i]!=null&&ys[i]!=null) pairs.push([xs[i],ys[i]]); }
  pairs.sort((a,b)=>a[0]-b[0]);
  const data=[pairs.map(p=>p[0]), pairs.map(p=>p[1])];
  const u=new uPlot({ width:parent.clientWidth-30, height, legend:{show:false},
    scales:{ x:{time:false} },
    axes:[ {...axisBaseRef(),} , {...axisBaseRef()} ],
    series:[ {label:xLabel}, { label:yLabel, stroke:color, fill:color+'33',
      paths:()=>null, points:{show:true,size:4,stroke:color,fill:color} } ],
  }, data, body);
  u._h=height; CHARTS.push(u); return u;
}
function axisBaseRef(){ return { stroke:C.muted, grid:{stroke:C.grid,width:1}, ticks:{stroke:C.axis,width:1},
  font:'11px -apple-system,sans-serif' }; }

function histogram(arr, nbins){
  const vals=arr.filter(v=>v!=null&&!isNaN(v));
  if(!vals.length) return null;
  let lo=Math.min(...vals), hi=Math.max(...vals); if(hi===lo) hi=lo+1;
  const bins=new Array(nbins).fill(0); const w=(hi-lo)/nbins;
  for(const v of vals){ let k=Math.floor((v-lo)/w); if(k>=nbins)k=nbins-1; if(k<0)k=0; bins[k]++; }
  return { lo, hi, w, bins, n:vals.length };
}
function renderHistogram(parent, title, col, color, unit, dec=0){
  if(!has(col)) return;
  const h=histogram(D.col[col], 28); if(!h) return;
  sectionTitle(parent, title);
  const wrap=el('div','hist'); const max=Math.max(...h.bins);
  h.bins.forEach((c,i)=>{ if(c===0 && i!==0 && i!==h.bins.length-1) {/*keep sparse*/}
    const x0=h.lo+i*h.w;
    const row=el('div','hrow');
    const pct=(c/h.n*100);
    row.innerHTML=`<div class="hl">${x0.toFixed(dec)}${unit||''}</div>`+
      `<div class="ht"><i style="width:${(c/max*100).toFixed(1)}%;background:${color}"></i></div>`+
      `<div class="hv">${pct.toFixed(1)}%</div>`;
    wrap.append(row);
  });
  parent.append(wrap);
}

/* ============================================================
   HISTOGRAMS (#3)
   ============================================================ */
function viewHistograms(){
  const m=clearMain(); topbar(m,'Histograms','where the ride actually spends its time');
  renderHistogram(m,'Speed', 'speed_kmh', C.warning, ' km/h', 0);
  renderHistogram(m,'Duty cycle', 'duty_pct', C.highlight, '%', 0);
  renderHistogram(m,'Current in', 'curr_in_A', C.wheel, ' A', 0);
  renderHistogram(m,'Power', 'power_W', C.target, ' W', 0);
  if(has('roll_deg')) renderHistogram(m,'Lean angle (roll)', 'roll_deg', C.gps, '°', 0);
  if(has('pitch_deg')) renderHistogram(m,'Pitch', 'pitch_deg', C.bran, '°', 0);
}

/* ============================================================
   SEGMENTS (#4) — accel / brake / cruise / idle
   ============================================================ */
function viewSegments(){
  const m=clearMain(); topbar(m,'Ride segments','auto-split into accel · cruise · brake · idle');
  const t=D.t, sp=movavg(D.col.speed_kmh, 7) || D.col.speed_kmh;
  const cls=new Array(D.n).fill('idle');
  for(let i=0;i<D.n;i++){ const v=sp[i]; if(v==null){cls[i]='idle';continue;}
    if(v<2){ cls[i]='idle'; continue; }
    let a=0; if(i>0 && sp[i-1]!=null){ const dt=t[i]-t[i-1]; if(dt>0) a=((v-sp[i-1])/3.6)/dt; }
    cls[i]= a>0.25?'accel':a<-0.25?'brake':'cruise';
  }
  // merge into segments
  const segs=[]; let s=0;
  for(let i=1;i<=D.n;i++){ if(i===D.n||cls[i]!==cls[s]){ const dur=t[Math.min(i,D.n-1)]-t[s];
    if(dur>=1.0||segs.length===0){ let dist=0,sumv=0,nn=0;
      for(let k=s;k<i;k++){ if(D.col.speed_kmh[k]!=null){ sumv+=D.col.speed_kmh[k]; nn++; if(k>s) dist+=D.col.speed_kmh[k]*(t[k]-t[k-1])/3600; } }
      segs.push({type:cls[s], t0:t[s], dur, dist, avg:nn?sumv/nn:0}); }
    s=i; } }
  // summary
  const tot={accel:0,brake:0,cruise:0,idle:0};
  segs.forEach(g=>tot[g.type]+=g.dur);
  const dur=t[D.n-1]||1;
  sectionTitle(m,'Time breakdown');
  const g=el('div','kpis');
  const colMap={accel:C.gps,cruise:C.wheel,brake:C.warning,idle:C.muted};
  ['accel','cruise','brake','idle'].forEach(k=>
    g.append(kpi(k[0].toUpperCase()+k.slice(1), (tot[k]/dur*100).toFixed(0), '%', `${(tot[k]/60).toFixed(1)} min`, colMap[k])));
  m.append(g);
  // table
  sectionTitle(m,`Segments (${segs.length})`);
  const tbl=el('table','cfgtable');
  tbl.innerHTML='<tr><th>start</th><th>type</th><th>dur</th><th>dist</th><th>avg speed</th></tr>';
  segs.forEach(s2=>{ const tr=el('tr');
    tr.innerHTML=`<td class="mono">${fmtTime(s2.t0)}</td>`+
      `<td style="color:${colMap[s2.type]};font-weight:600">${s2.type}</td>`+
      `<td class="num mono">${s2.dur.toFixed(1)}s</td>`+
      `<td class="num mono">${(s2.dist*1000).toFixed(0)} m</td>`+
      `<td class="num mono">${s2.avg.toFixed(1)} km/h</td>`;
    tbl.append(tr); });
  m.append(tbl);
}

/* ============================================================
   EFFICIENCY (#5) — Wh/km vs speed
   ============================================================ */
function viewEfficiency(){
  const m=clearMain(); topbar(m,'Efficiency','energy economy across the speed range');
  const h=H(D), dist=h.distanceKm(), wh=h.last('watt_hours');
  sectionTitle(m,'Summary');
  const g=el('div','kpis');
  g.append(
    kpi('Energy', wh.toFixed(0),'Wh'),
    kpi('Distance', dist.toFixed(2),'km'),
    kpi('Avg economy', dist>0?(wh/dist).toFixed(1):'–','Wh/km', null, C.teal),
    kpi('Range @ this rate', (dist>0&&wh>0)?((h.last('batt_wh')/(wh/dist))||0).toFixed(1):'–','km','from Wh left', C.gps),
  );
  m.append(g);
  // per-speed-bin Wh/km = avg power / speed
  sectionTitle(m,'Wh/km vs speed');
  const sp=D.col.speed_kmh, pw=D.col.power_W;
  const bins=24, lo=0, hi=Math.max(5,mx('speed_kmh')); const w=(hi-lo)/bins;
  const pSum=new Array(bins).fill(0), cnt=new Array(bins).fill(0);
  for(let i=0;i<D.n;i++){ const v=sp[i],p=pw?pw[i]:null; if(v==null||p==null||v<1) continue;
    let k=Math.floor((v-lo)/w); if(k>=bins)k=bins-1; if(k<0)continue; pSum[k]+=p; cnt[k]++; }
  const xs=[], ys=[];
  for(let k=0;k<bins;k++){ if(cnt[k]<3) continue; const sMid=lo+(k+0.5)*w; const avgP=pSum[k]/cnt[k];
    xs.push(+sMid.toFixed(1)); ys.push(+(avgP/sMid).toFixed(1)); }
  if(xs.length) plot(m,'Wh/km vs speed (binned)', xs, [{label:'Wh/km',color:C.teal,xs,ys,dec:1}], 200, null, {annot:false});
  else { const e=el('div','empty'); e.textContent='Not enough power/speed data for a curve.'; m.append(e); }
  // economy over distance
  makeChart(m,'Power / speed (time)',[
    {label:'kW',col:'power_W',color:C.target,dec:0},
    {label:'speed',col:'speed_kmh',color:C.warning,scale:'y2',dec:1},
  ],150);
}

/* ============================================================
   MOTOR / FOC (#11) — id/iq
   ============================================================ */
function viewMotor(){
  const m=clearMain(); topbar(m,'Motor / FOC','field-oriented control currents');
  const h=H(D);
  sectionTitle(m,'Peaks');
  const g=el('div','kpis');
  g.append(
    kpi('Max iq', h.mx('iq_A').toFixed(0),'A','torque current', C.bran),
    kpi('Max |id|', Math.max(Math.abs(h.mx('id_A')),Math.abs(h.mn('id_A'))).toFixed(0),'A','flux current', C.target),
    kpi('Max motor A', h.mx('curr_mot_A').toFixed(0),'A'),
    kpi('Max ERPM', h.mx('rpm').toFixed(0),'erpm', null, C.wheel),
  );
  m.append(g);
  makeChart(m,'FOC currents id / iq',[
    {label:'iq (torque)',col:'iq_A',color:C.bran,dec:1},
    {label:'id (flux)',col:'id_A',color:C.target,dec:1},
    {label:'motor A',col:'curr_mot_A',color:C.muted,dec:1},
  ],160);
  if(has('req_amps_A')) makeChart(m,'Requested vs delivered current',[
    {label:'requested A',col:'req_amps_A',color:C.highlight,dec:1},
    {label:'motor A',col:'curr_mot_A',color:C.bran,dec:1},
  ],150);
  makeChart(m,'RPM / duty',[
    {label:'erpm',col:'rpm',color:C.wheel,dec:0},
    {label:'duty %',col:'duty_pct',color:C.highlight,scale:'y2',dec:0},
  ]);
  if(has('id_A')&&has('iq_A')) scatterChart(m,'d–q current plane (id vs iq)', D.col.id_A, D.col.iq_A, C.bran,'id','iq',260);
}

/* ============================================================
   FAULTS (#13) — decoder + timeline
   ============================================================ */
const FAULT_NAMES = ['NONE','OVER_VOLTAGE','UNDER_VOLTAGE','DRV','ABS_OVER_CURRENT','OVER_TEMP_FET',
  'OVER_TEMP_MOTOR','GATE_DRIVER_OVER_VOLTAGE','GATE_DRIVER_UNDER_VOLTAGE','MCU_UNDER_VOLTAGE',
  'BOOTING_FROM_WATCHDOG_RESET','ENCODER_SPI','ENCODER_SINCOS_BELOW_MIN_AMPLITUDE',
  'ENCODER_SINCOS_ABOVE_MAX_AMPLITUDE','FLASH_CORRUPTION','HIGH_OFFSET_CURRENT_SENSOR_1',
  'HIGH_OFFSET_CURRENT_SENSOR_2','HIGH_OFFSET_CURRENT_SENSOR_3','UNBALANCED_CURRENTS','BRK',
  'RESOLVER_LOT','RESOLVER_DOS','RESOLVER_LOS','FLASH_CORRUPTION_APP_CFG','FLASH_CORRUPTION_MC_CFG',
  'ENCODER_NO_MAGNET','ENCODER_MAGNET_TOO_STRONG','PHASE_FILTER','ENCODER_FAULT','LV_OUTPUT_FAULT'];
function faultName(code){ return (code>=0&&code<FAULT_NAMES.length)?FAULT_NAMES[code]:('CODE_'+code); }
function viewFaults(){
  const m=clearMain(); topbar(m,'Faults','mc_fault_code decode + timeline');
  if(!has('fault')){ const e=el('div','empty'); e.textContent='No fault channel in this session.'; m.append(e); return; }
  const f=D.col.fault;
  // collect events (rising edges)
  const events=[]; let prev=0;
  for(let i=0;i<D.n;i++){ const c=f[i]||0; if(c>0 && c!==prev) events.push({t:D.t[i], code:c}); prev=c; }
  sectionTitle(m,'Summary');
  const g=el('div','kpis');
  g.append(
    kpi('Fault events', String(events.length), '', null, events.length?C.error:C.gps),
    kpi('Worst code', events.length?String(Math.max(...events.map(e=>e.code))):'0','', events.length?faultName(Math.max(...events.map(e=>e.code))):'clean', events.length?C.error:C.gps),
  );
  m.append(g);
  if(!events.length){ const ok=el('div','flag ok'); ok.innerHTML='<span class="ico">✓</span><div><div class="t">No faults logged</div><div class="d">mc_fault_code stayed at 0 the whole ride</div></div>'; m.append(ok); }
  else {
    sectionTitle(m,'Events');
    const fc=el('div','flags');
    events.forEach(e=>{ const node=el('div','flag sev-err');
      node.innerHTML=`<span class="ico">✕</span><div><div class="t">${faultName(e.code)} <span style="color:var(--muted)">(code ${e.code})</span></div>`+
        `<div class="d">${faultHint(e.code)}</div></div><div class="when">${fmtTime(e.t)}</div>`;
      fc.append(node); });
    m.append(fc);
  }
  makeChart(m,'Fault code timeline',[{label:'fault',col:'fault',color:C.error,dec:0}],120);
}
function faultHint(code){
  const h={1:'pack/regen voltage too high — lower regen or check charger',
    2:'voltage sag below cutoff — battery weak or overloaded',
    4:'abs over-current — current spike past hardware limit',
    5:'controller over-temp — improve cooling / lower l_temp_fet',
    6:'motor over-temp — sustained high load',
    18:'unbalanced phase currents — wiring/sensor issue',
    19:'brake fault'};
  return h[code]||'see VESC fault reference for this code';
}

/* ============================================================
   STATISTICS (#14) — per-channel table + CSV export
   ============================================================ */
function viewStats(){
  const m=clearMain(); topbar(m,'Statistics','per-channel min / max / avg / p50 / p95 / σ');
  const h=H(D);
  const rows=[];
  for(const f of D.fields){ if(f==='ts_ms') continue;
    const a=D.col[f]; if(!a || !a.some(v=>typeof v==='number')) continue;
    const vals=a.filter(v=>typeof v==='number'&&!isNaN(v)); if(!vals.length) continue;
    const mean=h.avg(f); let sd=0; for(const v of vals) sd+=(v-mean)*(v-mean); sd=Math.sqrt(sd/vals.length);
    rows.push({f, min:h.mn(f), max:h.mx(f), avg:mean, p50:h.p(f,0.5), p95:h.p(f,0.95), sd});
  }
  m.append(btn('⬇ Export stats.csv', ()=>{
    const L=['channel,min,max,avg,p50,p95,stddev'];
    rows.forEach(r=>L.push(`${r.f},${r.min},${r.max},${r.avg},${r.p50},${r.p95},${r.sd}`));
    downloadText(L.join('\n'), (D.name||'session')+'_stats.csv','text/csv');
  }));
  const tbl=el('table','cfgtable'); tbl.style.marginTop='12px';
  tbl.innerHTML='<tr><th>channel</th><th>min</th><th>max</th><th>avg</th><th>p50</th><th>p95</th><th>σ</th></tr>';
  const fmt=v=>Math.abs(v)>=100?v.toFixed(0):Math.abs(v)>=1?v.toFixed(2):v.toFixed(3);
  rows.forEach(r=>{ const tr=el('tr');
    tr.innerHTML=`<td class="mono">${r.f}</td>`+
      ['min','max','avg','p50','p95','sd'].map(k=>`<td class="num mono">${fmt(r[k])}</td>`).join('');
    tbl.append(tr); });
  m.append(tbl);
}

/* ============================================================
   REPLAY (#20) — scrubber + virtual dashboard
   ============================================================ */
let REPLAY = { timer:null, idx:0, speed:4 };
function replayStop(){ if(REPLAY.timer){ clearInterval(REPLAY.timer); REPLAY.timer=null; } }
function viewReplay(){
  replayStop();
  const m=clearMain(); topbar(m,'Replay','scrub or play the ride');
  const bar=el('div','replaybar');
  const playBtn=btn('▶ Play', ()=>replayToggle(playBtn),'sm');
  const range=el('input'); range.type='range'; range.min='0'; range.max=String(D.n-1); range.value=String(REPLAY.idx);
  const tlab=el('span','scrub-t');
  range.oninput=()=>{ REPLAY.idx=+range.value; replayPaint(tlab); };
  const spd=el('select','toolbtn'); [1,2,4,8,16].forEach(s=>{ const o=el('option'); o.value=s; o.textContent=s+'×'; if(s===REPLAY.speed)o.selected=true; spd.append(o); });
  spd.onchange=()=>{ REPLAY.speed=+spd.value; if(REPLAY.timer){ replayStop(); replayPlay(playBtn,range,tlab); } };
  bar.append(playBtn, range, tlab, spd);
  m.append(bar);
  const g=el('div','kpis'); g.id='replay-kpis';
  REPLAY_FIELDS.forEach(f=> g.append(kpi(f.label,'–',f.unit)));
  m.append(g);
  // context chart with a cursor line is provided by uPlot sync; show speed/duty
  makeChart(m,'Context — speed / duty',[
    {label:'speed',col:'speed_kmh',color:C.warning,dec:1},
    {label:'duty %',col:'duty_pct',color:C.highlight,scale:'y2',dec:0},
  ],150);
  REPLAY._range=range; REPLAY._tlab=tlab;
  replayPaint(tlab);
}
const REPLAY_FIELDS=[
  {key:'speed_kmh',label:'Speed',unit:'km/h',dec:1,color:C.warning},
  {key:'duty_pct',label:'Duty',unit:'%',dec:0},
  {key:'curr_in_A',label:'Current',unit:'A',dec:0},
  {key:'power_W',label:'Power',unit:'W',dec:0},
  {key:'voltage_V',label:'Voltage',unit:'V',dec:1},
  {key:'temp_fet_C',label:'FET',unit:'°C',dec:0},
  {key:'pitch_deg',label:'Pitch',unit:'°',dec:1},
  {key:'batt_pct',label:'Battery',unit:'%',dec:0},
];
function replayPaint(tlab){
  const i=REPLAY.idx, kp=$('#replay-kpis'); if(!kp) return;
  REPLAY_FIELDS.forEach((f,k)=>{ const a=D.col[f.key]; const v=a?a[i]:null;
    const node=kp.children[k]?.querySelector('.v'); if(node) node.textContent=(v!=null?(+v).toFixed(f.dec):'–'); });
  if(tlab) tlab.textContent=`${fmtTime(D.t[i]||0)} / ${fmtTime(D.t[D.n-1]||0)}`;
  // move uPlot cursors to this index
  CHARTS.forEach(u=>{ if(u._xs===D.t) u.setCursor({left:u.valToPos(D.t[i],'x'), top:0}); });
}
function replayToggle(btnEl){ if(REPLAY.timer){ replayStop(); btnEl.textContent='▶ Play'; } else replayPlay(btnEl, REPLAY._range, REPLAY._tlab); }
function replayPlay(btnEl, range, tlab){
  btnEl.textContent='⏸ Pause';
  REPLAY.timer=setInterval(()=>{ REPLAY.idx+=REPLAY.speed; if(REPLAY.idx>=D.n-1){ REPLAY.idx=D.n-1; replayStop(); btnEl.textContent='▶ Play'; }
    if(range) range.value=String(REPLAY.idx); replayPaint(tlab); }, 83);
}

/* ============================================================
   HISTORY (#2) — IndexedDB session library + trends
   ============================================================ */
function idbOpen(){ return new Promise((res,rej)=>{ const r=indexedDB.open('vesc-tuner',1);
  r.onupgradeneeded=()=>{ if(!r.result.objectStoreNames.contains('sessions')) r.result.createObjectStore('sessions',{keyPath:'name'}); };
  r.onsuccess=()=>res(r.result); r.onerror=()=>rej(r.error); }); }
async function idbAll(){ const db=await idbOpen(); return new Promise((res,rej)=>{ const tx=db.transaction('sessions','readonly');
  const rq=tx.objectStore('sessions').getAll(); rq.onsuccess=()=>res(rq.result||[]); rq.onerror=()=>rej(rq.error); }); }
async function idbPut(rec){ const db=await idbOpen(); return new Promise((res,rej)=>{ const tx=db.transaction('sessions','readwrite');
  tx.objectStore('sessions').put(rec); tx.oncomplete=()=>res(); tx.onerror=()=>rej(tx.error); }); }
async function idbDel(name){ const db=await idbOpen(); return new Promise((res,rej)=>{ const tx=db.transaction('sessions','readwrite');
  tx.objectStore('sessions').delete(name); tx.oncomplete=()=>res(); tx.onerror=()=>rej(tx.error); }); }

function sessionMetrics(d){ const h=H(d); const dist=h.distanceKm();
  return { dur:(d.t[d.n-1]||0)/60, dist, top:h.mx('speed_kmh'), whkm: dist>0?h.last('watt_hours')/dist:0,
    maxFet:h.mx('temp_fet_C'), minCell:h.has('cell_min')?h.mn('cell_min'):0 }; }

async function viewHistory(){
  const m=clearMain(); topbar(m,'History','saved sessions & trends (stored in this browser)');
  const bar=el('div','cmpbar');
  if(D) bar.append(btn('＋ Save current session', async()=>{
    if(!D.csvText){ alert('Reload the CSV to enable saving.'); return; }
    await idbPut({ name:D.name, csv:D.csvText, savedAt:Date.now(), metrics:sessionMetrics(D) });
    render('history');
  }));
  m.append(bar);
  let recs=[]; try{ recs=await idbAll(); }catch(e){}
  if(!recs.length){ const e=el('div','empty'); e.textContent='No saved sessions yet. Load a CSV and click “Save current session”.'; m.append(e); return; }
  recs.sort((a,b)=>b.savedAt-a.savedAt);

  // trends
  sectionTitle(m,'Trends (oldest → newest)');
  const ordered=[...recs].sort((a,b)=>a.savedAt-b.savedAt);
  trendRow(m,'Economy (Wh/km)', ordered.map(r=>r.metrics.whkm||0), C.teal, 1);
  trendRow(m,'Max FET (°C)', ordered.map(r=>r.metrics.maxFet||0), C.error, 0);
  trendRow(m,'Top speed (km/h)', ordered.map(r=>r.metrics.top||0), C.warning, 0);

  sectionTitle(m,`Saved sessions (${recs.length})`);
  const list=el('div','histlist');
  recs.forEach(r=>{ const row=el('div','hsess'); const mt=r.metrics||{};
    const d=new Date(r.savedAt);
    row.innerHTML=`<div><div class="hn">${r.name}</div>`+
      `<div class="hm">${mt.dur?.toFixed(1)||'?'} min · ${mt.dist?.toFixed(2)||'?'} km · ${mt.whkm?.toFixed(1)||'?'} Wh/km · FET ${mt.maxFet?.toFixed(0)||'?'}°C</div></div>`;
    const acts=el('div','ha');
    acts.append(
      btn('Load', ()=>{ loadCSV(r.csv, r.name); switchView('overview'); },'sm'),
      btn('B', ()=>{ CMP=parseCSV(r.csv,r.name); switchView('compare'); },'sm ghost'),
      btn('✕', async()=>{ if(confirm('Delete '+r.name+'?')){ await idbDel(r.name); render('history'); } },'sm ghost'),
    );
    row.append(acts); list.append(row);
  });
  m.append(list);
}
function trendRow(parent, label, vals, color, dec){
  const max=Math.max(...vals,0.0001);
  const wrap=el('div','hist');
  const head=el('div','hrow'); head.innerHTML=`<div class="hl" style="color:var(--text2)">${label}</div><div></div><div></div>`;
  wrap.append(head);
  vals.forEach((v,i)=>{ const row=el('div','hrow');
    row.innerHTML=`<div class="hl">#${i+1}</div>`+
      `<div class="ht"><i style="width:${(v/max*100).toFixed(1)}%;background:${color}"></i></div>`+
      `<div class="hv">${v.toFixed(dec)}</div>`;
    wrap.append(row); });
  parent.append(wrap);
}

/* ============================================================
   MAP / GPS (#5 geo) — from Float Control imports (lat/long/altitude)
   ============================================================ */
function haversine(a, b){
  const R=6371000, toR=Math.PI/180;
  const dLat=(b.lat-a.lat)*toR, dLon=(b.lon-a.lon)*toR;
  const la1=a.lat*toR, la2=b.lat*toR;
  const h=Math.sin(dLat/2)**2 + Math.cos(la1)*Math.cos(la2)*Math.sin(dLon/2)**2;
  return 2*R*Math.asin(Math.sqrt(h));
}
function viewMap(){
  const m=clearMain(); topbar(m,'Map / GPS','route coloured by speed · from Float Control imports');
  if(!has('gps_lat')||!has('gps_lon')){
    const e=el('div','empty'); e.innerHTML='No GPS in this session.<br>Import a Float Control ride (it carries lat/long + altitude).'; m.append(e); return;
  }
  const lat=D.col.gps_lat, lon=D.col.gps_lon, sp=D.col.speed_kmh||[];
  const pts=[]; for(let i=0;i<D.n;i++){ if(lat[i]!=null&&lon[i]!=null&&Math.abs(lat[i])>0.0001) pts.push({lat:lat[i],lon:lon[i],sp:sp[i]||0}); }
  // GPS distance + climb KPIs
  let gdist=0; for(let i=1;i<pts.length;i++) gdist+=haversine(pts[i-1],pts[i]);
  let climb=0, alt=D.col.altitude_m;
  if(alt){ for(let i=1;i<D.n;i++){ if(alt[i]!=null&&alt[i-1]!=null){ const d=alt[i]-alt[i-1]; if(d>0) climb+=d; } } }
  sectionTitle(m,'Route');
  const g=el('div','kpis');
  g.append(
    kpi('GPS points', String(pts.length),''),
    kpi('GPS distance', (gdist/1000).toFixed(2),'km', 'haversine', C.gps),
    kpi('Total climb', alt?climb.toFixed(0):'–','m', alt?'sum of ascents':'no altitude', C.teal),
    kpi('Max altitude', alt?mx('altitude_m').toFixed(0):'–','m'),
  );
  m.append(g);

  // canvas route
  const card=el('div','chart-card'); const cv=el('canvas','mapcanvas'); card.append(cv); m.append(card);
  drawRoute(cv, pts);
  // altitude over time
  if(alt) makeChart(m,'Altitude (m)',[{label:'alt',col:'altitude_m',color:C.teal,dec:0}],150);
}
function drawRoute(cv, pts){
  const W=cv.parentElement.clientWidth-28, Hh=360;
  const dpr=window.devicePixelRatio||1;
  cv.width=W*dpr; cv.height=Hh*dpr; cv.style.width=W+'px'; cv.style.height=Hh+'px';
  const ctx=cv.getContext('2d'); ctx.scale(dpr,dpr);
  if(pts.length<2){ ctx.fillStyle=C.muted; ctx.font='13px -apple-system'; ctx.fillText('Not enough GPS points',12,24); return; }
  let latMn=Infinity,latMx=-Infinity,lonMn=Infinity,lonMx=-Infinity;
  for(const p of pts){ if(p.lat<latMn)latMn=p.lat; if(p.lat>latMx)latMx=p.lat; if(p.lon<lonMn)lonMn=p.lon; if(p.lon>lonMx)lonMx=p.lon; }
  const latMid=(latMn+latMx)/2, kx=Math.cos(latMid*Math.PI/180);   // lon compressed by latitude
  const pad=24;
  const spanLon=Math.max(1e-9,(lonMx-lonMn)*kx), spanLat=Math.max(1e-9,(latMx-latMn));
  const sc=Math.min((W-2*pad)/spanLon, (Hh-2*pad)/spanLat);
  const ox=(W-spanLon*sc)/2, oy=(Hh-spanLat*sc)/2;
  const X=p=>ox+((p.lon-lonMn)*kx)*sc, Y=p=>Hh-(oy+((p.lat-latMn))*sc);  // flip Y (north up)
  const spMax=Math.max(1,...pts.map(p=>p.sp));
  ctx.lineWidth=3; ctx.lineCap='round'; ctx.lineJoin='round';
  for(let i=1;i<pts.length;i++){ const frac=pts[i].sp/spMax;
    ctx.strokeStyle=`hsl(${190*(1-frac)},85%,55%)`;   // cyan(slow) -> red(fast)
    ctx.beginPath(); ctx.moveTo(X(pts[i-1]),Y(pts[i-1])); ctx.lineTo(X(pts[i]),Y(pts[i])); ctx.stroke();
  }
  // start / end markers
  ctx.fillStyle=C.gps; ctx.beginPath(); ctx.arc(X(pts[0]),Y(pts[0]),5,0,7); ctx.fill();
  ctx.fillStyle=C.error; ctx.beginPath(); ctx.arc(X(pts[pts.length-1]),Y(pts[pts.length-1]),5,0,7); ctx.fill();
  ctx.fillStyle=C.text2; ctx.font='11px -apple-system';
  ctx.fillText('● start', 12, Hh-22); ctx.fillStyle=C.error; ctx.fillText('● end', 70, Hh-22);
  ctx.fillStyle=C.muted; ctx.fillText('colour = speed (cyan slow → red fast)', 12, Hh-8);
}

/* register */
Object.assign(VIEWS, {
  histograms:viewHistograms, segments:viewSegments, efficiency:viewEfficiency, map:viewMap,
  motor:viewMotor, faults:viewFaults, stats:viewStats, replay:viewReplay, history:viewHistory,
});
