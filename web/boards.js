/* ============================================================
   BOARDS — sidebar board library. Group rides by board and load
   them into the dashboard with one click.
   Two ride sources merge per board:
     • shipped  — rides/manifest.json (committed to the repo, shared
                  with collaborators, fetched on demand);
     • local    — CSVs the user adds in the browser, stored in the
                  same IndexedDB 'sessions' store (tagged .board),
                  reusing idbOpen/idbPut/idbAll/idbDel from views3.js.
   Loaded last; depends on app.js globals (el,$,importCSV,switchView,D)
   and views3.js IDB helpers + views2.js btn().
   ============================================================ */
let BOARDS_MANIFEST = [];                 // [{id,name,note,rides:[{name,file,src,date}]}]
let BOARDS_EXTRA = boardsLoadExtra();     // user-created board names (no shipped rides)
let BOARDS_LOADED = null;                 // name of the ride currently shown (for highlight)

function boardsLoadExtra(){ try { return JSON.parse(localStorage.getItem('vesc_boards')||'[]'); } catch(e){ return []; } }
function boardsSaveExtra(){ try { localStorage.setItem('vesc_boards', JSON.stringify(BOARDS_EXTRA)); } catch(e){} }

/* merge shipped + user boards into one ordered list of {id,name,note} */
function boardsAll(){
  const map = new Map();
  BOARDS_MANIFEST.forEach(b => map.set(b.id, { id:b.id, name:b.name||b.id, note:b.note||'' }));
  BOARDS_EXTRA.forEach(n => { if(!map.has(n)) map.set(n, { id:n, name:n, note:'' }); });
  return [...map.values()];
}
function shippedRides(boardId){
  const b = BOARDS_MANIFEST.find(b => b.id===boardId);
  return b && b.rides ? b.rides : [];
}

/* ---------- render ---------- */
async function renderBoards(){
  const host = $('#boards'); if(!host) return;
  host.innerHTML = '';
  const head = el('div','boards-head');
  head.innerHTML = '<span class="navlbl" style="padding-left:4px">BOARDS</span>';
  const add = el('button','board-add'); add.textContent='＋'; add.title='Add a board';
  add.onclick = addBoard;
  head.append(add); host.append(head);

  let local = [];
  try { local = (await idbAll()).filter(r => r.board); } catch(e){}

  for(const b of boardsAll()){
    const card = el('div','board');
    const bh = el('div','board-name');
    bh.innerHTML = `<span class="bdot"></span><b>${b.name}</b>` + (b.note?`<span class="bnote">${b.note}</span>`:'');
    const plus = el('button','board-csv'); plus.textContent='＋ CSV'; plus.title='Add a CSV (select its mcconf.bin too for full config) to this board';
    plus.onclick = ()=> addCsvToBoard(b.id);
    bh.append(plus); card.append(bh);

    const list = el('div','board-rides');
    const shipped = shippedRides(b.id);
    const mine = local.filter(r => r.board===b.id).sort((a,b)=>(b.savedAt||0)-(a.savedAt||0));
    if(!shipped.length && !mine.length){
      const e = el('div','board-empty'); e.textContent='no rides yet — ＋ CSV'; list.append(e);
    }
    shipped.forEach(r => { const row=rideRow({ name:r.name, sub:(r.grade?r.grade+' · ':'')+(r.date||'shipped'), key:'ship:'+b.id+':'+r.file },
      ()=> loadShipped(r), null);
      if(r.assessment){ const a=el('a','ride-note'); a.textContent='📋'; a.href='rides/'+r.assessment;
        a.target='_blank'; a.title='Ride assessment'; a.onclick=e=>e.stopPropagation(); row.append(a); }
      list.append(row); });
    mine.forEach(r => list.append(rideRow({ name:r.name, sub:fmtRideMeta(r), key:'idb:'+r.name },
      ()=> { BOARDS_LOADED='idb:'+r.name; if(r.cfg) CFG.mcconf=r.cfg; loadCSV(r.csv, r.name); switchView('overview'); renderBoards(); },
      async()=> { if(confirm('Remove '+r.name+' from '+b.id+'?')){ await idbDel(r.name); renderBoards(); } })));
    card.append(list); host.append(card);
  }
}

function fmtRideMeta(r){
  const m = r.metrics||{};
  const bits = [];
  if(m.dur>0 && m.dur<1440)  bits.push(m.dur.toFixed(0)+'m');     // hide absurd stored metrics
  if(m.dist>0 && m.dist<1000) bits.push(m.dist.toFixed(1)+'km');  // (e.g. odometer-glitch 1.2M km)
  return bits.length ? bits.join(' · ') : 'local';
}

function rideRow(info, onLoad, onDel){
  const row = el('button','ride'+(BOARDS_LOADED===info.key?' active':''));
  row.innerHTML = `<span class="rn">${info.name}</span><span class="rsub">${info.sub}</span>`;
  row.onclick = onLoad;
  if(onDel){ const x=el('span','rx'); x.textContent='✕'; x.title='Remove';
    x.onclick=(e)=>{ e.stopPropagation(); onDel(); }; row.append(x); }
  return row;
}

/* ---------- actions ---------- */
async function loadShipped(r){
  const key = 'rides/'+r.file;
  try{
    const res = await fetch(key); if(!res.ok) throw new Error(res.status);
    const text = await res.text();
    BOARDS_LOADED = 'ship:loaded:'+r.file;
    importCSV(text, r.name, r.src||null, { date:r.date, time:r.time });
    // decode this session's raw mcconf.bin → real config values (Norms / Tuning)
    if(r.mcconf && typeof decodeMcconfBin==='function'){
      try{ const b=await fetch('rides/'+r.mcconf); if(b.ok){ const cfg=decodeMcconfBin(await b.arrayBuffer()); if(cfg) CFG.mcconf=cfg; } }catch(e){}
    }
    switchView('overview');
    renderBoards();
  }catch(e){ alert('Could not load '+r.name+' ('+e.message+').\nIs rides/'+r.file+' deployed?'); }
}

function addBoard(){
  const name = (prompt('Board name (e.g. GAD, Pint, Float):')||'').trim();
  if(!name) return;
  if(boardsAll().some(b=>b.id===name)){ alert('Board "'+name+'" already exists.'); return; }
  BOARDS_EXTRA.push(name); boardsSaveExtra(); renderBoards();
}

let BOARD_TARGET = null;   // board id awaiting a CSV from the picker
function addCsvToBoard(boardId){
  BOARD_TARGET = boardId;
  const inp = $('#board-file'); inp.value=''; inp.click();
}
function wireBoardFile(){
  const inp = $('#board-file'); if(!inp) return;
  inp.onchange = async e => {
    const files = [...e.target.files]; const board = BOARD_TARGET; BOARD_TARGET=null;
    e.target.value='';
    if(!files.length || !board) return;
    const csvF = files.find(f=>/\.(csv|txt)$/i.test(f.name)) || files[0];
    const binF = files.find(f=>/\.bin$/i.test(f.name));   // optional paired mcconf.bin
    const readText = f => new Promise(res=>{ const r=new FileReader(); r.onload=()=>res(r.result); r.readAsText(f); });
    const readBin  = f => new Promise(res=>{ const r=new FileReader(); r.onload=()=>res(r.result); r.readAsArrayBuffer(f); });
    const text = await readText(csvF);
    let cfg = null;
    if(binF && typeof decodeMcconfBin==='function'){ try{ cfg = decodeMcconfBin(await readBin(binF)); }catch(err){} }
    importCSV(text, csvF.name, null);
    if(cfg) CFG.mcconf = cfg;            // real config from the paired mcconf.bin
    if(D && D.csvText){
      try{
        const metrics = (typeof sessionMetrics==='function') ? sessionMetrics(D) : {};
        await idbPut({ name:D.name, csv:text, board, savedAt:Date.now(), metrics, cfg });
        BOARDS_LOADED = 'idb:'+D.name;
      }catch(err){ alert('Loaded, but could not save to this browser: '+err.message); }
    }
    switchView('overview');
    renderBoards();
  };
}

/* ---------- boot ---------- */
(async function initBoards(){
  try{
    const res = await fetch('rides/manifest.json');
    if(res.ok){ const j = await res.json(); BOARDS_MANIFEST = j.boards||[]; }
  }catch(e){ BOARDS_MANIFEST = []; }
  wireBoardFile();
  renderBoards();
})();
