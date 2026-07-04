// ============================================================================
//  Wrist / forearm HUD bracer  —  VESC AI Tuner
//  A curved forearm cradle that carries BOTH:
//    • the HUD set  (AtomS3R 24x24 + 2x M5 Puzzle 8x8 landscape 48x24)  near the wrist
//    • the battery  (USB-C power bank / flat LiPo)                      near the elbow
//  joined by an internal CABLE CHANNEL (USB-C power + Grove data).
//
//  Why a forearm bracer (not a wristwatch): the power bank + Puzzle panel are
//  too big/heavy for the wrist. On the forearm the pack lays flat, the HUD sits
//  by the wrist tilted toward your face — a natural glance while riding, no
//  helmet combiner optics needed. Read-only telemetry (ESP-NOW), same firmware
//  as the helmet HUD [[helmet-hud-concept]].
//
//  Usage:  open in OpenSCAD → caliper the MEASURE params → set `part` →
//          F5 preview, F6 render, export STL.  Print each part separately.
//
//  Print:  PETG (tough + heat/sun stable; PLA warps/softens on skin+sun),
//          0.2 mm layer, 4 perimeters, 25-30% infill, brim on. No supports for
//          the tray if printed cavity-up; bezel + lid print flat.
// ============================================================================

part = "tray";        // "tray" | "bezel" | "battery_lid"   (export one at a time)

/* ================= MEASURE THESE (caliper) ================= */
arm_dia   = 62;       // forearm diameter where you'll wear it (across) -> saddle radius
// --- AtomS3R (the HUD "brain": ESP-NOW rx + IMU + 0.85" screen + top button) ---
atom_w    = 24.4;     // + a hair clearance
atom_h    = 24.4;
atom_t    = 18.0;     // body depth (into the tray)
atom_win  = 15.0;     // screen window (square-ish over the 0.85")
atom_btn  = 8.0;      // top button access hole
// --- panel = 2x M5 Puzzle 8x8, mounted LANDSCAPE 16w x 8h (24x24x15 each) ---
puz_w     = 48.6;     // 2 x 24 + tol  (this runs ACROSS the forearm)
puz_h     = 24.6;     // (this runs ALONG the forearm)
puz_t     = 15.0;
puz_lip   = 2.5;      // front retaining lip so LEDs show but panel can't fall out
// --- battery: default = slim USB-C power bank (Anker Nano 5K ~ MEASURE!). ---
//     Lighter alt (recommended for wrist): flat LiPo 103450 ~ 52 x 34 x 11.
bat_l     = 102;      // along the forearm
bat_w     = 48;       // across
bat_t     = 12;       // thickness (pocket depth)
bat_port  = 14;       // opening for the pack's USB-C OUT port (on the wrist-side end)

/* ================= WIRING ================= */
usbc_ch   = 9;        // width of the recessed USB-C power run (pack -> AtomS3R)
grove_ch  = 13;       // width of the Grove data run (AtomS3R G2 -> Puzzle chain)
strain    = true;     // add little strain-relief posts flanking cable exits

/* ================= STRAPS ================= */
strap_w   = 25;       // hook-and-loop webbing width
strap_t   = 3.0;      // webbing thickness (slot height)

/* ================= build constants ================= */
wall   = 2.6;
gap    = 8;           // dead space between HUD block and battery pocket (for the cable bend)
$fn    = 72;
eps    = 0.01;

R      = arm_dia/2;                          // saddle radius
W      = max(puz_w, bat_w, atom_w) + 2*wall; // tray width (across arm)
hud_L  = puz_h + atom_h + 3*wall;            // HUD block length (Puzzle then AtomS3R)
L      = hud_L + gap + bat_l + 2*wall;       // total length (wrist -> elbow)
edge_h = R - sqrt(max(R*R - (W/2)*(W/2), 1));// how high the saddle rails rise at the edges
top_z  = edge_h + max(puz_t, atom_t, bat_t) + wall;   // flat top surface height

// concave underside that hugs the forearm (saddle floor centered at z=0)
module arm_saddle(extra=0){
  translate([-1, W/2, -R])
    rotate([0,90,0])
      cylinder(r = R + extra, h = L + 2);
}

// horizontal webbing slot through both side rails at X=x
module strap_slot(x){
  translate([x - strap_w/2, -1, 2.5])
    cube([strap_w, W + 2, strap_t]);
}

/* ---------------- Part A: the bracer tray ---------------- */
module tray(){
  // X layout (0 = wrist end): [AtomS3R][Puzzle] | gap | [battery ....] elbow
  atom_x0 = wall;
  puz_x0  = atom_x0 + atom_h + wall;
  bat_x0  = hud_L + gap;

  difference(){
    // solid body
    cube([L, W, top_z]);

    // 1) forearm saddle (concave underside + cradling rails)
    arm_saddle();

    // 2) AtomS3R pocket (open top) + screen already faces up
    translate([atom_x0, (W-atom_w)/2, top_z - atom_t])
      cube([atom_h, atom_w, atom_t + eps]);

    // 3) Puzzle pocket (open top)
    translate([puz_x0, (W-puz_w)/2, top_z - puz_t])
      cube([puz_h, puz_w, puz_t + eps]);

    // 4) battery pocket (open top)
    translate([bat_x0, (W-bat_w)/2, top_z - bat_t])
      cube([bat_l, bat_w, bat_t + eps]);

    // 5) battery USB-C OUT port window (wrist-side end wall of the pack)
    translate([bat_x0 - wall - 1, W/2 - bat_port/2, top_z - bat_t + 1])
      cube([wall + 2, bat_port, bat_t]);

    // 6) CABLE CHANNEL — power (USB-C) run: battery -> AtomS3R, offset one side
    translate([atom_x0, W/2 - usbc_ch - 2, top_z - 6])
      cube([bat_x0 + 4, usbc_ch, 7]);
    // 7) CABLE CHANNEL — Grove data run: AtomS3R -> Puzzle, other side
    translate([atom_x0, W/2 + 2, top_z - 6])
      cube([puz_x0 + puz_h, grove_ch, 7]);

    // 8) AtomS3R side notch so its own USB-C / Grove pigtail can exit
    translate([atom_x0 + atom_h/2 - 6, -1, top_z - 8])
      cube([12, W + 2, 9]);

    // 9) two strap slots (near wrist, near elbow)
    strap_slot(hud_L * 0.55);
    strap_slot(L - bat_l*0.35);
  }

  // strain-relief posts flanking the channel mouth at the battery end
  if (strain){
    for (s = [-1, 1])
      translate([bat_x0 - 3, W/2 + s*(usbc_ch/2 + 2), top_z - 6])
        cylinder(d = 2.4, h = 6);
  }
}

/* ---------------- Part B: snap bezel over the HUD ---------------- */
// thin frame that clips down over AtomS3R + Puzzle to retain them, with windows.
module bezel(){
  bz_L = hud_L; bz_t = 2.4; clip = 4;
  difference(){
    union(){
      cube([bz_L, W, bz_t]);
      // side clips that grip the tray rails
      for (s=[0,1]) translate([0, s*(W-wall), -clip]) cube([bz_L, wall, clip+bz_t]);
    }
    // AtomS3R screen window + button hole
    translate([wall + atom_h/2, W/2, -1]) cube([atom_win, atom_win, bz_t+2], center=true);
    translate([wall + atom_h/2 + atom_win/2 + atom_btn/2, W/2, -1])
      cylinder(d=atom_btn, h=bz_t+2);
    // Puzzle LED window (inside the retaining lip)
    translate([atom_h + wall + puz_lip, (W-puz_w)/2 + puz_lip, -1])
      cube([puz_h - 2*puz_lip, puz_w - 2*puz_lip, bz_t+2]);
  }
}

/* ---------------- Part C: battery lid ---------------- */
// light cover so the pack can't pop out; still held down by the elbow strap.
module battery_lid(){
  ld_L = bat_l + 2*wall; ld_W = bat_w + 2*wall; ld_t = 2.4;
  difference(){
    union(){
      cube([ld_L, ld_W, ld_t]);
      for (s=[0,1]) translate([0, s*(ld_W-wall), -4]) cube([ld_L, wall, 4+ld_t]);
    }
    // vent slots (heat + weight)
    for (i=[1:4]) translate([ld_L*i/5 - 3, ld_W*0.2, -1]) cube([6, ld_W*0.6, ld_t+2]);
    // strap slot passthrough
    translate([ld_L/2 - strap_w/2, -1, -1]) cube([strap_w, ld_W+2, ld_t+2]);
  }
}

if      (part == "tray")        tray();
else if (part == "bezel")       bezel();
else if (part == "battery_lid") battery_lid();
