// ============================================================================
//  Helmet HUD mount — Bell Super Air
//  Two printed parts joined by a BENDABLE ALUMINIUM WIRE (the boom).
//    • bracket  → shares the visor pivot screw (no drilling)
//    • cradle   → holds 2x M5 Puzzle 8x8 + a sun hood
//  The wire = adjustable position AND breakaway (bends/pulls out in a crash).
//
//  Usage:  open in OpenSCAD → set the MEASURE params → set `part` →
//          F5 preview, F6 render, export STL.  Print each part separately.
//
//  Print:  PETG (sun/heat resistant + tough; PLA gets brittle/soft outdoors),
//          0.2 mm layer, 4 perimeters, 30% infill, brim on.  Bambu Studio: use
//          the Generic PETG profile.  No supports needed (parts print flat).
// ============================================================================

part = "cradle";          // "bracket"  or  "cradle"  (export one at a time)

/* ---------- MEASURE THESE (caliper) ---------- */
visor_screw_d = 4.0;      // Bell visor pivot screw shaft diameter
wire_d        = 3.2;      // your bendable boom wire diameter + ~0.2 clearance

/* ---------- panel = 2x M5 Puzzle 8x8 (24x24x15 each) ---------- */
panel_w = 48.6;           // 2 x 24 + tolerance
panel_h = 24.6;
panel_t = 15.0;
lip     = 2.5;            // front retaining lip (holds panel, LEDs show through)
grove_w = 13;             // side openings for the Grove cables

wall = 2.4;
$fn  = 64;

// friction tube that grips the bendable wire (flex slit lets it pinch)
module wire_tube(len = 18){
  difference(){
    cylinder(d = wire_d + 2*wall, h = len);
    translate([0,0,-1]) cylinder(d = wire_d, h = len + 2);
    translate([-wire_d/4, 0, -1]) cube([wire_d/2, wire_d, len + 2]);   // slit
  }
}

/* Part A — visor-pivot bracket */
module bracket(){
  pl = 22; arm = 16; t = 4;
  difference(){
    union(){
      hull(){
        cylinder(d = pl, h = t);
        translate([arm,0,0]) cylinder(d = 11, h = t);
      }
      translate([arm,0,t]) rotate([0,90,0]) wire_tube(18);   // boom socket
    }
    translate([0,0,-1]) cylinder(d = visor_screw_d, h = t + 2);        // screw hole
    translate([0,0,t-1.8]) cylinder(d = visor_screw_d + 3.5, h = 2);   // head recess
  }
}

/* Part B — Puzzle cradle + sun hood + boom socket */
module cradle(){
  ow = panel_w + 2*wall; oh = panel_h + 2*wall; d = panel_t + wall;
  difference(){
    cube([ow, oh, d]);
    translate([wall, wall, wall]) cube([panel_w, panel_h, d]);                 // pocket (open back)
    translate([wall+lip, wall+lip, -1]) cube([panel_w-2*lip, panel_h-2*lip, wall+2]); // window
    translate([-1, oh/2 - grove_w/2, wall]) cube([ow+2, grove_w, d]);          // grove slots
  }
  // sun hood over the top edge (shades the LEDs, kills glare)
  translate([0, oh - wall, 0]) cube([ow, wall, d + 10]);
  translate([0, oh - wall, d + 9]) rotate([30,0,0]) cube([ow, wall, 16]);
  // boom socket on the back, centered
  translate([ow/2, oh/2, d]) wire_tube(16);
}

if (part == "bracket") bracket();
else                    cradle();
