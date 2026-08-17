# ADV500 + Sidewinder: 4x current transient at ~2 km/h, ABS_OVER_CURRENT

**Two Floatwheel ADV2 boards, same ADV500 controller, same rider (106 kg), same provocation.
One cuts out, the other doesn't. The only physical difference is the motor.**

---

## The question

> What can produce a phase-current transient **4x above the filtered current** at **588 erpm
> in hall mode**, on a motor with **Lq/Ld = 1.34**, when the hall table is measurably healthy
> (max deviation 3.0 deg from 60 deg spacing)?

I am not asking how to suppress the fault. `l_abs_current_max` is doing its job. I want to know
what makes the current overshoot in the first place.

---

## Setup

| | board A - works | board B "GAD" - faults |
|---|---|---|
| Frame | Floatwheel ADV2 | Floatwheel ADV2 |
| Controller | ADV500 | ADV500 (`adv500-lckx`) |
| Firmware | 6.05 (ConfigVersion 3) | 6.06 (ConfigVersion 4) |
| Motor | Cannoncore (stock) | **Sidewinder** (swapped in) |
| Package | Refloat 1.2.3 | Refloat 1.2.3 |
| Rider | 106 kg | 106 kg |
| Sensor mode | HALL | HALL |

Same rider rocks both boards fore/aft while stepping on and off. Board A never cuts out.
Board B throws `ABS_OVER_CURRENT`.

---

## The three captured faults

| | 2026-08-09 | after Nico's tweaks | **2026-08-17** |
|---|---|---|---|
| Current | 258.0 A | -236.0 A | **260.9 A** |
| Current filtered | 34.9 A | -14.3 A | **62.0 A** |
| **peak / filtered** | 7.4x | 16.5x | **4.2x** |
| Duty | 0.215 | **0.950** | 0.285 |
| RPM | -370 | **0.0** | **-588** |
| Voltage | 53.77 V | 65.68 V | 61.55 V |
| Temperature | 31.4 C | - | 26.3 C |
| Cycles running | - | 99 | 28604 |

All three are at **1.3 / 0 / 2.1 km/h** - below walking pace, deep inside hall territory
(`foc_sl_erpm_start = 1800` = 6.3 km/h).

**Voltage is not battery sag from a weak pack.** Resting pack is 77.38 V (3.86 V/cell).
61.55 V under a 74 A battery-side draw implies ~213 mOhm, ~21 mOhm per cell in 20S2P. Normal.

---

## What we already ruled out (with data, not opinion)

**Hall sensors are healthy - and better than the working board's.**
Decoded `foc_hall_table` (value/200 * 360 deg, state order 1-3-2-6-4-5):

```
Board A (works):  52.2  55.8  61.2  64.8  66.6  59.4   -> max deviation 7.8 deg
Board B (faults): 59.4  61.2  59.4  57.6  63.0  59.4   -> max deviation 3.0 deg
```
Confirmed independently by `hall_analyze` (7 runs, 2.5-3.5 deg deviation, always 6 states).

**Motor electrical parameters favour the faulting board.**
`di/dt = V/L` at 61.55 V: board A **692 A/ms**, board B **478 A/ms**. Board B's higher
inductance builds current *slower*. A pure electrical transient from a misapplied voltage
vector should hurt board A more.

**Current limit is not "too low".** Board B makes more torque at its lower limit:
`kt = 1.5 * 15 * flux`; A = 0.574 Nm/A, B = 0.630 Nm/A.
A @ 240 A = 137.7 Nm. B @ 225 A = **141.8 Nm**. The factory scaled the limit to the motor.

**Also excluded:** field weakening (`foc_fw_current_max = 40` on both), start/stop click
(`startup_click_current = 10` on both), footpads (0% intermediate ADC values), battery,
temperature (26 C).

**Detection is repeatable, not noise.** R went 76.8 -> 79.9 mOhm between June and August,
which matches copper's temperature coefficient (0.393 %/C) for the recorded dT to within 0.4%.

---

## What changed on 2026-08-17 (and measurably helped)

Three settings, each aligning board B with board A:

| | before | after |
|---|---|---|
| `foc_offsets_cal_mode` | 0 | 1 (Calibrate on Boot) |
| `foc_sat_comp_mode` | 2 (Lambda) | 0 (Disabled) |
| `m_hall_extra_samples` | 1 | 3 |

Result: peak/filtered **16.5x -> 4.2x**, and the pathological `duty 0.950 @ 0 rpm` signature
is gone. Kick frequency dropped (1 kick in ~15-30 engagements). **The fault still happens.**

Note on `foc_sat_comp_mode`: with `observer_type >= 2`, `SAT_COMP_LAMBDA` scales
`L = L * (lambda_est / lambda)` with `lambda_est` clamped to `[0.3, 2.5] * lambda`. On a motor
with Ld-Lq = 29% of L, combined with the saliency correction
`L = L - ld_lq_diff/2 + ld_lq_diff * iq^2/(id^2+iq^2)`, worst case took the observer's L from
~148 uH to ~20 uH. Vedder's own comment there reads *"I have no idea if this is a valid or even
a reasonable assumption."* Board A had it Disabled; the firmware default is also Disabled.

---

## Current leading hypothesis (testable, not proven)

The FOC layer executes what Refloat commands. Refloat's rate term dominates during rocking:

```c
// refloat/src/pid.c:69
pid->rate_p = -imu->pitch_rate * config->kp2 * TORQUE_CONSTANT_COMPAT;

// refloat/src/motor_data.c:103-108  - torque -> current
if (flux_linkage > 0.001f && motor_poles > 0)
    m->speed_constant = 1 / (1.5f * 0.5 * motor_poles * flux_linkage);   // FW 6.06
else
    m->speed_constant = 1 / TORQUE_CONSTANT_COMPAT;                      // FW < 6.06

// refloat/src/lib/utils.h:38
#define TORQUE_CONSTANT_COMPAT (1.5f * 15 * 0.027f)   // = 0.6075
```

On **FW 6.05** flux linkage is not exposed, so `TORQUE_CONSTANT_COMPAT` cancels out and current
is simply `error * gain`. On **FW 6.06** the real flux is used, so the command is scaled by
`TCC / kt = 0.6075 / 0.630 = 0.9643`.

| | board A | board B |
|---|---|---|
| `kp2` | 0.7 | 0.9 |
| torque->current factor | 1.0000 (6.05 fallback) | 0.9643 (6.06 real flux) |
| **effective rate gain** | 0.7000 | **0.8679 (+24%)** |

With the measured 4.2x peak multiplier: 62 A base -> 261 A (fault). At board A's tune the base
would be ~50 A -> ~210 A, **under the 225 A limit**.

**Weak link, stated plainly:** the 4.2x multiplier comes from a single event. Treating it as a
constant is extrapolation. This hypothesis predicts a specific number and can be falsified.

**Planned test:** `kp2` 0.9 -> 0.7, single variable, 30 engagements, capture `faults`.

---

## Rider mass matters and explains why bench testing found nothing

Current needed just to hold a lean angle at standstill (`kt = 0.63 Nm/A`, CoG ~1 m above axle):

| lean | 106 kg | 70 kg | by hand (no rider) |
|---|---|---|---|
| 1 deg | 29 A | 19 A | 0 A |
| **2 deg** | **58 A** | 38 A | **0 A** |
| 3 deg | 86 A | 57 A | 0 A |

At a 4.2x multiplier the 225 A limit breaks once base current exceeds **53 A**. So a 2 degree
lean with this rider already sits at the threshold, while rocking the board by hand
(16 kg, ~6 A at 2 m/s^2) is roughly 10x below it. **A 70 kg rider might never see this fault
on the same hardware.**

---

## Remaining configuration differences (board A vs board B)

**Motor (detection output, expected):** R 62.9/79.8 mOhm - L 88.9/128.9 uH -
Ld-Lq 19.8/37.8 uH (Lq/Ld 1.25 vs **1.34**) - flux 25.5/28.0 mWb - observer_gain 750k/640k

**Still differing in FOC:** `foc_observer_type` 3 vs **2** - `foc_sl_erpm` 2300/2000 -
`foc_openloop_rpm` 1000/700 - `foc_fw_duty_start` 0.7/0.65.
All of these act above ~6.3 km/h, i.e. outside the fault regime.

**Identical:** `foc_hall_interp_erpm` 250 - `foc_dt_us` 0.12 - `foc_f_zv` 25 kHz -
`foc_sl_erpm_start` 1800 - `foc_phase_filter_max_erpm` 4000 - `l_max_duty` 0.95

**Limits:** board B is tighter everywhere (150/225 A vs 160/240 A, erpm +-17000 vs +-100000).

**Same controller?** No hardware ID exists in the exported XML. Indirect evidence: current
sensor offsets differ by **0.92 LSB out of 4096 (0.022%)**, both sitting ~14 LSB below ADC
midpoint; identical dead time, IMU type and IMU sample rate. `HW_NAME` readback from board A
still pending.

---

## Separate note for Dado / surfdado

The field-weakening speed gate described in the release notes
(*"Disable field weakening when erpm < sensorless"*, commit `a8bc61a`) is **not present in any
released branch** - checked `ADV_Vanilla_v605`, `v66_pinlock` and upstream. All of them have the
bare duty-only condition in `foc_run_fw()`. The commit exists in repo history but never landed
on a release branch. Worth correcting the notes, or cherry-picking the commit.

---

## Data available on request

Full `MotorCfg` / `AppCfg` / `RefloatCfg` XML exports for both boards (pre- and post-change),
BMS cell log, and ~20 ride sessions logged at 25 Hz (speed, erpm, voltage, motor/battery
current, duty, FET and motor temperature).
