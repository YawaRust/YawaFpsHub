#!/usr/bin/env python3
"""Сравнивает скриншоты exe и демо; --check отсеивает пустые и пропавшие кадры.

Порог визуального расхождения опционален: пока нет согласованного эталона
для разных версий Edge/DPI, произвольный порог не заменяет визуальную проверку.
"""

from __future__ import annotations

import argparse
import os
from hashlib import sha256
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

VIEWS = ("dashboard", "sensors", "bottleneck", "system", "overlay", "settings")


def load(path: Path, size: tuple[int, int]) -> Image.Image:
    with Image.open(path) as source:
        return source.convert("RGB").resize(size, Image.Resampling.LANCZOS)


def compare(demo: Image.Image, app: Image.Image) -> float:
    diff = ImageChops.difference(demo.convert("L"), app.convert("L"))
    return ImageStat.Stat(diff).mean[0]


def check_frame(frame: Image.Image, seen: set[str], label: str, minimum_stddev: float) -> list[str]:
    problems = []
    if ImageStat.Stat(frame.convert("L")).stddev[0] < minimum_stddev:
        problems.append(f"{label}: пустое или однородное изображение")
    digest = sha256(frame.tobytes()).hexdigest()
    if digest in seen:
        problems.append(f"{label}: повторён кадр другого экрана")
    seen.add(digest)
    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--demo", required=True, help="каталог с demo-<view>.png")
    parser.add_argument("--app", required=True, help="каталог со screenshot-<view>.png")
    parser.add_argument("--out", required=True, help="куда положить пары и отчёт")
    parser.add_argument("--width", type=int, default=1044)
    parser.add_argument("--height", type=int, default=788)
    parser.add_argument("--check", action="store_true", help="сбой при отсутствующем, пустом или повторённом кадре")
    parser.add_argument("--max-score", type=float, help="проверенный порог расхождения яркости из 255")
    args = parser.parse_args()
    if args.width <= 0 or args.height <= 0 or (args.max_score is not None and not 0 <= args.max_score <= 255):
        parser.error("неверный размер кадра или порог")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    size = (args.width, args.height)
    problems = []
    seen_demo: set[str] = set()
    seen_app: set[str] = set()
    for view in VIEWS:
        demo_path = Path(args.demo) / f"demo-{view}.png"
        app_path = Path(args.app) / f"screenshot-{view}.png"
        if not demo_path.exists() or not app_path.exists():
            problems.append(f"{view}: нет пары ({demo_path.name}, {app_path.name})")
            continue
        try:
            demo = load(demo_path, size)
            app = load(app_path, size)
        except (OSError, ValueError) as exc:
            problems.append(f"{view}: не удалось прочесть PNG: {exc}")
            continue
        if args.check:
            problems += check_frame(demo, seen_demo, f"demo-{view}", 2.0)
            problems += check_frame(app, seen_app, f"screenshot-{view}", 2.0)
        score = compare(demo, app)
        print(f"{view:10s} расхождение {score:5.1f} из 255")
        if os.environ.get("GITHUB_ACTIONS"):
            print(f"::notice title=UI {view}::расхождение {score:.1f} из 255")
        if args.max_score is not None and score > args.max_score:
            problems.append(f"{view}: расхождение {score:.1f} выше {args.max_score:.1f}")
        pair = Image.new("RGB", (size[0] * 2 + 12, size[1]), (12, 15, 20))
        pair.paste(demo, (0, 0))
        pair.paste(app, (size[0] + 12, 0))
        pair.save(out / f"pair-{view}.png")
    for problem in problems:
        print(f"ОШИБКА: {problem}")
        if os.environ.get("GITHUB_ACTIONS"):
            print(f"::error title=UI screenshot::{problem}")
    print(f"пары сохранены в {out}")
    return int(bool(problems) and (args.check or args.max_score is not None))


if __name__ == "__main__":
    raise SystemExit(main())
