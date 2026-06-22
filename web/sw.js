/* VESC Tuner — minimal offline cache (app shell + CDN libs).
   Bump CACHE when assets change to force an update. */
const CACHE = 'vesc-tuner-v18';
const SHELL = [
  './', './index.html', './styles.css',
  './app.js', './views2.js', './views3.js', './pushback.js', './reference.js', './params.js', './features.js', './boards.js',
  './manifest.json', './sample_session.csv', './rides/manifest.json',
  './icon-192.png', './icon-512.png',
  'https://cdn.jsdelivr.net/npm/uplot@1.6.31/dist/uPlot.min.css',
  'https://cdn.jsdelivr.net/npm/uplot@1.6.31/dist/uPlot.iife.min.js',
  'https://cdn.jsdelivr.net/npm/papaparse@5.4.1/papaparse.min.js',
  'https://unpkg.com/leaflet@1.9.4/dist/leaflet.css',
  'https://unpkg.com/leaflet@1.9.4/dist/leaflet.js',
];

self.addEventListener('install', e => {
  self.skipWaiting();
  e.waitUntil(caches.open(CACHE).then(c => Promise.allSettled(SHELL.map(u => c.add(u)))));
});
self.addEventListener('activate', e => {
  e.waitUntil(caches.keys().then(ks => Promise.all(ks.filter(k => k !== CACHE).map(k => caches.delete(k)))));
  self.clients.claim();
});
self.addEventListener('fetch', e => {
  const req = e.request;
  if (req.method !== 'GET') return;
  // never cache device API calls (live telemetry / sessions) or map tiles (bloat)
  if (/\/api\/|\/sd\?|tile\.openstreetmap\.org/.test(req.url)) return;
  e.respondWith(
    caches.match(req).then(hit => hit || fetch(req).then(res => {
      const copy = res.clone();
      caches.open(CACHE).then(c => c.put(req, copy)).catch(()=>{});
      return res;
    }).catch(() => hit))
  );
});
