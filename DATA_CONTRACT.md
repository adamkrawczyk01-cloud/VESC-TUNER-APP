# Data contract — Cardputer CSV ⇄ Web app

Audit of what the **Cardputer logs** vs what the **web dashboard reads**, so there
are no surprises about "different data in web vs on the board".

**Key rule:** the Cardputer CSV header uses the web's **native internal names**
(`parseCSV` reads the header 1:1, `source:'cardputer'`). So a Cardputer session
maps automatically — **no naming mismatch**. Float Control imports go through
`FC_MAP` (different column names). The gaps below are about *coverage*, not names.

## ✅ Both log & web shows (full match)
`speed_kmh, rpm, voltage_V, vcell_V, curr_in_A, curr_mot_A, power_W, duty_pct,
temp_fet_C, temp_mot_C, temp_bat_C, pitch_deg, roll_deg, setpoint_deg, atr_deg,
torquetilt_deg, turntilt_deg, braketilt_deg, remotetilt_deg, adc1, adc2,
req_amps_A, watt_hours, wh_charged, fault, batt_pct, odo_km, batt_wh, ts_ms`

## 🟦 Cardputer-only (web feature works ONLY for Cardputer sessions)
FC imports lack these, so for FC the matching view is empty / approximated:
- **Per-cell BMS:** `cell_01..cell_20`, `cell_min`, `cell_max`, `cell_delta_mV`,
  `vcell_V` → Battery view per-cell grid + imbalance. *(FC has no per-cell.)*
- **FOC currents:** `id_A`, `iq_A` → Motor/FOC d–q plane. *(FC has none.)*
- **Raw 6-axis IMU:** `acc_x/y/z`, `gyro_x/y/z`, `yaw` → Balance/IMU accel &
  gyro charts + hard-landing detection. *(FC has only pitch/roll → web derives
  `pitch_rate`/`roll_rate`/`ang_rate` as a proxy.)*

## 🟨 Float-Control-only (web shows, Cardputer CANNOT provide)
Hardware/firmware limits — Cardputer has no GPS module, no true-pitch:
- `gps_lat`, `gps_lon`, `gps_acc`, `altitude_m` → Map/GPS view (empty for Cardputer)
- `true_pitch_deg` → IMU "true pitch" line
- `fldweak_A` (I-FldWeak), `heartrate`, `alert_txt` (Alert) → FC `Alert` drives
  the alert markers/strip & the Alerts view. **Cardputer logs no text alerts**, so
  the Alerts/marker layer is FC-only (Pushback reconstruction still works from
  duty/V/temp for Cardputer).

## ⚙️ Web-derived (computed in browser, not logged by anyone)
`pitch_rate, roll_rate, ang_rate` (from angles when raw gyro absent — i.e. FC),
`cur_sat_pct` (Pushback). `power_W` is logged by Cardputer but recomputed (V·I)
for FC.

## ⚠️ Cardputer logs it, but web IGNORES it (wasted / potential surprise)
Logged to CSV every sample, never displayed anywhere in the web app:
- **BMS:** `bms_v_tot`, `bms_i_in`, `bms_temp_01..bms_temp_06`, `cell_max`
- **Energy:** `amp_hours`, `ah_charged`
- **Other:** `booster_A`, `foc_id_A` (web uses `id_A` instead), `tacho`,
  `tacho_abs`, `state`

→ e.g. you log **6 BMS pack temperatures** but no view shows them; same for
amp-hours and the booster current.

## 🐞 Known quality note
`batt_pct` is the VESC's `battery_level`, which is **unreliable** (wrong cell
count → saturates at 100%). The Cardputer HUD now ignores it and computes SOC
from voltage, but the **web Battery view still charts the raw `batt_pct`** — so
it can show a wrong %. Fix = recompute SOC from `voltage_V` in the web too.

## Suggested follow-ups (none applied — audit only)
1. Web Battery: add BMS-temp chart (`bms_temp_01..06`) + amp-hours — surface the
   orphan Cardputer data.
2. Web Battery %: derive from voltage like the HUD, don't trust `batt_pct`.
3. Drop unused columns from the logger (`foc_id_A`, `cell_max`, `tacho_abs`) or
   wire them into views — pick one so logged ≡ shown.
