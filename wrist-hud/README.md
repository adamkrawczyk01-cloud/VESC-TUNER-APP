# Wrist / forearm HUD bracer

A 3D-printed forearm cradle that carries the **HUD set + battery on your arm** —
an alternative to the helmet HUD. Same firmware (`helmet/atoms3r_hud`), same
ESP-NOW link from the board unit. Read-only telemetry, glance at your forearm.

```
   WRIST end  ────────────────────────────────────────►  ELBOW end
  ┌──────────┬──────────────┬──────┬────────────────────────────┐
  │ AtomS3R  │  Puzzle 8x8   │ gap  │      power bank / LiPo      │
  │ (screen  │  (2x, 48x24   │(cable│      (lays flat)            │
  │  + IMU   │   4 bars)     │ bend)│                             │
  │  + btn)  │              │      │                             │
  └────┬─────┴───────┬──────┴──────┴──────────────┬──────────────┘
       │ USB-C in    │ Grove data                 │ USB-C OUT (pack)
       └─────────────┴──── internal channels ─────┘
        two straps wrap the forearm (wrist + elbow)
```

## Why a bracer (not a wristwatch)
The Anker power bank + 2× Puzzle panel are too big/heavy for the wrist. On the
**forearm** the pack lays flat, the HUD sits by the wrist tilted toward your face.
Natural glance while riding, no combiner optics. Lighter and simpler than the
helmet build; you already have the electronics.

## Parts (print each separately)
Set `part` at the top of `wrist_mount.scad`, F5 preview, F6 render, export STL.

| part           | what it is                                             |
|----------------|--------------------------------------------------------|
| `tray`         | curved forearm base: HUD + battery pockets + cable channels + strap slots |
| `bezel`        | thin frame that clips over the HUD, windows for the screen/button/LEDs |
| `battery_lid`  | vented cover so the pack can't pop out                 |

**Print:** PETG, 0.2 mm, 4 perimeters, 25–30% infill, brim.
Print the **tray cavity-up** (pockets facing up) → no supports.
Bezel + lid print flat.

## MEASURE first (caliper) — top of the .scad
- `arm_dia` — your forearm diameter where you'll wear it (sets the saddle curve).
- `bat_l/bat_w/bat_t` — **your** pack. Default is a slim Anker-ish box; the Anker
  Nano 5K is chunky — a flat LiPo **103450 (~52×34×11, 2000 mAh)** is much lighter
  and plenty for a ride. Just re-measure and re-render.
- AtomS3R + Puzzle dims already match the helmet build; tweak only the tolerances.

## Wiring (READ-ONLY — nothing goes to the VESC)
Two short internal runs live in the tray channels:

1. **Power (USB-C):** power bank USB-C **OUT** → short USB-C→USB-C cable →
   AtomS3R USB-C **IN**. This is the only power path (5 V). Use a **right-angle
   USB-C** on the AtomS3R side so it sits flush in the channel.
2. **Data (Grove):** AtomS3R Grove port (**G2 / GPIO2 = LED data**, +5 V, GND) →
   Grove cable → first **M5 Puzzle 8×8** → chained to the second Puzzle.
   `NUM_LEDS = 128` in firmware. Keep **LED brightness low** — high brightness
   browns out the Grove 5 V and the bars flicker (known from the helmet build).

Cable exits: the battery pocket has a `bat_port` window for the pack's USB-C OUT;
the AtomS3R sits at the wrist end with a side notch for its pigtails; the two
channels keep the power run and the Grove run on opposite sides so they don't fight.
Optional `strain` posts give you something to zip-tie against.

## ⚠️ Firmware notes for the wrist orientation
- **IMU auto-orient was tuned for the helmet base** (gravity on `ay ≈ +0.98`).
  On the forearm the unit lies roughly flat (gravity mostly on `az`), so the
  auto-flip can misfire. Recommend: **pin the orientation** for the wrist (set
  `gFlip`/`gLedFlip`/`gScreenRot` to fixed values and skip the accel flip block),
  or re-tune the thresholds for a horizontal mount. TODO before first ride.
- **Keep-alive:** the power bank auto-offs at low draw — the firmware already
  pulses to keep it awake. Confirm your specific pack stays on with the AtomS3R
  idle current; if it drops, raise the pulse.
- Button (page cycle / brightness long-press) stays reachable through the bezel
  hole — good, since the board keyboard isn't reachable while riding.

## Mount
Two 25 mm hook-and-loop straps through the tray slots, wrapping the forearm.
Add thin foam on the saddle for grip + comfort. Breakaway is inherent (straps
pull free). Don't route any cable to the board/VESC — power is 100% self-contained
on the arm.
