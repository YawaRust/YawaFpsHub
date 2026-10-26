#!/usr/bin/env python3
"""Сравнение собранного приложения с HTML-прототипом.

CI снимает эталон (demo-<view>.png, Edge headless) и приложение
(screenshot-<view>.png, PrintWindow). Скрипт сводит их к одному размеру,
считает расхождение и складывает пары рядом — по ним видно, что именно
разъехалось: отступы, типографика, состав карточек.

    python3 tools/compare_ui.py --demo /tmp/ref --app /tmp/ref --out /tmp/diff
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

VIEWS = ("dashboard", "sensors", "bottleneck", "system", "overlay", "settings")


def load(path: Path, size: tuple[int, int]) -> Image.Image:
    return Image.open(path).convert("RGB").resize(size, Image.LANCZOS)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--demo", required=True, help="каталог с demo-<view>.png")
    parser.add_argument("--app", required=True, help="каталог со screenshot-<view>.png")
    parser.add_argument("--out", required=True, help="куда положить пары и отчёт")
    parser.add_argument("--width", type=int, default=1044)
    parser.add_argument("--height", type=int, default=788)
    args = parser.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    size = (args.width, args.height)

    for view in VIEWS:
        demo_path = Path(args.demo) / f"demo-{view}.png"
        app_path = Path(args.app) / f"screenshot-{view}.png"
        if not demo_path.exists() or not app_path.exists():
            print(f"{view:10s} пропущено: нет пары")
            continue

        demo = load(demo_path, size)
        app = load(app_path, size)

        # Грубая, но устойчивая метрика: средняя разница яркости по кадру.
        diff = ImageChops.difference(demo.convert("L"), app.convert("L"))
        score = ImageStat.Stat(diff).mean[0]
        print(f"{view:10s} расхождение {score:5.1f} из 255")

        pair = Image.new("RGB", (size[0] * 2 + 12, size[1]), (12, 15, 20))
        pair.paste(demo, (0, 0))
        pair.paste(app, (size[0] + 12, 0))
        pair.save(out / f"pair-{view}.png")

    print(f"пары сохранены в {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
