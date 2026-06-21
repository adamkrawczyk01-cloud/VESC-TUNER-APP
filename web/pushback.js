/* ============================================================
   PUSHBACK / HAPTIC — reconstruct *why & when* the board buzzes.
   Replays the Refloat decision tree (src/main.c ~L505-645 +
   haptic_feedback.c) per CSV sample against configurable thresholds.
   Thresholds seed from a loaded mcconf where possible, else Refloat
   defaults; all editable + persisted (board-level, localStorage).
   Loaded after app.js/views — uses C,H,el,kpi,sectionTitle,makeChart,
   chartCard,chartWidth,fmtTime,alertStrip,btn,VIEWS,CFG,D,render.
   ============================================================ */

// beep_reason codes mirror src/main.c (LV=1,HV=2,TEMPFET=3,TEMPMOT=4,CURRENT=5,DUTY=6,SPEED=11)
const PB_REASONS = {
  6:  { label:'Duty',        color:'#ef4444' },
  2:  { label:'High voltage',color:'#f97316' },
  1:  { label:'Low voltage', color:'#facc15' },
  3:  { label:'FET temp',    color:'#a855f7' },
  4:  { label:'Motor temp',  color:'#38bdf8' },
  5:  { label:'Current',     color:'#f43f5e' },
  11: { label:'Speed',       color:'#eab308' },
};

let PB_OVERRIDE = (()=>{ try{ return JSON.parse(localStorage.getItem('vesc_pb')||'{}'); }catch(e){ return {}; } })();
function pbSave(){ try{ localStorage.setItem('vesc_pb', JSON.stringify(PB_OVERRIDE)); }catch(e){} }

/* threshold defaults — from mcconf if loaded, else Refloat-style defaults */
function pbDefaults(d){
  const mc = CFG.mcconf || {};
  const g = (k,def)=>{ const v = mc[k] ?? mc[(k||'').toUpperCase()]; return (v!=null && v!=='') ? +v : def; };
  const maxV = d ? H(d).mx('voltage_V') : 84;
  const cells = Math.max(1, Math.round(maxV/4.2));
  return {
    cells,
    tiltback_duty: 80,                                   // % (Refloat tiltback_duty ×100)
    hv: g('l_max_vin', +(cells*4.25).toFixed(1)),        // pack HV pushback [V]
    lv: g('l_battery_cut_start', +(cells*3.0).toFixed(1)),// pack LV pushback [V]
    fet_max: g('l_temp_fet_start', 85) - 3,              // main.c: l_temp_fet_start − 3
    mot_max: g('l_temp_motor_start', 85) - 3,            // main.c: l_temp_motor_start − 3
    speed: 0,                                            // tiltback_speed (0 = off)
    cur_thresh: 80,                                      // haptic current_threshold [% saturation]
    i_mot_max: g('l_current_max', 0),                    // for current-saturation calc
    i_in_max: g('l_in_current_max', 0),
  };
}
function pbCfg(){ return { ...pbDefaults(D), ...PB_OVERRIDE }; }

/* per-sample reason + current saturation, replicating the firmware priority */
function computePushback(d, cfg){
  const n=d.n, c=d.col;
  const duty=c.duty_pct||[], V=c.voltage_V||[], Tf=c.temp_fet_C||[], Tm=c.temp_mot_C||[],
        sp=c.speed_kmh||[], Imot=c.curr_mot_A||[], Iin=c.curr_in_A||[];
  const reason=new Array(n).fill(0), cursat=new Array(n).fill(null);
  for(let i=0;i<n;i++){
    let cs=null;
    if(Imot[i]!=null && cfg.i_mot_max>0) cs=Math.abs(Imot[i])/cfg.i_mot_max;
    if(Iin[i]!=null && cfg.i_in_max>0)  cs=Math.max(cs||0, Math.abs(Iin[i])/cfg.i_in_max);
    cursat[i] = cs!=null ? cs*100 : null;

    const dv=duty[i], v=V[i];
    if(dv==null){ continue; }
    let r=0;
    if(dv > cfg.tiltback_duty) r=6;                                   // duty pushback
    else if(dv>5 && v!=null && v > cfg.hv) r=2;                       // high voltage
    else if(Tf[i]!=null && Tf[i] > cfg.fet_max) r=3;                  // FET temp
    else if(Tm[i]!=null && Tm[i] > cfg.mot_max) r=4;                  // motor temp
    else if(dv>5 && v!=null && v < cfg.lv){                           // low voltage (vsag-tolerant)
      const im=Math.abs(Imot[i]??0), vd=cfg.lv - v;
      if(vd>2 || im<5 || (im>0 && vd*20/im>1)) r=1;
    }
    else if(cfg.speed>0 && sp[i]!=null && sp[i] > cfg.speed) r=11;    // speed pushback
    if(r===0 && cursat[i]!=null && cursat[i] > cfg.cur_thresh) r=5;   // current-saturation buzz
    reason[i]=r;
  }
  return { reason, cursat };
}

/* group reason runs into segments + per-reason summary */
function pbSummary(d, reason){
  const seg=[]; let cur=0, t0=0;
  for(let i=0;i<d.n;i++){ const r=reason[i];
    if(r!==cur){ if(cur!==0) seg.push({r:cur,t0,t1:d.t[i]}); cur=r; t0=d.t[i]; } }
  if(cur!==0) seg.push({r:cur,t0,t1:d.t[d.n-1]||t0});
  const by={}; let total=0;
  seg.forEach(s=>{ const dur=s.t1-s.t0; total+=dur;
    if(!by[s.r]) by[s.r]={count:0,dur:0,first:s.t0}; by[s.r].count++; by[s.r].dur+=dur; });
  return { seg, by, total };
}

/* ---------- view ---------- */
function viewPushback(){
  const m=clearMain(); topbar(m,'Pushback / Haptic','reconstructed from Refloat thresholds — why & when it buzzes');
  alertStrip(m);
  pbPanel(m);

  const cfg=pbCfg();
  const pb=computePushback(D, cfg);
  D.col.cur_sat_pct = pb.cursat;                 // derived channel for charting
  const sum=pbSummary(D, pb.reason);
  const dom=Object.entries(sum.by).sort((a,b)=>b[1].dur-a[1].dur)[0];
  const ride=D.t[D.n-1]||0;

  sectionTitle(m,'Summary');
  const g=el('div','kpis');
  g.append(
    kpi('Buzz time', sum.total.toFixed(0),'s', ride?`${(100*sum.total/ride).toFixed(1)}% of session`:'', sum.total>0?C.warning:C.gps),
    kpi('Dominant', dom?PB_REASONS[dom[0]].label:'—','', dom?`${dom[1].dur.toFixed(0)}s`:'no buzz', dom?PB_REASONS[dom[0]].color:C.gps),
    kpi('Engagements', String(sum.seg.length),'', 'distinct buzz windows'),
    kpi('Peak cur-sat', pb.cursat.some(v=>v!=null)?Math.max(...pb.cursat.filter(v=>v!=null)).toFixed(0):'–','%', `buzz > ${cfg.cur_thresh}%`, C.error),
  );
  m.append(g);

  drawReasonRibbon(m, D, pb.reason);

  if(sum.seg.length){
    sectionTitle(m,'By reason');
    const tbl=el('table','cfgtable'); tbl.innerHTML='<tr><th>reason</th><th>count</th><th>total</th><th>first</th></tr>';
    Object.entries(sum.by).sort((a,b)=>b[1].dur-a[1].dur).forEach(([r,v])=>{ const p=PB_REASONS[r];
      const tr=el('tr'); tr.innerHTML=`<td style="color:${p.color};font-weight:700">${p.label}</td>`+
        `<td class="num mono">${v.count}</td><td class="num mono">${v.dur.toFixed(1)}s</td><td class="num mono">${fmtTime(v.first)}</td>`;
      tbl.append(tr); });
    m.append(tbl);
  } else {
    const ok=el('div','flag ok'); ok.innerHTML='<span class="ico">✓</span><div><div class="t">No buzz reconstructed</div><div class="d">no sample crossed a pushback/haptic threshold with these settings</div></div>'; m.append(ok);
  }

  sectionTitle(m,'Margins to threshold');
  makeChart(m,'Duty vs pushback',[{label:'duty %',col:'duty_pct',color:C.highlight,dec:0}],120,
    [{value:cfg.tiltback_duty,scale:'y',color:C.error,label:`tiltback_duty ${cfg.tiltback_duty}%`}]);
  makeChart(m,'Voltage vs HV / LV',[{label:'pack V',col:'voltage_V',color:C.teal,dec:1}],120,
    [{value:cfg.hv,scale:'y',color:C.warning,label:`HV ${cfg.hv}`},{value:cfg.lv,scale:'y',color:C.highlight,label:`LV ${cfg.lv}`}]);
  makeChart(m,'Temperatures vs limits',[
    {label:'FET',col:'temp_fet_C',color:C.error,dec:1},{label:'motor',col:'temp_mot_C',color:C.warning,dec:1}],120,
    [{value:cfg.fet_max,scale:'y',color:C.error,label:`FET ${cfg.fet_max}°`},{value:cfg.mot_max,scale:'y',color:C.warning,label:`motor ${cfg.mot_max}°`}]);
  if(pb.cursat.some(v=>v!=null))
    makeChart(m,'Current saturation',[{label:'sat %',col:'cur_sat_pct',color:'#f43f5e',dec:0}],120,
      [{value:cfg.cur_thresh,scale:'y',color:C.error,label:`buzz ${cfg.cur_thresh}%`}]);
}

/* editable threshold panel */
function pbPanel(parent){
  const cfg=pbCfg();
  const wrap=el('div','pbpanel');
  const fields=[
    ['tiltback_duty','Duty %'],['hv','HV V'],['lv','LV V'],
    ['fet_max','FET °C'],['mot_max','Motor °C'],['speed','Speed km/h'],
    ['cur_thresh','Cur-sat %'],['i_mot_max','I-mot max A'],['i_in_max','I-batt max A'],
  ];
  fields.forEach(([k,lab])=>{ const f=el('label','pbf'); const s=el('span'); s.textContent=lab;
    const inp=el('input'); inp.type='number'; inp.step='any'; inp.value=cfg[k];
    if(PB_OVERRIDE[k]!=null) inp.classList.add('edited');
    inp.onchange=()=>{ const v=parseFloat(inp.value); if(!isNaN(v)){ PB_OVERRIDE[k]=v; pbSave(); render('pushback'); } };
    f.append(s,inp); wrap.append(f); });
  wrap.append(btn('Reset', ()=>{ PB_OVERRIDE={}; pbSave(); render('pushback'); }, 'sm ghost'));
  parent.append(wrap);
  const note=el('div','hint'); note.style.margin='2px 2px 6px';
  note.textContent = CFG.mcconf
    ? 'Seeded from loaded mcconf (temps/current limits) + Refloat defaults — edit any value; saved per browser.'
    : 'No mcconf loaded → Refloat defaults + cells inferred from peak voltage. Edit to match your tune (load a Cardputer mcconf for exact limits).';
  parent.append(note);
}

/* horizontal ribbon coloured by buzz reason across the session timeline */
function drawReasonRibbon(parent, d, reason){
  const { body, leg } = chartCard(parent, 'Buzz reason over time');
  const seen=[...new Set(reason.filter(r=>r))];
  if(!seen.length){ const lp=el('div','lp'); const sw=el('span','sw'); sw.style.background=C.gps; lp.append(sw,document.createTextNode('OK (no buzz)')); leg.append(lp); }
  seen.forEach(r=>{ const p=PB_REASONS[r]; if(!p) return; const lp=el('div','lp'); const sw=el('span','sw'); sw.style.background=p.color; lp.append(sw,document.createTextNode(p.label)); leg.append(lp); });
  const cv=el('canvas'); body.append(cv);
  const W=chartWidth(body), Hh=30, dpr=window.devicePixelRatio||1;
  cv.width=W*dpr; cv.height=Hh*dpr; cv.style.width=W+'px'; cv.style.height=Hh+'px';
  const ctx=cv.getContext('2d'); ctx.scale(dpr,dpr);
  const t0=d.t[0], span=(d.t[d.n-1]-t0)||1, bw=Math.max(1, W/d.n + 0.6);
  const bg=getComputedStyle(document.body).getPropertyValue('--surface-deep').trim()||'#0f172a';
  ctx.fillStyle=bg; ctx.fillRect(0,0,W,Hh);
  for(let i=0;i<d.n;i++){ const r=reason[i]; if(!r) continue; const p=PB_REASONS[r]; if(!p) continue;
    ctx.fillStyle=p.color; ctx.fillRect(((d.t[i]-t0)/span)*W, 0, bw, Hh); }
  // alert markers from the FC Alert column, for cross-reference
  if(d.alerts) d.alerts.forEach(a=>{ ctx.fillStyle='#fff'; ctx.globalAlpha=.85;
    ctx.fillRect(((a.t-t0)/span)*W, 0, 1.5, Hh); ctx.globalAlpha=1; });
}

/* register */
Object.assign(VIEWS, { pushback:viewPushback });
