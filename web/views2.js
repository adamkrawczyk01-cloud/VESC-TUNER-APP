/* ============================================================
   VESC Tuner — extra views: Compare (A/B), Diagnostics, Tuning, Live.
   Depends on globals from app.js: D, CMP, CFG, C, H, plot, resample,
   makeChart, clearMain, topbar, sectionTitle, kpi, el, $, VIEWS,
   switchView, parseCSV, computeFlags, distanceKm.
   ============================================================ */

/* small UI helpers reused here */
function btn(label, onclick, cls){ const b=el('button', 'loadbtn '+(cls||'')); b.textContent=label; b.onclick=onclick; b.style.margin='0'; return b; }
function pickFile(accept, cb){ const i=el('input'); i.type='file'; i.accept=accept; i.hidden=true;
  i.onchange=e=>{ const f=e.target.files[0]; if(f){ const r=new FileReader(); r.onload=()=>cb(r.result,f.name); r.readAsText(f);} };
  document.body.append(i); i.click(); setTimeout(()=>i.remove(),0); }

/* ============================================================
   COMPARE  (A = D, B = CMP) — overlay on a shared time grid
   ============================================================ */
function viewCompare(){
  const m=clearMain(); topbar(m,'Compare A / B', 'overlay two rides on a shared timeline');
  const bar=el('div','cmpbar');
  const aTag=el('span','tag a'); aTag.textContent='A · '+(D?D.name:'—');
  const bTag=el('span','tag b'); bTag.textContent='B · '+(CMP?CMP.name:'load a second session');
  bar.append(aTag,bTag);
  bar.append(btn('↑ Load B', ()=>pickFile('.csv',(txt,name)=>{ CMP=parseCSV(txt,name); render('compare'); }), 'sm'));
  if (CMP) bar.append(btn('✕ Clear B', ()=>{ CMP=null; render('compare'); }, 'sm ghost'));
  if (CMP) bar.append(btn('⇄ Swap', ()=>{ const t=D; D=CMP; CMP=t; renderSidebar(); render('compare'); }, 'sm ghost'));
  m.append(bar);

  if(!CMP){ const e=el('div','empty'); e.textContent='Load session B to compare. A is the active session.'; m.append(e); return; }

  // diff KPIs
  const ha=H(D), hb=H(CMP);
  sectionTitle(m,'Deltas (B vs A)');
  const g=el('div','kpis');
  const rows=[
    ['Top speed', ha.mx('speed_kmh'), hb.mx('speed_kmh'),'km/h',1,false],
    ['Avg speed', ha.avg('speed_kmh'), hb.avg('speed_kmh'),'km/h',1,false],
    ['Distance', ha.distanceKm(), hb.distanceKm(),'km',2,false],
    ['Peak duty', ha.mx('duty_pct'), hb.mx('duty_pct'),'%',0,true],
    ['Max A in', ha.mx('curr_in_A'), hb.mx('curr_in_A'),'A',0,true],
    ['Wh/km', whPerKm(ha), whPerKm(hb),'Wh/km',1,true],
    ['Max FET', ha.mx('temp_fet_C'), hb.mx('temp_fet_C'),'°C',0,true],
    ['Min cell', ha.mn('cell_min')||ha.last('vcell_V'), hb.mn('cell_min')||hb.last('vcell_V'),'V',3,false],
  ];
  rows.forEach(([lab,a,b,u,dec,lowerBetter])=>{
    const dv=b-a, sign=dv>0?'+':'';
    const good = lowerBetter ? dv<0 : dv>0;
    const col = Math.abs(dv)<1e-9 ? C.muted : good ? C.gps : C.warning;
    const k=kpi(lab, b.toFixed(dec), u, `A ${a.toFixed(dec)} · Δ ${sign}${dv.toFixed(dec)}`, col);
    g.append(k);
  });
  m.append(g);

  // overlay charts on common grid
  const durA=D.t[D.n-1]||0, durB=CMP.t[CMP.n-1]||0, dur=Math.max(durA,durB);
  const dt=0.1, grid=[]; for(let s=0;s<=dur;s+=dt) grid.push(+s.toFixed(2));
  const ov=(col)=>([
    {label:'A '+col, color:C.bran, xs:grid, ys:resample(D.t, D.col[col]||[], grid), dec:2},
    {label:'B '+col, color:C.warning, xs:grid, ys:resample(CMP.t, CMP.col[col]||[], grid), dec:2, dash:true},
  ]);
  sectionTitle(m,'Overlay');
  plot(m,'Speed (km/h)', grid, ov('speed_kmh'),130);
  plot(m,'Duty (%)', grid, ov('duty_pct'));
  plot(m,'Current in (A)', grid, ov('curr_in_A'));
  plot(m,'FET temp (°C)', grid, ov('temp_fet_C'));
  plot(m,'Pack voltage (V)', grid, ov('voltage_V'));
}
function whPerKm(h){ const d=h.distanceKm(); return d>0 ? h.last('watt_hours')/d : 0; }

/* ============================================================
   DIAGNOSTICS — internal resistance, voltage sag, thermal headroom
   ============================================================ */
function viewDiag(){
  const m=clearMain(); topbar(m,'Diagnostics','battery health & thermal headroom');
  const h=H(D);

  /* ---- pack internal resistance from V-vs-I regression ---- */
  sectionTitle(m,'Pack internal resistance');
  const Rpack = linregSlope(D.col.curr_in_A, D.col.voltage_V); // dV/dI (negative)
  const rmOhm = Rpack!=null ? -Rpack*1000 : null;
  const g=el('div','kpis');
  g.append(
    kpi('Pack R (est.)', rmOhm!=null?rmOhm.toFixed(1):'–','mΩ', 'slope of V vs I', rmOhm>50?C.warning:C.gps),
    kpi('V no-load', h.mx('voltage_V').toFixed(1),'V','observed max'),
    kpi('V @ peak A', vAtPeakCurrent(h).toFixed(1),'V', `at ${h.mx('curr_in_A').toFixed(0)}A`, C.wheel),
    kpi('Max sag', maxSag(h).toFixed(1),'V','no-load − loaded', C.warning),
  );
  m.append(g);

  /* ---- per-cell internal resistance ---- */
  if (D.cells.length){
    sectionTitle(m,'Per-cell resistance (dV/dI vs pack current)');
    const I = D.col.curr_in_A;
    const cellR = D.cells.map(c=>{ const s=linregSlope(I, D.col[c]); return s!=null? -s*1000 : null; });
    const valid = cellR.filter(v=>v!=null && isFinite(v));
    const worst = valid.length?Math.max(...valid):0;
    const grid2=el('div','cellgrid');
    D.cells.forEach((c,i)=>{ const r=cellR[i];
      const bad = r!=null && r>=worst-1e-6 && valid.length>1;
      const box=el('div','cellbox'+(bad?' low':''));
      const f = r!=null && worst>0 ? Math.min(1, r/worst)*100 : 0;
      box.innerHTML=`<div class="cn">CELL ${String(i+1).padStart(2,'0')}</div>`+
        `<div class="cv">${r!=null?r.toFixed(1):'–'}<small style="font-size:10px;color:var(--muted)"> mΩ·n</small></div>`+
        `<div class="bar"><i style="width:${f}%;background:${bad?C.warning:C.teal}"></i></div>`;
      grid2.append(box);
    });
    m.append(grid2);
    const note=el('div','hint'); note.style.margin='2px 2px 8px';
    note.textContent='Relative per-cell figure (×N for cells in parallel). A cell standing out = weakest in the pack — watch it.';
    m.append(note);
  }

  /* ---- sag scatter & thermal ---- */
  sectionTitle(m,'Voltage under load');
  makeChart(m,'Pack V vs current (time)',[
    {label:'V',col:'voltage_V',color:C.teal,dec:1},
    {label:'A in',col:'curr_in_A',color:C.wheel,scale:'y2',dec:0},
  ],140);

  sectionTitle(m,'Thermal headroom');
  const tg=el('div','kpis');
  const fetMax=h.mx('temp_fet_C'), rise=tempRiseRate(D,'temp_fet_C');
  const limStart = cfgNum('l_temp_fet_start'), limEnd = cfgNum('l_temp_fet_end');
  tg.append(
    kpi('Max FET', fetMax.toFixed(0),'°C', limEnd?`limit ${limEnd}°C`:'', fetMax>70?C.error:C.text),
    kpi('Headroom', limEnd?(limEnd-fetMax).toFixed(0):'–','°C','to throttle', (limEnd&&limEnd-fetMax<10)?C.error:C.gps),
    kpi('Rise rate', rise!=null?rise.toFixed(1):'–','°C/min','sustained load'),
    kpi('Time to limit', (rise>0&&limEnd)?(((limEnd-fetMax)/rise).toFixed(1)):'–','min','at current rate', C.warning),
    kpi('Max motor', h.mx('temp_mot_C').toFixed(0),'°C'),
  );
  m.append(tg);
  makeChart(m,'Temperatures',[
    {label:'FET',col:'temp_fet_C',color:C.error,dec:1},
    {label:'motor',col:'temp_mot_C',color:C.warning,dec:1},
  ],128, (limStart||limEnd)?[
    limStart!=null?{value:limStart,scale:'y',color:C.warning,label:`fet_start ${limStart}°`}:null,
    limEnd!=null?{value:limEnd,scale:'y',color:C.error,label:`fet_end ${limEnd}°`}:null,
  ].filter(Boolean):null);

  /* ---- duty-cycle headroom (#9) ---- */
  sectionTitle(m,'Duty-cycle headroom');
  const duty=D.col.duty_pct||[];
  const cnt=(p)=>{ let k=0; for(const v of duty) if(v!=null&&p(v)) k++; return k; };
  const n90=cnt(v=>v>90), n95=cnt(v=>v>95), peak=h.mx('duty_pct');
  const dg=el('div','kpis');
  dg.append(
    kpi('Peak duty', peak.toFixed(0),'%', null, peak>95?C.error:peak>90?C.warning:C.gps),
    kpi('Headroom', (100-peak).toFixed(0),'%','to 100%', (100-peak)<5?C.error:C.text),
    kpi('Time >90%', (n90/Math.max(1,D.n)*100).toFixed(1),'%', `${n90} samples`, n90?C.warning:C.gps),
    kpi('Time >95%', (n95/Math.max(1,D.n)*100).toFixed(1),'%','spin-out risk', n95?C.error:C.gps),
  );
  m.append(dg);
  const dnote=el('div','hint'); dnote.style.margin='0 2px 8px';
  dnote.textContent = peak>95 ? 'Duty hit the ceiling — top speed is voltage-limited. More cells or lower load needed before pushing harder.'
    : peak>90 ? 'Brushing the limit. Leave margin — high duty + load is where cut-outs happen.'
    : 'Comfortable duty headroom for this ride.';
  m.append(dnote);

  /* ---- nosedive risk (#10): pitch lagging setpoint while duty/load is high ---- */
  if(has('pitch_deg') && has('setpoint_deg')){
    sectionTitle(m,'Nosedive risk');
    const pit=D.col.pitch_deg, sp=D.col.setpoint_deg, vmin=h.mn('voltage_V');
    let worst=0, worstT=0, riskN=0;
    const errCol=new Array(D.n).fill(null);
    for(let i=0;i<D.n;i++){ if(pit[i]==null||sp[i]==null) continue;
      const err=sp[i]-pit[i];           // board asks for more nose-up than it has
      errCol[i]=err;
      const hot=(duty[i]||0)>80;
      if(hot && err>2){ riskN++; if(err>worst){ worst=err; worstT=D.t[i]; } }
    }
    const ng=el('div','kpis');
    ng.append(
      kpi('Risk samples', String(riskN),'', 'duty>80% & lag>2°', riskN>5?C.error:riskN?C.warning:C.gps),
      kpi('Worst lag', worst.toFixed(1),'°', riskN?('@'+fmtTime(worstT)):'none', worst>5?C.error:C.text),
      kpi('Min voltage', vmin.toFixed(1),'V','under load', vmin<h.mx('voltage_V')*0.8?C.warning:C.text),
    );
    m.append(ng);
    D.col.__pitcherr = errCol;   // ephemeral column for the chart
    plot(m,'Setpoint − pitch (lag) vs duty', D.t, [
      {label:'lag °',color:C.error,xs:D.t,ys:errCol,dec:1},
      {label:'duty %',color:C.highlight,xs:D.t,ys:duty,scale:'y2',dec:0},
    ],150);
    const nn=el('div','hint'); nn.style.margin='6px 2px';
    nn.textContent='Large positive lag while duty is high = the motor can’t hold the nose up. Sustained spikes here are the classic nosedive precursor.';
    m.append(nn);
  }
}

/* least-squares slope of y vs x over paired non-null samples */
function linregSlope(xa, ya){
  if(!xa||!ya) return null;
  let n=0,sx=0,sy=0,sxx=0,sxy=0;
  for(let i=0;i<xa.length;i++){ const x=xa[i],y=ya[i]; if(x==null||y==null) continue;
    n++; sx+=x; sy+=y; sxx+=x*x; sxy+=x*y; }
  if(n<8) return null;
  const den=n*sxx-sx*sx; if(Math.abs(den)<1e-9) return null;
  return (n*sxy-sx*sy)/den;
}
function vAtPeakCurrent(h){ const I=h.d.col.curr_in_A, V=h.d.col.voltage_V; if(!I||!V) return 0;
  let bi=-1,bc=-Infinity; for(let i=0;i<I.length;i++) if(I[i]!=null&&I[i]>bc){bc=I[i];bi=i;} return bi>=0&&V[bi]!=null?V[bi]:0; }
function maxSag(h){ const noLoad=h.mx('voltage_V'); return noLoad - vAtPeakCurrent(h); }
function tempRiseRate(d, c){ // °C/min over the steepest sustained 60s window
  const a=d.col[c], t=d.t; if(!a) return null;
  let best=null;
  for(let i=0;i<d.n;i++){ if(a[i]==null) continue;
    let j=i; while(j<d.n-1 && t[j]-t[i]<60) j++;
    if(t[j]-t[i]<30 || a[j]==null) continue;
    const r=(a[j]-a[i])/((t[j]-t[i])/60);
    if(best==null||r>best) best=r;
  }
  return best;
}

/* ============================================================
   TUNING — mcconf / appconf / suggestions loop
   ============================================================ */
const WHITELIST = ['l_current_max','l_in_current_max','l_max_erpm','l_temp_fet_start','l_temp_fet_end'];
const MAX_CHANGE = 0.15;
function cfgNum(k){ const mc=CFG.mcconf; if(!mc) return null; const v=mc[k]??mc[k.toUpperCase()]; return v!=null?+v:null; }

function viewTuning(){
  const m=clearMain(); topbar(m,'Tuning loop','config snapshot · suggestions · AI report');
  const bar=el('div','cmpbar');
  bar.append(
    btn('↑ mcconf.json', ()=>pickFile('.json',(t)=>{ CFG.mcconf=safeJSON(t); render('tuning'); }),'sm'),
    btn('↑ appconf.json', ()=>pickFile('.json',(t)=>{ CFG.appconf=safeJSON(t); render('tuning'); }),'sm'),
    btn('↑ suggestions.json', ()=>pickFile('.json',(t)=>{ CFG.suggestions=safeJSON(t); render('tuning'); }),'sm'),
  );
  m.append(bar);

  // status row
  const st=el('div','hint'); st.style.margin='0 2px 6px';
  st.textContent = `mcconf: ${CFG.mcconf?'loaded':'—'}   ·   appconf: ${CFG.appconf?'loaded':'—'}   ·   suggestions: ${CFG.suggestions?'loaded':'—'}`;
  m.append(st);

  /* ---- whitelist current values ---- */
  if (CFG.mcconf){
    sectionTitle(m,'Current config (whitelist)');
    const tbl=el('table','cfgtable');
    tbl.innerHTML='<tr><th>parameter</th><th>value</th></tr>';
    WHITELIST.forEach(k=>{ const v=cfgNum(k);
      const tr=el('tr'); tr.innerHTML=`<td class="mono">${k}</td><td class="mono">${v!=null?v:'—'}</td>`; tbl.append(tr); });
    m.append(tbl);
  } else {
    const e=el('div','empty'); e.textContent='Load a session’s mcconf.json to view config and validate suggestions.'; m.append(e);
  }

  /* ---- suggestions diff with ±15% guard ---- */
  if (CFG.suggestions){
    sectionTitle(m,'Suggested changes');
    const sugg = normalizeSuggestions(CFG.suggestions);
    const tbl=el('table','cfgtable');
    tbl.innerHTML='<tr><th>parameter</th><th>current</th><th>suggested</th><th>Δ%</th><th>status</th></tr>';
    sugg.forEach(s=>{
      const cur = cfgNum(s.param);
      const pct = (cur!=null && cur!==0) ? (s.value-cur)/Math.abs(cur) : null;
      const wl = WHITELIST.includes(s.param);
      let status, cls;
      if(!wl){ status='✕ not in whitelist'; cls='bad'; }
      else if(pct!=null && Math.abs(pct)>MAX_CHANGE+1e-9){ status=`✕ exceeds ±15%`; cls='bad'; }
      else { status='✓ safe'; cls='ok'; }
      const tr=el('tr');
      tr.innerHTML=`<td class="mono">${s.param}</td><td class="mono">${cur!=null?cur:'—'}</td>`+
        `<td class="mono">${s.value}</td><td class="mono">${pct!=null?(pct*100).toFixed(1)+'%':'—'}</td>`+
        `<td class="cell-${cls}">${status}</td>`;
      tbl.append(tr);
    });
    m.append(tbl);
    const ok = sugg.every(s=>{ const cur=cfgNum(s.param); const pct=(cur!=null&&cur!==0)?(s.value-cur)/Math.abs(cur):null;
      return WHITELIST.includes(s.param) && !(pct!=null && Math.abs(pct)>MAX_CHANGE+1e-9); });
    const note=el('div','hint'); note.style.margin='8px 2px';
    note.innerHTML = ok ? '<span style="color:var(--gps)">All suggestions pass whitelist + ±15% guard.</span>'
                        : '<span style="color:var(--warning)">Some suggestions violate the safety rules and would be rejected on-device.</span>';
    m.append(note);
  }

  /* ---- AI report export ---- */
  sectionTitle(m,'AI tuning report');
  const note=el('div','hint'); note.style.margin='0 2px 10px';
  note.textContent='Export a markdown digest of this session (+ config) to paste into Claude for tuning suggestions.';
  m.append(note);
  m.append(btn('⬇ Export report.md', ()=>downloadText(buildReport(), (D?D.name:'session')+'_report.md', 'text/markdown'),''));
}

function safeJSON(t){ try{ return JSON.parse(t); }catch(e){ alert('Invalid JSON'); return null; } }
function normalizeSuggestions(s){
  // accept {param:value,...}, [{param,value}], or {suggestions:[...]}
  if (Array.isArray(s)) return s.map(x=>({param:x.param??x.name, value:+(x.value??x.suggested??x.to)}));
  if (s.suggestions) return normalizeSuggestions(s.suggestions);
  if (s.changes) return normalizeSuggestions(s.changes);
  return Object.entries(s).filter(([,v])=>typeof v==='number'||!isNaN(+v)).map(([param,value])=>({param,value:+value}));
}
function downloadText(text, name, mime){
  const blob=new Blob([text],{type:mime||'text/plain'}); const url=URL.createObjectURL(blob);
  const a=el('a'); a.href=url; a.download=name; a.click(); setTimeout(()=>URL.revokeObjectURL(url),1000);
}
function buildReport(){
  if(!D) return '# No session loaded';
  const h=H(D), dur=(D.t[D.n-1]||0)/60, dist=h.distanceKm();
  const L=[];
  L.push(`# VESC ride report — ${D.name}`,'');
  L.push('## Session summary');
  L.push(`- Duration: ${dur.toFixed(1)} min`);
  L.push(`- Distance: ${dist.toFixed(2)} km`);
  L.push(`- Top / avg speed: ${h.mx('speed_kmh').toFixed(1)} / ${h.avg('speed_kmh').toFixed(1)} km/h`);
  L.push(`- Peak duty: ${h.mx('duty_pct').toFixed(0)}%`);
  L.push(`- Max current in / motor: ${h.mx('curr_in_A').toFixed(0)} / ${h.mx('curr_mot_A').toFixed(0)} A`);
  L.push(`- Energy: ${h.last('watt_hours').toFixed(0)} Wh${dist>0?` (${(h.last('watt_hours')/dist).toFixed(1)} Wh/km)`:''}`);
  L.push(`- Max FET / motor temp: ${h.mx('temp_fet_C').toFixed(0)} / ${h.mx('temp_mot_C').toFixed(0)} °C`);
  if (h.has('cell_min')) L.push(`- Min cell / max Δ: ${h.mn('cell_min').toFixed(3)} V / ${h.mx('cell_delta_mV').toFixed(0)} mV`);
  L.push('');
  L.push('## Auto-flags');
  const flags=computeFlags(D);
  if(!flags.length) L.push('- none — duty, temps, balance, faults nominal');
  else flags.forEach(f=>L.push(`- [${f.sev}] ${f.title} — ${f.detail} (@${f.when})`));
  L.push('');
  if (CFG.mcconf){
    L.push('## Current config (whitelist)');
    WHITELIST.forEach(k=>{ const v=cfgNum(k); L.push(`- ${k}: ${v!=null?v:'—'}`); });
    L.push('');
  }
  L.push('## Request');
  L.push('Suggest VESC parameter changes for this ride. Rules: only whitelist params '+
    `(${WHITELIST.join(', ')}), max ±15% change each, relative to the current config above. `+
    'Return JSON: {"suggestions":[{"param":"...","value":N,"reason":"..."}]}.');
  return L.join('\n');
}

/* ============================================================
   LIVE — HTTP polling of the Cardputer LAN server (/api/live)
   The device runs a sync WebServer (no WebSocket); we poll JSON at
   ~5Hz and plot a rolling window. CORS is open on the device.
   ============================================================ */
let LIVE = { timer:null, buf:{}, t0:null, max:600, ms:200, host:localStorage.getItem('vesc_host')||'vesctuner.local' };
const LIVE_FIELDS = [
  {key:'speed', label:'speed km/h', color:C.warning},
  {key:'duty',  label:'duty %',     color:C.highlight, scale:'y2'},
  {key:'vin',   label:'V',          color:C.teal},
  {key:'ain',   label:'A in',       color:C.wheel},
  {key:'fet',   label:'FET °C',     color:C.error},
  {key:'pitch', label:'pitch °',    color:C.bran},
];
function viewLive(){
  const m=clearMain(); topbar(m,'Live','real-time telemetry over WiFi');
  const bar=el('div','cmpbar');
  const inp=el('input','hostinput'); inp.value=LIVE.host; inp.placeholder='vesctuner.local';
  inp.onchange=()=>{ LIVE.host=inp.value.trim(); localStorage.setItem('vesc_host',LIVE.host); };
  const status=el('span','tag'); status.id='live-status'; status.textContent='disconnected';
  bar.append(inp,
    btn('Connect', ()=>liveConnect(), 'sm'),
    btn('Disconnect', ()=>liveDisconnect(), 'sm ghost'),
    status);
  m.append(bar);
  const note=el('div','hint'); note.style.margin='0 2px 10px';
  note.innerHTML='Polls <span class="mono">http://&lt;host&gt;/api/live</span> on the Cardputer (home WiFi). '+
    'Open this dashboard over <b>http</b> (not the https GitHub Pages copy) — browsers block http requests from an https page.';
  m.append(note);

  // live gauges
  const g=el('div','kpis'); g.id='live-kpis';
  LIVE_FIELDS.forEach(f=> g.append(kpi(f.label, '–', '', null)));
  m.append(g);

  // rolling chart container
  const host=el('div'); host.id='live-charts'; m.append(host);
  renderLiveChart();
}
function renderLiveChart(){
  const host=$('#live-charts'); if(!host) return; host.innerHTML='';
  const xs = (LIVE.buf.t && LIVE.buf.t.length) ? LIVE.buf.t : [0];
  const lines = LIVE_FIELDS.map(f=>{ const y=LIVE.buf[f.key];
    return {label:f.label,color:f.color,scale:f.scale||'y',xs, ys:(y&&y.length)?y:xs.map(()=>null), dec:1}; });
  // reuse plot() by faking a dataset-independent call
  plot(host, 'Live telemetry', xs, lines, 220);
}
function liveBase(){ const h=LIVE.host.replace(/^https?:\/\//,'').replace(/\/$/,''); return `http://${h}`; }
function liveConnect(){
  liveDisconnect();
  LIVE.buf={t:[]}; LIVE.t0=null; LIVE_FIELDS.forEach(f=>LIVE.buf[f.key]=[]);
  const setS=(t,c)=>{ const s=$('#live-status'); if(s){ s.textContent=t; s.style.color=c||''; } };
  let misses=0;
  const poll=async()=>{
    try{
      const r=await fetch(liveBase()+'/api/live',{cache:'no-store'});
      const f=await r.json(); misses=0;
      setS(f.ble?'● live · BLE ok':'● live · no BLE', f.ble?C.gps:C.warning);
      liveFrame(f);
    }catch(e){ if(++misses>3) setS('offline — host reachable?', C.error); }
  };
  setS('connecting…', C.highlight);
  poll(); LIVE.timer=setInterval(poll, LIVE.ms);
}
function liveDisconnect(){ if(LIVE.timer){ clearInterval(LIVE.timer); LIVE.timer=null; }
  const s=$('#live-status'); if(s){ s.textContent='disconnected'; s.style.color=C.muted; } }
function liveFrame(f){
  const now = f.t!=null ? f.t/1000 : (LIVE.buf.t.length?LIVE.buf.t[LIVE.buf.t.length-1]+0.1:0);
  if(LIVE.t0==null) LIVE.t0=now;
  LIVE.buf.t.push(+(now-LIVE.t0).toFixed(2));
  LIVE_FIELDS.forEach(fl=> LIVE.buf[fl.key].push(f[fl.key]!=null?+f[fl.key]:null));
  // trim window
  while(LIVE.buf.t.length>LIVE.max){ LIVE.buf.t.shift(); LIVE_FIELDS.forEach(fl=>LIVE.buf[fl.key].shift()); }
  // update gauges
  const kp=$('#live-kpis'); if(kp){ LIVE_FIELDS.forEach((fl,i)=>{ const v=f[fl.key];
    const node=kp.children[i]?.querySelector('.v'); if(node) node.textContent=(v!=null?(+v).toFixed(1):'–'); }); }
  // update chart in place (cheap: full setData on the single live chart)
  const u=CHARTS[CHARTS.length-1];
  if(u){ u.setData([LIVE.buf.t, ...LIVE_FIELDS.map(fl=>LIVE.buf[fl.key])]); }
}

/* register new views */
Object.assign(VIEWS, { compare:viewCompare, diag:viewDiag, tuning:viewTuning, live:viewLive });
