# ART3M1S 参考调研与兼容性改造记录

> 参考项目：[`Alphaly2K/art3m1s`](https://github.com/Alphaly2K/art3m1s)（Flutter 宿主）与
> [`Alphaly2K/art3m1s-core`](https://github.com/Alphaly2K/art3m1s-core)（Rust 运行时）。
> 本文记录：实测差距、已落地改造、参考依据、以及本轮**不做**的部分与触发条件。
>
> 许可纪律：art3m1s-core 为 MPL-2.0。本仓库**只借鉴行为与设计**，不复制其代码/数据文件；
> 下方每条"参考依据"都是行为级对照（代码、CHANGELOG、文档语义），C++ 侧实现为本仓库自写。

## 1. 起点：本机 4 个真包实测（改造前）

桌面 `build_sdl2` 主机 + headless，改造前基线：

| 真包 | 改造前 | 改造后 |
|---|---|---|
| `tmny31` | 正常启动 | 正常（无回归） |
| `gzsq` | 正常启动 | 正常（无回归） |
| `jianyu` | `updater.init` 报 `attempt to call field 'execute' (a nil value)`（被容错） | 无该报错 |
| `Ar_枫笛_脏翅膀_od`（中文目录） | 中文路径直接抛 `filesystem error: Cannot convert character sequence`；换 ASCII 联接路径后暴露真因：`io.open("savedata/…","wb")` 拿不到句柄 → `system_load` 失败 → `tick error: jump: label not found: game_start`，**完全无法开局** | 中文路径直接开局，进入标题（`title_init executed`） |

复现命令：`tools/real_game_smoke.ps1`（见 §5）。

## 2. 差距表（与 art3m1s / art3m1s-core 对照）

| 能力 | art3m1s 的做法 | openartemis 改造前 | 本轮结论 |
|---|---|---|---|
| 宿主文件命名空间 | FileProvider 把 PFS/解包目录/存档统一成一个逻辑命名空间，脚本文件 IO 落在托管存档区 | Lua `io.open` 直接落进程 CWD，且 PFS 内资源无法用 stdio 打开 | **已实现**：`io.open` 走"存档根（可写）→ 项目文件系统（只读）"（`runtime_lua_file.cpp`） |
| 队列 call 语义 | 排队 `[call]` 拥有脚本流，调用方后续标签延后到 return（`QueuedCallBarrier`） | 排空队列时直接切到被调用脚本，调用方标签在错误上下文执行 | **已实现**：同语义屏障（`runtime_iet.h` QueuedCallBarrier + `restore_completed_queued_barriers`） |
| 宿主服务缺失 | 需宿主回调，未接线时脚本降级 | 直接移除 `os.execute` 等字段 → 脚本取值 nil 抛错 | **已实现**：保留"不可执行"语义但返回失败值（stub） |
| 非 ASCII 路径 | 平台层统一 UTF-8 ↔ 原生路径 | Windows 上 `path(std::string)` 按 ACP 解码，中文目录抛异常 | **已实现**：`core/util/path_utf8.h` 统一转换 + 入口 argv/env 归一化 |
| 每游戏兼容清单 | `art3m1s.json`：`reportedOs` / `fontOverride` / `environmentPatchEnabled` / `inputGate` / 译文路径 | 只有 `--platform` CLI，无清单来源 | **已实现引擎侧**：`oa_compat.json`（兼容读 `art3m1s.json` 同名子集）；VP 接线按计划后续接入 |
| 字体覆盖 / 缺字 | `art3m1s_set_font_override`：宿主提供 TTF/OTF 作全部脚本字体的光栅化来源 | 无 | **已实现**：清单 `font_override` → `FontSystem::set_font_override` |
| 图片异步解码 / 预取 | Rust 侧解码与合成解耦 | tick 线程同步解码 | P1（见 §4） |
| 纹理淘汰 | E-Mote/字体纹理缓存 + 释放 | 纹理常驻无淘汰 | P1 |
| 脏区渲染 / 静止帧跳过 | 0.3.0 引入 damage + static-frame skip | 有 static-frame skip，无 damage 区域 | P1 |
| 视频 GPU 路径 | 运行时 FFmpeg 解 YUV420P，GPU 侧转纹理；解码节奏独立于帧循环 | theora/ffmpeg 走 CPU YCbCr→RGBA | P1 |
| ASTC/DXT5 资源压缩 | 0.3.0 移动端 ASTC + DXT5 | E-mote 图集有 DXT5/BC7，普通图片无 | P1（按收益排期） |
| Profiler 埋点 | 33 个分段计时 + 500ms 发布窗口 | 有 `OA_*` 诊断开关，无分段计时/汇总 | P1 |
| `tag.ini` 解析 | 按项目字符集读可选 `tag.ini` | 无 | P2（本机 4 包普查无命中，见 §4） |
| 自定义 shader | Artemis HLSL 子集 → SPIR-V → 后端 | 无（`lyshader` 只作为配置标签被接受） | P2（本机 4 包普查 0 命中，见 §4） |

## 3. 本轮落地的改动（可用 `git log` 逐条追溯）

1. **Lua 文件 IO 虚拟化**（`src/core/runtime/runtime_lua_file.cpp`）
   - `io.open/io.close/io.lines/io.type` 换成存档根感知实现：写模式在内存里缓冲，`flush`/`close`
     时经 `SaveStore` 落盘并自动建父目录；读模式先查存档根（写完立刻能读回），再查项目文件系统
     （PFS 内的文本文件也能读）。
   - 路径规则与存档域一致：`\` 归一为 `/`、丢弃空段与 `.`、拒绝 `..` 与 `:`（盘符/流）、
     容忍前导 `/`（沙箱内映射）。非法路径返回 `nil, msg`，不抛异常。
   - `OA_LUA_STDLIB=stock` 仍回到原生 `io`/`os`（A/B 臂）。
2. **宿主服务桩**（同文件）：`os.execute`/`os.system` → 返回非零失败码；`os.remove`/`os.rename` →
   `nil, "operation not permitted"`；`os.tmpname` → 返回存档根内的沙箱相对路径；`io.popen`/`io.tmpfile`
   → `nil, "not supported"`；`io.input`/`io.output` → `nil`。目的：框架把结果当值比较，而不是
   "attempt to call field 'x' (a nil value)" 直接中断 boot。
3. **排队 call 屏障**（`src/core/runtime/runtime_iet.{h,cpp}`）
   - 新增 `QueuedCallBarrier`：排队 `[call]` 执行时，把队列里"调用方 continuation"整段摘下，
     被调用脚本先跑；该帧（含嵌套帧）return 后再按 FIFO 放回队列尾。
   - 新增 immediate/deferred 两段队列语义：`e:tag{}` 保持队列前缀（immediate），`e:enqueueTag{}`
     追加在尾部（deferred）；护栏按前缀长度切分，行为与参考实现一致。
   - 这是枫笛 `initLua2()`（`e:enqueueTag{call}` + `e:enqueueTag{jump label="game_start"}`）能推进的
     直接原因；新增回归测试 `tests/tag_queue_barrier_test.cpp`。
4. **UTF-8 路径统一**（`src/core/util/path_utf8.{h,cpp}`）
   - `native_path_from_utf8` / `path_to_utf8`：Windows 走 `u8string`（不受 ACP 影响），POSIX 字节等价。
   - `host_bytes_to_utf8` / `env_utf8`：入口处把 `argv`/`getenv`（Windows ACP）归一成 UTF-8 契约。
   - 替换点：`src/app/main.cpp`、`src/app/platform/*.cpp`、`src/core/fs/physfs_fs.cpp`（含
     `std::ifstream` 打开 PFS 卷）。
5. **每游戏兼容清单**（`src/core/fs/compat_config.{h,cpp}`）
   - 读取顺序 `oa_compat.json` → `art3m1s.json`（PFS 内或目录工程根皆可），字段表见下。
   - 优先级：CLI/env（如 `--platform`）> 清单 > 项目默认；无清单时行为与改造前完全一致。
   - 新增单元测试 `tests/compat_config_test.cpp`。

| 键（snake_case / art3m1s 拼写） | 作用 | 状态 |
|---|---|---|
| `platform` / `reportedOs` / `reported_os` | system.ini 段 + 脚本可见 `os` | 生效（`--platform` 优先） |
| `charset` / `scriptCharset` | 脚本解码字符集 | 生效 |
| `font_override` / `fontOverride` | 游戏内 TTF/OTF，作为全部脚本字体来源 | 生效 |
| `inputGate.keyboard` / `inputGate.wheelToKeys` | 键盘、滚轮→按键映射开关 | 生效 |
| 其他（`translationPatchPath`/`vndbId`/`experimentalElunaEnabled` 等） | 宿主/后续能力 | **接受但忽略**（不影响引擎行为，读取不报错） |

## 4. 本轮不做 + 触发条件（P1/P2）

- **P1 性能批次**（`docs/PERFORMANCE_OPTIMIZATION_PLAN.md` 的剩余项）：分段 Profiler、图片异步解码/预取、
  纹理淘汰、脏区渲染、视频 GPU 路径与解码预热、构建开关（`-fvisibility=hidden` 等）。
  触发条件：兼容批次稳定（真包冒烟全绿、现有 ctest 全绿）后按收益排序实施。
- **P2 `tag.ini` 解析**：触发条件 = 出现某真包把立绘命令当对白/宏参数串用的证据；
  本机 4 包普查未命中。
- **P2 自定义 shader（`lyshader`/HLSL 子集）**：触发条件 = 出现某真包实际使用 `lyshader` 且画面缺失的证据；
  本机 4 包（204 个 `.asb` 场景脚本）普查 `lyshader`/`hlsl` 命中数均为 0。
- **P2 环境补丁（`environmentPatchEnabled`）**：需要逐游戏确定"要屏蔽哪条检查"的语义，
  在拿到具体失败证据前不做通用实现；清单字段先接受不生效。
- **VP 侧接线**：`oa_compat.json` 的写入/设置页/导入流程属 VintagePomelo 仓库，本轮只保证引擎读取入口稳定。

## 5. 验证方式

```powershell
# 真包冒烟（4 个本机包 × 300 帧；失败会打印该包完整输出）
.\tools\real_game_smoke.ps1 -Frames 300 -Game `
  'D:\Games\Pomelo\Ar_枫笛_脏翅膀_od\root.pfs', `
  'D:\Games\Pomelo\jianyu\root.pfs', `
  'D:\Games\Pomelo\gzsq\root.pfs', `
  'D:\Games\Pomelo\tmny31\root.pfs'
```

- 冒烟判定：退出干净（`[app] bye (frames=…)`）且不出现 `tick error` / `label not found` /
  `calllua error` / `terminate called` / `filesystem error` / `assertion failed`。
  `[app] win exception …` 是 Windows 宿主对"内部已捕获异常"的标记，正常 boot 也会出现，**不**作为失败信号。
- 单元测试：`tag_queue_barrier_test`（队列屏障）、`lua_file_io_test`（io 虚拟化 + 服务桩）、
  `compat_config_test`（清单解析/优先级/容错）。
- 诊断开关：`OA_JUMPDBG=1`（标签缺失上下文）、`OA_LUA_FILE_DEBUG=1`（io 打开/落盘轨迹）。
- 真机（OHOS）验证待在设备可用后按同一脚本补做。
