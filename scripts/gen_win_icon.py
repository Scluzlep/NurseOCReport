#!/usr/bin/env python3
"""Generate multi-size windows/app.ico matching the Android launcher vector icon."""

import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT_ICO = ROOT / "windows" / "app.ico"

# Android ic_launcher_background (#1E5AA8) + white table glyph (#FFFFFF)
BG_RGB = (0x1E, 0x5A, 0xA8)
FG_RGB = (0xFF, 0xFF, 0xFF)


def point_in_rounded_rect(x: float, y: float, x0: float, y0: float, x1: float, y1: float, r: float) -> bool:
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    dx = x - cx
    dy = y - cy
    return (dx * dx + dy * dy) <= (r * r + 1e-9)


def sample_icon_rgba(px: float, py: float) -> tuple[int, int, int, int]:
    """Sample normalized coordinates [0..1] x [0..1] -> (R, G, B, A)."""
    # Slight outer padding (6%) with rounded corners (radius 22%) for crisp squircle/rounded-square icon
    if not point_in_rounded_rect(px, py, 0.06, 0.06, 0.94, 0.94, 0.22):
        return (0, 0, 0, 0)

    # Outer table glyph matching Android ic_launcher_foreground [22%..78%]
    # Inside: cutouts showing #1E5AA8 for top header row and 2 bottom cells
    in_outer = 0.22 <= px <= 0.78 and 0.22 <= py <= 0.78
    if in_outer:
        in_top_cutout = 0.30 <= px <= 0.70 and 0.30 <= py <= 0.44
        in_bl_cutout = 0.30 <= px <= 0.47 and 0.52 <= py <= 0.70
        in_br_cutout = 0.53 <= px <= 0.70 and 0.52 <= py <= 0.70
        if not (in_top_cutout or in_bl_cutout or in_br_cutout):
            return (FG_RGB[0], FG_RGB[1], FG_RGB[2], 255)

    return (BG_RGB[0], BG_RGB[1], BG_RGB[2], 255)


def render_rgba_pixels(size: int) -> bytes:
    """Render size x size RGBA8888 (top-to-bottom) with 4x4 supersampling."""
    sub = 4
    inv = 1.0 / (size * sub)
    out = bytearray(size * size * 4)
    idx = 0
    for y in range(size):
        for x in range(size):
            r_acc = 0.0
            g_acc = 0.0
            b_acc = 0.0
            a_acc = 0.0
            for sy in range(sub):
                py = (y * sub + sy + 0.5) * inv
                for sx in range(sub):
                    px = (x * sub + sx + 0.5) * inv
                    r, g, b, a = sample_icon_rgba(px, py)
                    af = a / 255.0
                    r_acc += r * af
                    g_acc += g * af
                    b_acc += b * af
                    a_acc += a
            n = float(sub * sub)
            alpha_f = a_acc / (255.0 * n)
            if alpha_f > 1e-6:
                rr = int(round(min(255.0, r_acc / (n * alpha_f))))
                gg = int(round(min(255.0, g_acc / (n * alpha_f))))
                bb = int(round(min(255.0, b_acc / (n * alpha_f))))
            else:
                rr = gg = bb = 0
            aa = int(round(min(255.0, a_acc / n)))
            out[idx] = rr
            out[idx + 1] = gg
            out[idx + 2] = bb
            out[idx + 3] = aa
            idx += 4
    return bytes(out)


def encode_png_rgba(size: int, rgba: bytes) -> bytes:
    raw = bytearray()
    row_bytes = size * 4
    for y in range(size):
        raw.append(0)
        start = y * row_bytes
        raw.extend(rgba[start:start + row_bytes])

    def chunk(ctype: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(ctype + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + ctype + data + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    idat = zlib.compress(bytes(raw), 9)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")


def encode_dib_bgra32(size: int, rgba: bytes) -> bytes:
    """32-bit bottom-up BGRA BITMAPINFOHEADER + AND mask for Win7 SP1 icon compatibility."""
    header = struct.pack(
        "<IiiHHIIiiII",
        40,           # biSize
        size,         # biWidth
        size * 2,     # biHeight (XOR + AND)
        1,            # biPlanes
        32,           # biBitCount
        0,            # biCompression (BI_RGB)
        0,            # biSizeImage
        0, 0, 0, 0,
    )
    xor_buf = bytearray(size * size * 4)
    and_stride = ((size + 31) // 32) * 4
    and_buf = bytearray(and_stride * size)
    for y in range(size):
        src_y = size - 1 - y
        for x in range(size):
            si = (src_y * size + x) * 4
            di = (y * size + x) * 4
            r, g, b, a = rgba[si], rgba[si + 1], rgba[si + 2], rgba[si + 3]
            xor_buf[di] = b
            xor_buf[di + 1] = g
            xor_buf[di + 2] = r
            xor_buf[di + 3] = a
            if a == 0:
                and_buf[y * and_stride + (x >> 3)] |= 1 << (7 - (x & 7))
    return header + bytes(xor_buf) + bytes(and_buf)


def build_ico(out_path: pathlib.Path) -> None:
    sizes = [16, 24, 32, 48, 64, 256]
    blobs: list[tuple[int, bytes]] = []
    for sz in sizes:
        rgba = render_rgba_pixels(sz)
        blob = encode_png_rgba(sz, rgba) if sz == 256 else encode_dib_bgra32(sz, rgba)
        blobs.append((sz, blob))

    # ICONDIR header (6 bytes) + N * ICONDIRENTRY (16 bytes)
    header = struct.pack("<HHH", 0, 1, len(blobs))
    offset = 6 + 16 * len(blobs)
    entries = bytearray()
    payload = bytearray()
    for sz, blob in blobs:
        w_byte = 0 if sz == 256 else sz
        h_byte = 0 if sz == 256 else sz
        entries.extend(struct.pack("<BBBBHHII", w_byte, h_byte, 0, 0, 1, 32, len(blob), offset))
        payload.extend(blob)
        offset += len(blob)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(header + bytes(entries) + bytes(payload))


def main() -> None:
    build_ico(OUT_ICO)
    print(f"Generated {OUT_ICO} ({OUT_ICO.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
