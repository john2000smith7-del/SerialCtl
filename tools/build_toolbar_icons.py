#!/usr/bin/env python3
"""Render the checked-in monochrome vector atlas (optional asset-authoring tool).

Requires resvg-py. Production compilation uses the checked-in transparent PNGs,
so no Python graphics library is required on Windows or an end-user computer.
"""
from pathlib import Path
import xml.etree.ElementTree as ET
import resvg_py

root = Path(__file__).resolve().parents[1] / "src/app/assets"
svg = (root / "toolbar-icons.svg").read_text()
width, height = (int(ET.fromstring(svg).attrib[key]) * 4 for key in ("width", "height"))
for name, color in (("black", "#000000"), ("white", "#ffffff")):
    (root / ("toolbar-" + name + ".png")).write_bytes(
        resvg_py.svg_to_bytes(svg_string=svg.replace("#000000", color), width=width, height=height))
