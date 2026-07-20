"""Build a clean layered SVG from the hair logo PNG."""
from __future__ import annotations

import re
import xml.etree.ElementTree as ET
from pathlib import Path

import cv2
import numpy as np

SRC = Path(
    r"C:\Users\Admin\.cursor\projects\a-3dModels-BendeRadio-main-BendeRadio-main-firmware\assets"
    r"\c__Users_Admin_AppData_Roaming_Cursor_User_workspaceStorage_ea15646045110ea121bdee4dfa46e368_images"
    r"_photo_2026-06-26_23-09-23-Photoroom-422e639a-5774-4ca4-896d-edbc798cb114.png"
)
OUT = Path(__file__).with_name("hair-logo-clean.svg")

PALETTE: list[tuple[int, int, int]] = [
    (0, 0, 0),
    (25, 25, 25),
    (77, 77, 77),
    (140, 140, 140),
    (173, 173, 173),
    (191, 191, 191),
    (209, 209, 209),
    (240, 240, 240),
]
CONTOUR_COLORS = {(25, 25, 25)}
HAIR_COLORS = [
    (77, 77, 77),
    (140, 140, 140),
    (173, 173, 173),
    (191, 191, 191),
    (209, 209, 209),
    (240, 240, 240),
]


def bgr_to_hex(color: tuple[int, int, int]) -> str:
    b, g, r = color
    return f"#{r:02X}{g:02X}{b:02X}"


def composite_rgba(img: np.ndarray) -> np.ndarray:
    if img.shape[2] == 4:
        b, g, r, a = cv2.split(img)
        alpha = a.astype(np.float32) / 255.0
        rgb = np.stack([b, g, r], axis=-1).astype(np.float32)
        return (rgb * alpha[..., None]).astype(np.uint8)
    return img[:, :, :3]


def quantize_image(img: np.ndarray) -> np.ndarray:
    palette = np.array(PALETTE, dtype=np.int16)
    flat = img.reshape(-1, 3).astype(np.int16)
    idx = ((flat[:, None, :] - palette[None, :, :]) ** 2).sum(axis=2).argmin(axis=1)
    return palette[idx].reshape(img.shape).astype(np.uint8)


def mask_for_colors(quant: np.ndarray, colors: set[tuple[int, int, int]]) -> np.ndarray:
    mask = np.zeros(quant.shape[:2], dtype=np.uint8)
    for color in colors:
        mask = cv2.bitwise_or(mask, np.all(quant == color, axis=2).astype(np.uint8) * 255)
    return mask


def clean_mask(mask: np.ndarray, close_size: int = 0) -> np.ndarray:
    if close_size <= 0:
        return mask
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (close_size, close_size))
    return cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel, iterations=1)


def fill_holes(mask: np.ndarray) -> np.ndarray:
    padded = cv2.copyMakeBorder(mask, 1, 1, 1, 1, cv2.BORDER_CONSTANT, value=0)
    flood = padded.copy()
    cv2.floodFill(flood, None, (0, 0), 255)
    holes = cv2.bitwise_not(flood)
    filled = cv2.bitwise_or(padded, holes)
    return filled[1:-1, 1:-1]


def contour_to_path(contour: np.ndarray, epsilon: float) -> str | None:
    if len(contour) < 3:
        return None
    approx = cv2.approxPolyDP(contour, epsilon, True)
    if len(approx) < 3:
        return None
    pts = approx.reshape(-1, 2)
    parts = [f"M {pts[0][0]:.2f} {pts[0][1]:.2f}"]
    for x, y in pts[1:]:
        parts.append(f"L {x:.2f} {y:.2f}")
    parts.append("Z")
    return " ".join(parts)


def extract_merged_path(mask: np.ndarray, epsilon: float, min_area: float) -> str | None:
    mask = clean_mask(mask, close_size=3)
    mask = cv2.morphologyEx(
        mask,
        cv2.MORPH_OPEN,
        cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3)),
        iterations=1,
    )
    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    parts: list[str] = []
    for contour in contours:
        if cv2.contourArea(contour) < min_area:
            continue
        path = contour_to_path(contour, epsilon)
        if path:
            parts.append(path)
    if not parts:
        return None
    return " ".join(parts)


def split_hair_locks(hair_mask: np.ndarray, min_lock_area: float = 12000.0) -> list[np.ndarray]:
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (7, 7))
    seeds = cv2.erode(hair_mask, kernel, iterations=2)
    seeds = clean_mask(seeds, close_size=5)

    num, labels, stats, centroids = cv2.connectedComponentsWithStats(seeds, connectivity=8)
    seed_points: list[tuple[int, int]] = []
    for idx in range(1, num):
        if stats[idx, cv2.CC_STAT_AREA] < 350:
            continue
        seed_points.append((int(centroids[idx][0]), int(centroids[idx][1])))

    if not seed_points:
        return [hair_mask]

    label_map = np.zeros(hair_mask.shape, dtype=np.int32)
    hair_coords = np.column_stack(np.where(hair_mask > 0))
    for y, x in hair_coords:
        distances = [(sx - x) ** 2 + (sy - y) ** 2 for sx, sy in seed_points]
        label_map[y, x] = int(np.argmin(distances)) + 1

    locks: list[np.ndarray] = []
    for lock_id in range(1, len(seed_points) + 1):
        lock_mask = np.zeros_like(hair_mask)
        lock_mask[(label_map == lock_id) & (hair_mask > 0)] = 255
        lock_mask = clean_mask(lock_mask, close_size=3)
        if cv2.countNonZero(lock_mask) < min_lock_area:
            continue
        locks.append(lock_mask)

    locks.sort(
        key=lambda mask: (
            cv2.moments(mask)["m01"] / max(cv2.moments(mask)["m00"], 1.0),
            cv2.moments(mask)["m10"] / max(cv2.moments(mask)["m00"], 1.0),
        )
    )
    return locks


def build_svg(
    width: int,
    height: int,
    contour_path: str,
    lock_layers: list[tuple[str, list[tuple[str, str]]]],
) -> str:
    svg_ns = "http://www.w3.org/2000/svg"
    root = ET.Element(
        "svg",
        {
            "xmlns": svg_ns,
            "width": str(width),
            "height": str(height),
            "viewBox": f"0 0 {width} {height}",
        },
    )
    defs = ET.SubElement(root, "defs")
    ET.SubElement(defs, "style").text = (
        "svg { background: transparent; } "
        ".contour path { fill: #191919; stroke: none; } "
        ".hair path { stroke: none; }"
    )

    contour_group = ET.SubElement(root, "g", {"id": "contour", "class": "contour"})
    ET.SubElement(contour_group, "path", {"id": "contour", "d": contour_path})

    for group_id, paths in lock_layers:
        group = ET.SubElement(root, "g", {"id": group_id, "class": "hair"})
        for path_id, (fill, path_d) in enumerate(paths, start=1):
            ET.SubElement(
                group,
                "path",
                {"id": f"{group_id}-shade-{path_id}", "d": path_d, "fill": fill},
            )

    return ET.tostring(root, encoding="unicode")


def main() -> None:
    raw = cv2.imread(str(SRC), cv2.IMREAD_UNCHANGED)
    if raw is None:
        raise SystemExit(f"Cannot read {SRC}")

    img = composite_rgba(raw)
    height, width = img.shape[:2]
    quant = quantize_image(img)

    contour_mask = fill_holes(clean_mask(mask_for_colors(quant, CONTOUR_COLORS), close_size=5))
    hair_mask = fill_holes(clean_mask(mask_for_colors(quant, set(HAIR_COLORS)), close_size=7))

    contour_path = extract_merged_path(contour_mask, epsilon=2.2, min_area=600)
    if not contour_path:
        raise SystemExit("Contour path was not found")

    lock_layers: list[tuple[str, list[tuple[str, str]]]] = []
    locks = split_hair_locks(hair_mask)

    for index, lock_mask in enumerate(locks, start=1):
        shade_paths: list[tuple[str, str]] = []
        for color in HAIR_COLORS:
            color_mask = np.all(quant == color, axis=2).astype(np.uint8) * 255
            color_mask = cv2.bitwise_and(color_mask, lock_mask)
            color_mask = clean_mask(color_mask, close_size=3)
            path = extract_merged_path(color_mask, epsilon=2.8, min_area=420)
            if path:
                shade_paths.append((bgr_to_hex(color), path))
        if shade_paths:
            lock_layers.append((f"lock-{index}", shade_paths))

    svg = build_svg(width, height, contour_path, lock_layers)
    OUT.write_text(svg, encoding="utf-8")

    print(f"Saved: {OUT}")
    print(
        f"Locks: {len(lock_layers)}, paths: {len(re.findall(r'<path', svg))}, "
        f"bytes: {OUT.stat().st_size}"
    )


if __name__ == "__main__":
    main()
