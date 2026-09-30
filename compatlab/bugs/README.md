# Bug Museum（plan §21 L1）

每个条目 = 一个历史修复的最小回归看护。断言在 `../corpus.json` 的
`asserts` 字段里，由 `tools/compatlab/regression.py` 每轮执行。
修复回退时对应断言 FAIL——不允许删断言来"修"失败。

---

## BUG-0001 getScriptStack 空表存根 → 菜单不可点

- **症状**：枫笛（原 ASB 系）能启动、标题正常，点击任何菜单无反应。
- **根因**：`e:getScriptStack()` 是返回空表的存根；`artemis.lua ReturnStack()`
  的 `ss[#ss].file` 对空表索引 nil → `setonpush_call` 处理器抛错 →
  所有点击的处理链断掉。日志特征：`[app] click layer …` 有、
  `handler setonpush_call error: … attempt to index field '?'` 紧随。
- **修复**：`LuaHost::get_script_stack`（build_lua_host 装填 call_stack_+当前脚本），
  `l_get_script_stack` 输出 `ss[1..n]`（顶=当前脚本，reservedCommands={} 兜底）。
- **affected**：runtime_lua.cpp/.h、runtime_iet.cpp
- **回归**：`fengti` 场景 assert `not_contains "setonpush_call error"`。

## BUG-0002 标签过滤器吞掉引擎原生 print + s.current_message_layer 缺失 → 对话框全空

- **症状**：进剧情后文本框/任务面板完全无字；日志无报错（最难查的一类）。
- **根因**（两处叠加）：
  1. 游戏 `tags.print` 以 `if getMessageMode() then … return 1 end return 0`
     实现；`getMessageMode()` 读 `e:var("s.current_message_layer")`——引擎没有
     这个变量 → 回 "0" → 框架判定非消息模式 → 返回 0（让开）。
  2. 过滤器裁决 `(!has_builtin && d != Missing)` 把"假值（PassThrough）"也当成
     被 Lua 消费 → 引擎自己的 `[print]` 原生分支被跳过 → 文字无处可去。
- **修复**：a) `is_builtin_tag` 登记原生标签（print/rt/rp/chgmsg/ruby/lyprop/lydel/trans/flip），
  恢复"真值=脚本接管，假值=引擎默认"语义；b) `apply_text_event` 的
  MessageLayerSwitch/Pop 维护 `s.current_message_layer`。
- **affected**：runtime_iet.cpp、runtime.cpp
- **回归**：`fengti` 场景 assert `min_count "layer=adv units=" >= 5`
  （OA_TEXT_IN_TRACE 下剧情文字必须入 adv 层；修复前=0）。
- **推广**：扫描器 `unknown_vars` 字段持续暴露同类缺口（s.status.* 等 15 项，
  见 game_profiles）。

## BUG-0003 字体全局提前返回 → 整部作品无字（渲染面）

- **症状**：默认字体解析失败（项目把字体放 font/ 之外，如 `image/font/`）时，
  `draw_text_layer` 的 `!default_face && font_faces.empty() → return 0` 在任何
  逐层 face 被尝试前短路，所有文字层画 0 字形，且无任何日志。
- **修复**：短路收窄为"本层 face 已知不可加载"；face 负缓存（face_missing_）；
  第一个成功加载的脚本字体接管默认面（`default face adopted from script font:` 行）；
  失败有 `[app] font face not found:` 行。
- **affected**：font.cpp/.h
- **回归**：`fengti_font`（windowed_dump 模式）assert `min_count "default face adopted…" ≥1`
  + 终帧 PPM 非空。渲染面行为 headless 不可见 ⇒ 必须窗口线。

## BUG-0004 媒体无扩展名探测 → BGM 静默不播

- **症状**：音乐没有。游戏给 `bgm/bgm_0401_a`（csv 表基名），包内是 `.ogg`；
  媒体 loader 精确路径读取失败 → 通道静默放弃。
- **修复**：runtime.cpp 媒体/视频 loader 加候选扩展名探测
  （.ogg/.mp3/.wav/.m4a/.opus/.flac；视频 .mp4/.webm/.ogv/...），精确命中优先，
  全失败打一行 `[app] media not found:`（去重）。
- **affected**：runtime.cpp
- **回归**：`fengti` 场景 assert `not_contains "media not found: bgm/"`。

## BUG-0005 [ANDROID] 段几何盖过 [WINDOWS]（真机分辨率不对）

- **症状**：带过手机移植版的包，舞台尺寸取了 `[ANDROID]` 的移动版几何，
  脚本排版假定 PC 分辨率 → 画面比例/位置错。
- **修复**：`Project::open` 解析段非 WINDOWS 且包内有 `[WINDOWS]` 时，
  WIDTH/HEIGHT 改从 [WINDOWS] 取；生效打印 `[project] stage geometry from [WINDOWS] …`。
- **affected**：project.cpp
- **回归**：**本地语料无此类包**（4 包均无冲突），暂无自动用例。需要人工
  收集一个问题包后补 assert（`contains "stage geometry from [WINDOWS]"`）。

## BUG-0007（候选）`[call file=]` 指向当前执行中的脚本自身 → return 落点错

- **症状**（syn_call_self 探针用例，2026-09-25）：call 自身 label 后，
  `[return]` 执行两次、控制流落到被调侧的 return 行而非调用点之后，
  `[exit]` 永不执行（脚本以 Completed 结束而非 exit_requested）。
- **对照**：call 目标为**另一个文件**（syn_jump_call_return，真实游戏的
  常规形态）时 return 正确落回调用点之后。
- **根因**：未定位（call 路径的 `load_external_script` 对"当前正在执行的
  脚本"重载的语义）。是否与参考引擎一致未知——需先确认真实游戏是否存在
  自 call 形态，再决定按 reference 修或按未定义行为记录。
- **回归**：`syn_call_self` 用例钉住当前行为（clean_exit + call 执行），
  根因修复后其 text_ev 应从 3 → 2、return 只执行一次。

---

## BUG-0008（已澄清）E-mote 语料前置条件

- tmny31 是 E-mote 参考游戏（`list_windows.tbl: fg_ext = .psb`，立绘即
  E-mote PSB 模型，脚本面 `emote=1` 标志 + `e:createEmoteLayer` 全链在）。
- **但本地包缺整个 `image/fg/` 模型目录**（PFS 内无任何 .psb/无扩展名文件，
  已逐卷核对）——汉化包剥离了模型数据。E-mote 场景测试的前置条件：
  拿到带模型的完整 tmny31 包，或用 M2 官方 sample（许可允许的验证用途）。
  在此之前 E-mote 分域断言无法落地（记录于 PROGRESS Next）。

## BUG-0006（背景）headless 驱动基础设施

- `OA_CLICK_ID` 的 `@x,y` 点在 headless 可用（id 点需要渲染器）；
  列表分隔符 `;`（坐标里的 `,` 是坐标语法——曾因逗号冲突静默失效）；
  headless 循环消费 left_click_edge 后清零。
- `OA_CLICK_FAST=1`：快速节奏（hover 90→10、cooldown 60→10）——深驱
  数百点击点的开场文件必需。
- 用途：无窗口的剧情推进回归（本目录所有"进剧情"类断言的载体）。
