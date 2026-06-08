# Qt Settings Detail Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 让参数设置页的“视觉检测策略”和“F4接入边界”支持点击查看完整说明，并从“相机、光源与存储”卡片移除当前没有计划使用的补光展示项。

**Architecture:** 复用告警页和历史页已经验证过的遮罩弹层模式，在 `Main.qml` 根对象中保存当前详情标题与正文，由两个详情函数生成云端契约相关说明。静态测试继续用 `test_qt_kms_overlay_assets.sh` 检查关键 UI marker 和防误导约束。

**Tech Stack:** Qt Quick/QML、shell 静态契约测试、Markdown 模块文档。

---

### Task 1: 静态测试先行

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`

**Step 1:** 增加对 `settingsDetailVisible`、`openSettingsDetail`、`settingsVisionDetailText`、`settingsF4DetailText`、`settingsDetailOverlay`、`settingsDetailFlickable` 和 `查看详情` 的检查。

**Step 2:** 增加详情正文关键契约检查：`record_no`、`source/annotated`、`断网补传`、`/dev/ttySTM1`、`LDC1614`、`HX711`、`Emm42_V5.0`。

**Step 3:** 增加局部块检查，确认“补光”不再出现在 `settingsStoragePanel`，并确认视觉详情入口位于 `settingsVisionPanel` 而不是误插到告警卡片。

**Step 4:** 运行：

```bash
"C:/Program Files/Git/bin/bash.exe" ./test_qt_kms_overlay_assets.sh
```

旧界面预期失败，新实现预期输出 `PASS: Qt KMS overlay assets contract`。

### Task 2: QML 参数页实现

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/qml/Main.qml`

**Step 1:** 增加 `settingsDetailVisible`、`settingsDetailTitle`、`settingsDetailText` 三个根属性。

**Step 2:** 新增 `settingsVisionDetailText()`，说明 MP157 视觉链路、云端记录契约、`record_no`、`source/annotated`、低置信度复核和断网补传边界。

**Step 3:** 新增 `settingsF4DetailText()`，说明 `/dev/ttySTM1`、`115200`、F4 心跳/CRC/帧序号、LDC1614、HX711、Emm42_V5.0、急停限位和 Qt 不接管运动控制的边界。

**Step 4:** 新增 `openSettingsDetail(detailKey)`，按 `vision` 或 `f4` 写入标题、正文并打开浮层。

**Step 5:** 在 `settingsVisionPanel` 和 `settingsMotionPanel` 内增加 `查看详情` 按钮。

**Step 6:** 从 `settingsStoragePanel` 的 Repeater 模型删除 `补光` 行。

**Step 7:** 新增 `settingsDetailOverlay` 可滚动弹层，点击遮罩或关闭按钮关闭。

### Task 3: 文档同步

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/README.md`

**Step 1:** 更新参数设置路线，说明视觉检测策略和 F4 接入边界已支持详情弹层。

**Step 2:** 增加 2026-05-21 经验记录，说明补光项移除、详情内容来源和验证方式。

**Step 3:** 在修改文件清单中同步 `Main.qml`、测试脚本和计划文档的变化原因。

### Task 4: 验证

**Files:**
- Test: `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`

**Step 1:** 运行静态契约测试。

**Step 2:** 后续部署到板端后，打开 `参数设置`，分别点击 `视觉检测策略 -> 查看详情` 和 `F4接入边界 -> 查看详情`，确认详情可滚动阅读。

**Step 3:** 确认 `相机、光源与存储` 卡片不再显示 `补光` 行。
