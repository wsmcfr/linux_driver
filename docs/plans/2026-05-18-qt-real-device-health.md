# Qt Real Device Health Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make the Qt status bar and health panels show real 4G, camera, F4, and cloud/device health instead of fixed online text, without blocking the UI.

**Architecture:** Add one C++ `DeviceHealthController` exposed to QML. It performs short, asynchronous probes for 4G, KMS overlay camera frames, F4 UART handshake, and cloud health; QML only reads properties and never runs blocking commands. Health refresh must not call `waitForStarted()` in the UI thread; process startup errors are handled through Qt async error signals.

**Tech Stack:** Qt Quick/QML, Qt C++ `QTimer`, async `QProcess`, POSIX Unix socket, POSIX termios, existing `4g-ppp`, existing `uvc_kms_overlay` socket.

---

### Task 1: Static Contract

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`

**Steps:**
1. Add checks for overlay `STATUS` command.
2. Add checks for `DeviceHealthController` and async probe function names.
3. Add checks that QML uses `deviceHealth.*StatusText`.
4. Run the static check on a Linux shell and confirm it fails before implementation.

### Task 2: Overlay Camera Status

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/uvc_kms_overlay.c`

**Steps:**
1. Add a `STATUS` socket command.
2. Reply `OK STATUS has_frame=<0|1> serial=<n> visible=<0|1> width=<w> height=<h>`.
3. Keep the command non-blocking and only read cached `latest_frame`.

### Task 3: Qt Health Controller

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/main.cpp`

**Steps:**
1. Add `DeviceHealthController` QObject with status text/color properties.
2. Use async `QProcess` for `4g-ppp test`; kill after short timeout, and handle start failure with `errorOccurred` instead of `waitForStarted()`.
3. Use overlay `STATUS` socket query for camera; on repeated offline, attempt detached overlay restart script.
4. Use worker thread termios probe for F4: send `STATUS\r\n`, accept `ACK|OK|F4|READY`.
5. Expose the controller to QML as `deviceHealth`.

### Task 4: QML Binding

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/qml/Main.qml`

**Steps:**
1. Replace fixed top-bar network/F4/cloud values with `deviceHealth`.
2. Replace KMS camera fake-online logic with `deviceHealth.cameraStatusText`.
3. Update alarm/manual health panels to use the same source.

### Task 5: Documentation And Verification

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/README.md`
- Modify: `20_uvc_camera/README.md` if module-level usage changes.

**Steps:**
1. Document exact online criteria and non-blocking design.
2. Document board verification commands for 4G, camera hot-plug, F4 handshake, and cloud.
3. Run static contract checks and available shell syntax checks.
