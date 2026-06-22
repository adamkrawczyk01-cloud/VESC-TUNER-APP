# TODO

## 🔜 Next: BLE proxy — Cardputer relay to phone (Float Control)

Goal: `Phone (Float Control) ↔BLE↔ Cardputer ↔BLE↔ VESC Express ↔CAN↔ motor`.
Cardputer runs **central** (to VESC, already does) **+ peripheral** (fake VESC, phone
connects to it). **Raw byte passthrough** (don't parse/modify), log via a tap copy.

**Constraint:** VESC Express accepts one BLE link → must relay or use 2 transports.
**Safety:** keep it read-only — relay only forwards bytes the phone sends; Cardputer
itself never originates a SET command.

### Phases
- [ ] **Phase 0 — feasibility spike (do FIRST).** Separate sketch: NimBLE dual-role
      (central holds VESC + peripheral advertises NUS `6e400001…`). Confirm phone can
      see+connect to Cardputer-as-VESC while VESC link is held. **Measure throughput.**
      If S3 bandwidth (BLE central+peripheral, maybe +WiFi) is too low → stop here.
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

## Backlog / deferred

- [ ] **After-ride verification:** new session after the FOC change (observer type /
      sl_erpm) → compare `detectChatter` before (session_004: peak Δ89A @ ~1208 erpm)
      vs after, to confirm the low-speed chatter dropped.
- [ ] **Full mcconf decode (path A):** fetch VESC bldc `confgenerator.c` (WebFetch was
      rate-limited) for fw-6.5 sig `0x2efd0142`; validate vs anchors (150/70/225) →
      decode temps/battery_cut/FOC so sessions auto-populate current values without the
      VESC Tool XML export. (Workaround in place: load VESC Tool XML in Tuning loop.)
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
