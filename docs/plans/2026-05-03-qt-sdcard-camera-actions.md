# Qt SD Card Camera Actions Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add real Qt touch buttons for saving the currently displayed UVC frame to SD card and safely unmounting the SD card.

**Architecture:** Qt owns the UI and exposes two QML-callable C++ actions. The save action talks to the KMS overlay helper through a Unix domain socket, because the live video is drawn by the overlay plane rather than the Qt scene. The safe-remove action reuses the existing `sdcard-safe-remove` command.

**Tech Stack:** Qt Quick/QML, Qt C++ `QObject`, Unix domain sockets, V4L2/KMS overlay C helper, FAT SD card mount service.

---

### Task 1: Static Contract Test First

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`

**Step 1:** Add checks for `保存图片`, `安全卸载`, QML calls, C++ controller methods, overlay socket support, PPM image writing, and `fsync`.

**Step 2:** Run `sh 20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`.

**Expected:** It fails before implementation because the new contracts are missing.

### Task 2: Overlay Save Endpoint

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/uvc_kms_overlay.c`

**Step 1:** Add a default Unix socket path `/tmp/uvc-kms-overlay-control.sock` and command-line override.

**Step 2:** Keep the latest converted ARGB8888 frame in memory and service a non-blocking `SAVE <dir>` command.

**Step 3:** Save the frame as `uvc_YYYYMMDD_HHMMSS.ppm`, flush with `fsync`, and return `OK <path>` or `ERR <reason>`.

### Task 3: Qt Action Controller

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/main.cpp`

**Step 1:** Add a `CameraStorageController` with QML slots `saveCurrentFrameToSdCard()` and `safeRemoveSdCard()`.

**Step 2:** Validate `/mnt/sdcard` is mounted before saving and call `sdcard-safe-remove` for unmounting.

**Step 3:** Expose the controller to QML as `storageController`.

### Task 4: QML Buttons

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/qml/Main.qml`

**Step 1:** Add state text for storage operations.

**Step 2:** Add `保存图片` and `安全卸载` buttons to the right-side result panel for KMS overlay mode.

**Step 3:** Wire buttons to the C++ controller and keep them outside the video rectangle.

### Task 5: Documentation

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/README.md`

**Step 1:** Update changed file list, usage flow, verification matrix, read/write verification, and modification log.

**Step 2:** Document exact board commands for mount check, save verification, file size stability, and safe remove.

### Task 6: Verification

**Commands:**
- `sh 20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`
- `sh -n 20_uvc_camera/qt_camera_display/run_qt_kms_overlay_display.sh`
- Cross-build if the STM32MP157 SDK is available in the current environment.
