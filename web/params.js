/* ============================================================
   PARAMS — VESC-Tool-style parameter catalog + tuning advisor.
   The "expanded whitelist": every parameter we reason about, grouped
   like VESC Tool, with current board value + safe range + effect +
   the VESC-Tool path to change it. Plus tuningAdvice(): detect a
   problem in the session → say what it is → which params to change
   (MANUALLY in VESC Tool — the app never writes to the VESC).
   Loaded after reference.js. Uses C,el,kpi,sectionTitle,H,D,CFG,fmtTime.
   ============================================================ */

// Values decoded from the GAD board backup (backup_003 == session_004_mcconf.bin).
// Used as "current value" when no mcconf JSON is loaded. Only the current limits
// decode reliably from the raw blob; the rest await the fw-6.5 confgenerator.
const DECODED_BOARD = {
  l_current_max: 150, l_current_min: -150, l_abs_current_max: 225,
  l_in_current_max: 70, l_in_current_min: -45, l_max_erpm: 80000,
};

// editability tiers: 'wl' auto-tunable (±15% guard), 'manual' change by hand in
// VESC Tool, 'ro' read-only/derived (detection result — don't hand-edit lightly).
const VT_PARAMS = [
  { sec:'Motor · Current', items:[
    {k:'l_current_max',     n:'Motor Current Max',        u:'A', path:'Motor Cfg → General → Current', range:[20,150],  tier:'wl',     eff:'Peak motor torque. Higher = more accel, more heat + nosedive risk.'},
    {k:'l_current_min',     n:'Motor Current Max Brake',  u:'A', path:'Motor Cfg → General → Current', range:[-150,-20],tier:'manual', eff:'Braking torque. More negative = stronger brakes.'},
    {k:'l_abs_current_max', n:'Absolute Max Current',     u:'A', path:'Motor Cfg → General → Current', range:[40,300], tier:'manual', eff:'Hard fault trip. Must sit above the highest real peak.'},
    {k:'l_in_current_max',  n:'Battery Current Max',      u:'A', path:'Motor Cfg → General → Current', range:[10,90],  tier:'wl',     eff:'Battery draw cap — critical for pack health.'},
    {k:'l_in_current_min',  n:'Battery Current Max Regen',u:'A', path:'Motor Cfg → General → Current', range:[-60,-5], tier:'manual', eff:'Regen into the pack. Limited near full (HV buzz).'},
  ]},
  { sec:'Motor · Voltage', items:[
    {k:'l_battery_cut_start',n:'Battery Cutoff Start',    u:'V', path:'Motor Cfg → General → Voltage', range:[40,75],  tier:'manual', eff:'Where power starts tapering (LV).'},
    {k:'l_battery_cut_end',  n:'Battery Cutoff End',      u:'V', path:'Motor Cfg → General → Voltage', range:[36,72],  tier:'manual', eff:'Full cutoff. Set ~3.0 V/cell.'},
  ]},
  { sec:'Motor · RPM', items:[
    {k:'l_max_erpm',        n:'Max ERPM',                 u:'erpm', path:'Motor Cfg → General → RPM', range:[10000,150000], tier:'wl', eff:'Top-speed cap. Tiltback should trigger before this.'},
  ]},
  { sec:'Motor · Temperature', items:[
    {k:'l_temp_fet_start',  n:'MOSFET Temp Cutoff Start', u:'°C', path:'Motor Cfg → General → Temperature', range:[60,90], tier:'wl',     eff:'FET temp where current taper begins.'},
    {k:'l_temp_fet_end',    n:'MOSFET Temp Cutoff End',   u:'°C', path:'Motor Cfg → General → Temperature', range:[65,100],tier:'wl',     eff:'Full FET cutoff.'},
    {k:'l_temp_motor_start',n:'Motor Temp Cutoff Start',  u:'°C', path:'Motor Cfg → General → Temperature', range:[70,100],tier:'manual', eff:'Motor temp taper start (needs motor thermistor).'},
    {k:'l_temp_motor_end',  n:'Motor Temp Cutoff End',    u:'°C', path:'Motor Cfg → General → Temperature', range:[80,110],tier:'manual', eff:'Full motor cutoff.'},
  ]},
  { sec:'Motor · FOC', items:[
    {k:'foc_motor_r',          n:'Motor Resistance',      u:'mΩ',  scale:1000,  path:'Motor Cfg → FOC → General (Detect)', range:null, tier:'ro',     eff:'From detection. Wrong value → low-speed chatter / heat.'},
    {k:'foc_motor_l',          n:'Motor Inductance',      u:'µH',  scale:1e6,   path:'Motor Cfg → FOC → General (Detect)', range:null, tier:'ro',     eff:'From detection. Affects current-loop & observer.'},
    {k:'foc_motor_flux_linkage',n:'Flux Linkage',         u:'mWb', scale:1000,  path:'Motor Cfg → FOC → General (Detect)', range:null, tier:'ro',     eff:'From detection. Off value = #1 cause of low-speed crunch.'},
    {k:'foc_observer_gain',    n:'Observer Gain',         u:'',    path:'Motor Cfg → FOC → General',          range:null, tier:'manual', eff:'Position-estimate tracking. Too high = hunting/chatter at low speed.'},
    {k:'foc_openloop_rpm',     n:'Openloop ERPM',         u:'erpm',path:'Motor Cfg → FOC → Sensorless',       range:[0,2000], tier:'manual', eff:'How long it stays open-loop before handing to the observer.'},
    {k:'foc_sl_erpm',          n:'Sensorless ERPM',       u:'erpm',path:'Motor Cfg → FOC → Sensorless',       range:[500,5000], tier:'manual', eff:'Open-loop → sensorless handoff. The noisy zone for chatter.'},
    {k:'foc_current_kp',       n:'Current Loop Kp',       u:'',    path:'Motor Cfg → FOC → General',          range:null, tier:'manual', eff:'Current PI gain. Too high = audible whine/oscillation.'},
    {k:'foc_sensor_mode',      n:'Sensor Mode',           u:'',    path:'Motor Cfg → FOC → General',          range:null, tier:'ro',     eff:'Sensorless / Hall / Encoder.'},
  ]},
  { sec:'App · Float/Refloat', items:[
    {k:'tiltback_duty',     n:'Tiltback Duty',            u:'%', path:'App Cfg → Float → Tune', range:[60,90], tier:'manual', eff:'Duty where pushback engages. Your duty-Geiger limit.'},
  ]},
];

// current board value for a key: loaded config (mcconf JSON / VESC Tool XML)
// first, else the decoded GAD snapshot. Applies the display-unit scale (e.g.
// foc_motor_r Ω→mΩ). returns {v, src} or {v:null}.
function paramCurrent(k){
  let raw=null, src=null;
  const mc = CFG.mcconf;
  if(mc){ const v = mc[k] ?? mc[k.toUpperCase?.()]; if(v!=null && v!=='' && Math.abs(+v)<1e9){ raw=+v; src='config'; } }
  if(raw==null && (k in DECODED_BOARD)){ raw=DECODED_BOARD[k]; src='backup'; }
  if(raw==null) return {v:null, src:null};
  if(src!=='backup'){ const p=VT_PARAMS.flatMap(s=>s.items).find(p=>p.k===k); if(p&&p.scale) raw*=p.scale; }
  return { v: Math.round(raw*100)/100, src };
}

// Parse a VESC Tool "Save Configuration XML" into a flat {param: number} map.
// Leaf element tag names == param keys (l_current_max, foc_observer_gain, …).
function parseVescXML(text){
  const out={};
  try{
    const doc=new DOMParser().parseFromString(text,'text/xml');
    doc.querySelectorAll('*').forEach(el=>{
      if(el.children.length===0){ const s=(el.textContent||'').trim();
        if(s!=='' && !isNaN(+s)) out[el.tagName]=+s; }
    });
  }catch(e){}
  return out;
}

/* ---------- diagnostics ---------- */
// low-speed iq chatter ("crunching"): iq oscillates hard at low erpm, net torque ~0
function detectChatter(d){
  const iq=d.col.iq_A, erpm=d.col.rpm, idf=d.col.id_A;
  if(!iq || !erpm) return {present:false};
  let lowN=0, lowSum=0, peak=0, worstE=0;
  for(let i=1;i<d.n;i++){
    if(iq[i]==null||iq[i-1]==null||erpm[i]==null) continue;
    const e=Math.abs(erpm[i]), del=Math.abs(iq[i]-iq[i-1]);
    if(e>500 && e<3500){ lowN++; lowSum+=del; if(del>peak){peak=del; worstE=Math.round(e);} }
  }
  const meanDel = lowN? lowSum/lowN : 0;
  // chatter = brisk iq deltas in the low-erpm band with big peaks
  const present = lowN>50 && meanDel>1.2 && peak>40;
  const iqAmp = Math.max(Math.abs(H(d).mx('iq_A')), Math.abs(H(d).mn('iq_A')));
  return { present, meanDel:+meanDel.toFixed(1), peak:Math.round(peak), worstE, iqAmp:Math.round(iqAmp) };
}

// Build the diagnose→fix list for the active session. Each issue:
// {title, sev, benign, evidence, fixes:[{k,action,why}]}.
function tuningAdvice(d){
  const h=H(d), out=[];
  const N = (typeof norm==='function') ? norm : (()=>({value:null}));

  const ch=detectChatter(d);
  if(ch.present) out.push({
    title:'Low-speed motor chatter ("crunching")', sev:'info', benign:true,
    evidence:`iq swings ±${ch.iqAmp}A at low ERPM (worst Δ${ch.peak}A @ ~${ch.worstE} erpm), id≈0, net torque ~0 → observer hunting, not a mechanical fault.`,
    fixes:[
      {k:'foc_motor_flux_linkage', action:'Re-run FOC detection', why:'an off flux_linkage is the #1 cause of low-speed chatter'},
      {k:'foc_observer_gain',      action:'lower ~15–20%',         why:'less position-estimate hunting in the handoff zone'},
      {k:'foc_openloop_rpm',       action:'raise (e.g. +200)',     why:'stay open-loop through the noisy low-erpm band'},
      {k:'foc_sl_erpm',            action:'raise a little',        why:'delay the sensorless handoff past where it chatters'},
    ],
  });

  const fw=N('fet_warn'), fc=N('fet_crit'), fet=h.mx('temp_fet_C');
  if(fet>0 && fet>fw.value) out.push({
    title:`Controller running hot (max ${fet.toFixed(0)}°C)`, sev:fet>fc.value?'err':'warn', benign:false,
    evidence:`FET peaked ${fet.toFixed(0)}°C vs warn ${fw.value}°C.`,
    fixes:[
      {k:'l_current_max',    action:'lower ~10%',  why:'less heat per accel'},
      {k:'l_temp_fet_start', action:'verify',      why:'taper should start before damage'},
    ],
  });

  const dmax=h.mx('duty_pct'), dwarn=N('duty_warn').value;
  if(dmax>dwarn) out.push({
    title:`Duty hitting ${dmax.toFixed(0)}%`, sev:dmax>N('duty_crit').value?'err':'warn', benign:false,
    evidence:`Peak duty ${dmax.toFixed(0)}% — near the limit is nosedive territory.`,
    fixes:[
      {k:'l_max_erpm',    action:'verify headroom', why:'tiltback must engage before 100% duty'},
      {k:'tiltback_duty', action:'lower if needed', why:'earlier pushback = more margin'},
    ],
  });

  if(h.has('cell_delta_mV')){ const dm=h.mx('cell_delta_mV');
    if(dm>norm('imbal_warn').value) out.push({
      title:`Cell imbalance ${dm.toFixed(0)} mV`, sev:'warn', benign:false,
      evidence:`Max pack spread ${dm.toFixed(0)} mV — and HV buzz on braking gets worse as it grows.`,
      fixes:[{k:'(charger)', action:'balance charge', why:'BMS balances at the top — leave it on the charger after full'}],
    });
  }
  return out;
}
