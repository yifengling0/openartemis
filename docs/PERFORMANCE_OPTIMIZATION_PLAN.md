# OpenArtemis HarmonyOS 性能优化方案

> 日期：2026-09-15
> 范围：`libopenartemis.so`（HarmonyOS 原生路径，GamePlay XComponent + SDL2 + GLES3）
> 背景：功能已对齐 KR2/RPGRunner（铺满、虚拟鼠标/按键、触摸、音频不卡顿），但整体帧率/流畅度仍偏低。本文从渲染架构、文件加载、Lua/运行时、编译开关四个维度定位热点，给出分阶段的统一优化方案。

---

## 状态更新（2026-09-19，兼容性批次之后回填）

同批次的兼容性改造见 **`docs/ART3M1S_REFERENCE_NOTES.md`**（对照 art3m1s/art3m1s-core，
已修：Lua 存档写入虚拟化、排队 call 屏障、UTF-8 路径、宿主服务桩、每游戏清单）。
本文各阶段的实际状态：

| 阶段 | 项 | 状态 |
|---|---|---|
| §4.2 | Release + LTO + strip（`.so` 2.15 MB） | **已完成**（构建脚本已用 Release + OHOS LTO + strip） |
| §1.2-1 | 字形图集（多页，1024²） | **已完成**（`font.cpp` atlas_pages_） |
| §1.2-2 | GLES draw call 合批 | **已完成**（`backend_gles.cpp` batch_append/flush_batch） |
| §1.2-4 | `glGetError` drain 门控（`OA_RENDER_DIAG`） | **已完成** |
| §1.2-5 | 静态帧跳过 | **已完成**（窗口线 + OHOS 的 re-blit 分支显式保留） |
| §3.1-1 | 逻辑帧率封顶 60Hz（高刷面板） | **已完成**（`pace_windowed_frame` 在 OHOS 生效） |
| §1.3 | 视频/E-mote 流式纹理 in-place 更新 | **部分**（GLES `update_texture` 已在；CPU 重上传路径仍存在） |
| §1.2-3 | 绘制状态去重 | 未做 |
| §2.1-2 | 图片**负缓存**（缺失名不再每帧重探测） | **已完成**（`decoded_miss_`） |
| §2.1 | 图片异步解码（默认关，`OA_ASYNC_DECODE=1` 开） | **已实现**：专用 worker 解码，命中/失败回填 `decoded`/`decoded_miss_`，完成即强制一帧重绘；见下方实测 |
| §2.1-3 | 纹理 LRU 淘汰（GPU 资产纹理，预算 256MB，`OA_TEX_BUDGET_MB` 可调） | **已完成**：`RenderEngine::evict_asset_textures`（未用满则零开销；本机两包资产纹理仅 ~30MB，未触发淘汰） |
| §3.1-2 | Lua GC 调优 | 未做 |
| — | Profiler 计数（draw calls / batches / binds / 纹理新建 / 上传字节 / 图片解码次数与耗时 / 读字节）+ `OA_PROFILE` 5 s 差分 | **已完成**（`RenderStats`/`AssetStats` + main.cpp；`tools/real_game_smoke.ps1 -Profile` 落盘基线） |
| — | 分段计时（逻辑/解释器/文本/合成各自的 ns 桶，x86/OHOS 上按需加） | 未做（当前计数面覆盖渲染/IO；脚本侧用现有 `OA_*` 诊断） |
| — | 脏区渲染（damage rect + scissor + 局部上传） | 未做（P1，需先做驱动 back-buffer 语义实测） |
| §4 | `-fvisibility=hidden`（OHOS 动态符号面收敛） | **已完成**：`.so` 2.20→1.92MB，内部 C++ dynsym 归零，5 个 dlsym 入口（runner_main / requestShutdown / cleanupSDL / getLastRubyError / get_last_ruby_error）保留 |
| §4 | `-mcpu`/`-march` 目标 SoC 调优 | 未做（保持通用 arm64 基线：产物要在多机型分发，收益/风险不划算） |
| §1.2-3 | 绘制状态去重 | **已完成**（GLES 侧 program/blend/scissor/viewport/uniform 均已缓存去重） |
| — | **字形度量缓存**（(face,ppem,cp) → GlyphMeasure；布局每帧重排整页、绘制每字形都调 FreeType） | **已完成**（`FontSystem::GlyphMetricsKey` + `glyphs h/f m/f` 计数） |
| §1.2 | **intermediate_render 组烘焙的全屏回读**（`glReadPixels` + 整幅 CPU 合成 + 重新上传，gzsq 实测 152–385 MB/s 上传/120 次纹理新建每秒） | **已完成**：参数为恒等的组走 GPU 预乘混合直通（`BlendMode::Premul`），不再回读；见下方 A/B 数据 |

基线采集：

```powershell
# 每个真包跑 1200 帧，prof 行与完整日志写到 <save>/<game>/profile.log
.\tools\real_game_smoke.ps1 -Frames 1200 -Profile -KeepSaveRoot -Game `
  'D:\Games\Pomelo\Ar_枫笛_脏翅膀_od\root.pfs','D:\Games\Pomelo\jianyu\root.pfs'

# 单包窗口线（有像素数据，OHOS 真机同构）：--fps 30 时跑满 5 s 即有 prof 行
OA_PROFILE=1 ./build_sdl2/src/app/openartemis --fps 30 --frames 200 <game>/root.pfs
```

2026-09-19 实测（桌面 GLES 线，`--fps 1000` 去节流）：

| 包 | fps | draws/f | uploads/f | upMB/s | tick/f（script/content/other） |
|---|---|---|---|---|---|
| tmny31 | 357 | 0.4 | 0.0 | 8.1 | 0.35ms（0.30/0.02/0.04） |
| gzsq（111→127 层） | 46–60 | 9–18 | 1.0–2.0 | 152–385 | 0.12–0.43ms（0.00/0.03–0.30/0.08） |

结论：逻辑 tick 不是瓶颈（0.1–0.4 ms/帧）；gzsq 这类含 `intermediate_render` 组与
E-mote/视频画布的包，成本集中在**每帧整幅纹理上传与组烘焙回读**上。

### 组烘焙 GPU 直通（2026-09-19 落地）A/B 实测

同一二进制、同一命令（`--fps 60 --frames 600`，窗口 GLES），仅切
`OA_GROUP_PREMUL`（0 = 旧的回读+CPU 合成，1 = 预乘直通）：

| 指标 | 旧路径 (0) | 新路径 (1) | 变化 |
|---|---|---|---|
| uploads/frame | 0.8 | 0.0 | −100% |
| 上传带宽 upMB/s | 151.4 | 2.6 | **−98.3%** |
| 纹理新建（5.5s 内） | 473 | 240 | −49% |
| CPU 时间（600 帧） | 7.94 s | **3.08 s** | **−61%** |
| 组合成 premul/f · readback/f | 0.00 · 0.76 | 0.76 · 0.00 | 全部走直通 |
| journal（luma/layers/drawn/layer_ev/text_ev/switch_ev） | 216.5/111/7/266/0/30 | 同左 | 完全一致 |
| 末帧像素（PPM 逐通道） | — | — | **0 差异** |

另两个包（tmny31 / 枫笛，标题带动画）的 A/B 像素差（5.1% / 0.95%）均**小于**
同设置的跑间噪声（7.9% / 2.2%），即差异来自动画相位而非合成路径。

实现要点：离屏 target 的像素本来就是预乘 RGBA（离屏 pass 用 GLES 的
premultiplied 混合写入），因此恒等参数组（无 color multiply/灰度/负片/mask、
layermode=over）可以直接用 `BlendMode::Premul` + rgb/alpha 双 mod 叠回主 target；
缓存记 flavour（预乘 target vs 直接 RGBA bake），flavour 变化时重烘焙，避免两种
纹理被错误混合。`OA_GROUP_PREMUL=0` 保留旧路径用于二分定位与旧像素基线复现。

### 异步图片解码实测（tmny31，`--fps 60 --frames 600`）

| 指标 | 同步（默认） | `OA_ASYNC_DECODE=1` |
|---|---|---|
| tick 线程 readMB/s | 0.1 | **0.0**（读盘与解码都移到 worker） |
| async started / done / fail | 0/0/0 | 3/3/0 |
| journal（luma/layers/drawn/layer_ev/text_ev/switch_ev/msg_lines） | 2.5/17/2/336/9/31/1 | 完全相同 |
| 末帧像素 A/B | — | 4.7% 通道差异（同设置跑间噪声为 7.9%），即落在噪声内 |

为什么默认关：异步路径会让"首次需要某张图的那 1..N 帧"先不出图（等 worker 完成
后由 `consume_async_completion()` 强制重绘）。这个换法在 OHOS 上收益最大
（1920×1080 PNG 解码 20–60ms），但会改变出图时序，必须在**真实像素基线**
（FPM/NekoMiko 的 journey 套件）上验证后才适合改为默认。开关已就位，真机可用
`OA_ASYNC_DECODE=1` 直接 A/B。

---

## 0. 结论速览（TL;DR）

| 优先级 | 项 | 现状 | 预期收益 |
|---|---|---|---|
| P0 | 编译开关：OHOS 仍是 `-O2 -g`（RelWithDebInfo），无 LTO、无 `-mcpu`、带完整调试信息 | `libopenartemis.so` ~24 MB | 全场景 10–30% CPU 收益，零风险 |
| P0 | 文本渲染：每个字形一张独立小纹理 + 每字形一次 draw call（`glBufferData`+`glDrawArrays`） | 一页正文 = 数百次 draw call/帧 | 文本场景（VN 主场景）最大单项收益 |
| P0 | 120Hz 面板下 tick/Lua `onEnterFrame` 跑 120 次/秒 | 脚本/tween/音频 pacing 双倍于 60Hz 设计 | 高刷设备 CPU 占用近乎减半 |
| P1 | 图片解码全部同步发生在 tick 线程（`resolve_image`） | 场景切换/立绘出现 = 几十 ms 卡顿 | 消除转场掉帧 |
| P1 | 视频/E-mote 流式纹理每帧销毁重建（`glTexImage2D`+`glTexSubImage2D`+memcpy） | OP/ED 播放、E-mote 立绘每帧一次全量纹理分配 | 视频场景显著收益 |
| P1 | 静态帧在 OHOS 上仍每 vsync 重 blit + SwapWindow | 静态画面也消耗 GPU/带宽 | 省电、降温、给音频让出 CPU |
| P2 | 图片缓存无淘汰（`decoded` + `textures` 双份常驻） | 长流程内存单调增长 | 降低低端机 OOM/换页风险 |
| P2 | Lua 5.1.5 解释执行，无 GC 调优 | 脚本重场景有 GC 毛刺 | 消除偶发卡顿 |

---

## 1. 渲染架构分析

### 1.1 当前管线

```
tick (vsync) → draw_scene → [stage FBO 离屏合成] → render_end: FBO→window blit → SDL_GL_SwapWindow
```

- 场景先渲染到 `stage_rt`（FBO），`render_end()` 再把 stage 纹理画到窗口（`renderer.cpp:1446-1460`）。多一次全屏拷贝，但这是转场/快照/[takess] 的正确性依赖，保留。
- GLES 后端为逐 quad 绘制：`draw_texture`/`fill_rect`/`draw_geometry` 每次都 `glBufferData(GL_DYNAMIC_DRAW)` 上传 6 顶点然后 `glDrawArrays`（`backend_gles.cpp:1052-1056`）。无合批。
- `apply_draw_state` 每次绘制重算 viewport、重设 blend/scissor/uniform（`backend_gles.cpp:928-1007`），即使前后两次绘制状态完全相同。
- `present()` 每帧 drain 一次 `glGetError`（`backend_gles.cpp:1210-1217`）——在部分 ARM 驱动上会触发隐式同步。

### 1.2 文本渲染是最大热点

`font.cpp` 的字形缓存策略是**每个字形一张独立 GL 纹理**（`glyph_slot`/`edge_slot` → `create_texture`，`font.cpp:446/487`），绘制时逐字形 `draw_texture`（`font.cpp:595-616`）：

- 一页 ADV 正文（~300 字，含描边+阴影 3 个 pass）≈ **600–900 次 draw call/帧**，每次伴随纹理绑定 + VBO 上传 + uniform 设置。
- 字形纹理终生不淘汰（`glyph_cache`/`edge_cache` 无上限），长流程下纹理对象数持续增长。
- 描边是独立栅格的 edge 位图（FT_Stroker），质量正确但使 draw call 翻倍。

**优化方向（按收益排序）：**

1. **字形图集（glyph atlas）**：把同 face/ppem 的字形打包进 1–2 张 1024² 图集（天空盒式行列排布或 stb_rect_pack），文本 pass 内按图集合并成一个动态 VBO 批次。draw call 从 O(字形数) 降到 O(1–2)。这是文本场景最大单项收益。
2. **draw call 合批**：即便不做图集，也应把同纹理/同 blend 的连续 quad 累积到一个增长型 VBO，按纹理切换 flush。场景层绘制同样受益。
3. **状态去重**：`apply_draw_state` 缓存上次 blend/scissor/viewport/uniform 值，相同则跳过 GL 调用。
4. **present 的 `glGetError` 改为编译期/环境变量门控**（`OA_RENDER_DIAG` 打开才 drain）。
5. **静态帧跳过**：桌面线有 static-frame skip；OHOS 为让 vsync 阻塞每帧都 `render_end()` 重 blit（`main.cpp:2177-2184`）。可改为：静态时仍 SwapWindow（保 vsync 节拍）但跳过 FBO→window 的 blit（back buffer 内容不变，无需重画）；或更进一步用 `SDL_GL_SetSwapInterval` + 帧回调只在脏时交换。**注意**：需实测各家驱动的 back buffer 保持语义，若 swap 后内容未定义则必须重 blit——KR2 每帧重画就是为了规避这个差异。建议先做 1/2/3，此项放最后验证。

### 1.3 流式纹理（视频/E-mote）每帧重建

`upload_host_frame` 每帧 **销毁旧纹理 + 新建纹理 + lock + memcpy + unlock**（`renderer.cpp:890-929`），注释说明这是为了绕过桌面软件渲染器下 in-place 更新不生效的问题。但在 GLES 路径上 `update_texture`（`glTexSubImage2D`）是可靠的：

- **OHOS/GLES 下改为 in-place 更新**：尺寸不变时直接 `glTexSubImage2D` 到既有纹理；尺寸变化才重建。消除每帧一次全尺寸纹理分配（1920×1080 RGBA ≈ 8 MB/帧的驱动侧分配+拷贝）。
- 更进一步可用 PBO 双缓冲或 `GL_EXT_texture_storage`（immutable texture）减少驱动内部拷贝；作为第二阶段。

### 1.4 E-mote CPU 光栅

E-mote 姿态重光栅默认节流 ~8 fps（`render_interval_ms_ = 125`，`emote_player.h:352`），全分辨率画布 + 多线程行带（`emote_render.cpp:1027-1041`）。已有 GPU 部件合成路径（`emote_render_parts`）。优化项：

- 确认 OHOS 真机走 GPU 部件路径而非 CPU 光栅回退（`OA_EMOTE_DEBUG` 日志核对）。
- 若必须 CPU 光栅，`OA_EMOTE_HALFRES=1` 可半分辨率 raster（有 2× 拉伸模糊代价，作为低端机 fallback 环境变量保留）。

---

## 2. 文件加载分析

### 2.1 当前路径

```
resolve_image(name)
  → 探测 3 个候选名（原名 / +.png / +.jpg）
  → overlay_read / fs_->read（PhysFS 全局递归锁，整文件读进 vector）
  → decode_image（libpng 简化 API / libjpeg，同步，tick 线程）
  → make_texture 上传
  → decoded[name] + textures[key] 双缓存，终生不淘汰
```

问题点：

1. **同步解码在 tick 线程**：1600×900 PNG 在 ARM 上解码 20–60 ms，场景切换/立绘出现即掉帧。DecodePool（`decode_pool.cpp`，默认 min(4, 核数) 线程）目前只服务音视频，**图片没有走异步**。
2. **无负缓存**：图片缺失时 `resolve_image` 返回 nullptr 且不记录，引用缺失图的层**每帧**重新探测 3 个候选 × 2 个文件系统面（`renderer.cpp:716-745`），每层每帧最多 6 次加锁 open/close。
3. **PhysFS 全局递归锁**（`physfs_fs.cpp:48-51`）：音视频 decode worker 与 tick 线程的读互相串行。锁是必要的（PhysFS 非线程安全），但应缩短临界区 + 减少进入次数。
4. **无预取**：场景切换用到的 bg/立绘/rule 图本可在脚本解析阶段（load 场景标签时）提前投递到 DecodePool。
5. **缓存无淘汰**：`decoded`（CPU RGBA）+ `textures`（GPU）双份常驻。长流程（多章节）内存单调增长，低端机有压力。`decoded` 的 CPU 像素在上传后仅用于 hit sampling 和 mask 合成，可按需保留。

**优化方向：**

1. **图片异步解码**：`resolve_image` 未命中时投递 DecodePool（读文件+解码在 worker），tick 线程本帧先用占位（不上屏或上帧保留），完成后回调上传纹理。配合"场景切换时批量预取本场景引用图"效果最佳。
2. **负缓存**：解码失败/文件不存在也写入 `decoded`（空 Image 标记），避免每帧重探测。
3. **缓存淘汰**：`textures` 按 LRU/引用计数淘汰 GPU 纹理（`decoded` 保留 CPU 像素以便重建）；或场景切换（`[alldelete]`/章节跳转）时整体清一轮。需先确认 hit sampling 的调用点是否容忍重建延迟。
4. **锁收缩**：PFS 读取的 open→read→close 已在锁内完成且必须如此；收益点在于减少调用次数（负缓存、预取、批量读），不建议拆锁。

---

## 3. Lua / 运行时 tick 分析

### 3.1 现状

- 内嵌 **Lua 5.1.5**（`third_party/lua-5.1.5`，解释执行，无 JIT），随主工程以 RelWithDebInfo（`-O2 -g`）编译。
- 每个 tick 触发 `onEnterFrame`（`runtime.cpp:766-767`），tick 频率 = 面板 vsync。
- **OHOS 上 tick 无 60Hz 上限**：`pace_windowed_frame` 在 OHOS 被编译排除（`main.cpp:273-303`），完全靠 `SwapInterval(1)` 阻塞。90/120/144Hz 面板上 Lua 钩子、tween 求值、音频 pacing 全部按面板频率运行——120Hz 设备脚本开销是 60Hz 设计的 2 倍。
- 未见 `lua_gc` 步进调优；每帧 Lua 侧若有表/字符串分配，默认 GC 参数下会有周期性毛刺。

**优化方向：**

1. **tick 与渲染解耦 / 逻辑帧率封顶**：脚本逻辑（onEnterFrame、tween、wait 解析）固定 60Hz 累加器推进；渲染仍按 vsync 呈现（静态帧本来就会跳过场景遍历）。这是高刷设备上最大的单项 CPU 收益，且与桌面线 `frame_pace_hz` 语义一致。注意验证依赖高频率回调的脚本模式（FPM dummy-click 等）在 60Hz 下行为不变——桌面线本来就是 60Hz 封顶，风险低。
2. **GC 调优**：启动后 `lua_gc(L, LUA_GCSETPAUSE/SETSTEPMUL)` 调宽（如 pause=200, stepmul=200），或在每帧 onEnterFrame 后做固定小步进 `LUA_GCSTEP`，把 GC 摊平到帧预算内。需实测脚本分配速率。
3. **LuaJIT 替换**（大改，P3）：5.1 语法兼容，脚本密集场景 3–10×。但引入运行时依赖与兼容性风险（Artemis 脚本有用到 5.1 边角语义的可能），仅在 1+2 不够时考虑。

---

## 4. 编译开关与链接

### 4.1 现状（`build-harmonyos.ps1` → Strawberry CMake + Ninja + `ohos.toolchain.cmake`）

- `CMAKE_BUILD_TYPE=RelWithDebInfo` → Clang `-O2 -g`。
- **无 LTO**（未设 `INTERPROCEDURAL_OPTIMIZATION`）。
- **无 `-mcpu`/`-march`**：arm64 基线通用代码，未利用目标 SoC 的扩展。
- **`-g` 调试信息保留在 .so**：`libopenartemis.so` ~24 MB（strip 后预计 6–8 MB）。hvigor 打包未做 native strip。
- vendored lua51/physfs/vorbis/theora 同吃 `-O2`，无独立调优。
- C++20、`-fPIC` 已就位。

### 4.2 优化项（全部低风险，建议一次做完）

| 开关 | 值 | 说明 |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release`（或保留 RelWithDebInfo + 下方 strip） | Release = `-O3` |
| `INTERPROCEDURAL_OPTIMIZATION` | `ON` | LTO；跨 TU 内联，对渲染/运行时这种多文件热点收益明显。vendored 静态库一起参与 |
| `CMAKE_CXX_FLAGS_RELEASE` 追加 | `-mcpu=generic -ffp-contract=fast` | 保持通用性；**不要** `-ffast-math`（破坏像素对照基线） |
| strip | `llvm-strip --strip-unneeded`（POST_BUILD） | .so 24 MB → ~7 MB，加载更快；保留一份未 strip 副本供崩溃符号化 |
| `-fvisibility=hidden` + 显式导出 | 可选 | 减少动态符号表，略微加快 dlopen |

注意：LTO 后需全量重跑桌面 sdl 线像素对照测试（`docs/TESTING.md`），确认 `-O3`+LTO 不改变渲染结果（理论上不影响，但本项目有逐字节基线，必须验证）。

---

## 5. 实施路线

### 阶段 1（本周，纯构建+小改，零行为风险）

1. Release + LTO + strip（§4.2）。
2. `glGetError` drain 加环境变量门控（§1.2-4）。
3. 图片负缓存（§2.1-2）。
4. GLES 路径视频/E-mote 纹理 in-place 更新（§1.3）。

### 阶段 2（下周，渲染管线）

5. 字形图集 + 文本合批（§1.2-1）。
6. 通用 draw call 合批 + 状态去重（§1.2-2/3）。
7. 逻辑 60Hz 封顶（§3.1-1），真机验证 FPM/iMel 系脚本行为。

### 阶段 3（按需，结构性）

8. 图片异步解码 + 场景预取（§2.1-1/4）。
9. 纹理 LRU 淘汰（§2.1-3）。
10. Lua GC 调优（§3.1-2）；仍不够再评估 LuaJIT。

### 验证矩阵（每阶段必跑）

- 桌面 sdl 线逐字节像素对照（`docs/TESTING.md` 的 journey 套件）——任何渲染改动不得破坏基线。
- OHOS 真机：60Hz 与 120Hz 面板各一台，记录场景切换帧时间、文本页稳态帧率、OP 视频播放帧率、30 分钟长流程内存曲线。
- `OA_RENDER_DIAG` / `OA_AUDIO_DIAG` 前后对比。

---

## 6. 关键文件索引

| 主题 | 文件 |
|---|---|
| GLES 后端（绘制/状态/present） | `src/core/render/backend_gles.cpp` |
| 场景合成 / 纹理缓存 / 流式上传 | `src/core/render/renderer.cpp` |
| 字形缓存与文本绘制 | `src/core/render/font.cpp` / `font.h` |
| 图片解码 | `src/core/media/image.cpp` |
| PFS/PhysFS 读取与全局锁 | `src/core/fs/physfs_fs.cpp` |
| 解码线程池 | `src/core/media/decode_pool.cpp` |
| 运行时 tick / onEnterFrame | `src/core/runtime/runtime.cpp` |
| Lua 宿主 | `src/core/runtime/runtime_lua.cpp` |
| 帧节拍（OHOS vsync / 桌面 60Hz 封顶） | `src/app/main.cpp:273-303, 2177-2184` |
| E-mote 光栅节流 | `src/core/emote/emote_player.h:352` |
| 构建脚本 | `build-harmonyos.ps1` / `CMakeLists.txt` / `cmake/oa_deps.cmake` |
