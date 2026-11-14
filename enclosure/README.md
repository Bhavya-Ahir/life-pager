# Enclosure

Parametric model in [CadQuery](https://cadquery.readthedocs.io): one Python file, every
dimension a named constant. Two variants come from the same file.

```bash
pip install cadquery
```

```bash
python3 enclosure/pager.py
```

```bash
python3 enclosure/pager.py --lte
```

| Variant | Size | Output folder |
|---|---|---|
| Bluetooth only | 107 x 47 x 14 mm | `out/` |
| With the nRF9151 kit | 110 x 47 x 18.5 mm | `out-lte/` |

Each run writes STL files (print these), STEP files (open in Fusion, FreeCAD, Onshape),
`assembly.step` (printed parts plus keep-out blocks for every module) and an interference
report. The script exits non-zero if any two solids overlap.

| | |
|---|---|
| ![front](../docs/img/enclosure_front.png) | ![inside](../docs/img/enclosure_inside.png) |

Right picture: the shell from the back with the modules in place. From the right: display
panel with the EN05 behind it, battery, Grove vibration motor.

## Parts

| File | Qty | Print orientation | Notes |
|---|---|---|---|
| `shell.stl` | 1 | face down | The face is the bed surface: a textured PEI sheet gives the best finish. No supports. |
| `back_cover.stl` | 1 | outside face down | Has pads that hold the modules in place. |
| `button_cap.stl` | 3 | flange down | Pushes the EN05's side buttons through the top wall. |

0.2 mm layers, 3 perimeters, PETG or PLA. Fasteners: 4x M2 x 6 mm self-tapping screws.

## Assembly (shell face down on the desk)

1. Stick the e-paper panel, display side down, into its pocket behind the window with thin
   double-sided tape round the edge (not over the active area).
2. Plug the panel's ribbon into the EN05, then lay the EN05 on its two rails, XIAO side facing
   the panel, USB-C towards the bottom wall opening.
3. Drop the three button caps into the top wall holes from inside.
4. Battery and Grove motor into their bays. Route the battery lead to the EN05.
5. *(LTE variant)* nRF9151 kit over the battery and motor, antenna taped to the inside of the
   back cover.
6. Back cover, four screws.

## Measure before you print the final copy

The panel outline (31.8 x 37.32 x 1.0 mm) is from GoodDisplay's datasheet. **The EN05's outline,
heights and button positions are not published in anything I could read, so they are
estimates.** Measure your board with calipers and edit these constants at the top of `pager.py`:

| Constant | What to measure |
|---|---|
| `EN05` | board length, width, PCB thickness |
| `EN05_FRONT_H`, `EN05_BACK_H` | tallest part on each side (the XIAO and its USB-C on one side) |
| `EN05_BTN_X`, `EN05_BTN_Z` | where the three side buttons sit along the edge, and their height |
| `EN05_SW_X` | position of the power slide switch |
| `USB_X`, `USB_Z` | position of the XIAO's USB-C |
| `ACTIVE_OFF_Y` | offset of the panel's visible area from the panel's centre |
| `BATT` | your actual battery |

If your EN05's buttons are on a different edge than assumed, move the hole loop in
`make_shell()` to that wall. Print the shell once in draft quality as a fit check first.
