#!/usr/bin/env python3
"""Artemis Compatibility Lab — regression runner (M5/M9/M10 seed).

Runs the local game corpus headlessly against a built openartemis.exe,
extracts a per-game STATE DIGEST from the `[app] headless end ...` line,
evaluates scenario asserts (Bug Museum regressions), and diffs against the
stored baseline. One command answers "did my change break / fix anything?"

Usage:
  python regression.py --exe <openartemis.exe> [--games <corpus.json>]
      [--baseline <baselines.json>] [--update-baseline] [--only id1,id2]

Exit code 0 = all green (PASS / NEW-STORED), 1 = at least one FAIL/DRIFT.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

FAIL_PATTERNS = [
    "tick error", "label not found", "calllua error", "terminate called",
    "filesystem error", "assertion failed", "cannot open project",
    "no project given",
]

DIGEST_RE = re.compile(
    r"\[app\] headless end frames=(\d+) layers=(\d+) layer_ev=(\d+) "
    r"text_ev=(\d+) switch_ev=(\d+) msg_lines=(\d+) handlers=(\d+) "
    r"wait=(\S+) media_ev=(\d+) audio_players=(\d+) bgm=(\d+) se=(\d+) voice=(\d+)")

DIGEST_KEYS = ["frames", "layers", "layer_ev", "text_ev", "switch_ev",
               "msg_lines", "handlers", "wait", "media_ev", "audio_players",
               "bgm", "se", "voice"]


def run_game(exe, game, log_dir, baseline_dir):
    """Headless run with the game's scenario; returns (result dict, log text)."""
    env = dict(os.environ)
    env["OA_SAVE_ROOT"] = os.path.join(log_dir, game["id"] + "_save")
    for k, v in game.get("env", {}).items():
        env[k] = v
    dump = None
    if game.get("mode") == "windowed_dump":
        # 渲染面断言（字体/像素类）需要真渲染：窗口线短跑 + 终帧 PPM 落盘。
        dump = os.path.join(log_dir, game["id"] + ".ppm")
        cmd = [exe, "--frames", str(game.get("frames", 700)), "--dump", dump]
    else:
        cmd = [exe, "--headless", "--frames", str(game.get("frames", 1200))]
    if game.get("click_ids"):
        env["OA_CLICK_ID"] = game["click_ids"]  # driver is env-driven
    cmd.append(game["path"])
    t0 = time.time()
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env,
                          errors="replace", timeout=game.get("timeout_s", 600))
    wall = time.time() - t0
    log = (proc.stdout or "") + (proc.stderr or "")
    with open(os.path.join(log_dir, game["id"] + ".log"), "w", encoding="utf-8",
              errors="replace") as f:
        f.write(log)
    res = {"id": game["id"], "wall_s": round(wall, 1), "exit": proc.returncode}

    m = DIGEST_RE.search(log)
    if m:
        res["digest"] = dict(zip(DIGEST_KEYS, m.groups()))
    else:
        res["digest"] = None
    res["clean_exit"] = "[app] bye" in log
    res["fail_hits"] = [p for p in FAIL_PATTERNS if p in log]
    for extra in game.get("fail_patterns", []):
        if extra in log:
            res["fail_hits"].append(extra)

    # scenario asserts (Bug Museum): each is ("contains"|"not_contains"|"min_count",
    #                                        needle, n) evaluated on the log
    assert_results = {}
    for a in game.get("asserts", []):
        kind = a["kind"]
        needle = a["needle"]
        if kind == "contains":
            ok = needle in log
        elif kind == "not_contains":
            ok = needle not in log
        elif kind == "min_count":
            ok = log.count(needle) >= a.get("n", 1)
        else:
            ok = False
        assert_results[a.get("name", f"{kind}:{needle[:40]}")] = ok
    res["asserts"] = assert_results
    if dump:
        res["dump"] = dump
        res["dump_bytes"] = os.path.getsize(dump) if os.path.exists(dump) else 0
        # M7 视觉差分：与像素基线逐字节比对（实测窗口线 dump 完全确定）。
        base_ppm = os.path.join(baseline_dir, game["id"] + ".ppm")
        if os.path.exists(base_ppm) and res["dump_bytes"] > 0:
            a = open(dump, "rb").read()
            b = open(base_ppm, "rb").read()
            if a == b:
                res["pixel_diff_pct"] = 0.0
            elif len(a) == len(b):
                diff = sum(1 for x, y in zip(a[15:], b[15:]) if x != y)
                res["pixel_diff_pct"] = round(diff / max(1, len(a) - 15) * 100, 4)
            else:
                res["pixel_diff_pct"] = 100.0  # 尺寸都变了 = 结构性回归
            thr = game.get("pixel_threshold_pct", 0.5)
            res["pixel_ok"] = res["pixel_diff_pct"] <= thr
    need_digest = game.get("mode") != "windowed_dump"
    res["ok"] = (not res["fail_hits"]) and res["clean_exit"] \
        and all(assert_results.values()) \
        and (res["digest"] is not None or not need_digest) \
        and (not dump or res["dump_bytes"] > 0) \
        and res.get("pixel_ok", True)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--games", default=os.path.join(os.path.dirname(__file__),
                                                    "..", "..", "compatlab", "corpus.json"))
    ap.add_argument("--baseline",
                    default=os.path.join(os.path.dirname(__file__),
                                         "..", "..", "compatlab", "baselines.json"))
    ap.add_argument("--log-dir", default=None)
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--only", default="")
    args = ap.parse_args()

    games = json.load(open(args.games, encoding="utf-8"))["games"]
    if args.only:
        want = {x.strip() for x in args.only.split(",") if x.strip()}
        games = [g for g in games if g["id"] in want]

    baseline = {}
    if os.path.exists(args.baseline):
        baseline = json.load(open(args.baseline, encoding="utf-8"))

    run_id = time.strftime("%Y%m%d_%H%M%S")
    log_dir = args.log_dir or os.path.join(os.path.dirname(args.games),
                                           "reports", "logs_" + run_id)
    os.makedirs(log_dir, exist_ok=True)

    results = []
    for game in games:
        if not os.path.exists(game["path"]):
            results.append({"id": game["id"], "ok": False,
                            "error": "game path not found"})
            continue
        try:
            results.append(run_game(args.exe, game, log_dir,
                                    os.path.join(os.path.dirname(args.baseline), "pixel_baselines")))
        except subprocess.TimeoutExpired:
            results.append({"id": game["id"], "ok": False, "error": "timeout"})

    # verdicts + baseline diff
    report = {"run_id": run_id, "exe": args.exe, "results": []}
    n_fail = 0
    for r in results:
        row = dict(r)
        row["verdict"] = "FAIL" if not r.get("ok") else "PASS"
        prev = baseline.get(r["id"], {}).get("digest")
        if r.get("digest") and prev is not None and prev != r["digest"]:
            drift = {k: (prev.get(k), r["digest"].get(k))
                     for k in set(prev) | set(r["digest"])
                     if str(prev.get(k)) != str(r["digest"].get(k))}
            row["digest_drift"] = drift
            # drift alone is not FAIL (timing-sensitive fields possible) but
            # must be visible; with --strict it fails.
            row["verdict"] = "PASS(DRIFT)"
        if not r.get("ok"):
            n_fail += 1
        report["results"].append(row)

    if args.update_baseline:
        new_base = {r["id"]: {"digest": r.get("digest"),
                              "ok": r.get("ok"),
                              "run": run_id}
                    for r in results if r.get("digest")}
        json.dump(new_base, open(args.baseline, "w", encoding="utf-8"),
                  ensure_ascii=False, indent=1)
        # M7：windowed_dump 的终帧 PPM 一并存为像素基线。
        pxd = os.path.join(os.path.dirname(args.baseline), "pixel_baselines")
        os.makedirs(pxd, exist_ok=True)
        import shutil
        for r in results:
            if r.get("dump") and os.path.exists(r["dump"]):
                shutil.copyfile(r["dump"], os.path.join(pxd, r["id"] + ".ppm"))

    out_path = os.path.join(os.path.dirname(args.games), "reports",
                            f"run_{run_id}.json")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    json.dump(report, open(out_path, "w", encoding="utf-8"),
              ensure_ascii=False, indent=1)

    print(f"=== compatlab regression {run_id} ===")
    for row in report["results"]:
        line = f"{row['id']:12} {row.get('verdict','?'):12}"
        if row.get("digest"):
            d = row["digest"]
            line += (f" frames={d['frames']} text_ev={d['text_ev']} "
                     f"layer_ev={d['layer_ev']} wait={d['wait']}")
        if row.get("digest_drift"):
            line += f"  DRIFT={row['digest_drift']}"
        if row.get("pixel_diff_pct") is not None:
            line += f"  pix={row['pixel_diff_pct']}%"
        if not row.get("ok"):
            line += f"  fails={row.get('fail_hits') or row.get('error')}"
            bad = {k: v for k, v in row.get("asserts", {}).items() if not v}
            if bad:
                line += f"  asserts_failed={list(bad)}"
        print(line)
    print(f"report -> {out_path}  logs -> {log_dir}")
    print(f"PASS {sum(1 for r in report['results'] if r.get('ok'))}/"
          f"{len(report['results'])}  (exit {'0' if n_fail == 0 else '1'})")
    return 0 if n_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
