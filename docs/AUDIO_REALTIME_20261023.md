# Artemis 独立音频供给（2026-10-23）

基线：`3fef90773f90180383d66c8e4e12348f648f1f70`。针对部分设备音频卡顿，改造 native 主程序的音频供给路径。本轮没有修改应用的 MIDI 缓冲区设置、共享 SDL 鸿蒙驱动或旧发布标签，也没有做 HarmonyOS 真机听感验收。

## 原因与改动

原路径在主帧末混音；脚本、场景更新、渲染或文件读取阻塞时，音频也停止供给。新声道还需要同步读取压缩文件、初始化 Vorbis；旧预解码与同步补给共享解码锁，替换声道会等待其 worker 结束。SDL2 音频回调也会与提交线程争用锁。

现在 native 主程序在设备初始化后启用以下路径：

1. 主线程解析 magic path，复制播放状态，接收完成事件。后台只接触解析后的路径与状态副本，不调用 Lua 或解释器。
2. 一个加载线程读取音频并初始化 Vorbis；A/B 循环的 B 段在播放前读取。新语音的加载不阻塞已有 BGM 的解码和混音。
3. 两个解码线程按任务轮转，为每个声道准备 44100 Hz、双声道、无增益的 PCM。每声道预解码环为 8192 帧，解码块为 1024 帧。同一解码器始终只有一个任务使用。
4. 一个混音线程按设备队列水位补给，每块最多 256 帧，应用音量、pan 与 fade。队列目标为两倍设备周期，至少 1024 帧；不再依赖主帧及时结束。
5. SDL2 转换留在提交端，输出设备格式 PCM 写入预分配环。设备回调只读取和混合，不再拿提交端的锁、执行 SDL 转换或扩容 scratch。

线程数固定，不随声道数增长。停止或替换声道只标记取消；共享任务持有其生命周期，主线程不再逐声道 join。所有生产线程在关闭设备和 loader 依赖前退出。混音线程申请较高调度优先级，失败时继续运行；使用标准 C++ 线程调用 SDL 后显式清理其 TLS。

主线程完成事件同时检查播放序号和 reset epoch。相同 id、相同文件、同一时钟的重播也不会被旧 EOF 误停。路径解析缓存按活动声道回收，播放中的路径映射保持稳定。SDL2 后台事件和设备暂停会暂停独立混音，暂停期间不推进其 fade 时钟。

SPSC 环的 flush 只发布丢弃水位；生产者不会改消费者游标并提前复用回调正在读取的内存。Movie 仍有独立音频来源，保留原有延迟上限和停止 flush。

原来的确定性、调用方时钟驱动路径继续供无设备运行、离线 PCM 回归和 Emscripten 使用。新的后台解码复用原采样窗口、声道映射、循环和插值算法。

## WSL 验证

Ubuntu 22.04，沿用 `scripts/engine-linux.py` 的 `native_env()` 与 ext4 构建 mirror，RelWithDebInfo、SDL2。音频使用 dummy 设备。Windows、WSL 检出和构建 mirror 显式同步；游戏保存目录独立隔离。

`audio_realtime` 的行为测试覆盖：

- 主线程停止更新 500ms，新路径仍生成 22528 帧，没有增加设备短读。
- 新语音 loader 人为阻塞 350ms，主线程提交耗时约十几至几十微秒，已有 BGM 没有设备短读或源 PCM 断供。
- 同样停止更新的旧逐帧供给负对照出现 8–9 次设备短读，证明测试能识别原架构的断供。
- resolver 与完成回调只在主线程执行，文件读取在后台执行。
- 暂停/恢复、自然结束、同一时钟重播、fade-out stop 和 reset。
- SPSC 环并发传递 50 万递增整数，包含 wrap 和 flush，检查损坏、重复和回退。
- 单/双声道、32k/48k、A/B 循环及重采样，与独立 Vorbis 解码并自行索引插值的结果比较，四组最大误差均为 0。

原 `audio_window` 的 811440 个采样最大误差仍为 0；`audio_latency_bound` 的原恢复、Movie 上限及负对照均通过。

相关回归 17 项全部通过、无跳过：`decode_pool_test`、`audio_window`、`audio_realtime`、`interpreter_test`、`tag_queue_barrier_test`、`lua_bridge_test`、`input_dispatch_test`、`media_state_test`、`media_runtime_test`、`audio_latency_bound`、`save_domain_test`、`p1c2_control_test`、`save_compat_test`、`emote_semantics`、`emote_gpu`、`eustia_save_ui`、`tg3_emote_script`。包括原包覆盖存档流程与 TG3 实际 Lua 框架，未执行全仓库测试。

AddressSanitizer 单独构建并运行新音频测试，开启 `detect_leaks=1`，通过。发现并修复了标准线程调用 SDL 后遗留的 TLS；独立测试进程通过 `SDL_SHUTDOWN_DBUS_ON_QUIT=1` 释放 SDL 动态加载的 DBus 全局缓存，没有关闭泄漏检测或添加 suppression。ThreadSanitizer 的空程序预检即报 `unexpected memory mapping`，没有获得 TSAN 并发检测结论。

## 实际游戏启动

最终 native 主程序挂载 `/mnt/d/Games/Pomelo/Ar_枫笛_脏翅膀_od/root.pfs`，WSLg/D3D12 Intel UHD 770、GLES、1280×720，运行 300 帧并正常退出。日志确认启用新路径，进入标题画面：

- 混音输出 153600 帧，音源断供计数 0。
- 设备回调 204 次，mix 和 Movie 短读计数均为 0，无队列 resync。
- 最大混音队列 2048 帧（约 46.4ms），最大输出块 256 帧（约 5.8ms）。
- 音频加载最大耗时约 8.7ms。
- `OA_AUDIO_REC` 的 mix 和 device WAV 均有有效数据，退出时文件头正常收尾。

这只是短时启动检查。设备 WAV 包括启动尚无音频时的静音，不能将其总帧数与 mix 帧数的差解释为断流，也不能将 dummy 设备的计数推广为 HarmonyOS 扬声器听感。

## 后续诊断与边界

`OA_AUDIO_DIAG=1` 的心跳与退出日志增加 `source_short`、`load_max_us` 和活动声道数。`source_short` 统计已经开始播放、尚未 EOF 的声道缺少 PCM 的帧数；`under_mix` 统计混音流活动期间设备回调的短读。两者可区分解码供给不足与设备队列供给不足。初次加载等待和正常停止后的静音不算断供。

`OA_AUDIO_REC=<prefix>` 同时记录新 producer 的 mix 和设备端数据，tick CSV 在独立混音路径中每输出块一行，`delta_ms=0`。录制涉及同步文件 I/O，仅用于诊断；SDL2 的 device WAV 捕获仍限于 F32 输出。

`OA_AUDIO_FRAME_FEED=1` 可在启动时保留旧逐帧供给，用于同设备 A/B 对照。它是诊断开关，不是用户设置。

共享 SDL 鸿蒙后端 `thirdparty/sdl/src/audio/ohos/SDL_ohosaudiomanager.c` 仍存在单中转缓冲和 `OHOSAUDIO_AudioRenderer_OnWriteData` 中的条件变量握手。本轮没有修改此共享驱动。因此系统音频回调的调度、共享桥接的等待、路由切换等仍需在发生问题的设备上结合日志验证。

TG3 的缺失分卷和模型资源情况未变；本轮没有新增完整 e-mote 模型的验证结论。旧覆盖存档回归通过也不能替代真机触摸与对话框显示验收。

详细构建、回归、内存检查、原包启动日志和 WAV：WSL `/home/liufeng/vp/vp-main/workspace_temp/build_cache/artemis-realtime-20261023/`。
