/* ============================================================
   REFERENCE / NORMS — where "is this normal?" comes from, with
   provenance. Mirrors data/pev-reference/*.json (community source)
   so the deployed app is self-contained; data/ stays canonical for
   the Python/Mac analysis side.

   The resolver norm(key) prefers the DEVICE'S OWN config (mcconf) as
   ground truth and only falls back to community/chemistry references,
   tagging each value with a trust tier so the UI can cite it.
   Loaded after app.js + pushback.js. Uses CFG,D,H,el,kpi,sectionTitle,
   VIEWS,clearMain,topbar,pbCfg,PB_OVERRIDE,switchView.
   ============================================================ */
const REF = {
  _sources: {
    safety:     'pev.dev + theboardgarage.com',
    benchmarks: 'pev.dev community benchmarks',
    params:     'Float Package docs',
    chemistry:  'Li-ion cell datasheet',
  },
  battery20s: { full_V:84, nominal_V:72, empty_V:60, cell_full_V:4.2, cell_nominal_V:3.6,
                cell_min_V:3.0, cell_warn_V:3.3, hv_tiltback_V:85, lv_tiltback_V:60 },
  temperature:{ fet_warning_C:65, fet_cutoff_start_C:70, fet_cutoff_end_C:80, fet_abs_max_C:100,
                motor_warning_C:75, motor_cutoff_start_C:80, motor_cutoff_end_C:90, motor_abs_max_C:100,
                battery_warning_C:50, battery_critical_C:60 },
  current:    { battery_max_A:30, battery_regen_max_A:15, motor_max_A:120 },
  duty:       { safe_max_pct:80, warning_pct:85, critical_pct:90 },
  imbalance:  { warn_mV:100, crit_mV:200 },
  efficiency: { city:{good:15,average:22,poor:35}, trail:{good:25,average:38,poor:55} },
  anomaly:    { temp_rise_warn_C_min:3, temp_rise_crit_C_min:6, sag_warn_V:4, sag_crit_V:7,
                landing_g:2.2, spikes_acceptable:3, spikes_investigate:5 },
};

/* trust tiers for any threshold, strongest → weakest */
const TIER = {
  config:    { label:'BOARD',     color:'#22c55e', hint:"this board's mcconf" },
  chemistry: { label:'CHEM',      color:'#14b8a6', hint:'Li-ion datasheet' },
  firmware:  { label:'FW',        color:'#06b6d4', hint:'Refloat firmware' },
  user:      { label:'YOU',       color:'#a855f7', hint:'edited in Pushback panel' },
  community: { label:'COMMUNITY', color:'#facc15', hint:'forum consensus, advisory' },
};
function tierTag(tier, source){ const t=TIER[tier]||{label:tier||'?',color:'#64748b'};
  return `<span class="srcref" style="border-color:${t.color};color:${t.color}" title="${source||t.hint||''}">${t.label}</span>`; }

function mcGet(k){ const mc=CFG.mcconf; if(!mc) return null;
  const v = mc[k] ?? mc[k.toUpperCase ? k.toUpperCase() : k]; return (v!=null && v!=='') ? +v : null; }

/* resolve a named threshold to {value,tier,source} — device config first */
function norm(key){
  const c = REF, S = REF._sources;
  switch(key){
    case 'fet_warn':  { const v=mcGet('l_temp_fet_start'); return v!=null?{value:v,tier:'config',source:'mcconf l_temp_fet_start'}:{value:c.temperature.fet_cutoff_start_C,tier:'community',source:S.safety}; }
    case 'fet_crit':  { const v=mcGet('l_temp_fet_end');   return v!=null?{value:v,tier:'config',source:'mcconf l_temp_fet_end'}:{value:c.temperature.fet_cutoff_end_C,tier:'community',source:S.safety}; }
    case 'mot_warn':  { const v=mcGet('l_temp_motor_start');return v!=null?{value:v,tier:'config',source:'mcconf l_temp_motor_start'}:{value:c.temperature.motor_cutoff_start_C,tier:'community',source:S.safety}; }
    case 'batt_warn': return {value:c.temperature.battery_warning_C,tier:'community',source:S.safety};
    case 'batt_crit': return {value:c.temperature.battery_critical_C,tier:'community',source:S.safety};
    case 'duty_warn': { const has=(typeof pbCfg==='function'&&D); const v=has?pbCfg().tiltback_duty:c.duty.safe_max_pct;
                        const edited=(typeof PB_OVERRIDE!=='undefined'&&PB_OVERRIDE.tiltback_duty!=null);
                        return {value:v,tier:edited?'user':(has?'firmware':'community'),source:edited?'Pushback panel':'Refloat tiltback_duty'}; }
    case 'duty_crit': return {value:c.duty.critical_pct,tier:'community',source:S.benchmarks};
    case 'cell_warn': return {value:c.battery20s.cell_warn_V,tier:'chemistry',source:S.chemistry};
    case 'cell_min':  return {value:c.battery20s.cell_min_V,tier:'chemistry',source:S.chemistry};
    case 'imbal_warn':return {value:c.imbalance.warn_mV,tier:'community',source:S.benchmarks};
    case 'imbal_crit':return {value:c.imbalance.crit_mV,tier:'community',source:S.benchmarks};
    case 'landing_g': return {value:c.anomaly.landing_g,tier:'community',source:S.benchmarks};
  }
  return {value:null,tier:'?',source:'?'};
}

/* ---------- live ride assessment (grade + findings + recommendations) ---------- */
function computeAssessment(d){
  const h=H(d), dims=[], rec=[];
  const push=(label,status,detail,tier)=>dims.push({label,status,detail,tier});

  const fw=norm('fet_warn'), fc=norm('fet_crit'), fet=h.mx('temp_fet_C');
  if(fet>0) push('FET temp', fet>fc.value?'err':fet>fw.value?'warn':'ok', `max ${fet.toFixed(0)}° (warn ${fw.value}°)`, fw.tier);
  const mw=norm('mot_warn'), mot=h.mx('temp_mot_C');
  if(mot>0) push('Motor temp', mot>mw.value?'warn':'ok', `max ${mot.toFixed(0)}° (warn ${mw.value}°)`, mw.tier);
  const bw=norm('batt_warn'), bc=norm('batt_crit'), bt=h.mx('temp_bat_C');
  if(bt>0) push('Battery temp', bt>bc.value?'err':bt>bw.value?'warn':'ok', `max ${bt.toFixed(0)}° (warn ${bw.value}°)`, bw.tier);

  const dw=norm('duty_warn'), dc=norm('duty_crit'), dmax=h.mx('duty_pct');
  let over=0,tot=0; const duty=d.col.duty_pct; if(duty) for(const v of duty){ if(v!=null){tot++; if(v>dw.value)over++;} }
  const pctOver=tot?100*over/tot:0;
  if(dmax>0){ push('Duty', (dmax>dc.value&&pctOver>1)?'err':dmax>dw.value?'warn':'ok',
    `max ${dmax.toFixed(0)}%, ${pctOver.toFixed(1)}% time >${dw.value}%`, dw.tier);
    if(dmax>dw.value) rec.push('Mind brief duty spikes — don’t push top speed on low battery.'); }

  if(typeof computePushback==='function' && typeof pbCfg==='function'){
    const pb=computePushback(d,pbCfg()), sum=pbSummary(d,pb.reason);
    const dom=Object.entries(sum.by).sort((a,b)=>b[1].dur-a[1].dur)[0];
    if(sum.seg.length) push('Pushback/haptic','warn',`${sum.seg.length} buzz windows, dominant ${dom?PB_REASONS[dom[0]].label:'—'}`,'firmware');
  }
  const hvAlerts=(d.alerts||[]).filter(a=>/cell voltage high|high voltage/i.test(a.label)).length;
  if(hvAlerts) rec.push('Balance-charge the pack — high-voltage buzz on braking points to cell imbalance.');

  if(h.has('cell_min')){ const cw=norm('cell_warn'), vmin=h.mn('cell_min');
    if(vmin>0) push('Min cell', vmin<cw.value?'warn':'ok', `${vmin.toFixed(2)}V (warn ${cw.value}V)`, cw.tier); }

  const dist=h.distanceKm(), whkm=dist>0?(h.last('watt_hours')-(h.last('wh_charged')||0))/dist:null;
  if(whkm!=null && whkm>0){ const e=REF.efficiency.city;
    push('Efficiency', whkm<=e.average?'ok':whkm<=e.poor?'warn':'err', `${whkm.toFixed(1)} Wh/km (good ≤${e.good})`, 'community'); }

  if(d.col.fault){ let n=0; for(const v of d.col.fault) if(v>0) n++;
    if(n){ push('Faults','err',`${n} fault samples`,'firmware'); rec.push('Investigate the logged mc_fault codes (Faults view).'); } }

  if(!CFG.mcconf) rec.push('Load this board’s mcconf (a Cardputer session) for board-exact, BOARD-tier limits.');

  const err=dims.filter(x=>x.status==='err').length, warn=dims.filter(x=>x.status==='warn').length;
  const score=Math.max(0, 100 - err*20 - warn*8);
  const grade = score>=90?'A':score>=82?'B+':score>=72?'B':score>=62?'C+':score>=50?'C':'D';
  const findings=dims.filter(x=>x.status!=='ok').map(x=>`${x.label}: ${x.detail}`);
  return { grade, score, dims, findings, rec };
}
function assessmentCard(parent, d){
  if(typeof norm!=='function') return;
  const a=computeAssessment(d);
  sectionTitle(parent,'Assessment');
  const gcol = a.score>=82?'#22c55e':a.score>=62?'#facc15':'#ef4444';
  const card=el('div','assess');
  card.innerHTML=`<div class="agrade" style="color:${gcol};border-color:${gcol}">${a.grade}<small>${a.score}</small></div>`;
  const body=el('div','abody');
  const chips=el('div','achips');
  a.dims.forEach(x=>{ const c=x.status==='err'?'#ef4444':x.status==='warn'?'#f97316':'#22c55e';
    const s=el('span','astat'); s.style.borderColor=c; s.title=x.detail+(x.tier?` · ${x.tier}`:'');
    s.innerHTML=`<i style="background:${c}"></i>${x.label}`; chips.append(s); });
  body.append(chips);
  if(a.findings.length){ const fd=el('div','afind'); fd.innerHTML='<b>Findings:</b> '+a.findings.join(' · '); body.append(fd); }
  if(a.rec.length){ const rc=el('ul','arec'); a.rec.forEach(r=>{ const li=el('li'); li.textContent=r; rc.append(li); }); body.append(rc); }
  card.append(body); parent.append(card);
}

/* ---------- Norms / reference view ---------- */
function viewNorms(){
  const m=clearMain(); topbar(m,'Norms / reference','where every "is this normal?" threshold comes from');

  // tier legend
  const leg=el('div','tierlegend');
  Object.entries(TIER).forEach(([k,t])=>{ const s=el('span','tieritem');
    s.innerHTML=`<i style="background:${t.color}"></i><b>${t.label}</b> ${t.hint}`; leg.append(s); });
  m.append(leg);

  const cfgLoaded = !!CFG.mcconf;
  const note=el('div','hint'); note.style.margin='0 2px 10px';
  note.innerHTML = cfgLoaded
    ? '✓ Device <b>mcconf loaded</b> — temperature & current thresholds use <b>this board\'s</b> limits (BOARD tier). Others fall back to community/chemistry references.'
    : 'No mcconf loaded for this session — using community/chemistry references. Load a <b>Cardputer session</b> (it logs mcconf) for board-exact, BOARD-tier thresholds.';
  m.append(note);

  sectionTitle(m,'Active thresholds (this session)');
  const keys=[['duty_warn','Duty warning','%'],['duty_crit','Duty critical','%'],
    ['fet_warn','FET warning','°C'],['fet_crit','FET critical','°C'],['mot_warn','Motor warning','°C'],
    ['batt_warn','Battery warning','°C'],['batt_crit','Battery critical','°C'],
    ['cell_warn','Low-cell warning','V'],['cell_min','Cell minimum','V'],
    ['imbal_warn','Imbalance warning','mV'],['imbal_crit','Imbalance critical','mV'],
    ['landing_g','Hard-landing','g']];
  const tbl=el('table','cfgtable'); tbl.innerHTML='<tr><th>threshold</th><th>value</th><th>tier</th><th>source</th></tr>';
  keys.forEach(([k,lab,u])=>{ const nv=norm(k); const tr=el('tr');
    tr.innerHTML=`<td>${lab}</td><td class="num mono">${nv.value!=null?nv.value+(u||''):'–'}</td>`+
      `<td>${tierTag(nv.tier,nv.source)}</td><td style="color:var(--muted);font-size:11.5px">${nv.source}</td>`;
    tbl.append(tr); });
  m.append(tbl);

  // Board config laid out like VESC Tool — current value + safe range + effect.
  if(typeof VT_PARAMS!=='undefined'){
    sectionTitle(m,'Board config — VESC Tool layout');
    const tierTip={wl:'auto-tunable (±15% guard)', manual:'change by hand in VESC Tool', ro:'detection result / read-only'};
    VT_PARAMS.forEach(grp=>{
      const h=el('div','vtsec'); h.textContent=grp.sec; m.append(h);
      const tbl=el('table','cfgtable');
      tbl.innerHTML='<tr><th>parameter</th><th>current</th><th>typical range</th><th>edit</th><th>effect</th></tr>';
      grp.items.forEach(p=>{ const cur=(typeof paramCurrent==='function')?paramCurrent(p.k):{v:null};
        const curStr = cur.v!=null ? `${cur.v}${p.u||''} <span class="src">${cur.src}</span>` : '—';
        const rng = p.range ? `${p.range[0]}…${p.range[1]}${p.u||''}` : 'detected';
        const tr=el('tr'); tr.title=p.path;
        tr.innerHTML=`<td>${p.n}<div class="vtkey">${p.k}</div></td>`+
          `<td class="num mono">${curStr}</td><td class="num mono" style="color:var(--muted)">${rng}</td>`+
          `<td><span class="srcref" title="${tierTip[p.tier]||''}">${p.tier.toUpperCase()}</span></td>`+
          `<td style="color:var(--text2);font-size:11.5px">${p.eff}</td>`;
        tbl.append(tr); });
      m.append(tbl);
    });
    const cn=el('div','hint'); cn.style.margin='4px 2px 12px';
    cn.innerHTML='Current value: <b>mcconf</b> = loaded config JSON · <b>backup</b> = decoded from the GAD backup (currents only so far) · <b>—</b> = needs full mcconf decode (fw-6.5 confgenerator). Changes are <b>manual in VESC Tool</b> — the app never writes to the VESC.';
    m.append(cn);
  }

  sectionTitle(m,'Reference library (community / datasheet)');
  const lib=el('div','reflib');
  const card=(title, obj, src)=>{ const c=el('div','refcard');
    let rows=''; const walk=(o,p='')=>{ for(const [k,v] of Object.entries(o)){
      if(v&&typeof v==='object') walk(v,p+k+'.'); else rows+=`<div class="refrow"><span>${p}${k}</span><b>${v}</b></div>`; } };
    walk(obj); c.innerHTML=`<div class="refh">${title}</div>${rows}<div class="refsrc">${src}</div>`; return c; };
  lib.append(
    card('Battery (20S)', REF.battery20s, REF._sources.safety),
    card('Temperature', REF.temperature, REF._sources.safety),
    card('Current', REF.current, REF._sources.safety),
    card('Duty cycle', REF.duty, REF._sources.benchmarks),
    card('Imbalance', REF.imbalance, REF._sources.benchmarks),
    card('Efficiency (Wh/km)', REF.efficiency, REF._sources.benchmarks),
    card('Anomaly thresholds', REF.anomaly, REF._sources.benchmarks),
  );
  m.append(lib);

  const foot=el('div','hint'); foot.style.margin='10px 2px';
  foot.innerHTML='Mirrors <code>data/pev-reference/</code>. <b>COMMUNITY</b> values are forum consensus (advisory), not a certified standard — <b>BOARD</b> (mcconf) and <b>CHEM</b> (datasheet) are the hard references.';
  m.append(foot);
}

/* register */
Object.assign(VIEWS, { norms:viewNorms });
