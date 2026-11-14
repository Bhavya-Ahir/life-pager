#!/usr/bin/env python3
"""Check hardware/netlist.yaml and regenerate hardware/bom.csv and hardware/wiring.svg.

The wiring diagram is drawn in net-label style: each module is a box, each connection point
carries the name of the wire or plug it joins. Two points with the same label are connected.
Labels starting with PLUG_ are plug-in connections; the others are wires.
"""
import csv
import html
import os
import sys

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
HW = os.path.dirname(HERE)


def load():
    with open(os.path.join(HW, "netlist.yaml")) as f:
        return yaml.safe_load(f)


def split(node):
    ref, pin = node.split(".", 1)
    return ref, pin


def check(d):
    errors = []
    pin_net = {}
    for net, nodes in d["nets"].items():
        if len(nodes) < 2:
            errors.append(f"net {net}: needs at least two pins")
        for node in nodes:
            ref, pin = split(node)
            part = d["parts"].get(ref)
            if part is None:
                errors.append(f"net {net}: unknown part {ref}")
                continue
            if pin not in [str(p) for p in part["pins"]]:
                errors.append(f"net {net}: {ref} has no pin '{pin}'")
            if pin in [str(p) for p in part.get("nc", [])]:
                errors.append(f"net {net}: {node} is declared nc")
            if node in pin_net:
                errors.append(f"{node} is on two nets: {pin_net[node]} and {net}")
            pin_net[node] = net
    for ref, part in d["parts"].items():
        for pin in part["pins"]:
            node = f"{ref}.{pin}"
            if node not in pin_net and str(pin) not in [str(p) for p in part.get("nc", [])]:
                errors.append(f"{node} is unconnected and not declared nc")
    # every connection must touch a ready-made module - there is no custom board to land on
    for net, nodes in d["nets"].items():
        if len(nodes) > 3:
            errors.append(f"net {net}: {len(nodes)} ends - a wire should join at most three points")
    return errors, pin_net


def write_bom(d):
    rows = []
    total = 0.0
    for ref, p in d["parts"].items():
        rows.append([ref, p["qty"], p["name"], p["role"], p.get("source", ""), p["cost_usd"]])
        total += p["qty"] * p["cost_usd"]
    for e in d.get("extras", []):
        rows.append(["-", e["qty"], e["name"], "", "", e["cost_usd"]])
        total += e["qty"] * e["cost_usd"]
    with open(os.path.join(HW, "bom.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ref", "qty", "part", "role", "source", "unit_cost_usd_estimate"])
        w.writerows(rows)
        w.writerow(["", "", "TOTAL (estimate)", "", "", round(total, 2)])
    return total


POWER = {"VBAT_MODEM": "#b3261e", "3V3": "#b3261e", "GND": "#3b5b92"}
LAYOUT = [  # columns of part refs
    ["U1", "BT1"],
    ["A1", "DS1", "W2"],
    ["M1", "W1"],
    ["U2"],
]


def write_svg(d, pin_net):
    col_w, box_w, row_h, pad = 330, 150, 18, 26
    out = []
    height = 0
    for ci, col in enumerate(LAYOUT):
        y = 84
        x = 30 + ci * col_w
        for ref in col:
            p = d["parts"][ref]
            pins = [str(q) for q in p["pins"]]
            h = max(len(pins), 1) * row_h + 14
            out.append(f'<rect x="{x}" y="{y}" width="{box_w}" height="{h}" rx="5" class="box"/>')
            out.append(f'<text x="{x}" y="{y - 18}" class="ref">{ref} <tspan class="role">'
                       f'{html.escape(p["role"][:44])}</tspan></text>')
            out.append(f'<text x="{x}" y="{y - 5}" class="name">{html.escape(p["name"][:52])}</text>')
            for i, pin in enumerate(pins):
                py = y + 16 + i * row_h
                net = pin_net.get(f"{ref}.{pin}")
                out.append(f'<text x="{x + box_w - 8}" y="{py + 4}" class="pin" text-anchor="end">'
                           f'{html.escape(pin)}</text>')
                if net is None:
                    out.append(f'<text x="{x + box_w + 8}" y="{py + 4}" class="nc">nc</text>')
                    continue
                color = POWER.get(net, "#7a7a7a" if net.startswith("PLUG_") else "#1b6e4b")
                out.append(f'<line x1="{x + box_w}" y1="{py}" x2="{x + box_w + 22}" y2="{py}" '
                           f'stroke="{color}" stroke-width="1.6"/>')
                out.append(f'<text x="{x + box_w + 27}" y="{py + 4}" class="net" fill="{color}">'
                           f'{html.escape(net)}</text>')
            y += h + pad + 22
        height = max(height, y)
    width = 30 + len(LAYOUT) * col_w
    style = (".box{fill:#fbfbf8;stroke:#222;stroke-width:1.2}"
             ".ref{font:700 13px ui-monospace,Menlo,monospace;fill:#111}"
             ".role{font:400 11px system-ui,sans-serif;fill:#555}"
             ".name{font:400 10.5px system-ui,sans-serif;fill:#555}"
             ".pin{font:12px ui-monospace,Menlo,monospace;fill:#111}"
             ".net{font:700 12px ui-monospace,Menlo,monospace}"
             ".nc{font:italic 11px system-ui,sans-serif;fill:#999}"
             ".title{font:700 16px system-ui,sans-serif;fill:#111}"
             ".note{font:11.5px system-ui,sans-serif;fill:#444}")
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" '
           f'height="{height}"><style>{style}</style><rect width="100%" height="100%" fill="#fff"/>'
           f'<text x="30" y="26" class="title">Life Pager Rev A - wiring between ready-made modules</text>'
           f'<text x="30" y="42" class="note">Points that share a label are connected. Grey PLUG_ labels plug in; '
           f'green and red labels are wires. No custom circuit. Generated from netlist.yaml.</text>' + "".join(out) + "</svg>")
    with open(os.path.join(HW, "wiring.svg"), "w") as f:
        f.write(svg)


def main():
    d = load()
    errors, pin_net = check(d)
    for e in errors:
        print("ERROR", e)
    if errors:
        return 1
    total = write_bom(d)
    write_svg(d, pin_net)
    print(f"netlist ok: {len(d['parts'])} parts, {len(d['nets'])} nets, "
          f"{len(pin_net)} connected pins, BOM estimate ${total:.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
