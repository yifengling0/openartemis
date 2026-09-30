# Compatibility Lab Baseline（TASK-001 / M0）

日期：2026-09-24　范围：`vintage-pomelo/openartemis`（Artemis OHOS 原生运行时）
审计方式：源码走读 + 本会话实测。**本文件只记录现状，不改代码。**

## Build
- 桌面线：`build-windows.ps1`（MSYS2 MinGW + CMake，Release，产物 `build_sdl2/src/app/openartemis.exe`）
- OHOS 线：`VintagePomelo/build-openartemis-harmony-and-deploy.ps1`（Strawberry CMake + ohos.toolchain，Release+LTO+strip，`libopenartemis.so` ~1.9MB）
- 单测线：`tests/`（`OA_ENABLE_TESTS=ON`）**在本机不可构建**——tests/CMakeLists 需要 vcpkg 的 Vorbis CONFIG 包；引擎本体走 vendored vorbis。⇒ 回归以 headless 语料为准。

## Runtime Entry
- 桌面/`main.cpp`：`SDL_AppInit` → `SDL_AppIterate`（tick+draw+present）；`--headless` 走独立虚拟 16ms tick 循环（无窗口/无渲染）。
- OHOS：`oa_harmony.cpp runner_main`（dlopen by engine_loader），SDL2+GLES3，契约见 HARMONY_NATIVE_HANDOVER.md。

## Script
- 三格式：`.iet/.ast` 文本标签、`.asb` 二进制命令流（decode_asb → 标签流）、Lua 块（load 时执行）。
- 两谱系（HANDOVER §7b）：FPM 系（标签=引擎原生）；原 ASB 命令系（游戏 Lua `tags.<命令>` 经过滤器接管）。
- 标签分发：`execute_tag`（原生分支：calllua/lua/call/return/stop/wait/var/if/loop/print/rt/rp/chgmsg/ruby/lyprop/lydel/trans/flip…）+ Lua tag filter（`tags.X` 真值=脚本接管，假值=引擎默认——2026-09-24 修正）。

## Lua
- Lua 5.1.5 单 VM（`LuaBridge`），surface：tag/sys/input/surface/emote/filter/pluto。
- GC：引擎默认 pause=300（`OA_LUA_GCPAUSE/GCMUL/GCSTEP_KB` 可调）。
- 已知存根：`getScriptBlock`（返回 nil）；`getScriptStack` 已实现（2026-09-24）。

## Render
- 双后端：`backend_sdl`（SDL_Renderer，桌面参考线）/ `backend_gles`（GLES3，OHOS 主线）。
- P1 优化已落地：字形图集、合批、状态去重、组烘焙 GPU 预乘直通、纹理 LRU、异步解码开关（PERFORMANCE_OPTIMIZATION_PLAN.md）。
- 分相计时：`OA_PROFILE` 输出 tick/script/content/other/luaKB + rscene/rtext/present。

## Audio / Video
- 音频：SDL 回调线程 + DecodePool 预取；SE/Voice/BGM 三总线；`OA_AUDIO_MIXDIAG` 出 RMS 电平。媒体加载带扩展名探测（2026-09-24）。
- 视频：FFmpeg（Ogg/Theora 优先走 FFmpeg——jianyu heap smash 修复线）。

## Save
- `<游戏目录>/savedata`（OHOS）/`OA_SAVE_ROOT`（测试）；Lua 存档写入虚拟化（ART3M1S_REFERENCE_NOTES.md）。

## Input
- 窗口线：SDL 鼠标/键盘；`OA_CLICK_ID`/`OA_AUTO_START` 合成驱动（headless 亦生效，2026-09-24）。
- OHOS：唯一指针入口 = ArkTS 触摸板（sdlInputMouse）；SDL 事件层无 FINGER（HANDOVER §7.3）。

## Package
- PFS pf2/pf6/pf8（XOR key=SHA1(index)），分卷拼接（root.pfs.000…），sidecar 目录层优先。
- system.ini：`--platform android` → [ANDROID]，几何优先取 [WINDOWS]（2026-09-24）。

## E-mote
- `src/core/emote/`（emote_player/emote_render/psb_reader/bc7decomp），CPU 光栅 ~8fps 节流 + GPU 部件合成路径。
- **无任何 E-mote 专项测试**。语料中的枫笛含 E-mote 素材（待场景覆盖）。

## Tests（现状）
- `tools/real_game_smoke.ps1`：headless 冒烟（fail patterns + clean bye）。
- `tools/compatlab/regression.py`：**本回合新增**——语料批量 headless/windowed 运行 + 状态摘要 + Bug Museum 断言 + 基线差分（见 PROGRESS）。
- `tools/compatlab/scan_game.py`：静态特征扫描（ASB 命令/IET 标签/Lua 面/系统变量缺口）。

## Existing Observability（OA_* 面）
TAG_TRACE（标签+过滤器裁决+队列+calllua）、TEXT_IN_TRACE、SCENE_LAYERS、SCENE_HANDLERS、PROFILE、AUDIO_MIXDIAG/DEBUG、RENDER_DIAG、VIDEO_DEBUG、TOUCH_DIAG、SELTRACE、DEBUG_RESET/TRANSQ、CLICK_ID、AUTO_START。字符串日志为主；**JSONL 结构化 trace 未建**（M1 待办）。

## Missing Infrastructure（按里程碑）
- M1：JSONL TraceSink/ring buffer（当前是 printf 面）。
- M2：完整 TestBridge（socket/IPC）；确定性模式缺 seeded 全局 RNG 汇总面（Lua LCG 已定值）。
- M3：Spec Harvester（官方页抓取）——未建。
- M5：Synthetic Suite（自建 fixture 工程）——未建；现以真包语料代偿。
- M6：OHOS device runner（hdc 自动化）——未建。
- M7：视觉差分 oracle（当前仅 fengti_font 的 PPM 存在性）。
- M9：Bug Museum 已种子化 4 条（corpus.json asserts）；几何修复（[ANDROID]）无本地可复现包，仅记录。
- M10/11：Reducer/AI Fixer——未建（遵守"先基建后自迭代"）。

## Recommended M1 Patch
JSONL TraceSink：把 TAG_TRACE/TEXT_IN/SCENE_LAYERS 的打印面收敛为可开关的 JSONL 输出（域/seq/frame/op），先 host 线落地——后续差分 oracle 与聚类都依赖它。
