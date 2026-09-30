# Compatibility Lab（openartemis）

Artemis → HarmonyOS 原生运行时的自动兼容性测试与回归系统。
设计任务书：`C:\Users\liufeng\Downloads\Artemis_OHOS_AI_Compatibility_Lab_Codex_Plan_2026-09-24.md`
（融入现有工程结构，未机械复制其目录）。

## 一条命令的闭环

```powershell
# 1. 构建
powershell -File build-windows.ps1
# 2. 回归（语料批量 headless/窗口运行 + Bug Museum 断言 + 基线差分）
python tools/compatlab/regression.py --exe build_sdl2/src/app/openartemis.exe
#    首次或确认修复后存基线： --update-baseline
# 3. 新游戏入库：先静态扫描，再手工在 corpus.json 加场景与断言
python tools/compatlab/scan_game.py D:/Games/Pomelo/<game> --out compatlab/game_profiles/<id>.json
```

## 目录

| 路径 | 内容 |
|---|---|
| `corpus.json` | 本地语料 + 每游戏的驱动场景（click_ids/frames/env）+ Bug Museum 断言 |
| `baselines.json` | 每游戏的状态摘要基线（headless end 行 13 字段）；DRIFT 自动可见 |
| `bugs/README.md` | Bug Museum：历史修复 ↔ 回归断言的对照（症状/根因/修复/回归） |
| `game_profiles/*.json` | 静态扫描特征清单（ASB 命令/IET 标签/Lua 面/未知系统变量/资产计数） |
| `reports/run_*.json` | 每轮回归的完整结果（日志在 reports/logs_<runid>/） |
| `COMPATLAB_BASELINE.md` | M0 仓库审计（现状 + 缺口） |
| `COMPATLAB_PROGRESS.md` | 里程碑进度（What changed / Evidence / Next） |

## 原则（任务书 §0.1 摘要）

先证据后改码；每个修复必须留一个永久回归断言；禁止游戏名特判；
基建（Trace/TestBridge/确定性）未稳前不做大重构；商业资产只以计数/哈希出现，不入库。
