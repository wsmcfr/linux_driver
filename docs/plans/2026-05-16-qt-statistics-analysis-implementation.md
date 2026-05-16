# Qt Statistics Analysis Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 在 STM32MP157 Qt 工业检测界面中实现 `统计分析` 页面，基于本地上传历史显示生产、上传和文件保存统计。

**Architecture:** QML 页面直接复用 C++ 暴露的 `uploadHistory` 模型，不新增数据库或云端实时查询。统计计算放在 `Main.qml` 的小型 helper 函数中，图表用 QML `Rectangle` 绘制，最近记录点击复用已有历史详情页。

**Tech Stack:** Qt Quick/QML、Qt 5 C++ `UploadHistoryModel`、本地 `/mnt/sdcard/images/upload_history.json`、现有 KMS overlay 可见性控制。

---

### Task 1: 静态契约测试

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`

**Step 1: 写失败检查**

新增这些统计页契约标记检查：

```sh
require_grep "statsPageVisible" "qml/Main.qml"
require_grep "statsSummary" "qml/Main.qml"
require_grep "statsRecentBars" "qml/Main.qml"
require_grep "statsDistributionBars" "qml/Main.qml"
require_grep "statsRecentRows" "qml/Main.qml"
require_grep "openHistoryDetailFromStats" "qml/Main.qml"
require_grep "id: statsPage" "qml/Main.qml"
```

**Step 2: 运行红灯**

```bash
"C:/Program Files/Git/bin/bash.exe" 20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh
```

预期：实现前失败在 `statsPageVisible` 或后续统计页标记。

### Task 2: QML 状态与数据 helper

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/qml/Main.qml`

**Step 1: 增加页面状态**

新增：

```qml
property bool statsPageVisible: activePage === "stats"
```

并允许 `switchPage("stats")`。

**Step 2: 增加统计 helper**

新增 `isUploadSuccess()`、`isGoodRecord()`、`percentText()`、`statsSummary()`、`statsRecentBars()`、`statsDistributionBars()`、`statsRecentRows()` 和 `openHistoryDetailFromStats()`。

**Step 3: Overlay 可见性**

保持 `storageController.setOverlayVisible(pageName === "home")`，使 `history` 和 `stats` 都隐藏实时视频 plane。

### Task 3: 统计分析页面布局

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/qml/Main.qml`

**Step 1: 新增 `statsPage`**

在历史页后、底部首页统计区前新增独立 `Rectangle { id: statsPage ... }`。

**Step 2: 新增五个区域**

| 区域 | QML ID |
|---|---|
| KPI 卡片 | `statsKpiGrid` |
| 最近保存趋势 | `statsTrendPanel` |
| 分布概览 | `statsDistributionPanel` |
| 最近记录 | `statsRecentPanel` |
| 云端与文件状态 | `statsCloudPanel` |

**Step 3: 最近记录跳转**

最近记录行 `MouseArea` 调用：

```qml
root.openHistoryDetailFromStats(modelData.index)
```

### Task 4: 文档和规范同步

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/README.md`
- Modify: `.trellis/spec/frontend/component-guidelines.md`
- Create: `docs/plans/2026-05-16-qt-statistics-analysis-design.md`
- Create: `docs/plans/2026-05-16-qt-statistics-analysis-implementation.md`

**Step 1: README**

记录统计分析路线、修改文件、使用方法、测试矩阵、数据路径验证和修改记录。

**Step 2: Code-spec**

在 frontend component guidelines 中新增 `Qt Statistics Analysis Screen Contract`，写清数据来源、导航、overlay、验证矩阵和测试要求。

### Task 5: 验证

**Files:**
- Verify only

**Step 1: 静态契约**

```bash
"C:/Program Files/Git/bin/bash.exe" 20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh
```

预期：`PASS: Qt KMS overlay assets contract`。

**Step 2: 标记检查**

```bash
rg -n "statsPageVisible|statsSummary|statsRecentBars|statsDistributionBars|statsRecentRows|openHistoryDetailFromStats|id: statsPage" 20_uvc_camera/qt_camera_display/qml/Main.qml
```

预期：列出统计页状态、函数和页面 ID。

**Step 3: 板端人工验证**

部署后在 LCD 上点击 `统计分析`，确认页面显示 KPI、趋势、分布、最近记录和云端状态；点击最近记录进入历史详情页；返回首页后视频恢复。

