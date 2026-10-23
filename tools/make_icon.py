"""Генерация иконки приложения без сторонних библиотек.

Рисует тёмный скруглённый квадрат, мятный спидометр и мини-график кадров,
затем собирает многоразмерный .ico (32-битные DIB-записи, как ждёт rc.exe).

Запуск: python3 tools/make_icon.py
"""

import math
import struct
import zlib
from pathlib import Path

SIZES = [16, 24, 32, 48, 64, 128, 256]

ACCENT = (0x5E, 0xF2, 0x9B)
ACCENT_DARK = (0x2C, 0xC9, 0x77)
WHITE = (0xEA, 0xF2, 0xFA)
BG_TOP = (0x12, 0x1A, 0x24)
BG_BOTTOM = (0x07, 0x0A, 0x0F)
BORDER = (0x2B, 0x36, 0x44)
GPU = (0x6A, 0xA8, 0xFF)
RAM = (0xB4, 0x7B, 0xFF)


def mix(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def rounded_rect(x, y, left, top, right, bottom, radius):
    """SDF скруглённого прямоугольника: <0 внутри."""
    half_w = (right - left) / 2.0
    half_h = (bottom - top) / 2.0
    cx = left + half_w
    cy = top + half_h
    dx = abs(x - cx) - (half_w - radius)
    dy = abs(y - cy) - (half_h - radius)
    outside = math.hypot(max(dx, 0.0), max(dy, 0.0))
    inside = min(max(dx, dy), 0.0)
    return outside + inside - radius


def segment_distance(px, py, ax, ay, bx, by):
    vx, vy = bx - ax, by - ay
    wx, wy = px - ax, py - ay
    length_sq = vx * vx + vy * vy
    t = 0.0 if length_sq == 0.0 else max(0.0, min(1.0, (wx * vx + wy * vy) / length_sq))
    return math.hypot(px - (ax + vx * t), py - (ay + vy * t))


def sample(x, y):
    """Цвет и альфа в точке (0..1). Возвращает (r, g, b, a)."""
    result = (0.0, 0.0, 0.0, 0.0)

    body = rounded_rect(x, y, 0.015, 0.015, 0.985, 0.985, 0.225)
    if body < 0.0:
        t = max(0.0, min(1.0, y))
        r, g, b = mix(BG_TOP, BG_BOTTOM, t)
        # тонкая внутренняя подсветка сверху
        glow = 255.0 * max(0.0, 1.0 - y / 0.35) * 0.10
        result = ((r + glow) / 255.0, (g + glow) / 255.0, (b + glow) / 255.0, 1.0)

    ring = rounded_rect(x, y, 0.015, 0.015, 0.985, 0.985, 0.225)
    if -0.016 <= ring <= 0.0:
        result = (BORDER[0] / 255, BORDER[1] / 255, BORDER[2] / 255, 1.0)

    # Спидометр: дуга 235 градусов, открытая снизу.
    cx, cy, radius, thickness = 0.5, 0.49, 0.285, 0.072
    dx, dy = x - cx, y - cy
    distance = math.hypot(dx, dy)
    angle = math.degrees(math.atan2(-dy, dx))
    if angle < 147.5:
        angle += 360.0
    if 147.5 <= angle <= 392.5:
        if abs(distance - radius) <= thickness / 2.0:
            shade = mix(ACCENT_DARK, ACCENT, (angle - 147.5) / 245.0)
            result = (shade[0] / 255, shade[1] / 255, shade[2] / 255, 1.0)

    # Стрелка: от центра к правому верхнему сектору.
    tip_x = cx + math.cos(math.radians(48.0)) * (radius - 0.055)
    tip_y = cy - math.sin(math.radians(48.0)) * (radius - 0.055)
    if segment_distance(x, y, cx, cy, tip_x, tip_y) <= 0.030:
        result = (WHITE[0] / 255, WHITE[1] / 255, WHITE[2] / 255, 1.0)
    if distance <= 0.052:
        result = (ACCENT[0] / 255, ACCENT[1] / 255, ACCENT[2] / 255, 1.0)

    # Три столбика кадров под шкалой — читаются даже в 16 px.
    bars = [(0.395, 0.050, 0.865, 0.905, ACCENT), (0.485, 0.050, 0.835, 0.905, GPU), (0.575, 0.050, 0.805, 0.905, RAM)]
    for left, width, top, bottom, color in bars:
        if left <= x <= left + width and top <= y <= bottom:
            result = (color[0] / 255, color[1] / 255, color[2] / 255, 1.0)

    return result


def render(size, supersample):
    pixels = []
    step = 1.0 / (size * supersample)
    for row in range(size):
        for column in range(size):
            r = g = b = a = 0.0
            for sy in range(supersample):
                for sx in range(supersample):
                    px = (column * supersample + sx + 0.5) * step
                    py = (row * supersample + sy + 0.5) * step
                    sr, sg, sb, sa = sample(px, py)
                    r += sr * sa
                    g += sg * sa
                    b += sb * sa
                    a += sa
            count = supersample * supersample
            if a > 0.0:
                pixels.append((r / a, g / a, b / a, a / count))
            else:
                pixels.append((0.0, 0.0, 0.0, 0.0))
    return pixels


def to_bgra(pixels):
    out = bytearray()
    for r, g, b, a in pixels:
        out += bytes((
            0 if b <= 0.0 else min(255, int(b * 255.0 + 0.5)),
            0 if g <= 0.0 else min(255, int(g * 255.0 + 0.5)),
            0 if r <= 0.0 else min(255, int(r * 255.0 + 0.5)),
            0 if a <= 0.0 else min(255, int(a * 255.0 + 0.5)),
        ))
    return bytes(out)


def dib_entry(size, bgra):
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, size * size * 4, 0, 0, 0, 0)
    rows = bytearray()
    stride = size * 4
    for row in range(size - 1, -1, -1):
        rows += bgra[row * stride:(row + 1) * stride]
    mask_stride = ((size + 31) // 32) * 4
    mask = bytes(mask_stride * size)
    return header + bytes(rows) + mask


def write_ico(path, images):
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    directory = bytearray()
    payload = bytearray()
    for size, data in images:
        directory += struct.pack("<BBBBHHII", 0 if size == 256 else size, 0 if size == 256 else size, 0, 0, 1, 32,
                                 len(data), offset)
        payload += data
        offset += len(data)
    path.write_bytes(header + bytes(directory) + bytes(payload))


def write_png(path, size, pixels):
    raw = bytearray()
    for row in range(size):
        raw.append(0)
        for column in range(size):
            r, g, b, a = pixels[row * size + column]
            raw += bytes((
                min(255, int(r * 255.0 + 0.5)),
                min(255, int(g * 255.0 + 0.5)),
                min(255, int(b * 255.0 + 0.5)),
                min(255, int(a * 255.0 + 0.5)),
            ))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


def main():
    root = Path(__file__).resolve().parents[1]
    resources = root / "app" / "resources"
    resources.mkdir(parents=True, exist_ok=True)

    images = []
    for size in SIZES:
        pixels = render(size, 4 if size <= 64 else 2)
        images.append((size, dib_entry(size, to_bgra(pixels))))
        if size == 256:
            # Крупный PNG для документации: .ico в просмотрщиках выглядит по-разному.
            preview = root / "docs" / "screenshots" / "icon.png"
            preview.parent.mkdir(parents=True, exist_ok=True)
            write_png(preview, size, pixels)

    write_ico(resources / "yawa.ico", images)
    print(f"иконка: {resources / 'yawa.ico'}")


if __name__ == "__main__":
    main()
