#!/usr/bin/env python3
"""Номер релиза YawaFpsHub: проверка, подъём и выжимка из CHANGELOG.

Правило выпуска: патч растёт до 9 включительно (1.0.0 → 1.0.1 → … → 1.0.9),
следующий релиз — 1.1.0. Минор так же: после x.y.9 идёт x.(y+1).0. Версия лежит
в файле VERSION, из неё же CMake берёт и свойства exe.

    python3 tools/version.py show                 # текущая версия
    python3 tools/version.py check v1.0.3         # тег совпадает с VERSION и правило не нарушено
    python3 tools/version.py bump                 # 1.0.3 → 1.0.4, записывает VERSION
    python3 tools/version.py notes v1.0.3         # раздел CHANGELOG для тела релиза
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION_FILE = ROOT / "VERSION"
CHANGELOG = ROOT / "CHANGELOG.md"
MAX_PART = 9


def read_version() -> tuple[int, int, int]:
    text = VERSION_FILE.read_text(encoding="utf-8").strip()
    return parse(text)


def parse(text: str) -> tuple[int, int, int]:
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", text.strip().lstrip("v"))
    if not match:
        raise SystemExit(f"версия «{text}» не формата X.Y.Z")
    return tuple(int(part) for part in match.groups())  # type: ignore[return-value]


def format_version(parts: tuple[int, int, int]) -> str:
    return ".".join(str(part) for part in parts)


def next_version(parts: tuple[int, int, int], kind: str) -> tuple[int, int, int]:
    major, minor, patch = parts
    if kind == "minor":
        return (major, minor + 1, 0) if minor < MAX_PART else (major + 1, 0, 0)
    if kind == "major":
        return (major + 1, 0, 0)
    if patch >= MAX_PART:
        return next_version(parts, "minor")
    return (major, minor, patch + 1)


def check(tag: str) -> int:
    current = read_version()
    if not tag:
        print(format_version(current))
        return 0
    wanted = tag.strip().lstrip("v")
    if parse(wanted) != current:
        print(f"тег {tag} не совпадает с VERSION ({format_version(current)})", file=sys.stderr)
        return 1
    for name, value in zip(("мажор", "минор", "патч"), current):
        if value > MAX_PART and name != "мажор":
            print(f"{name} = {value} больше {MAX_PART}: правило выпуска нарушено", file=sys.stderr)
            return 1
    print(format_version(current))
    return 0


def notes(version: str) -> int:
    """Печатает раздел CHANGELOG для тела релиза: от «## x.y.z» до следующего заголовка."""
    wanted = version.strip().lstrip("v")
    if not CHANGELOG.exists():
        return 0
    lines = CHANGELOG.read_text(encoding="utf-8").splitlines()
    collected: list[str] = []
    inside = False
    for line in lines:
        heading = re.match(r"^##+\s+(v?[\d.]+)", line)
        if heading:
            if heading.group(1).lstrip("v") == wanted:
                inside = True
                continue
            if inside:
                break
            continue
        if inside:
            collected.append(line)
    text = "\n".join(collected).strip()
    print(text or f"YawaFpsHub {wanted}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("show", "check", "bump", "notes"))
    parser.add_argument("argument", nargs="?", default="")
    parser.add_argument("--kind", default="patch", choices=("patch", "minor", "major"))
    parser.add_argument("--write", action="store_true", help="записать новый VERSION (для bump)")
    args = parser.parse_args()

    if args.action == "show":
        print(format_version(read_version()))
        return 0
    if args.action == "check":
        return check(args.argument)
    if args.action == "notes":
        return notes(args.argument or format_version(read_version()))

    bumped = next_version(read_version(), args.kind)
    text = format_version(bumped)
    if args.write:
        VERSION_FILE.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
