# TODO

## 🔜 Next: BLE proxy — Cardputer relay to phone (Float Control)

Goal: `Phone (Float Control) ↔BLE↔ Cardputer ↔BLE↔ VESC Express ↔CAN↔ motor`.
Cardputer runs **central** (to VESC, already does) **+ peripheral** (fake VESC, phone
connects to it). **Raw byte passthrough** (don't parse/modify), log via a tap copy.

**Constraint:** VESC Express accepts one BLE link → must relay or use 2 transports.
**Safety:** keep it read-only — relay only forwards bytes the phone sends; Cardputer
itself never originates a SET command.

### Phases
- [~] **Phase 0 — feasibility spike (IN PROGRESS).** Separate sketch
      `cardputer/ble_relay/` — NimBLE dual-role (central holds VESC + peripheral
      advertises NUS `6e400001…`) + minimal raw relay + throughput counter on screen.
      → user flashes, connects Float Control, reports: does it connect + show live? B/s?
- [ ] **Phase 1 — raw byte relay.** phone→(write RX)→VESC RX; VESC notify→(TX)→phone.
      Fragment per each side's MTU. Goal: Float Control actually connects + shows live.
- [ ] **Phase 2 — logging tap.** In RELAY mode Cardputer STOPS its own polling (don't
      double-load the link); parse a copy of VESC→phone responses (GET_VALUES/ALLDATA/
      BMS — Float Control polls them) → SD CSV as today.
- [ ] **Phase 3 — robustness.** connect/disconnect (phone+VESC), VESC auto-reconnect,
      re-advertise, [RELAY] mode toggle/screen + status. Consider WiFi off in relay.
- [ ] **Phase 4 — polish.** advertise name Float Control accepts, conn params, fallback
      to normal mode.

**Risks:** throughput (2 BLE hops + shared link); dual-role+WiFi coex on S3; Float
Control handshake quirks (FW_VERSION/MTU); latency. Mitigate: raw relay, no double-poll.

---

## 🪖 Concept: Helmet HUD (design only — see memory helmet-hud-concept)

Miniaturize the Cardputer rig into a helmet heads-up display.
- Architecture: board unit (BLE→VESC, SD black box) **broadcasts ESP-NOW** → helmet
  unit (tiny ESP32-S3 + small display + opt. GPS) renders a glanceable HUD. ESP-NOW
  avoids the VESC single-BLE-link contention.
- Helmet BOM: XIAO ESP32-S3 / StampS3 · ATGM336H GPS (115200) · 500–1000 mAh LiPo ·
  3D pod + boom. Display over SPI (like ST7789), NOT MIPI/HDMI microdisplays.
- Optics phased: T1 boom screen in periphery (glanceable) → T2 micro-OLED + 45°
  combiner on visor (distance-focused, bright for sunlight, transparent OLED ideal).
- Safety: no shell drilling, breakaway/soft mount, low mass near shell, periphery
  not central, minimal UI, collimated focus. Distraction + crash impact = real risks.
- Phases: P0 ESP-NOW link · P1 boom+UI · P2 power+mount · P3 combiner · P4 polish.

---

## Backlog / deferred

- [~] **After-ride verification:** session_008 (observer_type 2→3) vs session_004 →
      chatter **−32%** (peak 89→61A). Ongoing: next is foc_sl_erpm 2000→~3000.
- [x] **Full mcconf decode — DONE.** `decodeMcconfBin()` + `MCCONF_SCHEMA` in
      params.js (confgenerator_serialize_mcconf order, fw 6.06, mixed types). Validated
      (foc_observer_type @251). Auto-decode for shipped + paired `.bin` on local +CSV.
- [ ] **P3 branch (`p3`, not merged):** mobile drawer, chart decimation toggle,
      self-baseline from history, collapsible nav. Decide merge or drop.
- [ ] **Logger cleanup (audit #3):** drop unused CSV cols (foc_id_A, cell_max,
      tacho_abs) or wire them into views — make logged ≡ shown.
- [ ] **NTP over WiFi:** sync the clock from the internet when [I] connects, for exact
      file dates (now: build-date baseline, ~flash day).
- [ ] **Refloat customcfg decode:** `tiltback_duty` etc. — version-fragile (FPKP/FPPZ);
      revisit if we pin the Refloat version. (Now: duty limit from `device_profile.json`.)

## Done (recent, for context)
Read-only audit + hardening · duty-Geiger from SD profile · Q/W volume + build-date
clock · full mcconf+appconf capture (1024B bufs) · mcconf parser fix (currents) · BMS
SOC + charge chime/pings + duty Geiger · web: tuning advisor + VESC-Tool param catalog
+ VESC Tool XML import · session_004 added + analyzed · data contract audit.
