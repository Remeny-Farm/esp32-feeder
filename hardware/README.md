# hardware/

Mechanical design and bench tests for the chicken treat feeder.
The first design is the dosing auger: one auger, its tube, a small hopper and a
NEMA 17 face plate.

| Path | Content |
|---|---|
| `cad/auger.scad` | Parametric OpenSCAD model: `auger`, `tube`, `hopper`, `motor_mount` (and `assembly` for a preview). Feed presets: `grain`, `pellet`, `mealworm`. |
| `bench/auger-calibration.md` | Bench test for grams per revolution, with the Prusa i3 MK3 as the motor driver. |

Print orientation, parameters and units are in the comments at the top of
`cad/auger.scad`. All lengths are in mm.

## Export an STL

Install [OpenSCAD](https://openscad.org) 2021.01 or newer. Run these from the
repository root. Pick one `part` and one `preset` per run.

```sh
openscad -D 'part="auger"'       -o auger.stl       hardware/cad/auger.scad
openscad -D 'part="tube"'        -o tube.stl        hardware/cad/auger.scad
openscad -D 'part="hopper"'      -o hopper.stl      hardware/cad/auger.scad
openscad -D 'part="motor_mount"' -o motor_mount.stl hardware/cad/auger.scad
```

Choose the feed type with `-D 'preset="pellet"'` (or `"grain"`, `"mealworm"`).
Any parameter can be overridden the same way, for example `-D 'clearance=1.5'`.
Print the auger, the tube and the hopper from the same preset.
For a final print add `-D '$fn=96'`.

## License

Hardware files in this directory are released under
[CERN-OHL-P-2.0](../LICENSE-HARDWARE).
