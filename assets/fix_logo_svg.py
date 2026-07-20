"""Clean up the hair logo SVG by merging mosaic fragments."""
from __future__ import annotations

import re
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path

import cv2
import numpy as np
from svgpathtools import parse_path

SRC = Path(r"C:\Users\Admin\Downloads\photo_2026-06-26_23-09-23-Photoroom.svg")
OUT = Path(r"C:\Users\Admin\Downloads\photo_2026-06-26_23-09-23-Photoroom-clean.svg")
OUT_COPY = Path(__file__).with_name("hair-logo-clean.svg")

WIDTH = 862
HEIGHT = 1160
CONTOUR_LUMA_MAX = 95
LOCK_COUNT_TARGET = 4


def hex_to_rgb(color: str) -> tuple[int, int, int]:
    color = color.strip().lower()
    if color.startswith("#"):
        color = color[1:]
    if len(color) == 3:
        color = "".join(ch * 2 for ch in color)
    return int(color[0:2], 16), int(color[2:4], 16), int(color[4:6], 16)


def rgb_to_hex(rgb: tuple[int, int, int]) -> str:
    r, g, b = rgb
    return f"#{r:02X}{g:02X}{b:02X}"


def luma(rgb: tuple[int, int, int]) -> float:
    r, g, b = rgb
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def parse_paths(svg_text: str) -> list[dict]:
    items: list[dict] = []
    for match in re.finditer(
        r'<path[^>]*\sfill="([^"]+)"[^>]*\sd="([^"]+)"',
        svg_text,
        flags=re.IGNORECASE,
    ):
        fill = match.group(1)
        d = match.group(2)
        rgb = hex_to_rgb(fill)
        items.append({"fill": fill, "rgb": rgb, "d": d, "luma": luma(rgb)})
    return items


def path_centroid(d: str) -> tuple[float, float] | None:
    try:
        parsed = parse_path(d)
    except Exception:
        return None
    if len(parsed) == 0:
        return None
    points = [parsed.point(t) for t in np.linspace(0, 1, 80)]
    xs = [p.real for p in points]
    ys = [p.imag for p in points]
    return float(sum(xs) / len(xs)), float(sum(ys) / len(ys))


def rasterize_path(d: str, width: int, height: int) -> np.ndarray:
    mask = np.zeros((height, width), dtype=np.uint8)
    try:
        parsed = parse_path(d)
    except Exception:
        return mask
    if len(parsed) == 0:
        return mask

    samples = max(120, int(parsed.length(error=1.0)))
    points: list[tuple[int, int]] = []
    for t in np.linspace(0, 1, samples):
        p = parsed.point(t)
        x = int(round(p.real))
        y = int(round(p.imag))
        if 0 <= x < width and 0 <= y < height:
            points.append((x, y))
    if len(points) < 3:
        return mask
    pts = np.array(points, dtype=np.int32).reshape(-1, 1, 2)
    cv2.fillPoly(mask, [pts], 255)
    return mask


def rasterize_paths(paths: list[dict], width: int, height: int) -> np.ndarray:
    mask = np.zeros((height, width), dtype=np.uint8)
    for item in paths:
        mask = cv2.bitwise_or(mask, rasterize_path(item["d"], width, height))
    return mask


def clean_mask(mask: np.ndarray, close_size: int = 5) -> np.ndarray:
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (close_size, close_size))
    return cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel, iterations=1)


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


def mask_to_merged_path(mask: np.ndarray, epsilon: float, min_area: float) -> str | None:
    mask = clean_mask(mask, close_size=5)
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


def split_hair_locks(hair_paths: list[dict], width: int, height: int) -> list[list[dict]]:
    centroids: list[tuple[float, float]] = []
    valid_paths: list[dict] = []
    for item in hair_paths:
        center = path_centroid(item["d"])
        if center is None:
            continue
        item["centroid"] = center
        centroids.append(center)
        valid_paths.append(item)

    if not valid_paths:
        return []

    hair_mask = rasterize_paths(valid_paths, width, height)
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (7, 7))
    seeds = cv2.erode(hair_mask, kernel, iterations=2)
    num, _, stats, centroids_arr = cv2.connectedComponentsWithStats(seeds, connectivity=8)

    seed_points: list[tuple[float, float]] = []
    for idx in range(1, num):
        if stats[idx, cv2.CC_STAT_AREA] < 400:
            continue
        seed_points.append((float(centroids_arr[idx][0]), float(centroids_arr[idx][1])))

    if len(seed_points) < LOCK_COUNT_TARGET:
        # Fallback: cluster path centroids when erosion yields too few locks.
        points = np.array([item["centroid"] for item in valid_paths], dtype=np.float32)
        criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 1.0)
        _, labels, centers = cv2.kmeans(
            points,
            LOCK_COUNT_TARGET,
            None,
            criteria,
            8,
            cv2.KMEANS_PP_CENTERS,
        )
        groups: list[list[dict]] = [[] for _ in range(LOCK_COUNT_TARGET)]
        for item, label in zip(valid_paths, labels.flatten(), strict=False):
            groups[int(label)].append(item)
        groups = [group for group in groups if group]
        groups.sort(
            key=lambda group: (
                np.mean([p["centroid"][1] for p in group]),
                np.mean([p["centroid"][0] for p in group]),
            )
        )
        return groups

    groups = [[] for _ in seed_points]
    for item in valid_paths:
        cx, cy = item["centroid"]
        distances = [(sx - cx) ** 2 + (sy - cy) ** 2 for sx, sy in seed_points]
        group_idx = int(np.argmin(distances))
        groups[group_idx].append(item)

    groups = [group for group in groups if group]
    groups.sort(
        key=lambda group: (
            np.mean([p["centroid"][1] for p in group]),
            np.mean([p["centroid"][0] for p in group]),
        )
    )
    return groups


def quantize_hair_color(rgb: tuple[int, int, int]) -> tuple[int, int, int]:
    palette = [
        (77, 77, 77),
        (140, 140, 140),
        (173, 173, 173),
        (191, 191, 191),
        (209, 209, 209),
        (240, 240, 240),
    ]
    best = palette[0]
    best_dist = float("inf")
    for color in palette:
        dist = sum((a - b) ** 2 for a, b in zip(rgb, color))
        if dist < best_dist:
            best_dist = dist
            best = color
    return best


def build_svg(
    width: int,
    height: int,
    contour_path: str,
    lock_layers: list[tuple[str, list[tuple[str, str]]]],
) -> str:
    root = ET.Element(
        "svg",
        {
            "xmlns": "http://www.w3.org/2000/svg",
            "width": f"{width}px",
            "height": f"{height}px",
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

    for group_id, shade_paths in lock_layers:
        group = ET.SubElement(root, "g", {"id": group_id, "class": "hair"})
        for idx, (fill, path_d) in enumerate(shade_paths, start=1):
            ET.SubElement(
                group,
                "path",
                {"id": f"{group_id}-shade-{idx}", "d": path_d, "fill": fill},
            )

    return '<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding="unicode")


def main() -> None:
    svg_text = SRC.read_text(encoding="utf-8", errors="replace")
    paths = parse_paths(svg_text)
    contour_paths = [p for p in paths if p["luma"] <= CONTOUR_LUMA_MAX]
    hair_paths = [p for p in paths if p["luma"] > CONTOUR_LUMA_MAX]

    contour_mask = rasterize_paths(contour_paths, WIDTH, HEIGHT)
    contour_path = mask_to_merged_path(contour_mask, epsilon=2.0, min_area=500)
    if not contour_path:
        raise SystemExit("Failed to build contour path")

    lock_groups = split_hair_locks(hair_paths, WIDTH, HEIGHT)
    lock_layers: list[tuple[str, list[tuple[str, str]]]] = []

    for lock_index, group in enumerate(lock_groups, start=1):
        by_color: dict[tuple[int, int, int], list[dict]] = defaultdict(list)
        for item in group:
            by_color[quantize_hair_color(item["rgb"])].append(item)

        shade_paths: list[tuple[str, str]] = []
        for color in sorted(by_color, key=lambda c: luma(c)):
            mask = rasterize_paths(by_color[color], WIDTH, HEIGHT)
            path = mask_to_merged_path(mask, epsilon=2.5, min_area=350)
            if path:
                shade_paths.append((rgb_to_hex(color), path))
        if shade_paths:
            lock_layers.append((f"lock-{lock_index}", shade_paths))

    cleaned = build_svg(WIDTH, HEIGHT, contour_path, lock_layers)
    OUT.write_text(cleaned, encoding="utf-8")
    OUT_COPY.write_text(cleaned, encoding="utf-8")

    print(f"Input paths: {len(paths)}")
    print(f"Contour source paths: {len(contour_paths)}")
    print(f"Hair source paths: {len(hair_paths)}")
    print(f"Locks: {len(lock_layers)}")
    print(f"Output paths: {len(re.findall(r'<path', cleaned))}")
    print(f"Saved: {OUT}")
    print(f"Copy: {OUT_COPY}")


if __name__ == "__main__":
    main()
