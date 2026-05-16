# Qt Manual Control Screen Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 在 STM32MP157 Qt 工业检测界面中实现 `手动控制` 页面，让现场人员可以进行安全的调试级手动操作展示，并为后续 F4 串口实控接入预留清晰接口。

**Architecture:** 第一阶段只改 Qt Quick 页面和静态契约测试，不新增真实硬件控制路径。QML 负责页面入口、模拟状态、安全置灰、命令日志和保存当前帧复用；后续再新增 C++ `MotionController` 把这些 QML helper 连接到 MP157 与 STM32F4 的 UART 协议。

**Tech Stack:** Qt Quick/QML、Qt 5 C++ context property、现有 `storageController`、现有 KMS overlay 可见性控制、`20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh` 静态契约测试。

---

### Task 1: 静态契约测试

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`

**Step 1: 写失败检查**

在脚本中新增手动控制页标记检查：

```sh
require_grep "manualPageVisible" "qml/Main.qml"
require_grep "manualMode" "qml/Main.qml"
require_grep "manualCommandLog" "qml/Main.qml"
require_grep "handleManualAction" "qml/Main.qml"
require_grep "appendManualCommandLog" "qml/Main.qml"
require_grep "id: manualPage" "qml/Main.qml"
require_grep "id: manualBeltPanel" "qml/Main.qml"
require_grep "id: manualArmPanel" "qml/Main.qml"
require_grep "id: manualLightPanel" "qml/Main.qml"
require_grep "id: manualSafetyPanel" "qml/Main.qml"
require_grep "id: manualCommandLogView" "qml/Main.qml"
```

**Step 2: 运行红灯**

```bash
"C:/Program Files/Git/bin/bash.exe" 20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh
```

Expected: 实现前失败，至少提示找不到 `manualPageVisible` 或 `id: manualPage`。

**Step 3: Commit**

```bash
git add 20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh
git commit -m "test(qt): add manual control screen contract"
```

### Task 2: 页面状态和 helper

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/qml/Main.qml`

**Step 1: 新增页面状态**

在根对象属性区新增：

```qml
property bool manualPageVisible: activePage === "manual"
property bool manualMode: false
property string manualBeltState: "停止"
property string manualArmState: "待机"
property string manualLightState: "背光常亮"
property string manualLastAckText: "F4控制器待接入"
property bool manualEmergencyStop: false
property bool manualArmHomeOk: false
```

**Step 2: 新增命令日志模型**

在根对象内新增：

```qml
ListModel {
    id: manualCommandLog
}
```

**Step 3: 允许页面切换**

修改 `switchPage(pageName)` 的白名单，允许 `manual`：

```qml
if (pageName !== "home" && pageName !== "history" && pageName !== "stats" && pageName !== "manual") {
    return
}
```

Overlay 保持只在首页可见：

```qml
storageController.setOverlayVisible(pageName === "home")
```

**Step 4: 新增日志 helper**

新增：

```qml
function appendManualCommandLog(commandText, targetText, resultText) {
    manualCommandLog.insert(0, {
        "time": Qt.formatDateTime(new Date(), "hh:mm:ss"),
        "command": commandText,
        "target": targetText,
        "result": resultText
    })

    while (manualCommandLog.count > 8) {
        manualCommandLog.remove(manualCommandLog.count - 1)
    }
}
```

**Step 5: 新增动作 helper**

新增 `handleManualAction(action, label)`，第一版只做状态更新、保护判断和日志：

```qml
function handleManualAction(action, label) {
    if (action !== "enter-manual" && action !== "stop" && action !== "save-frame" && !manualMode) {
        manualLastAckText = "请先进入手动模式"
        appendManualCommandLog(label, "安全联锁", manualLastAckText)
        showStorageToast()
        return
    }

    if (manualEmergencyStop && action !== "stop" && action !== "refresh" && action !== "clear-alarm") {
        manualLastAckText = "急停中，禁止执行运动命令"
        appendManualCommandLog(label, "急停保护", manualLastAckText)
        showStorageToast()
        return
    }

    if (action === "save-frame") {
        handleStorageAction("save-image")
        appendManualCommandLog(label, "检测辅助", "已请求保存当前帧")
        return
    }

    /* 后续这里替换为 motionController.sendManualCommand(action)。 */
}
```

**Step 6: 运行静态搜索**

```bash
rg -n "manualPageVisible|manualMode|manualCommandLog|handleManualAction|appendManualCommandLog" 20_uvc_camera/qt_camera_display/qml/Main.qml
```

Expected: 列出新增状态和函数。

**Step 7: Commit**

```bash
git add 20_uvc_camera/qt_camera_display/qml/Main.qml
git commit -m "feat(qt): add manual control state helpers"
```

### Task 3: 手动控制页面布局

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/qml/Main.qml`

**Step 1: 新增 `manualPage`**

在 `historyPage` 和 `statsPage` 同级位置新增：

```qml
Rectangle {
    id: manualPage
    x: 176
    y: 72
    width: 832
    height: 512
    visible: root.manualPageVisible
    color: root.panelColor
    border.color: root.borderColor
    radius: 8
}
```

**Step 2: 新增顶部标题和返回按钮**

标题显示 `手动控制`、当前模式、最近 ACK；返回按钮调用：

```qml
root.switchPage("home")
```

**Step 3: 新增四个控制面板**

| 面板 ID | 内容 |
|---|---|
| `manualBeltPanel` | 手动模式切换、正向点动、反向点动、停止、速度档位 |
| `manualArmPanel` | 回零、待机位、抓取测试、放良品、放坏品 |
| `manualLightPanel` | 背光常亮状态、补光、亮度档位、保存当前帧 |
| `manualSafetyPanel` | F4 状态、急停、限位、回零状态、清故障、刷新状态 |

**Step 4: 新增命令日志**

新增 `ListView { id: manualCommandLogView ... }`，模型使用 `manualCommandLog`，最新命令在最上方。

**Step 5: 避免首页内容叠加**

把现有首页中间视频区、右侧结果区、底部控制区的 `visible` 条件从：

```qml
!root.historyPageVisible && !root.statsPageVisible
```

扩展为：

```qml
!root.historyPageVisible && !root.statsPageVisible && !root.manualPageVisible
```

**Step 6: 运行静态契约**

```bash
"C:/Program Files/Git/bin/bash.exe" 20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh
```

Expected: 输出 `PASS: Qt KMS overlay assets contract`。

**Step 7: Commit**

```bash
git add 20_uvc_camera/qt_camera_display/qml/Main.qml
git commit -m "feat(qt): add manual control screen layout"
```

### Task 4: 文档同步

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/README.md`
- Create: `docs/plans/2026-05-16-qt-manual-control-design.md`
- Create: `docs/plans/2026-05-16-qt-manual-control-implementation.md`

**Step 1: 更新模块 README 总览**

在 `20_uvc_camera/qt_camera_display/README.md` 记录：

| 位置 | 内容 |
|---|---|
| 模块文档总览 | 新增手动控制页路线，说明第一版是 UI 和模拟状态。 |
| 修改文件清单 | 记录 `Main.qml`、测试脚本、两个 plan 文档。 |
| 使用流程 | 增加“点击左侧手动控制进入页面”的动作。 |
| 验证矩阵 | 增加页面入口、安全置灰、命令日志、保存当前帧、overlay 隐藏验证。 |
| 修改记录 | 增加 2026-05-16 手动控制页设计和实施记录。 |

**Step 2: 更新设计文档**

确认 `docs/plans/2026-05-16-qt-manual-control-design.md` 已包含：

- 页面目标
- 安全边界
- 控件设计
- 数据与状态契约
- 第一版与后续边界
- 验收方式

**Step 3: 更新实施计划**

确认 `docs/plans/2026-05-16-qt-manual-control-implementation.md` 已包含：

- 计划头
- 具体文件
- 测试先行步骤
- QML 状态 helper
- 页面布局
- 文档同步
- 验证命令

**Step 4: Commit**

```bash
git add 20_uvc_camera/qt_camera_display/README.md docs/plans/2026-05-16-qt-manual-control-design.md docs/plans/2026-05-16-qt-manual-control-implementation.md
git commit -m "docs(qt): plan manual control screen"
```

### Task 5: 验证

**Files:**
- Verify only

**Step 1: 静态契约**

```bash
"C:/Program Files/Git/bin/bash.exe" 20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh
```

Expected: `PASS: Qt KMS overlay assets contract`。

**Step 2: QML 标记检查**

```bash
rg -n "manualPageVisible|manualMode|manualCommandLog|handleManualAction|appendManualCommandLog|id: manualPage|id: manualBeltPanel|id: manualArmPanel|id: manualLightPanel|id: manualSafetyPanel|id: manualCommandLogView" 20_uvc_camera/qt_camera_display/qml/Main.qml
```

Expected: 列出手动页状态、函数和页面面板 ID。

**Step 3: 文档标记检查**

```bash
rg -n "手动控制|manualPageVisible|CMD_MANUAL_AXIS|CMD_GO_HOME|急停|限位|MotionController" docs/plans/2026-05-16-qt-manual-control-design.md docs/plans/2026-05-16-qt-manual-control-implementation.md 20_uvc_camera/qt_camera_display/README.md
```

Expected: 列出设计文档、实施计划和模块 README 中的手动控制契约。

**Step 4: 板端人工验证**

部署后在开发板 LCD 上执行：

| 测试目标 | 动作 | 预期现象 |
|---|---|---|
| 页面入口 | 点击左侧 `手动控制` | 进入手动控制页，实时视频不覆盖页面。 |
| 安全置灰 | 未进入手动模式时点击运动按钮 | 不执行动作，提示先进入手动模式。 |
| 模式切换 | 点击进入手动模式 | 模式状态变为手动，日志新增一条命令。 |
| 传送带停止 | 点击停止 | 传送带状态显示停止，日志记录停止命令。 |
| 保存当前帧 | 点击保存当前帧 | 复用现有保存链路，生成 JPG/PNG 并提示结果。 |
| 返回首页 | 点击返回首页 | 首页实时视频恢复。 |

**Step 5: 最终状态检查**

```bash
git status --short
```

Expected: 没有未提交修改，或只剩用户明确保留的无关修改。
