# OpenArtemis 原生 HarmonyOS 交接

日期：2026-09-15  
范围：把 vendored OpenArtemis 编成 `libopenartemis.so`，经 VintagePomelo `engine_loader` 启动，对齐 KR2 / RPGRunner 的宿主行为。  
状态：**能启动、能出画、能出声；未宣布真机可玩。** 分卷 PFS、Lua 全流程、触控命中、存档、FFmpeg 影片、E-mote 在鸿蒙上仍待实机走完。

本文给后续接手的人（含其他 AI）用。上游引擎文档仍以 `README.md`、`docs/ARCHITECTURE.md`、`docs/TESTING.md`、`docs/PLATFORMS.md` 为准；那些文件**几乎不覆盖** VintagePomelo Harmony 车道。

---

## 1. 目标与硬约束

1. 原生通道必须真的在鸿蒙上跑起来，行为对齐 KR2 / RGSS 宿主，而不是再包一层 WASM。
2. **保留** `artemis_runtime` Web/WASM 通道。设置里是 `native | web`，默认 `native`。缺库或启动失败时**提示，不自动切 Web**。
3. 检测类型仍是 **16**（`WebEngineType.ARTEMIS`，特征 `root.pfs`）。原生加载用 **17**（`RGSS_VERSION_ARTEMIS_NATIVE`）。GamePlay 在 `rgssVersion === 16` 时 `setForcedGameVersion(17)`。
4. 在一局游戏能稳定停在标题/剧情之前，**不要**把 OHOS 写成 play-verified。
5. 未明确要求时不要 git commit / push。
6. 改 native 或 ETS 后，HAP 需要 **DevEco 正式签名**（`build-harmonyos.ps1` 的 debug 包通常不能当发布验证）。

包名是 `com.vintage.pomelo`，不是 pomelopro。

---

## 2. 仓库地图

| 路径 | 职责 |
|---|---|
| `vintage-pomelo/openartemis/` | 引擎本体（本工作 vendor 进来的） |
| `vintage-pomelo/openartemis/src/app/main.cpp` | SDL 宿主：窗口、触控→鼠标、tick、present |
| `vintage-pomelo/openartemis/src/app/harmony/oa_harmony.cpp` | Harmony `runner_main` / `requestShutdown` / `getLastRubyError` |
| `vintage-pomelo/openartemis/src/app/platform/platform_harmony.cpp` | 存档根、前后台 resume 闩 |
| `vintage-pomelo/openartemis/src/core/render/backend_gles.cpp` | GLES3 呈现、letterbox、竖屏偏移、vsync |
| `vintage-pomelo/openartemis/src/core/media/` | 音频回调 + 影片；Harmony 编进 FFmpeg |
| `vintage-pomelo/openartemis/src/platform/sdl2_bridge/` | SDL3 头伪装成调 SDL2 |
| `vintage-pomelo/VintagePomelo/entry/src/main/cpp/engine/engine_loader.c` | `dlopen("libopenartemis.so")` |
| `vintage-pomelo/VintagePomelo/entry/src/main/cpp/application/main.c` | 原生启动 argv：`openartemis <game_dir>` |
| `vintage-pomelo/VintagePomelo/entry/src/main/ets/pages/GameList.ets` | 16 → native GamePlay 或 web WebGamePlay |
| `vintage-pomelo/VintagePomelo/entry/src/main/ets/pages/GamePlay.ets` | 强制 17、竖屏偏移、虚拟鼠标/触摸板 |
| `vintage-pomelo/VintagePomelo/entry/src/main/ets/pages/GameSettings.ets` | `artemisRuntimeMode` |
| `vintage-pomelo/krkr2/cpp/krkrsdl_harmony.cpp` | **帧循环 / GLES / vsync 对照实现** |
| `vintage-pomelo/krkr2/cpp/krkrsdl_gl.cpp` | letterbox + `TAPIR_PORTRAIT_TOP_OFFSET` |
| `vintage-pomelo/RPGRunner/RPGRunner/sdl_misc.c` | 同样的 TAPIR 偏移；`SwapInterval(1)`（MKXPZ 除外） |

工作区根：`F:\MyProject`。引擎在 `vintage-pomelo/` 下，不是仓库根。

---

## 3. 启动链路（不要改错版本号）

```
GameList  (rgssVersion=16, artemisRuntimeMode=native)
  → GamePlay
      configureForcedNativeGameVersion: 16 → setForcedGameVersion(17)
      setPortraitTopOffset → env TAPIR_PORTRAIT_TOP_OFFSET (0=顶, 50=中, 100=底)
  → native thread / engine_loader
      dlopen libopenartemis.so
      runner_main(argc, argv)  argv[1]=游戏目录
  → oa_harmony.cpp
      若存在 <game>/root.pfs 则把数据源换成该文件
      SDL_AppInit(... "--renderer" "gles" "--platform" "android")
  → 主循环：PollEvent → SDL_AppIterate → SDL_Delay(1)
```

`--platform android` 是故意的：`system.ini` 读 `[ANDROID]`，没有则回退 `[WINDOWS]`（`src/core/fs/project.cpp`）。不要改成 `ohos`，除非同时改 ini 解析。

导出符号（对齐 KR2）：

- `runner_main`
- `requestShutdown` / `cleanupSDL`
- `getLastRubyError` **和** `get_last_ruby_error`（loader 两套名字都可能问）

---

## 4. 编译方案

### 4.1 工具链（Harmony .so）

| 项 | 必须用 | 不要用 |
|---|---|---|
| CMake / Ninja | `C:\Strawberry\c\bin\cmake.exe` + `ninja.exe` | MSYS2 cmake |
| 工具链 | DevEco `ohos.toolchain.cmake` | 自制 toolchain |
| 构建类型 | `RelWithDebInfo` | 日常不要 Debug（体积/性能） |
| 并行 | `-j8` 即可 | |

其它固定路径（写在 `VintagePomelo/build-openartemis-harmony-and-deploy.ps1`）：

- SDL 头：`F:/MyProject/SDL2/SDL/include`
- FFmpeg 头：`F:/MyProject/third_party_ffmpeg/install-harmony-aarch64/include`
- freetype 等：`F:/MyProject/Python-3.12.12/harmony_deps/aarch64/include`
- 预编译 `.so`：`VintagePomelo/entry/libs/arm64-v8a`（`PREBUILT_LIBS_DIR`）
- 构建目录：`vintage-pomelo/openartemis/build_harmony_arm64`
- 产物：同目录 `libopenartemis.so`（约 24MB RelWithDebInfo）

链接必须按**短名**（`libz.so`），不能把 Windows 绝对路径写进 `DT_NEEDED`。POST_BUILD 用 `patch_elf_soname.py` 把 `libfreetype.so.6` / `libavcodec.so.61` 等改成 HAP 里实际文件名。

`VintagePomelo/entry/src/main/cpp/CMakeLists.txt` 里有 `add_prebuilt_lib(openartemis libopenartemis.so)`，只是让 HAP 打包器收集该文件；运行时仍是 `engine_loader` `dlopen`，不是 `libentry.so` 静态链引擎。

### 4.2 增量编库（日常）

已配置过 `build_harmony_arm64/CMakeCache.txt` 时：

```powershell
& 'C:\Strawberry\c\bin\cmake.exe' --build 'F:\MyProject\vintage-pomelo\openartemis\build_harmony_arm64' --parallel 8
Copy-Item 'F:\MyProject\vintage-pomelo\openartemis\build_harmony_arm64\libopenartemis.so' `
  'F:\MyProject\vintage-pomelo\VintagePomelo\entry\libs\arm64-v8a\libopenartemis.so' -Force
Copy-Item 'F:\MyProject\vintage-pomelo\openartemis\build_harmony_arm64\libopenartemis.so' `
  'F:\MyProject\vintage-pomelo\VintagePomelo\products\phone\libs\arm64-v8a\libopenartemis.so' -Force
Copy-Item 'F:\MyProject\vintage-pomelo\openartemis\build_harmony_arm64\libopenartemis.so' `
  'F:\MyProject\vintage-pomelo\VintagePomelo\products\padpc\libs\arm64-v8a\libopenartemis.so' -Force
```

或一条脚本（会编库并拷三处；HAP 用开关跳过）：

```powershell
& F:\MyProject\vintage-pomelo\VintagePomelo\build-openartemis-harmony-and-deploy.ps1 `
  -SkipPackage -SkipInstall -SkipLaunch
```

缓存丢失时不要自己猜参数，跑上面脚本**不要** `-SkipConfigure`。

### 4.3 改了 ETS 之后

`GamePlay.ets` / `GameSettings.ets` / `GameList.ets` 改完后跑：

```powershell
& F:\MyProject\vintage-pomelo\VintagePomelo\hvigor\sync-vintagepomelo-product.ps1
```

否则 `products/phone` 仍是旧 ArkTS。然后 DevEco 编签 HAP。

### 4.4 装包与真机

- 脚本全流程：同一 `build-openartemis-harmony-and-deploy.ps1`（不 SkipPackage/Install）。它会再调 `build-harmonyos.ps1`。
- 当前验证习惯：native 拷进 libs → **DevEco 正式签名 assembleHap** → 安装。
- hdc：`C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe`
- USB 经常 `Offline`；无线 `hdc list targets` 选 `Connected` 那条。目标会变，不要写死 IP。
- hilog 标签：`openartemis`、`oa-kr2gl`、`gles`、`EngineLoader`。

### 4.5 Windows 桌面（对照 / 修核心逻辑）

对齐 KR2 `build_sdl2` 的 MinGW 车道，**不要**用 VS 默认生成器除非有人改过：

```powershell
& F:\MyProject\vintage-pomelo\openartemis\build-windows.ps1
```

产物：`vintage-pomelo/openartemis/build_sdl2/openartemis.exe`（脚本会旁路拷 MinGW DLL）。

本机冒烟游戏：jianyu（开始游戏曾 heap smash，已修；见第 6 节）。桌面 GLES 仍可能对 120Hz 用 interval N 把 present 收成约 60Hz，再 `pace_windowed_frame` 补余量。**Harmony 不要抄这套 60Hz 软件帽。**

---

## 5. 帧循环、vsync、音频（2026-09-15 对齐 KR2）

**vsync ≠ 60fps。** `SDL_GL_SetSwapInterval(1)` 等的是合成器一次 vblank。面板 60/90/120/144 都合法，那就是 tick 率。

| | KR2 Harmony | RPGRunner | OA Harmony（当前） |
|---|---|---|---|
| swap | `SetSwapInterval(1)` | 默认 1（MKXPZ 可关） | `SetSwapInterval(1)` |
| 每圈 | `Application->Run` + 绘制 + `SwapWindow` | `Graphics.update` 另有 RGSS `frame_rate` 债务 | `SDL_AppIterate`：tick + 绘制或 blit 缓存舞台 + `SwapWindow` |
| 让出 | `SDL_Delay(1)`，注释写明 VSync 负责节拍 | 债务 Delay | `oa_harmony` 循环末 `SDL_Delay(1)` |
| 软件 60Hz | **没有** | 脚本帧率，不是 vsync 本身 | **没有**（曾叠 vsync+Delay(60)≈30fps） |

关键实现：

- 上下文建完立刻 `SetSwapInterval(1)`：`main.cpp`（CreateContext 后）、`backend_gles.cpp`（OHOS 分支）。
- **禁止**再对 OHOS 调 `pace_windowed_frame(60)`。
- 静态画面也要 `render_end()`（blit 上一帧 stage FBO + swap）。不 swap 就等不到 vsync，循环会空转，Lua `onEnterFrame` 会炸频率。
- 双缓冲：只 swap 不重画会出未定义后缓冲，所以静态路径必须 blit `stage_rt`。
- 音频：SDL 回调线程，与 vsync 脱钩。tick 里 `MediaPlayers::update` 按 `delta_ms` 推 mix，pacer 约 2 个 device period。设备缓冲 **1024** frames @ 44100（约 23ms）。不要为了“卡顿”再改 512——上次卡是 30fps tick 饿回调。
- 影片泵也在 iterate 里，跟 vsync 走；A/V 时钟在解码器/音频队列。

120Hz 手机上 `onEnterFrame` 会 120 次/秒。时间类等待用 `delta_ms` 是对的。若某作 Lua 把“每次 vsync = 1 个 60fps 帧”硬编码且不换算 elapsed，会快一倍——KR2 在同机上也是面板 Hz。先观察，不要先加回假 60 锁。

启动后 hilog 应有：

```
[gles] vsync interval=1 display=N Hz (interval 1 = one vblank, not a 60 Hz lock)
```

`N` 可能是 0（SDL 查不到 refresh）；真正节拍仍是 compositor。

对照源：`krkr2/cpp/krkrsdl_harmony.cpp` 里 `sdl_render_frame`、`SDL_GL_SetSwapInterval(1)`、主循环 `SDL_Delay(1)`。

---

## 6. 已经踩过、不要退回去的坑

### 6.1 GLES 创建（“render 失败”）

OHOS 上 `SDL_GL_GetProcAddress` 查的是 **GLESv2**。VAO 等 GLES3 入口是 NULL。

正确做法（抄 KR2）：

- `#include <GLES3/gl3.h>`，链 `libGLESv3.so`
- **不要**在 `__OHOS__` 下把 `gl*` 宏到 GetProcAddress 表
- 着色器源第一字节必须是 ASCII `#version 300 es`（不要 UTF-8 BOM、不要先导空行/注释）
- 窗口：`0x0` + `SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP`，属性先设 ES 3.0，失败再 3.2
- CreateContext 已经 `eglMakeCurrent`。第二次 `MakeCurrent` 可能重建 XComponent 表面并解绑上下文（shader 编译时没有 GL）。仅当 `SDL_GL_GetCurrentContext()` 不是本 context 时才 MakeCurrent，且忽略返回值
- 不要在第一份 shader 编出来之前乱查 display mode / 反复 vsync 舞蹈

宿主侧 GLES 在 `main.cpp` 建；`GlesRenderBackend::create` 在 OHOS 上复用 `SDL_GL_GetCurrentContext()`，不要再 CreateContext（单表面）。

### 6.2 立刻退出 / 符号 / SDL_Init

- loader 要 `getLastRubyError` 与 `get_last_ruby_error` 都导出。
- VintagePomelo 已经 `SDL_Init`。OA 再 `SDL_Init(VIDEO|AUDIO|EVENTS)` 会在 AUDIO 上炸掉，并可能把还活着的 XComponent VIDEO 拆掉。OHOS：只在未 init 时补 VIDEO/EVENTS；AUDIO 用 `InitSubSystem`，失败打日志继续。
- `system.ini` 无 `[ANDROID]` 时扫 `[WINDOWS]`。

### 6.3 画面只有邮票大小

`SDL_CreateWindow(0,0)` 后 `SDL_GetWindowSize` 可能仍是 0 或 stage 大小，而 EGL drawable 已经是全屏。letterbox 必须用 **`SDL_GL_GetDrawableSize`**，不能用逻辑窗口尺寸。

iterate 里 drawable 或 `TAPIR_PORTRAIT_TOP_OFFSET` 变化要 `force_repaint`，否则静态 skip 会钉死第一帧小画面。

竖屏偏移与 KR2/RPGRunner 同一合同：`TAPIR_PORTRAIT_TOP_OFFSET` 百分比。GamePlay 调 `entry.setPortraitTopOffset`。`napi_init.cpp` 注释写“像素”是错的，实现是百分比。

### 6.4 虚拟鼠标 / 触摸板

`GamePlay.ets` 触摸板按钮版本列表必须含 **16 和 17**。速度用 `virtualMouseSensitivity`。注入走 `entry.sdlInputMouse`（与 KR2/Ren'Py 相同）。

OA 自己还有一套 Android 式 touch→mouse（`main.cpp`）。鸿蒙上宿主触摸板和引擎触控可能叠，真机要对点击命中做一次。

### 6.5 Windows 开始游戏 0xC0000374

jianyu 开始游戏 heap smash：Theora 紧缓冲。处理：Ogg/Theora 优先走 FFmpeg；PhysFS 递归 mutex；JPEG `longjmp` 安全；PNG 尺寸帽；FFmpeg 拆 `fmt->pb`。不要在没有这些保护的情况下“简化”解码路径。

### 6.6 30fps 音画双卡（已纠正）

错误组合：`SwapInterval(1)` + `pace_windowed_frame(60)` → 约 30fps，mix 推太慢，音频 underrun。  
错误纠正：`SwapInterval(0)` + 软件 60Hz（把 vsync 理解成 60）。  
当前：KR2 模型，见第 5 节。

---

## 7. 残留问题（接手优先序）

按建议顺序：

1. **真机可玩性（未完成）**  
   用真实 PFS（含分卷）走到标题、点开始、进第一段剧情。确认：Lua 不炸、存档可写（Harmony 默认 `<游戏目录>/savedata`）、点击命中 letterbox 后的舞台坐标、BGM/SE、影片、E-mote。未完成前不要写 play-verified。

2. **120Hz 脚本速率**  
   看 `onEnterFrame` / `em:progress` 是 elapsed 时间还是“每 vsync +1 帧”。若 2 倍速，优先改 Lua 桥换算，而不是再叠 60Hz Delay。

3. **触控 vs 触摸板**  
   原生触控合成 + ArkTS `sdlInputMouse` 是否双击、错位、被 letterbox/竖屏偏移打偏。对照 KR2 `krkrsdl_harmony.cpp` 手势状态机。OA 文档里的 Android 手势（按下不发、抬起先 motion；见 `docs/PLATFORMS.md`）在 OHOS 上是否仍适用未核实。

4. **前后台**  
   KR2 后台 `SDL_Delay(50)` 且不 render。OA 有 lifecycle watch 重置 tick 时钟。进后台是否仍 vsync 空转、回前台是否巨 delta，要测。

5. **Web 通道回归**  
   设置切 `web` 仍应走 `WebGamePlay` + `artemis_runtime`。不要为了修 native 误伤 WASM 部署。

6. **Windows 车道保持可跑**  
   Harmony 专用 `#ifdef` 不要破坏 `build-windows.ps1`。桌面仍用 interval-N + 60Hz remainder。

7. **HAP 收集**  
   改 CMake 输出名/SONAME 后确认三份 libs 都更新，且 `patch_elf_soname.py` 仍覆盖新增 `DT_NEEDED`。

8. **PLATFORMS.md 过时**  
   仍写 Android/wasm，未写 Harmony。行为以本文 + 源码为准。

---

## 8. 注意事项（后续 AI 容易踩）

- 对照实现是 **KR2 Harmony**，不是 OA 自己的 Android Gradle 壳，也不是桌面 60Hz cap。
- 不要用 `SDL_GL_GetProcAddress` 在 OHOS 上取 GLES3。
- 不要第二次 `SDL_GL_MakeCurrent`（除非 TLS 当前为空）。
- 不要全量 `SDL_Init`。
- 不要把 letterbox 基准当成 `SDL_GetWindowSize`。
- 不要假设 vsync=60，也不要用 interval N 在手机上“折成 60”除非实测脚本 2 倍速且换算不了。
- 不要跳过静态帧的 `SwapWindow`。
- 不要 `SDL_Quit`：`OA_HARMONY_LIB` 下宿主拥有进程 SDL（`SDL_AppQuit` 已跳过）。
- 链接预编译库用短名；编完看 `readelf -d libopenartemis.so` 的 NEEDED 是否还有 `.so.6` 之类。
- 改 ETS 必须 product sync。
- 验证 UI 要用真机/hdc，不是只看编译成功。
- 日志：`hdc hilog | findstr openartemis`（或 DevEco HiLog）。关键句：`[oa-kr2gl]`、`[gles] context`、`[gles] layout`、`[gles] vsync`、`first-frame`、`SDL_AppInit failed`。
- 工作区引擎构建习惯：日志放到 `workspace_temp/logs/`（deploy 脚本已这么做）。

---

## 9. 建议的接手第一步

1. 读 `oa_harmony.cpp`、`main.cpp` 里 `__OHOS__` 窗口/上下文、`backend_gles.cpp` 的 OHOS 分支、`krkrsdl_harmony.cpp` 主循环。
2. 增量编 `.so`，确认三处 libs 时间戳更新。
3. DevEco 签 HAP，装到无线 hdc 设备。
4. 开一盘带 `root.pfs` 的游戏，抓 hilog：vsync interval、drawable 布局、是否还 `render失败`。
5. 标题能停住之后再动 Lua/影片/E-mote，不要先重构调度。

Windows 上修核心（解码、Lua、渲染数学）用 `build-windows.ps1` + jianyu 更快；OHOS 专用坑（EGL 表面、GetProcAddress、SDL_Init）只能真机复现。
