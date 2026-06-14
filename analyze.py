#!/usr/bin/env python3
"""
VESC AI Tuner — Analizator sesji jazdy v2.0
Warstwa 1: Weryfikacja configu  |  Warstwa 2: Analiza CSV
Warstwa 3: Sugestie z safety_impact  |  Warstwa 4: Raport bezpieczenstwa

Uzycie: python3 analyze.py <nazwa_sesji>
"""

from __future__ import annotations

import sys
import json
import csv
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Set

try:
    from rich.console import Console
    from rich.table import Table
    from rich.panel import Panel
    from rich import box as rich_box
    HAS_RICH = True
except ImportError:
    HAS_RICH = False

BASE_DIR     = Path(__file__).parent
SESSIONS_DIR = BASE_DIR / "data" / "sessions"
PEV_REF_DIR  = BASE_DIR / "data" / "pev-reference"
CONFIG_DIR   = BASE_DIR / "config"
PROFILE_PATH = BASE_DIR / "device_profile.json"   # written by Cardputer to SD root

WHITELIST = [
    "l_current_max",
    "l_in_current_max",
    "l_max_erpm",
    "l_temp_fet_start",
    "l_temp_fet_end",
]

FOC_SENSOR_MODES = {0: "SENSORLESS", 1: "ENCODER", 2: "HALL", 3: "HFI"}

# ─────────────────────────────────────────────────────────────────────────────
# I/O
# ─────────────────────────────────────────────────────────────────────────────

def load_json(path: Path) -> dict:
    with open(path, encoding="utf-8") as f:
        return json.load(f)

def load_device_profile() -> dict:
    """Load device_profile.json from SD root (next to analyze.py) or return empty dict."""
    if PROFILE_PATH.exists():
        try:
            p = load_json(PROFILE_PATH)
            return p
        except Exception:
            pass
    return {}

def profile_to_limits(profile: dict) -> dict:
    """Build a voltage limits dict from device profile (replaces 20S hardcodes)."""
    cells     = int(profile.get("batt_cells", 20))
    batt_max  = float(profile.get("batt_max_v", cells * 4.2))
    batt_min  = float(profile.get("batt_min_v", cells * 3.0))
    return {"cells": cells, "batt_max_v": batt_max, "batt_min_v": batt_min}

def load_csv(path: Path) -> List[Dict]:
    rows = []
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            try:
                rows.append({k: float(v) for k, v in row.items()})
            except ValueError:
                pass
    return rows

# ─────────────────────────────────────────────────────────────────────────────
# Sandbox
# ─────────────────────────────────────────────────────────────────────────────

def sandbox(param: str, suggested: float, current: float,
            safety_limits: dict) -> Optional[float]:
    """Clamp suggested to safe_range and ±max_change_pct%. None if param unknown."""
    sb = safety_limits.get("sandbox_limits", {}).get(param)
    if sb is None:
        return None
    lo, hi    = sb["min"], sb["max"]
    max_pct   = sb.get("max_change_pct", 15) / 100.0
    val       = max(current * (1 - max_pct), min(current * (1 + max_pct), suggested))
    val       = max(lo, min(hi, val))
    if param == "l_max_erpm":
        val = round(val / 100) * 100
    else:
        val = round(val * 2) / 2      # nearest 0.5
    return val

# ─────────────────────────────────────────────────────────────────────────────
# WARSTWA 1 — Weryfikacja configu
# ─────────────────────────────────────────────────────────────────────────────

def verify_config(mcconf: dict,
                  session_stats: Optional[dict] = None) -> List[dict]:
    """
    Zwraca liste alertow konfiguracyjnych.
    Kazdy alert: {level, param, message, value, expected}
    session_stats: jesli podany — cross-check config vs dane z jazdy.
    """
    alerts: List[dict] = []

    def alert(level: str, param: str, message: str,
              value=None, expected: Optional[str] = None) -> None:
        alerts.append({"level": level, "param": param,
                        "message": message, "value": value, "expected": expected})

    cut_start = mcconf.get("l_battery_cut_start")
    cut_end   = mcconf.get("l_battery_cut_end")
    fet_start = mcconf.get("l_temp_fet_start")
    fet_end   = mcconf.get("l_temp_fet_end")
    mot_start = mcconf.get("l_temp_motor_start") or mcconf.get("l_temp_mot_start")
    mot_end   = mcconf.get("l_temp_motor_end")   or mcconf.get("l_temp_mot_end")
    l_in_max  = mcconf.get("l_in_current_max")
    l_erpm    = mcconf.get("l_max_erpm")
    sensor    = mcconf.get("foc_sensor_mode")

    # ── Odciecia bateryjne ───────────────────────────────────────────────────
    if cut_start is not None and cut_start < 46.0:
        alert("critical", "l_battery_cut_start",
              "Prog odciecia baterii PONIZEJ minimum 20S — ryzyko glebokiego rozladowania ogniw",
              value="{}V".format(cut_start), expected=">=46.0V")

    if cut_end is not None and cut_end < 40.0:
        alert("critical", "l_battery_cut_end",
              "Twardy cutoff ponizej minimum 20S — ryzyko uszkodzenia ogniw Li-Ion",
              value="{}V".format(cut_end), expected=">=40.0V")

    if (cut_start is not None and cut_end is not None
            and cut_end >= cut_start):
        alert("critical", "l_battery_cut_end/start",
              "BLAD KONFIGURACJI: cut_end >= cut_start — VESC nie zadba o baterie",
              value="{}/{}V".format(cut_end, cut_start),
              expected="cut_end < cut_start")

    # ── Limit ERPM ───────────────────────────────────────────────────────────
    if not l_erpm:
        alert("critical", "l_max_erpm",
              "Brak limitu ERPM — jazda bez ograniczenia predkosci = NIEBEZPIECZNE",
              value=str(l_erpm), expected=">0")

    # ── Temperatury FET ──────────────────────────────────────────────────────
    if fet_start is not None and fet_start > 75.0:
        alert("critical", "l_temp_fet_start",
              "Prog startu dławienia FET za wysoki — ryzyko uszkodzenia tranzystorow MOSFET",
              value="{}C".format(fet_start), expected="<=75C")

    if fet_end is not None and fet_end > 85.0:
        alert("critical", "l_temp_fet_end",
              "Prog odciecia FET za wysoki — powyzej 85C wzrasta ryzyko uszkodzenia VESC",
              value="{}C".format(fet_end), expected="<=85C")

    if (fet_start is not None and fet_end is not None
            and fet_end <= fet_start):
        alert("critical", "l_temp_fet_end/start",
              "BLAD KONFIGURACJI: l_temp_fet_end <= l_temp_fet_start — thermal cutoff nie zadziala",
              value="{}/{}C".format(fet_end, fet_start),
              expected="fet_end > fet_start")

    # ── Temperatury silnika ──────────────────────────────────────────────────
    if mot_start is not None and mot_start > 85.0:
        alert("critical", "l_temp_motor_start",
              "Prog dławienia silnika za wysoki — powyzej 82C magnesy trac sile",
              value="{}C".format(mot_start), expected="<=85C")

    if mot_end is not None and mot_end > 95.0:
        alert("critical", "l_temp_motor_end",
              "Prog odciecia silnika za wysoki — powyzej 100C ryzyko demagnetyzacji stalej",
              value="{}C".format(mot_end), expected="<=95C")

    # ── Prad bateryjny ───────────────────────────────────────────────────────
    if l_in_max is not None and l_in_max > 60.0:
        alert("critical", "l_in_current_max",
              "Prad bateryjny przekracza limit bezpieczny dla 2P — ryzyko BMS cutoff / uszkodzenia ogniw",
              value="{}A".format(l_in_max), expected="<=60A (2P pack)")

    # ── Tryb sensorow ────────────────────────────────────────────────────────
    if sensor is not None and sensor == 0:
        alert("warning", "foc_sensor_mode",
              "Tryb SENSORLESS — mozliwy bucking/szarpanie przy ruszaniu i niskich predkosciach",
              value="SENSORLESS (0)", expected="HALL (2) lub ENCODER (1)")

    # ── Cross-check: config vs dane sesji ────────────────────────────────────
    if session_stats:
        max_fet  = session_stats.get("max_temp_fet_C", 0.0)
        min_volt = session_stats.get("min_voltage_V", 999.0)

        if fet_start is not None and max_fet > fet_start:
            alert("critical", "l_temp_fet_start",
                  "FET PRZEKROCZYL prog startu dławienia — VESC throttlował prad silnikowy "
                  "(max {:.1f}C > prog {}C = utrata mocy podczas jazdy)".format(
                      max_fet, fet_start),
                  value="{:.1f}C osiagniete".format(max_fet),
                  expected="max_temp_fet < {}C".format(fet_start))

        if cut_start is not None and min_volt < cut_start:
            alert("critical", "l_battery_cut_start",
                  "NAPIECIE SPALO SIE PONIZEJ progu odciecia w tej sesji "
                  "({:.1f}V < {}V) — bateria rozladowana za gleboko".format(
                      min_volt, cut_start),
                  value="{:.1f}V min".format(min_volt),
                  expected=">{}V".format(cut_start))

    return alerts

# ─────────────────────────────────────────────────────────────────────────────
# WARSTWA 2 — Statystyki i analiza CSV
# ─────────────────────────────────────────────────────────────────────────────

def compute_stats(rows: List[Dict], mcconf: Optional[dict],
                  safety_limits: dict) -> dict:
    if not rows:
        return {}

    speeds    = [r["speed_kmh"]  for r in rows]
    curr_in   = [r["curr_in_A"]  for r in rows]
    curr_mot  = [r["curr_mot_A"] for r in rows]
    duties    = [r["duty_pct"]   for r in rows]
    temp_fet  = [r["temp_fet_C"] for r in rows]
    temp_mot  = [r["temp_mot_C"] for r in rows]
    voltages  = [r["voltage_V"]  for r in rows]
    rpms      = [r["rpm"]        for r in rows]
    amp_hours = [r["amp_hours"]  for r in rows]
    n         = len(rows)

    duration_s      = (rows[-1]["ts_ms"] - rows[0]["ts_ms"]) / 1000.0
    l_in_max        = (mcconf or {}).get("l_in_current_max", 30.0)
    fet_start_thresh = (mcconf or {}).get("l_temp_fet_start", 70.0)
    spike_threshold  = l_in_max * 0.80
    dt_ms            = (rows[-1]["ts_ms"] - rows[0]["ts_ms"]) / max(n - 1, 1)

    # ── Current spikes ───────────────────────────────────────────────────────
    spike_events = [
        {"ts_ms": rows[i]["ts_ms"], "curr_in_A": curr_in[i]}
        for i in range(n)
        if curr_in[i] > spike_threshold
    ]

    # ── Duty cycle ───────────────────────────────────────────────────────────
    duty_buckets: Dict[str, int] = {
        "<50": 0, "50-70": 0, "70-80": 0, "80-85": 0, "85-90": 0, ">90": 0
    }
    for d in duties:
        if d < 50:   duty_buckets["<50"]   += 1
        elif d < 70: duty_buckets["50-70"] += 1
        elif d < 80: duty_buckets["70-80"] += 1
        elif d < 85: duty_buckets["80-85"] += 1
        elif d < 90: duty_buckets["85-90"] += 1
        else:        duty_buckets[">90"]   += 1

    # Longest continuous run above 80%
    max_duty_streak_ms = 0.0
    streak_ms          = 0.0
    for d in duties:
        if d > 80.0:
            streak_ms += dt_ms
            if streak_ms > max_duty_streak_ms:
                max_duty_streak_ms = streak_ms
        else:
            streak_ms = 0.0

    # Duty >85% at speed >25 km/h — high-danger events
    high_danger_duty = [
        {"ts_ms": rows[i]["ts_ms"], "duty": duties[i], "speed": speeds[i]}
        for i in range(n)
        if duties[i] > 85.0 and speeds[i] > 25.0
    ]

    # ── Voltage analysis ─────────────────────────────────────────────────────
    # Per-spike sag
    voltage_sag_events = []
    for i in range(n):
        if curr_in[i] > spike_threshold:
            lo_i = max(0, i - 6)
            hi_i = min(n - 1, i + 6)
            w    = voltages[lo_i:hi_i + 1]
            if w:
                voltage_sag_events.append({
                    "ts_ms":   rows[i]["ts_ms"],
                    "curr_in": curr_in[i],
                    "sag_V":   max(w) - min(w),
                })

    # Overall sag during high-current moments
    hc_volts    = [voltages[i] for i, c in enumerate(curr_in) if c > spike_threshold * 0.6]
    voltage_sag = (max(voltages) - min(hc_volts)) if hc_volts \
                  else (max(voltages) - min(voltages))

    # SOC estimation — linear, range from device profile (fallback 20S: 84V/60V)
    _batt_max = float((mcconf or {}).get("_batt_max_v", 84.0))
    _batt_min = float((mcconf or {}).get("_batt_min_v", 60.0))
    def volt_to_soc(v: float) -> float:
        span = _batt_max - _batt_min
        if span <= 0:
            return 0.0
        return max(0.0, min(100.0, (v - _batt_min) / span * 100.0))

    soc_start = volt_to_soc(voltages[0])
    soc_end   = volt_to_soc(voltages[-1])

    # Samples below l_battery_cut_start
    cut_start = (mcconf or {}).get("l_battery_cut_start", 46.0)
    below_cutoff = [
        (rows[i]["ts_ms"], voltages[i])
        for i in range(n)
        if voltages[i] < cut_start
    ]

    # ── Thermal analysis ─────────────────────────────────────────────────────
    def max_rise(series: List[float], window: int) -> float:
        best = 0.0
        for i in range(len(series) - window):
            r = series[i + window] - series[i]
            if r > best:
                best = r
        return best

    w30 = min(int(30 * 12), n - 1)   # 30s @ 12Hz
    w60 = min(int(60 * 12), n - 1)   # 60s @ 12Hz

    rise_30s   = max_rise(temp_fet, w30)
    rise_60s   = max_rise(temp_fet, w60)
    rise_total = temp_fet[-1] - temp_fet[0] if n > 1 else 0.0

    # Mean temp in top-20% current samples (thermal load under stress)
    top20_idx = sorted(range(n), key=lambda i: curr_mot[i], reverse=True)[:max(1, n // 5)]
    temp_at_high_current = sum(temp_fet[i] for i in top20_idx) / len(top20_idx)

    # Minutes to thermal cutoff at current rise rate
    if rise_60s > 0.0 and temp_fet[-1] < fet_start_thresh:
        mins_to_cutoff: Optional[float] = (fet_start_thresh - temp_fet[-1]) / rise_60s
    else:
        mins_to_cutoff = None

    # ── Nosedive Risk Score (0-100) ──────────────────────────────────────────
    nd_score      = 0
    nd_breakdown: List[str] = []

    # duty >85% at speed >20 km/h
    hd20 = [
        {"ts_ms": rows[i]["ts_ms"], "duty": duties[i], "speed": speeds[i]}
        for i in range(n)
        if duties[i] > 85.0 and speeds[i] > 20.0
    ]
    hd_pts = min(60, len(hd20) * 30)
    if hd_pts > 0:
        nd_score += hd_pts
        nd_breakdown.append("duty>85% @ >20km/h: {} event(s) +{}pkt".format(
            len(hd20), hd_pts))

    # Voltage sag >8V
    if voltage_sag >= 8.0:
        nd_score += 20
        nd_breakdown.append("voltage sag {:.1f}V (>8V): +20pkt".format(voltage_sag))

    # Current spikes >90% of limit
    spikes_90 = [e for e in spike_events if e["curr_in_A"] > l_in_max * 0.90]
    sp90_pts  = min(30, len(spikes_90) * 15)
    if sp90_pts > 0:
        nd_score += sp90_pts
        nd_breakdown.append("spiki >90% limitu: {} event(s) +{}pkt".format(
            len(spikes_90), sp90_pts))

    # Temp FET > throttle start
    if max(temp_fet) > fet_start_thresh:
        nd_score += 10
        nd_breakdown.append("temp FET {:.1f}C > prog {:.0f}C: +10pkt".format(
            max(temp_fet), fet_start_thresh))

    nd_score = min(100, nd_score)

    if nd_score <= 20:
        nd_label, nd_level = "BEZPIECZNE", "ok"
    elif nd_score <= 50:
        nd_label, nd_level = "UWAGA",      "warn"
    elif nd_score <= 80:
        nd_label, nd_level = "RYZYKOWNE",  "high"
    else:
        nd_label, nd_level = "NIEBEZPIECZNE", "crit"

    # ── Standard counters ────────────────────────────────────────────────────
    nosedive_candidates = [
        {"ts_ms":     rows[i]["ts_ms"],
         "rpm_before": rpms[i - 1], "rpm_after": rpms[i],
         "drop":       rpms[i - 1] - rpms[i],
         "duty_pct":   duties[i],   "speed_kmh": speeds[i]}
        for i in range(1, n)
        if rpms[i - 1] - rpms[i] > 800
    ]

    total_ah  = amp_hours[-1] - amp_hours[0]
    avg_volt  = sum(voltages) / n
    total_wh  = total_ah * avg_volt
    dt_s      = duration_s / n
    total_km  = sum(speeds) * dt_s / 3600
    wh_per_km = (total_wh / total_km) if total_km > 0.1 else 0.0

    return {
        # — basic —
        "duration_s":              duration_s,
        "max_speed_kmh":           max(speeds),
        "avg_speed_kmh":           sum(speeds) / n,
        "max_curr_in_A":           max(curr_in),
        "avg_curr_in_A":           sum(curr_in) / n,
        "max_curr_mot_A":          max(curr_mot),
        "max_temp_fet_C":          max(temp_fet),
        "max_temp_mot_C":          max(temp_mot),
        "min_voltage_V":           min(voltages),
        "max_voltage_V":           max(voltages),
        "total_ah":                total_ah,
        "wh_per_km":               wh_per_km,
        "sample_count":            n,
        "spike_threshold_A":       spike_threshold,
        "spike_events":            spike_events,
        "current_spikes":          len(spike_events),
        "duty_over_80":            sum(1 for d in duties if d > 80.0),
        "duty_over_85":            sum(1 for d in duties if d > 85.0),
        "duty_over_90":            sum(1 for d in duties if d > 90.0),
        "max_temp_rise_C_per_min": rise_60s,
        "voltage_sag_V":           voltage_sag,
        "nosedive_candidates":     nosedive_candidates,
        # — extended —
        "duty_histogram":          duty_buckets,
        "max_duty_streak_ms":      max_duty_streak_ms,
        "high_danger_duty_events": high_danger_duty,
        "voltage_sag_events":      voltage_sag_events,
        "soc_start_pct":           soc_start,
        "soc_end_pct":             soc_end,
        "below_cutoff_events":     below_cutoff,
        "temp_rise_30s_C":         rise_30s,
        "temp_rise_60s_C":         rise_60s,
        "temp_rise_total_C":       rise_total,
        "temp_at_high_current_C":  temp_at_high_current,
        "mins_to_cutoff":          mins_to_cutoff,
        "nosedive_score":          nd_score,
        "nosedive_risk_label":     nd_label,
        "nosedive_risk_level":     nd_level,
        "nosedive_score_breakdown": nd_breakdown,
    }

# ─────────────────────────────────────────────────────────────────────────────
# WARSTWA 3 — Anomalie i sugestie
# ─────────────────────────────────────────────────────────────────────────────

def detect_anomalies(stats: dict, mcconf: dict, safety_limits: dict,
                     benchmarks: dict, config_alerts: List[dict]) -> List[dict]:
    """Zwraca surowe anomalie (przed sandboxem)."""
    temp_ref = safety_limits["temperature"]
    bench    = benchmarks
    anomalies: List[dict] = []

    # ── 1. Spiki pradu — POPRAWIONA LOGIKA ───────────────────────────────────
    spikes   = stats["current_spikes"]
    l_in_max = mcconf.get("l_in_current_max", 30.0)
    max_cin  = stats["max_curr_in_A"]

    # Czy VESC juz throttluje? (max_cin bliskie/powyzej limitu)
    already_throttling = max_cin > l_in_max * 0.95

    if spikes >= bench["current_spikes"]["investigate_per_session"] and not already_throttling:
        # Wielokrotnie blisko limitu → ochrona ogniw 2P przez obniżenie
        anomalies.append({
            "param":         "l_in_current_max",
            "current":        l_in_max,
            "suggested_raw":  l_in_max * 0.88,
            "reason":         "{}x prad bat. >80% limitu ({:.0f}A) — ochrona ogniw 2P".format(
                                  spikes, stats["spike_threshold_A"]),
            "confidence":    "high" if spikes >= 8 else "medium",
            "safety_impact": "medium",
            "safety_consequence": (
                "Bez zmiany ogniwa 2P narazone na cykliczne przeciazenia. "
                "Degradacja pojemnosci i ryzyko BMS cutoff podczas jazdy."
            ),
            "evidence": (
                ["spike_count={}".format(spikes),
                 "max_curr_in={:.1f}A".format(max_cin),
                 "threshold={:.1f}A".format(stats["spike_threshold_A"])]
                + ["ts={}ms ({:.1f}A)".format(e["ts_ms"], e["curr_in_A"])
                   for e in stats["spike_events"][:3]]
            ),
        })

    # ── 2. Nosedive Risk Score > 50 → obniż l_max_erpm ───────────────────────
    nd_score   = stats["nosedive_score"]
    l_max_erpm = mcconf.get("l_max_erpm", 70000)
    max_speed  = stats["max_speed_kmh"]

    if nd_score > 50:
        risk_pct = min(80, 40 + (nd_score - 50) // 2)
        anomalies.append({
            "param":         "l_max_erpm",
            "current":        l_max_erpm,
            "suggested_raw":  l_max_erpm * 0.90,
            "reason":         "Nosedive Risk Score {}/100 — obniż l_max_erpm".format(nd_score),
            "confidence":    "high" if nd_score > 70 else "medium",
            "safety_impact": "critical" if nd_score > 70 else "high",
            "safety_consequence": (
                "Bez zmiany ryzyko nosedive przy predkosci >{:.0f}km/h wzrasta o ~{}%. "
                "Nosedive = natychmiastowy upadek bez ostrzezenia.".format(
                    max_speed * 0.85, risk_pct)
            ),
            "evidence": (
                ["nosedive_score={}/100".format(nd_score),
                 "risk={}".format(stats["nosedive_risk_label"])]
                + stats["nosedive_score_breakdown"][:2]
            ),
        })
    elif stats["duty_over_85"] >= 3 and stats["nosedive_candidates"]:
        anomalies.append({
            "param":         "l_max_erpm",
            "current":        l_max_erpm,
            "suggested_raw":  l_max_erpm * 0.90,
            "reason":         "{}x duty>85% + {} nosedive kandydat — obniz l_max_erpm".format(
                                  stats["duty_over_85"], len(stats["nosedive_candidates"])),
            "confidence":    "high",
            "safety_impact": "high",
            "safety_consequence": (
                "Wysokie duty + spadki RPM sugeruja ze silnik nie nadaza za zadaniem. "
                "Kolejny podobny manewr moze skonczyc sie nosedive."
            ),
            "evidence": [
                "duty_over_85={}".format(stats["duty_over_85"]),
                "nosedives={}".format(len(stats["nosedive_candidates"])),
            ] + ["ts={}ms drop={:.0f}rpm".format(c["ts_ms"], c["drop"])
                 for c in stats["nosedive_candidates"][:2]],
        })

    # ── 3. Temperatura FET → obniz l_temp_fet_start ──────────────────────────
    max_fet          = stats["max_temp_fet_C"]
    l_temp_fet_start = mcconf.get("l_temp_fet_start", 70.0)
    l_temp_fet_end   = mcconf.get("l_temp_fet_end",   80.0)

    if max_fet >= temp_ref["fet_cutoff_start_C"]:
        mins_left = (
            "{:.0f} min".format(
                (temp_ref["fet_cutoff_end_C"] - max_fet) / stats["max_temp_rise_C_per_min"])
            if stats["max_temp_rise_C_per_min"] > 0 else "n/d"
        )
        anomalies.append({
            "param":         "l_temp_fet_start",
            "current":        l_temp_fet_start,
            "suggested_raw":  l_temp_fet_start - 5.0,
            "reason":         "FET osiagnal {:.0f}C = prog ciecia {:.0f}C — obniz prog".format(
                                  max_fet, temp_ref["fet_cutoff_start_C"]),
            "confidence":    "high",
            "safety_impact": "high",
            "safety_consequence": (
                "Kolejne sesje z tym configiem moga skonczyc sie thermal cutoff "
                "w polowie manewru — nagla utrata mocy na deskorolce (szac. za {}).".format(
                    mins_left)
            ),
            "evidence": [
                "max_temp_fet={:.1f}C".format(max_fet),
                "cutoff_start={}C".format(temp_ref["fet_cutoff_start_C"]),
                "cutoff_end={}C".format(temp_ref["fet_cutoff_end_C"]),
            ],
        })
    elif max_fet >= temp_ref["fet_warning_C"]:
        anomalies.append({
            "param":         "l_temp_fet_start",
            "current":        l_temp_fet_start,
            "suggested_raw":  l_temp_fet_start - 3.0,
            "reason":         "FET {:.0f}C blisko progu warning {:.0f}C".format(
                                  max_fet, temp_ref["fet_warning_C"]),
            "confidence":    "medium",
            "safety_impact": "medium",
            "safety_consequence": (
                "Jesli trasa bedzie dluzsha lub goraco na zewnatrz, "
                "grozi thermal throttle lub cutoff podczas jazdy."
            ),
            "evidence": ["max_temp_fet={:.1f}C".format(max_fet)],
        })

    # ── 4. Szybki wzrost temp → obniz l_current_max ──────────────────────────
    rise          = stats["max_temp_rise_C_per_min"]
    l_current_max = mcconf.get("l_current_max", 100.0)
    rise_thr      = bench["anomaly_thresholds"]

    if rise >= rise_thr["temp_rise_per_min_critical_C"]:
        fet_cutoff = temp_ref["fet_cutoff_start_C"]
        if max_fet >= fet_cutoff:
            consequence = (
                "FET JUZ przekroczyl prog cutoff {:.0f}C (osiagnieto {:.1f}C) — "
                "thermal cutoff nastapil w tej sesji, VESC uciął moc podczas jazdy.".format(
                    fet_cutoff, max_fet)
            )
        elif rise > 0:
            eta_min = (fet_cutoff - max_fet) / rise
            consequence = (
                "Przy obecnym tempie wzrostu VESC osiagnie thermal cutoff "
                "po ok. {:.1f} min jazdy — nagla utrata mocy.".format(eta_min)
            )
        else:
            consequence = "Krytyczny wzrost temperatury — ryzyko thermal cutoff."

        anomalies.append({
            "param":         "l_current_max",
            "current":        l_current_max,
            "suggested_raw":  l_current_max * 0.88,
            "reason":         "Wzrost temp FET {:.1f}C/min (krit.={}C/min) — redukcja".format(
                                  rise, rise_thr["temp_rise_per_min_critical_C"]),
            "confidence":    "high",
            "safety_impact": "high",
            "safety_consequence": consequence,
            "evidence": ["max_temp_rise={:.2f}C/min".format(rise),
                         "max_temp_fet={:.1f}C".format(max_fet)],
        })
    elif rise >= rise_thr["temp_rise_per_min_warning_C"]:
        anomalies.append({
            "param":         "l_current_max",
            "current":        l_current_max,
            "suggested_raw":  l_current_max * 0.93,
            "reason":         "Podwyzszony wzrost temp FET {:.1f}C/min — delikatna redukcja".format(rise),
            "confidence":    "low",
            "safety_impact": "low",
            "safety_consequence": "Obserwuj temperature w dluzszych sesjach.",
            "evidence": ["max_temp_rise={:.2f}C/min".format(rise)],
        })

    # ── 5. Duzy voltage sag → obniz l_in_current_max ─────────────────────────
    sag = stats["voltage_sag_V"]
    if (sag >= 8.0
            and not any(a["param"] == "l_in_current_max" for a in anomalies)):
        l_in = mcconf.get("l_in_current_max", 30.0)
        anomalies.append({
            "param":         "l_in_current_max",
            "current":        l_in,
            "suggested_raw":  l_in * 0.88,
            "reason":         "Voltage sag {:.1f}V (>8V krit.) — redukcja pradu bateryjnego".format(sag),
            "confidence":    "medium",
            "safety_impact": "high",
            "safety_consequence": (
                "Duzy voltage sag oznacza przeciazenie ogniw lub niski SOC. "
                "Ryzyko nagłego BMS cutoff i upadku przy wysokiej predkosci."
            ),
            "evidence": [
                "voltage_sag={:.2f}V".format(sag),
                "min_voltage={:.1f}V".format(stats["min_voltage_V"]),
                "soc_end={:.0f}%".format(stats["soc_end_pct"]),
            ],
        })

    return anomalies


def build_suggestions(anomalies: List[dict], safety_limits: dict,
                      config_alerts: List[dict]) -> List[dict]:
    """Aplikuje sandbox, zwraca maks. 5 sugestii."""
    has_critical = any(a["level"] == "critical" for a in config_alerts)
    suggestions: List[dict] = []
    seen: Set[str] = set()

    for a in anomalies:
        param = a["param"]
        if param in seen or param not in WHITELIST:
            continue
        current   = a["current"]
        sandboxed = sandbox(param, a["suggested_raw"], current, safety_limits)
        if sandboxed is None or sandboxed == current:
            continue
        if has_critical and sandboxed > current:
            continue   # blokuj wzrosty gdy config ma błąd krytyczny

        delta_pct = round((sandboxed - current) / current * 100, 1)
        suggestions.append({
            "param":              param,
            "current":            current,
            "suggested":          sandboxed,
            "delta_pct":          delta_pct,
            "reason":             a["reason"][:100],
            "confidence":         a["confidence"],
            "safety_impact":      a.get("safety_impact", "medium"),
            "safety_consequence": a.get("safety_consequence", ""),
            "evidence":           a["evidence"],
            "safe":               True,
        })
        seen.add(param)

    # Sortuj: critical > high > medium > low
    impact_order = {"critical": 0, "high": 1, "medium": 2, "low": 3}
    suggestions.sort(key=lambda s: impact_order.get(s["safety_impact"], 4))
    return suggestions[:5]


def generate_summary(stats: dict, suggestions: List[dict],
                     config_alerts: List[dict]) -> str:
    lines: List[str] = []
    crits = [a for a in config_alerts if a["level"] == "critical"]
    nd    = stats.get("nosedive_score", 0)

    if crits:
        lines.append(
            "UWAGA: config VESC ma {} krytycznych problemow — "
            "napraw przed nastepna jazda.".format(len(crits))
        )
    if nd > 50:
        lines.append(
            "Nosedive Risk {}/100 ({}) — sesja zawierala zdarzenia "
            "wysokiego ryzyka upadku.".format(nd, stats.get("nosedive_risk_label", "?"))
        )
    if suggestions:
        top = suggestions[0]
        lines.append(
            "Priorytet: {} ({} → {}) — {}.".format(
                top["param"], top["current"], top["suggested"],
                top["reason"][:70])
        )
    if not lines:
        lines.append("Sesja w normie — config i dane bez powaznych anomalii.")
    return "  ".join(lines)

# ─────────────────────────────────────────────────────────────────────────────
# WARSTWA 4 — Raport (plain text)
# ─────────────────────────────────────────────────────────────────────────────

def _c(text: str, code: str) -> str:
    return "\033[{}m{}\033[0m".format(code, text)

def _badge_str(val, warn, crit, fmt: Optional[str] = None) -> str:
    s = ("{:.1f}".format(val) if isinstance(val, float) else str(val)) if fmt is None else fmt
    if val >= crit: return "{:<8}  {}".format(s, _c("KRYT",  "31;1"))
    if val >= warn: return "{:<8}  {}".format(s, _c("UWAGA", "33"))
    return "{:<8}  {}".format(s, _c("OK", "32"))


def print_report_plain(session_name: str, stats: dict, suggestions: List[dict],
                       config_alerts: List[dict], summary: str) -> None:
    W = 66

    def hdr(title: str, color: str = "1;36") -> str:
        line = "─" * 3 + " " + title + " "
        return _c(line + "─" * max(0, W - len(line)), color)

    # ── Nagłówek ─────────────────────────────────────────────────────────────
    inner  = W - 2
    title  = "VESC AI TUNER — RAPORT BEZPIECZENSTWA"
    pad_l  = (inner - len(title)) // 2
    pad_r  = inner - len(title) - pad_l
    print()
    print("╔" + "═" * inner + "╗")
    print("║" + " " * pad_l + _c(title, "1;36") + " " * pad_r + "║")
    print("╚" + "═" * inner + "╝")
    print("  sesja: {}  |  {}".format(
        _c(session_name, "1;33"),
        datetime.now().strftime("%Y-%m-%d %H:%M")))

    # ── Alerty konfiguracji ───────────────────────────────────────────────────
    if config_alerts:
        print("\n" + hdr("ALERTY KONFIGURACJI", "1;31"))
        for a in config_alerts:
            icon = _c("  ✖ KRYTYCZNY", "31;1") if a["level"] == "critical" \
                   else _c("  ⚠ OSTRZEZENIE", "33")
            print("{} [{}]".format(icon, _c(a["param"], "1")))
            print("    {}".format(a["message"]))
            if a.get("value") is not None:
                print("    Wartosc: {}    Oczekiwane: {}".format(
                    _c(str(a["value"]), "33"), a.get("expected", "?")))

    # ── Statystyki sesji ──────────────────────────────────────────────────────
    print("\n" + hdr("STATYSTYKI SESJI"))
    lw = 34
    print("  {:<{w}} {:.1f} min  ({} probek @ ~12Hz)".format(
        "Czas trwania", stats["duration_s"] / 60, stats["sample_count"], w=lw))
    print("  {:<{w}} {:.1f} / {:.1f} km/h".format(
        "Maks. / sr. predkosc", stats["max_speed_kmh"], stats["avg_speed_kmh"], w=lw))
    print("  {:<{w}} {:.1f} A  (prog spike: {:.1f} A)".format(
        "Maks. prad bateryjny", stats["max_curr_in_A"], stats["spike_threshold_A"], w=lw))
    print("  {:<{w}} {:.1f} A".format(
        "Maks. prad silnikowy", stats["max_curr_mot_A"], w=lw))

    fet_col = "31" if stats["max_temp_fet_C"] >= 70 else "33" if stats["max_temp_fet_C"] >= 65 else "32"
    print("  {:<{w}} {}".format(
        "Maks. temp FET",
        _c("{:.1f} C".format(stats["max_temp_fet_C"]), fet_col), w=lw))

    mot_col = "31" if stats["max_temp_mot_C"] >= 80 else "33" if stats["max_temp_mot_C"] >= 75 else "32"
    print("  {:<{w}} {}".format(
        "Maks. temp silnik",
        _c("{:.1f} C".format(stats["max_temp_mot_C"]), mot_col), w=lw))

    print("  {:<{w}} {:.1f} V".format(
        "Min napiecie", stats["min_voltage_V"], w=lw))
    print("  {:<{w}} {:.0f}% → {:.0f}%  (delta -{:.0f}%)".format(
        "SOC start → koniec",
        stats["soc_start_pct"], stats["soc_end_pct"],
        stats["soc_start_pct"] - stats["soc_end_pct"], w=lw))
    print("  {:<{w}} {:.3f} Ah  ({:.1f} Wh/km)".format(
        "Zuzycie", stats["total_ah"], stats["wh_per_km"], w=lw))

    # Histogram duty
    print("\n  Histogram duty cycle:")
    dh = stats["duty_histogram"]
    total = max(1, sum(dh.values()))
    for bucket, count in dh.items():
        pct = count / total * 100
        bar = "█" * min(30, int(pct / 2))
        print("    {:>6}%  {:>4}x  {:>5.1f}%  {}".format(bucket, count, pct, bar))
    print("  {:<{w}} {:.0f} ms".format(
        "Najdluzszy ciag duty>80%", stats["max_duty_streak_ms"], w=lw))

    if stats["high_danger_duty_events"]:
        print("  " + _c("  ⚠ duty>85% przy >25km/h: {}x  ← EKSTREMALNIE NIEBEZPIECZNE".format(
            len(stats["high_danger_duty_events"])), "31;1"))

    # Thermal
    print("\n  Analiza termiczna:")
    print("  {:<{w}} {:.1f} C/30s  |  {:.1f} C/min  (sesja: +{:.1f} C)".format(
        "Wzrost temp FET", stats["temp_rise_30s_C"],
        stats["temp_rise_60s_C"], stats["temp_rise_total_C"], w=lw))
    if stats["mins_to_cutoff"] is not None:
        m = stats["mins_to_cutoff"]
        col = "31" if m < 5 else "33" if m < 15 else "32"
        print("  {:<{w}} {}".format(
            "Prognoza: czas do thermal cutoff",
            _c("{:.1f} min".format(m), col), w=lw))

    # ── Nosedive Risk Score ───────────────────────────────────────────────────
    nd    = stats["nosedive_score"]
    level = stats["nosedive_risk_level"]
    label = stats["nosedive_risk_label"]
    col   = {"ok": "32", "warn": "33", "high": "31", "crit": "31;1"}.get(level, "37")
    fill  = int(nd / 2.5)
    bar   = "█" * fill + "░" * (40 - fill)

    print("\n" + hdr("NOSEDIVE RISK SCORE", "1;" + col))
    print("  {} / 100   {}   {}".format(
        _c("{:3d}".format(nd), "1;" + col),
        _c("[" + bar + "]", col),
        _c(label, "1;" + col)))
    for line in stats["nosedive_score_breakdown"]:
        print("    · " + line)
    risk_desc = {
        "ok":   "Sesja przebiegla bezpiecznie.",
        "warn": "Wykryto zdarzenia wymagajace uwagi. Rozważ sugestie.",
        "high": "Sesja ryzykowna. Zastosuj sugestie PRZED nastepna jazda.",
        "crit": "SESJA NIEBEZPIECZNA. Konieczne natychmiastowe zmiany konfiguracji!",
    }.get(level, "")
    print("  " + _c(risk_desc, col))

    # ── Anomalie ──────────────────────────────────────────────────────────────
    print("\n" + hdr("ANOMALIE", "1;33"))
    for lbl, val, warn, crit in [
        ("Spike pradu >80% limitu",  stats["current_spikes"],        3,   5),
        ("Duty >85%",                 stats["duty_over_85"],           1,   5),
        ("Duty >90%",                 stats["duty_over_90"],           1,   3),
        ("Wzrost temp FET [C/min]",   stats["max_temp_rise_C_per_min"], 3.0, 6.0),
        ("Voltage sag [V]",           stats["voltage_sag_V"],         4.0, 7.0),
        ("Nosedive kandydaci",        len(stats["nosedive_candidates"]), 1, 3),
    ]:
        fmt = "{:.1f}".format(val) if isinstance(val, float) else str(val)
        print("  {:<34} {}".format(lbl, _badge_str(val, warn, crit, fmt)))

    # ── Sugestie ──────────────────────────────────────────────────────────────
    print("\n" + hdr("SUGESTIE ZMIAN ({}/5)".format(len(suggestions)), "1;36"))
    if not suggestions:
        print("  " + _c("Brak sugestii — sesja w normie.", "32"))
    for i, s in enumerate(suggestions, 1):
        ic  = {"critical": "31;1", "high": "31", "medium": "33", "low": "90"}.get(
               s["safety_impact"], "37")
        cc  = {"high": "32", "medium": "33", "low": "90"}.get(s["confidence"], "37")
        arr = (_c("▲ +{}%".format(abs(s["delta_pct"])), "33")
               if s["delta_pct"] > 0
               else _c("▼ {}%".format(s["delta_pct"]), "31"))
        print()
        print("  {}. {}   {} → {}   {}   conf:{} impact:{}".format(
            i,
            _c(s["param"], "36;1"),
            s["current"], s["suggested"],
            arr,
            _c(s["confidence"], cc),
            _c(s["safety_impact"].upper(), ic)))
        print("     Powod:        {}".format(s["reason"]))
        print("     Konsekwencja: {}".format(s["safety_consequence"][:88]))
        print("     Dowody:       {}".format(", ".join(s["evidence"][:3])))

    # ── Podsumowanie ──────────────────────────────────────────────────────────
    print("\n" + hdr("PODSUMOWANIE"))
    print("  " + summary)

    print("\n" + "─" * W)
    print("  " + _c(
        "PAMIETAJ: zadna zmiana nie idzie do VESC bez Twojej akceptacji [Y/N] na Cardputerze",
        "1;32"))
    print("─" * W + "\n")


# ─────────────────────────────────────────────────────────────────────────────
# Warstwa 4 — Raport (rich)
# ─────────────────────────────────────────────────────────────────────────────

def print_report_rich(session_name: str, stats: dict, suggestions: List[dict],
                      config_alerts: List[dict], summary: str) -> None:
    from rich.rule import Rule
    console = Console()

    console.print()
    console.rule("[bold cyan]VESC AI TUNER — RAPORT BEZPIECZENSTWA[/bold cyan]")
    console.print("  sesja: [bold yellow]{}[/bold yellow]  |  {}".format(
        session_name, datetime.now().strftime("%Y-%m-%d %H:%M")))
    console.print()

    # ── Alerty konfiguracji ───────────────────────────────────────────────────
    if config_alerts:
        console.rule("[bold red]ALERTY KONFIGURACJI[/bold red]")
        for a in config_alerts:
            icon = "[red]✖ KRYTYCZNY[/red]" if a["level"] == "critical" \
                   else "[yellow]⚠ OSTRZEZENIE[/yellow]"
            console.print("  {} [bold]{}[/bold]  {}".format(
                icon, a["param"], a["message"]))
            if a.get("value") is not None:
                console.print(
                    "    [dim]Wartosc:[/dim] [yellow]{}[/yellow]   "
                    "[dim]Oczekiwane:[/dim] [green]{}[/green]".format(
                        a["value"], a.get("expected", "?")))
        console.print()

    # ── Statystyki ────────────────────────────────────────────────────────────
    t = Table(title="Statystyki sesji", box=rich_box.ROUNDED,
              show_header=False, padding=(0, 1))
    t.add_column(style="cyan", no_wrap=True)
    t.add_column(justify="right")

    t.add_row("Czas trwania",
              "{:.1f} min  ({} probek)".format(stats["duration_s"] / 60, stats["sample_count"]))
    t.add_row("Maks. / sr. predkosc",
              "{:.1f} / {:.1f} km/h".format(stats["max_speed_kmh"], stats["avg_speed_kmh"]))
    t.add_row("Maks. prad bateryjny",
              "{:.1f} A  (prog: {:.1f} A)".format(stats["max_curr_in_A"], stats["spike_threshold_A"]))
    t.add_row("Maks. prad silnikowy", "{:.1f} A".format(stats["max_curr_mot_A"]))

    fc = "red" if stats["max_temp_fet_C"] >= 70 else "yellow" if stats["max_temp_fet_C"] >= 65 else "green"
    t.add_row("Maks. temp FET",
              "[{fc}]{v:.1f} C[/{fc}]".format(fc=fc, v=stats["max_temp_fet_C"]))
    mc = "red" if stats["max_temp_mot_C"] >= 80 else "yellow" if stats["max_temp_mot_C"] >= 75 else "green"
    t.add_row("Maks. temp silnik",
              "[{mc}]{v:.1f} C[/{mc}]".format(mc=mc, v=stats["max_temp_mot_C"]))
    t.add_row("Min napiecie", "{:.1f} V".format(stats["min_voltage_V"]))
    t.add_row("SOC start → koniec",
              "{:.0f}% → {:.0f}%  (Δ -{:.0f}%)".format(
                  stats["soc_start_pct"], stats["soc_end_pct"],
                  stats["soc_start_pct"] - stats["soc_end_pct"]))
    t.add_row("Zuzycie",
              "{:.3f} Ah  ({:.1f} Wh/km)".format(stats["total_ah"], stats["wh_per_km"]))
    console.print(t)

    # Duty histogram
    dh = stats["duty_histogram"]
    total = max(1, sum(dh.values()))
    hist_parts = ["[dim]{}%:[/dim] {}x".format(k, v) for k, v in dh.items()]
    console.print("  [dim]Duty histogram:[/dim]  " + "   ".join(hist_parts))
    console.print("  [dim]Najdluzszy ciag duty>80%:[/dim]  {:.0f} ms".format(
        stats["max_duty_streak_ms"]))
    if stats["high_danger_duty_events"]:
        console.print("[bold red]  ⚠ duty>85% przy >25km/h: {}x  ← EKSTREMALNIE NIEBEZPIECZNE[/bold red]".format(
            len(stats["high_danger_duty_events"])))

    # Thermal
    console.print("  [dim]Wzrost temp FET:[/dim]  {:.1f} C/30s  |  {:.1f} C/min  (sesja: +{:.1f} C)".format(
        stats["temp_rise_30s_C"], stats["temp_rise_60s_C"], stats["temp_rise_total_C"]))
    if stats["mins_to_cutoff"] is not None:
        m = stats["mins_to_cutoff"]
        col = "red" if m < 5 else "yellow" if m < 15 else "green"
        console.print("  [dim]Prognoza do thermal cutoff:[/dim]  [{c}]{m:.1f} min[/{c}]".format(
            c=col, m=m))
    console.print()

    # ── Nosedive Risk ─────────────────────────────────────────────────────────
    nd    = stats["nosedive_score"]
    level = stats["nosedive_risk_level"]
    label = stats["nosedive_risk_label"]
    col   = {"ok": "green", "warn": "yellow", "high": "red", "crit": "bold red"}.get(level, "white")
    fill  = int(nd / 2.5)
    bar   = "█" * fill + "░" * (40 - fill)

    console.rule("[bold]NOSEDIVE RISK SCORE[/bold]")
    console.print("  [{c}]{n:3d} / 100   [{bar}]   {label}[/{c}]".format(
        c=col, n=nd, bar=bar, label=label))
    for line in stats["nosedive_score_breakdown"]:
        console.print("    [dim]·[/dim] {}".format(line))
    risk_desc = {
        "ok":   "[green]Sesja bezpieczna.[/green]",
        "warn": "[yellow]Zdarzenia wymagajace uwagi.[/yellow]",
        "high": "[red]Sesja ryzykowna — zastosuj sugestie przed nastepna jazda.[/red]",
        "crit": "[bold red]SESJA NIEBEZPIECZNA — konieczne natychmiastowe zmiany![/bold red]",
    }.get(level, "")
    console.print("  " + risk_desc)
    console.print()

    # ── Anomalie ──────────────────────────────────────────────────────────────
    a = Table(title="Anomalie", box=rich_box.ROUNDED, padding=(0, 1))
    a.add_column("Parametr", style="cyan")
    a.add_column("Wartosc", justify="right")
    a.add_column("Status")

    def badge(val, warn, crit):
        if val >= crit: return "[red]KRYTYCZNE[/red]"
        if val >= warn: return "[yellow]UWAGA[/yellow]"
        return "[green]OK[/green]"

    a.add_row("Spike pradu >80% limitu", str(stats["current_spikes"]),
              badge(stats["current_spikes"], 3, 5))
    a.add_row("Duty >85%", str(stats["duty_over_85"]),
              badge(stats["duty_over_85"], 1, 5))
    a.add_row("Duty >90%", str(stats["duty_over_90"]),
              badge(stats["duty_over_90"], 1, 3))
    r = stats["max_temp_rise_C_per_min"]
    a.add_row("Wzrost temp FET", "{:.1f} C/min".format(r), badge(r, 3.0, 6.0))
    sg = stats["voltage_sag_V"]
    a.add_row("Voltage sag", "{:.1f} V".format(sg), badge(sg, 4.0, 7.0))
    nd_c = len(stats["nosedive_candidates"])
    a.add_row("Nosedive kandydaci", str(nd_c), badge(nd_c, 1, 3))
    console.print(a)

    # ── Sugestie ──────────────────────────────────────────────────────────────
    if not suggestions:
        console.print(Panel("[green]Brak sugestii — sesja w normie.[/green]",
                            title="Sugestie"))
    else:
        console.print("[bold]Sugestie zmian ({}/5):[/bold]".format(len(suggestions)))
        for i, s in enumerate(suggestions, 1):
            ic  = {"critical": "bold red", "high": "red",
                   "medium": "yellow", "low": "dim"}.get(s["safety_impact"], "white")
            cc  = {"high": "green", "medium": "yellow", "low": "dim"}.get(s["confidence"], "white")
            arr = ("[green]▲ +{}%[/green]".format(abs(s["delta_pct"]))
                   if s["delta_pct"] > 0
                   else "[red]▼ {}%[/red]".format(s["delta_pct"]))
            console.print(
                "  [bold]{i}.[/bold] [bold yellow]{p}[/bold yellow]  "
                "{cur} → {sug}  {arr}  [{cc}][{conf}][/{cc}]  "
                "[{ic}]impact:{imp}[/{ic}]".format(
                    i=i, p=s["param"], cur=s["current"], sug=s["suggested"],
                    arr=arr, cc=cc, conf=s["confidence"],
                    ic=ic, imp=s["safety_impact"].upper()))
            console.print("     [dim]Powod:[/dim]        {}".format(s["reason"]))
            console.print("     [dim]Konsekwencja:[/dim]  {}".format(
                s["safety_consequence"][:88]))
            console.print("     [dim]Dowody:[/dim]        {}\n".format(
                ", ".join(s["evidence"][:3])))

    # ── Podsumowanie ──────────────────────────────────────────────────────────
    console.print(Panel(summary, title="[bold]Podsumowanie[/bold]"))
    console.rule(
        "[bold green]PAMIETAJ: zadna zmiana nie idzie do VESC "
        "bez Twojej akceptacji [Y/N] na Cardputerze[/bold green]")
    console.print()

# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def main() -> None:
    if len(sys.argv) < 2:
        print("Uzycie:   python3 analyze.py <session_name>")
        print("Przyklad: python3 analyze.py example_session")
        sys.exit(1)

    session_name = sys.argv[1]
    csv_path     = SESSIONS_DIR / "{}.csv".format(session_name)
    mcconf_path  = SESSIONS_DIR / "{}_mcconf.json".format(session_name)
    appconf_path = SESSIONS_DIR / "{}_appconf.json".format(session_name)

    if not csv_path.exists():
        print("BLAD: Nie znaleziono pliku sesji: {}".format(csv_path))
        sys.exit(1)

    safety_limits = load_json(PEV_REF_DIR / "safety_limits_20s.json")
    benchmarks    = load_json(PEV_REF_DIR / "tune_benchmarks.json")

    # Load device profile (from SD root, written by Cardputer)
    profile = load_device_profile()
    if profile:
        vlimits = profile_to_limits(profile)
        print("[PROFIL] {} | {}S | {:.0f}V/{:.0f}V".format(
            profile.get("device_name", "Unknown"),
            vlimits["cells"], vlimits["batt_max_v"], vlimits["batt_min_v"]))
    else:
        vlimits = {"cells": 20, "batt_max_v": 84.0, "batt_min_v": 60.0}
        print("[PROFIL] Brak device_profile.json — uzywam domyslnych 20S (84V/60V)")

    rows = load_csv(csv_path)
    if not rows:
        print("BLAD: Plik CSV pusty lub uszkodzony.")
        sys.exit(1)

    mcconf = None
    if mcconf_path.exists():
        mcconf = load_json(mcconf_path)
    else:
        print("\n[UWAGA] Brak {} — tryb opisowy, ZERO sugestii zmian.\n".format(
            mcconf_path.name))

    # Inject profile voltage limits into mcconf so compute_stats can use them
    if mcconf is None:
        mcconf = {}
    mcconf.setdefault("_batt_max_v", vlimits["batt_max_v"])
    mcconf.setdefault("_batt_min_v", vlimits["batt_min_v"])

    # Pipeline analityczny
    stats         = compute_stats(rows, mcconf, safety_limits)
    config_alerts = verify_config(mcconf, session_stats=stats) if mcconf else []
    anomalies     = detect_anomalies(stats, mcconf, safety_limits,
                                     benchmarks, config_alerts) if mcconf else []
    suggestions   = build_suggestions(anomalies, safety_limits, config_alerts)
    summary       = generate_summary(stats, suggestions, config_alerts)

    if HAS_RICH:
        print_report_rich(session_name, stats, suggestions, config_alerts, summary)
    else:
        print_report_plain(session_name, stats, suggestions, config_alerts, summary)

    # Zapis
    CONFIG_DIR.mkdir(exist_ok=True)
    ts = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = {
        "session":        session_name,
        "generated_at":   ts,
        "mcconf_source":  str(mcconf_path) if mcconf else None,
        "config_alerts":  config_alerts,
        "nosedive_score": stats.get("nosedive_score", 0),
        "suggestions":    suggestions,
    }
    out_path    = CONFIG_DIR / "suggestions.json"
    backup_path = CONFIG_DIR / "suggestions_{}.json".format(ts)
    for path in (out_path, backup_path):
        with open(path, "w", encoding="utf-8") as f:
            json.dump(output, f, indent=2, ensure_ascii=False)

    print("Zapisano:  {}".format(out_path))
    print("Backup:    {}".format(backup_path))
    if suggestions:
        max_delta = max(abs(s["delta_pct"]) for s in suggestions)
        print("\n→ {} sugestia(e), maks. zmiana {}% — gotowe do Cardputera.".format(
            len(suggestions), max_delta))
    else:
        print("\n→ Brak sugestii.")


if __name__ == "__main__":
    main()
