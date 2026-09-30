#!/usr/bin/env python3
"""Artemis Compatibility Lab — static game scanner (TASK-008 / M4).

Scans a local Artemis package (root.pfs [+ volumes] and/or sidecar root/ dir)
and outputs a FEATURE MANIFEST: tag/ASB-command census, Lua surface census
(e:var system variables, tags.* handlers, e:tag/e:enqueueTag targets),
system.ini geometry, asset-type counts. Counts and names only — no game
asset bytes ever leave the machine (plan §6).

Usage:
  python scan_game.py <game_root_dir> [--out profile.json]

game_root_dir = directory containing root.pfs / root/ (e.g. D:/Games/Pomelo/gzsq)
"""

import collections
import hashlib
import json
import os
import re
import struct
import sys

# ---------------------------------------------------------------- PFS layer

class PfsArchive:
    """pf2/pf6/pf8 reader: index + payload (pf8 XOR per engine rule)."""

    def __init__(self, first_volume_path):
        # 卷发现与引擎 sibling_volumes 同约定：<stem>.NNN 全枚举、按号排序、
        # 允许缺号（NUKITASHI 实测 .000/.010/.020 布局）。
        import glob as _glob
        d, b = os.path.split(first_volume_path)
        cands = []
        for p in _glob.glob(os.path.join(d, b + ".[0-9][0-9][0-9]")):
            suffix = p[len(first_volume_path) + 1:]
            if len(suffix) == 3 and suffix.isdigit():
                cands.append((int(suffix), p))
        self.volumes = [first_volume_path] + [p for _, p in sorted(cands)]
        self.sizes = [os.path.getsize(v) for v in self.volumes]
        self.starts = [sum(self.sizes[:i]) for i in range(len(self.sizes))]
        self.total = sum(self.sizes)
        self.files = {}  # normalized path -> (offset, size)
        self.version = None
        self.xor_key = None
        self._parse_index()

    def _read_global(self, off, size):
        out = bytearray()
        while size > 0:
            vi = 0
            while vi + 1 < len(self.starts) and off >= self.starts[vi + 1]:
                vi += 1
            local = off - self.starts[vi]
            with open(self.volumes[vi], "rb") as f:
                f.seek(local)
                chunk = f.read(min(size, self.sizes[vi] - local))
            if not chunk:
                break
            out.extend(chunk)
            off += len(chunk)
            size -= len(chunk)
        return bytes(out)

    def _parse_index(self):
        head = self._read_global(0, 11)
        if head[:2] != b"pf":
            raise ValueError(f"not a PFS archive: {self.volumes[0]}")
        self.version = chr(head[2])
        index_size = struct.unpack("<I", head[3:7])[0]
        count = struct.unpack("<I", head[7:11])[0]
        if self.version == "8":
            key_material = self._read_global(7, index_size)
            self.xor_key = hashlib.sha1(key_material).digest()
        pos = 11
        raw = self._read_global(pos, index_size)
        p = 0
        for _ in range(count):
            (ln,) = struct.unpack_from("<I", raw, p)
            p += 4
            name = raw[p:p + ln].decode("utf-8", "replace")
            p += ln
            _reserved, off, size = struct.unpack_from("<III", raw, p)
            p += 12
            self.files[name.replace("\\", "/").lower()] = (off, size)

    def read(self, path):
        key = path.replace("\\", "/").lower()
        if key not in self.files:
            return None
        off, size = self.files[key]
        data = bytearray(self._read_global(off, size))
        if self.xor_key:
            # pf8 XOR 相位 = 文件内偏移（引擎 io_read：ctx->pos 是本次打开
            # 文件的窗口位置，lo=档案全局起点不参与相位）。曾误用全局偏移，
            # 无 sidecar 的包（jianyu/tmny31）全部解错。
            for i in range(len(data)):
                data[i] ^= self.xor_key[i % len(self.xor_key)]
        return bytes(data)

    def names(self):
        return self.files.keys()


# ---------------------------------------------------------------- ASB layer

def decode_asb(data):
    """Binary .asb -> list of ('label', name) / ('cmd', name, [(k, v)...])."""
    if data[:4] != b"ASB\x00":
        raise ValueError("bad ASB magic")
    total = struct.unpack_from("<I", data, 5)[0]
    pos = 9

    def cstr():
        nonlocal pos
        (ln,) = struct.unpack_from("<I", data, pos)
        pos += 4
        s = data[pos:pos + ln].decode("utf-8", "replace")
        pos += ln + 1  # NUL terminator
        return s

    out = []
    for _ in range(total):
        (t,) = struct.unpack_from("<I", data, pos)
        pos += 4
        name = cstr()
        if t == 1:
            out.append(("label", name, []))
        else:
            pos += 4  # serial
            (pc,) = struct.unpack_from("<I", data, pos)
            pos += 4
            out.append(("cmd", name, [(cstr(), cstr()) for _ in range(pc)]))
    return out


# ---------------------------------------------------------------- census

LUA_VAR_RE = re.compile(r'e:var\(\s*"([A-Za-z0-9_.]+)"')
LUA_TAGDEF_RE = re.compile(r'function\s+tags\.([A-Za-z0-9_]+)|tags\["([^"]+)"\]\s*=')
LUA_EMISSION_RE = re.compile(r'e:(?:tag|enqueueTag)\s*\{\s*\{?"?([A-Za-z0-9_./]+)"?')

ENGINE_KNOWN_VARS = {
    "s.engineversion", "s.windowsversion", "s.screen_width", "s.screen_height",
    "s.savepath", "s.current_message_layer", "s.copyright",
}


def scan_game(root_dir):
    pfs_path = os.path.join(root_dir, "root.pfs")
    archive = PfsArchive(pfs_path) if os.path.exists(pfs_path) else None

    profile = {
        "root": root_dir,
        "package": {
            "pfs": bool(archive),
            "version": archive.version if archive else None,
            "volumes": len(archive.volumes) if archive else 0,
            "files": len(archive.files) if archive else 0,
        },
        "asset_types": collections.Counter(),
        "asb_commands": collections.Counter(),
        "asb_param_shape": {},
        "iet_tags": collections.Counter(),
        "lua": {
            "files": 0,
            "tags_handlers": collections.Counter(),
            "vars_read": collections.Counter(),
            "tags_emitted": collections.Counter(),
        },
        "system_ini": {},
        "unknown_vars": [],
    }

    # --- system.ini (sidecar preferred, then PFS) ---
    ini_bytes = None
    sidecar_ini = os.path.join(root_dir, "root", "system.ini")
    if os.path.exists(sidecar_ini):
        ini_bytes = open(sidecar_ini, "rb").read()
    elif archive and "system.ini" in archive.names():
        ini_bytes = archive.read("system.ini")
    if ini_bytes:
        try:
            text = ini_bytes.decode("utf-8")
        except UnicodeDecodeError:
            text = ini_bytes.decode("shift_jis", "replace")
        section = None
        for line in text.splitlines():
            s = line.strip()
            if s.startswith("[") and s.endswith("]"):
                section = s[1:-1].upper()
                continue
            m = re.match(r"(WIDTH|HEIGHT|FPS|CHARSET|BOOT)\s*=\s*(.*)", s, re.I)
            if m and section:
                profile["system_ini"][f"{section}.{m.group(1).upper()}"] = m.group(2).strip()[:60]

    # --- file census ---
    def note_ext(name):
        ext = os.path.splitext(name)[1].lower().lstrip(".")
        profile["asset_types"][ext or "(none)"] += 1

    def census_text(name, data):
        low = name.lower()
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            try:
                text = data.decode("shift_jis")
            except UnicodeDecodeError:
                return
        if low.endswith(".asb") or data[:4] == b"ASB\x00":
            try:
                for kind, cname, params in decode_asb(data):
                    if kind != "cmd":
                        continue
                    profile["asb_commands"][cname] += 1
                    shape = profile["asb_param_shape"].setdefault(cname, collections.Counter())
                    shape["|".join(sorted(k for k, _ in params))] += 1
            except (ValueError, struct.error):
                profile["asb_commands"]["(decode_error)"] += 1
        elif low.endswith(".iet") or low.endswith(".ast"):
            for m in re.finditer(r"\[([a-zA-Z_][a-zA-Z0-9_]*)", text):
                profile["iet_tags"][m.group(1)] += 1
        elif low.endswith(".lua"):
            profile["lua"]["files"] += 1
            for m in LUA_TAGDEF_RE.finditer(text):
                profile["lua"]["tags_handlers"][m.group(1) or m.group(2)] += 1
            for m in LUA_VAR_RE.finditer(text):
                profile["lua"]["vars_read"][m.group(1)] += 1
            for m in LUA_EMISSION_RE.finditer(text):
                profile["lua"]["tags_emitted"][m.group(1)] += 1

    # sidecar dir first (patch layer), then PFS-only files
    sidecar_files = {}
    side_root = os.path.join(root_dir, "root")
    if os.path.isdir(side_root):
        for dirpath, _dirs, files in os.walk(side_root):
            for fn in files:
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, side_root).replace("\\", "/")
                sidecar_files[rel.lower()] = full
                note_ext(fn)
    if archive:
        for n in archive.names():
            if n in sidecar_files:
                continue
            note_ext(n)

    seen = set()
    for rel, full in sorted(sidecar_files.items()):
        key = ("s", rel)
        if key in seen:
            continue
        seen.add(key)
        ext = os.path.splitext(rel)[1].lower()
        if ext in (".asb", ".iet", ".ast", ".lua") or rel == "system.ini":
            try:
                with open(full, "rb") as f:
                    census_text(rel, f.read())
            except OSError:
                pass
    if archive:
        for n, (off, size) in sorted(archive.files.items()):
            if n in sidecar_files:
                continue
            ext = os.path.splitext(n)[1].lower()
            if ext not in (".asb", ".iet", ".ast", ".lua"):
                continue
            try:
                census_text(n, archive.read(n))
            except (ValueError, struct.error, OSError):
                profile["asb_commands"]["(read_error)"] += 1

    # unknown system vars (gap candidates for the engine, plan §38)
    for var in profile["lua"]["vars_read"]:
        if var.startswith("s.") and var not in ENGINE_KNOWN_VARS:
            profile["unknown_vars"].append(var)

    # counters -> plain dicts for JSON
    def plain(obj):
        return {k: v for k, v in sorted(obj.items())} if isinstance(obj, dict) else obj

    profile["asset_types"] = dict(sorted(profile["asset_types"].items()))
    profile["asb_commands"] = dict(sorted(profile["asb_commands"].items()))
    profile["asb_param_shape"] = {k: dict(v.most_common(4)) for k, v in sorted(profile["asb_param_shape"].items())}
    profile["iet_tags"] = dict(sorted(profile["iet_tags"].items()))
    for k in ("tags_handlers", "vars_read", "tags_emitted"):
        profile["lua"][k] = dict(sorted(profile["lua"][k].items()))
    return profile


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    out = None
    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]
    profile = scan_game(root)
    text = json.dumps(profile, ensure_ascii=False, indent=1)
    if out:
        os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
        with open(out, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"profile -> {out}")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
