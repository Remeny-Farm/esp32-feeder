// auger.scad - dosing auger for the chicken treat feeder (first printable design)
//
// License: CERN-OHL-P-2.0 (see LICENSE-HARDWARE at the repository root).
// Written for OpenSCAD 2021.01 (no manifold-only syntax). Units: mm.
//
// Export one part at a time:
//   openscad -D 'part="auger"' -o auger.stl hardware/cad/auger.scad
//   openscad -D 'part="tube"' -D 'preset="pellet"' -o tube.stl hardware/cad/auger.scad
//
// Parts (value of `part`):
//   "auger"        screw with a NEMA 17 D-shaft socket and an M3 grub screw
//   "tube"         auger housing: motor flange, top inlet, bottom outlet at the far end
//   "hopper"       small funnel hopper (about 0.5 L) that sits on the tube inlet
//   "motor_mount"  NEMA 17 face plate between the motor and the tube flange
//   "assembly"     all parts in place, for a look in the preview only (never export it)
//
// Axis frames:
//   Each part is modelled in its PRINT orientation, except where a comment says otherwise.
//   Tube: axis = +Z, flange on the bed (Z = 0), inlet on the -X side, outlet on the +X side.
//   Mounted, the tube lies on its side: the -X side is the top (inlet), +X is the bottom.
//
// Assembly order and fixing:
//   motor | motor_mount | tube flange. ONE set of four M3 screws goes through the tube
//   flange and the plate into the motor (31 mm pattern). Screw length is about
//   flange_t + plate_t + 4 mm of motor thread = M3 x 12 with the defaults.
//   Check the thread depth of your motor before you pick the length.

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

part   = "auger";    // "auger" | "tube" | "hopper" | "motor_mount" | "assembly"
preset = "grain";    // "grain" | "pellet" | "mealworm" - picks the feed-dependent values below

$fn = 64;            // circle facets. Use 96 or more for a final print, 32 for a quick preview.

// Feed presets. Each line is pick(grain, pellet, mealworm).
// Any value can still be overridden on the command line with -D name=value.
function pick(g, p, m) =
    preset == "grain" ? g :
    preset == "pellet" ? p :
    preset == "mealworm" ? m : g;

assert(preset == "grain" || preset == "pellet" || preset == "mealworm",
       "preset must be grain, pellet or mealworm");

// --- Auger ---------------------------------------------------------------
auger_od     = pick(25, 25, 28);     // outer diameter of the flight (mm)
pitch        = pick(20, 22, 28);     // axial advance per turn (mm). One turn moves one pocket of feed.
flight_t     = pick(2.0, 2.0, 2.4);  // flight thickness, measured along the axis (mm)
core_d       = 10;                   // core (root) diameter (mm). Wide pockets need a thin core.
flighted_len = 80;                   // length of the flighted section (mm)
right_hand   = true;                 // true = right-hand screw (like a normal bolt)
// Direction: with a right-hand screw, feed moves AWAY from the motor when the shaft turns
// counter-clockwise seen from the motor looking at the auger (like a drill bit: it pulls
// swarf toward the chuck when it turns clockwise). Check it at the bench with a few grains.
// If feed moves the wrong way, reverse the motor or set right_hand = false.

// Clearance between the flight tip and the tube bore, per side (mm).
// - A small gap (about 1 mm) is below the size of whole grain kernels, so a whole kernel
//   cannot enter the gap. Only dust and cracked bits pass.
// - A gap near the size of a pellet can trap and crush it between the flight tip and the wall.
//   Pellets (4-8 mm) therefore get 1.5 mm and a slightly larger pitch (less shear per turn).
// - Dried mealworms are soft and about 3 mm thick, so they do not wedge. Their problem is
//   bridging, so the pitch is larger (wide pockets) and the gap is 1.5 mm.
// These are starting values. The bench test (hardware/bench/auger-calibration.md) decides.
clearance    = pick(1.0, 1.5, 1.5);

// --- NEMA 17 D-shaft socket (in the auger hub) ------------------------------
shaft_d      = 5;      // motor shaft diameter (mm)
shaft_flat   = 4.5;    // D-shaft width across the flat: round side to flat (mm)
shaft_tol    = 0.15;   // extra clearance on the socket (mm). Printed holes shrink: test fit, adjust.
shaft_len    = 24;     // LONGEST shaft length you expect, from the motor body face (mm). Measure it.
hub_gap      = 1.0;    // gap between the hub and the plate face (mm). Set it with a 1 mm shim.
hub_d        = 16;     // hub diameter (mm). Needs room for the M3 nut trap.
hub_cap      = 2.5;    // solid plastic above the blind socket (mm)
grub_d       = 3.2;    // M3 grub screw clearance hole (mm)
nut_af       = 5.5;    // M3 nut across flats (mm)
nut_tol      = 0.2;    // nut pocket clearance (mm)

// --- Tube (auger housing) -----------------------------------------------------
wall         = 2.4;    // tube wall thickness (mm)
end_gap      = 1.0;    // gap between the last flight and the end wall (mm)
end_wall     = 3.0;    // thickness of the closed far end (mm)
flange_t     = 4.0;    // motor flange thickness (mm)
inlet_len    = pick(24, 24, 30);   // inlet window length along the axis (mm)
inlet_w      = pick(18, 18, 22);   // inlet window width across the tube (mm)
outlet_len   = pick(20, 22, 30);   // outlet window length along the axis (mm), at the far end
outlet_w     = pick(14, 16, 20);   // outlet window width across the tube (mm)

// --- NEMA 17 motor and face plate ----------------------------------------------
nema_w       = 42.3;   // motor face, square (mm)
nema_hole    = 31;     // hole pattern, square (mm)
m3_d         = 3.4;    // M3 clearance hole (mm)
boss_d       = 22;     // motor centering boss diameter (mm)
boss_tol     = 0.4;    // boss pocket clearance (mm)
boss_h       = 2.0;    // motor boss height (mm)
plate_t      = 4.0;    // face plate thickness (mm)
shaft_hole_d = 8;      // hole for the shaft in the plate front wall (mm)

// --- Hopper -----------------------------------------------------------------------
hopper_l     = 110;    // inside opening along the tube axis (mm)
hopper_w     = 110;    // inside opening across the tube (mm)
funnel_h     = 80;     // height of the sloped part (mm)
rim_h        = 10;     // straight rim above the slope (mm)
hopper_wall  = 1.6;    // hopper wall thickness (mm)
foot         = 4;      // width of the seat ring around the inlet window (mm)
slot_clear   = 0.3;    // clearance between the hopper tongue and the inlet window, per side (mm)

// ---------------------------------------------------------------------------
// Derived values (do not edit)
// ---------------------------------------------------------------------------

eps = 0.01;

bore_d      = auger_od + 2 * clearance;           // tube inside diameter
tube_r_out  = bore_d / 2 + wall;                  // tube outside radius
socket_depth = shaft_len - plate_t - hub_gap;     // depth of the blind socket in the hub
cone_h      = (auger_od - hub_d) / 2;             // 45 degree cone from the hub to the flight diameter
hub_len     = socket_depth + hub_cap;             // total hub length
z_flights   = hub_gap + hub_len;                  // where the flights start, in tube coordinates
bore_len    = z_flights + flighted_len + end_gap; // inside length of the tube
tube_len    = bore_len + end_wall;                // total tube length
inlet_z0    = z_flights;                          // inlet starts where the flights start
outlet_z0   = bore_len - outlet_len;              // outlet ends at the end wall

assert(hub_d < auger_od - 2, "hub_d must be smaller than auger_od");
assert(core_d < auger_od - 4, "core_d is too large for this outer diameter");
assert(360 * flight_t / pitch < 110, "flight_t is too thick for this pitch");
assert(clearance >= 0.5, "clearance below 0.5 mm will rub on a printed bore");
assert(tube_r_out <= 18.5, "tube too wide: the M3 screw heads at the flange corners would hit it");
assert(outlet_z0 > inlet_z0 + inlet_len, "inlet and outlet windows overlap: shorten them or lengthen flighted_len");
assert(hub_len > cone_h + hub_cap, "hub too short for its 45 degree cone: reduce auger_od or increase shaft_len");

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Rounded square, centred, 2D.
module rounded_square(size, r) {
    offset(r = r) square(size - 2 * r, center = true);
}

// Box centred on X and Y, standing on Z = 0.
module slab(l, w, h) {
    translate([-l / 2, -w / 2, 0]) cube([l, w, h]);
}

// The four motor screw positions (31 mm square pattern).
module nema_holes(h) {
    for (sx = [-1, 1], sy = [-1, 1])
        translate([sx * nema_hole / 2, sy * nema_hole / 2, -eps])
            cylinder(d = m3_d, h = h + 2 * eps, $fn = 24);
}

// ---------------------------------------------------------------------------
// PART: auger
// Print orientation: upright, axis vertical, the socket end on the bed. No brim needed.
//   - The socket opens on the bed. A first-layer "elephant foot" can narrow it: check the fit
//     and ream with a 5 mm drill bit if needed.
//   - The hub widens to the flight diameter on a 45 degree cone, so the first flight has a base
//     and nothing hangs in the air.
//   - The flight underside is a shallow helical ramp. It prints without supports on a Prusa MK3
//     at 0.15 or 0.20 mm layers, but the underside can look rough or sag a little.
//     If it sags, enable support enforcers or print with 0.15 mm layers.
//   - The M3 nut pocket is a horizontal hexagon with a corner on top, so it prints
//     without support.
// ---------------------------------------------------------------------------

// Cross-section of one flight at its start: a wedge from the core out to the flight diameter.
// The wedge angle is the share of one turn that the flight thickness takes up.
module flight_section() {
    r_out = auger_od / 2;
    fl_a  = 360 * flight_t / pitch;
    intersection() {
        difference() {
            circle(r = r_out);
            circle(r = core_d / 2 - 0.5);   // the flight overlaps the core by 0.5 mm for a good weld
        }
        // Triangle that covers the wedge (valid while fl_a < 120 degrees).
        polygon([[0, 0], [2 * r_out, 0], [2 * r_out * cos(fl_a), 2 * r_out * sin(fl_a)]]);
    }
}

// 2D D-shaped hole for the motor shaft. The flat faces +X.
module d_socket_2d() {
    flat_x = shaft_flat - shaft_d / 2 + shaft_tol / 2;
    intersection() {
        circle(d = shaft_d + shaft_tol, $fn = 48);
        translate([-shaft_d, -shaft_d]) square([shaft_d + flat_x, 2 * shaft_d]);
    }
}

// Blind D socket, grub screw hole and nut pocket. The grub screw presses on the flat (+X side).
module hub_cuts() {
    flat_x = shaft_flat - shaft_d / 2 + shaft_tol / 2;
    grub_z = socket_depth / 2;
    nut_d  = (nut_af + nut_tol) / cos(30);    // across corners
    // socket
    translate([0, 0, -eps]) linear_extrude(height = socket_depth + eps) d_socket_2d();
    // grub screw hole from the flat to the hub surface
    translate([0, 0, grub_z]) rotate([0, 90, 0]) cylinder(d = grub_d, h = hub_d, $fn = 24);
    // nut pocket, open to the +X side. A 1.6 mm wall stays between the nut and the shaft flat.
    translate([flat_x + 1.6, 0, grub_z]) rotate([0, 90, 0]) cylinder(d = nut_d, h = hub_d, $fn = 6);
}

module auger() {
    turns = flighted_len / pitch;
    twist = (right_hand ? -1 : 1) * 360 * turns;   // OpenSCAD: a negative twist gives a right-hand helix
    difference() {
        union() {
            // hub (cylinder, then the 45 degree cone up to the flight diameter)
            cylinder(d = hub_d, h = hub_len - cone_h);
            translate([0, 0, hub_len - cone_h - eps]) cylinder(d1 = hub_d, d2 = auger_od, h = cone_h + eps);
            // core
            translate([0, 0, hub_len - eps]) cylinder(d = core_d, h = flighted_len + eps);
            // flight
            translate([0, 0, hub_len - eps])
                linear_extrude(height = flighted_len + eps, twist = twist,
                               slices = max(16, ceil(turns * 48)), convexity = 10)
                    flight_section();
        }
        hub_cuts();
    }
}

// ---------------------------------------------------------------------------
// PART: tube
// Print orientation: upright, axis vertical, flange on the bed.
//   - Walls, flange and screw holes are vertical: no support.
//   - The closed far end is a bridge of one bore diameter (27 mm with the defaults). The MK3
//     bridges this with default settings and part cooling. The underside will be a little
//     rough. It faces the auger, so sand it if a flight catches.
//   - The inlet window roof is a bridge of inlet_w (18 mm). Same remark.
//   - The outlet window reaches up to the end wall, so its roof is the end wall itself.
// Flange: M3 screw heads sit on its front face, in the corners, beside the tube.
// ---------------------------------------------------------------------------

module tube() {
    difference() {
        union() {
            linear_extrude(height = flange_t) rounded_square(nema_w, 3);
            cylinder(r = tube_r_out, h = tube_len);
        }
        // bore
        translate([0, 0, -eps]) cylinder(d = bore_d, h = bore_len + eps);
        // motor screws
        nema_holes(flange_t);
        // inlet window on the -X side (top when mounted)
        translate([-(tube_r_out + 1), -inlet_w / 2, inlet_z0])
            cube([tube_r_out + 1, inlet_w, inlet_len]);
        // outlet window on the +X side (bottom when mounted), at the far end
        translate([0, -outlet_w / 2, outlet_z0])
            cube([tube_r_out + 1, outlet_w, outlet_len]);
    }
}

// ---------------------------------------------------------------------------
// PART: motor_mount
// Print orientation: flat. The tube-side face is on the bed, the motor pocket faces up.
//   - No support, no brim needed. 4 perimeters, 30% infill.
// Fixing: the plate has no screws of its own. The four M3 screws that hold the motor also hold
// the tube flange (see the header). The plate front wall closes the back of the tube.
// The shaft passes through a hole in that wall. A little dust can reach the boss: no seal in v1.
// ---------------------------------------------------------------------------

module motor_mount() {
    pocket = boss_h + 0.2;    // the front wall keeps plate_t - pocket = 1.8 mm with the defaults
    difference() {
        linear_extrude(height = plate_t) rounded_square(nema_w, 3);
        // shaft hole
        translate([0, 0, -eps]) cylinder(d = shaft_hole_d, h = plate_t + 2 * eps);
        // pocket for the motor boss, on the motor side
        translate([0, 0, plate_t - pocket]) cylinder(d = boss_d + boss_tol, h = pocket + eps);
        // motor and tube screws
        nema_holes(plate_t);
    }
}

// ---------------------------------------------------------------------------
// PART: hopper
// Modelled in its mounted position (tube axis along X, tube centre at Z = -tube_r_out, the top
// of the tube at Z = 0). The "hopper" part flips it for printing.
// Print orientation: UPSIDE DOWN, the wide rim on the bed, the spout pointing up.
//   - The walls lean inward as they rise (about 30 degrees from vertical): no support.
//   - The seat ring at the spout is cut to the outside curve of the tube. Printed upside down
//     this is an open trough, so it needs no support.
//   - Use a 5 mm brim: the thin rim is large and can lift at the corners.
// The tongue fits in the tube inlet window. Fix the hopper with a drop of hot glue, or with two
// zip ties round the tube and the seat ring. v1 has no clip.
// The slope is about 60 degrees from horizontal. Grain and pellets flow. Dried mealworms may
// bridge: tap the hopper or add a stirrer later.
// ---------------------------------------------------------------------------

// Volume of the sloped part between two rectangles (prismatoid formula), in mL.
function frustum_ml(a1, b1, a2, b2, h) = h / 6 * (2 * a1 * b1 + a1 * b2 + a2 * b1 + 2 * a2 * b2) / 1000;

module hopper_body() {
    tongue_d = wall;                                   // the tongue ends flush with the top of the bore
    sp_o_l = inlet_len - 2 * slot_clear;               // tongue outside
    sp_o_w = inlet_w - 2 * slot_clear;
    sp_i_l = sp_o_l - 2 * hopper_wall;                 // channel inside
    sp_i_w = sp_o_w - 2 * hopper_wall;
    foot_l = inlet_len + 2 * foot;                     // seat ring outside
    foot_w = inlet_w + 2 * foot;
    top_o_l = hopper_l + 2 * hopper_wall;              // top outside
    top_o_w = hopper_w + 2 * hopper_wall;
    // depth of the seat ring below the tube top, so the ring reaches the curved tube at its edges
    foot_d = tube_r_out - sqrt(tube_r_out * tube_r_out - foot_w * foot_w / 4) + 1;

    echo(str("hopper volume (mL, sloped part + rim): ",
             frustum_ml(sp_i_l, sp_i_w, hopper_l, hopper_w, funnel_h) + hopper_l * hopper_w * rim_h / 1000));

    difference() {
        union() {
            difference() {
                union() {
                    translate([0, 0, -foot_d]) slab(foot_l, foot_w, foot_d + eps);
                    hull() {
                        slab(foot_l, foot_w, eps);
                        translate([0, 0, funnel_h]) slab(top_o_l, top_o_w, eps);
                    }
                    translate([0, 0, funnel_h]) slab(top_o_l, top_o_w, rim_h);
                }
                // saddle: the outside of the tube
                translate([0, 0, -tube_r_out]) rotate([0, 90, 0]) cylinder(r = tube_r_out, h = foot_l + 10, center = true);
            }
            // tongue into the inlet window
            translate([0, 0, -tongue_d]) slab(sp_o_l, sp_o_w, tongue_d + 1);
        }
        // channel through the tongue
        translate([0, 0, -tongue_d - 1]) slab(sp_i_l, sp_i_w, tongue_d + 2);
        // sloped inside
        hull() {
            slab(sp_i_l, sp_i_w, eps);
            translate([0, 0, funnel_h]) slab(hopper_l, hopper_w, eps);
        }
        // straight inside of the rim
        translate([0, 0, funnel_h]) slab(hopper_l, hopper_w, rim_h + 1);
    }
}

module hopper() {
    translate([0, 0, funnel_h + rim_h]) rotate([180, 0, 0]) hopper_body();
}

// ---------------------------------------------------------------------------
// PART: assembly (preview only)
// Mounted frame: tube axis along +X, motor at X < 0, Z up. Look at it with F5, do not export.
// ---------------------------------------------------------------------------

module assembly() {
    color("silver") rotate([0, -90, 0]) motor_mount();
    color("khaki") rotate([0, 90, 0]) tube();
    color("orange") translate([hub_gap, 0, 0]) rotate([0, 90, 0]) auger();
    color("seagreen") translate([inlet_z0 + inlet_len / 2, 0, tube_r_out]) hopper_body();
}

// ---------------------------------------------------------------------------
// Part selector
// ---------------------------------------------------------------------------

if (part == "auger") auger();
else if (part == "tube") tube();
else if (part == "hopper") hopper();
else if (part == "motor_mount") motor_mount();
else if (part == "assembly") assembly();
else echo(str("Unknown part '", part, "'. Use auger, tube, hopper, motor_mount or assembly."));
