"""
Life Pager - parametric enclosure for the ready-made module build.

Holds: Seeed XIAO ePaper Display Board EN05 with a XIAO nRF52840 Plus, Seeed 1.54" e-paper panel,
Grove vibration motor, 803040 LiPo and, optionally (WITH_LTE), the Makerdiary nRF9151 Connect Kit.

Run:  python3 enclosure/pager.py            (Bluetooth-only case)
      python3 enclosure/pager.py --lte      (deeper case with room for the LTE kit)
Out:  enclosure/out[-lte]/*.stl (print these), *.step, assembly.step, and an interference report.

Coordinates (mm): X = length, Y = width, Z = thickness. Z = 0 is the FRONT FACE, which is also the
print bed for the shell; Z = T is the back. Seen from the front, the display is on the left (-X),
battery and motor on the right, side buttons on the top edge (+Y).

!!! Measure before printing the final copy. Every constant marked VERIFY comes from a published
!!! datasheet or is an estimate - the EN05's outline and button positions are not published in
!!! anything I could read. enclosure/README.md lists what to measure and where it goes.
"""
import os
import sys

import cadquery as cq

WITH_LTE = "--lte" in sys.argv
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out-lte" if WITH_LTE else "out")

# ----------------------------------------------------------------------------- shell
WALL, FACE, BACK = 2.0, 1.6, 1.6
FIT = 0.15                      # per-side clearance for mating printed parts
R = 8.0                         # plan-view corner radius
FRONT_FILLET, BACK_FILLET = 1.8, 0.8
T = 18.5 if WITH_LTE else 14.0

# ----------------------------------------------------------------------------- parts (VERIFY)
# 1.54" panel GDEY0154D67: outline 31.8 x 37.32 x 1.0 (GoodDisplay). Active area 27 x 27.
PANEL = (31.8, 37.32, 1.0)
ACTIVE = (27.6, 27.6)
ACTIVE_OFF_Y = 2.6              # VERIFY: active area sits toward the end opposite the ribbon
WIN_MARGIN = 0.6                # window is this much larger than the active area per side

# EN05 carrier with the XIAO on it. VERIFY ALL: outline and heights are estimates.
EN05 = (47.0, 30.0, 1.2)        # board x, y, PCB thickness
EN05_FRONT_H = 5.0              # tallest part on the side facing the panel (the XIAO + USB-C)
EN05_BACK_H = 1.5               # tallest part on the side facing the back cover
EN05_BTN_X = (-12.0, 0.0, 12.0) # side buttons along the board's +Y edge, from board centre
EN05_BTN_Z = 0.8                # button centre height above the board's back surface
EN05_SW_X = 18.0                # power slide switch on the +Y edge, from board centre
USB_X = -14.0                   # XIAO USB-C on the board's -Y edge, from board centre
USB_Z = 2.4                     # USB-C centre height above the board's front surface

BATT = (30.0, 40.0, 8.0)        # 803040 LiPo, x along length
MOTOR = (20.0, 24.0, 9.8)       # Grove vibration motor module, x along length
KIT = (55.88, 20.32, 4.6)       # nRF9151 Connect Kit (only WITH_LTE)

# ----------------------------------------------------------------------------- derived layout
GAP = 1.5                       # rib / clearance between zones
ZONE_A = max(EN05[0], PANEL[0]) + 1.0
CAV_W = max(PANEL[1], EN05[1], BATT[1], MOTOR[1]) + 3.0
ZONE_C = MOTOR[0] + 1.0
if WITH_LTE:   # the kit lies over the battery and motor zones, so they must be at least as long
    ZONE_C += max(0.0, KIT[0] + 1.0 - (BATT[0] + 1.0 + GAP + ZONE_C))
CAV_L = ZONE_A + GAP + BATT[0] + 1.0 + GAP + ZONE_C
L, W = CAV_L + 2 * WALL, CAV_W + 2 * WALL
CAV_R = R - WALL
X0 = -CAV_L / 2                                  # left inner wall
A_C = X0 + ZONE_A / 2                            # display / EN05 centre
B_C = X0 + ZONE_A + GAP + (BATT[0] + 1.0) / 2    # battery centre
C_C = B_C + (BATT[0] + 1.0) / 2 + GAP + ZONE_C / 2
KIT_C = X0 + ZONE_A + GAP + (BATT[0] + 1.0 + GAP + ZONE_C) / 2

PANEL_Z = FACE                                   # panel glued to the inside of the face
EN05_Z = PANEL_Z + PANEL[2] + EN05_FRONT_H + 0.4 # front surface of the EN05 PCB
PLATE_Z = T - BACK                               # underside of the back cover
KIT_Z = PLATE_Z - 0.3 - KIT[2]

assert EN05_Z + EN05[2] + EN05_BACK_H < PLATE_Z - 0.3, "EN05 does not fit - raise T"
assert FACE + MOTOR[2] < PLATE_Z - (KIT[2] + 0.6 if WITH_LTE else 0.3), "motor does not fit"
assert FACE + BATT[2] < PLATE_Z - (KIT[2] + 0.6 if WITH_LTE else 0.3), "battery does not fit"

BOSSES = [(sx * (L / 2 - 5.5), sy * (W / 2 - 5.5)) for sx in (-1, 1) for sy in (-1, 1)]
BOSS_R, PILOT_D, CLEAR_D, HEAD_D = 2.6, 1.7, 2.3, 4.2
BTN_HOLE_D, BTN_CAP_D, BTN_FLANGE = 3.6, 3.2, (5.4, 0.8)
LEDGE = 1.0


def slab(lx, ly, r, z0, z1):
    s = cq.Workplane("XY").workplane(offset=z0).rect(lx, ly).extrude(z1 - z0)
    return s.edges("|Z").fillet(r) if r > 0 else s


def box(cx, cy, sx, sy, z0, z1):
    return cq.Workplane("XY").workplane(offset=z0).center(cx, cy).rect(sx, sy).extrude(z1 - z0)


def cyl(cx, cy, d, z0, z1):
    return cq.Workplane("XY").workplane(offset=z0).center(cx, cy).circle(d / 2).extrude(z1 - z0)


def side_hole_y(cx, cz, d, y_wall):
    """Round hole through the +Y (y_wall > 0) or -Y wall."""
    wp = cq.Workplane("XZ").workplane(offset=-y_wall - (1 if y_wall > 0 else -1) * 0)
    return (cq.Workplane("XZ", origin=(0, y_wall + (2 if y_wall > 0 else -2), 0))
            .center(cx, cz).circle(d / 2).extrude(WALL + 4 if y_wall > 0 else -(WALL + 4)))


def make_shell():
    body = slab(L, W, R, 0, T)
    body = body.faces("<Z").edges().fillet(FRONT_FILLET)
    body = body.faces(">Z").edges().fillet(BACK_FILLET)
    body = body.cut(slab(CAV_L, CAV_W, CAV_R, FACE, T))
    body = body.cut(slab(CAV_L + 2 * LEDGE, CAV_W + 2 * LEDGE, CAV_R + LEDGE, PLATE_Z, T))

    for (x, y) in BOSSES:
        body = body.union(cyl(x, y, 2 * BOSS_R, FACE, PLATE_Z))
        body = body.union(box(x + (1.5 if x > 0 else -1.5), y + (1.5 if y > 0 else -1.5),
                              3.2, 3.2, FACE, PLATE_Z))
        body = body.cut(cyl(x, y, PILOT_D, FACE + 1.0, T))

    # display window with a bevel, and a shallow pocket that locates the panel
    wx, wy = A_C, ACTIVE_OFF_Y
    win = box(wx, wy, ACTIVE[0] + 2 * WIN_MARGIN, ACTIVE[1] + 2 * WIN_MARGIN, -1, FACE + 1)
    body = body.cut(win.edges("|Z").fillet(1.2))
    bevel = (cq.Workplane("XY").center(wx, wy)
             .rect(ACTIVE[0] + 2 * WIN_MARGIN + 2.0, ACTIVE[1] + 2 * WIN_MARGIN + 2.0)
             .workplane(offset=1.0).rect(ACTIVE[0] + 2 * WIN_MARGIN, ACTIVE[1] + 2 * WIN_MARGIN)
             .loft(combine=True))
    body = body.cut(bevel)
    for sx in (-1, 1):   # panel locating rails left and right of the panel
        body = body.union(box(A_C + sx * (PANEL[0] / 2 + 0.2 + 0.6), 0, 1.2, PANEL[1] - 6,
                              FACE, FACE + PANEL[2] + 0.4))

    # EN05 shelf: two rails under its short edges, clear of the panel
    for sx in (-1, 1):
        body = body.union(box(A_C + sx * (EN05[0] / 2 - 1.0), 0, 2.0, EN05[1] - 6, FACE, EN05_Z))
    # side buttons, power switch and USB-C through the walls
    for bx in EN05_BTN_X:
        body = body.cut(side_hole_y(A_C + bx, EN05_Z + EN05[2] + EN05_BTN_Z, BTN_HOLE_D, W / 2 - WALL))
    sw = box(A_C + EN05_SW_X, W / 2 - WALL / 2, 7.0, WALL + 2, EN05_Z + EN05[2] - 0.6,
             EN05_Z + EN05[2] + 2.4)
    body = body.cut(sw)
    usb = (cq.Workplane("XZ", origin=(0, -W / 2 + WALL + 1, 0))
           .center(A_C + USB_X, EN05_Z - USB_Z).rect(9.6, 3.8).extrude(WALL + 2)
           .edges("|Y").fillet(1.5))
    body = body.cut(usb)

    # dividers between zones (also stop the battery and motor sliding)
    for x in (X0 + ZONE_A + GAP / 2, B_C + (BATT[0] + 1.0) / 2 + GAP / 2):
        body = body.union(box(x, 0, 1.2, CAV_W - 12, FACE, FACE + 5.0))
    return body


def make_back():
    p = slab(CAV_L + 2 * LEDGE - 2 * FIT, CAV_W + 2 * LEDGE - 2 * FIT, CAV_R + LEDGE - FIT, PLATE_Z, T)
    for (x, y) in BOSSES:
        p = p.cut(cyl(x, y, CLEAR_D, PLATE_Z - 1, T + 1))
        cs = (cq.Workplane("XY").workplane(offset=T - 1.0).center(x, y).circle(CLEAR_D / 2)
              .workplane(offset=1.0).circle(HEAD_D / 2).loft(combine=True))
        p = p.cut(cs)
    # hold-down pads: press the EN05 onto its rails and the modules onto the face
    en05_top = EN05_Z + EN05[2] + EN05_BACK_H
    for sx in (-1, 1):
        p = p.union(box(A_C + sx * (EN05[0] / 2 - 1.0), 0, 2.0, 8.0, en05_top + 0.2, PLATE_Z))
    if not WITH_LTE:
        p = p.union(box(B_C, 0, 10.0, 10.0, FACE + BATT[2] + 0.4, PLATE_Z))
        p = p.union(box(C_C, 0, 8.0, 8.0, FACE + MOTOR[2] + 0.4, PLATE_Z))
    return p


def make_button(bx):
    y_in = W / 2 - WALL
    z = EN05_Z + EN05[2] + EN05_BTN_Z
    cap = (cq.Workplane("XZ", origin=(0, y_in - 0.4, 0)).center(A_C + bx, z)
           .circle(BTN_CAP_D / 2).extrude(-(WALL + 0.4 + 0.6)))
    flange = (cq.Workplane("XZ", origin=(0, y_in - 0.4, 0)).center(A_C + bx, z)
              .circle(BTN_FLANGE[0] / 2).extrude(BTN_FLANGE[1]))
    return cap.union(flange)


def components():
    c = {
        "panel": box(A_C, 0, PANEL[0], PANEL[1], PANEL_Z, PANEL_Z + PANEL[2]),
        "en05": box(A_C, 0, EN05[0], EN05[1], EN05_Z, EN05_Z + EN05[2])
        .union(box(A_C, 0, EN05[0] - 8, EN05[1] - 4, EN05_Z - EN05_FRONT_H, EN05_Z))
        .union(box(A_C, 0, EN05[0] - 8, EN05[1] - 4, EN05_Z + EN05[2], EN05_Z + EN05[2] + EN05_BACK_H)),
        "battery": box(B_C, 0, BATT[0], BATT[1], FACE, FACE + BATT[2]),
        "motor": box(C_C, 0, MOTOR[0], MOTOR[1], FACE, FACE + MOTOR[2]),
    }
    if WITH_LTE:
        c["nrf9151_kit"] = box(KIT_C, 0, KIT[0], KIT[1], KIT_Z, KIT_Z + KIT[2])
    return c


def volume(a, b):
    try:
        return a.intersect(b).val().Volume()
    except Exception:
        return 0.0


def main():
    os.makedirs(OUT, exist_ok=True)
    print(f"{'LTE' if WITH_LTE else 'Bluetooth-only'} case: {L:.1f} x {W:.1f} x {T:.1f} mm")
    parts = {"shell": make_shell(), "back_cover": make_back(), "button_cap": make_button(0.0)}
    for name, part in parts.items():
        cq.exporters.export(part, os.path.join(OUT, f"{name}.step"))
        cq.exporters.export(part, os.path.join(OUT, f"{name}.stl"), tolerance=0.02, angularTolerance=0.1)
        bb = part.val().BoundingBox()
        print(f"  {name:11s} {bb.xlen:6.1f} x {bb.ylen:5.1f} x {bb.zlen:5.1f} mm  valid={part.val().isValid()}")

    printed = {"shell": parts["shell"], "back_cover": parts["back_cover"]}
    for i, bx in enumerate(EN05_BTN_X):
        printed[f"button_cap_{i}"] = make_button(bx)
    comps = components()
    solids = {**printed, **comps}
    names = list(solids)
    clashes = 0
    print("interference check (mm3, anything > 0.5 is a clash):")
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            v = volume(solids[a], solids[b])
            if v > 0.5:
                clashes += 1
                print(f"  CLASH {a} x {b}: {v:.1f}")
    print("  none" if clashes == 0 else f"  {clashes} clash(es)")

    asm = cq.Assembly()
    for name, part in printed.items():
        color = (0.93, 0.93, 0.91) if not name.startswith("button") else (0.25, 0.25, 0.27)
        asm.add(part, name=name, color=cq.Color(*color))
    for name, part in comps.items():
        asm.add(part, name=name, color=cq.Color(0.15, 0.45, 0.35))
    asm.export(os.path.join(OUT, "assembly.step"))

    def svg(obj, fname, direction):
        obj = obj.rotate((0, 0, 0), (0, 0, 1), 90)
        cq.exporters.export(obj, os.path.join(OUT, fname), opt={
            "width": 900, "height": 560, "marginLeft": 30, "marginTop": 30,
            "projectionDir": direction, "showAxes": False, "strokeWidth": 0.25,
            "strokeColor": (30, 30, 30), "hiddenColor": (170, 170, 170), "showHidden": False})

    closed = parts["shell"].union(parts["back_cover"])
    for i in range(len(EN05_BTN_X)):
        closed = closed.union(printed[f"button_cap_{i}"])
    svg(closed, "preview_front.svg", (0.5, 0.45, -1.0))
    svg(closed, "preview_back.svg", (0.5, -0.45, 1.0))
    inside = parts["shell"]
    for part in comps.values():
        inside = inside.union(part)
    svg(inside, "preview_inside.svg", (0.0, 0.0, 1.0))
    return clashes


if __name__ == "__main__":
    raise SystemExit(1 if main() else 0)
