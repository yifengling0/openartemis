# Compatibility Lab Progress

## 2026-09-25 第 5 轮 — 整理定稿 + OHOS .so 真机构建

### What changed
- **全量改动盘点定稿**：13 改 + 3 新增，全部有归属——
  - 兼容修复（保留）：`[WINDOWS]` 几何优先、getScriptStack 实装、builtin 标签
    裁决语义、s.current_message_layer、字体接管/负缓存、媒体扩展名探测、
    s.windowstouch/enterbuttonassign 种子。
  - 性能项（保留）：Lua GC 调宽（pause=300）、渲染分相计时、纹理 LRU 等
    （本轮之前已落地）。
  - 诊断面（保留，全部 env 门控、默认零开销）：OA_TAG_TRACE / TEXT_IN_TRACE /
    SCENE_LAYERS / SCENE_HANDLERS / CLICK_ID(+FAST) / TRACE_JSONL / PROFILE。
  - 测试基建（保留）：compatlab/（语料 7+11、Bug Museum 8 条、扫描器、
    合成生成器、回归器）。
- **OHOS 入口 trace 对等**：`oa_harmony.cpp runner_main` 补
  `OA_TRACE_JSONL` configure（此前只在桌面 main()，真机 trace 不生效）。
- **libopenartemis.so 构建**：build_harmony_arm64 增量编译，2,045,952 字节
  （Release+LTO+strip），三处 libs（entry/phone/padpc）已部署；
  patch_elf_soname 通过（DT_NEEDED 17 项全短名）；5 个 dlsym 导出
  （runner_main/requestShutdown/cleanupSDL/getLastRubyError/get_last_ruby_error）齐全。
- **ETS product 同步**：GamePlay.ets 的 Artemis(16/17) 默认 TOUCHPAD 修复
  已同步 phone+padpc。

### Evidence
- 桌面回归：游戏语料 7/7 + 合成 11/11（构建定稿后复跑）。

### 真机验证清单（DevEco 正式签名 HAP 后）
1. **不能点修复**：Artemis 游戏直接进（不动设置）→ 屏幕点按应有响应
   （默认触摸板模式：拖动移光标、轻点=左键）。
2. **分辨率修复**：之前分辨率不对的包 → hilog 找
   `[project] stage geometry from [WINDOWS]`。
3. **对话/字体/音乐**：fengti 类包 → hilog 找
   `default face adopted from script font`、`media not found`（应为 0）。
4. **诊断**：`hdc hilog | findstr openartemis`；OA_TRACE_JSONL 可由宿主
   setenv 后取事件流文件。
5. 回归纪律：真机确认的每一条 → 在 corpus.json 补 assert（BUG-0005 的
   geometry 断言就在等真机包名）。

### Next step
1. 真机结果回填（geometry/touchpad 断言）。
2. E-mote 等完整 tmny31 包（BUG-0008 前置）。
3. M6 真机 runner 自动化。

## 2026-09-25 第 4 轮 — M5 合成套件 v1（11 用例）+ E-mote 语料澄清

### What changed
- **E-mote 语料澄清（用户指正 + 取证）**：E-mote 参考游戏是 **tmny31**
  （`list_windows.tbl: fg_ext = .psb`，立绘即 PSB 模型）；枫笛 020xxx 是普通
  fg 立绘。但本地 tmny31 包**缺整个 image/fg/ 模型目录**（逐卷核对无 .psb）——
  E-mote 场景测试等完整包或官方 sample（BUG-0008 记录前置条件）。
- **M5 合成套件 v1**（`tools/compatlab/gen_synthetic.py` →
  `compatlab/synthetic/` 11 个目录工程 + `corpus_synthetic.json`）：零游戏
  资产、可入库的永久回归层。用例：boot/var 表达式/text 流水线/trans+flip/
  wait 边界/call-return（跨文件）/loop/chgmsg 双层/ruby/lua 块/call-self 探针。
- **headless 末态摘要统一出口**：`[exit]` 提前退出也出 headless end 行
  （此前只有帧数目标路径有——synthetic 用例 1 帧即退曾被误判 FAIL）。
- **钉住引擎表达式契约**：`data=`/`estimate=` 需 `$` 前缀才求值（裸名=字面量，
  曾让计数循环死转）；`[loop estimate=]…[/loop]` 是循环的引擎形态；
  `[jump]` 在 `[if]` 块内会触发 "missing /if"（不得用 if+jump 写循环）。
- **新引擎怪癖候选（BUG-0007）**：`[call file=]` 指向当前执行脚本自身 →
  return 落点错（双 return、Completed 收尾）。跨文件 call（真实形态）正常。
  探针用例钉住现状，待对照参考实现定处置。

### Evidence
- 合成套件 **11/11 PASS**（含对齐后基线）。
- 游戏语料回归 **7/7 PASS**（像素差 0.0%、digest 零漂移）。
- 本轮修复的 runner/生成器 bug：末态摘要单出口、断言 needle 形态
  （`] [tag]`→`[tag]`）、Lua 侧标签不走 step trace 的断言面。

### Known limitations
- `[exit]` 提前退出时 bgm/se/voice 字段为退出时刻值（0）——摘要语义按
  "退出时刻快照"理解。
- call_self 怪癖根因未定位（待真实游戏证据决定是否按 reference 修）。

### Next step
1. E-mote：等完整 tmny31 包（BUG-0008 前置）→ emote 域 trace + 分域断言。
2. M5 扩容：ASB 参数形状驱动的边界用例批量生成（profiles 数据已备）。
3. regression.py 消费 host.digest JSONL；M6 真机 runner。

## 2026-09-25 第 3 轮 — 立绘/E-mote 覆盖 + M7 像素差分 + 驱动加速

### What changed
- **OA_CLICK_FAST=1**：快速驱动节奏（hover 90→10 帧、cooldown 60→10）——开场文件
  动辄数百个点击点（fengti 开场 536 个 click 阻塞），慢节奏 150 帧/点在 2 万帧内
  走不到立绘。
- **scene_layers_dump headless 化**：rect 改为可选（无渲染器时省略），挂进 headless
  循环；dump 配额 12→200（开场 UI 状态多，12 次在立绘出现前就耗尽）。
- **语料新增 2 条立绘回归**：
  - `fengti_sprites`（headless fast 深驱 20000 帧）：assert `file=image/fg/` 立绘
    图层必须物化（TatiFa→set_stand→[lyc] 链路）。
  - `fengti_sprites_dump`（窗口线 6500 帧 + 终帧 PPM）：**立绘渲染面像素基线**。
- **M7 像素差分 v1**：regression.py 对 windowed_dump 终帧与
  `pixel_baselines/<id>.ppm` 逐字节比对（实测窗口线 dump **完全确定**：两跑
  2764816 字节逐字节一致），阈值 `pixel_threshold_pct`（默认 0.5%），超阈 FAIL。
- **_corpus 教训**：语料 env 少放一个 `OA_CLICK_FAST` 导致回归跑用慢节奏、
  深驱断言假阴性——驱动节奏是语料契约的一部分，env 必须显式。

### Evidence
- **立绘链路全程验证**：深驱 143 点击后 `[TatiFa]` 执行 39 次，运行时面
  `id=1.1.0.10.1 file=image/fg/002_eri/tati_020142 vis=1 alpha=1.00`；
  窗口线终帧 PNG 呈现完整立绘（白发公主特写）+ 对话框 + 菜单栏 + 标题
  「Blind Alley」。此前"疑似立绘缺失"结论：runtime 与渲染面均正常，
  未达立绘深度（开场 536 点击）即撤退是误判来源。
- 回归 **7/7 PASS**，像素差 0.0%，digest 零漂移（深驱 20000 帧的
  text_ev=1233 两轮完全一致——headless 确定性在长驱动下成立）。
- 深驱全程 3 个音频播放器活跃（BGM/SE）——媒体扩展名探测在整场开场下受考。

### Known limitations
- E-mote 专项断言仍未分立（020xxx 立绘走 E-mote 或静帧尚可区分，待 emote 域
  trace 挂钩后分域断言）。
- 像素基线是"单帧终态"，覆盖不到动画中段（M7 后续：多帧 checkpoint）。

### Next step
1. M5：按 profiles 的 ASB 参数形状生成 synthetic 边界用例。
2. E-mote 域 trace（load/update/draw 计数进 JSONL）→ 分域断言。
3. 真机 runner（M6）接入同一回归面。

## 2026-09-25 第 2 轮 — M1 完成 + 扫描器 pf8 修复 + s.* 缺口收敛

### What changed
- **M1 JSONL TraceSink**（`src/core/util/trace_sink.h`，header-only）：`OA_TRACE_JSONL=<path>`
  开启，四域事件——script（每条标签）/text（正文入域，只记字数）/asset（纹理解码、媒体缺失）/
  host（headless 摘要）。未配置零开销。**验收**：fengti 3000 帧驱动 = 199 行
  合法 JSON、0 坏行（修掉数字启发式对 `1.90.0` 层 id 的误判）。
- **扫描器 pf8 修复（实验室工具 bug）**：卷发现改成引擎 `sibling_volumes` 同款
  （`.NNN` 全枚举、容忍缺号——tmny31 实测 000/002/003/004 布局）；XOR 相位从
  「档案全局偏移」改为「文件内窗口偏移」（引擎 io_read：`ctx->pos` 是文件窗口位置）。
  此前所有 pf8 载荷解码都是错的——gzsq/fengti 看似成功全因 sidecar 目录兜底。
- **语料普查更新**（修复后）：
  ```
             asb_cmds  iet_tags  lua_files  handlers  unknown_svars
  fengti      106       124       34         291        7
  gzsq        6         90        59         111       10
  jianyu      6         20        74         123       12
  tmny31      6         17        57         110       10
  ```
- **s.* 缺口收敛（先取证再动手）**：逐变量提取游戏内用法后，15 个"缺失变量"
  实际分三类：
  1. 游戏自写自读（`s.status.controlskip/clickskip`、`s.videovol`、`s.segain.*`）——
     引擎 `[var]` 写路径本就支持 s.* 域，**无需改动**；
  2. 平台条件变量（`s.builddate/s.datapath` DMM 判定、`s.wasm_sync_*`）——
     **缺失即正确**（非 DMM/wasm 构建的正确 no-op），显式不种子；
  3. 只读环境查询补种子：`s.windowstouch="0"`、`s.enterbuttonassign="0"`
     （seed_engine_variables_impl，注释记录取证出处）。

### Evidence
- M1：fengti 驱动 3000 帧 → 199 行合法 JSONL（script 175/text 23/host 1）。
- 回归：5/5 PASS，与基线零 DRIFT（M1+种子不改变行为）。
- 扫描器：jianyu `system/first.iet` 解出明文文本；`script/brandlogo.ast`
  解出 `astver = 2.0`（此前乱码）。

### Known limitations
- asset 域在 headless 为 0（资源解析在绘制路径上）——asset 差分需窗口线或
  后续把 resolve_image 提前挂钩。
- windowed_dump 的 PPM 仅做存在性检查，像素差分 oracle（M7）未建。

### Next step
1. M5 起步：按 profiles 的 ASB 参数形状生成 synthetic 边界用例（trans/wait/var）。
2. JSONL 驱动回归报告（regression.py 消费 host.digest 行，替代文本解析）。
3. E-mote 场景覆盖：枫笛驱动到 E-mote 立绘段（需 CLICK_ID 走更深的剧情）。
4. M7 视觉差分：windowed_dump 终帧与基线 PPM 的像素比对。

## 2026-09-25 — M0 完成 + 回归闭环 v1（本回合）

### What changed
- **M0 基线审计**：`COMPATLAB_BASELINE.md`（构建/脚本/Lua/渲染/音频/输入/封装/E-mote/测试/观测面盘点 + 缺口清单）。
- **M2（最小形态）**：headless 合成点击驱动——`OA_CLICK_ID="@x,y;@center;id"` 在
  无窗口下推进剧情（修复了 `@` 坐标与列表分隔符的逗号冲突；headless 循环消费
  left_click_edge 后清零）。id→矩形仍需窗口线（layer_world_rect 依赖渲染器）。
- **M4 静态扫描器**：`tools/compatlab/scan_game.py`——PFS pf2/6/8（含 pf8 XOR、
  分卷拼接）+ sidecar 双层读取；ASB 命令普查（含参数形状）、IET 标签、Lua 面
  （tags.* 处理器 / e:var 系统变量 / e:tag 发射）、system.ini 分段几何、资产类型计数。
  只输出计数与名字，不导出资产（plan §6）。
- **回归运行器**：`tools/compatlab/regression.py`——语料批量运行（headless +
  windowed_dump 两模式）→ 状态摘要（headless end 行 13 字段）→ Bug Museum 断言
  → 基线存储/差分（DRIFT 可见）→ JSON 报告 + 全量日志落盘。
- **M9 Bug Museum 种子**：`compatlab/bugs/README.md`（6 条）+ corpus.json 内 5 个
  自动断言（BUG-0001/0002/0004 headless，BUG-0003 窗口线）。
- 语料：`compatlab/corpus.json`（fengti 剧情、fengti_font 渲染面、tmny31/gzsq/jianyu 冒烟）。
  kskbl 经查为 SiglusEngine 游戏，移出 Artemis 语料。

### Evidence（首轮运行 2026-09-25_002937 / 002953）
```
fengti       PASS frames=6000 text_ev=69 layer_ev=2507   （剧情驱动 21 条正文入 adv 层）
fengti_font  PASS （窗口线 500 帧，字体接管行 + 终帧 PPM）
tmny31       PASS frames=1200 text_ev=9
gzsq         PASS frames=1200
jianyu       PASS frames=1200
两轮 digest 完全一致 → headless 线确定性成立（M2 验收标准达成）
```

### 静态扫描产出（覆盖引导的兼容缺口清单）
```
            pfs   files  asb_cmds  iet_tags  lua_handlers  unknown_svars
fengti      pf8   38807  106       124       291           7
gzsq        pf8   22392  6         90        111           10
jianyu      pf8   13053  0*        0*        0*            2
tmny31      pf8   3359   0*        0*        4             2
* 脚本可能在子封包/加密段，扫描器待扩展（记入 Next）
```
聚合未知系统变量（= 引擎缺口候选，按影响游戏数排序）：
`s.status.controlskip`(3)、`s.windowstouch`(2)、`s.enterbuttonassign`(2)、
`s.datapath`(2)、`s.builddate`(2)、`s.status.commandskip/alreadyread/windowbutton/avoid`、
`s.segain.*`、`s.clickskip`、`s.wasm_sync_*` 各 1。

### Tests / Pass-Fail
- 回归 5/5 PASS × 2 轮（第二轮零 DRIFT）。
- 修复回退验证：将 corpus 中 fengti 的 BUG-0002 断言手动注入失败（模拟）→
  runner 正确报 FAIL + asserts_failed（机制可用）。

### Known limitations
- 测试单测线（tests/ vcpkg Vorbis）本机不可构建——语料回归代偿。
- jianyu/tmny31 的 ASB/Lua 普查为 0：脚本可能在嵌套封包/加密段，扫描器需扩展。
- windowed_dump 会短暂弹窗口（<10s），CI 需接受或有头环境。
- E-mote 无专项用例（枫笛含 E-mote 素材，需先能自动走进对应场景）。
- kskbl（Siglus）超范围——本 Lab 只管 Artemis。

### Next step（按任务书优先序）
1. M1 JSONL TraceSink：TAG_TRACE/TEXT_IN/SCENE_LAYERS 收敛为结构化输出（差分/聚类的前置）。
2. 扫描器扩展：jianyu/tmny31 嵌套脚本解包；ASB→coverage 向量入库（compat.db schema 简化版）。
3. `s.status.*` 系统变量缺口逐个实现 + 每个配 synthetic 用例（先枫笛/gzsq 共用的 controlskip）。
4. 用例生成器：按 profiles 的 ASB 参数形状生成边界用例（M5 起步）。
5. OHOS device runner（M6）待真机车道稳定后接入同一 regression.py 报告面。
