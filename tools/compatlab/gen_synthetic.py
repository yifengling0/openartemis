#!/usr/bin/env python3
"""Artemis Compatibility Lab — synthetic suite generator (M5 v1).

Generates self-contained DIRECTORY projects (no game assets — fully
committable, plan §21 L0) from a case table, plus a corpus JSON consumable
by regression.py. Each case boots the engine headlessly and exercises a
narrow slice of script semantics; asserts ride on TAG_TRACE / digest.

Usage:
  python gen_synthetic.py            # (re)generate compatlab/synthetic/ + corpus_synthetic.json
"""

import json
import os

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
SYN = os.path.join(ROOT, "compatlab", "synthetic")

SYSTEM_INI = """[WINDOWS]
WIDTH = 640
HEIGHT = 480
FPS = 60
CHARSET = UTF-8
BOOT = system/first.iet
SAVEPATH = savedata
"""

# 每个 case：(id, 描述, 脚本正文, 关键标签集合——进入 TAG_TRACE 断言)
CASES = [
    ("boot_min", "空脚本直达 [exit]：启动/退出闭环",
     """*top
[exit]
""", ["exit"]),

    ("var_arith", "表达式契约：data/estimate 需 $ 前缀才求值（裸名=字面量）；1+2==3 才走 else",
     """*top
[var name="t.a" data="1"]
[var name="t.a" data="$t.a + 2"]
[if estimate="$t.a == 3"]
[print data="arith ok"]
[endif]
[exit]
""", ["var", "if", "print"]),

    ("text_basic", "chgmsg/print/rt/rp 文本面流水线",
     """*top
[chgmsg id="adv"]
[print data="line one"]
[rt]
[print data="line two"]
[rp]
[print data="page two"]
[/chgmsg]
[exit]
""", ["chgmsg", "print", "rt", "rp"]),

    ("trans_clear", "trans type=0（整场清除）+ flip 边界",
     """*top
[print data="before"]
[flip]
[trans time=0 type=0]
[print data="after"]
[exit]
""", ["trans", "flip"]),

    ("wait_time", "定时等待边界（1ms / 1000ms，时钟推进后继续）",
     """*top
[wait time=1]
[print data="w1"]
[wait time=1000]
[print data="w2"]
[exit]
""", ["wait", "print"]),

    ("jump_call_return", "call/return 调用栈（call 目标=另一文件，真实游戏形态）",
     """*top
[call file="system/sub.iet" label="sub"]
[print data="back in top"]
[exit]
""", ["call", "return"]),

    ("call_self", "call 指向当前脚本自身——已知怪癖探针：return 会落到被调侧导致双 return/Completed，不达 [exit]（engine 怪癖候选，待对照参考实现）",
     """*top
[call file="system/first.iet" label="sub"]
[print data="back in top"]
[exit]

*sub
[print data="in sub"]
[return]
""", ["call"]),

    ("loop_bound", "[loop estimate=]…[/loop] 有界循环（计数到 5）",
     """*top
[var name="t.i" data="0"]
[loop estimate="$t.i < 5"]
[var name="t.i" data="$t.i + 1"]
[/loop]
[print data="loop done"]
[exit]
""", ["loop", "var", "print"]),

    ("lua_block", "[lua] 块（load 时执行）+ e:tag 写变量回传",
     """*top
[lua]
function syn_w(engine)
    engine:tag{"var", name="t.lua", data="fromlua"}
end
[/lua]
[calllua function="syn_w"]
[if estimate="$t.lua == 'fromlua'"]
[print data="lua ok"]
[endif]
[exit]
""", ["calllua"]),

    ("chgmsg_two", "双消息层切换（s.current_message_layer 维护面）",
     """*top
[chgmsg id="adv"]
[print data="adv layer"]
[/chgmsg]
[chgmsg id="name"]
[print data="name layer"]
[/chgmsg]
[exit]
""", ["chgmsg", "print"]),

    ("ruby_inline", "ruby/字体栈标签不炸文本管线",
     """*top
[chgmsg id="adv"]
[ruby text="かんじ"]
[print data="漢字"]
[/ruby]
[/chgmsg]
[exit]
""", ["ruby", "print"]),
]


def main():
    os.makedirs(SYN, exist_ok=True)
    games = []
    for cid, desc, script, key_tags in CASES:
        d = os.path.join(SYN, cid)
        os.makedirs(os.path.join(d, "system"), exist_ok=True)
        if cid == "jump_call_return":
            with open(os.path.join(d, "system", "sub.iet"), "w",
                      encoding="utf-8", newline="\n") as f:
                f.write('*sub\n[print data="in sub"]\n[return]\n')
        with open(os.path.join(d, "system.ini"), "w", encoding="utf-8",
                  newline="\n") as f:
            f.write(SYSTEM_INI)
        with open(os.path.join(d, "system", "first.iet"), "w", encoding="utf-8",
                  newline="\n") as f:
            f.write(script)
        env = {"OA_TAG_TRACE": ",".join(key_tags)}
        asserts = [
            {"name": "clean_exit", "kind": "contains", "needle": "[app] bye"},
        ]
        # 逐关键标签断言（trace 行形态：[tag] file:line [tagname]）。
        # 注意：[lua] 块是 LuaBlock 行不是 [tag] 行，Lua 侧 e:tag 也不走
        # step 循环 trace——用 calllua 的分发行作它的执行证据。
        for t in key_tags:
            asserts.append({"name": f"tag_{t}_executed", "kind": "min_count",
                            "needle": f"[{t}]", "n": 1})
        if "calllua" in key_tags:
            asserts.append({"name": "calllua_dispatched", "kind": "min_count",
                            "needle": "[calllua] ", "n": 1})
        games.append({"id": "syn_" + cid, "path": d.replace(os.sep, "/"),
                      "frames": 600, "timeout_s": 60, "env": env,
                      "asserts": asserts, "_comment": desc})
    out = os.path.join(ROOT, "compatlab", "corpus_synthetic.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump({"_comment": "M5 合成用例（目录工程，无游戏资产）", "games": games},
                  f, ensure_ascii=False, indent=1)
    print(f"{len(games)} synthetic cases -> {SYN}")
    print(f"corpus -> {out}")


if __name__ == "__main__":
    main()
