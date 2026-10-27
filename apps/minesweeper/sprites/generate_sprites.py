#!/usr/bin/env python3
"""Generate Minesweeper sprites for PicoDeck (32x32 pixels)."""

import struct
import zlib
import os

SPRITE_DIR = os.path.dirname(os.path.abspath(__file__))
W, H = 32, 32


def create_png(width, height, pixels):
    """Create a PNG file from RGB565 pixel data."""
    def crc32(data):
        return struct.pack('>I', zlib.crc32(data) & 0xffffffff)

    def chunk(chunk_type, data):
        return struct.pack('>I', len(data)) + chunk_type + data + crc32(chunk_type + data)

    signature = b'\x89PNG\r\n\x1a\n'
    ihdr_data = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    ihdr = chunk(b'IHDR', ihdr_data)

    raw_data = b''
    for y in range(height):
        raw_data += b'\x00'
        for x in range(width):
            rgb = pixels[y * width + x]
            r5 = (rgb >> 11) & 0x1F
            g6 = (rgb >> 5) & 0x3F
            b5 = rgb & 0x1F
            r = (r5 * 527 + 23) >> 6
            g = (g6 * 259 + 33) >> 6
            b = (b5 * 527 + 23) >> 6
            raw_data += bytes([r, g, b])

    compressed = zlib.compress(raw_data, 9)
    idat = chunk(b'IDAT', compressed)
    iend = chunk(b'IEND', b'')

    return signature + ihdr + idat + iend


def rgb565(r, g, b):
    """Convert RGB888 to RGB565."""
    return (((r >> 3) & 0x1F) << 11) | (((g >> 2) & 0x3F) << 5) | ((b >> 3) & 0x1F)


def fill(pixels, color):
    """Fill all pixels with a color."""
    for i in range(len(pixels)):
        pixels[i] = color


def fill_rect(pixels, x0, y0, w, h, color):
    """Fill a rectangle."""
    for y in range(y0, min(y0 + h, H)):
        for x in range(x0, min(x0 + w, W)):
            pixels[y * W + x] = color


def fill_circle(pixels, cx, cy, r, color):
    """Fill a circle."""
    for y in range(H):
        for x in range(W):
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                pixels[y * W + x] = color


def draw_thick_line(pixels, points, color, thickness=2):
    """Draw thick points (each point becomes a thickness x thickness block)."""
    half = thickness // 2
    for px, py in points:
        for dy in range(-half, half + thickness % 2):
            for dx in range(-half, half + thickness % 2):
                nx, ny = px + dx, py + dy
                if 0 <= nx < W and 0 <= ny < H:
                    pixels[ny * W + nx] = color


# ── Sprite generators ─────────────────────────────────────────────────────────

def create_covered_sprite():
    """Beveled raised cell."""
    pixels = [rgb565(75, 75, 80)] * (W * H)

    # Inner fill
    fill_rect(pixels, 3, 3, W - 6, H - 6, rgb565(85, 85, 90))

    # Top highlight (2px)
    for t in range(2):
        for x in range(W):
            pixels[t * W + x] = rgb565(110, 110, 115)
    # Left highlight (2px)
    for y in range(H):
        for t in range(2):
            pixels[y * W + t] = rgb565(110, 110, 115)

    # Bottom shadow (2px)
    for t in range(2):
        for x in range(W):
            pixels[(H - 1 - t) * W + x] = rgb565(40, 40, 45)
    # Right shadow (2px)
    for y in range(H):
        for t in range(2):
            pixels[y * W + (W - 1 - t)] = rgb565(40, 40, 45)

    return create_png(W, H, pixels)


def create_revealed_sprite():
    """Flat revealed cell with subtle border."""
    bg = rgb565(190, 190, 190)
    border = rgb565(160, 160, 160)
    pixels = [bg] * (W * H)

    # 1px border on all edges
    for x in range(W):
        pixels[x] = border
        pixels[(H - 1) * W + x] = border
    for y in range(H):
        pixels[y * W] = border
        pixels[y * W + W - 1] = border

    return create_png(W, H, pixels)


def create_mine_sprite():
    """Black mine on revealed background."""
    bg = rgb565(190, 190, 190)
    border = rgb565(160, 160, 160)
    pixels = [bg] * (W * H)

    # Border
    for x in range(W):
        pixels[x] = border
        pixels[(H - 1) * W + x] = border
    for y in range(H):
        pixels[y * W] = border
        pixels[y * W + W - 1] = border

    cx, cy = 15, 16
    mine_color = rgb565(20, 20, 20)
    highlight = rgb565(80, 80, 80)

    # Main body
    fill_circle(pixels, cx, cy, 8, mine_color)

    # Spikes (cross pattern)
    for i in range(-10, 11):
        for t in range(-1, 2):
            # Horizontal
            nx, ny = cx + i, cy + t
            if 0 <= nx < W and 0 <= ny < H and abs(i) > 6:
                pixels[ny * W + nx] = mine_color
            # Vertical
            nx, ny = cx + t, cy + i
            if 0 <= nx < W and 0 <= ny < H and abs(i) > 6:
                pixels[ny * W + nx] = mine_color

    # Diagonal spikes
    for i in range(-8, 9):
        if abs(i) > 5:
            for t in range(-1, 2):
                nx1, ny1 = cx + i + t, cy + i
                nx2, ny2 = cx + i + t, cy - i
                if 0 <= nx1 < W and 0 <= ny1 < H:
                    pixels[ny1 * W + nx1] = mine_color
                if 0 <= nx2 < W and 0 <= ny2 < H:
                    pixels[ny2 * W + nx2] = mine_color

    # Highlight dot
    fill_circle(pixels, cx - 3, cy - 3, 2, highlight)

    return create_png(W, H, pixels)


def create_flag_sprite():
    """Red flag on pole over covered cell."""
    bg = rgb565(190, 190, 190)
    border = rgb565(160, 160, 160)
    pixels = [bg] * (W * H)

    # Border
    for x in range(W):
        pixels[x] = border
        pixels[(H - 1) * W + x] = border
    for y in range(H):
        pixels[y * W] = border
        pixels[y * W + W - 1] = border

    pole_color = rgb565(60, 60, 60)
    flag_color = rgb565(220, 30, 30)
    flag_dark = rgb565(180, 20, 20)
    base_color = rgb565(60, 60, 60)

    # Pole (2px wide)
    fill_rect(pixels, 16, 4, 2, 22, pole_color)

    # Flag (triangle pointing right)
    for row in range(10):
        fw = 10 - row
        y = 4 + row
        c = flag_color if row < 5 else flag_dark
        fill_rect(pixels, 8, y, fw, 1, c)

    # Base
    fill_rect(pixels, 11, 24, 12, 2, base_color)
    fill_rect(pixels, 13, 26, 8, 2, base_color)

    return create_png(W, H, pixels)


def create_question_sprite():
    """Question mark on revealed background."""
    bg = rgb565(190, 190, 190)
    border = rgb565(160, 160, 160)
    pixels = [bg] * (W * H)

    # Border
    for x in range(W):
        pixels[x] = border
        pixels[(H - 1) * W + x] = border
    for y in range(H):
        pixels[y * W] = border
        pixels[y * W + W - 1] = border

    color = rgb565(80, 80, 200)

    # Question mark glyph (2px thick strokes)
    # Top arc
    pts = []
    # Top horizontal
    for x in range(12, 21):
        pts.append((x, 5))
        pts.append((x, 6))
    # Right side going down
    for y in range(7, 14):
        pts.append((20, y))
        pts.append((21, y))
    # Top-left curve
    pts.append((11, 7))
    pts.append((10, 7))
    pts.append((10, 8))
    # Middle horizontal
    for x in range(14, 21):
        pts.append((x, 14))
        pts.append((x, 15))
    # Stem going down
    for y in range(16, 22):
        pts.append((14, y))
        pts.append((15, y))
    # Dot
    for dy in range(3):
        for dx in range(3):
            pts.append((14 + dx, 24 + dy))

    draw_thick_line(pixels, pts, color, thickness=1)

    return create_png(W, H, pixels)


def create_exploded_sprite():
    """Red background with black mine (exploded)."""
    bg = rgb565(220, 50, 50)
    pixels = [bg] * (W * H)

    # Darker red border
    dark_border = rgb565(180, 30, 30)
    for x in range(W):
        pixels[x] = dark_border
        pixels[(H - 1) * W + x] = dark_border
    for y in range(H):
        pixels[y * W] = dark_border
        pixels[y * W + W - 1] = dark_border

    cx, cy = 15, 16
    mine_color = rgb565(20, 20, 20)

    # Main body
    fill_circle(pixels, cx, cy, 8, mine_color)

    # Spikes
    for i in range(-10, 11):
        for t in range(-1, 2):
            nx, ny = cx + i, cy + t
            if 0 <= nx < W and 0 <= ny < H and abs(i) > 6:
                pixels[ny * W + nx] = mine_color
            nx, ny = cx + t, cy + i
            if 0 <= nx < W and 0 <= ny < H and abs(i) > 6:
                pixels[ny * W + nx] = mine_color

    for i in range(-8, 9):
        if abs(i) > 5:
            for t in range(-1, 2):
                nx1, ny1 = cx + i + t, cy + i
                nx2, ny2 = cx + i + t, cy - i
                if 0 <= nx1 < W and 0 <= ny1 < H:
                    pixels[ny1 * W + nx1] = mine_color
                if 0 <= nx2 < W and 0 <= ny2 < H:
                    pixels[ny2 * W + nx2] = mine_color

    # White highlight
    fill_circle(pixels, cx - 3, cy - 3, 2, rgb565(200, 200, 200))

    return create_png(W, H, pixels)


def create_number_sprite(num, color_rgb):
    """Number sprite with bold 7-segment font, centered in 32x32 cell."""
    bg = rgb565(190, 190, 190)
    border = rgb565(160, 160, 160)
    pixels = [bg] * (W * H)

    # Border
    for x in range(W):
        pixels[x] = border
        pixels[(H - 1) * W + x] = border
    for y in range(H):
        pixels[y * W] = border
        pixels[y * W + W - 1] = border

    r, g, b = color_rgb
    color = rgb565(r, g, b)

    # Bold 7-segment style on a 14x20 glyph area, centered in 32x32
    # Segments are 3px thick, glyph is 14px wide x 20px tall
    # Format: (x, y, w, h) relative to glyph origin
    T = 3   # stroke thickness
    GW = 14  # glyph width
    GH = 20  # glyph height
    ox = (W - GW) // 2  # 9
    oy = (H - GH) // 2  # 6

    # Segment positions within glyph:
    #  AAA
    # F   B
    # F   B
    #  GGG
    # E   C
    # E   C
    #  DDD
    seg_a = (0, 0, GW, T)                          # top horizontal
    seg_b = (GW - T, 0, T, GH // 2 + 1)            # upper-right vertical
    seg_c = (GW - T, GH // 2, T, GH // 2 + 1)      # lower-right vertical
    seg_d = (0, GH - T + 1, GW, T)                 # bottom horizontal
    seg_e = (0, GH // 2, T, GH // 2 + 1)            # lower-left vertical
    seg_f = (0, 0, T, GH // 2 + 1)                  # upper-left vertical
    seg_g = (0, GH // 2 - 1, GW, T)                 # middle horizontal

    # Which segments are active for each digit
    digit_segments = {
        1: [seg_b, seg_c],
        2: [seg_a, seg_b, seg_g, seg_e, seg_d],
        3: [seg_a, seg_b, seg_g, seg_c, seg_d],
        4: [seg_f, seg_g, seg_b, seg_c],
        5: [seg_a, seg_f, seg_g, seg_c, seg_d],
        6: [seg_a, seg_f, seg_g, seg_e, seg_c, seg_d],
        7: [seg_a, seg_b, seg_c],
        8: [seg_a, seg_b, seg_c, seg_d, seg_e, seg_f, seg_g],
    }

    for sx, sy, sw, sh in digit_segments.get(num, []):
        fill_rect(pixels, ox + sx, oy + sy, sw, sh, color)

    return create_png(W, H, pixels)


# ── Generate all sprites ──────────────────────────────────────────────────────

sprite_generators = {
    'covered.png': create_covered_sprite,
    'revealed_empty.png': create_revealed_sprite,
    'mine.png': create_mine_sprite,
    'flagged.png': create_flag_sprite,
    'question.png': create_question_sprite,
    'mine_exploded.png': create_exploded_sprite,
    'n1.png': lambda: create_number_sprite(1, (30, 90, 220)),
    'n2.png': lambda: create_number_sprite(2, (30, 160, 50)),
    'n3.png': lambda: create_number_sprite(3, (220, 50, 50)),
    'n4.png': lambda: create_number_sprite(4, (130, 40, 180)),
    'n5.png': lambda: create_number_sprite(5, (150, 40, 40)),
    'n6.png': lambda: create_number_sprite(6, (30, 150, 150)),
    'n7.png': lambda: create_number_sprite(7, (50, 50, 50)),
    'n8.png': lambda: create_number_sprite(8, (120, 120, 120)),
}

for filename, generator in sprite_generators.items():
    path = os.path.join(SPRITE_DIR, filename)
    with open(path, 'wb') as f:
        f.write(generator())
    print(f'Created {filename}')

print('Done!')
