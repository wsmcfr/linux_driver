# Qt 自动停止与参数持久化修复 Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 修复 Qt 自动检测在 `CYCLE_DONE` 后的停止/续跑状态机，以及参数设置保存后重启不回显的问题。

**Architecture:** 在 QML 中把“自动循环会话”和“当前 cycle 运行状态”拆开，在 C++ 中让 `CYCLE_DONE` 正确释放旧 cycle 状态；参数持久化继续以 `DetectSettingsController` 为唯一真值源，并在保存后强制从 JSON 回读一次。

**Tech Stack:** Qt Quick/QML、Qt/C++、F4 二进制协议、JSON/QSaveFile。

---

### Task 1: 修正自动循环会话状态机

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/qml/Main.qml`
- Modify: `20_uvc_camera/qt_camera_display/main.cpp`

**Step 1: 写出失败场景约束**

- `停止` 终止整个自动循环，不是只停当前一步。
- `CYCLE_DONE` 后如果没停止，要自动进入下一轮 `START_CYCLE`。
- `CYCLE_DONE` 后如果点 `停止`，不能再拿旧 cycle 发 `STOP_CYCLE`。

**Step 2: 在 QML 中拆分会话状态**

- 增加“自动循环会话启用”和“当前 cycle 活跃”两个属性。
- 修改按钮使能条件和 `handleControlAction()` 的停止分支。

**Step 3: 在 C++ 中修正 cycle 完成后的本地 F4 状态**

- `handleF4FinalSortFinished()` 成功时清掉 `m_f4AutoRunning` / `m_f4AutoPaused`。
- 保留最近 `cycle_id` 用于下一轮递增。

**Step 4: 在 QML 中接入自动续跑**

- `onF4FinalSortFinished(ok=true)` 成功后：
  - 当前 cycle 置空；
  - 会话仍启用则自动再次 `start`；
  - 会话已停止则停在“停止”状态。

**Step 5: 手动检查关键分支**

- `start -> cycle done -> auto restart`
- `start -> cycle done -> user stop before next cycle`
- `start -> running -> stop`
- `start -> pause -> stop`

### Task 2: 修正参数保存后重启不回显

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/qml/Main.qml`
- Modify: `20_uvc_camera/qt_camera_display/main.cpp`

**Step 1: 确认 QML 只读真实配置**

- 保持参数显示直接依赖 `detectSettings` 属性。
- 不新增第二份长期缓存配置对象。

**Step 2: 在保存路径增加 JSON 回读闭环**

- `settingsApplyAction("save")` 在 `saveSettingsToDisk()` 成功后调用 `loadSettingsFromDisk()`。
- 如果回读失败，提示明确错误，不让界面假装已经永久保存。

**Step 3: 刷新参数页展示**

- 依赖 `detectSettings.settingsChanged` 更新摘要、步进参数版本号和状态文本。
- 避免保存后仍显示旧文本或默认文本。

**Step 4: 补状态提示**

- 保存成功：提示“已写入 JSON 并重新加载”。
- 保存失败：提示真实失败原因，例如 SD 卡未挂载、JSON 无法写入。

### Task 3: 同步静态验证与模块文档

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`
- Modify: `20_uvc_camera/qt_camera_display/README.md`

**Step 1: 补静态 marker**

- 检查新的自动循环会话属性、`CYCLE_DONE` 后自动续跑入口、停止分支 marker。
- 检查保存后回读 marker。

**Step 2: 更新模块文档**

- 写清停止键语义。
- 写清 `CYCLE_DONE` 后下一轮自动启动。
- 写清参数保存后的 JSON 回读和重启回显验证步骤。

### Task 4: 运行验证

**Files:**
- Test: `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`

**Step 1: 运行静态测试**

Run:

```bash
"C:/Program Files/Git/bin/bash.exe" -lc 'cd /c/Users/caofengrui/Desktop/linux/20_uvc_camera/qt_camera_display && ./test_qt_kms_overlay_assets.sh'
```

Expected: `PASS: Qt KMS overlay assets contract`

**Step 2: 运行差异格式检查**

Run:

```powershell
git diff --check
```

Expected: 无 trailing whitespace、无冲突标记。

**Step 3: 给出板端人工验证清单**

- 单件完成后是否自动进入下一轮。
- 单件完成后按 `停止` 是否直接结束自动循环。
- 修改参数并保存后，重启 Qt/开发板是否仍显示保存值。
