"""Build the 6-action animated tabby cat sprite atlas and remove legacy cat sprites."""

from collections import deque
import hashlib
import json
import os
from pathlib import Path
import zipfile
import io
from PIL import Image


ACTIONS = [
    ("01_idle", "待机", 220),
    ("02_listening", "倾听", 180),
    ("03_thinking", "思考", 180),
    ("04_working", "工作", 160),
    ("09_stretching", "伸懒腰", 140),
    ("11_walking", "走路", 180),
]

CELL_SIZE = 192
COLUMNS = 8
ROWS = 6
BASELINE = 180
DEFAULT_ZIP = Path(r"D:\Users\jiojoey\Chatgpt\SerialCtl\素材包.zip")


def load_zip_entries(zip_path: Path) -> tuple[zipfile.ZipFile, dict[str, str]]:
    zf = zipfile.ZipFile(zip_path, "r")
    name_map: dict[str, str] = {}
    for info in zf.infolist():
        decoded = info.filename
        if not (info.flag_bits & 0x800):
            try:
                decoded = info.filename.encode("cp437").decode("gbk")
            except Exception:
                pass
        name_map[decoded] = info.filename
    return zf, name_map


def extract_row_cats(row_img: Image.Image, action_name: str, row_idx: int) -> list[Image.Image]:
    w, h = row_img.size
    alpha = list(row_img.getchannel("A").getdata())
    visited = bytearray(w * h)
    components = []

    neighbors = (-w - 1, -w, -w + 1, -1, 1, w - 1, w, w + 1)
    for start in range(w * h):
        if visited[start] or alpha[start] <= 20:
            continue
        visited[start] = 1
        q = deque([start])
        pixels = []
        min_x = w
        min_y = h
        max_x = -1
        max_y = -1
        sum_x = 0
        while q:
            idx = q.popleft()
            pixels.append(idx)
            y, x = divmod(idx, w)
            sum_x += x
            if x < min_x:
                min_x = x
            if x > max_x:
                max_x = x
            if y < min_y:
                min_y = y
            if y > max_y:
                max_y = y
            for offset in neighbors:
                nidx = idx + offset
                if nidx < 0 or nidx >= w * h or visited[nidx]:
                    continue
                nx = nidx % w
                if abs(nx - x) > 1:
                    continue
                if alpha[nidx] > 20:
                    visited[nidx] = 1
                    q.append(nidx)
        components.append({
            "pixels": pixels,
            "area": len(pixels),
            "cx": sum_x / len(pixels),
            "bbox": (min_x, min_y, max_x + 1, max_y + 1),
        })

    # Select the 4 main cat body components and order left-to-right
    large = sorted(components, key=lambda c: c["area"], reverse=True)[:4]
    if len(large) != 4 or large[3]["area"] < 4000:
        raise RuntimeError(
            f"{action_name} row {row_idx}: expected 4 main cat components, got {[c['area'] for c in large]}"
        )
    mains = sorted(large, key=lambda c: c["cx"])
    main_ids = {id(m) for m in mains}
    main_coords = [
        [(idx % w, idx // w) for idx in m["pixels"]]
        for m in mains
    ]

    # Assign small detached components (whiskers, tiny accents) to nearest cat within 28 px
    owner = [-1] * (w * h)
    for col_idx, m in enumerate(mains):
        for idx in m["pixels"]:
            owner[idx] = col_idx

    for comp in components:
        if id(comp) in main_ids or comp["area"] >= 800:
            continue
        best_col = -1
        best_dist2 = 28 * 28 + 1
        bx0, by0, bx1, by1 = comp["bbox"]
        # Quick bbox filter before pixel distance check
        for col_idx, m in enumerate(mains):
            mx0, my0, mx1, my1 = m["bbox"]
            dx = max(0, bx0 - mx1, mx0 - bx1)
            dy = max(0, by0 - my1, my0 - by1)
            if dx * dx + dy * dy > 28 * 28:
                continue
            comp_pts = [(idx % w, idx // w) for idx in comp["pixels"]]
            # Sample boundary/all pixels cleanly (small components are tiny)
            for cx, cy in comp_pts:
                for mx, my in main_coords[col_idx][::4]:
                    d2 = (cx - mx) * (cx - mx) + (cy - my) * (cy - my)
                    if d2 < best_dist2:
                        best_dist2 = d2
                        best_col = col_idx
        if best_col >= 0 and best_dist2 <= 28 * 28:
            for idx in comp["pixels"]:
                owner[idx] = best_col

    # Dilate each cat owner mask by 2 px into unowned pixels so smooth anti-aliased edges stay intact
    row_rgba = row_img.load()
    extracted: list[Image.Image] = []
    for col_idx in range(4):
        owned_pixels = [idx for idx, o in enumerate(owner) if o == col_idx]
        xs = [idx % w for idx in owned_pixels]
        ys = [idx // w for idx in owned_pixels]
        x0 = max(0, min(xs) - 3)
        y0 = max(0, min(ys) - 3)
        x1 = min(w, max(xs) + 4)
        y1 = min(h, max(ys) + 4)

        patch = Image.new("RGBA", (x1 - x0, y1 - y0), (0, 0, 0, 0))
        patch_px = patch.load()
        owned_set = set(owned_pixels)
        for py in range(y0, y1):
            for px in range(x0, x1):
                idx = py * w + px
                if idx in owned_set:
                    patch_px[px - x0, py - y0] = row_rgba[px, py]
                elif owner[idx] == -1 and row_rgba[px, py][3] > 0:
                    # Include anti-aliased fringe adjacent (radius <= 2) to this cat
                    near = False
                    for dy in range(-2, 3):
                        ny = py + dy
                        if ny < 0 or ny >= h:
                            continue
                        row_off = ny * w
                        for dx in range(-2, 3):
                            nx = px + dx
                            if 0 <= nx < w and (row_off + nx) in owned_set:
                                near = True
                                break
                        if near:
                            break
                    if near:
                        patch_px[px - x0, py - y0] = row_rgba[px, py]

        alpha_bb = patch.getchannel("A").point(lambda v: 255 if v > 20 else 0).getbbox()
        if not alpha_bb:
            raise RuntimeError(f"{action_name} row {row_idx} col {col_idx}: empty mask after extraction")
        crop_bb = (
            max(0, alpha_bb[0] - 2),
            max(0, alpha_bb[1] - 2),
            min(patch.width, alpha_bb[2] + 2),
            min(patch.height, alpha_bb[3] + 2),
        )
        extracted.append(patch.crop(crop_bb))

    return extracted


def weighted_bottom_center_x(sprite: Image.Image, fraction: float) -> float:
    sw, sh = sprite.size
    start_y = max(0, min(sh - 1, int(sh * (1.0 - fraction))))
    px = sprite.getchannel("A").load()
    sum_x = 0
    count = 0
    for y in range(start_y, sh):
        for x in range(sw):
            if px[x, y] > 100:
                sum_x += x
                count += 1
    if count == 0:
        return sw / 2.0
    return sum_x / count


def clear_legacy_cat_from_actions(actions_path: Path) -> str:
    img = Image.open(actions_path).convert("RGBA")
    if img.size != (1280, 800):
        raise RuntimeError(f"Unexpected ai-pet-actions.png size: {img.size}")
    clear_cell = Image.new("RGBA", (160, 160), (0, 0, 0, 0))
    for slot in range(27, 40):
        x = (slot % 8) * 160
        y = (slot // 8) * 160
        img.paste(clear_cell, (x, y))
    img.save(actions_path, format="PNG", optimize=True)
    return hashlib.sha256(actions_path.read_bytes()).hexdigest()


def main() -> None:
    repo_root = Path(__file__).resolve().parents[1]
    assets_dir = repo_root / "src" / "app" / "assets"
    zip_path = Path(os.environ.get("SERIALCTL_CAT_ZIP", str(DEFAULT_ZIP)))
    if not zip_path.is_file():
        raise FileNotFoundError(f"Asset zip not found: {zip_path}")

    zf, name_map = load_zip_entries(zip_path)
    frames_by_action: list[tuple[str, str, int, list[Image.Image]]] = []
    with zf:
        for action_key, name_zh, duration_ms in ACTIONS:
            sheet_key = f"SerialCtl_猫咪动画资源_6动作_审核版/{action_key}/sprite_sheet.png"
            raw = zf.read(name_map[sheet_key])
            sheet = Image.open(io.BytesIO(raw)).convert("RGBA")
            w, h = sheet.size
            ycut = h // 2
            top_row = extract_row_cats(sheet.crop((0, 0, w, ycut)), action_key, 0)
            bot_row = extract_row_cats(sheet.crop((0, ycut, w, h)), action_key, 1)
            frames_by_action.append((action_key, name_zh, duration_ms, top_row + bot_row))

    max_w = max(frame.width for _, _, _, frames in frames_by_action for frame in frames)
    max_h = max(frame.height for _, _, _, frames in frames_by_action for frame in frames)
    scale = min((CELL_SIZE - 8) / max_w, (BASELINE - 6) / max_h)

    atlas = Image.new("RGBA", (CELL_SIZE * COLUMNS, CELL_SIZE * ROWS), (0, 0, 0, 0))
    action_records = []
    for row_idx, (action_key, name_zh, duration_ms, frames) in enumerate(frames_by_action):
        for col_idx, frame in enumerate(frames):
            sw = max(1, round(frame.width * scale))
            sh = max(1, round(frame.height * scale))
            sprite = frame.resize((sw, sh), Image.Resampling.LANCZOS)

            if action_key == "04_working":
                anchor_x = weighted_bottom_center_x(sprite, 0.15)
                cx = round(CELL_SIZE / 2.0 - anchor_x)
            elif action_key in ("01_idle", "02_listening", "03_thinking"):
                anchor_x = weighted_bottom_center_x(sprite, 0.25)
                cx = round(CELL_SIZE / 2.0 - anchor_x)
            else:
                cx = (CELL_SIZE - sw) // 2

            cx = max(2, min(CELL_SIZE - sw - 2, cx))
            x = col_idx * CELL_SIZE + cx
            y = row_idx * CELL_SIZE + BASELINE - sh
            atlas.alpha_composite(sprite, (x, y))

        action_records.append({
            "row": row_idx,
            "action": action_key,
            "name_zh": name_zh,
            "duration_ms": duration_ms,
            "frame_count": len(frames),
        })

    cat_png = assets_dir / "ai-pet-cat.png"
    atlas.save(cat_png, format="PNG", optimize=True)
    cat_sha = hashlib.sha256(cat_png.read_bytes()).hexdigest()

    cat_json = assets_dir / "ai-pet-cat.json"
    manifest = {
        "columns": COLUMNS,
        "rows": ROWS,
        "cell": CELL_SIZE,
        "baseline": BASELINE,
        "width": atlas.width,
        "height": atlas.height,
        "scale": round(scale, 6),
        "sha256": cat_sha,
        "source_zip": zip_path.name,
        "source_zip_sha256": hashlib.sha256(zip_path.read_bytes()).hexdigest(),
        "actions": action_records,
    }
    cat_json.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    actions_sha = clear_legacy_cat_from_actions(assets_dir / "ai-pet-actions.png")
    print(json.dumps({
        "ai_pet_cat_png": str(cat_png),
        "ai_pet_cat_bytes": cat_png.stat().st_size,
        "ai_pet_cat_size": list(atlas.size),
        "ai_pet_cat_sha256": cat_sha,
        "ai_pet_actions_bytes": (assets_dir / "ai-pet-actions.png").stat().st_size,
        "ai_pet_actions_sha256": actions_sha,
    }, ensure_ascii=False))


if __name__ == "__main__":
    main()
