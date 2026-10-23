"""Development-only: render editable SVG masters with resvg-py; no runtime dependency."""
from pathlib import Path
import resvg_py
root = Path(__file__).resolve().parents[1] / "src/app/assets"
for name in ("utility-icons", "surface-fill", "surface-stroke"):
    (root / (name + ".png")).write_bytes(resvg_py.svg_to_bytes(svg_path=str(root / (name + ".svg")), zoom=4))
