"""
Life Pager - parametric enclosure (Rev A, module-based build).

Run:  python3 enclosure/pager.py
Out:  enclosure/out/*.stl (print these), *.step (edit in any CAD), assembly.step,
      preview_*.svg, and an interference report on stdout.

Coordinate system (mm): X = length, Y = width, Z = thickness.
Z = 0 is the FRONT FACE (also the print bed for the shell), Z = T is the back.
Looking at the face from the front (from -Z, with +Y up), +X points LEFT: the buttons (+X)
are on the left, the display (-X) on the right, and USB-C is on the bottom edge (-Y).

Every dimension a builder may need to tune is a named constant below. Dimensions marked
VERIFY are taken from vendor listings, not measured parts - measure yours with calipers
before printing the final copy.
"""
import os
import cadquery as cq

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")

# ----------------------------------------------------------------------------- body
L, W, T = 102.0, 50.0, 15.6      # outer size
R = 9.0                          # plan-view corner radius
WALL = 2.2                       # side wall
FACE = 1.6                       # front face thickness
PLATE_T = 1.6                    # back plate thickness
LEDGE = 1.0                      # rebate width the back plate sits on
FIT = 0.15                       # per-side clearance for printed parts that mate
FRONT_FILLET, BACK_FILLET = 2.0, 0.8

CAV_L, CAV_W = L - 2 * WALL, W - 2 * WALL
CAV_R = R - WALL
PLATE_Z = T - PLATE_T            # underside of back plate

# ----------------------------------------------------------------------------- screws
BOSS_X, BOSS_Y = L / 2 - 6.0, W / 2 - 6.0
BOSS_R = 2.7
SCREW_PILOT_D = 1.7              # M2 self-tapping. For M2 heat-set inserts use 3.2
SCREW_CLEAR_D = 2.3
SCREW_HEAD_D = 4.2               # countersink top diameter

# ----------------------------------------------------------------------------- display
# Adafruit #3502 Sharp Memory LCD breakout (LS013B7DH05, 168x144).  VERIFY
DISP_PCB = (40.0, 39.0)          # board outline
DISP_T = 4.6                     # total thickness incl. glass
DISP_C = (-22.0, 0.0)            # board centre
WIN = (26.0, 22.5)               # window = 24.5 x 21 viewable area + margin
WIN_OFF = (0.0, 0.0)             # window centre relative to board centre.  VERIFY
DISP_TOP_Z = FACE + DISP_T + 0.1

# ----------------------------------------------------------------------------- buttons
# Three 6x6x4.3 mm through-hole tact switches on a strip of 2.54 mm perfboard.
AUX = (13.0, 32.0, 1.6)          # perfboard strip (x, y, thickness)
AUX_C = (42.0, 0.0)
BTN_Y = (10.0, 0.0, -10.0)       # UP, SELECT, DOWN
BTN_HOLE_D = 6.6
BTN_CAP_D = 6.2
BTN_FLANGE_D, BTN_FLANGE_T = 8.4, 0.8
BTN_PROUD = 0.6                  # cap height above the face
SW_H = 4.3                       # switch height, board to actuator tip
AUX_Z = FACE + BTN_FLANGE_T + 0.1 + SW_H     # front surface of perfboard
AUX_SOLDER = 1.0                 # solder joints behind the perfboard

# ----------------------------------------------------------------------------- XIAO nRF52840
XIAO = (17.8, 21.0, 1.2)         # rotated: USB-C points to -Y (bottom edge)
XIAO_C = (25.4, -W / 2 + WALL + 0.5 + 21.0 / 2)
USB_Z = T / 2                    # USB-C centred in the thickness
USB = (9.4, 3.6)                 # receptacle opening
USB_RELIEF = (12.6, 6.8, 1.2)    # recess for the plug overmould
XIAO_Z = USB_Z - 1.6 - XIAO[2]   # front (bottom) surface of the XIAO PCB

# ----------------------------------------------------------------------------- back layer
# Makerdiary nRF9151 Connect Kit 55.88 x 20.32 (VERIFY height), LiPo 602050.
KIT = (55.88, 20.32, 4.6)
KIT_C = (-42.0 + 55.88 / 2, -W / 2 + WALL + 0.4 + 20.32 / 2)
BATT = (50.0, 20.0, 6.2)         # "602050" cell with a little swell margin
BATT_C = (-42.0 + 25.0, W / 2 - WALL - 0.6 - 10.0)
MOTOR_D, MOTOR_T = 10.0, 2.7     # coin vibration motor, stuck to the back plate
MOTOR_C = (24.0, 13.0)

# ----------------------------------------------------------------------------- tray
TRAY_T = 1.0
TRAY_Z = DISP_TOP_Z              # left deck sits on the display brackets
STEP_X = 14.0                    # where the tray steps up over the XIAO / buttons
TRAY_HI_Z = max(AUX_Z + AUX[2] + AUX_SOLDER, USB_Z + 1.6) + 0.2
TRAY_HI_T = 0.8
LEG = (30.0, 12.0)               # tray leg down to the face


def rrect(length, width, radius):
    return cq.Workplane("XY").rect(length, width).extrude(1).edges("|Z").fillet(radius)


def slab(length, width, radius, z0, z1):
    s = cq.Workplane("XY").workplane(offset=z0).rect(length, width).extrude(z1 - z0)
    return s.edges("|Z").fillet(radius) if radius > 0 else s


def box(cx, cy, sx, sy, z0, z1):
    return cq.Workplane("XY").workplane(offset=z0).center(cx, cy).rect(sx, sy).extrude(z1 - z0)


def cyl(cx, cy, d, z0, z1):
    return cq.Workplane("XY").workplane(offset=z0).center(cx, cy).circle(d / 2).extrude(z1 - z0)


BOSSES = [(sx * BOSS_X, sy * BOSS_Y) for sx in (-1, 1) for sy in (-1, 1)]


def make_shell():
    body = slab(L, W, R, 0, T)
    body = body.faces("<Z").edges().fillet(FRONT_FILLET)
    body = body.faces(">Z").edges().fillet(BACK_FILLET)
    body = body.cut(slab(CAV_L, CAV_W, CAV_R, FACE, T))
    body = body.cut(slab(CAV_L + 2 * LEDGE, CAV_W + 2 * LEDGE, CAV_R + LEDGE, PLATE_Z, T))

    # screw bosses, full height, fused into the corners
    for (x, y) in BOSSES:
        body = body.union(cyl(x, y, 2 * BOSS_R, FACE, PLATE_Z))
        # web that ties the boss to the corner so it is not a free-standing pin
        wx = x + (1.6 if x > 0 else -1.6)
        wy = y + (1.6 if y > 0 else -1.6)
        body = body.union(box(wx, wy, 3.4, 3.4, FACE, PLATE_Z))
    for (x, y) in BOSSES:
        body = body.cut(cyl(x, y, SCREW_PILOT_D, FACE + 1.0, T))

    # display: four corner brackets locate the breakout, the tray clamps it
    dx, dy = DISP_PCB[0] / 2 + 0.2, DISP_PCB[1] / 2 + 0.2
    for sx in (-1, 1):
        for sy in (-1, 1):
            cx, cy = DISP_C[0] + sx * dx, DISP_C[1] + sy * dy
            body = body.union(box(cx + sx * 0.6, cy - sy * 2.4, 1.2, 6.0, FACE, DISP_TOP_Z))
            body = body.union(box(cx - sx * 2.4, cy + sy * 0.6, 6.0, 1.2, FACE, DISP_TOP_Z))
    win = box(DISP_C[0] + WIN_OFF[0], DISP_C[1] + WIN_OFF[1], WIN[0], WIN[1], -1, FACE + 1)
    body = body.cut(win.edges("|Z").fillet(1.5))
    bevel = (cq.Workplane("XY").center(DISP_C[0] + WIN_OFF[0], DISP_C[1] + WIN_OFF[1])
             .rect(WIN[0] + 2.0, WIN[1] + 2.0).workplane(offset=1.0)
             .rect(WIN[0], WIN[1]).loft(combine=True))
    body = body.cut(bevel)

    # buttons: holes through the face, perfboard rails either side of the strip
    for by in BTN_Y:
        body = body.cut(cyl(AUX_C[0], AUX_C[1] + by, BTN_HOLE_D, -1, FACE + 1))
    for sy in (-1, 1):   # end rests: the strip sits on these, switch legs clear them
        body = body.union(box(AUX_C[0], AUX_C[1] + sy * (AUX[1] / 2 - 1.0), AUX[0], 2.0, FACE, AUX_Z))
    for sy in (-1, 1):   # end stops
        body = body.union(box(AUX_C[0], AUX_C[1] + sy * (AUX[1] / 2 + 0.8), AUX[0], 1.2, FACE,
                              AUX_Z + AUX[2]))
    # side stop, shared with the XIAO (it sits in the 1 mm gap between the two boards)
    body = body.union(box(AUX_C[0] - AUX[0] / 2 - 0.55, AUX_C[1], 0.9, AUX[1], FACE, AUX_Z + AUX[2]))

    # XIAO: two rails under the long edges + a back stop; USB-C through the bottom wall
    for sx in (-1, 1):
        body = body.union(box(XIAO_C[0] + sx * (XIAO[0] / 2 - 0.6), XIAO_C[1], 1.2, XIAO[1] - 4.0,
                              FACE, XIAO_Z))
    body = body.union(box(XIAO_C[0] - XIAO[0] / 2 - 0.8, XIAO_C[1], 1.2, XIAO[1] - 8.0,
                          FACE, XIAO_Z + XIAO[2]))
    body = body.union(box(XIAO_C[0], XIAO_C[1] + XIAO[1] / 2 + 0.8, XIAO[0], 1.2, FACE, XIAO_Z + XIAO[2]))
    usb = (cq.Workplane("XZ").workplane(offset=W / 2 - WALL - 1).center(XIAO_C[0], USB_Z)
           .rect(USB[0], USB[1]).extrude(WALL + 2).edges("|Y").fillet(1.5))
    body = body.cut(usb)
    relief = (cq.Workplane("XZ").workplane(offset=W / 2 - USB_RELIEF[2]).center(XIAO_C[0], USB_Z)
              .rect(USB_RELIEF[0], USB_RELIEF[1]).extrude(USB_RELIEF[2] + 1).edges("|Y").fillet(3.0))
    body = body.cut(relief)

    # landing pad for the tray leg
    body = body.union(cyl(LEG[0], LEG[1], 6.0, FACE, FACE + 0.6))
    return body


def make_tray():
    outline = slab(CAV_L - 2 * FIT - 0.3, CAV_W - 2 * FIT - 0.3, CAV_R, 0, 30)
    lo = box((-L / 2 + STEP_X) / 2, 0, L / 2 + STEP_X, W, TRAY_Z, TRAY_Z + TRAY_T)
    hi = box((L / 2 + STEP_X) / 2, 0, L / 2 - STEP_X, W, TRAY_HI_Z, TRAY_HI_Z + TRAY_HI_T)
    web = box(STEP_X + 0.5, 0, 1.0, W, FACE + 0.1, TRAY_HI_Z + TRAY_HI_T)
    tray = lo.union(hi).union(web)
    # wire notches through the web (display + battery leads to XIAO)
    tray = tray.cut(box(STEP_X + 0.5, 10.0, 3.0, 12.0, FACE, TRAY_HI_Z))
    tray = tray.cut(box(STEP_X + 0.5, -12.0, 3.0, 8.0, FACE, TRAY_Z))
    tray = tray.intersect(outline)
    for (x, y) in BOSSES:
        tray = tray.cut(cyl(x, y, 2 * BOSS_R + 0.8, 0, 30))
        tray = tray.cut(box(x + (3 if x > 0 else -3), y + (3 if y > 0 else -3), 7.0, 7.0, 0, 30))
    # leg + hold-down pads that touch the back plate
    tray = tray.union(cyl(LEG[0], LEG[1], 4.0, FACE + 0.7, TRAY_HI_Z))
    gap_y = (KIT_C[1] + KIT[1] / 2 + BATT_C[1] - BATT[1] / 2) / 2
    for px in (-34.0, -6.0):
        tray = tray.union(box(px, gap_y, 5.0, 2.4, TRAY_Z, PLATE_Z - 0.1))
    tray = tray.union(cyl(LEG[0] + 8.0, LEG[1] - 6.0, 4.0, TRAY_HI_Z, PLATE_Z - 0.1))
    # wire slots: display header -> back layer, button strip -> back layer
    tray = tray.cut(box(DISP_C[0], DISP_C[1] + DISP_PCB[1] / 2 + 1.6, 26.0, 2.6, 0, TRAY_Z + 5))
    tray = tray.cut(box(DISP_C[0], DISP_C[1] - DISP_PCB[1] / 2 - 1.6, 26.0, 2.6, 0, TRAY_Z + 5))
    tray = tray.cut(box(AUX_C[0] - AUX[0] / 2 - 3.0, 0, 3.0, 14.0, TRAY_HI_Z - 1, TRAY_HI_Z + 5))
    tray = tray.cut(box(XIAO_C[0], XIAO_C[1] + 4.0, 10.0, 6.0, TRAY_HI_Z - 1, TRAY_HI_Z + 5))
    return tray


def make_plate():
    p = slab(CAV_L + 2 * LEDGE - 2 * FIT, CAV_W + 2 * LEDGE - 2 * FIT, CAV_R + LEDGE - FIT, PLATE_Z, T)
    for (x, y) in BOSSES:
        p = p.cut(cyl(x, y, SCREW_CLEAR_D, PLATE_Z - 1, T + 1))
        cs = (cq.Workplane("XY").workplane(offset=T - 1.0).center(x, y).circle(SCREW_CLEAR_D / 2)
              .workplane(offset=1.0).circle(SCREW_HEAD_D / 2).loft(combine=True))
        p = p.cut(cs)
    return p


def make_button(by=0.0):
    z_flange = FACE + 0.1
    cap = cyl(AUX_C[0], AUX_C[1] + by, BTN_CAP_D, -BTN_PROUD, z_flange + BTN_FLANGE_T)
    cap = cap.faces("<Z").edges().fillet(0.5)
    return cap.union(cyl(AUX_C[0], AUX_C[1] + by, BTN_FLANGE_D, z_flange, z_flange + BTN_FLANGE_T))


def components():
    """Keep-out solids for the bought parts; used for the interference check and assembly."""
    c = {}
    c["display"] = box(DISP_C[0], DISP_C[1], DISP_PCB[0], DISP_PCB[1], FACE, FACE + DISP_T)
    c["button_strip"] = box(AUX_C[0], AUX_C[1], AUX[0], AUX[1] - 4.2, AUX_Z, AUX_Z + AUX[2] + AUX_SOLDER) \
        .union(box(AUX_C[0], AUX_C[1], AUX[0], AUX[1], AUX_Z, AUX_Z + AUX[2]))
    for i, by in enumerate(BTN_Y):
        c[f"switch_{i}"] = box(AUX_C[0], AUX_C[1] + by, 6.2, 6.2, AUX_Z - 3.5, AUX_Z) \
            .union(cyl(AUX_C[0], AUX_C[1] + by, 3.5, AUX_Z - SW_H, AUX_Z))
    c["xiao"] = box(XIAO_C[0], XIAO_C[1], XIAO[0], XIAO[1], XIAO_Z, XIAO_Z + XIAO[2]) \
        .union(box(XIAO_C[0], XIAO_C[1] + 2.0, 12.0, 12.0, XIAO_Z + XIAO[2], XIAO_Z + XIAO[2] + 2.2)) \
        .union(box(XIAO_C[0], XIAO_C[1] - XIAO[1] / 2 + 3.0, 8.9, 7.4, USB_Z - 1.6, USB_Z + 1.6))
    c["nrf9151_kit"] = box(KIT_C[0], KIT_C[1], KIT[0], KIT[1], TRAY_Z + TRAY_T, TRAY_Z + TRAY_T + KIT[2])
    c["battery"] = box(BATT_C[0], BATT_C[1], BATT[0], BATT[1], TRAY_Z + TRAY_T, TRAY_Z + TRAY_T + BATT[2])
    c["motor"] = cyl(MOTOR_C[0], MOTOR_C[1], MOTOR_D, PLATE_Z - MOTOR_T, PLATE_Z)
    return c


def volume(a, b):
    try:
        return a.intersect(b).val().Volume()
    except Exception:
        return 0.0


def main():
    os.makedirs(OUT, exist_ok=True)
    parts = {"shell": make_shell(), "tray": make_tray(), "back_plate": make_plate(),
             "button_cap": make_button(0.0)}
    for name, part in parts.items():
        cq.exporters.export(part, os.path.join(OUT, f"{name}.step"))
        cq.exporters.export(part, os.path.join(OUT, f"{name}.stl"), tolerance=0.02, angularTolerance=0.1)
        bb = part.val().BoundingBox()
        print(f"{name:11s} {bb.xlen:6.1f} x {bb.ylen:5.1f} x {bb.zlen:5.1f} mm  "
              f"vol {part.val().Volume() / 1000:5.1f} cm3  valid={part.val().isValid()}")

    comps = components()
    printed = dict(parts)
    del printed["button_cap"]
    for i, by in enumerate(BTN_Y):
        printed[f"button_cap_{i}"] = make_button(by)

    print("\ninterference check (mm3, anything > 0.5 is a clash):")
    solids = {**printed, **comps}
    names = list(solids)
    clashes = 0
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            v = volume(solids[a], solids[b])
            if v > 0.5:
                clashes += 1
                print(f"  CLASH {a} x {b}: {v:.1f}")
    print("  none" if clashes == 0 else f"  {clashes} clash(es)")

    asm = cq.Assembly()
    colors = {"shell": (0.92, 0.92, 0.90), "tray": (0.55, 0.57, 0.60), "back_plate": (0.85, 0.85, 0.83)}
    for name, part in printed.items():
        asm.add(part, name=name, color=cq.Color(*colors.get(name, (0.95, 0.45, 0.15))))
    for name, part in comps.items():
        asm.add(part, name=name, color=cq.Color(0.15, 0.45, 0.35))
    asm.save(os.path.join(OUT, "assembly.step"))

    def svg(obj, fname, direction):
        # the SVG exporter draws model +X up the page; turn the part so it reads landscape
        obj = obj.rotate((0, 0, 0), (0, 0, 1), 90)
        cq.exporters.export(obj, os.path.join(OUT, fname), opt={
            "width": 900, "height": 560, "marginLeft": 30, "marginTop": 30,
            "projectionDir": direction, "showAxes": False, "strokeWidth": 0.25,
            "strokeColor": (30, 30, 30), "hiddenColor": (170, 170, 170), "showHidden": False})

    closed = parts["shell"].union(parts["back_plate"])
    for i, by in enumerate(BTN_Y):
        closed = closed.union(printed[f"button_cap_{i}"])
    svg(closed, "preview_front.svg", (0.5, 0.45, -1.0))
    svg(closed, "preview_back.svg", (0.5, -0.45, 1.0))
    inside = parts["shell"]
    for part in comps.values():
        inside = inside.union(part)
    svg(inside, "preview_inside.svg", (0.0, 0.0, 1.0))
    svg(parts["tray"], "preview_tray.svg", (0.5, -0.45, 1.0))
    return clashes


if __name__ == "__main__":
    raise SystemExit(1 if main() else 0)
