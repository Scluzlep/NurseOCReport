#!/usr/bin/env python3
"""Pre-commit and CI privacy guard for NurseOCReport.

Ensures no archived personal data, original photos, spreadsheets, signing keys,
or real staff/department names are tracked in Git.
"""

from __future__ import annotations

import csv
import hashlib
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ARCHIVE_DIR = ROOT / "NurseOCR_讨论归档_2026-11-22"

FORBIDDEN_PATH_PARTS = (
    "NurseOCR_讨论归档_2026-11-22",
    "测试方法报告.md",
)

FORBIDDEN_EXTENSIONS = {
    ".p12",
    ".pfx",
    ".jks",
    ".keystore",
    ".key",
    ".pem",
    ".xlsx",
    ".xls",
    ".jpg",
    ".jpeg",
    ".webp",
    ".bmp",
}


def load_sensitive_tokens() -> list[str]:
    tokens: set[str] = set()
    csv_path = ARCHIVE_DIR / "config" / "staff_2026_08.csv"
    if csv_path.is_file():
        text = csv_path.read_text(encoding="utf-8-sig")
        reader = csv.reader(text.splitlines())
        header = next(reader, None)
        for row in reader:
            if len(row) >= 2:
                name = row[1].strip()
                if name:
                    tokens.add(name)
                    if len(name) == 3:
                        tokens.add(name[:2] + "研")
    excel_dir = ARCHIVE_DIR / "excel"
    if excel_dir.is_dir():
        for entry in excel_dir.iterdir():
            if "_" in entry.name:
                prefix = entry.name.split("_", 1)[0].strip()
                if len(prefix) >= 2:
                    tokens.add(prefix)
    return sorted(tokens)


def git_tracked_and_staged_files() -> list[pathlib.Path]:
    git_dir = ROOT / ".git"
    if not git_dir.exists():
        return []
    files: set[str] = set()
    for args in (["git", "ls-files", "-z"], ["git", "diff", "--cached", "--name-only", "-z"]):
        proc = subprocess.run(
            args,
            cwd=str(ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if proc.returncode == 0 and proc.stdout:
            for item in proc.stdout.split(b"\x00"):
                if item:
                    files.add(item.decode("utf-8", "replace"))
    return [ROOT / f for f in sorted(files)]


def main() -> int:
    errors: list[str] = []
    files = git_tracked_and_staged_files()
    tokens = load_sensitive_tokens()

    for path in files:
        rel = path.relative_to(ROOT).as_posix()
        for bad in FORBIDDEN_PATH_PARTS:
            if bad in rel:
                errors.append(f"Forbidden path tracked in git: {rel}")
        ext = path.suffix.lower()
        if ext in FORBIDDEN_EXTENSIONS:
            errors.append(f"Forbidden binary/data extension tracked in git: {rel}")
        if not path.is_file():
            continue
        try:
            raw = path.read_bytes()
        except OSError as exc:
            errors.append(f"Cannot read {rel}: {exc}")
            continue
        if b"\x00" in raw[:4096]:
            continue
        text = raw.decode("utf-8", "ignore")
        for tok in tokens:
            if tok and tok in text:
                digest = hashlib.sha256(tok.encode("utf-8")).hexdigest()[:12]
                errors.append(f"Sensitive token (sha256:{digest}) found in {rel}")
        # Also block non-ASCII emoji characters outside downloaded deps
        for ch in text:
            cp = ord(ch)
            if (0x1F300 <= cp <= 0x1FAFF) or (0x2600 <= cp <= 0x27BF):
                errors.append(f"Disallowed symbol/emoji U+{cp:04X} in {rel}")
                break

    if errors:
        for err in errors:
            print(f"[PRIVACY/POLICY ERROR] {err}", file=sys.stderr)
        return 1

    print(f"Privacy and policy check passed ({len(files)} tracked files scanned).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
