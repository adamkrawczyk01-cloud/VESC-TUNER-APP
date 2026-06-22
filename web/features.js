/* ============================================================
   VESC Tuner — cross-cutting features: theme, smoothing & marker
   toolbar, PNG export, PWA registration.
   Loaded last. Depends on app.js globals (C, SMOOTH, ANNOT, THEME,
   CHARTS, render, VIEW, switchView, $, el, annotKey, saveAnnot).
   ============================================================ */

/* ---------- theme ---------- */
const PALETTE = {
  dark:  { text:'#f1f5f9', text2:'#94a3b8', muted:'#64748b', grid:'#1e293b', axis:'#334155' },
  light: { text:'#0f172a', text2:'#475569', muted:'#64748b', grid:'#e2e8f0', axis:'#cbd5e1' },
};
function applyTheme(t){
  THEME = t; localStorage.setItem('vesc_theme', t);
  document.documentElement.dataset.theme = t;
  Object.assign(C, PALETTE[t] || PALETTE.dark);
  const tc=document.querySelector('meta[name=theme-color]'); if(tc) tc.content = t==='light'?'#f1f5f9':'#0f172a';
  if (typeof render==='function') render(VIEW);
}

/* ---------- PNG export (stack all visible charts) ---------- */
function exportPNG(){
  const cv = CHARTS.map(u=>u.ctx && u.ctx.canvas).filter(Boolean);
  if(!cv.length){ alert('No charts to export on this view.'); return; }
  const pad=12, gap=10, w=Math.max(...cv.map(c=>c.width));
  const h=cv.reduce((s,c)=>s+c.height+gap,0)-gap;
  const out=document.createElement('canvas'); out.width=w+pad*2; out.height=h+pad*2;
  const ctx=out.getContext('2d');
  ctx.fillStyle = getComputedStyle(document.body).backgroundColor || '#0f172a';
  ctx.fillRect(0,0,out.width,out.height);
  let y=pad; for(const c of cv){ ctx.drawImage(c,pad,y); y+=c.height+gap; }
  out.toBlob(b=>{ const url=URL.createObjectURL(b); const a=el('a');
    a.href=url; a.download=(D?D.name:'session')+'_'+VIEW+'.png'; a.click();
    setTimeout(()=>URL.revokeObjectURL(url),1000); });
}

/* ---------- toolbar wiring ---------- */
function syncToolButtons(){
  const s=$('#t-smooth'); if(s) s.classList.toggle('on', SMOOTH.on);
  const d=$('#t-decim'); if(d) d.classList.toggle('on', DECIMATE.on);
}
(function wireToolbar(){
  const s=$('#t-smooth'), th=$('#t-theme'), pg=$('#t-png'), cl=$('#t-clr'), dc=$('#t-decim');
  if(s)  s.onclick =()=>{ SMOOTH.on=!SMOOTH.on; syncToolButtons(); if(typeof render==='function') render(VIEW); };
  if(dc) dc.onclick=()=>{ DECIMATE.on=!DECIMATE.on; syncToolButtons(); if(typeof render==='function') render(VIEW); };
  if(th) th.onclick=()=> applyTheme(THEME==='dark'?'light':'dark');
  if(pg) pg.onclick = exportPNG;
  if(cl) cl.onclick=()=>{ if(!ANNOT[annotKey()]||!ANNOT[annotKey()].length){ return; }
    if(confirm('Clear all markers for this session?')){ delete ANNOT[annotKey()]; saveAnnot(); CHARTS.forEach(c=>c.redraw(false,false)); } };
  applyTheme(THEME); syncToolButtons();
})();

/* ---------- PWA: service worker + install prompt ---------- */
if ('serviceWorker' in navigator) {
  window.addEventListener('load', ()=> navigator.serviceWorker.register('sw.js').catch(()=>{}));
}
let DEFERRED_INSTALL = null;
window.addEventListener('beforeinstallprompt', e => {
  e.preventDefault(); DEFERRED_INSTALL = e;
  const b=$('#t-install'); if(b) b.hidden = false;
});
window.addEventListener('appinstalled', ()=>{ const b=$('#t-install'); if(b) b.hidden = true; DEFERRED_INSTALL=null; });
(function wireInstall(){
  const b=$('#t-install'); if(!b) return;
  // already running as an installed app? keep it hidden
  if (window.matchMedia && window.matchMedia('(display-mode: standalone)').matches) b.hidden = true;
  b.onclick = async ()=>{ if(!DEFERRED_INSTALL) return;
    DEFERRED_INSTALL.prompt(); try{ await DEFERRED_INSTALL.userChoice; }catch(e){}
    DEFERRED_INSTALL=null; b.hidden=true; };
})();

/* ---------- mobile drawer + collapsible nav sections ---------- */
(function wireNav(){
  const mt=$('#menu-toggle');
  if(mt) mt.onclick=(e)=>{ e.stopPropagation(); document.body.classList.toggle('nav-open'); };
  // tap outside the drawer closes it
  document.addEventListener('click', e=>{ if(document.body.classList.contains('nav-open')
    && !e.target.closest('.sidebar') && !e.target.closest('#menu-toggle')) document.body.classList.remove('nav-open'); });
  // close the drawer after picking a view on mobile
  document.querySelectorAll('.navitem').forEach(b=> b.addEventListener('click', ()=>{
    if(window.matchMedia('(max-width:760px)').matches) document.body.classList.remove('nav-open');
  }));
  // collapse a section by clicking its label; hides .navitem siblings until next label
  let collapsed={}; try{ collapsed=JSON.parse(localStorage.getItem('vesc_nav_collapsed')||'{}'); }catch(e){}
  const apply=(lbl)=>{ const key=lbl.textContent.trim(); const on=!!collapsed[key];
    lbl.classList.toggle('collapsed', on);
    let el=lbl.nextElementSibling;
    while(el && !el.classList.contains('navlbl')){ if(el.classList.contains('navitem')) el.style.display=on?'none':''; el=el.nextElementSibling; } };
  document.querySelectorAll('#nav .navlbl').forEach(lbl=>{ lbl.style.cursor='pointer'; lbl.title='collapse / expand';
    apply(lbl);
    lbl.addEventListener('click', ()=>{ const key=lbl.textContent.trim(); collapsed[key]=!collapsed[key];
      try{ localStorage.setItem('vesc_nav_collapsed', JSON.stringify(collapsed)); }catch(e){} apply(lbl); });
  });
})();
