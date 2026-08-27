/* ============================================================
   JSONL — Debug Recording loader (raw BLE chunk replay)

   Reads a recording of every raw BLE chunk a session ever saw and rebuilds a
   dataset from it, so a ride can be re-analysed as many times as you like
   without going out and riding it again. That matters here specifically: the
   abs-overcurrent hunt on the GAD board keeps needing "one more ride" to test
   the next idea, and a recording removes that dependency entirely.

   The file format is VESCape's Debug Recording (their ADR-0024). VESCape is
   GPL-3.0 and this repo is not, so nothing was copied — this is our own decoder
   for a documented line format, which makes recordings interchangeable in both
   directions. Our Cardputer writes the same shape (session_NNN_raw.jsonl).

     {"t":0,"kind":"meta","version":1,"deviceName":"ADV2","pollIntervalMs":40,..}
     {"t":1201,"kind":"ble-chunk","direction":"tx","base64":"AgYiYiRlCgKhugM="}
     {"t":1237,"kind":"ble-chunk","direction":"rx","base64":".."}
     {"t":725,"kind":"location","latitude":51.13,"longitude":16.99,"speedMps":1.09}
     {"t":91000,"kind":"marker","type":"auto_pause"}

   Only rx chunks carry telemetry; tx is kept because it shows what was asked
   for and when, which is how you tell a slow board from a slow poller.

   Loaded after app.js. Produces the same {col,t,n} dataset shape as parseCSV,
   so every existing view works on a replay unchanged.
   ============================================================ */

/* ---------- VESC packet framing ----------
   0x02 <len8> payload crc16 0x03      (short, len < 256)
   0x03 <len16> payload crc16 0x03     (long)
   Same framing the firmware builds in buildPkt() / unpackPkt().            */
const JL_CRC16_TAB = (() => {
  const t = new Uint16Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i << 8;
    for (let j = 0; j < 8; j++) c = (c & 0x8000) ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
    t[i] = c;
  }
  return t;
})();
function jlCrc16(buf, from, len) {
  let c = 0;
  for (let i = 0; i < len; i++) c = ((JL_CRC16_TAB[((c >> 8) ^ buf[from + i]) & 0xFF] ^ (c << 8)) & 0xFFFF);
  return c;
}

function jlB64(s) {
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

/* Stream reassembler. BLE hands over arbitrary fragments, so frames straddle
   chunk boundaries — exactly as on the board. Feeding it the recorded chunks
   exercises the same framing path a live link does. */
class JLReassembler {
  constructor(onFrame) { this.buf = new Uint8Array(4096); this.len = 0; this.onFrame = onFrame; }
  push(chunk) {
    if (this.len + chunk.length > this.buf.length) this.len = 0;   // overflow guard
    this.buf.set(chunk, this.len); this.len += chunk.length;
    this.pump();
  }
  pump() {
    for (;;) {
      if (this.len < 2) return;
      const s = this.buf[0];
      let hdr, plen;
      if (s === 2)      { hdr = 2; plen = this.buf[1]; }
      else if (s === 3) { hdr = 3; if (this.len < 3) return; plen = (this.buf[1] << 8) | this.buf[2]; }
      else { this.buf.copyWithin(0, 1, this.len); this.len--; continue; }   // resync
      const total = hdr + plen + 3;
      if (plen === 0 || plen > 1024) { this.buf.copyWithin(0, 1, this.len); this.len--; continue; }
      if (this.len < total) return;                       // wait for more chunks
      if (this.buf[total - 1] !== 3) { this.buf.copyWithin(0, 1, this.len); this.len--; continue; }
      const got = (this.buf[hdr + plen] << 8) | this.buf[hdr + plen + 1];
      if (got === jlCrc16(this.buf, hdr, plen)) this.onFrame(this.buf.slice(hdr, hdr + plen));
      this.buf.copyWithin(0, total, this.len); this.len -= total;
    }
  }
}

/* ---------- payload readers (big-endian, as VESC sends) ---------- */
const jlI16 = (b, o) => { const v = (b[o] << 8) | b[o + 1]; return v > 32767 ? v - 65536 : v; };
const jlI32 = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) | 0;

const JL_CMD_GET_VALUES = 4, JL_CMD_CUSTOM_APP_DATA = 36, JL_CMD_FORWARD_CAN = 34;
const JL_REFLOAT_MAGIC = 101, JL_REFLOAT_ALLDATA = 10;

/* Offsets mirror the firmware's parseValues() — keep the two in step. */
function jlParseValues(p, S) {
  if (p.length < 50) return false;
  S.temp_fet_C = jlI16(p, 1) / 10;
  S.temp_mot_C = jlI16(p, 3) / 10;
  S.curr_mot_A = jlI32(p, 5) / 100;
  S.curr_in_A  = jlI32(p, 9) / 100;
  S.id_A       = jlI32(p, 13) / 100;
  S.iq_A       = jlI32(p, 17) / 100;
  S.duty_pct   = jlI16(p, 21) / 10;
  S.rpm        = jlI32(p, 23);
  S.voltage_V  = jlI16(p, 27) / 10;
  S.amp_hours  = jlI32(p, 29) / 10000;
  if (p.length >= 41) S.watt_hours = jlI32(p, 37) / 10000;
  S.tacho = jlI32(p, 45);
  if (p.length >= 54) S.fault = p[53];
  return true;
}

/* Refloat ALLDATA — the layout is cmd_send_all_data() in refloat/src/main.c.
   Offsets below are that function's buffer index + 1, because the reply reaches
   us wrapped as COMM_CUSTOM_APP_DATA (p[0]=36, p[1]=101, p[2]=10, p[3]=mode).

   Worth knowing: this one reply already carries voltage, erpm, speed, motor and
   battery current and duty. Mode 1 (35 B) has all of that plus foc_id; mode 2
   (42 B) only adds absolute distance and the two temperatures. VESCape polls
   nothing else — their whole recording is 11.5k ALLDATA frames and zero
   GET_VALUES, which is why a GET_VALUES-only reader saw an empty file. */
function jlParseAllData(p, S) {
  if (p.length < 35 || p[1] !== JL_REFLOAT_MAGIC || p[2] !== JL_REFLOAT_ALLDATA) return false;
  if (p[3] === 69) { S.fault = p[4]; return false; }    // fault marker, fields zeroed
  S.alldata_mode   = p[3];
  S.req_amps_A     = jlI16(p, 4) / 10;
  S.balance_pitch  = jlI16(p, 6) / 10;
  S.roll_deg       = jlI16(p, 8) / 10;
  S.state          = p[10] & 0x0F;
  S.adc1           = p[12] / 50;
  S.adc2           = p[13] / 50;
  S.setpoint_deg   = (p[14] - 128) / 5;
  S.atr_deg        = (p[15] - 128) / 5;
  S.braketilt_deg  = (p[16] - 128) / 5;
  S.torquetilt_deg = (p[17] - 128) / 5;
  S.turntilt_deg   = (p[18] - 128) / 5;
  S.remotetilt_deg = (p[19] - 128) / 5;
  S.pitch_deg      = jlI16(p, 20) / 10;
  S.booster_A      = p[22] - 128;
  // motor block
  S.voltage_V      = jlI16(p, 23) / 10;
  S.rpm            = jlI16(p, 25);
  S.speed_kmh      = jlI16(p, 27) / 10 * 3.6;     // sent as m/s
  S.curr_mot_A     = jlI16(p, 29) / 10;
  S.curr_in_A      = jlI16(p, 31) / 10;
  S.duty_pct       = p[33] - 128;
  const f = p[34]; S.foc_id_A = (f === 222) ? null : f / 3;
  // mode >= 2 tail: float32_auto distance at p[35..38], then the temperatures.
  // Alignment is confirmed by p[41], which Refloat hardcodes to 0.
  // ⚠️ Order here follows cmd_send_all_data() (mosfet first, then motor), but in
  // VESCape's Thor301 fixture the mosfet channel is pinned at one value while the
  // motor channel moves 33-45 C — which is the opposite of what is usually
  // missing on a onewheel. Cross-check against GET_VALUES temps in one of our own
  // recordings, where both feeds exist in the same file, before trusting either.
  if (p.length >= 42) {
    S.temp_fet_C = p[39] / 2;
    S.temp_mot_C = p[40] / 2;
  }
  return true;
}

/* Strip the CAN-forward wrapper so the parsers see a plain reply. */
function jlUnwrap(p) {
  return (p.length > 2 && p[0] === JL_CMD_FORWARD_CAN) ? p.subarray(2) : p;
}

function jlIsRecording(text) {
  const nl = text.indexOf('\n');
  const first = (nl < 0 ? text : text.slice(0, nl)).trim();
  if (!first.startsWith('{')) return false;
  try { return JSON.parse(first).kind === 'meta'; } catch { return false; }
}

/* ---------- main entry ---------- */
function parseJSONL(text, name) {
  const lines = text.split('\n');
  const meta = {};
  const re = new JLReassembler(f => onFrame(f));

  // Live sample, carried forward between frames. A row is emitted per rx frame
  // that updates GET_VALUES, so row cadence matches the board's reply rate.
  const S = { alldata_mode: 0 };
  const rows = [];
  let curT = 0, sawValues = false;
  const markers = [];
  let gps = null;

  // A row is emitted per telemetry reply, whichever kind the recorder used.
  // Our Cardputer polls GET_VALUES at 25 Hz and ALLDATA alongside it; VESCape
  // polls ALLDATA only. Emitting on either means both files land in the same
  // shape and the row cadence always matches whatever that session actually did.
  function emit() {
    const r = Object.assign({}, S);
    r.ts_ms = curT;
    if (gps) { r.gps_lat = gps.lat; r.gps_lon = gps.lon; r.gps_spd_kmh = gps.spd; r.altitude_m = gps.alt; }
    rows.push(r);
  }
  function onFrame(payload) {
    const p = jlUnwrap(payload);
    if (p[0] === JL_CMD_GET_VALUES) {
      if (jlParseValues(p, S)) { sawValues = true; emit(); }
    } else if (p[0] === JL_CMD_CUSTOM_APP_DATA) {
      // Only emit from ALLDATA when it is the primary feed, otherwise a session
      // polling both would get two rows per cycle with half-updated fields.
      if (jlParseAllData(p, S) && !sawValues) emit();
    }
  }

  for (const raw of lines) {
    const s = raw.trim(); if (!s) continue;
    let j; try { j = JSON.parse(s); } catch { continue; }
    curT = j.t || 0;
    switch (j.kind) {
      case 'meta':     Object.assign(meta, j); break;
      case 'ble-chunk':
        // tx is recorded too, but only rx carries replies worth decoding
        if (j.direction === 'rx' && j.base64) re.push(jlB64(j.base64));
        break;
      case 'location':
        gps = { lat: j.latitude, lon: j.longitude,
                spd: (j.speedMps != null ? j.speedMps * 3.6 : null), alt: j.altitudeM };
        break;
      case 'marker':
      case 'session-state':
        markers.push({ t: j.t, type: j.type || j.status });
        break;
    }
  }

  if (!rows.length) return null;

  // GET_VALUES does not carry speed, so derive it the same way the firmware does:
  // rpm / pole_pairs * circumference. ALLDATA sends speed directly, so rows that
  // already have it are left alone.
  const poles = (meta.polePairs || 15), wheelMm = (meta.wheelMm || 234);
  const circKm = Math.PI * wheelMm / 1e6;
  for (const r of rows)
    if (r.speed_kmh == null && r.rpm != null) r.speed_kmh = Math.abs(r.rpm) / poles * circKm * 60;

  // Column-oriented, exactly the shape H(d) expects.
  const keys = new Set(); for (const r of rows) for (const k in r) keys.add(k);
  keys.delete('ts_ms');
  const col = {}; for (const k of keys) col[k] = new Array(rows.length).fill(null);
  const t = new Array(rows.length);
  rows.forEach((r, i) => {
    t[i] = r.ts_ms / 1000;
    for (const k of keys) if (r[k] != null) col[k][i] = r[k];
  });

  return {
    name: name || meta.deviceName || 'replay',
    col, t, n: rows.length,
    markers, meta,
    source: 'jsonl',
    boardName: meta.deviceName || null
  };
}
