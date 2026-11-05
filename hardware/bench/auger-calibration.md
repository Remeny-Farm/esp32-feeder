# Auger calibration bench test (grams per revolution)

This test measures how many grams of feed one auger revolution moves. It uses the
intact Prusa i3 MK3 as the motor driver. It needs no new code and no new wiring,
except moving one motor cable.

Why the Prusa and not the stock driver: the Adafruit TB6612 FeatherWing has no
current limiting, and the Prusa NEMA 17 motors have low winding resistance, so the
TB6612 must not drive them at 12 V. The Einsy TMC2130 drivers limit the current.

Result of this test: one grams-per-revolution number per feed type and speed. The
firmware will use it later, together with the daily gram budget.

## What you need

- Printed parts from `hardware/cad/auger.scad`: `auger`, `tube`, `motor_mount`,
  `hopper`. Use the preset that matches the feed (`grain`, `pellet`, `mealworm`).
- 4 x M3 screws, about 12 mm long, and 1 M3 grub screw and 1 M3 nut for the auger.
- A NEMA 17 motor with a plain 5 mm D-shaft. Use a spare motor or the E motor.
  The Z motors use a lead screw as the shaft, so they do not fit the D socket.
- The intact Prusa i3 MK3 with its power supply, and a USB cable to a computer.
- A kitchen scale, 0.1 g if possible, and a cup or tray to catch the feed.
- Dry feed of each type: grain, pellets, dried mealworms. Use the real feed.
- A serial terminal: Pronterface, or `screen` in a shell.

## Safety

- Switch the printer off and unplug the motor before you touch a motor cable.
  Never plug or unplug a motor with the power on. It can destroy the driver.
- Keep fingers and hair away from the auger while it turns.
- The hotend stays cold. If you must heat it (see step 8), do not touch the nozzle.

## Steps

1. Print the parts. Follow the print orientation in the comments of `auger.scad`.
   Test the D socket on the motor shaft. If it is tight, ream it with a 5 mm drill.
2. Insert the M3 nut in the pocket on the hub. Do not tighten the grub screw yet.
3. Bolt the stack together in this order: motor, `motor_mount`, `tube` flange.
   One set of four M3 screws holds all three.
4. Slide the auger on the motor shaft. Put a 1 mm shim between the hub and the plate.
   Align the grub screw with the shaft flat. Tighten the grub screw.
5. Turn the auger by hand. It must turn freely, with no rubbing on the tube.
6. Put the tube so the outlet points down, over the scale tray. Fit the hopper on the
   inlet. Do not fill it yet.
7. Switch the printer off. Unplug the motor you chose from the Einsy.
   Plug the bench motor into the **E motor** port. Leave everything else as it is.
8. Switch the printer on. Connect over USB at 115200 baud.
   - `screen /dev/tty.usbmodemXXXX 115200` (macOS). Quit with Ctrl-A, then K.
   - Or Pronterface: connect at 115200.
9. Allow cold extrusion. Send `M302 S0`.
   - Check that the firmware accepts it. Prusa firmware is not the same as Marlin.
     The reply must be `ok`. If it says `Unknown command`, or later it says
     `cold extrusion prevented`, heat the nozzle to its minimum extrusion
     temperature instead (for example `M104 S200`). Switch the heater off after the test.
10. Read the steps per mm. Send `M503`. Find the `M92` line. Read the `E` value.
    - The Prusa MK3 default is E280. The E driver runs at 1/32 microstepping.
      That gives 200 x 32 = 6400 steps per revolution.
    - Compute `mm_per_rev = 6400 / E`. With E280 this is 22.86 mm per revolution.
    - Confirm these numbers on your printer. The microstep setting is not always in
      the `M503` output. Check them in step 12.
11. Set relative moves. Send `G91`, then `M83`. Both are needed in some firmware
    versions, so send both.
12. Check the revolution count. Take the hopper off. Make a pen mark on a flight that
    you can see through the inlet window. Send `G1 E22.86 F300` (use your `mm_per_rev`).
    The mark must come back to the same place after one revolution. If it does not,
    fix `mm_per_rev` before you go on.
13. Pick the speeds. The feed rate is `F = rpm x mm_per_rev` (mm/min).
    Use three speeds, for example 30, 60 and 120 rpm.
    With 22.86 mm per revolution: F686, F1372 and F2743.
14. Fit the hopper again. Fill it to a fixed mark. Use the same level at the start of every run.
15. Prime the auger. Run 5 revolutions and throw the feed away.
    Example: `G1 E114.3 F1372`. Check the direction: the feed must move toward the outlet.
    If it moves the wrong way, use a negative E value for all runs, or turn the motor
    plug around.
16. Run one measured test.
    1. Empty the tray and tare the scale.
    2. Send `G1 E<N x mm_per_rev> F<feed rate>`. Use N = 10 revolutions.
       Example for 60 rpm: `G1 E228.6 F1372`.
    3. Send `M400`. The `ok` for `M400` arrives when the move has finished.
    4. Weigh the feed in the tray at once. This is the **grams** value.
    5. Wait 10 seconds and weigh again. The extra weight is the **dribble** after the stop
       (see "What to watch").
    6. Write the result in the table.
    7. If the firmware says `Too long extrusion prevented`, use fewer revolutions
       and write the new N in the table.
17. Repeat step 16 five times for each speed. Refill the hopper to the mark between runs.
18. Repeat steps 14 to 17 for each feed type. Use the matching preset parts.
    Print a new auger and tube for each preset.
19. Switch the printer off. Unplug the bench motor. Plug the original motor back in.

If the motor stalls at 120 rpm, do not raise the current at first. Lower the speed.
If you must raise it, check the motor label, and set it with `M907 E<mA>`. Check that
your firmware has this command. Never exceed the rated motor current.

## Calculations

- `g/rev = grams / N`
- `mean = average of the 5 g/rev values`
- `spread = (max - min) / mean`, as a percentage

Aim for a spread below 10 percent. This is a starting target, not a firmware limit.
If the spread is higher, find the cause (see below) before you trust the mean.

## Result tables

Make one table per feed type. Copy this template.

Feed: ____________  Brand / batch: ____________  Preset: ____________
Auger: OD ___ mm, pitch ___ mm, clearance ___ mm. Date: ____________
Scale resolution: ___ g. Hopper fill: ___________. Room: ___ C.
`mm_per_rev` used: ______ (from E = ______).

| Speed (rpm) | Run | Revolutions N | Grams | g/rev | Dribble (g) | Notes |
|---|---|---|---|---|---|---|
| 30 | 1 | 10 | | | | |
| 30 | 2 | 10 | | | | |
| 30 | 3 | 10 | | | | |
| 30 | 4 | 10 | | | | |
| 30 | 5 | 10 | | | | |
| 60 | 1 | 10 | | | | |
| 60 | 2 | 10 | | | | |
| 60 | 3 | 10 | | | | |
| 60 | 4 | 10 | | | | |
| 60 | 5 | 10 | | | | |
| 120 | 1 | 10 | | | | |
| 120 | 2 | 10 | | | | |
| 120 | 3 | 10 | | | | |
| 120 | 4 | 10 | | | | |
| 120 | 5 | 10 | | | | |

Summary (one row per speed):

| Speed (rpm) | Mean g/rev | Min | Max | Spread (%) | Verdict |
|---|---|---|---|---|---|
| 30 | | | | | |
| 60 | | | | | |
| 120 | | | | | |

## What to watch

- **Jamming.** The motor hums or stops, or feed packs at the inlet. Note the speed and
  the feed. Dried mealworms can bridge in the hopper. Note if you must tap the hopper.
- **Pellet crushing.** Look for dust and broken pellets in the tray. Compare with a
  sample of fresh pellets. If pellets break, try a larger `clearance` and print again.
- **Motor skipping.** The auger stops turning but the printer thinks it moved. Listen for
  a rattle. If g/rev drops at high rpm, the motor is probably skipping. The TMC2130
  StallGuard can detect this later. Nothing in this test uses it.
- **Dribble after the stop.** Feed keeps falling out when the auger stops. Weigh it
  as the "Dribble" column. If it is large, the outlet is too big or the end gap is too
  wide. Dribble counts as dose: the firmware must include it in the grams.
- **Flight rubbing on the tube.** A printed auger bends a little. Look for scratches
  on the tube bore. A scratch means the clearance is too small.
- **Hopper level.** Feed rate can change with the hopper level. Note it if the g/rev
  falls as the hopper empties.
