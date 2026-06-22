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
    {k:'foc_observer_type',    n:'Observer Type',         u:'', enumNames:['Ortega original','MXLEMMING','MXLEMMING λ-comp','Ortega λ-comp'], path:'Motor Cfg → FOC → General', range:null, tier:'manual', eff:'Observer algorithm — changing it often fixes low-speed chatter.'},
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

// Exact VESC mcconf serialize layout (confgenerator_serialize_mcconf, fw 6.06).
// Type codes: u8 / f32 (float32_auto = IEEE BE) / f16:<scale> (int16 BE / scale)
// / h8 (int8[8]). Validated against the GAD board blob (foc_observer_type @251).
const MCCONF_SCHEMA = [
  ['pwm_mode','u8'],['comm_mode','u8'],['motor_type','u8'],['sensor_mode','u8'],
  ['l_current_max','f32'],['l_current_min','f32'],['l_in_current_max','f32'],['l_in_current_min','f32'],
  ['l_in_current_map_start','f16',10000],['l_in_current_map_filter','f16',10000],
  ['l_abs_current_max','f32'],['l_min_erpm','f32'],['l_max_erpm','f32'],
  ['l_erpm_start','f16',10000],['l_max_erpm_fbrake','f32'],['l_max_erpm_fbrake_cc','f32'],
  ['l_min_vin','f16',10],['l_max_vin','f16',10],['l_battery_cut_start','f16',10],['l_battery_cut_end','f16',10],
  ['l_battery_regen_cut_start','f16',10],['l_battery_regen_cut_end','f16',10],['l_slow_abs_current','u8'],
  ['l_temp_fet_start','u8'],['l_temp_fet_end','u8'],['l_temp_motor_start','u8'],['l_temp_motor_end','u8'],
  ['l_temp_accel_dec','f16',10000],['l_min_duty','f16',10000],['l_max_duty','f16',10000],
  ['l_watt_max','f32'],['l_watt_min','f32'],
  ['l_current_max_scale','f16',10000],['l_current_min_scale','f16',10000],['l_duty_start','f16',10000],
  ['sl_min_erpm','f32'],['sl_min_erpm_cycle_int_limit','f32'],['sl_max_fullbreak_current_dir_change','f32'],
  ['sl_cycle_int_limit','f16',10],['sl_phase_advance_at_br','f16',10000],['sl_cycle_int_rpm_br','f32'],['sl_bemf_coupling_k','f32'],
  ['hall_table','h8'],['hall_sl_erpm','f32'],
  ['foc_current_kp','f32'],['foc_current_ki','f32'],['foc_f_zv','f32'],['foc_dt_us','f32'],
  ['foc_encoder_inverted','u8'],['foc_encoder_offset','f32'],['foc_encoder_ratio','f32'],['foc_sensor_mode','u8'],
  ['foc_pll_kp','f32'],['foc_pll_ki','f32'],['foc_motor_l','f32'],['foc_motor_ld_lq_diff','f32'],['foc_motor_r','f32'],['foc_motor_flux_linkage','f32'],
  ['foc_observer_gain','f32'],['foc_observer_gain_slow','f32'],['foc_observer_offset','f16',1000],
  ['foc_duty_dowmramp_kp','f32'],['foc_duty_dowmramp_ki','f32'],
  ['foc_start_curr_dec','f16',10000],['foc_start_curr_dec_rpm','f32'],
  ['foc_openloop_rpm','f32'],['foc_openloop_rpm_low','f16',1000],
  ['foc_d_gain_scale_start','f16',1000],['foc_d_gain_scale_max_mod','f16',1000],
  ['foc_sl_openloop_hyst','f16',100],['foc_sl_openloop_time_lock','f16',100],['foc_sl_openloop_time_ramp','f16',100],['foc_sl_openloop_time','f16',100],
  ['foc_sl_openloop_boost_q','f16',100],['foc_sl_openloop_max_q','f16',100],
  ['foc_hall_table','h8'],['foc_hall_interp_erpm','f32'],
  ['foc_sl_erpm_start','f32'],['foc_sl_erpm','f32'],
  ['foc_control_sample_mode','u8'],['foc_current_sample_mode','u8'],['foc_sat_comp_mode','u8'],['foc_sat_comp','f16',1000],
  ['foc_temp_comp','u8'],['foc_temp_comp_base_temp','f16',100],['foc_current_filter_const','f16',10000],
  ['foc_cc_decoupling','u8'],['foc_observer_type','u8'],
];
// decode a raw COMM_GET_MCCONF blob ([cmd][sig:4][serialized…]) → {param:value}.
// returns null if it doesn't look like an mcconf (sanity-checked on l_current_max).
function decodeMcconfBin(buf){
  const dv = new DataView(buf); if(dv.byteLength < 60) return null;
  let ind = 5;                          // skip cmd(1) + signature(4)
  const out = {};
  for(const f of MCCONF_SCHEMA){
    const [name,typ,scale]=f;
    if(ind > dv.byteLength-1) break;
    if(typ==='u8'){ out[name]=dv.getUint8(ind); ind+=1; }
    else if(typ==='f32'){ out[name]=dv.getFloat32(ind,false); ind+=4; }
    else if(typ==='f16'){ out[name]=dv.getInt16(ind,false)/scale; ind+=2; }
    else if(typ==='h8'){ ind+=8; }      // hall tables — skip
  }
  if(!(out.l_current_max>1 && out.l_current_max<1000)) return null;   // sanity / version check
  return out;
}
const FOC_OBSERVER_NAMES = ['Ortega original','MXLEMMING','MXLEMMING λ-comp','Ortega λ-comp'];

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
  if(ch.present){
    // data-aware advice: use the current FOC values + where the chatter sits
    const cv = k => (typeof paramCurrent==='function') ? paramCurrent(k).v : null;
    const obsT = cv('foc_observer_type'), slNow = cv('foc_sl_erpm'), kpNow = cv('foc_current_kp');
    const slTgt = slNow!=null ? Math.round((Math.max(ch.worstE, slNow)+1000)/100)*100 : null;
    const obsName = (typeof FOC_OBSERVER_NAMES!=='undefined' && obsT!=null) ? FOC_OBSERVER_NAMES[obsT] : null;
    const fixes=[];
    fixes.push({k:'foc_sl_erpm', action: slTgt?`raise ${slNow}→~${slTgt}`:'raise',
      why:`chatter now sits ~${ch.worstE} erpm — move the sensorless handoff past it`});
    fixes.push({k:'foc_current_kp', action: kpNow!=null?`lower ~15% (→${(kpNow*0.85).toFixed(3)})`:'lower ~15%',
      why:'reduces current-loop whine/oscillation'});
    fixes.push({k:'foc_observer_type', action: obsName?`keep ${obsName} — or try another`:'try another observer',
      why:'switching this is what cut your chatter; keep the best one you find'});
    fixes.push({k:'foc_motor_flux_linkage', action:'re-detect, verify it changed', why:'common cause if detection is off'});
    out.push({
      title:'Low-speed motor chatter ("crunching")', sev:'info', benign:true,
      evidence:`iq swings ±${ch.iqAmp}A at low ERPM (worst Δ${ch.peak}A @ ~${ch.worstE} erpm)`+(obsName?`, observer = ${obsName}`:'')+`, id≈0, net torque ~0 → observer hunting, not mechanical.`,
      fixes,
    });
  }

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
