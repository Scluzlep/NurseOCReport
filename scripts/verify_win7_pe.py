#!/usr/bin/env python3
"""Verify compiled Windows PE binaries target Windows 7 SP1 or earlier cleanly.

Checks PE subsystem version <= 6.01, zero external UCRT/VCRedist DLL imports
(ensuring /MT static CRT), and zero static imports of Win8+ system DLLs/APIs.
"""

from __future__ import annotations

import pathlib
import struct
import sys

FORBIDDEN_DLLS = (
    "ucrtbase",
    "api-ms-win-crt-",
    "vcruntime140",
    "msvcp140",
    "shcore.dll",
    "api-ms-win-core-synch-l1-2-0",
    "mfplat.dll",
    "d3d12.dll",
)

FORBIDDEN_WIN8_PLUS_APIS = {
    "WaitOnAddress",
    "WakeByAddressSingle",
    "WakeByAddressAll",
    "GetSystemTimePreciseAsFileTime",
    "CreateFile2",
    "GetDpiForWindow",
    "GetDpiForSystem",
    "GetDpiForMonitor",
    "SetProcessDpiAwareness",
    "SetProcessDpiAwarenessContext",
    "SetThreadDescription",
    "VirtualAlloc2",
    "DiscardVirtualMemory",
    "PrefetchVirtualMemory",
}


def rva_to_offset(rva: int, sections: list[tuple[int, int, int, int]]) -> int | None:
    for vaddr, vsize, raw_ptr, raw_size in sections:
        span = max(vsize, raw_size)
        if vaddr <= rva < vaddr + span:
            off = raw_ptr + (rva - vaddr)
            return off
    return None


def read_cstr(buf: bytes, off: int) -> str:
    end = buf.find(b"\x00", off)
    if end < 0:
        end = len(buf)
    return buf[off:end].decode("ascii", errors="replace")


def verify_pe(path: pathlib.Path) -> list[str]:
    errs: list[str] = []
    data = path.read_bytes()
    if len(data) < 0x100 or data[:2] != b"MZ":
        return [f"{path.name}: Not a valid PE file"]

    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_off + 24 > len(data) or data[pe_off : pe_off + 4] != b"PE\x00\x00":
        return [f"{path.name}: Invalid PE signature"]

    machine, num_sections, _, _, _, opt_size, _ = struct.unpack_from("<HHIIIHH", data, pe_off + 4)
    opt_off = pe_off + 24
    magic = struct.unpack_from("<H", data, opt_off)[0]
    is_pe32_plus = magic == 0x20B
    if magic not in (0x10B, 0x20B):
        return [f"{path.name}: Unknown optional header magic {magic:#x}"]

    major_os, minor_os, _, _, major_sub, minor_sub = struct.unpack_from("<HHHHHH", data, opt_off + 40)
    if (major_sub, minor_sub) > (6, 1):
        errs.append(f"{path.name}: Subsystem {major_sub}.{minor_sub:02d} exceeds Win7 (6.01)")
    if (major_os, minor_os) > (6, 1):
        errs.append(f"{path.name}: OS version {major_os}.{minor_os:02d} exceeds Win7 (6.01)")

    dd_off = opt_off + (112 if is_pe32_plus else 96)
    import_rva, import_sz = struct.unpack_from("<II", data, dd_off + 8)

    sec_off = opt_off + opt_size
    sections: list[tuple[int, int, int, int]] = []
    for i in range(num_sections):
        off = sec_off + i * 40
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((vaddr, vsize, raw_ptr, raw_size))

    if import_rva != 0 and import_sz > 0:
        imp_off = rva_to_offset(import_rva, sections)
        if imp_off is None:
            errs.append(f"{path.name}: Unable to resolve import directory RVA")
            return errs
        cur = imp_off
        while cur + 20 <= len(data):
            orig_first_thunk, _, _, name_rva, first_thunk = struct.unpack_from("<IIIII", data, cur)
            if name_rva == 0:
                break
            cur += 20
            name_off = rva_to_offset(name_rva, sections)
            if name_off is None:
                continue
            dll = read_cstr(data, name_off)
            dll_lower = dll.lower()
            for bad in FORBIDDEN_DLLS:
                if bad in dll_lower:
                    errs.append(f"{path.name}: Forbidden static DLL dependency '{dll}'")

            thunk_rva = orig_first_thunk if orig_first_thunk != 0 else first_thunk
            thunk_off = rva_to_offset(thunk_rva, sections)
            if thunk_off is None:
                continue
            step = 8 if is_pe32_plus else 4
            ord_flag = (1 << 63) if is_pe32_plus else (1 << 31)
            tpos = thunk_off
            while tpos + step <= len(data):
                val = struct.unpack_from("<Q" if is_pe32_plus else "<I", data, tpos)[0]
                tpos += step
                if val == 0:
                    break
                if val & ord_flag:
                    continue
                hint_off = rva_to_offset(int(val & 0x7FFFFFFF), sections)
                if hint_off is not None and hint_off + 2 < len(data):
                    fn = read_cstr(data, hint_off + 2)
                    if fn in FORBIDDEN_WIN8_PLUS_APIS:
                        errs.append(f"{path.name}: Forbidden Win8+ static API '{fn}' from {dll}")

    arch_str = "x64" if machine == 0x8664 else ("x86" if machine == 0x14C else hex(machine))
    if not errs:
        print(f"[OK] {path.name} ({arch_str}, Subsystem {major_sub}.{minor_sub:02d}, OS {major_os}.{minor_os:02d}) passed Win7 SP1 PE check.")
    return errs


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: verify_win7_pe.py <path-to-exe-or-dll> [...]", file=sys.stderr)
        return 2
    all_errs: list[str] = []
    for arg in sys.argv[1:]:
        p = pathlib.Path(arg)
        if not p.is_file():
            all_errs.append(f"Missing binary: {p}")
        else:
            all_errs.extend(verify_pe(p))
    if all_errs:
        for e in all_errs:
            print(f"[PE CHECK ERROR] {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
