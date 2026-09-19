#!/usr/bin/env python3
"""Copy MinGW runtime DLLs next to openartemis.exe so it can run without PATH."""
from __future__ import annotations

import argparse
import shutil
import struct
import sys
from pathlib import Path

SYSTEM_DLLS = {
    "advapi32.dll", "bcrypt.dll", "bcryptprimitives.dll", "combase.dll",
    "crypt32.dll", "gdi32.dll", "gdi32full.dll", "imm32.dll", "kernel32.dll",
    "kernelbase.dll", "msimg32.dll", "msvcp_win.dll", "msvcrt.dll",
    "ntdll.dll", "ole32.dll", "oleaut32.dll", "rpcrt4.dll", "sechost.dll",
    "setupapi.dll", "shell32.dll", "shlwapi.dll", "ucrtbase.dll",
    "user32.dll", "userenv.dll", "win32u.dll", "ws2_32.dll", "dwmapi.dll",
    "version.dll", "winmm.dll", "cfgmgr32.dll", "hid.dll", "opengl32.dll",
    "glu32.dll", "dinput8.dll", "xinput1_4.dll", "xinput1_3.dll",
    "xinput9_1_0.dll", "imm32.dll", "nsi.dll", "iphlpapi.dll",
}


def rva_to_off(secs, rva: int) -> int | None:
    for va, vsz, raw, rawsz in secs:
        if va <= rva < va + max(vsz, rawsz):
            return raw + (rva - va)
    return None


def read_import_names(path: Path, delay: bool = False) -> list[str]:
    data = path.read_bytes()
    if data[0:2] != b"MZ":
        return []
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        return []
    magic = struct.unpack_from("<H", data, e_lfanew + 24)[0]
    dd_off = e_lfanew + 24 + (112 if magic == 0x20B else 96)
    idx = 13 if delay else 1
    import_rva, _size = struct.unpack_from("<II", data, dd_off + idx * 8)
    if import_rva == 0:
        return []
    nsec = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    sizeopt = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    sec_off = e_lfanew + 24 + sizeopt
    secs = []
    for i in range(nsec):
        o = sec_off + i * 40
        vsz, va, rawsz, raw = struct.unpack_from("<IIII", data, o + 8)
        secs.append((va, vsz, raw, rawsz))
    off = rva_to_off(secs, import_rva)
    if off is None:
        return []
    names: list[str] = []
    while True:
        if delay:
            attrs, name_rva, _mod, _iat, _int = struct.unpack_from("<IIIII", data, off)
            if attrs == 0 and name_rva == 0:
                break
            rec_size = 32 if magic == 0x20B else 20
            # IMAGE_DELAYLOAD_DESCRIPTOR is 32 bytes
            rec_size = 32
            name_rva = struct.unpack_from("<I", data, off + 4)[0]
            off += rec_size
        else:
            orig, _t, _fwd, name_rva, _ft = struct.unpack_from("<IIIII", data, off)
            if orig == 0 and name_rva == 0:
                break
            off += 20
        if not name_rva:
            continue
        no = rva_to_off(secs, name_rva)
        if no is None:
            continue
        end = data.find(b"\0", no)
        names.append(data[no:end].decode("ascii", "replace"))
    return names


def is_system_dll(name: str) -> bool:
    n = name.lower()
    if n in SYSTEM_DLLS:
        return True
    if n.startswith("api-ms-win-") or n.startswith("ext-ms-"):
        return True
    if n.startswith("vcruntime") or n.startswith("msvcp"):
        return True
    return False


def collect(exe: Path, search_dirs: list[Path]) -> list[Path]:
    needed: dict[str, Path] = {}
    queue = [exe]
    seen: set[str] = set()
    missing: list[str] = []
    while queue:
        current = queue.pop()
        key = current.name.lower()
        if key in seen:
            continue
        seen.add(key)
        for delay in (False, True):
            try:
                imports = read_import_names(current, delay=delay)
            except Exception:
                imports = []
            for name in imports:
                ln = name.lower()
                if ln in seen or is_system_dll(ln):
                    continue
                found = None
                for d in search_dirs:
                    cand = d / name
                    if cand.exists():
                        found = cand
                        break
                    matches = list(d.glob(name))
                    if matches:
                        found = matches[0]
                        break
                if found:
                    needed[ln] = found
                    queue.append(found)
                else:
                    missing.append(name)
    if missing:
        uniq = sorted(set(missing), key=str.lower)
        print("warning: unresolved dlls (usually API-set/system):", file=sys.stderr)
        for n in uniq:
            if not is_system_dll(n):
                print("  ", n, file=sys.stderr)
    return list(needed.values())


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--mingw-bin", default=r"C:\msys64\mingw64\bin")
    ap.add_argument("--dest", default="")
    args = ap.parse_args()
    exe = Path(args.exe).resolve()
    if not exe.exists():
        print("exe not found:", exe, file=sys.stderr)
        return 1
    dest = Path(args.dest).resolve() if args.dest else exe.parent
    dest.mkdir(parents=True, exist_ok=True)
    search = [Path(args.mingw_bin), exe.parent]
    dlls = collect(exe, search)
    copied = 0
    for src in sorted(dlls, key=lambda p: p.name.lower()):
        out = dest / src.name
        if out.resolve() == src.resolve():
            continue
        shutil.copy2(src, out)
        copied += 1
        print("copied", src.name)
    print(f"copied {copied} dlls -> {dest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
