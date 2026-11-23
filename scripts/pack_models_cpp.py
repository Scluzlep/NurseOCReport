#!/usr/bin/env python3
"""Pack NCNN OCR model files into a zlib-compressed binary bundle for Win32 RCDATA.

Binary layout (little-endian):
  - magic:      8 bytes  b"NOCRMPAK"
  - version:    uint32   1
  - hash16:     16 bytes ASCII lowercase hex prefix of SHA-256 over all entries
  - count:      uint32   4
  - entries[count]:
      - name_len:   uint32
      - raw_size:   uint64
      - comp_size:  uint64
      - name_utf8:  name_len bytes
      - zlib_data:  comp_size bytes (zlib.compress(raw_bytes, 9))
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct
import sys
import zlib

MAGIC = b"NOCRMPAK"
VERSION = 1

REQUIRED_MODELS = (
    "PP_OCRv5_mobile_det.ncnn.param",
    "PP_OCRv5_mobile_det.ncnn.bin",
    "PP_OCRv5_mobile_rec.ncnn.param",
    "PP_OCRv5_mobile_rec.ncnn.bin",
)


def pack_models(models_dir: pathlib.Path, out_path: pathlib.Path) -> None:
    hasher = hashlib.sha256()
    entries: list[tuple[bytes, int, bytes]] = []

    for name in REQUIRED_MODELS:
        src = models_dir / name
        if not src.is_file():
            raise FileNotFoundError(f"Required model file missing: {src}")
        raw = src.read_bytes()
        name_bytes = name.encode("utf-8")
        hasher.update(name_bytes)
        hasher.update(b"\x00")
        hasher.update(struct.pack("<Q", len(raw)))
        hasher.update(raw)
        comp = zlib.compress(raw, 9)
        entries.append((name_bytes, len(raw), comp))

    hash16 = hasher.hexdigest()[:16].encode("ascii")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = out_path.with_suffix(out_path.suffix + ".tmp")

    with tmp_path.open("wb") as fh:
        fh.write(MAGIC)
        fh.write(struct.pack("<I", VERSION))
        fh.write(hash16)
        fh.write(struct.pack("<I", len(entries)))
        for name_bytes, raw_size, comp in entries:
            fh.write(struct.pack("<IQQ", len(name_bytes), raw_size, len(comp)))
            fh.write(name_bytes)
            fh.write(comp)

    tmp_path.replace(out_path)
    total_raw = sum(r for _, r, _ in entries)
    total_comp = sum(len(c) for _, _, c in entries)
    print(
        f"Packed {len(entries)} models into {out_path} "
        f"(hash16={hash16.decode('ascii')}, raw={total_raw} B, compressed={total_comp} B)"
    )


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="Pack NCNN OCR models for Windows RCDATA.")
    parser.add_argument(
        "--models-dir",
        type=pathlib.Path,
        default=repo_root / "models",
        help="Directory containing PP_OCRv5_mobile_{det,rec}.ncnn.{param,bin}",
    )
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        required=True,
        help="Output .pack file path",
    )
    args = parser.parse_args()
    try:
        pack_models(args.models_dir, args.output)
    except Exception as exc:
        print(f"[pack_models_cpp] ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
