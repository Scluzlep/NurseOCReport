#!/usr/bin/env python3
"""Fetch and verify C++ dependencies and PP-OCRv5 ncnn model assets.

All downloads go into git-ignored deps/ and models/ directories.
Supports optional --copy-android-assets to stage ncnn model files for APK builds.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import shutil
import socket
import tarfile
import urllib.error
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEPS_DIR = ROOT / "deps"
CACHE_DIR = DEPS_DIR / "_downloads"
MODELS_DIR = ROOT / "models"
ANDROID_ASSETS_DIR = ROOT / "android" / "app" / "src" / "main" / "assets"

ARTIFACTS = [
    {
        "name": "ncnn-20261020-full-source.zip",
        "url": "https://github.com/Tencent/ncnn/releases/download/20261020/ncnn-20261020-full-source.zip",
        "extract_to": "ncnn-src",
        "marker": "ncnn-src/CMakeLists.txt",
        "strip_top": True,
    },
    {
        "name": "libwebp-1.6.0.tar.gz",
        "url": "https://github.com/webmproject/libwebp/archive/refs/tags/v1.6.0.tar.gz",
        "extract_to": "libwebp",
        "marker": "libwebp/src/webp/decode.h",
        "strip_top": True,
    },
    {
        "name": "pugixml-1.16.zip",
        "url": "https://github.com/zeux/pugixml/releases/download/v1.16/pugixml-1.16.zip",
        "extract_to": "pugixml",
        "marker": "pugixml/src/pugixml.hpp",
        "strip_top": True,
    },
    {
        "name": "miniz-3.1.2.zip",
        "url": "https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip",
        "extract_to": "miniz",
        "marker": "miniz/miniz.c",
        "strip_top": False,
    },
    {
        "name": "nlohmann-json.hpp",
        "url": "https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp",
        "dest": "deps/nlohmann/json.hpp",
    },
    {
        "name": "stb_image.h",
        "url": "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h",
        "dest": "deps/stb/stb_image.h",
    },
    {
        "name": "YY-Thunks-Objs.zip",
        "url": "https://github.com/Chuyu-Team/YY-Thunks/releases/download/v1.2.3/YY-Thunks-Objs.zip",
        "extract_to": "yy-thunks",
        "marker": "yy-thunks/objs/x64/YY_Thunks_for_Win7.obj",
        "strip_top": False,
    },
]

PPOCR_BASE = "https://raw.githubusercontent.com/nihui/ncnn-android-ppocrv5/master/app/src/main"
MODEL_FILES = [
    ("PP_OCRv5_mobile_det.ncnn.param", f"{PPOCR_BASE}/assets/PP_OCRv5_mobile_det.ncnn.param", MODELS_DIR / "PP_OCRv5_mobile_det.ncnn.param"),
    ("PP_OCRv5_mobile_det.ncnn.bin", f"{PPOCR_BASE}/assets/PP_OCRv5_mobile_det.ncnn.bin", MODELS_DIR / "PP_OCRv5_mobile_det.ncnn.bin"),
    ("PP_OCRv5_mobile_rec.ncnn.param", f"{PPOCR_BASE}/assets/PP_OCRv5_mobile_rec.ncnn.param", MODELS_DIR / "PP_OCRv5_mobile_rec.ncnn.param"),
    ("PP_OCRv5_mobile_rec.ncnn.bin", f"{PPOCR_BASE}/assets/PP_OCRv5_mobile_rec.ncnn.bin", MODELS_DIR / "PP_OCRv5_mobile_rec.ncnn.bin"),
    ("ppocrv5_dict.h", f"{PPOCR_BASE}/jni/ppocrv5_dict.h", DEPS_DIR / "ppocr" / "ppocrv5_dict.h"),
]


def detect_proxy_opener() -> urllib.request.OpenerDirector | None:
    env_proxy = os.environ.get("HTTPS_PROXY") or os.environ.get("HTTP_PROXY")
    if env_proxy:
        return urllib.request.build_opener(
            urllib.request.ProxyHandler({"http": env_proxy, "https": env_proxy})
        )
    for host, port in (("127.0.0.1", 7897), ("192.168.3.101", 7897)):
        try:
            with socket.create_connection((host, port), timeout=0.4):
                proxy = f"http://{host}:{port}"
                return urllib.request.build_opener(
                    urllib.request.ProxyHandler({"http": proxy, "https": proxy})
                )
        except OSError:
            continue
    return None


def download_file(url: str, dest: pathlib.Path, proxy_opener: urllib.request.OpenerDirector | None) -> None:
    if dest.is_file() and dest.stat().st_size > 0:
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    headers = {"User-Agent": "NurseOCReport-Fetcher/1.0"}
    last_err: Exception | None = None
    openers = [urllib.request.build_opener()]
    if proxy_opener is not None:
        openers.append(proxy_opener)
    for opener in openers:
        try:
            req = urllib.request.Request(url, headers=headers)
            with opener.open(req, timeout=120) as resp, open(tmp, "wb") as out:
                shutil.copyfileobj(resp, out, length=256 * 1024)
            tmp.replace(dest)
            return
        except Exception as exc:
            last_err = exc
            if tmp.exists():
                tmp.unlink()
    raise RuntimeError(f"Failed to download {url}: {last_err}")


def extract_archive(archive_path: pathlib.Path, target_dir: pathlib.Path, strip_top: bool) -> None:
    if target_dir.exists():
        shutil.rmtree(target_dir)
    staging = target_dir.with_name(target_dir.name + "_staging")
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True, exist_ok=True)

    name = archive_path.name.lower()
    if name.endswith(".zip"):
        with zipfile.ZipFile(archive_path, "r") as zf:
            zf.extractall(staging)
    elif name.endswith(".tar.gz") or name.endswith(".tgz"):
        with tarfile.open(archive_path, "r:gz") as tf:
            tf.extractall(staging, filter="data")
    else:
        raise ValueError(f"Unsupported archive format: {archive_path}")

    entries = [p for p in staging.iterdir()]
    if strip_top and len(entries) == 1 and entries[0].is_dir():
        shutil.move(str(entries[0]), str(target_dir))
        shutil.rmtree(staging)
    else:
        staging.rename(target_dir)


def sha256_of(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1024 * 1024):
            h.update(chunk)
    return h.hexdigest()


def ensure_deps() -> None:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    MODELS_DIR.mkdir(parents=True, exist_ok=True)
    proxy_opener = detect_proxy_opener()

    for item in ARTIFACTS:
        if "dest" in item:
            dest = ROOT / item["dest"]
            print(f"[fetch] {item['name']} -> {dest.relative_to(ROOT)}")
            download_file(item["url"], dest, proxy_opener)
        else:
            marker = DEPS_DIR / item["marker"]
            if marker.is_file():
                print(f"[ok] {item['extract_to']} already present")
                continue
            archive = CACHE_DIR / item["name"]
            print(f"[fetch] {item['name']}")
            download_file(item["url"], archive, proxy_opener)
            print(f"[extract] {item['name']} -> deps/{item['extract_to']}")
            extract_archive(archive, DEPS_DIR / item["extract_to"], item["strip_top"])
            if not marker.is_file():
                raise RuntimeError(f"Expected marker missing after extract: {marker}")

    for label, url, dest in MODEL_FILES:
        print(f"[fetch] {label} -> {dest.relative_to(ROOT)}")
        download_file(url, dest, proxy_opener)


def copy_android_assets() -> None:
    ANDROID_ASSETS_DIR.mkdir(parents=True, exist_ok=True)
    for fn in (
        "PP_OCRv5_mobile_det.ncnn.param",
        "PP_OCRv5_mobile_det.ncnn.bin",
        "PP_OCRv5_mobile_rec.ncnn.param",
        "PP_OCRv5_mobile_rec.ncnn.bin",
    ):
        src = MODELS_DIR / fn
        if not src.is_file():
            raise RuntimeError(f"Model file missing: {src}")
        shutil.copy2(src, ANDROID_ASSETS_DIR / fn)
    print(f"[ok] Copied 4 ncnn model files to {ANDROID_ASSETS_DIR.relative_to(ROOT)}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--copy-android-assets", action="store_true")
    parser.add_argument("--print-hashes", action="store_true")
    args = parser.parse_args()

    ensure_deps()
    if args.copy_android_assets:
        copy_android_assets()
    if args.print_hashes:
        for _, _, dest in MODEL_FILES:
            print(f"{dest.name}: {sha256_of(dest)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
