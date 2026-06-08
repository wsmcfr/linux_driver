# Qt Alarm Advice Detail Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a scrollable full-detail view for alarm troubleshooting advice and replace the alarm health "配置" cell with the real 4G online status.

**Architecture:** Keep the compact alarm maintenance panel as a short summary, then open a modal overlay with a bounded `Flickable` for complete troubleshooting text. Reuse existing `DeviceHealthController::networkStatusText` and `networkStatusColor` for the 4G health cell, so no new C++ probe is needed.

**Tech Stack:** Qt Quick QML, shell static contract test, Markdown module documentation.

---

### Task 1: Static Contract Test

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`

**Steps:**
1. Add failing `require_grep` checks for `alarmAdviceDetailVisible`, `alarmFullAdviceText`, `alarmAdviceDetailOverlay`, `alarmAdviceDetailFlickable`, `查看全部`, and the `4G` health cell bound to `deviceHealth.networkStatusText`.
2. Run `sh ./test_qt_kms_overlay_assets.sh`.
3. Expected red result: the script fails because the new markers do not exist yet.

### Task 2: QML Implementation

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/qml/Main.qml`

**Steps:**
1. Add `alarmAdviceDetailVisible` state.
2. Add `alarmFullAdviceText()` to assemble full advice from existing `alarmSourceAdvice()`.
3. Replace the health cell named `配置` with `4G` and bind it to `deviceHealth.networkStatusText`.
4. Add a `查看全部` button in `alarmAdvicePanel`.
5. Add `alarmAdviceDetailOverlay` with `alarmAdviceDetailFlickable`.

### Task 3: Documentation

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/README.md`

**Steps:**
1. Update the alarm maintenance route description.
2. Add validation rows for the full advice overlay and 4G health cell.
3. Keep commands copy-pasteable for board-side checks.

### Task 4: Verification

**Commands:**
```sh
cd 20_uvc_camera/qt_camera_display
sh ./test_qt_kms_overlay_assets.sh
```

**Expected green result:** `PASS: Qt KMS overlay assets contract`.
