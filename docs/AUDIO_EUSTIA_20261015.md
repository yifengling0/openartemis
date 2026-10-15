# 音频开销与《脏翅膀》覆盖存档修复（2026-10-15）

基线为 `9759d2ded6bf0b82146930c2e5de59281aa44c7f`。保留同日 e-mote 改动；本轮修复音频混音/预解码开销和旧版保存 UI 的输入派发，在 WSL 编译、自测。尚未做 HarmonyOS 真机验收。

## 实际资源与复现

游戏：`D:\Games\Pomelo\Ar_枫笛_脏翅膀_od`。读取 `root.pfs`（38807 项）与 `.000`（5647 项），核对相关 Lua、IET、ASB 与现有解包文件一致，没有修改游戏包或用户存档。

真实 `system/ui/save.lua` 中，空槽走 `save_reserve → save_check`，已有槽的透明缩略图则接收 drag/dragout。`save_thumb_drag` 先写 `sys.save.btn` 和 `adv.fakeclick`，`save_thumb_dragout` 依赖它们；vsync 随后通过 `overrideKey{key=146,status=32}` 合成短按。`save_check` 检查文件，选择 `save2`，调用 `system/system.iet` 的 `adv_save → yesno`；该对话框使用原包的 `dialog_03`。

修改前，静止按下/松开没有初始 drag 样本，`save_thumb_dragout` 在第 696 行拼接未初始化的 `sys.save.btn` 时出错；即使初始化了，合成 key146 原先也没有到达 setonpush。

## 修改

- 按下可拖动层时发送一次初始 drag 样本；同帧的指针移动不再重复发送。保持后续拖动、夹取范围与松手行为。
- 新出现的 override status32 按键进入 push 派发；物理键和合成键同帧同 key 去重。
- queue 来源的点击等待只阻止其延迟续接，允许新输入产生的即时 `e:tag` 前缀执行。此前保存后回正文再按 F2，菜单命令会等到下一次点击才执行。限定 drain 保留延迟续接和原等待来源标记，沿用 call barrier。
- 音频由每输出采样准备一次数据，改为每声道、每块准备完整重采样窗口（含最后的插值采样）；保留数据与输出缓冲，环形缓冲最多两段批量复制。
- worker 和同步补给在锁下共享同一个 Vorbis 解码器。避免独立重开源文件，以及补给时从头重解历史；保留 A/B 循环、声道映射和自然结束逻辑。
- native 音频 worker 停止时 join；GameRuntime 在其 loader 依赖析构前停止媒体并结束线程池。
- SDL2 的断流计数改为实际短读，避免把“刚好读完队列”误报为断流；跨线程诊断字段改为 atomic。SDL3 仍使用剩余队列估计。
- 音频诊断探针增加 `OA_AQ_WORKERS` 与源文件读取次数；SDL2 shim 补充测试所需的 `SDL_HINT_AUDIO_DRIVER` 别名。

试验过的 inline-event marker 栈修补，经正常 F2 输入路径对照确认不需要，最终已移除。

## WSL 验证

Ubuntu 22.04，现有 `scripts/engine-linux.py` 的 native_env 和 ext4 增量构建缓存，`RelWithDebInfo`、SDL2。Windows、WSL 与构建 mirror 独立，改动显式同步。音频设备用 dummy；WSLg 的 PulseAudio 连接不可用，不能用本轮结果证明扬声器听感。

`eustia_save_ui` 挂载原 PFS，正常启动到允许菜单的正文，使用游戏 F2 绑定打开保存 UI，再投递实际坐标的按下/松手。所有写入都在唯一临时 SaveStore 中；只从真实 PNG 头读取命中尺寸，不替换游戏按钮脚本。验证：

- 空槽确认创建一个编号存档。
- 静止点击已有缩略图，原始 `save2` 对话框的 YES/NO 图层存在且可见。
- 弹提示尚未写入；取消后原文件字节与写入次数不变。
- 取消后再次打开，确认只覆盖相同编号一次，并关闭对话框。
- 保存后可重新打开菜单；槽 1 拖到槽 2 后保留其文件关联，且不误触发覆盖确认。

仅在构建 mirror 撤掉初始 drag 和虚拟 push 修复，同一测试再次出现 `save_thumb_dragout:696 ... field 'btn' (a nil value)`，覆盖断言失败。随后恢复最终源码重建。

`audio_window` 独立解码 fixture，再自行索引、插值，与实际混音录制比较。覆盖单声道 32k、立体声 48k、22.05k A/B 循环、非循环 EOF，同步/worker 两路和 16/33/700/1/400ms 不规则块。最终 811440 个采样最大误差为 **0**，每个源只读取一次。旧版音频在相同测试中 PCM 误差也为 0，但有 5 条重复打开源文件的断言失败；本轮解决的是开销与预解码失效。

真实音频为 `bgm/bgm_0301_b.ogg`，44100Hz、双声道、120.231701 秒，1906725 字节；解包数据与 `root.pfs` 一致。SHA256：`2c3bf6b9ddf627b6744c5dc3c4caf878cdc54f31e2cb6584a8eefc69aef25513`。

同一编译配置，仅替换 mirror 的音频实现为 HEAD 基线，分别处理约 60 秒虚拟时钟，三次重复，包含 PCM 录制开销：

| 模式 | 修改前耗时 | 修改后耗时 | 源文件读取次数 |
| --- | --- | --- | --- |
| 同步解码 | 0.13–0.14s | 0.10s | 1 → 1 |
| 开启 worker | 0.17s | 0.14s | 2 → 1 |

所有 12 份混音 PCM 的 SHA256 相同：`46bbf1f5c63317afa7366a18af1578090069c0583b297e3f17244586cde33c3d`。这只量化该声道的解码/混音与录制，不代表游戏总帧率，也不是严格统计基准。

真实 BGM + dummy 设备 + worker 按实时时钟运行 7.98 秒：345 次设备回调，实际短读计数 0，源读取 1 次。已有 `audio_latency_bound` 也通过：人为 400ms 阻塞后的积压回到约 62ms；关闭 bound 的负对照仍保持约 400ms。该测试的人为阻塞会产生断流，不能把它解释为正常播放故障。

最终相关回归 **16 项全部通过，无跳过**：decode pool、音频窗口、interpreter、tag queue barrier、Lua bridge、输入派发、媒体状态/运行时、音频延迟、保存域/兼容性、P1c2 控制、e-mote semantics/GPU、真实《脏翅膀》保存 UI、TG3 实际脚本。没有执行全仓库测试。

另以最终 `openartemis` 主程序启动该游戏，WSLg/D3D12 Intel UHD 770、GLES、1280×720、180 帧，退出码 0，生成标题画面；单独隔离保存目录。该启动运行记录 **1 次音频短读**，尚未定位其时点，故不能把独立 BGM probe 的零短读推广到整个游戏。

复测命令（现有 Linux 依赖配置下）：

```sh
cmake -S <source> -B <build> -DOA_ENABLE_TESTS=ON \
  -DOA_TEST_EUSTIA_PFS='/mnt/d/Games/Pomelo/Ar_枫笛_脏翅膀_od/root.pfs'
cmake --build <build> --target eustia_save_ui_test audio_window_test audio_latency_bound_test
SDL_AUDIODRIVER=dummy ctest --test-dir <build> \
  -R '^(eustia_save_ui|audio_window|audio_latency_bound)$' --output-on-failure
```

详细构建、回归、对照日志：WSL `/home/liufeng/vp/vp-main/workspace_temp/build_cache/artemis-audio-save-20261015/`。

## 待真机确认

混音供给仍由主帧循环驱动，画面或 I/O 长时间阻塞仍可能耗空设备队列。本轮没有将混音改为独立实时线程，不能宣称所有卡顿已消除。保存测试验证脚本、输入、对话框图层和持久化结果；HarmonyOS 的触摸、渲染与音频设备路径需真机确认。TG3 模型资源仍缺，e-mote 现有验证边界见 `EMOTE_TG3_20261015.md`。
