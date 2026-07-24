# User-Space Command Guidelines

> This replaces web component guidance. The user-facing components here are small C command-line test programs.

---

## Program Structure

Use the existing single-file shape:

1. Include POSIX headers needed for the syscall surface.
2. Define command constants or ioctl ABI values near the top.
3. Add a small `show_usage()` helper when commands have more than one mode.
4. Validate `argc` before opening device files.
5. Open the device node from `argv[1]`.
6. Parse command arguments.
7. Call `read`, `write`, or `ioctl`.
8. Close the file descriptor on every path after `open`.

Examples:

- `05_dtsled/ledAPP.c`: minimal write-based LED control.
- `09_timer/timerapp.c`: multi-command ioctl app with usage output.
- `13_inputkey/inputkeyapp.c`: input-event oriented test program.

---

## Command Interface Conventions

- First argument should be the device node path.
- Use readable command words for multi-mode tools: `on`, `off`, `blink`, `led`.
- Validate numeric arguments after `atoi()` and reject non-positive timing values.
- Keep user-space ioctl macros synchronized with the kernel driver.

Example from `09_timer/timerapp.c`:

```c
#define TIMERLED_CMD_BLINK _IOW(TIMERLED_IOC_MAGIC, 2, int)
```

The same command definition exists in `09_timer/timer.c`; update both together.

---

## Output And Error Handling

- Print usage for invalid command shapes.
- Use `perror()` for syscall failures where `errno` is meaningful.
- Return non-zero on failure.
- Close file descriptors before returning after a failed operation.

---

## Linux UART/RS485 User-Space Test Tools

Use this convention when writing or debugging Linux user-space UART or RS485 test programs for STM32MP157-to-MCU links, such as `MP157 <-> STM32F4` communication.

### Scope / Trigger

- Trigger: adding a user-space serial test app, validating UART/RS485 wiring, replacing `minicom` with a deterministic CLI test, or debugging bidirectional data between STM32MP157 and an external MCU.
- Hardware shape: STM32MP157 Linux opens `/dev/ttySTM*`, `/dev/ttyUSB*`, or another TTY node; the peer MCU sends and receives bytes through UART/RS485.
- RS485 adapter shape: if the module provides automatic direction control, treat it as a normal UART in user space; only add GPIO-based DE/RE control when the hardware exposes manual direction pins.

### Signatures

| Operation | Signature |
|---|---|
| Receive-only test | `./uartapp /dev/ttySTM2 115200 recv` |
| Send-once test | `./uartapp /dev/ttySTM2 115200 send "hello\r\n"` |
| Loop-send test | `./uartapp /dev/ttySTM2 115200 loop "ping\r\n" 1000` |
| Terminal passthrough | `./uartapp /dev/ttySTM2 115200 term` |
| Cross-build with ST SDK | `source /opt/st/stm32mp1/3.1-snapshot/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi && make APP_CC="$CC" app` |
| NFS rootfs deploy | `install -m 0755 uartapp /home/cfr/linux/nfs/rootfs/root/uartapp` |

### Contracts

| Area | Contract |
|---|---|
| TTY configuration | Use `termios` raw mode, set the requested baud rate, use 8 data bits, no parity, 1 stop bit, and disable software and hardware flow control unless the wiring explicitly includes flow-control lines. |
| Argument validation | Validate command shape, baud rate, interval values, and escaped send text before opening the TTY device. |
| Read loop | Use `select()` or another explicit wait mechanism before `read()` so receive mode can exit cleanly and does not busy-loop. |
| Write path | Loop on partial `write()` results and call `tcdrain()` after sending so the program only reports success after the kernel output queue is flushed. |
| Display format | Print both text and hex forms for received data; invisible bytes such as `\r`, `\n`, `0x00`, and protocol markers must be diagnosable without relying on terminal rendering. |
| Newline handling | Treat `\r\n` as an explicit payload choice. Do not infer link failure from a terminal cursor staying at the last column when the sender repeatedly transmits text without newline. |
| Minicom comparison | `minicom` is useful for manual observation, but its display settings such as local echo, line wrap, and sender newline behavior can make good data look stuck. Confirm suspected failures with a raw serial test app or hex dump. |

### Validation & Error Matrix

| Check | Good Result | Failure Meaning |
|---|---|---|
| `-h` command | Prints usage and exits `0` | CLI is not self-documenting |
| Missing arguments | Prints usage and exits non-zero | Invalid command shapes may silently run |
| Unsupported baud | Fails before opening the device | Bad serial settings may be tested accidentally |
| PTY send test | Peer side receives the exact bytes, including `\r\n` | Escape parsing or write flushing is wrong |
| PTY receive test | App prints expected text and hex bytes | Receive loop or display path is wrong |
| Target binary check | `file uartapp` reports ARM 32-bit EABI for STM32MP157 | Built for the host instead of the board |
| Board smoke test | `/root/uartapp -h` runs on the board | Deployment path or runtime linker is wrong |
| Minicom mismatch | `uartapp recv` shows increasing RX hex data while minicom display appears stuck | Treat as terminal rendering/newline/wrap issue, not a serial-link failure |

### Good / Base / Bad Cases

```bash
# Good: send an explicit line ending so both minicom and raw tools show separate messages.
./uartapp /dev/ttySTM2 115200 send "F4_TEST\r\n"
```

```bash
# Base: receive raw peer data and inspect the HEX column before judging the link.
./uartapp /dev/ttySTM2 115200 recv
```

```bash
# Bad: repeatedly send text without newline, then conclude the UART is broken
# only because minicom's cursor stays at the last visible column.
./uartapp /dev/ttySTM2 115200 loop "adwdaw" 200
```

### Tests Required

- Run the serial app's local regression test with a PTY or equivalent pseudo-terminal before deploying.
- Cross-compile with the STM32MP157 SDK and verify the binary with `file`.
- Deploy to the NFS rootfs or board path and run the app's help command on the board.
- For real hardware, test all four directions/modes in this order: MP157 receive-only, MP157 send-once, MP157 loop-send, and terminal passthrough.
- When comparing against `minicom`, send at least one payload with explicit `\r\n` and one payload without newline, then confirm whether the symptom is only a display/wrap behavior.

### Wrong vs Correct

#### Wrong

```text
minicom shows the cursor flickering at the last character, so the RS485 link must have stopped receiving.
```

#### Correct

```text
First check whether the sender is omitting \r\n or minicom line wrap/local echo settings are hiding updates.
Then confirm with a raw app that prints RX byte count and HEX bytes.
If HEX keeps changing, the physical UART/RS485 link is still receiving data.
```

---

## Qt/Wayland Cross-Compile Convention

Use this convention when writing, modifying, configuring, or compiling Qt applications that must run on the STM32MP157 board through the ST OpenSTLinux Weston Qt Wayland SDK.

### Scope / Trigger

- Trigger: any user-space Qt or Qt/Wayland program intended to run on STM32MP157.
- Host: Linux virtual machine user `cfr`.
- SDK: ST OpenSTLinux Weston Qt Wayland SDK `3.1-snapshot`.
- Target: `cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi`.

### Signature

```bash
source /opt/st/stm32mp1/3.1-snapshot/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi
```

Equivalent POSIX form:

```bash
. /opt/st/stm32mp1/3.1-snapshot/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi
```

### Contract

- Run the environment setup command once in every new shell session before invoking `qmake`, `cmake`, `make`, `pkg-config`, or compiler commands for MP157 Qt programs.
- Treat the sourced shell as the owner of the Qt cross-build environment; opening another terminal requires sourcing the script again.
- After sourcing, build commands should use the SDK-provided cross compiler, target sysroot, Qt headers, Qt libraries, and package metadata.
- For non-Qt helper tools that intentionally use a different toolchain, do not inherit the SDK `CC` variable by default. The ST SDK exports `CC` as a command string with compiler flags, not a single executable path, so helper scripts that test `[ -x "$CC" ]` or invoke `"$CC"` must use a tool-specific override such as `OVERLAY_CC`.
- Do not assume the environment is persistent after logout, SSH reconnect, terminal restart, or a new non-interactive build shell.
- When code is edited on Windows but built on `cfr-vm`, verify the VM source tree contains the new feature markers before building, and verify the produced ARM binary contains the expected strings before deploying. A successful build of stale VM sources is a failed deployment.
- When QML changes, verify `rcc -name qml` runs during the cross-build or otherwise force regeneration of `qrc_qml.cpp`; otherwise the board can run a new C++ binary with old embedded QML.

### Validation & Error Matrix

| Check | Expected Result | Failure Meaning |
|---|---|---|
| `echo $SDKTARGETSYSROOT` | Prints the SDK target sysroot path | SDK environment was not loaded |
| `which qmake` or `command -v qmake` | Resolves to an SDK-provided Qt tool | Qt toolchain is not visible in `PATH` |
| `pkg-config --variable=prefix Qt5Core` | Resolves through the SDK metadata | `PKG_CONFIG_PATH` / sysroot variables are missing |
| Qt build command | Uses target cross compiler instead of host compiler | The resulting binary may not run on STM32MP157 |
| Non-Qt helper build after SDK source | `source ...environment-setup... && ./build_uvc_kms_overlay.sh` still produces an ARM `uvc_kms_overlay` binary; overrides use `OVERLAY_CC=/path/to/gcc` | The script inherited SDK `CC="compiler flags..."` and treated it as an executable path |
| VM source freshness | `grep -n -E '<new_feature_marker>' main.cpp qml/Main.qml` on `cfr-vm` finds the local changes before build | The VM is compiling stale source copied from a previous task |
| Binary feature proof | `strings build-mp157/qt_camera_display \| grep -E '<new_feature_marker>'` finds the expected QML/C++ marker | The build artifact does not include the intended feature even if compilation succeeded |
| Board deployment proof | Board-side `strings /root/qt_camera_display/qt_camera_display \| grep -E '<new_feature_marker>'` or a feature-specific runtime check succeeds after copy/restart | The board is still running an old binary or `scp` failed due to `Text file busy` |

### Good / Base / Bad Cases

```bash
# Good: load the SDK first, then configure and build the Qt project for STM32MP157.
source /opt/st/stm32mp1/3.1-snapshot/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi
qmake
make
```

```bash
# Base: if the current shell may already be configured, verify before building.
echo "$SDKTARGETSYSROOT"
command -v qmake
```

```bash
# Bad: building a target Qt program from a fresh shell without loading the SDK.
qmake
make
```

```sh
# Bad: a helper script inherits the SDK CC command string and then treats it as a single executable path.
CC="${CC:-$BR_OUTPUT/host/bin/arm-none-linux-gnueabihf-gcc}"
[ -x "$CC" ] || exit 1
"$CC" --sysroot="$SYSROOT" helper.c -o helper
```

```sh
# Correct: helper scripts use a tool-specific override, so the SDK CC cannot pollute a different toolchain.
OVERLAY_CC="${OVERLAY_CC:-$BR_OUTPUT/host/bin/arm-none-linux-gnueabihf-gcc}"
CC="$OVERLAY_CC"
[ -x "$CC" ] || exit 1
"$CC" --sysroot="$SYSROOT" helper.c -o helper
```

```bash
# Bad: deploying after a successful VM build without proving the VM source and board binary contain the feature just edited locally.
./build_qt_camera_display.sh
scp build-mp157/qt_camera_display root@192.168.1.250:/root/qt_camera_display/
```

```bash
# Correct: prove source -> build -> board all contain the same feature contract.
grep -n -E 'historyDetailVisible|compactUploadStatus' main.cpp qml/Main.qml
./build_qt_camera_display.sh
strings build-mp157/qt_camera_display | grep -E 'historyDetailVisible|compactUploadStatus'
ssh root@192.168.1.250 '/root/qt_camera_display/run_qt_kms_overlay_display.sh stop'
scp build-mp157/qt_camera_display root@192.168.1.250:/root/qt_camera_display/
ssh root@192.168.1.250 'chmod 755 /root/qt_camera_display/qt_camera_display && /root/qt_camera_display/run_qt_kms_overlay_display.sh restart'
```

### Tests Required

- Verify the SDK variables are visible before the build, especially `SDKTARGETSYSROOT`.
- Verify Qt tools resolve from the SDK environment before configuring the project.
- After build, confirm the generated binary is for ARM/Linux target rather than the x86_64 host before deploying to the board.
- For helper binaries that do not use the Qt SDK compiler, run at least one build after sourcing the Qt SDK to prove external `CC` does not break the helper script, and add a static contract check for the tool-specific override variable.
- For Windows-to-VM workflows, confirm the VM source has the local change markers before build and the ARM binary has those markers before board deployment.
- Before overwriting a running board binary, stop the Qt service and confirm `pidof qt_camera_display` is empty; `scp` can fail with `Text file busy` if the executable is still mapped.

---

## Scenario: Qt Touch Calibration Weight Input And F4 Reply Display Contract

Use this convention when the STM32MP157 Qt camera UI lets an operator enter a calibration weight, sends a calibration command to the STM32F4, or displays the serial reply returned by an F4 command.

### 1. Scope / Trigger

- Trigger: changing `20_uvc_camera/qt_camera_display/qml/Main.qml`, `20_uvc_camera/qt_camera_display/main.cpp`, `DeviceHealthController`, or `test_qt_kms_overlay_assets.sh` around weight calibration, F4 serial commands, or calibration result display.
- Bug learned: a `TextInput` with `IntValidator` is not a complete touch workflow on the board, because the deployed Qt/Wayland image may not provide a usable system soft keyboard; if only preset buttons exist, the operator cannot enter arbitrary grams.
- Bug learned: an F4 command can return more text than a compact label can show. If C++ truncates the reply with `reply.left(...)` and QML then renders the only copy in a single-line `Text` with `elide`, the operator loses the evidence needed to confirm calibration.
- Goal: arbitrary `1..5000` gram values must be enterable by touch, validated once before sending, and F4 replies must remain readable to the last meaningful byte or line in the result overlay.

### 2. Signatures

| Operation | Signature / Marker |
|---|---|
| Calibration text state | `property string calibrationWeightText` in `Main.qml` |
| Touch digit append | `appendCalibrationDigit(digit)` |
| Touch digit delete | `backspaceCalibrationDigit()` |
| Touch input clear | `clearCalibrationWeight()` |
| Calibration send action | `sendCalibrationCommand()` |
| Serial command bridge | `DeviceHealthController::sendF4Command(QString command)` |
| F4 reply reader | `readF4ReplyText(int fd, QString *errorText)` |
| Result scroll owner | `calibrationResultFlickable` |
| Static contract test | `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh` |

### 3. Contracts

| Area | Contract |
|---|---|
| Touch input ownership | Numeric entry must not rely only on `TextInput`, `inputMethodHints`, or a platform soft keyboard. Provide in-app touch controls for digits, backspace, and clear, while keeping preset value buttons only as shortcuts. |
| Validation owner | `sendCalibrationCommand()` or the single send path must validate the final integer range before calling C++; digit buttons may limit obvious overflow, but they must not be the only protection. |
| Calibration range | Accept arbitrary integer grams in the supported range, currently `1..5000`. Reject empty text, non-numeric text, zero, negative values, and values above the allowed maximum with an operator-visible message. |
| Preset behavior | Preset buttons such as `50g`, `100g`, or `500g` must assign the same `calibrationWeightText` state used by the keypad and send path, not a separate hidden state. |
| Serial read length | The C++ F4 reply path must preserve enough response text for diagnostics. Do not create the displayed message from `reply.left(96)`, `reply.left(48)`, or another small fixed preview unless a full-detail field is also available. |
| Display budget | The modal or result detail must use a bounded multi-line area such as `Flickable` plus wrapped `Text`. A toast, status chip, or one-line label may show a short summary only; it must not be the only copy of the reply. |
| Reset behavior | Reopening a calibration result or sending a new command must reset the result `Flickable.contentY` to the top after assigning new text, so stale scroll position cannot hide the beginning. |
| Resource packaging | QML changes are embedded through the Qt resource build. Rebuild and redeploy `qt_camera_display`; copying `Main.qml` alone is not a valid board fix. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning | Required Action |
|---|---|---|---|
| Arbitrary touch entry | Tapping digits can build a value not present in the preset buttons, such as `137g` | UI still depends on an unavailable soft keyboard or only exposes fixed choices | Add or repair the in-app keypad path |
| Delete and clear | Backspace removes one digit; clear returns the field to empty or the documented default | Operator cannot correct a mistyped weight without leaving the dialog | Add keypad correction controls |
| Range rejection | Empty, `0`, and `5001` show a clear validation failure and do not send serial bytes | Invalid calibration payload can reach the F4 | Centralize validation in `sendCalibrationCommand()` |
| Preset/keypad consistency | Preset and keypad values update the same displayed text and send the same command format | There are two sources of truth for calibration weight | Route both paths through `calibrationWeightText` |
| Long F4 reply | A reply longer than one line remains scrollable/readable in the result overlay | C++ truncation or QML elide hides calibration proof | Preserve full-enough reply text and show it in `calibrationResultFlickable` |
| Static test | `./test_qt_kms_overlay_assets.sh` finds keypad markers, F4 reader marker, result `Flickable`, and forbids `reply.left(96)` / `reply.left(48)` | A future edit can silently reintroduce the two bugs | Add or repair static assertions |
| Binary marker proof | `strings build-mp157/qt_camera_display` and board-side `strings /root/qt_camera_display/qt_camera_display` find the keypad/result markers | The VM or board is running stale embedded QML/C++ | Resync sources, rebuild, redeploy, and restart Qt |

### 5. Good / Base / Bad Cases

```qml
// Good: preset buttons and keypad digits update one text state that the send path validates.
function appendCalibrationDigit(digit) {
    calibrationWeightText = calibrationWeightText + digit
}

function sendCalibrationCommand() {
    var grams = parseInt(calibrationWeightText, 10)
    if (isNaN(grams) || grams < 1 || grams > 5000) {
        calibrationResultText = "请输入 1~5000g 的标定克重"
        return
    }
    deviceHealthController.sendF4Command("CAL " + grams)
}
```

```qml
// Good: long F4 replies live in a scrollable detail area, not only in a toast.
Flickable {
    id: calibrationResultFlickable
    clip: true
    contentHeight: calibrationResultTextItem.paintedHeight

    Text {
        id: calibrationResultTextItem
        width: calibrationResultFlickable.width
        wrapMode: Text.Wrap
        text: calibrationResultText
    }
}
```

```cpp
/* Good: the serial helper returns the reply text collected for this command,
 * and the UI decides how to present a short summary versus full details.
 */
QString detail = readF4ReplyText(fd, &errorText);
```

```qml
// Bad: this depends on a platform keyboard that may not exist on the board.
TextInput {
    inputMethodHints: Qt.ImhDigitsOnly
    validator: IntValidator { bottom: 1; top: 5000 }
}
```

```cpp
/* Bad: this destroys the diagnostic bytes before QML has any chance to show them. */
return QStringLiteral("标定成功：") + reply.left(48);
```

### 6. Tests Required

- Run `./test_qt_kms_overlay_assets.sh` after changing calibration UI, F4 command code, QML result overlays, or the static marker list.
- Add static assertions for `calibrationKeypadGrid`, `appendCalibrationDigit`, `backspaceCalibrationDigit`, `clearCalibrationWeight`, `calibrationResultFlickable`, `readF4ReplyText`, and the absence of known truncation calls such as `reply.left(96)` / `reply.left(48)`.
- Cross-build `qt_camera_display` with the ST Qt SDK after every QML or C++ change, then verify the ARM binary contains the relevant QML/C++ markers with `strings`.
- Deploy the rebuilt binary to the board, restart `/root/qt_camera_display/run_qt_kms_overlay_display.sh`, and verify the running board binary contains the same markers.
- On the LCD, enter at least one non-preset value such as `137g`, correct it with backspace, clear it, send a valid value, and test invalid values `0` and `5001`.
- For F4 reply display, use a real or simulated F4 response longer than one compact line and confirm the result overlay can scroll to the final line.

### 7. Wrong vs Correct

#### Wrong

```text
The QML has a TextInput and IntValidator, so the board operator can type any gram value.
```

#### Correct

```text
The board must prove arbitrary touch entry through in-app digit, backspace, and clear controls; TextInput validation is only an auxiliary text-state constraint.
```

#### Wrong

```text
The F4 reply is visible enough because the toast or one-line status label shows the first 48 bytes.
```

#### Correct

```text
Keep a short summary for the toast, but preserve the reply details and render them in a bounded scrollable result area.
```

---

## Qt Boot Display And Early Framebuffer Splash Contract

Use this convention when changing the STM32MP157 Qt boot display chain, the SysV init scripts that start it, or any helper that draws before Qt/eglfs is ready.

### 1. Scope / Trigger

- Trigger: changing `20_uvc_camera/S05display-quiet`, `20_uvc_camera/S90uvc-camera`, `20_uvc_camera/qt_camera_display/run_qt_kms_overlay_display.sh`, `20_uvc_camera/qt_camera_display/fb_boot_splash.c`, `20_uvc_camera/qt_camera_display/ai_boot_splash_preview.html`, `20_uvc_camera/qt_camera_display/generate_boot_splash_asset.py`, generated splash assets, or the QML `splashOverlay`.
- Goal: the LCD should show a static first-frame style image as soon as `/dev/fb0` is writable, then QML `splashOverlay`, then the home page with KMS overlay video restored.
- Boundary: early framebuffer splash must not depend on Qt, OpenGL, DRM/KMS, UVC camera nodes, image codecs, or `/dev/galcore`.
- Design source rule: high-fidelity early splash visuals with Chinese text, glows, transparent layers, chip logos, and precise typography must be designed in HTML/CSS first, rendered into PNG/RGB565 during build/deploy preparation, and then blitted by `fb_boot_splash`. Do not hand-rebuild complex UI artwork in C except as a low-fidelity resource-missing fallback.

### 2. Signatures

| Operation | Signature |
|---|---|
| HTML splash design source | `20_uvc_camera/qt_camera_display/ai_boot_splash_preview.html` |
| Splash asset generator | `python3 generate_boot_splash_asset.py` |
| Generated PNG preview | `20_uvc_camera/qt_camera_display/boot_splash.png` |
| Generated raw asset | `20_uvc_camera/qt_camera_display/boot_splash.rgb565` |
| Raw asset byte contract | `1024 * 600 * 2 = 1228800` bytes |
| Early splash build | `./build_fb_boot_splash.sh` |
| Early splash binary | `/root/qt_camera_display/fb_boot_splash` |
| Early splash command | `/root/qt_camera_display/fb_boot_splash -f /dev/fb0 -q` |
| Early splash asset override | `/root/qt_camera_display/fb_boot_splash -f /dev/fb0 -a /root/qt_camera_display/boot_splash.rgb565` |
| Build compiler override | `SPLASH_CC=/path/to/arm-gcc ./build_fb_boot_splash.sh` |
| Init helper env | `FB_BOOT_SPLASH_BIN=/root/qt_camera_display/fb_boot_splash` |
| Init framebuffer env | `FB_DEV=/dev/fb0` |
| Disable switch | `FB_BOOT_SPLASH_ENABLE=0` |
| Startup scripts | `S05display-quiet`, `S90uvc-camera`, `run_qt_kms_overlay_display.sh` call `show_boot_splash()` |
| Static contract | `./test_qt_kms_overlay_assets.sh` checks `generate_boot_splash_asset.py`, `boot_splash.png`, `boot_splash.rgb565`, `fb_boot_splash.c`, `build_fb_boot_splash.sh`, `FB_BOOT_SPLASH_BIN`, `show_boot_splash`, `/dev/fb0`, `FBIOGET_VSCREENINFO`, `mmap`, `draw_splash_asset`, `draw_splash_fallback`, and `msync` |

### 3. Contracts

| Area | Contract |
|---|---|
| Dependency boundary | `fb_boot_splash` must use Linux framebuffer ioctls and `mmap` only. It must not require Qt runtime, PNG/JPEG decoders, DRM resources, UVC camera nodes, or GPU initialization. |
| Design-to-asset pipeline | `ai_boot_splash_preview.html` is the visual source of truth. `generate_boot_splash_asset.py` renders it to `boot_splash.png`, converts that image to little-endian RGB565 raw pixels, and writes `boot_splash.rgb565`. The board runtime must not execute a browser, HTML renderer, PNG decoder, or Pillow. |
| Asset-first rendering | `fb_boot_splash` must try the RGB565 asset first through `DEFAULT_SPLASH_ASSET` or `-a`. Only if the asset is missing, unreadable, or has an invalid byte count may it draw `draw_splash_fallback`. A successful resource path should log `asset: /root/qt_camera_display/boot_splash.rgb565` or equivalent. |
| Canvas and crop | The HTML artboard and generated asset are fixed at `1024x600`. The generator must capture the actual splash artboard, not the browser window, body margin, or a decorative preview frame. Borders, shadows, or dark page chrome around a desktop preview must not be baked into `boot_splash.rgb565` unless the final board design intentionally includes them. |
| Pixel support | The helper must support the framebuffer formats used on the board, at minimum 16 bpp RGB565 and common 24/32 bpp RGB layouts through framebuffer bitfields. Unsupported formats must return non-zero with a clear error. |
| Script behavior | `show_boot_splash()` is visual fallback only. Missing binary, missing `/dev/fb0`, or draw failure must not block Qt startup. |
| Startup order | Draw early static splash after `/dev/fb0` exists and before long waits for camera, GPU, overlay socket, or Qt QML load. |
| Overlay order | KMS overlay video must still start hidden with `-V 0`; QML restores it with `VISIBLE 1` only after the QML splash fades out. |
| Build isolation | `build_fb_boot_splash.sh` must use `SPLASH_CC`, not inherited `CC`, because the ST Qt SDK exports `CC` as a compiler command plus flags. |
| Deployment | `deploy_qt_camera_display.sh` must require and install `build-mp157/fb_boot_splash` with mode `755` and `boot_splash.rgb565` with mode `644` beside the Qt binary and overlay helper. |
| Documentation | `20_uvc_camera/README.md` and `20_uvc_camera/qt_camera_display/README.md` must document the file list, build command, deploy path, board test command, and failure triage. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning |
|---|---|---|
| Static contract | `./test_qt_kms_overlay_assets.sh` prints `PASS: Qt KMS overlay assets contract` | One of the boot display chain markers drifted or a required file is missing |
| Asset generation | `python3 generate_boot_splash_asset.py` writes `boot_splash.png` and `boot_splash.rgb565` | Chromium/Edge, Playwright/Selenium glue, Pillow, the HTML source, or the crop selector is broken |
| Raw asset size | `stat -c %s boot_splash.rgb565` prints `1228800` | The output size no longer matches the 1024x600 RGB565 framebuffer contract |
| PNG visual review | `boot_splash.png` visually matches the HTML artboard with no unintended black border | The generator captured the preview wrapper, body background, browser margin, or an old HTML frame/shadow |
| Early helper build | `./build_fb_boot_splash.sh` outputs an ARM ELF at `build-mp157/fb_boot_splash` | Buildroot compiler/sysroot path is wrong, or the helper picked up a bad compiler environment |
| Board helper smoke | `/root/qt_camera_display/fb_boot_splash -f /dev/fb0 -q && echo fb_splash_rc=0` prints `fb_splash_rc=0` and logs/use confirms the asset path | `/dev/fb0` is missing, the raw asset is absent or the wrong size, the pixel format is unsupported, or the binary is not executable for the board |
| Board fb facts | `cat /sys/class/graphics/fb0/bits_per_pixel; cat /sys/class/graphics/fb0/virtual_size` matches expected screen facts | The board display path changed; revisit helper format support and layout scaling |
| Startup logs | `run_qt_kms_overlay_display.sh restart` logs `early static splash drawn on /dev/fb0` before Qt/overlay status | The script did not call the early helper or `/dev/fb0` was not ready |
| Runtime status | `run_qt_kms_overlay_display.sh status` shows both `qt_camera_display` and `uvc_kms_overlay` PIDs | Early splash or startup script changes broke the formal display stack |

### 5. Good / Base / Bad Cases

```sh
# Good: edit the visual source first, then regenerate the exact board asset.
python3 generate_boot_splash_asset.py
stat -c %s boot_splash.rgb565
```

```text
Good: `stat` prints `1228800`, and `boot_splash.png` is the expected 1024x600 artboard without desktop preview borders.
```

```sh
# Good: build the early framebuffer helper with its own compiler variable.
SPLASH_CC=/home/cfr/linux/buildroot/buildroot-2020.02.6/output-uvc/host/bin/arm-none-linux-gnueabihf-gcc ./build_fb_boot_splash.sh
```

```sh
# Good: the init script treats early splash as optional visual fallback.
if [ -x "$FB_BOOT_SPLASH_BIN" ] && [ -e "$FB_DEV" ]; then
    "$FB_BOOT_SPLASH_BIN" -f "$FB_DEV" -q >/dev/null 2>&1 || true
fi
```

```c
/* Good: runtime code blits the generated resource first and keeps C drawing as fallback only. */
used_asset = (draw_splash_asset(&fb, cfg.asset_path) == 0);
if (!used_asset) {
    draw_splash_fallback(&fb);
}
```

```c
/* Bad: manually redrawing the full HTML visual in C and expecting pixel-level fidelity. */
draw_complex_chinese_title_with_many_rectangles();
draw_css_like_glows_with_integer_loops();
draw_transparent_cards_by_hand();
```

```sh
# Bad: blocking boot because the optional early splash binary is absent.
[ -x "$FB_BOOT_SPLASH_BIN" ] || exit 1
```

```sh
# Bad: compiling the helper with inherited CC after sourcing the Qt SDK.
CC="${CC:-$BR_OUTPUT/host/bin/arm-none-linux-gnueabihf-gcc}"
```

```text
Base: If the LCD driver registers `/dev/fb0` late, `S05display-quiet` may skip the helper and `S90uvc-camera` should draw it after `wait_for_node "$FB_DEV"` succeeds.
```

```text
Base: If `boot_splash.rgb565` is missing on a test board, `fb_boot_splash` may use `draw_splash_fallback`, but that fallback is not the design source and must not be used to judge final visual fidelity.
```

### 6. Tests Required

- After any visual change, regenerate assets with `python3 generate_boot_splash_asset.py`; assert `boot_splash.rgb565` is exactly `1228800` bytes and visually inspect `boot_splash.png`.
- Run `./test_qt_kms_overlay_assets.sh` after changing early splash code, boot display scripts, deploy scripts, or QML `splashOverlay`.
- Run `sh -n` on `S05display-quiet`, `S90uvc-camera`, `build_fb_boot_splash.sh`, `run_qt_kms_overlay_display.sh`, and `deploy_qt_camera_display.sh`.
- Cross-build `fb_boot_splash` and confirm `file build-mp157/fb_boot_splash` reports an ARM 32-bit EABI executable.
- Deploy the helper, `boot_splash.rgb565`, and scripts to the board or NFS rootfs, then assert `/root/qt_camera_display/fb_boot_splash -f /dev/fb0 -q && echo fb_splash_rc=0`; if possible, run once without `-q` and confirm the log says the `asset` path was used rather than fallback.
- Restart the formal display stack and assert logs include `early static splash drawn on /dev/fb0` and status shows both Qt and overlay PIDs.

### 7. Wrong vs Correct

#### Wrong

```text
The black gap happens before Qt starts, but the fix only changes QML splash timing.
```

```text
The HTML preview looks right, so manually reimplementing the same layout in C should be close enough for the board.
```

```text
The generator screenshots a desktop preview wrapper that includes a border, box-shadow, or body background, then the board shows an unintended black edge.
```

#### Correct

```text
Draw a static first-frame style image directly to `/dev/fb0` before Qt/GPU/camera startup, then let QML `splashOverlay` continue the animated boot sequence.
```

```text
Treat HTML/CSS as the source of truth, regenerate `boot_splash.png` and `boot_splash.rgb565`, deploy the raw RGB565 asset, and let `fb_boot_splash` blit that asset before Qt starts.
```

```text
Capture only the 1024x600 splash artboard and remove preview-only frames, shadows, margins, or backgrounds before generating `boot_splash.rgb565`.
```

---

## Scenario: Qt Real Device Health Refresh And KMS Overlay Visibility

Use this convention when the STM32MP157 Qt camera UI shows real 4G, camera, F4, cloud, or SD-card health, or when QML controls the KMS overlay video plane during boot and USB camera hotplug recovery.

### 1. Scope / Trigger

- Trigger: changing `20_uvc_camera/qt_camera_display/main.cpp`, `20_uvc_camera/qt_camera_display/qml/Main.qml`, `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`, or `20_uvc_camera/qt_camera_display/run_qt_kms_overlay_display.sh` for health status, camera hotplug, cloud probing, serial probing, or overlay visibility.
- Trigger: fixing flicker where network/cloud repeatedly alternates between `检测中` and `在线` / `已连接`.
- Trigger: fixing startup ordering where the external KMS camera plane appears before the Qt splash or home UI is ready.
- Goal: health labels must represent the last proven real device state without blocking QML, and the camera plane must be visible only after Qt has completed its boot display handoff.
- Boundary: QML binds and displays state only. Real socket, shell, serial, and filesystem probes belong in C++ controllers or helper scripts using asynchronous processes or worker threads.

### 2. Signatures

| Operation | Signature |
|---|---|
| Health controller | `class DeviceHealthController : public QObject` |
| Periodic refresh | `m_healthTimer.setInterval(8000)` or slower unless a task explicitly requires faster diagnostics |
| Network probe | `startNetworkProbe()` asynchronously starts `4g-ppp test` |
| Cloud probe | `startCloudProbe()` asynchronously starts `curl -fsS --max-time 2 http://139.9.35.72/health` |
| Process completion | `handleNetworkProcessFinished(...)`, `handleCloudProcessFinished(...)` update final state |
| Process errors | `handleNetworkProcessError(...)`, `handleCloudProcessError(...)` update a failure state |
| Probe timeout | `handleNetworkProbeTimeout()`, `handleCloudProbeTimeout()` kill the probe and set timeout/failure state |
| Overlay startup | `uvc_kms_overlay` starts hidden through `-V 0` |
| Overlay restore gate | `root.bootOverlayRestoreFinished && !root.splashOverlayVisible && root.activePage === "home"` |
| QML camera recovery | `onCameraStatusChanged` may call `storageController.setOverlayVisible(true)` only behind the restore gate |
| Page navigation recovery | `switchPage("home")` may restore overlay only behind the same restore gate |
| Static regression test | `./test_qt_kms_overlay_assets.sh` checks health refresh markers and QML overlay restore gates |

### 3. Contracts

| Area | Contract |
|---|---|
| Stable refresh semantics | Only the first unknown state may show `检测中`. Later periodic refreshes must keep the last stable label while probes run in the background, then update only on success, failure, timeout, or explicit device absence. |
| Network truth source | 4G/network status may show `在线` only when the real test command exits successfully. A missing `4g-ppp`, non-zero exit, timeout, or process error must not display online. |
| Cloud truth source | Cloud status may show `已连接` only when the configured health URL succeeds within the timeout. DNS, TCP, HTTP, command, or timeout failure must show a non-connected state. |
| No flicker writes | `startNetworkProbe()` must not call `setNetworkStatus(QStringLiteral("检测中"), ...)` on every cycle. `startCloudProbe()` must not call `setCloudStatus(QStringLiteral("检测中"), ...)` on every cycle. |
| UI responsiveness | `DeviceHealthController` must not call `waitForStarted()`, `waitForFinished()`, blocking shell commands, blocking serial reads, or sleeps on the Qt/UI thread. Use Qt signals, timeouts, and worker threads. |
| Camera truth source | Camera status may show online only after the overlay `STATUS` path proves frames are present and progressing. USB unplug or a stopped serial/frame counter must become offline and may trigger `restart-overlay` without killing Qt. |
| F4 truth source | F4 may show `接入` only after a configured serial handshake returns an accepted response such as `ACK`, `OK`, `F4`, or `READY`. Missing TTY, open failure, timeout, or unrecognized response is `待接入` / offline. |
| Overlay initial visibility | The KMS video plane must start hidden with `-V 0`; early camera initialization is allowed, but early camera display is not. |
| QML boot handoff | QML may send `VISIBLE 1` only after `splashOverlayVisible` is false, `bootOverlayRestoreFinished` is true, and the active page is `home`. |
| Page ownership | Non-home pages keep overlay hidden. Returning to home does not bypass the boot handoff gate, even if camera status already says online. |
| Build deployment proof | Because QML is compiled through `qml.qrc`, QML changes require proving `rcc -name qml` reran and the deployed board binary contains the expected markers. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning |
|---|---|---|
| Static health contract | `./test_qt_kms_overlay_assets.sh` passes checks for `m_healthTimer.setInterval(8000)`, no `waitForStarted`, and no repeated `检测中` writes inside network/cloud probe starters | Health refresh may flicker or block the UI thread |
| Static overlay contract | The same test finds `bootOverlayRestoreFinished` and restore conditions containing `bootOverlayRestoreFinished`, `!splashOverlayVisible`, and `activePage === "home"` in both camera-status and page-return paths | Camera video can appear before Qt splash/home is ready |
| 4G board check | `4g-ppp test; echo "exit=$?"` exits `0`, and Qt shows `在线` only after that success | UI is showing a guessed state instead of a real network probe |
| Cloud board check | `curl -fsS --max-time 2 http://139.9.35.72/health; echo "exit=$?"` succeeds, and Qt shows `已连接` only after that success | UI is showing cloud connectivity without a live backend check |
| Camera unplug check | Unplugging the USB camera changes Qt camera status to offline and does not freeze navigation | Camera state is cached, overlay polling is blocked, or hotplug recovery is not isolated |
| Camera replug check | Replugging the USB camera lets `restart-overlay` recover frames while the Qt PID stays unchanged; the video becomes visible only on home after the boot gate | Recovery killed Qt, or QML restored overlay from the wrong page/state |
| F4 serial check | Only a successful `/dev/ttySTM2` handshake changes F4 from `待接入` to `接入` | F4 status is hard-coded or not tied to serial communication |
| Startup ordering check | `/tmp/uvc-kms-overlay.log` contains `initial-visible=0`; the LCD shows early splash, then QML splash, then home with video | Overlay was launched visible or QML restored it too early |
| Deployment check | Board-side `strings /root/qt_camera_display/qt_camera_display \| grep -E 'bootOverlayRestoreFinished|DeviceHealthController'` finds the markers after deploy | Board is still running an old binary or stale embedded QML |

### 5. Good / Base / Bad Cases

```cpp
/* Good: 周期刷新只启动异步探测，不把稳定的在线状态先改回“检测中”。 */
void startNetworkProbe()
{
    m_networkProbe.start(QStringLiteral("4g-ppp"), QStringList() << QStringLiteral("test"));
    m_networkTimeout.start();
}
```

```cpp
/* Bad: 每 8 秒先写“检测中”，再写“在线”，用户会看到顶部网络状态循环闪烁。 */
void startNetworkProbe()
{
    setNetworkStatus(QStringLiteral("检测中"), QStringLiteral("#f4b942"));
    m_networkProbe.start(QStringLiteral("4g-ppp"), QStringList() << QStringLiteral("test"));
}
```

```cpp
/* Bad: 在 Qt 主线程等待进程启动或结束，会让触摸、动画和页面切换短暂停顿。 */
m_networkProbe.start(QStringLiteral("4g-ppp"), QStringList() << QStringLiteral("test"));
m_networkProbe.waitForStarted(1000);
```

```qml
// Good: camera recovery can show the external video plane only after Qt has completed the boot handoff.
if (root.bootOverlayRestoreFinished && !root.splashOverlayVisible && root.activePage === "home") {
    storageController.setOverlayVisible(true)
}
```

```qml
// Bad: camera status changes before the splash ends, so the KMS plane appears above the Qt boot UI.
onCameraStatusChanged: {
    if (deviceHealth.cameraStatus === "在线") {
        storageController.setOverlayVisible(true)
    }
}
```

```text
Base: On cold boot the overlay helper may initialize and collect frames before QML is visible, but it must remain hidden until QML explicitly sends `VISIBLE 1` after the splash fade has completed.
```

### 6. Tests Required

- Run `./test_qt_kms_overlay_assets.sh` after changing health status, cloud/network probing, camera hotplug handling, QML splash logic, page navigation, overlay visibility, or deployment scripts.
- The static test must assert that `DeviceHealthController` does not contain `waitForStarted` and that `startNetworkProbe()` / `startCloudProbe()` do not reset labels to `检测中` during every periodic probe.
- The static test must assert that QML contains `bootOverlayRestoreFinished`, that `onCameraStatusChanged` uses the full restore gate, and that `switchPage("home")` uses the same gate before sending `VISIBLE 1`.
- On board, verify 4G and cloud with the exact shell commands used by the controller and compare the UI labels with command exit codes.
- On board, unplug and replug the USB camera. Confirm Qt remains responsive, the Qt PID does not change during `restart-overlay`, camera status becomes offline then online, and video is restored only on the home page.
- On board, verify F4 status with a real serial handshake. Do not accept a screenshot or hard-coded QML label as proof.
- After QML changes, rebuild and deploy the Qt binary, then prove the board binary contains the expected QML/C++ markers before accepting visual behavior.

### 7. Wrong vs Correct

#### Wrong

```text
Periodic health refresh writes `检测中` immediately, then writes `在线` or `已连接` after each successful probe.
```

#### Correct

```text
Periodic health refresh runs silently in the background and leaves the previous stable label visible until a real success, failure, timeout, or unplug event changes the state.
```

#### Wrong

```text
Camera online status directly sends `VISIBLE 1`, so a hotplug or early frame can display video before the Qt splash has disappeared.
```

#### Correct

```text
Camera online status and `switchPage("home")` both use the same boot/page gate: `bootOverlayRestoreFinished && !splashOverlayVisible && activePage === "home"`.
```

---

## Qt And Procfs Runtime Checks

Use this convention when a Qt or user-space helper on the STM32MP157 board must read kernel-generated procfs files such as `/proc/mounts`, `/proc/bus/input/devices`, `/proc/<pid>/fd`, or `/proc/<pid>/status`.

### Scope / Trigger

- Trigger: checking whether `/mnt/sdcard` is mounted before saving pictures.
- Trigger: checking whether Qt opened a Goodix input event node.
- Trigger: reading procfs files from Qt/C++ code or shell-adjacent board utilities.

### Signatures

| Operation | Signature |
|---|---|
| POSIX mount check | `FILE *fp = fopen("/proc/mounts", "r"); fscanf(fp, "%255s %4095s %*s %*s %*d %*d\n", device, path)` |
| Shell mount proof | `mount | grep ' /mnt/sdcard '` |
| SD save self-test | `/root/qt_camera_display/qt_camera_display --storage-self-test` |
| Touch FD proof | `qtpid=$(pidof qt_camera_display); ls -l /proc/$qtpid/fd | grep /dev/input/event` |

### Contracts

| Area | Contract |
|---|---|
| Procfs read semantics | Do not rely on ordinary file size, seekability, or high-level EOF helpers for procfs files. Read records until the read/scan function itself stops returning complete records. |
| Qt mount checks | For `/proc/mounts` in Qt/C++, prefer POSIX `fopen/fscanf/fclose` or another stream parser that does not use file size as the EOF signal. |
| Error reporting | If a mount check fails, report whether `/proc/mounts` could not be opened or whether the exact mount point was absent. Do not collapse both into a generic SD-card failure. |
| Cross-layer proof | A successful shell `mount` output and a failing Qt mount check means the bug is in the Qt parsing layer, not in the SD card service. |
| Screen-button proof | A direct socket probe or `--storage-self-test` proves only the lower save path. The physical `保存图片` button is accepted only when the running Qt main-process log records `storage action save-image requested/result` after a screen tap and new JPG/PNG files appear on `/mnt/sdcard/images`. |

### Validation & Error Matrix

| Check | Good Result | Failure Meaning |
|---|---|---|
| `mount | grep ' /mnt/sdcard '` | Shows `/dev/mmcblk0p1 on /mnt/sdcard type vfat` | SD card is not mounted or the mount service failed |
| Qt `--storage-self-test` | Prints `保存成功：JPG /mnt/sdcard/images/uvc_*.jpg PNG /mnt/sdcard/images/uvc_*.png`, followed by `上传成功` or a specific upload failure reason | Qt controller, procfs parsing, socket command, overlay save path, or COS upload helper failed |
| File content check | Latest JPG starts with JPEG SOI bytes and latest PNG starts with the PNG signature; both files have stable nonzero size | The overlay did not write complete browser-previewable images |
| Qt log check | `/tmp/qt-kms-overlay-shell.log` contains `storage action save-image requested/result` | The QML button did not reach the C++ controller |
| Regression signature | Shell `mount` shows `/mnt/sdcard`, but Qt returns `/mnt/sdcard 未挂载` | The Qt procfs parser is suspect; check for `QTextStream::atEnd()` or any logic that depends on procfs file size/seek semantics |

### Good / Base / Bad Cases

#### Good

```cpp
/* Good: /proc/mounts 是内核动态生成文件，使用 fscanf 逐字段读取，不依赖文件大小或 seek 语义。 */
FILE *mounts = std::fopen("/proc/mounts", "r");
while (std::fscanf(mounts, "%255s %4095s %*s %*s %*d %*d\n", device, path) == 2) {
    if (std::strcmp(path, "/mnt/sdcard") == 0) {
        found = true;
        break;
    }
}
```

#### Bad

```cpp
/* Bad: 在 procfs 上用高级 EOF 状态推断可能提前认为文件结束，导致 Qt 误判 /mnt/sdcard 未挂载。 */
QTextStream stream(&mounts);
while (!stream.atEnd()) {
    const QString line = stream.readLine();
}
```

### Tests Required

- Run the static contract test after changing Qt storage checks: `./test_qt_kms_overlay_assets.sh`.
- Cross-build the Qt binary with the STM32MP157 SDK and deploy it to the NFS rootfs.
- On the board, run `mount | grep ' /mnt/sdcard '`, `/root/qt_camera_display/qt_camera_display --storage-self-test`, and latest JPG/PNG header plus size-stability checks.
- For screen-button acceptance, verify the Qt main-process log contains `storage action save-image requested/result` after the human taps `保存图片`, then verify the newest JPG/PNG pair under `/mnt/sdcard/images` has correct headers and stable nonzero sizes.

---

## Qt JPG/PNG Save And COS Upload Contract

Use this convention when the STM32MP157 Qt camera screen saves the current KMS overlay frame and sends image objects to the defect cloud backend.

### Scope / Trigger

- Trigger: changing `20_uvc_camera/qt_camera_display/main.cpp`, `uvc_kms_overlay.c`, `defect-cos-upload`, deployment scripts, or cloud upload mapping for the `保存图片` button.
- Local frame source: `uvc_kms_overlay` owns the live camera plane and must save the frame it is actually displaying, not a Qt screenshot.
- Cloud preview requirement: files registered to the cloud must be browser-previewable image formats. Do not upload PPM as the final user-visible file.

### Signatures

| Operation | Signature |
|---|---|
| Overlay socket command | `SAVE_DUAL /mnt/sdcard/images` |
| Overlay success reply | `OK JPG /mnt/sdcard/images/uvc_<timestamp>_<serial>.jpg PNG /mnt/sdcard/images/uvc_<timestamp>_<serial>.png` |
| Qt save request | `sendOverlayCommand(QStringLiteral("SAVE_DUAL ") + m_imageDir)` |
| Board upload helper | `/root/qt_camera_display/defect-cos-upload --jpg <local.jpg> --png <local.png>` |
| Upload parser self-test | `sh defect-cos-upload --self-test-json-parser` |
| Default upload account config | `/root/qt_camera_display/cos-upload.env` or `CLOUD_UPLOAD_ENV_FILE=<absolute-board-path>` |
| COS prepare API | `POST /api/v1/uploads/cos/prepare` with `record_id`, `file_kind`, `file_name`, `content_type` |
| File registration API | `POST /api/v1/records/{record_id}/files` with `file_kind`, `storage_provider`, `bucket_name`, `region`, `object_key`, `content_type`, `size_bytes`, `etag`, optional timezone-qualified `uploaded_at` |

### Contracts

| Area | Contract |
|---|---|
| Frame identity | JPG and PNG must be encoded from the same copied RGB24 frame buffer so the two cloud files describe the same camera moment. |
| File formats | JPG uses `image/jpeg`; PNG uses `image/png`. Both files must be written through a temporary file, flushed with `fsync`, closed, and renamed before success is reported. |
| SD-card boundary | Save output must stay under `/mnt/sdcard`; the controller must check `/proc/mounts` before asking the overlay to write. |
| Overlay compatibility | Keep legacy `SAVE <dir>` PPM support only as a compatibility path. The Qt save button must use `SAVE_DUAL`. |
| Cloud file_kind mapping | The backend enum is `source`, `annotated`, `thumbnail`. Map JPG to `source` and PNG to `annotated`; do not invent `source_jpg`, `source_png`, or other values. |
| Authentication | `defect-cos-upload` reuses `CLOUD_COOKIE_FILE` when present. If no cookie exists, it first reads the local private env file from `CLOUD_UPLOAD_ENV_FILE` or `/root/qt_camera_display/cos-upload.env`, then may log in with `CLOUD_ACCOUNT` and `CLOUD_PASSWORD`; never hard-code credentials in source. Runtime environment variables override the local env file for temporary account or ID changes. |
| Local secret file | `cos-upload.env` is a board/rootfs-only file and must not be committed. Deploy it with owner `root:root` and mode `600`; at minimum it contains `CLOUD_ACCOUNT='<account>'` and `CLOUD_PASSWORD='<password>'`. |
| Part/device IDs | `CLOUD_PART_ID` and `CLOUD_DEVICE_ID` may override the target cloud objects. If left empty, the script must query the current account's first accessible part and device instead of assuming global ID `1`. |
| Created record parsing | The script must parse the top-level `id` and `record_no` returned by `POST /api/v1/records`. Do not use a generic greedy `"id"` extractor for the create-record response, because nested `part.id`, `device.id`, or `files[].id` can point at old/demo objects such as `record_id=3`. |
| Record ID propagation | The `record_id` returned by create-record is the only valid value for both `POST /api/v1/uploads/cos/prepare` and `POST /api/v1/records/{record_id}/files` during that save action. |
| COS object path guard | The prepare response `object_key` must include the current `record_no`; if it still contains an older record name such as `SIM-DIANPIAN-20260420185013`, stop before COS PUT or file registration. |
| Record isolation | One `保存图片` action must create or select a cloud record that contains only that action's JPG `source` and PNG `annotated` files. Do not treat upload transport success as complete if the backend reuses a historical/demo `record_id` and the detail page accumulates older images; the helper must fail when the returned detail file count is not exactly 2. |
| Upload timestamps | Board-side `captured_at`, `detected_at`, and any `uploaded_at` sent by the helper should include an explicit timezone offset such as `+08:00`; otherwise UTC fallback values can be stored without timezone and displayed as 8 hours earlier. |
| Error propagation | Local save failures should return `保存失败：...`; upload failures should preserve the local save success and append `上传失败：...` with the first actionable script line. |

### Validation & Error Matrix

| Check | Good Result | Failure Meaning |
|---|---|---|
| Static contract | `./test_qt_kms_overlay_assets.sh` finds `SAVE_DUAL`, libjpeg/libpng, `defect-cos-upload`, `source`, `annotated`, prepare, and file registration paths | The save/upload contract drifted between Qt, overlay, script, or docs |
| Overlay build | `./build_uvc_kms_overlay.sh` links `-ljpeg -lpng -lz` and outputs an ARM ELF | Target sysroot lacks image libraries or C code no longer compiles |
| Qt build | `./build_qt_camera_display.sh` outputs an ARM ELF | The save controller or QProcess path no longer compiles under Qt 5.12 |
| Local files | Newest `.jpg` and `.png` both exist, are nonempty, and keep the same size across two `stat` checks | The overlay write/rename/fsync path is incomplete or SD card is unstable |
| Headers | `head -c 2 "$jpg"` is JPEG SOI and `head -c 8 "$png" \| hexdump -C` is the PNG signature | Encoder output or file selection is wrong |
| COS upload | `defect-cos-upload` prints `上传成功：record_id=... jpg_kind=source png_kind=annotated` | Login, prepare, PUT, ETag extraction, or file registration failed |
| JSON parser self-test | `sh defect-cos-upload --self-test-json-parser` passes with a fixture whose top-level `record_id=8` and nested `part.id/device.id=3` | Create-record parsing can regress and bind new images to old records |
| Prepare object key | `prepare_object_key` in script stderr contains the current `record_no` | Prepare used a stale `record_id` or backend returned a path for the wrong record |
| Register URL | Script stderr prints `register_url=/api/v1/records/<created_record_id>/files` for both `source` and `annotated` | File registration can attach images to a historical record even when COS PUT succeeds |
| Default account config | Running without inline `CLOUD_ACCOUNT/CLOUD_PASSWORD` succeeds when `/root/qt_camera_display/cos-upload.env` exists with mode `600` | The board default account was not deployed, cannot be read, or contains wrong credentials |
| ID discovery | Running without `CLOUD_PART_ID/CLOUD_DEVICE_ID` succeeds for an account that has at least one part and device | Script is still relying on fixed IDs or cannot access `/parts` and `/devices` |
| Record detail | `GET /api/v1/records/{record_id}` returns files with `source` and `annotated`, each with `preview_url` | Files reached COS but were not registered, or the backend cannot build preview URLs |
| Record isolation | The returned record detail for a single save contains exactly the current JPG and PNG pair, not a growing list such as 9 historical files under `record_id=3`; otherwise the helper exits non-zero | The script or backend is reusing an old/demo record number or the backend de-duplicates records unexpectedly |

### Good / Base / Bad Cases

```cpp
/* Good: Qt asks the overlay for two browser-previewable files and leaves encoding to the process that owns the real camera frame. */
result = sendOverlayCommand(QStringLiteral("SAVE_DUAL ") + m_imageDir);
```

```sh
# Base: upload a previously saved pair by hand to isolate cloud/COS failures from UI touch failures.
/root/qt_camera_display/defect-cos-upload --jpg "$jpg" --png "$png"
```

```cpp
/* Bad: saving a Qt screenshot can miss the external KMS overlay plane and produce UI chrome instead of the camera frame. */
QQuickWindow::grabWindow();
```

```sh
# Bad: these file_kind values are not accepted by the current backend enum.
file_kind=source_jpg
file_kind=source_png
```

```text
Bad: `record_id=3` returns preview_url for the new JPG/PNG, but the detail page also shows old images from earlier uploads, so the single-save evidence set grows to 9 files.
```

```text
Correct: one save action creates an isolated evidence set; the detail page for that returned record shows only the current JPG source and PNG annotated files.
```

```sh
# Bad: a generic id extractor can return the nested part/device id instead of the new record id.
record_id=$(extract_json_number "id" "/tmp/defect-cos-record.json")
```

```sh
# Correct: create-record responses use a dedicated top-level parser, then reuse the returned id everywhere.
record_id=$(extract_created_record_id "/tmp/defect-cos-record.json")
prepare_upload "$record_id" "$created_record_no" "source" "$jpg_file" "image/jpeg"
register_file "$record_id" "source" "$jpg_file" "image/jpeg" "$SOURCE_HEADERS"
```

### Tests Required

- Run `./test_qt_kms_overlay_assets.sh` after changing the Qt save, overlay save, upload script, deployment script, or README contract.
- Run `sh defect-cos-upload --self-test-json-parser` after changing JSON parsing or create-record handling.
- Run `sh -n defect-cos-upload` and `sh -n` on changed shell scripts.
- Cross-build `uvc_kms_overlay` in `cfr-vm` and confirm the output is an ARM ELF with no compiler warnings.
- Cross-build `qt_camera_display` through the ST Qt SDK and confirm the output is an ARM ELF.
- Deploy the local-only account config with `CLOUD_ACCOUNT` and `CLOUD_PASSWORD` set in the deploy shell, then confirm the board file is `600` and the upload helper works without inline credentials.
- On the board, verify `/mnt/sdcard` is mounted, run `--storage-self-test`, inspect the newest JPG/PNG headers and stable file sizes, then run the COS upload helper with a real cookie or credentials.
- After a successful upload, query the record detail API and assert both `source` and `annotated` file objects have preview URLs.
- Check the helper stderr for `created_record_id`, `prepare_object_key`, and two `register_url` lines; both register URLs must use the created record id, and each object key must contain the returned `record_no`.
- Assert the returned record detail contains only the current save's two files. If it contains older files, mark the feature incomplete even if COS `PUT` and `preview_url` both succeeded.

### Wrong vs Correct

#### Wrong

```text
The board uploaded a PPM file to COS, so the feature is done.
```

This ignores the browser/cloud preview requirement and can leave the detail page unable to render the image.

#### Correct

```text
The board saved a JPG/PNG pair from the same overlay frame, registered JPG as source and PNG as annotated, and the cloud record detail returns preview_url for both.
```

---

## Scenario: MP157 Auto Vision LOCATE Reacquisition Contract

Use this convention when the STM32MP157 Qt automatic flow asks `uvc_kms_overlay` to locate or reacquire a washer-like part before sending `VISION_POS`, stopping the conveyor, or starting ROI fine tuning.

### 1. Scope / Trigger

- Trigger: changing `20_uvc_camera/qt_camera_display/uvc_kms_overlay.c` around `LOCATE`, `locate_part_in_yuyv_frame()`, candidate filtering, brightness thresholds, ring-hole detection, or the `OK LOCATE ...` reply.
- Trigger: changing `20_uvc_camera/qt_camera_display/main.cpp` around `handleAutoVisionLocateReply()` or `sendF4VisionPosition()`.
- Trigger: changing `20_uvc_camera/qt_camera_display/qml/Main.qml` around `handleAutoVisionLocateFinished()`, first-detect confirmation, auto vision bottom status, or ROI fine-tune locate handling.
- Current target family: wave washer, flat washer, and split washer. These are ring/hole parts. Lighting can temporarily hide the hole, so a high-confidence no-ring candidate may be used only after this cycle has already established a real target; first detection defaults to ring evidence to avoid conveyor false positives.

### 2. Signatures

| Boundary | Signature / Marker |
|---|---|
| Overlay socket command | `LOCATE` sent to `/tmp/uvc-kms-overlay-control.sock` |
| Overlay success reply | `OK LOCATE has_target=<0|1> frame_id=<n> width=<w> height=<h> center_x=<x> center_y=<y> bbox_x=<x> bbox_y=<y> bbox_w=<w> bbox_h=<h> confidence=<0..100> ring=<0|1> diag=<code> roi_y=<y> roi_h=<h> thr=<dark>,<body>,<bright> cand_box=<w>x<h> cand_area=<px> cand_density=<pct> cand_conf=<0..100> cand_ring=<0|1>` |
| Overlay no-hard-gate marker | `#define AUTO_LOCATE_REQUIRE_RING_HOLE_FOR_TARGET 0U` in `uvc_kms_overlay.c` |
| Metal body expansion marker | `AUTO_LOCATE_MIN_BODY_LUMA_DELTA`, `AUTO_LOCATE_BODY_LUMA_DELTA_PERCENT`, and `auto_locate_body_threshold_from_delta()` |
| Oversized candidate refine marker | `AUTO_LOCATE_OVERSIZE_REFINE_MAX_SIDE` and `auto_locate_refine_oversized_ring_candidate()` in `uvc_kms_overlay.c` |
| Diagnostic marker | `LOCATE_DIAG_*` and `auto_locate_record_reject_candidate()` in `uvc_kms_overlay.c` |
| Candidate structure marker | `struct locate_result` includes `unsigned int has_ring_hole` |
| Qt parser marker | `result.insert(QStringLiteral("has_ring"), tokenValue(reply, QStringLiteral("ring")).toInt())` |
| QML first-detect policy marker | `autoVisionFirstDetectRequiredFramesForCandidate(hasRing, confidence)` |
| QML no-ring first-detect guard marker | `autoVisionAllowNonRingFirstDetect: false`, `autoVisionFirstDetectNonRingConfirmRequired`, and `autoVisionFirstDetectMinNonRingConfidence` |
| QML operator marker | Bottom status includes `box=<w>x<h> ring=<0|1>` and `diag/cand/cconf/cring/cdens` |
| QML bbox limit marker | `property int autoVisionExpectedPartMaxBboxArea: 45000` |
| Static contract | `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh` checks the markers and rejects enabling first-detect no-ring by default |

### 3. Contracts

| Area | Contract |
|---|---|
| Source frame | `LOCATE` must analyze the raw `latest_frame.yuyv_map`, not the decorated Qt/KMS framebuffer. Display-only ROI graphics must not influence target selection. |
| Search region | The horizontal search width remains aligned to the model ROI width; the vertical region should be the detected black conveyor band or a conservative center fallback, not the entire frame when belt detection fails. |
| Candidate growth | `bright_threshold` starts a component and `body_threshold` expands it. Do not start components from `body_threshold` alone, or gray belt texture can become a candidate; do not expand only with `bright_threshold`, or shadowed washer arcs can be split and fail ring detection. |
| Oversized candidate handling | A bbox larger than `AUTO_LOCATE_MAX_BBOX_SIDE` is not automatically "no part." It can be a real washer connected to a white support, highlight, or bright background. Before returning `LOCATE_DIAG_BBOX`, the overlay must try a local ring-hole refinement inside the oversized component, cap the refined bbox within QML's area budget, and recompute area/density/confidence from the refined window. If no four-side ring support is found, then reject with `diag=3`. |
| Candidate class | For the current washer-like part set, ring evidence is still the strongest signal, but it must not be the only signal at every stage. A real washer can become `ring=0` when exposure, highlight, shadow, or blur hides the center hole. |
| No-ring handling | `AUTO_LOCATE_REQUIRE_RING_HOLE_FOR_TARGET` defaults to `0U`. The overlay may return a no-ring candidate only after stricter bbox, size, and confidence gates. QML defaults `autoVisionAllowNonRingFirstDetect` to `false`, so a no-ring candidate cannot establish the first target; after `autoVisionHasSeenTarget` is true, no-ring candidates may help hold or reacquire the same part. |
| Reply compatibility | Existing fields keep their names and units. Adding `ring=<0|1>` is additive; Qt must parse missing or malformed `ring` as `0`, not crash. |
| Debuggability | `LOCATE` replies must keep diagnostic fields even when `has_target=0`, because field failures often occur while the part is moving and cannot be reconstructed from logs alone. Extra fields must remain additive so older QML parsers keep working. |
| QML filtering | QML keeps confidence, bbox area, and multi-frame confirmation as the second defense. For first detection, `ring=1` uses the normal confirmation frame count. `ring=0` returns zero confirmation frames while `autoVisionAllowNonRingFirstDetect` is false; that default must stay false for board operation because the conveyor is a stable background. |
| QML bbox budget | The QML bbox max area must fit real washer boxes observed from overlay. With the current 640x480 camera setup, a real washer can return about `173x173` (`~29929 px²`), so the max area contract is `45000`, not the earlier `12000`. |
| Operator diagnostics | When a target is accepted or rejected, the bottom status must show `confidence`, `box`, `ring`, and `diag/cand/cconf/cring/cdens` so field testing can distinguish a washer outline from belt edge, white support artifacts, or a hidden center hole. |
| F4 protocol boundary | `ring` is not sent to F4 in `VISION_POS`; it is an MP157-side validation/debug field. Do not change the F4 payload unless the MP157-F407 protocol document is updated. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning | Required Action |
|---|---|---|---|
| Overlay no hard ring gate | `./test_qt_kms_overlay_assets.sh` finds `AUTO_LOCATE_REQUIRE_RING_HOLE_FOR_TARGET 0U` and rejects `require_ring_hole_for_target > 0U && !has_ring` | A real washer can disappear permanently when the center hole is hidden by lighting | Keep overlay no-ring fallback, while retaining stricter bbox, size, and confidence gates |
| First-detect no-ring guard | Static test finds `autoVisionAllowNonRingFirstDetect: false`, `autoVisionFirstDetectRequiredFramesForCandidate`, `autoVisionFirstDetectNonRingConfirmRequired`, and `autoVisionFirstDetectMinNonRingConfidence` | If enabled by default, the stable black conveyor can satisfy multi-frame confirmation and become a false part | Keep first-detect no-ring disabled by default; use no-ring fallback only after the current cycle has seen a real target |
| Body expansion contract | Static test finds `auto_locate_body_threshold_from_delta`, a `start_luma, bright_threshold` seed check, a `next_luma, body_threshold` neighbor expansion check, and ring detection using `body_threshold` | Real washers under uneven light can be split into several bright arcs and never produce `ring=1` | Restore high-brightness seed plus lower metal-body expansion; do not require ring as the only final signal |
| Oversized bbox refinement | Static test finds `AUTO_LOCATE_OVERSIZE_REFINE_MAX_SIDE`, `auto_locate_refine_oversized_ring_candidate`, and a bbox-filter block that calls the helper before `LOCATE_DIAG_BBOX` | A visible washer can be rejected before ring detection with `diag=3 cand_box=300x261` because it touched a white support or highlight | Restore local ring refinement before the bbox reject path; do not solve this by simply raising the max bbox to the full ROI |
| Locate diagnostic reply | Static test finds `diag=%u`, `roi_y=%u roi_h=%u`, `thr=%u,%u,%u`, `cand_box=%dx%d`, `LOCATE_DIAG_RING`, and `auto_locate_record_reject_candidate` | A moving washer can fail in the field with no evidence of which gate rejected it | Restore additive diagnostic fields and candidate rejection tracking |
| QML bbox max | Static test finds `property int autoVisionExpectedPartMaxBboxArea: 45000` | Overlay can return a valid `173x173 ring=1` washer while QML turns it back into `VISION_LOST` | Restore the max area budget or replace it with a ring-aware QML filter backed by board evidence |
| Reply parser | Static test finds the `ring` parser in `main.cpp` and `ring=` display in QML | The overlay may return ring evidence but the UI cannot show or validate it | Parse `ring` into `has_ring` and display it in the auto vision status |
| Empty conveyor board test | With no part on the conveyor, repeated `LOCATE` does not stably return `has_target=1` | Belt edge, white support, or fixed reflection is being accepted as a part | Capture `box/ring/conf`, inspect the raw scene, and tighten the overlay candidate filter before tuning F4 motion |
| Real washer board test | A washer fully entering the ROI returns `has_target=1` with a plausible `bbox_w/bbox_h`; first detection should establish from `ring=1`, and after that high-confidence `ring=0` candidates may hold/reacquire the same part | The ring-hole detector is too strict, lighting hides the hole, or the part never reaches a ring-visible frame | Check exposure/lighting and ROI placement, then inspect `diag/cand/cconf/cring/cdens` before adjusting thresholds |
| QML motion boundary | `VISION_POS` is sent only after overlay accepted `has_target=1` and QML confirmation passes | QML can still drive conveyor movement from a rejected or malformed locate result | Keep QML `hasTarget` filtering, bbox checks, and first-detect confirmation active |
| Deployment proof | Board `strings /root/qt_camera_display/qt_camera_display` shows `ring=` or the QML status marker, and board `uvc_kms_overlay` contains `AUTO_LOCATE_REQUIRE_RING_HOLE_FOR_TARGET` marker if not stripped | Source was changed locally but old board binaries are still running | Rebuild on VM, deploy both binaries, restart the display stack, and verify with `strings` or checksums |

### 5. Good / Base / Bad Cases

```text
Good: A real washer candidate has `ring=1`, a bbox close to the physical part, and QML enters normal first-detect confirmation.
```

```text
Good: A bright washer highlight starts the component, then adjacent dimmer silver pixels expand the same bbox so the center hole can be sampled reliably.
```

```text
Good: An oversized `cand_box=300x261` first attempts local ring refinement and returns a <=210x210 washer-local bbox only if the dark center has metal support on all four sides.
```

```text
Base: A partial washer entering from the top may return `ring=0`; before the cycle has seen a real target, QML keeps waiting instead of entering tracking from that no-ring candidate.
```

```text
Bad: Raising `AUTO_LOCATE_MAX_BBOX_SIDE` to the full 300px ROI and returning the whole connected support/background blob as the target.
```

```text
Bad: A large no-ring white support or conveyor reflection reaches `confidence=100` and enters `TRACKING` because first-detect no-ring was enabled by default.
```

```cpp
/* Good: no-ring candidates are stricter, but not globally rejected. */
if (!has_ring &&
    (bbox_w < AUTO_LOCATE_RING_REQUIRED_BBOX_SIDE ||
     bbox_h < AUTO_LOCATE_RING_REQUIRED_BBOX_SIDE ||
     confidence < AUTO_LOCATE_MIN_NON_RING_CONFIDENCE)) {
    continue;
}
```

```qml
// Good: ring=1 confirms quickly; ring=0 first-detect is disabled by default.
firstDetectConfirmRequired = autoVisionFirstDetectRequiredFramesForCandidate(hasRing, confidence)
if (firstDetectConfirmRequired <= 0) {
    hasTarget = false
}
```

```cpp
/* Good: high threshold starts the component, lower body threshold only expands from that seed. */
if (!auto_locate_is_bright_candidate_luma(start_luma, bright_threshold)) {
    if (!auto_locate_is_bright_candidate_luma(start_luma, body_threshold)) {
        visited[start_index] = 1U;
    }
    continue;
}
```

```qml
/* Bad: first detection allows no-ring candidates by default. */
property bool autoVisionAllowNonRingFirstDetect: true
```

```qml
/* Good: first detection can only use no-ring if a maintainer deliberately opens the debug gate. */
property bool autoVisionAllowNonRingFirstDetect: false

if (!autoVisionAllowNonRingFirstDetect) {
    return 0
}
```

### 6. Tests Required

- Run `cd 20_uvc_camera/qt_camera_display && ./test_qt_kms_overlay_assets.sh` after any `LOCATE`, overlay reply, Qt parser, QML auto vision, or README contract change.
- Run `git diff --check` after editing C++, C, QML, shell, Markdown, or spec files.
- Cross-build `uvc_kms_overlay` in `cfr-vm` and confirm the output is an ARM ELF before board deployment.
- Cross-build `qt_camera_display` after QML or C++ parser changes, because `Main.qml` is embedded through Qt resources.
- For `diag=3` field failures, verify that the oversized-bbox path tries local ring refinement before rejection and keeps the refined bbox within QML's max-area budget.
- On the board, verify an empty conveyor does not repeatedly show `TRACKING`, verify first detection proceeds from `ring=1`, and verify already-seen targets can survive short no-ring drops without sending `VISION_LOST reason=1`.
- When board `nc -U` is available, run `printf 'LOCATE\n' | nc -U /tmp/uvc-kms-overlay-control.sock`; otherwise use the home-page bottom status and overlay logs.

### 7. Wrong vs Correct

#### Wrong

```text
The bottom status shows `conf=100`, so it must be a real part.
```

High confidence can come from large fixed bright artifacts. For washer-like parts, ring evidence is required to establish the first target in normal board operation; no-ring recovery is only for an already-seen target.

#### Correct

```text
The bottom status shows a plausible bbox and `ring=1` before the first tracking state. If a later frame becomes `ring=0`, QML can keep/reacquire the already-seen target without treating an empty conveyor as a new part.
```

---

## Qt Detection Result Payload Contract

Use this convention when the STM32MP157 Qt defect screen runs the model detection chain, renders the latest result on the home page, writes local history, or sends a detection record to the defect cloud backend.

### 1. Scope / Trigger

- Trigger: changing `20_uvc_camera/qt_camera_display/main.cpp`, `20_uvc_camera/qt_camera_display/qml/Main.qml`, `20_uvc_camera/qt_camera_display/defect-cos-upload`, history JSON parsing/writing, README acceptance text, or cloud record payload mapping for a detection action.
- Trigger: touching any nearby save/upload code that already contains placeholder values, fixed result strings, fixed part labels, fixed IDs, or old display scaling. When a file is already being modified in this area, remove obsolete fixed fields in the same change instead of leaving them for a later pass.
- Transaction boundary: one `检测` action captures one source frame, runs the classifier first, runs the segmentation model second, computes the total model time after both models finish, stores one local history entry, and sends one cloud record payload with the actual model result.

### 2. Signatures

| Operation | Signature |
|---|---|
| Board self-test | `/root/qt_camera_display/qt_camera_display --detect-self-test` |
| Detection result line | `RESULT status=<GOOD|BAD> class=<model_class> ... segment_status=<OK|NG> defect_pixels=<n> fused_status=<GOOD|BAD|REVIEW> fused_result=<good|bad|review> fused_reason=<text> segment_time_ms=<ms> total_time_ms=<ms> upload_status=<OK|FAIL>` |
| First-model UI signal | `void detectClassificationReady(const QString &resultText)` |
| All-models UI signal | `void detectModelsReady(const QString &resultText)` |
| QML first-model handler | `onDetectClassificationReady: updateDetectClassificationFields(resultText)` |
| QML all-models handler | `onDetectModelsReady: updateDetectClassificationFields(resultText); updateDetectFusedFields(resultText); updateDetectModelTimeFields(resultText)` |
| Model-fusion function | `fusedResultFromModelResults(const QString &classificationResult, const QString &segmentationResult) -> FusedDetectResult` |
| Fused-to-cloud mapping | `cloudResultFromFusedResult(const FusedDetectResult &fusedResult) -> good|bad|review` |
| Fused-to-history mapping | `historyTextFromFusedResult(const FusedDetectResult &fusedResult) -> 良品|待复核` |
| Upload environment field | `CLOUD_RESULT=good|bad|review` |
| Upload result validator | `validate_cloud_result "$CLOUD_RESULT"` |
| Upload success helper | `isUploadStatusSuccess(QString uploadStatus)` in `main.cpp` |
| Upload failure helper | `isUploadStatusFailure(rawStatus)` in `Main.qml` |
| Cloud create-record field | JSON payload field `"result":"good|bad|review"` |
| Local history fields | `classification_result`, `segmentation_result`, `total_time_ms`, `source_path`, `annotated_images` |
| Home-page display fields | model-derived part name, percent confidence on a 0-100 scale, and `total_time_ms` when present |
| Cloud detail verification | `GET /api/v1/records/{record_id}` must return `result` and `effective_result` matching the model outcome |

### 3. Contracts

| Area | Contract |
|---|---|
| Result source of truth | `records.result` must come from the fused classifier + UNet result for the same detection transaction. Map classifier `BAD` or UNet `NG/defect_pixels>0` to cloud `bad`; map `good` only when the classifier is `GOOD` and UNet reports no defect; map incomplete or unknown model evidence to `review`. Never default a model-backed detection to `good`. |
| Upload default | `defect-cos-upload` may use `review` as the conservative default for manual diagnostics without a model result. It must not use `good` as a fallback default, because that turns missing data into a false pass. |
| Result validation | The upload helper must reject any `CLOUD_RESULT` outside `good`, `bad`, and `review` before create-record. Invalid values should fail locally and not create a misleading cloud record. |
| Placeholder cleanup | When editing the detection/upload/history/QML chain, search the touched files for fixed payload values such as hard-coded `good`, `待接入`, demo IDs, fixed part names, fixed result text, and old confidence scaling. Replace them with model-derived or explicitly conservative values. |
| Part display | The home page part name must be derived from the model class or backend part field for the current record. For class names such as `washer_bad` or `gasket_good`, strip only the quality suffix and display the remaining part token. Do not keep a fixed part label. |
| Confidence display | The home page confidence must be rendered as a 0-100 percentage. Do not divide confidence by `1000` or show a permille-style value unless the upstream model contract explicitly changes. |
| Progressive result display | The home page must show each model stage as soon as that stage has a complete result. After the first classifier returns `RESULT`, QML must immediately refresh part name, class name, classifier tendency, confidence, and good/bad totals, but the main pass/fail banner must stay in a waiting/review style until fusion finishes. It must not wait for segmentation, COS upload, or history append to show classifier details. |
| Final fused display | After the last local model returns, QML must apply `fused_status/fused_reason` over the first classifier status. If the classifier says `GOOD` but UNet reports `NG` or a positive `defect_pixels`, the home page must show a bad/review-style final state, not a good state. |
| Progressive time display | `total_time_ms` must appear when the last model in the local model chain finishes. For the current classifier + UNet chain, emit `detectModelsReady` after UNet returns and before COS upload starts. Do not wait for `upload_status=OK/FAIL` to show the model elapsed time. |
| Final completion boundary | `detectCurrentFrameFinished` means the whole detect transaction finished, including upload attempt and history append eligibility. It should restore busy state and show final upload status, but it must not be the first moment when model result fields become visible. |
| Total detection time | `total_time_ms` is measured from classifier start through segmentation completion. It must include both model runtimes and exclude COS upload time unless the field name is changed to an upload-inclusive metric. |
| History/cloud parity | Local history and the cloud record must describe the same source frame, model class, status, annotated evidence, and total model time. UI success is incomplete until cloud detail round-trip confirms the same result fields. |
| Upload success source of truth | Board UI, history, retry, alarms, and statistics must classify upload state through `upload_status=OK|FAIL|SKIP` or the shared success/failure helpers. Do not infer failure from a diagnostic string that merely contains Chinese `失败` after an `upload_status=OK` result. |
| Post-upload verification warning | After create-record, COS prepare, PUT, and file registration have all succeeded, a later detail round-trip failure is a verification warning, not an upload failure. `defect-cos-upload` should keep stdout successful with `upload_status=OK record_id=... record_no=... verify_status=warning`, while stderr preserves the warning for troubleshooting. |
| Legacy save paths | Old manual save or diagnostic paths that do not run the models may upload evidence only as `review` or mark it as local-only. They must not create a cloud `good` record just because image upload succeeded. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning |
|---|---|---|
| Static model-to-cloud markers | `./test_qt_kms_overlay_assets.sh` finds `fusedResultFromModelResults`, `cloudResultFromFusedResult`, `historyTextFromFusedResult`, `CLOUD_RESULT`, `validate_cloud_result`, `fused_status`, and `total_time_ms` | Result mapping, validation, or total-time propagation can drift silently |
| Fixed result search | `rg -n 'CLOUD_RESULT=.*good|"result":"good"|result=good|待接入|固定|/1000' main.cpp qml/Main.qml defect-cos-upload README.md` has no unreviewed detection payload defaults | A touched path may still send placeholder content or old confidence scaling |
| Upload helper validation | `CLOUD_RESULT=bad sh defect-cos-upload ...` creates a `bad` payload; `CLOUD_RESULT=badness sh defect-cos-upload ...` fails before create-record | Invalid or missing result values can become cloud records |
| Upload status interpretation | A helper line with `上传成功：upload_status=OK ... verify_status=warning` displays as upload success in history/statistics; a line with `upload_status=FAIL` displays as failure | UI logic is still scanning raw text for `失败` instead of parsing status tokens |
| Board self-test BAD case | `--detect-self-test` can produce `fused_result=bad ... total_time_ms=<nonzero> upload_status=OK` when either the classifier is `BAD` or UNet reports defects | The classifier result, segmentation result, timing, fusion, or upload path is not wired together |
| Cloud detail BAD round-trip | For the returned `record_id`, detail JSON contains `"result":"bad"` and `"effective_result":"bad"` | The board sent a fixed/default good payload or the backend interpreted it incorrectly |
| Home part display | A class such as `washer_bad` displays part `washer` on the home page | The UI still shows a fixed part name or exposes quality suffix as part identity |
| Home confidence display | A confidence value renders on a 0-100 percent scale and no QML `/1000` scaling remains | The UI still uses the old permille contract |
| First-model display timing | Board binary contains `detectClassificationReady`; on the LCD, part/class/classifier tendency/confidence update immediately after classifier completion while the main banner still says waiting for fusion | The UI is tied to final upload completion or all-model completion, or it prematurely shows classifier GOOD as final good |
| All-model time display timing | Board binary contains `detectModelsReady`; on the LCD, `total_time_ms` updates after UNet completion before COS upload returns | The elapsed-time UI is tied to `detectCurrentFrameFinished` and waits for the network |
| Total time display | Home page prefers `total_time_ms` over the first model's elapsed time | The operator sees only classifier latency instead of the complete detection latency |

### 5. Good / Base / Bad Cases

```cpp
/* Good: the cloud payload is derived from both model outputs for this detection transaction. */
const FusedDetectResult fusedResult = fusedResultFromModelResults(classificationResult, segmentationResult);
const QString cloudResult = cloudResultFromFusedResult(fusedResult);
env.insert(QStringLiteral("CLOUD_RESULT"), cloudResult);
```

```sh
# Base: manual diagnostics without a classifier result are conservative review records, not pass records.
CLOUD_RESULT="${CLOUD_RESULT:-review}"
validate_cloud_result "$CLOUD_RESULT"
```

```qml
// Good: display the model-derived part token and percent confidence from the current result.
text: displayPartNameFromClass(root.detectClassName)
```

```qml
// Good: first-stage model fields update when the classifier emits RESULT, before upload finishes.
onDetectClassificationReady: {
    updateDetectClassificationFields(resultText)
}
```

```qml
// Good: total model time updates after the local model chain finishes, before COS upload returns.
onDetectModelsReady: {
    updateDetectClassificationFields(resultText)
    updateDetectFusedFields(resultText)
    updateDetectModelTimeFields(resultText)
}
```

```text
Bad: the classifier reports GOOD, UNet reports `segment_status=NG`, but the upload helper creates `"result":"good"` because the Qt controller only used the first model.
```

```text
Bad: the home page says the detection took only the classifier time even though the segmentation model is still running afterward.
```

```text
Bad: the classifier result is already known, but the home page still shows "当前帧" or "检测中..." for part/class until COS upload returns.
```

```text
Bad: source and annotated files were already uploaded and registered, but the final detail check timed out, so the board history records `上传失败` even though the cloud page shows the new record.
```

### 6. Tests Required

- Run `./test_qt_kms_overlay_assets.sh` after changing detection, result display, history, upload, or README contracts.
- Run `sh -n defect-cos-upload` after changing the upload helper, then test at least one accepted `CLOUD_RESULT` and one rejected value.
- Run `sh defect-cos-upload --self-test-json-parser` and `sh defect-cos-upload --self-test-args` after changing upload status parsing, argument parsing, create-record parsing, or post-upload verification handling.
- Search touched files for old fixed payload/display markers: `CLOUD_RESULT`, hard-coded `good`, fixed part names, fixed IDs, placeholder text such as `待接入`, and confidence `/1000`.
- Cross-build `qt_camera_display` in `cfr-vm` and confirm the ARM binary contains the expected detection markers such as `fusedResultFromModelResults`, `cloudResultFromFusedResult`, `CLOUD_RESULT`, `fused_status`, `total_time_ms`, `detectClassificationReady`, and `detectModelsReady`.
- On the board, run `--detect-self-test` and assert the result line includes `classification_result`, `segmentation_result`, nonzero `total_time_ms`, and `upload_status=OK` when network credentials are available.
- For at least one BAD detection acceptance test and one classifier-GOOD/UNet-NG conflict case, query the returned cloud detail and assert both `result` and `effective_result` are `bad`. Do not accept the feature based only on upload stdout.
- On the LCD, verify the home page shows the model-derived part name, 0-100 percent confidence, and total two-model detection time without overlapping controls.
- On the LCD, verify timing explicitly: part/class/classifier tendency/confidence appear after the first classifier finishes while the main banner waits for fusion; `total_time_ms` and final good/bad/review appear after the final local model finishes; upload completion only changes final status/history.

### 7. Wrong vs Correct

#### Wrong

```text
The upload succeeded, so sending the helper's default `"result":"good"` is acceptable even when the model classified the part as BAD.
```

#### Correct

```text
The classifier and UNet results are fused first. If either model detects a defect, Qt passes `CLOUD_RESULT=bad`, the helper validates that value, create-record sends `"result":"bad"`, and cloud detail returns `result/effective_result=bad` for the same `record_id`.
```

#### Wrong

```text
Only fix the QML display bug and leave nearby fixed cloud payload fields untouched because they were pre-existing.
```

#### Correct

```text
When editing the detection/upload/history files, search and remove stale placeholder fields in the touched path so UI, history, and cloud all report the same model-backed result.
```

#### Wrong

```text
The final `detectCurrentFrameFinished` signal already contains every field, so it is fine to update the home page only after COS upload returns.
```

#### Correct

```text
Emit `detectClassificationReady` after the first model returns and update part/class/confidence immediately; emit `detectModelsReady` after the last local model returns and update `total_time_ms`; reserve `detectCurrentFrameFinished` for final upload status and busy-state reset.
```

---

## Scenario: MP157 Dynamic Classification Model Replacement Contract

### 1. Scope / Trigger

- Trigger: replacing the MobileNetV3-Small classification ONNX or labels used by `20_uvc_camera/qt_camera_display/defect_classify.cpp`.
- Trigger: changing the number or order of classifier labels, quantizing a new classifier, or deploying model files to STM32MP157.
- This contract does not authorize replacing the UNet segmentation model. A classifier-only replacement must prove that the board UNet SHA256 remains unchanged.
- The current six-class UNet is a temporary compatibility model. When the separately trained reduced-class UNet is ready, evaluate, quantize, and deploy it as a separate task; do not infer that classifier class removal has already changed segmentation outputs.

### 2. Signatures

| Boundary | Signature |
|---|---|
| Classifier executable | `/root/qt_camera_display/defect-classify --image <jpg> [--model <onnx>] [--labels <json>]` |
| Stable board model | `/root/qt_camera_display/models/defect_classifier_static_mixed_int8.onnx` |
| Stable board labels | `/root/qt_camera_display/models/defect_classifier_static_mixed_int8_labels.json` |
| ONNX output inspection | `session.GetOutputTypeInfo(0)` and `GetTensorTypeAndShapeInfo().GetShape()` |
| Runtime element inspection | `outputs.front().GetTensorTypeAndShapeInfo().GetElementCount()` |
| Static regression | `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh` |
| Current four-class source | `/home/cfr/linux/model_picture/checkpoints_classify_4classes_v2/defect_classifier_static_mixed_int8.onnx` |
| Current four-class labels | `/home/cfr/linux/model_picture/checkpoints_classify_4classes_v2/defect_classifier_static_mixed_int8_labels.json` |
| Immutable segmentation model | `/root/qt_camera_display/models/defect_unet_test_decoder_head_int8.onnx` |

### 3. Contracts

| Area | Contract |
|---|---|
| Class count source | Do not define a fixed `MODEL_CLASS_COUNT`. Read the positive final dimension from a rank-2 ONNX output shaped `[batch, classes]`. |
| Labels mapping | Parse every `idx_to_class` entry, reject negative/duplicate indexes, and require continuous indexes `0..N-1` with no empty values. |
| Boundary equality | Before inference, require `labels.size() == model_class_count`. After inference with batch size one, require `GetElementCount() == model_class_count`. |
| Quality groups | Every classification label must contain a distinct `good` or `bad` token so probability aggregation cannot silently treat an unknown label as good. |
| Quantization gate | Evaluate FP32 and INT8 on the same complete validation split. Do not deploy when INT8 exact accuracy drops by more than one percentage point or a class has no evaluated samples. |
| Stable destination | New classifier artifacts may replace the contents of the stable board filenames; Qt launch paths and settings do not need a filename migration. |
| Selective deployment | A classifier-only update replaces the classifier model, labels, `defect-classify`, and a relinked Qt binary when required. It must not copy or overwrite the UNet model. |
| Same-ABI model-only update | When input/output types and shapes, preprocessing, label order, helper CLI, and Qt invocation are unchanged, replace only the classifier ONNX and matching labels. Do not rebuild or overwrite Qt, helpers, overlay, or UNet merely because model weights changed. |
| Completion proof | Model generation is not board deployment. Require VM/board SHA256 equality, service restart completion, four representative board inferences, and pre/post UNet SHA256 equality. |
| Current v2 evidence | The v2 FP32/INT8 pair was evaluated on all 187 validation images (`40/40/51/56` per class): both reached 100% argmax and 0.85-threshold accuracy, prediction agreement was 100%, maximum probability delta was 0.007114, and mean probability delta was 0.000378. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning | Required Action |
|---|---|---|---|
| ONNX output shape | Rank 2 and final dimension is positive | Model is not the supported single-output classifier contract | Reject the model before reading logits |
| Labels continuity | Indexes are exactly `0..N-1` | Missing, duplicate, negative, or ambiguous class mapping | Exit nonzero with a labels error |
| Model/labels equality | Example: `model=4`, `labels=4` | Only one artifact was replaced, or labels came from another training run | Exit nonzero; redeploy the matching pair |
| Runtime element count | Batch-one output contains exactly `N` floats | Unexpected output shape or runtime contract drift | Exit nonzero before softmax |
| Quantized accuracy | INT8 loss is at most 1 percentage point versus FP32 | Quantized node scope damages model accuracy | Reduce quantized nodes and repeat calibration/evaluation |
| Board hash | Board classifier/labels hashes equal staged VM files | Old or partial deployment is still active | Stop service, upload `.new`, verify, atomically move, `sync`, restart |
| UNet hash | Pre-deployment hash equals post-deployment hash | Classifier update accidentally changed segmentation | Restore the backed-up UNet immediately and audit the copy command |
| Qt self-test with empty ROI | Fails before models with `current ROI has no part` diagnostics | Physical target is absent; not a classifier failure | Put a supported part in the green ROI; do not bypass the safety gate |

### 5. Good / Base / Bad Cases

```cpp
// Good: derive the count from the ONNX output and validate the labels boundary.
const Ort::TypeInfo output_type_info = session.GetOutputTypeInfo(0);
const auto output_tensor_info = output_type_info.GetTensorTypeAndShapeInfo();
const std::vector<int64_t> output_shape = output_tensor_info.GetShape();
const size_t model_class_count = static_cast<size_t>(output_shape.back());
if (labels.size() != model_class_count) {
    throw std::runtime_error("classification model/labels class count mismatch");
}
```

```text
Base: keep old cloud display/upload mappings for historical records, even when the current classifier no longer emits those old classes.
```

```text
Bad: change `MODEL_CLASS_COUNT = 6` to `MODEL_CLASS_COUNT = 4` and assume future model replacements will remember to edit C++ again.
```

```text
Bad: run the full deployment script for a classifier-only update when it also copies the UNet model, then claim segmentation was unchanged without comparing hashes.
```

### 6. Tests Required

- Run the FP32 and INT8 classifiers on the same complete validation directory and record exact accuracy, class counts, file sizes, and hashes.
- Run `sh ./test_qt_kms_overlay_assets.sh`; it must reject a fixed six-class constant and require output-shape/element-count validation markers.
- Cross-build `defect-classify` with the deployed ONNX Runtime ARM headers. ONNX Runtime 1.17 returns `ConstTensorTypeAndShapeInfo` from a const `TypeInfo`; use `auto` rather than forcing an owned wrapper type.
- On the board, run one known image for every current class and assert the `class=` token and `status=GOOD|BAD` agree with the label suffix.
- On the board, deliberately pass a known mismatched old labels file and assert nonzero exit plus `model=<N> labels=<M>` in the error.
- Record the UNet SHA256 before deployment and assert the same value after service restart.
- Run Qt `--detect-self-test` with a supported physical part inside the ROI. If no part is present, record the ROI-gate failure separately from model execution evidence.

### 7. Wrong vs Correct

#### Wrong

```text
Copy a four-class ONNX over the old model, leave six-class labels and a fixed loop bound in place, then debug the resulting out-of-bounds probabilities on the board.
```

#### Correct

```text
Quantize and evaluate the matching ONNX/labels pair, derive the output class count at runtime, reject any mismatch before softmax, deploy through `.new` files, and prove the unchanged UNet hash after restart.
```

---

## MP157 Cloud Part Auto-Create Upload Contract

Use this convention when the STM32MP157 board uploads a detection record and the model can produce a part class that does not already exist in the cloud `parts` table.

### 1. Scope / Trigger

- Trigger: changing `20_uvc_camera/qt_camera_display/main.cpp`, `20_uvc_camera/qt_camera_display/defect-cos-upload`, retry-upload code, cloud record payload mapping, or README acceptance text for part identity.
- Trigger: the Detect button or history retry reports upload failure before COS prepare/PUT/register, while local source and annotated images already exist on `/mnt/sdcard/images`.
- Root-cause lesson from 2026-05-20: image upload can fail because create-record fails to resolve the part, not because COS or network upload failed. If the board stops after `GET /api/v1/parts?limit=100` with no matching part, the downstream image upload path never starts.
- Goal: a new real part type such as `wave_washer` must create or reuse exactly one cloud part and then upload one source image plus all annotated images for the same record.

### 2. Signatures

| Operation | Signature |
|---|---|
| Qt part extraction | `partCodeFromClassificationResult("... class=wave_washer_good ...") -> "wave_washer"` |
| Upload env from Qt | `CLOUD_PART_CODE=<part_code>` and `CLOUD_CLASS_LABEL=<raw_model_class>` |
| Manual override | `CLOUD_PART_NAME=<display-name>` and `CLOUD_PART_CATEGORY=<category>` |
| Upload command | `/root/qt_camera_display/defect-cos-upload --jpg <source.jpg> --annotated <overlay.jpg> --annotated <mask.png>` |
| Existing part lookup | `GET /api/v1/parts?limit=100` |
| Create record existing-part payload | `POST /api/v1/records` with `"part_id": <id>` |
| Create record auto-create payload | `POST /api/v1/records` with `part_code`, `part_name`, `part_category`, `auto_create_part:true` |
| Auto-create self-test | `sh defect-cos-upload --self-test-json-parser` must run `run_auto_create_part_self_test` |
| Real detail proof | `GET /api/v1/records/<record_id>` must return `part.part_code`, `part.name`, `part.category`, and file counts |

### 3. Contracts

| Area | Contract |
|---|---|
| Part vs result split | Model labels such as `gasket_good`, `gasket_bad`, and `wave_washer_good` contain both part identity and quality result. Strip only the final `_good/_bad/-good/-bad` suffix for `CLOUD_PART_CODE`; send quality through `CLOUD_RESULT`. |
| Existing part priority | If `CLOUD_PART_ID` is a positive number, keep the legacy path and send only `part_id` to create-record. Do not also send auto-create fields. |
| Lookup behavior | Without `CLOUD_PART_ID`, the helper may query `/api/v1/parts?limit=100` to reuse an existing part. A miss is not an upload failure when `CLOUD_PART_CODE` or `CLOUD_CLASS_LABEL` is present. |
| Auto-create payload | On lookup miss, create-record must send top-level `part_code`, `part_name`, optional `part_category`, and `auto_create_part:true`. The helper must not fail with "please create the part first" in this case. |
| Missing identity | If both `part_id` and normalized `part_code` are absent, fail locally before create-record. Do not create a record with the first arbitrary part as a fallback for a model-backed Detect action. |
| Chinese name mapping | `gasket` is a historical training code for the same business object as `wave_washer`; both must map to `part_name=波形垫圈` and `part_category=垫圈类` unless explicitly overridden. `washer` maps to `平垫圈 / 垫圈类`; `splitwasher` maps to `弹性垫圈 / 垫圈类`. Do not translate `wave_washer` as `电平` or display `gasket` as `垫片`. |
| Device context | Always write normalized `device_context.part_code` and raw `device_context.class_label` so cloud debugging can distinguish part identity from model quality labels. |
| Retry parity | History retry must reconstruct the same `CLOUD_PART_CODE`, `CLOUD_CLASS_LABEL`, and `CLOUD_RESULT` from local `classification_result` and `segmentation_result`; retry must not drop the part identity and fall back to an unrelated cloud part. |
| Upload completion proof | Upload is complete only after create-record succeeds, COS prepare/PUT/register succeeds for every file, and record detail contains the expected part plus `source_count=1` and `annotated_count=<input count>`. |

Example auto-create create-record payload:

```json
{
  "record_no": "MP157-20260520-125945",
  "device_id": 3,
  "part_code": "wave_washer",
  "part_name": "波形垫圈",
  "part_category": "垫圈类",
  "auto_create_part": true,
  "result": "good",
  "device_context": {
    "part_code": "wave_washer",
    "class_label": "wave_washer_good"
  },
  "captured_at": "2026-05-20T12:59:45+08:00",
  "detected_at": "2026-05-20T12:59:45+08:00"
}
```

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning | Required Action |
|---|---|---|---|
| Part-code extraction | `wave_washer_good` becomes `wave_washer`; `gasket_bad` becomes `gasket` | Quality suffix is being treated as part identity | Fix Qt `partCodeFromClassificationResult` and shell `class_label_to_part_code` together |
| Existing-part path | Known `part_code` logs `resolved_part_id=<id>` and create-record sends `part_id` | Existing parts may be duplicated | Fix `/parts` parser or matching keys |
| Auto-create path | Unknown `part_code` logs `resolved_part_id=未匹配到云端零件，将由云端自动创建` and create-record succeeds | The helper still blocks before upload | Send `part_code/part_name/part_category/auto_create_part=true` instead of failing locally |
| Missing identity | No `CLOUD_PART_ID`, no `CLOUD_PART_CODE`, and no `CLOUD_CLASS_LABEL` fails before record creation | The board may upload under the wrong first part | Reject the detection or fix Qt env propagation |
| Cloud detail | Record detail returns `part.part_code=<code>`, expected `name/category`, one `source`, and all annotated files | Auto-create or file registration did not complete | Inspect `/tmp/defect-cos-record.json`, prepare logs, and register URLs |
| UI retry | Retrying a failed history row creates a record with the same part code as the original classification | Retry drops part identity | Rebuild retry env from saved model results |

### 5. Good/Base/Bad Cases

```sh
# Good: model-backed upload sends part identity and lets the cloud create a missing real part type.
CLOUD_PART_CODE=wave_washer \
CLOUD_CLASS_LABEL=wave_washer_good \
CLOUD_RESULT=good \
  ./defect-cos-upload --jpg "$raw_jpg" --annotated "$overlay_jpg" --annotated "$mask_png"
```

```sh
# Base: manual test of the auto-create path with an intentionally unique part_code.
CLOUD_PART_CODE=codex_auto_create_20260520125945 \
CLOUD_PART_NAME=波形垫圈自动创建验证20260520125945 \
CLOUD_PART_CATEGORY=垫圈类 \
CLOUD_CLASS_LABEL=codex_auto_create_20260520125945_good \
CLOUD_RESULT=good \
  ./defect-cos-upload --jpg "$raw_jpg" --annotated "$overlay_jpg" --annotated "$mask_png"
```

```text
Bad: `/parts` lookup does not find `wave_washer`, so the board reports image upload failed and tells the operator to create a part manually.
```

```text
Correct: `/parts` lookup miss keeps `CLOUD_PART_CODE=wave_washer`, create-record sends auto-create fields, and the returned record detail contains `part.name=波形垫圈`.
```

### 6. Tests Required

- Run `sh defect-cos-upload --self-test-json-parser` and assert it covers part-code normalization plus `auto_create_part=true` payload generation.
- Run `sh defect-cos-upload --self-test-args` after changing `--jpg`, `--annotated`, or legacy `--png` parsing.
- Run `./test_qt_kms_overlay_assets.sh` and require markers: `CLOUD_PART_CODE`, `CLOUD_CLASS_LABEL`, `partCodeFromClassificationResult`, `auto_create_part`, `part_name`, `part_category`, and `run_auto_create_part_self_test`.
- On the board, deploy `/root/qt_camera_display/defect-cos-upload`, run both helper self-tests, and record the script hash before real upload validation.
- Perform one real upload using a unique `CLOUD_PART_CODE` that does not exist in the cloud. Assert stdout contains `上传成功：record_id=... source_kind=source annotated_count=<n>`.
- Query `GET /api/v1/records/<record_id>` and assert `part.part_code`, `part.name`, `part.category`, `source_count=1`, and `annotated_count=<n>`.
- When diagnosing image upload failure, first check whether create-record failed on `part_not_found` or local "missing part type" before investigating COS PUT or preview URLs.

### 7. Wrong vs Correct

#### Wrong

```text
The raw/overlay/mask files exist locally but upload failed, so COS or network must be broken.
```

#### Correct

```text
Check the create-record stage first. If `CLOUD_PART_CODE` is missing or `/parts` lookup miss still aborts locally, no COS prepare/PUT/register request will happen.
```

#### Wrong

```text
Create separate cloud parts named `wave_washer_good` and `wave_washer_bad`.
```

#### Correct

```text
Create or reuse one part `wave_washer`; send `result=good|bad|review` separately and keep the raw class in `device_context.class_label`.
```

---

## Qt Long-Running Action Responsiveness Contract

Use this convention when a STM32MP157 Qt Quick screen starts a slow board operation such as `保存图片`, COS upload, SD-card sync, diagnostics export, log dump, or safe-remove preparation.

### 1. Scope / Trigger

- Trigger: changing `20_uvc_camera/qt_camera_display/main.cpp`, `20_uvc_camera/qt_camera_display/qml/Main.qml`, or any QML handler that calls C++ code which can touch `/mnt/sdcard`, sockets, network helpers, `sync`, image encoding, or upload scripts.
- Trigger: a visible button starts an operation that can take more than one event-loop tick, especially SD-card image save plus cloud upload.
- User contract: while the operation is running, the operator must still be able to tap other pages such as `历史记录`, `统计分析`, `手动控制`, `参数设置`, and `告警维护`.
- Boundary: the running action may lock only its own conflicting commands, such as `保存图片` and `安全卸载`. It must not block the global QML event loop or disable unrelated navigation.

### 2. Signatures

| Operation | Signature |
|---|---|
| QML save action | `storageController.requestSaveCurrentFrameToSdCard()` |
| C++ async save API | `Q_INVOKABLE void requestSaveCurrentFrameToSdCard()` |
| C++ sync save API | `Q_INVOKABLE QString saveCurrentFrameToSdCard()` kept for CLI/self-test paths only |
| QML busy flag | `property bool saveInProgress` |
| C++ completion signal | `saveCurrentFrameFinished(const QString &message)` |
| QML completion handler | `Connections { target: storageController; function onSaveCurrentFrameFinished(message) { ... } }` |
| Button busy text | `保存中...` |
| SSH save-path self-test | `/root/qt_camera_display/qt_camera_display --storage-self-test` |
| Static contract | `./test_qt_kms_overlay_assets.sh` rejects QML direct calls to `saveCurrentFrameToSdCard()` and requires async markers |

### 3. Contracts

| Area | Contract |
|---|---|
| Main-thread boundary | QML click handlers must return quickly. Do not call a synchronous C++ method from QML when that method waits for SD-card writes, overlay socket replies, upload helpers, or `sync`. |
| Async entry | Use a QML-facing async wrapper such as `requestSaveCurrentFrameToSdCard()` for the screen button. It owns the worker dispatch and emits a completion signal back to the GUI thread. |
| Sync self-test retention | Keep `saveCurrentFrameToSdCard()` available for `--storage-self-test` and direct SSH validation, because it gives a deterministic exit code and output. QML must not use it for the interactive button. |
| Busy scope | `saveInProgress` may disable only commands that conflict with the same storage transaction, currently `保存图片` and `安全卸载`. It must not disable left navigation or unrelated controls. |
| Duplicate-click guard | A second `保存图片` tap while `saveInProgress == true` must be ignored or reported as already running. It must not launch a second overlapping save/upload. |
| Completion signal | The worker must reset `saveInProgress` only through the GUI-thread completion path and must surface the same success/failure message that the synchronous save path would have returned. |
| Navigation during save | Page switches are UI state changes and must remain local to QML. They must not wait for image encoding, SD-card flush, upload, or overlay socket completion. |
| Error propagation | Save/upload failures should update the status text after completion, but an error must not leave `saveInProgress` stuck true or permanently disable storage buttons. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning |
|---|---|---|
| Static QML contract | `./test_qt_kms_overlay_assets.sh` finds `requestSaveCurrentFrameToSdCard`, `saveCurrentFrameFinished`, `saveInProgress`, and rejects QML `saveCurrentFrameToSdCard()` calls | The interactive UI can regress to a blocking save path |
| VM Qt build | `./build_qt_camera_display.sh` outputs an ARM ELF and `strings build-mp157/qt_camera_display` contains `requestSaveCurrentFrameToSdCard` and `saveCurrentFrameFinished` | The async API, signal, or QML resource packaging was not compiled into the deployed binary |
| Board deployment proof | Board `strings /root/qt_camera_display/qt_camera_display` contains the async markers and `run_qt_kms_overlay_display.sh status` reports Qt and overlay PIDs | The board is still running an old binary or the display stack did not restart |
| Save-path proof | `--storage-self-test` creates a new JPG/PNG pair under `/mnt/sdcard/images` and `sync` completes | The lower storage path is broken independently of UI responsiveness |
| Human touch proof | Tap `保存图片`; while the button shows `保存中...`, immediately tap another left navigation page and it changes pages without waiting for the save result | The GUI thread is still blocked or global navigation is disabled |
| Conflict-button proof | During `保存中...`, `保存图片` and `安全卸载` cannot start conflicting storage operations | The app can overlap two writes or unmount while a save is in progress |
| Completion proof | After save/upload completes, status text shows the result and both storage buttons become usable again | The completion signal did not reach QML or busy-state reset is missing |

### 5. Good / Base / Bad Cases

```qml
// Good: interactive QML starts the async request, marks only the storage action busy, and returns to the event loop.
root.saveInProgress = true
storageController.requestSaveCurrentFrameToSdCard()
```

```qml
// Good: unrelated page navigation remains a local QML state change during the save.
activePage = "history"
storageController.setOverlayVisible(false)
```

```qml
// Good: completion resets the busy state on the GUI side and shows the real controller result.
function onSaveCurrentFrameFinished(message) {
    root.saveInProgress = false
    root.storageStatusText = message
}
```

```cpp
/* Good: 同步保存路径保留给命令行自检，QML 按钮改走异步入口，避免阻塞 Qt GUI 主线程。 */
Q_INVOKABLE void requestSaveCurrentFrameToSdCard();
Q_INVOKABLE QString saveCurrentFrameToSdCard();
```

```qml
// Bad: this blocks the QML event loop while the C++ path waits for SD card, overlay socket, upload, or sync.
root.storageStatusText = storageController.saveCurrentFrameToSdCard()
```

```qml
// Bad: a single busy flag disables the whole shell, so other pages cannot be clicked during a save.
navigationPanel.enabled = !root.saveInProgress
```

```text
Base: SSH `--storage-self-test` may still run synchronously because it is not an interactive touch workflow. Do not use that as proof that the screen remains responsive.
```

### 6. Tests Required

- Run `./test_qt_kms_overlay_assets.sh` after changing storage buttons, QML navigation, `CameraStorageController`, save/upload code, or README acceptance text.
- Cross-build `qt_camera_display` in the VM and confirm `file build-mp157/qt_camera_display` reports an ARM ELF.
- Confirm the VM ARM binary and the board binary contain `requestSaveCurrentFrameToSdCard`, `saveCurrentFrameFinished`, and `保存中`.
- Deploy to the NFS rootfs or directly to the board, restart `/root/qt_camera_display/run_qt_kms_overlay_display.sh`, and verify Qt plus overlay PIDs.
- On the board, run `--storage-self-test`, list the newest JPG/PNG pair under `/mnt/sdcard/images`, and execute `sync` to prove the save path still works.
- On the LCD, tap `保存图片`, then while `保存中...` is visible tap `历史记录`, `统计分析`, `手动控制`, `参数设置`, and `告警维护`; each page switch must respond without waiting for the save result.
- On the LCD, confirm `保存图片` and `安全卸载` are temporarily disabled during the running save and become usable again after `saveCurrentFrameFinished`.

### 7. Wrong vs Correct

#### Wrong

```text
The save path writes files correctly, so it is acceptable if the whole Qt UI cannot be tapped until the save finishes.
```

#### Correct

```text
The save path still writes and syncs files, but the screen button starts it through an async controller entry. Only conflicting storage commands are busy; unrelated navigation remains clickable during `保存中...`.
```

---

## Qt Alarm Snapshot And SD-Card Diagnostic File Contract

Use this convention when the STM32MP157 Qt alarm maintenance page saves a diagnostic snapshot to `/mnt/sdcard/logs/qt_alarm_snapshot.txt`.

### 1. Scope / Trigger

- Trigger: changing `20_uvc_camera/qt_camera_display/main.cpp`, `qml/Main.qml`, the alarm maintenance save action, or the static contract test that guards alarm snapshot behavior.
- Goal: the `保存诊断` button must create a real UTF-8 text file on the SD card, not only show a target path in the UI.
- Boundary: QML may assemble the human-readable diagnostic text, but C++ must own mount checking, directory creation, file overwrite, flush, and `fsync`.

### 2. Signatures

| Operation | Signature |
|---|---|
| QML snapshot text builder | `alarmSnapshotText()` |
| QML save action | `storageController.saveAlarmSnapshotToSdCard(alarmSnapshotText())` |
| C++ save API | `Q_INVOKABLE QString saveAlarmSnapshotToSdCard(const QString &snapshotText)` |
| Default log directory | `/mnt/sdcard/logs` |
| Default snapshot file | `/mnt/sdcard/logs/qt_alarm_snapshot.txt` |
| SSH self-test | `/root/qt_camera_display/qt_camera_display --alarm-snapshot-self-test` |
| Static contract | `./test_qt_kms_overlay_assets.sh` checks `saveAlarmSnapshotToSdCard`, `DEFAULT_SDCARD_LOG_DIR`, `DEFAULT_ALARM_SNAPSHOT_FILE`, `alarm-snapshot-self-test`, and `fsync` |

### 3. Contracts

| Area | Contract |
|---|---|
| UI contract | The alarm page may show a short status string, but the save path must be backed by an actual file write. Do not leave the button in a "path only" state. |
| Mount contract | `saveAlarmSnapshotToSdCard()` must check that `/mnt/sdcard` is mounted before writing. If the mount check fails, return `诊断保存失败：...` and do not create a file under the rootfs by mistake. |
| Directory contract | The controller must create `/mnt/sdcard/logs` before opening the file, so SSH users can enter the directory and see the snapshot file directly. |
| Write contract | The snapshot file is overwritten on each save. This keeps the latest diagnostic state in a fixed, easy-to-check path. |
| Flush contract | The controller must call `flush` and then `fsync` before reporting success, so the result is durable on removable media. |
| Text contract | `alarmSnapshotText()` should include the alarm code, alarm status, camera status, storage state, and recent alarm history. The text must remain UTF-8 and end with a trailing newline. |
| Self-test contract | `--alarm-snapshot-self-test` must exercise the same C++ save path as the UI button, so SSH validation proves the real button path. |
| Error contract | Any failure should preserve the specific reason from the failing boundary, such as mount, directory creation, file open, flush, or `fsync`. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning |
|---|---|---|
| UI button save | Clicking `保存诊断` shows `诊断已保存：/mnt/sdcard/logs/qt_alarm_snapshot.txt` | The UI still only formats a path or the controller did not write the file |
| SSH self-test | `/root/qt_camera_display/qt_camera_display --alarm-snapshot-self-test` exits `0` | The same C++ save path cannot write the diagnostic file |
| File existence | `test -s /mnt/sdcard/logs/qt_alarm_snapshot.txt` succeeds | The file was not created or is empty |
| Content proof | `wc -c /mnt/sdcard/logs/qt_alarm_snapshot.txt` is non-zero and `tail -n 30` shows `alarm_code=`, `camera_status=`, and `[recent_alarm_history]` | The file exists but does not contain the expected diagnostic payload |
| Mount proof | `mount | grep ' /mnt/sdcard '` shows the SD card mount | The save path may be writing to the wrong filesystem or the card is not mounted |
| Static contract | `./test_qt_kms_overlay_assets.sh` passes | The code-spec and implementation drifted |

### 5. Good / Base / Bad Cases

```qml
// Good: QML only builds the diagnostic text, then hands it to the C++ controller for the real file write.
resultText = storageController.saveAlarmSnapshotToSdCard(alarmSnapshotText())
```

```cpp
/* Good: the controller creates the log directory, writes the snapshot, flushes it, and fsyncs before success. */
if (!QDir().mkpath(m_logDir)) {
    return QStringLiteral("诊断保存失败：无法创建 ") + m_logDir;
}
```

```text
Base: the snapshot text can be simple, but it must still include a time stamp, the alarm code, and recent history so SSH can confirm the file is real.
```

```text
Bad: `快照目标：/mnt/sdcard/logs/qt_alarm_snapshot.txt` only tells the operator where a file should have been written.
```

### 6. Tests Required

- Run `./test_qt_kms_overlay_assets.sh` after changing the alarm save flow, the QML text builder, the C++ controller, or the alarm README contract.
- Cross-build `qt_camera_display` and confirm the generated ARM binary contains `--alarm-snapshot-self-test` and `/mnt/sdcard/logs/qt_alarm_snapshot.txt`.
- On the board, run `mount | grep ' /mnt/sdcard '`, `/root/qt_camera_display/qt_camera_display --alarm-snapshot-self-test`, `test -s /mnt/sdcard/logs/qt_alarm_snapshot.txt`, `wc -c /mnt/sdcard/logs/qt_alarm_snapshot.txt`, and `tail -n 30 /mnt/sdcard/logs/qt_alarm_snapshot.txt`.
- Verify the screen button and SSH self-test use the same save path by comparing their success messages and file contents.

### 7. Wrong vs Correct

#### Wrong

```text
The alarm page shows a snapshot target path, so the diagnostic file must exist.
```

#### Correct

```text
The alarm page calls the C++ save controller, the controller writes and fsyncs /mnt/sdcard/logs/qt_alarm_snapshot.txt, and SSH can prove the file exists with test -s and tail.
```

---

## Qt Upload History Screen Contract

Use this convention when the STM32MP157 Qt camera UI records, displays, or reviews previous `保存图片` upload attempts.

### 1. Scope / Trigger

- Trigger: changing `20_uvc_camera/qt_camera_display/main.cpp`, `qml/Main.qml`, `uvc_kms_overlay.c`, or any save/upload path that should appear in the Qt `历史记录` page.
- Trigger: adding or fixing failed-upload retry, especially code that reuses saved local images and updates cloud `record_id`/`record_no`.
- Trigger: changing what "show the latest history record" means in QML navigation, list selection, detail entry, or statistics-to-history handoff.
- Data source: upload history is local board evidence under `/mnt/sdcard/images/upload_history.json`, not a live cloud query.
- Display boundary: in `kms-overlay` mode, the live camera plane is outside the QML scene and can visually cover history images unless it is explicitly hidden.

### 2. Signatures

| Operation | Signature |
|---|---|
| History file | `/mnt/sdcard/images/upload_history.json` |
| C++ model | `UploadHistoryModel uploadHistory(QString::fromLatin1(DEFAULT_UPLOAD_HISTORY_FILE))` |
| QML context property | `view.rootContext()->setContextProperty(QStringLiteral("uploadHistory"), &uploadHistory)` |
| Append hook | `appendUploadHistoryRecord(pair, uploadResult)` after `uploadSavedImagesToCos(pair)` |
| Delete hook | `Q_INVOKABLE QString removeRecord(int row)` from QML `deleteHistoryRecord(index)` |
| Retry payload hook | `retryPayloadAt(row, &sourcePath, &annotatedPaths, &classificationResult, &segmentationResult, &errorText)` |
| Retry upload entry | `Q_INVOKABLE void retryUploadRecord(int row)` |
| Retry model update | `bool updateRecordUploadResult(int row, const QString &uploadStatus, const QString &recordId, const QString &recordNo, QString *errorText)` |
| Retry completion signal | `retryUploadFinished(row, resultText)` |
| Required JSON fields | `upload_time`, `result_text`, `workflow_text`, `jpg_path`, `png_path`, `upload_status`, `record_id`, `record_no`, `jpg_size_bytes`, `png_size_bytes` |
| QML list state | `historyDetailVisible == false`, `historyListPanel`, `historyListView` with horizontal `ListView` bound to `uploadHistory` |
| QML detail state | `historyDetailVisible == true`, `historyDetailPanel`, `selectedHistoryRecord`, `imageCarousel`, `backToHistoryList()` |
| QML latest-list focus | `switchPage("history")` calls `focusLatestHistoryListRecord()`; that helper sets `selectedHistoryIndex = uploadHistory.count - 1`, keeps `historyDetailVisible = false`, and calls `positionHistoryListAtSelected()` |
| Retry success ordering | `updateRecordUploadResult()` uses `beginMoveRows(...)`, `m_entries.move(row, lastRow)`, then updates `m_entries[lastRow].uploadTime = refreshedUploadTime` before saving JSON |
| Overlay hide/show | `VISIBLE 0` when entering history, `VISIBLE 1` when returning home |
| Cloud status summary | `compactUploadStatus(uploadResult)` in C++ and `cloudStatusSummary(rawStatus)` in QML |

### 3. Contracts

| Area | Contract |
|---|---|
| Append timing | Append a history record after local JPG/PNG save succeeds and the upload helper returns success or failure. Upload failure must still create a local history entry so the saved evidence can be reviewed. |
| Persistence | Write history JSON through a temporary file, flush, `fsync`, close, then rename. The history file lives beside the images so SD-card backup/removal keeps evidence and metadata together. |
| Self-test parity | `--storage-self-test` must use the same `CameraStorageController` and `UploadHistoryModel` path as the screen button, so SSH save tests appear in the same history page. |
| QML navigation | The left `历史记录` item switches `activePage` to `history`, keeps the list layer visible with `historyDetailVisible=false`, and focuses the newest card at `uploadHistory.count - 1`. It must not automatically open the newest detail page; detail opens only after `查看` or an explicit detail handoff. |
| Latest entry meaning | "显示最新一条记录" means "the latest card is selected and scrolled into view on the list layer." It does not mean replacing the list with the newest detail panel, because operators still need to see and choose from the history cards. |
| Retry success local time | A retry success creates a new cloud record with the current time, but the board JSON is a separate local store. `updateRecordUploadResult()` must update the same local record's `upload_time` to the retry completion time, update `upload_status`/`record_id`/`record_no`, and move that entry to the end of `m_entries` so it becomes the newest board record. |
| Retry failure stability | A retry failure must not refresh `upload_time`, must not move the row, and must preserve the old cloud identity unless the helper returned a valid new identity. This keeps old failed evidence available for another retry and prevents false "latest" ordering. |
| Retry persistence rollback | If JSON save fails after a successful retry update or row move, restore the old model entries and notify QML through reset/data change. Do not leave QML showing a moved row while `/mnt/sdcard/images/upload_history.json` still has the old order. |
| Retry QML selection | After `retryUploadFinished` reports `重新发送成功`, QML must select `uploadHistory.count - 1` and refresh `selectedHistoryRecord`. Continuing to read the old row can show the pre-retry time even though C++ moved the record. |
| Delete behavior | A card-level `删除` command deletes both the JSON history entry and the local JPG/PNG files referenced by that entry. The model must write the new JSON successfully before deleting image files; if JSON persistence fails, restore the model row and return a `删除失败：...` message. |
| Delete safety boundary | Only delete regular files under the history file directory, currently `/mnt/sdcard/images`. Skip and log paths outside that directory instead of trying to be clever, because a damaged JSON path must not delete arbitrary board files. |
| Swipe feel | Horizontal history lists and image carousels should set bounded velocity/deceleration and cache neighboring pages so touch swipes feel continuous on STM32MP157. Avoid settings that let a light flick skip several records. |
| Selection without auto-scroll | Selecting a history card must only update visual selection state, such as comparing delegate `index` with `selectedHistoryIndex`. Do not bind the horizontal history `ListView.currentIndex` to `selectedHistoryIndex` or use `ListView.ApplyRange`, because Qt will automatically scroll the list to the selected item and make the UI look like it jumped from the beginning. |
| Two-level layout | The first layer must show only upload record cards and commands such as `查看`; the image carousel and detection details must be hidden until the user taps `查看`. Do not place the list and full detail view in the same visible layer on a 1024x600 screen. |
| Detail layout | The detail view shows the selected upload's images on the left with horizontal swiping, and detection result, cloud record info, file sizes, upload status, and image archive summary on the right. It must provide `返回列表` without returning all the way to `首页`. Do not show local filesystem paths as a standalone operator-facing section. |
| Detection-info text budget | The `检测信息` panel must use bounded text such as `maximumLineCount`, `elide`, and `clip: true`. Long model confidence text, defect hints, or archive summaries must not overflow the panel or overlap the next page section. |
| Overlay ownership | QML must not draw required history content under the KMS overlay video plane. Call `storageController.setOverlayVisible(false)` on history entry and `true` when returning home. |
| Cloud identity | `record_id` and `record_no` are parsed from the upload helper output only after the helper created an isolated cloud record. Do not invent IDs in QML. |
| Display text budget | List cards and detail panels must not show raw file paths or raw upload script output. Local image paths may be used internally to load images, but the operator-facing text should describe the archive count/purpose. Cloud status must be a short summary such as `上传成功 ID 21 MP157-...`. |
| Legacy status cleanup | Old `upload_status` values may contain mojibake or verbose script output. QML must derive display text from `record_id`/`record_no` tokens when possible instead of rendering the raw string. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning |
|---|---|---|
| Static contract | `./test_qt_kms_overlay_assets.sh` finds `UploadHistoryModel`, `upload_history.json`, `historyDetailVisible`, `historyListPanel`, `historyListView`, `historyDetailPanel`, `imageCarousel`, `backToHistoryList`, `deleteHistoryRecord`, `removeRecord`, `compactUploadStatus`, `cloudStatusSummary`, swipe tuning markers, and `VISIBLE` | The save-history-display-delete contract drifted between C++, QML, overlay, or docs |
| Qt build | `./build_qt_camera_display.sh` recompiles `main.cpp`, regenerates `qrc_qml.cpp`, and outputs an ARM ELF | C++ model, context property, or QML resource packaging is broken |
| Overlay build | `./build_uvc_kms_overlay.sh` outputs an ARM ELF with `VISIBLE` support | History page can still be covered by the live video plane |
| History JSON | `test -s /mnt/sdcard/images/upload_history.json` and `tail -n 40` show the newest paths and status | Save/upload completed but the UI has no durable history source |
| History UI list layer | Touch `历史记录`; only upload cards are visible, the newest card is selected and visible, with no large image carousel or right-side detection panel | The list and detail layers are still collapsed together, or "latest record" was incorrectly implemented as auto-opening detail |
| History UI detail layer | Tap `查看`; detail view appears, images swipe on the left, detection/cloud/file data appears on the right, and `返回列表` returns to cards | Navigation, horizontal card overflow, image carousel, or detail-state transition is broken |
| Selection stability | Swipe to the middle of the history list, tap a visible card body, then continue swiping; the tapped card becomes highlighted but the list does not animate back from the first card or snap to the selected card | `currentIndex` or highlight-range binding is still coupled to selection state |
| Cloud status display | New history JSON has a short `upload_status`; old verbose/garbled entries display as clean status using extracted IDs | UI renders raw helper output, causing mojibake and clipped cards |
| Retry success JSON | Start with a row whose `upload_status` contains `上传失败`, tap `重新发送`, then `tail -n 120 /mnt/sdcard/images/upload_history.json`; on success the same evidence entry is the last JSON item and its `upload_time` is the retry completion time | Cloud retry succeeded but board history still shows the old failure time or old order |
| Retry failure JSON | Force retry failure; the row keeps its old `upload_time`, old order, and failure status while the button remains available | Retry failure was treated as a new/latest successful board record |
| Retry detail selection | After successful retry, the detail page shows the last row's refreshed time/status instead of the old row | QML did not reselect `uploadHistory.count - 1` after C++ moved the record |
| Detection-info bounds | Use a long `classification_result` or defect hint; the `检测信息` panel clips/elides within its rectangle and does not cover lower content | Missing `maximumLineCount`, `elide`, or `clip` lets text overflow on 1024x600 |
| Overlay visibility | Entering history hides live video; returning home restores it | QML content is fighting the external KMS plane instead of controlling its visibility |
| Delete record and files | Capture one entry's `jpg_path`/`png_path`, tap `删除`, then verify both files no longer exist and `upload_history.json` no longer contains the deleted path | QML deleted only the visible row, C++ failed to persist JSON, or entity files were left behind on the SD card |

### 5. Good / Base / Bad Cases

```cpp
/* Good: local save success is preserved even if upload fails, and the evidence still appears in history. */
const QString uploadResult = uploadSavedImagesToCos(pair);
appendUploadHistoryRecord(pair, uploadResult);
```

```qml
// Good: history page changes QML state and asks the overlay process to hide the external video plane.
activePage = "history"
historyDetailVisible = false
storageController.setOverlayVisible(false)
```

```qml
// Good: entering history focuses the latest card while staying on the list layer.
function focusLatestHistoryListRecord() {
    selectedHistoryIndex = uploadHistory.count - 1
    selectedHistoryRecord = uploadHistory.entryAt(selectedHistoryIndex)
    historyDetailVisible = false
    positionHistoryListAtSelected()
}
```

```qml
// Good: detail is a second layer reached only after the operator picks a specific upload record.
function showHistoryDetail(index) {
    selectedHistoryIndex = index
    selectedHistoryRecord = uploadHistory.entryAt(index)
    historyDetailVisible = true
}
```

```qml
// Good: selection is visual state only; the ListView keeps the operator's current scroll position.
color: root.selectedHistoryIndex === index ? "#1f3a2f" : "#20262a"
```

```qml
// Bad: this couples selection to ListView positioning and causes automatic scrolling.
currentIndex: root.selectedHistoryIndex
highlightRangeMode: ListView.ApplyRange
```

```cpp
/* Good: retry success refreshes local time after moving the record to the newest position. */
if (uploadSucceeded) {
    const int lastRow = m_entries.size() - 1;
    if (row != lastRow) {
        beginMoveRows(QModelIndex(), row, row, QModelIndex(), m_entries.size());
        m_entries.move(row, lastRow);
        endMoveRows();
    }
    m_entries[lastRow].uploadTime = refreshedUploadTime;
}
```

```cpp
/* Bad: cloud upload used current time, but local JSON keeps the old failed upload_time and old row position. */
m_entries[row].uploadStatus = QStringLiteral("上传成功 ...");
saveToDisk();
```

```qml
// Bad: drawing cards, large images, detection result, raw upload status, and paths all in the first visible history layer.
historyListView.visible = true
historyDetailPanel.visible = true
```

```cpp
/* Good: keep stored/displayed cloud status short; details stay in logs, not in the card UI. */
entry.uploadStatus = compactUploadStatus(uploadResult);
```

```cpp
/* Good: write the shortened history JSON first, then delete the image files referenced by the removed entry. */
if (!saveToDisk()) {
    restoreRemovedRow();
    return QStringLiteral("删除失败：历史文件写入失败");
}
return removeHistoryImageFiles(removedEntry);
```

```cpp
/* Bad: deleting image paths before the JSON update can leave a visible history card pointing at missing files. */
QFile::remove(entry.jpgPath);
QFile::remove(entry.pngPath);
saveToDisk();
```

```text
Base: If the cloud is offline, the latest history entry may show `上传失败：...` but still lets the operator review the locally saved JPG/PNG pair.
```

### 6. Tests Required

- Run `./test_qt_kms_overlay_assets.sh` after changing history model, history QML, save/upload controller, overlay visibility command, or README.
- Cross-build `qt_camera_display`; if QML changed, force `qrc_qml.o` regeneration or confirm the build log runs `rcc -name qml`.
- Cross-build `uvc_kms_overlay` after adding or changing `VISIBLE` command handling.
- Before deploying, prove the VM source and ARM binary contain the intended history markers, for example `historyDetailVisible`, `historyListPanel`, `backToHistoryList`, `deleteHistoryRecord`, `removeRecord`, and `compactUploadStatus`.
- On the board, save at least one image pair, then assert `/mnt/sdcard/images/upload_history.json` contains the newest `jpg_path`, `png_path`, and `upload_status`.
- On the LCD, open `历史记录`; verify the first layer shows only upload cards and the latest card is selected/visible. It must not auto-open the latest detail page. Then tap `查看`, swipe between JPG and PNG, tap `返回列表`, and finally return to `首页`.
- On the LCD, swipe the history list away from the first card, tap a visible card body to select it, and verify the list does not automatically scroll back or snap to the selected card.
- Prepare or keep one failed upload entry, tap `重新发送`, then verify success and failure cases separately in `/mnt/sdcard/images/upload_history.json`: success updates `upload_time` and moves the same entry to the last array position; failure preserves old time and old position.
- After retry success, verify QML detail selection reads `uploadHistory.count - 1` so the visible time/status matches the last JSON item.
- Verify the detail `检测信息` panel with long model text; text must elide/clip inside the panel and the screen must not show a standalone `本地图片位置` section.
- On the LCD, tap a history card's `删除`, then assert the removed `jpg_path` and `png_path` files no longer exist and the JSON no longer contains that path.
- In KMS overlay mode, verify the live video plane is hidden on the history page and restored on the home page.

### 7. Wrong vs Correct

#### Wrong

```text
Only the cloud detail page keeps upload history, so the board Qt UI can query later when needed.
```

#### Correct

```text
The board writes `/mnt/sdcard/images/upload_history.json` at save time. The Qt history page reads local evidence immediately, even if cloud upload failed or the network is offline.
```

#### Wrong

```text
Historical cards are visible at the top, while the selected image preview, detection result, raw cloud status, and file path are also visible below on the same 1024x600 layer.
```

This crowds the screen, truncates Chinese text, and makes the user think the detail panel appeared before pressing `查看`.

#### Correct

```text
`历史记录` opens a list-only layer. `查看` switches to a detail-only layer with `返回列表`, and cloud status is summarized instead of rendering raw script output.
```

#### Wrong

```text
The cloud retry uses the current time, so the local board history can keep the old failed upload_time.
```

#### Correct

```text
Cloud time and board history time are separate stores. Retry success updates the local JSON `upload_time`, moves the same entry to the last array position, and refreshes QML selection to that last row.
```

#### Wrong

```text
"Enter history and show the latest record" means immediately opening the newest detail panel.
```

#### Correct

```text
"Enter history and show the latest record" means the history list remains visible, the latest card is selected and scrolled into view, and detail opens only after `查看`.
```

---

## Qt Fixed-Screen Layout Capacity Contract

Use this convention when adding, resizing, or reorganizing Qt Quick panels on the fixed 1024x600 STM32MP157 LCD, especially dashboards, statistic cards, history panels, setting panels, alarm panels, and any `Repeater`/`Column`/`Grid` whose item count can change.

### 1. Scope / Trigger

- Trigger: changing `20_uvc_camera/qt_camera_display/qml/Main.qml` page geometry, panel height/width, `Column`, `Row`, `Grid`, `Repeater`, `ListView`, chart bars, KPI cards, button groups, or status rows.
- Trigger: adding one more metric, row, button, legend, status chip, chart series, or text line to an existing fixed-height panel.
- Trigger: a board photo shows a row, button, chart, or label clipped by the panel border, covered by another component, or pushed below the visible 1024x600 screen.
- Goal: before writing QML, prove the content has a display budget. If it does not fit, choose a layout strategy such as split columns, scrolling, pagination, summarization, or moving details to a secondary surface.

### 2. Signatures

| Operation | Signature / Marker |
|---|---|
| Fixed screen root | `root.width === 1024` and `root.height === 600` as the target design boundary |
| Fixed panel geometry | `Rectangle { id: <panelId>; x: ...; y: ...; width: ...; height: ...; clip: true }` |
| Vertical capacity formula | `availableHeight = panel.height - headerHeight - topPadding - bottomPadding` |
| Repeated vertical demand | `requiredHeight = itemHeight * itemCount + spacing * Math.max(0, itemCount - 1)` |
| Horizontal capacity formula | `requiredWidth = fixedColumnsWidth + spacing * gapCount + dynamicMinWidth` |
| Split-column helper | `statsDistributionLeftBars()` and `statsDistributionRightBars()` split five metrics across two visible columns |
| Scroll owner | `ListView { height: ...; clip: true; boundsBehavior: Flickable.StopAtBounds }` or `Flickable { contentHeight: ... }` |
| Global toast layer | `Item { id: globalStorageToastLayer; z: 900; visible: true; Rectangle { id: storageToast; anchors.bottom: parent.bottom } }` |
| Static contract | `test_qt_kms_overlay_assets.sh` greps helper names and panel IDs for fragile fixed-screen contracts |
| Board proof | Photo/screenshot after deployment, or a marker check plus human LCD inspection when screenshots are not available |

### 3. Contracts

| Area | Contract |
|---|---|
| Budget before implementation | Before adding visible items, calculate whether the worst-case count fits the panel. Use actual QML values: panel height, title `y`, title height, row height, delegate height, spacing, and bottom margin. Do not rely on visual intuition. |
| One-row addition rule | Adding a new row to an existing `Column + Repeater` is a layout change, not a data-only change. Recompute the budget and update validation if item count changes. |
| Fit decision | If `requiredHeight <= availableHeight`, keep the simple fixed layout. If it exceeds the budget, choose an explicit strategy: split into columns, reduce nonessential chrome, move overflow into `ListView/Flickable`, create a second detail page, or summarize visible text and preserve full data elsewhere. |
| No hidden required content | `clip: true` is only an overlap guard. It is not a valid way to hide required metrics, action buttons, fault rows, or explanations. Required content must remain visible, scrollable, or reachable through a clear control. |
| Stable repeated delegates | Repeated items must use fixed `height`, fixed row widths, bounded text, and stable spacing so dynamic values do not resize the panel or push later rows out of bounds. |
| Split-column threshold | When a fixed-height panel contains five or more short metrics, first consider two-column grouping by meaning before shrinking fonts. For example, statistics distribution uses left `良品/坏品/待复核` and right `上传成功/上传失败`. |
| Scroll threshold | When the operator must inspect arbitrary-length history, logs, alarms, or records, use a bounded `ListView` or `Flickable` inside the card rather than expanding the card height. |
| Cross-page toast ownership | A toast that reports actions from multiple pages must be rendered in a root-level layer after ordinary pages, not inside a home/history/settings panel. It must have a documented z value above page content and below modal/boot overlays as appropriate, so bottom controls cannot cover it. |
| Toast layer lifetime | The root-level toast layer must remain `visible: true`; only the toast rectangle itself may fade with `opacity` and toggle `visible`. Do not bind the parent layer to `storageToast.visible`, because a hidden parent can prevent the child animation/visibility path from bringing the toast back. |
| Meaningful grouping | Layout compression must preserve meaning. Group by workflow boundary, such as detection result vs upload state, device health vs action buttons, summary vs full explanation. Do not split purely by arbitrary item order if it makes the screen harder to scan. |
| Board font reality | Chinese font metrics on the board can differ from desktop expectation. The budget must include margins for longer Chinese labels, percent values, record numbers, and translated part names. |
| Documentation sync | If a module README or plan describes a screen, update its validation matrix with the layout fit requirement and the first failure check. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning | Required Action |
|---|---|---|---|
| Static marker check | `./test_qt_kms_overlay_assets.sh` finds the layout helper and panel IDs that protect the known fragile area | Future edits can silently regress to an overflowing layout | Add or update marker checks for the new layout contract |
| Capacity arithmetic | `requiredHeight <= availableHeight` for every fixed `Column/Repeater`, or overflow is handled by split columns/scroll/detail page | A row can be clipped at the bottom on 1024x600 | Redesign the panel before coding more fields |
| Panel border inspection | On the LCD, all required rows, bars, and buttons stay inside their panel border | Board font metrics or fixed sizes were underestimated | Adjust layout and rerun board check |
| Repeated data stress | Test with the maximum planned metric count, longest status labels, and multi-digit counts/percentages | The layout only works for short demo data | Add elide/wrap, split columns, or scroll owner |
| Touch target retention | Buttons and list rows remain large enough to tap after compression | A fit fix made controls hard to use | Prefer pagination/scrolling over shrinking below usable size |
| Raw data retention | Summarized panels still keep full raw details in history JSON, logs, or a detail overlay | UI fit was achieved by deleting diagnostic evidence | Restore raw data in a reachable non-compact surface |
| Toast layer visibility | Trigger storage/detection/settings/alarm actions from history, statistics, settings, alarm, and logs pages; the toast remains visible above page content | The toast is owned by a lower page layer, lacks a high enough z value, or the parent layer is hidden through `visible: storageToast.visible` | Move the toast to a root-level always-visible layer and add static marker checks |

### 5. Good / Base / Bad Cases

```qml
// Good: five short metrics do not fit one vertical stack, so the data is split by meaning.
Row {
    id: statsDistributionSplitRow
    width: parent.width - 28
    spacing: 16

    Column {
        id: statsDistributionLeftColumn
        width: (statsDistributionSplitRow.width - statsDistributionSplitRow.spacing) / 2
        Repeater { model: root.statsDistributionLeftBars() }
    }

    Column {
        id: statsDistributionRightColumn
        width: (statsDistributionSplitRow.width - statsDistributionSplitRow.spacing) / 2
        Repeater { model: root.statsDistributionRightBars() }
    }
}
```

```qml
// Base: arbitrary-length rows stay inside a bounded scroll owner instead of growing the page.
ListView {
    id: statsRecentListView
    height: parent.height - 48
    clip: true
    model: root.statsRecentRows()
    delegate: Rectangle {
        width: statsRecentListView.width
        height: 22
    }
}
```

```qml
// Good: a cross-page action result lives in a root-level layer, not inside a specific page.
Item {
    id: globalStorageToastLayer
    anchors.fill: parent
    z: 900
    visible: true

    Rectangle {
        id: storageToast
        anchors.bottom: parent.bottom
        opacity: root.storageToastVisible ? 1.0 : 0.0
    }
}
```

```qml
// Bad: the parent layer depends on the child toast's visible state, so the whole layer
// can stay hidden and the next toast request may never draw on the board.
Item {
    id: globalStorageToastLayer
    visible: storageToast.visible

    Rectangle {
        id: storageToast
        visible: opacity > 0.01
    }
}
```

```qml
// Bad: five fixed rows are stacked in a 160px panel without checking title, spacing, and bottom margin.
Column {
    y: 34
    spacing: 4
    Repeater {
        model: root.statsDistributionBars()
    }
}
```

### 6. Tests Required

- Run `./test_qt_kms_overlay_assets.sh` after any QML page geometry, repeated metric count, or fixed panel layout change.
- For global action toasts, assert the static test checks `globalStorageToastLayer`, `storageToast`, `z: 900`, `visible: true`, bottom anchoring, and rejects `visible: storageToast.visible`.
- Run `git diff --check` after documentation/QML layout edits.
- For QML changes, cross-build `qt_camera_display` and verify the build log runs `rcc -name qml`, then verify the ARM binary contains the new layout marker strings with `strings`.
- On the board, open the changed page and inspect the exact 1024x600 LCD, not only desktop source code. Required rows must be visible, scrollable, or reachable through a clear detail control.
- Stress the screen with the largest expected visible data set: five distribution bars, many history rows, long upload status, multi-digit sample counts, long part names, or long alarm messages as applicable.
- Update the module README validation table with columns for `测试目标`, `执行位置`, `命令/动作`, `预期输出/现象`, and `失败时排查`.

### 7. Wrong vs Correct

#### Wrong

```text
This panel currently has four rows and looks fine, so adding a fifth row is only a data change.
```

#### Correct

```text
Adding a fifth row changes the layout capacity. Recompute required height. If it exceeds the panel, split columns, scroll, or move details before implementation.
```

#### Wrong

```text
Set `clip: true`; then overflow is fixed because it no longer draws outside the card.
```

#### Correct

```text
`clip: true` only prevents overlap. Required information must still be visible inside the budget or reachable through scrolling/detail navigation.
```

#### Wrong

```text
Use a smaller font until all rows appear to fit.
```

#### Correct

```text
Keep touch/readability first. Prefer meaningful grouping, two-column layout, or bounded scrolling over shrinking text below board-readable size.
```

---

## Qt Small-Screen Text Budget Contract

Use this convention when a Qt Quick screen on the 1024x600 STM32MP157 LCD displays model output, cloud status, file metadata, diagnostic text, or any operator-facing string whose length can change at runtime.

### 1. Scope / Trigger

- Trigger: adding or changing `Text`, `Label`, `Repeater` rows, status cards, history detail panels, result panels, toasts, or navigation labels in `20_uvc_camera/qt_camera_display/qml/Main.qml`.
- Trigger: displaying raw model classes such as `splitwasher_good`, fused result reasons, upload helper output, filesystem paths, diagnostics, or any text derived from JSON/script/model output.
- Display boundary: the KMS overlay layout leaves narrow QML side panels such as the 182 px home result panel, so a string that looks acceptable in source code can be unreadable or truncated on the board.
- Goal: every required operator-facing string must either fit completely, wrap within a bounded area, or be intentionally summarized with a short display helper while preserving full details in history/logs.

### 2. Signatures

| Surface | Signature / Marker |
|---|---|
| Home short class helper | `function compactHomeClassText(classText)` |
| Home short result helper | `function compactHomeModelText(stateText)` |
| History text cleanup helper | `function compactHistoryInfoLine(lineText)` |
| Bounded QML text | `Text { width: ...; wrapMode: Text.Wrap; maximumLineCount: ...; elide: Text.ElideRight }` |
| Clipped panel | `Rectangle { clip: true; ... }` |
| Dense detail text | `lineHeightMode: Text.ProportionalHeight` plus `lineHeight: <value>` |
| Static contract | `test_qt_kms_overlay_assets.sh` must grep for the helper names and bounded-text markers when the screen depends on them |

### 3. Contracts

| Area | Contract |
|---|---|
| Raw vs display text | Keep raw model/script values in C++ records or JSON fields, but never render long raw strings directly in narrow panels. Use a display helper for the visible value. |
| Home result panel | In `videoBackend === "kms-overlay"`, the right result panel must show short values for `类别` and `模型`, such as `弹垫-良`, `综合良品`, or `分类坏，等UNet`. Full model names and fused reasons belong in history detail or logs. |
| History detail panel | The `检测信息` block must remove extra whitespace, avoid blank lines, set `clip: true`, and bound every text row with `maximumLineCount` and `elide`. If all required rows cannot fit, shorten the text or increase the panel height intentionally. |
| Dynamic text budget | Before adding a field, estimate the longest realistic Chinese and ASCII value, then assign either fixed width plus elide, wrapping plus line count, or a short helper. Do not rely on the current sample value being short. |
| UI meaning preservation | Short text may summarize, but it must not change the decision meaning. For example, `splitwasher_good` can become `弹垫-良`, while full class and confidence remain in the underlying record. |
| No accidental empty lines | Do not assemble detail text with untrimmed raw output that may contain `\n`, tabs, or repeated spaces. Normalize with a helper such as `compactHistoryInfoLine()`. |
| Board-first verification | QML layout correctness is not proven by compilation. Verify the actual 1024x600 board screen or a screenshot/photo after deploying, especially when the user reports clipped text. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning |
|---|---|---|
| Static helper markers | `./test_qt_kms_overlay_assets.sh` finds `compactHomeClassText`, `compactHomeModelText`, and `compactHistoryInfoLine` when these display paths exist | Long model/status strings may be rendered directly again |
| Bounded history text | The test finds `clip: true`, `maximumLineCount`, `elide`, `spacing: 1`, and `lineHeight` inside the history detection-info panel | A long defect hint or archive line can overflow or hide the last row |
| Home result display | The home result `Repeater` uses `compactHomeClassText(root.detectClassName)` and `compactHomeModelText(root.detectState)` | The 182 px KMS result panel can clip `splitwasher_good` or fused reason text |
| Long-value smoke case | Test with `splitwasher_good`, `综合判定坏品：分类GOOD；UNet发现缺陷`, and a long defect hint; required labels remain readable | The display was only tested with short placeholders |
| Board screenshot/photo | On the 1024x600 LCD, no required row is cut off, hidden behind another row, or clipped without an intentional ellipsis | Desktop/source review missed font metrics, scaling, or panel-size limits |
| Raw evidence retention | JSON/history still stores the full model result and upload status even when QML shows a short summary | The UI fix lost diagnostic information needed for review or retry |

### 5. Good / Base / Bad Cases

```qml
// Good: the narrow home panel shows a short value, while the raw class remains in detectClassName.
{"name": "类别", "value": root.compactHomeClassText(root.detectClassName)}
{"name": "模型", "value": root.compactHomeModelText(root.detectState)}
```

```qml
// Base: a detail line wraps in a bounded rectangle and is clipped instead of overlapping the next row.
Rectangle {
    clip: true

    Text {
        width: parent.width
        text: root.compactHistoryInfoLine("缺陷提示：" + root.historyDefectHintText(record))
        wrapMode: Text.Wrap
        maximumLineCount: 2
        elide: Text.ElideRight
        lineHeightMode: Text.ProportionalHeight
        lineHeight: 0.86
    }
}
```

```qml
// Bad: raw model output is rendered directly in a narrow status row.
Text {
    width: 120
    text: root.detectState
}
```

### 6. Tests Required

- Run `./test_qt_kms_overlay_assets.sh` after changing home result text, history detail text, model-result display helpers, or panel geometry.
- Run `git diff --check` so whitespace-only layout edits do not introduce formatting defects.
- Cross-build `qt_camera_display` after QML changes and confirm the build log runs `rcc -name qml`; otherwise the board can run stale embedded QML.
- Deploy to the board and compare the actual LCD screen against long-value cases: home `类别/模型`, history `检测信息`, cloud status, and any button labels touched by the change.
- When a user supplies a photo showing clipped text, verify the same screen after the fix with a fresh board run, not only by code inspection.

### 7. Wrong vs Correct

#### Wrong

```text
The string is correct, so it is fine if the 1024x600 screen cuts the end off.
```

#### Correct

```text
The visible string must fit the target panel. Use a short helper for the panel and preserve the full raw value in history JSON or logs.
```

#### Wrong

```text
The history detail panel has clip: true, so clipped final content is acceptable.
```

#### Correct

```text
clip: true prevents overlap; it does not prove required content is visible. Required rows need line budgets, compact wording, and board-photo verification.
```

---

## Qt Statistics Analysis Screen Contract

Use this convention when adding or changing the STM32MP157 Qt `统计分析` page for local production, upload, and file-save summaries.

### 1. Scope / Trigger

- Trigger: changing `20_uvc_camera/qt_camera_display/qml/Main.qml`, `UploadHistoryModel`, or any field that should appear in the `统计分析` page.
- Data source: the statistics page reads the same local `uploadHistory` model backed by `/mnt/sdcard/images/upload_history.json`; it must not query the cloud live during page rendering.
- Display boundary: in `kms-overlay` mode, the statistics page is a QML-only full work area, so the external KMS video plane must be hidden on entry and restored only on `首页`.

### 2. Signatures

| Operation | Signature |
|---|---|
| Navigation state | `activePage === "stats"` |
| Page-visible property | `property bool statsPageVisible: activePage === "stats"` |
| Page switch | `switchPage("stats")` |
| Overlay visibility contract | `storageController.setOverlayVisible(pageName === "home")` |
| Summary helper | `function statsSummary()` in `qml/Main.qml` |
| Trend helper | `function statsRecentBars()` in `qml/Main.qml` |
| Distribution helper | `function statsDistributionBars()` in `qml/Main.qml` |
| Distribution left helper | `function statsDistributionLeftBars()` in `qml/Main.qml` |
| Distribution right helper | `function statsDistributionRightBars()` in `qml/Main.qml` |
| Distribution columns | `statsDistributionLeftColumn` and `statsDistributionRightColumn` inside `statsDistributionPanel` |
| Recent rows helper | `function statsRecentRows()` in `qml/Main.qml` |
| Recent rows view | `statsRecentListView` vertical `ListView` inside `statsRecentPanel` |
| Detail handoff | `openHistoryDetailFromStats(index)` calls `switchPage("history")` and `showHistoryDetail(index)` |
| Static contract | `./test_qt_kms_overlay_assets.sh` checks `statsPageVisible`, `statsSummary`, `statsRecentBars`, `statsDistributionBars`, `statsDistributionLeftBars`, `statsDistributionRightBars`, `statsRecentRows`, `statsRecentListView`, `openHistoryDetailFromStats`, and `statsPage` panel IDs |

### 3. Contracts

| Area | Contract |
|---|---|
| Data ownership | Statistics are derived from local upload history only. Do not add a second statistics file or a live cloud dependency unless the user explicitly asks for cloud-side analytics. |
| Field meaning | `total` means local history record count; `good` means records whose `resultText === "良品"`; `review` covers all non-good records; upload success is inferred from `recordId`, `recordNo`, or a success marker in `uploadStatus`. |
| Empty state | If `uploadHistory.count <= 0`, show a clear empty state telling the operator to save an image first; do not render empty axes or blank panels. |
| Chart implementation | Use lightweight QML rectangles for KPI cards, bars, and distribution strips. Do not introduce Qt Charts or another runtime dependency for this embedded 1024x600 screen without a separate design decision. |
| Screen density | Keep the page data-dense but scannable: top KPI row, middle trend/distribution panels, bottom recent records and cloud/file state panels. Avoid showing raw JSON, long file paths, or script output on the statistics page. |
| Distribution layout | `statsDistributionBars()` may contain five metrics, but the 160px distribution panel must render them as two columns: left column for `良品/坏品/待复核`, right column for `上传成功/上传失败`. Do not put all five bars in one vertical column on the 1024x600 LCD, because the last upload-failure row can overflow the panel. |
| Recent rows scroll | `statsRecentRows()` should return the latest records in newest-first order, not only the first visible five rows. `statsRecentPanel` must contain a vertical `ListView` so operators can swipe inside the card to review more records without leaving the statistics page. |
| Touch behavior | Touch targets on recent rows must be large enough to tap and should hand off to the existing history detail screen rather than duplicating image/detail UI. The vertical list must keep row tapping intact after a drag gesture ends. |
| Overlay ownership | Entering `统计分析` hides the live KMS video plane. Returning to `首页` restores it. QML must not draw statistics under a visible external video plane. |
| Historical truthfulness | Until the real defect model writes defect categories, the statistics page must describe values as save/upload history and `良品/待复核`, not as final defect-type analytics. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning |
|---|---|---|
| Static contract | `./test_qt_kms_overlay_assets.sh` finds all `stats*` helpers and panel IDs | Navigation or layout markers drifted and the page may become unreachable or incomplete |
| QML navigation | Touch left `统计分析`; `activePage` becomes `stats` and `statsPageVisible` is true | Navigation item still behaves as placeholder text |
| Overlay visibility | In KMS mode, statistics page is not covered by live camera video, and `首页` restores video | `setOverlayVisible(pageName === "home")` or overlay `VISIBLE` command is broken |
| Count parity | Board `grep -c '"upload_time"' /mnt/sdcard/images/upload_history.json` matches the visible `总记录` | The page is reading stale data or calculating totals from a different source |
| Distribution fit | `分布概览` shows `良品/坏品/待复核` in the left column and `上传成功/上传失败` in the right column, all inside the panel border | The panel regressed to a single vertical list or fixed heights no longer fit the LCD |
| Recent rows scroll | With more records than fit the card, the operator can swipe `statsRecentListView` vertically and reveal older rows | The page still uses a fixed `Column + Repeater` or `statsRecentRows()` truncates to five records |
| Recent handoff | Tapping a recent row opens the corresponding `历史记录` detail page | Row index mapping or `openHistoryDetailFromStats()` is broken |
| Empty state | With no history JSON or zero records, the page shows a readable empty message | The page appears blank or misleadingly reports zeros without guidance |

### 5. Good / Base / Bad Cases

```qml
// Good: one local summary function owns the calculation used by KPI, distribution, and file panels.
function statsSummary() {
    var summary = { total: uploadHistory.count, good: 0, review: 0 }
    // Iterate uploadHistory.entryAt(i) and calculate derived values.
    return summary
}
```

```qml
// Good: statistics page reuses the history detail page for image review.
function openHistoryDetailFromStats(index) {
    root.switchPage("history")
    root.showHistoryDetail(index)
}
```

```qml
// Bad: a statistics page that queries the cloud on every render can block the embedded UI and disagree with local evidence.
onVisibleChanged: cloudApi.fetchStats()
```

```text
Base: If cloud upload is offline, the statistics page still shows local file counts and marks upload failures using the saved `upload_status`.
```

### 6. Tests Required

- Run `./test_qt_kms_overlay_assets.sh` after changing `Main.qml`, history fields, or statistics layout.
- Cross-build `qt_camera_display` after QML changes and verify the ARM binary contains markers such as `统计分析`, `statsSummary`, and `statsPage`.
- On the board, save at least one JPG/PNG pair, then open `统计分析` and compare visible `总记录` with `grep -c '"upload_time"' /mnt/sdcard/images/upload_history.json`.
- On the board, confirm the distribution panel keeps all five metrics visible without clipping: left `良品/坏品/待复核`, right `上传成功/上传失败`.
- In KMS overlay mode, verify statistics page hides the video plane and `首页` restores it.
- With more than five history rows, swipe inside the `最近记录` card and confirm older rows become visible without moving the whole statistics page.
- Tap a recent row and confirm the app opens the matching history detail page with JPG/PNG preview.

### 7. Wrong vs Correct

#### Wrong

```text
The statistics page calls the cloud API directly and shows whatever the backend returns, even when local saved images exist but upload failed.
```

#### Correct

```text
The statistics page summarizes local `uploadHistory` first, so saved JPG/PNG evidence remains visible and countable even when the network or cloud upload path is down.
```

---

## Qt Quick Touch Controls On KMS Overlay UI

Use this convention when adding touch buttons or status controls to the STM32MP157 Qt Quick industrial UI while camera video is drawn by an external KMS overlay plane.

### Scope / Trigger

- Trigger: adding or changing QML buttons, `MouseArea` handlers, workflow-state text, or touch validation in `20_uvc_camera/qt_camera_display/qml/Main.qml`.
- Display route: Qt owns the primary UI surface and `uvc_kms_overlay` owns the video plane.
- Input route: Goodix GT911/GT9147 reports through Linux input, normally as `/dev/input/eventX`.

### Signatures

| Operation | Signature |
|---|---|
| QML button handler | `MouseArea { anchors.fill: parent; onClicked: root.handleControlAction(modelData.action, modelData.state) }` |
| KMS-mode state update | `handleControlAction(action, stateText)` updates `workflowState` without stopping `uvc_kms_overlay` |
| Touch runtime proof | `qtpid=$(pidof qt_camera_display); ls -l /proc/$qtpid/fd | grep /dev/input/event` |
| Human acceptance | Tap `开始`, `暂停`, `继续`, and `停止`; verify the visible workflow/status text changes |

### Contracts

| Area | Contract |
|---|---|
| Button placement | Keep KMS-overlay controls on the Qt UI plane outside the video rectangle, such as the right result panel. Do not place required controls inside the external video plane area where DRM plane ordering can hide or visually confuse them. |
| Button set | The production control set for this screen is `开始`, `暂停`, `继续`, and `停止`; avoid drifting back to older placeholder actions such as `单步` or `复位` unless the user asks for those modes. |
| KMS-mode behavior | In `videoBackend === "kms-overlay"`, QML buttons update UI state only. They must not directly kill, pause, or restart the external overlay process; use `run_qt_kms_overlay_display.sh` for process lifecycle. |
| Non-KMS fallback | In `qt-safe` mode, the same handler may set `cameraView.running=false` for `暂停/停止` and `true` for `开始/继续`, because Qt owns the camera only in that mode. |
| Touch verification | A visual button and a `MouseArea` are not proof of touch support. Verify the runtime script loaded `evdevtouch`, Qt opened the Goodix `/dev/input/eventX`, and a human tap changes the text. |

### Validation & Error Matrix

| Check | Good Result | Failure Meaning |
|---|---|---|
| QML static contract | `test_qt_kms_overlay_assets.sh` finds `overlayControls`, the four labels, and `MouseArea` | The UI may regress to non-touch or missing controls |
| Process ownership | Tapping a QML button does not remove the `uvc_kms_overlay` PID | The UI layer is incorrectly controlling the KMS video process |
| Touch FD | `/proc/$(pidof qt_camera_display)/fd` includes `/dev/input/eventX` | Qt may render buttons but never receive Goodix touch events |
| Human tap | The visible workflow/status text changes for all four buttons | Coordinates, event routing, or button hit areas are wrong |

### Good / Base / Bad Cases

```qml
// Good: one handler owns the UI state transition and respects the backend boundary.
MouseArea {
    anchors.fill: parent
    onClicked: root.handleControlAction(modelData.action, modelData.state)
}
```

```qml
// Bad: a KMS-mode QML button tries to manage the external overlay process.
MouseArea {
    anchors.fill: parent
    onClicked: cameraView.running = false
}
```

### Tests Required

- Run `./test_qt_kms_overlay_assets.sh` after editing `Main.qml` or Qt camera scripts.
- Build the Qt app with the STM32MP157 SDK after QML resource changes.
- On the board, run `run_qt_kms_overlay_display.sh restart`, check the Qt input FD, and collect human tap confirmation for all four buttons.

---

## Qt Status-Bar Clock And Board Timezone

Use this convention when changing the STM32MP157 Qt top status bar, startup scripts, or any visible time shown by QML.

### Scope / Trigger

- Trigger: QML displays current time through `new Date()` or `Qt.formatDateTime(...)`.
- Trigger: fixing a mismatch where `board-time-sync status` and COS upload timestamps are correct, but the Qt screen still shows UTC.
- Runtime route: `run_qt_kms_overlay_display.sh` starts `run_qt_camera_display.sh`, which then `exec`s `qt_camera_display`.

### Signatures

| Operation | Signature |
|---|---|
| Qt entry default timezone | `qputenv("TZ", DEFAULT_BOARD_TIME_ZONE); tzset();` before `QGuiApplication app(...)` |
| Startup script timezone | `TZ="${TZ:-CST-8}"; export TZ` |
| QML clock source | `Qt.formatDateTime(new Date(), "hh:mm:ss")` |
| Process timezone proof | `tr '\0' '\n' < /proc/$(pidof qt_camera_display)/environ | grep '^TZ='` |
| Board comparison | `TZ=CST-8 date '+%H:%M:%S %Z'; date -u '+%H:%M:%S UTC'` |

### Contracts

| Area | Contract |
|---|---|
| Time source boundary | The Qt top bar reads the Qt process timezone through QML `new Date()`. It is separate from `defect-cos-upload`; fixing upload timestamps does not change an already running Qt process. |
| Default timezone | `main.cpp` must set `TZ=CST-8` before creating `QGuiApplication` when the caller did not provide `TZ`. Call `tzset()` immediately after setting `TZ` so libc and Qt see the same local-time rules. |
| Script inheritance | Both `run_qt_camera_display.sh` and `run_qt_kms_overlay_display.sh` must export `TZ="${TZ:-CST-8}"` so init, SSH, and manual starts inherit Beijing local time. |
| No QML hour math | Do not add or subtract eight hours in QML. If QML time is wrong, fix the process environment or entry initialization. |
| Manual override | Preserve a caller-provided `TZ` for debugging by using the `${TZ:-CST-8}` pattern instead of unconditionally overwriting it in shell. |

### Validation & Error Matrix

| Check | Good Result | Failure Meaning |
|---|---|---|
| Static contract | `./test_qt_kms_overlay_assets.sh` finds `DEFAULT_BOARD_TIME_ZONE`, `qputenv("TZ", DEFAULT_BOARD_TIME_ZONE)`, and `TZ="${TZ:-CST-8}"` in both startup scripts | A future edit can make the visible Qt clock drift back to UTC |
| Process environment | `/proc/$(pidof qt_camera_display)/environ` contains `TZ=CST-8` | The running Qt process inherited an old environment or an old script is deployed |
| Screen comparison | Top-bar time matches `TZ=CST-8 date` hour and differs from `date -u` by 8 hours | QML is using UTC, an old binary is running, or the system clock itself is wrong |
| Upload comparison | COS `captured_at/uploaded_at` and Qt top-bar time are in the same Beijing hour after a save | Upload helper and UI are using different time sources |

### Good / Base / Bad Cases

```cpp
/* Good: 设置 Qt 进程默认业务时区，再创建 QGuiApplication。 */
if (qEnvironmentVariableIsEmpty("TZ")) {
    qputenv("TZ", DEFAULT_BOARD_TIME_ZONE);
}
tzset();
QGuiApplication app(argc, argv);
```

```sh
# Base: 启动脚本给 Qt 进程继承北京时间，同时允许临时 TZ 覆盖。
TZ="${TZ:-CST-8}"
export TZ
```

```qml
// Bad: 不要在 QML 里手工加 8 小时；这会和系统时区修复叠加。
currentTimeText = Qt.formatDateTime(new Date(Date.now() + 8 * 3600 * 1000), "hh:mm:ss")
```

### Tests Required

- Run `./test_qt_kms_overlay_assets.sh` after changing `main.cpp`, `Main.qml`, `run_qt_camera_display.sh`, or `run_qt_kms_overlay_display.sh`.
- Cross-build `qt_camera_display` after changing `main.cpp` or QML resources.
- Deploy both the new binary and startup scripts; updating only scripts is not enough for direct `qt_camera_display` runs.
- On the board, restart the Qt KMS overlay route, then compare `/proc/$(pidof qt_camera_display)/environ`, `TZ=CST-8 date`, `date -u`, and the LCD top bar.
- If a save/upload is also involved, compare the screen clock with `/tmp/defect-cos-record*.json` and `/tmp/defect-cos-register-*.json` before editing cloud frontend/backend code.

### Wrong vs Correct

#### Wrong

```text
COS upload time is now correct but the Qt screen still shows UTC, so change defect-cos-upload again.
```

#### Correct

```text
Qt screen time comes from the qt_camera_display process environment. Check the running process TZ and startup scripts first; upload-helper TZ only affects API payload timestamps.
```

---

## STM32MP157 Defect Model Training And Deployment Contract

Use this convention when the user mentions the defect inspection model, defect detection model, industrial defect model, or MP157 model deployment in this workspace.

### 1. Scope / Trigger

- Trigger: discussing, training, exporting, quantizing, deploying, or documenting the industrial defect inspection model for STM32MP157.
- Durable source-of-truth project: `D:\model_picture`.
- Human-readable model status document: `D:\model_picture\模型目录评估与MP157部署建议.md`.
- Target board: STM32MP157DAA1 with dual Cortex-A7 CPU and no NPU, so the production path must be CPU-friendly and preferably INT8.

### 2. Signatures

| Operation | Signature |
|---|---|
| Restore model project context | `cd /d D:\model_picture` |
| Classification dataset layout | `datasets_classify\train\bad`, `datasets_classify\train\good`, `datasets_classify\val\bad`, `datasets_classify\val\good` |
| Classification train script | `D:\model_picture\defect-unet\python.exe train_classify.py` |
| Classification ONNX export | `D:\model_picture\defect-unet\python.exe export_classify_onnx.py` |
| Classification INT8 quantization | `D:\model_picture\defect-unet\python.exe quantize_classify_int8.py` |
| Classification inference script | `D:\model_picture\defect-unet\python.exe infer_classify.py --model <model.onnx> --image <image>` |
| Classification unit tests | `D:\model_picture\defect-unet\python.exe -m unittest tests.test_infer_classify tests.test_infer_camera_onnx -v` |
| Current four-class classification INT8 ONNX | `D:\model_picture\checkpoints_classify_4classes_v2\defect_classifier_static_mixed_int8.onnx` |
| Current two-class segmentation FP32 ONNX | `D:\model_picture\checkpoints_unet_2parts\scratch_unet.onnx` |
| Current two-class segmentation INT8 ONNX | `D:\model_picture\checkpoints_unet_2parts\scratch_unet_decoder_head_int8.onnx` |
| Segmentation quantization | `D:\model_picture\defect-unet\python.exe quantize_segment_int8.py --preset decoder_head --onnx_input .\checkpoints_unet_2parts\scratch_unet.onnx --onnx_output .\checkpoints_unet_2parts\scratch_unet_decoder_head_int8.onnx --calib_dir .\datasets_unet_2parts\val\images --num_calib 60` |

### 3. Contracts

| Area | Contract |
|---|---|
| Default project meaning | In this STM32MP157 workspace, “检测缺陷模型” means the Windows model-training project at `D:\model_picture` unless the user explicitly names another path. |
| Current recommended board path | Use the four-class MobileNetV3-Small mixed INT8 classifier together with the two-class mixed INT8 UNet. The Qt detection transaction runs each model once for a saved ROI instead of running either model on every camera frame. |
| Classification label order | The deployed classifier labels are `splitwasher_bad`, `splitwasher_good`, `washer_bad`, and `washer_good`; `defect_classify.cpp` must read all labels and require their count to match the ONNX output dimension. |
| Classification model state | The current v2 four-class classifier is `static_mixed` INT8 with SHA256 `1637c31846cc4efb589a06eaf65a8db0969bb29c9259c458863c274ddca934a3`. It retains the approved flat-washer/split-washer label order; the removed black waveform part must not reappear in current result labels. |
| Segmentation model state | The current real-part UNet uses `background=0` and `defect=1`, input `[1,3,224,224]`, and output `[1,2,224,224]`. Its mixed INT8 artifact is 6,434,978 bytes; its 61-image test metrics are mIoU 79.23%, defect IoU 58.91%, and defect F1 74.14%. |
| Dynamic segmentation output | `defect_segment.cpp` must derive class count and mask dimensions from the only ONNX output tensor, require `tensor(float)` with shape `[1,C,224,224]`, use the derived values for argmax/palette/overlay, and emit the derived count in `RESULT_SEG classes=`. Never replace fixed six classes with fixed two classes. |
| Board runtime policy | Classification and segmentation coexist inside one explicit detection transaction, but neither model runs continuously on camera frames. This keeps the Cortex-A7 workload bounded and gives every history record one classification result plus one segmentation result. |
| Documentation policy | When model status changes, update both `D:\model_picture\模型目录评估与MP157部署建议.md` and the STM32MP157 project documents that describe deployment strategy. |
| Legacy Severstal boundary | Legacy `datasets_severstal` models remain flow-validation assets only. The current board segmentation source must be the real-part `checkpoints_unet_2parts/scratch_unet_decoder_head_int8.onnx` artifact. |
| Known conveyor-reflection limit | The current two-class model may label reflective regions on the black conveyor as defects. Quantization does not fix FP32 false positives; this deployment preserves the existing runtime `segmentMinPixels` setting and does not add post-processing. The direct board acceptance command uses 80 pixels explicitly and must not be confused with changing the persisted Qt setting. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning |
|---|---|---|
| `git -C D:\model_picture status --short --branch` | Shows the current model-project branch and local modifications before editing | Edits may overwrite unreviewed training or inference changes |
| `Get-ChildItem D:\model_picture\datasets_classify -Recurse` | Shows non-empty `train/val/good/bad` image sets before classification training | Training would create an empty or meaningless classifier |
| Classification contract test | Static/runtime checks prove ONNX output count equals the four labels and GOOD/BAD grouping follows label suffixes | A model replacement can silently reverse or omit a product class |
| Segmentation metadata check | ONNX Runtime reports input `tensor(float) [1,3,224,224]` and output `tensor(float) [1,2,224,224]` | The helper can interpret the output buffer with the wrong element width, read past a tensor boundary, or produce a meaningless mask |
| Quantization comparison | FP32 mIoU 79.39% versus INT8 mIoU 79.23%; defect IoU 59.22% versus 58.91% | A broader quantization preset caused unacceptable segmentation drift |
| Board segmentation smoke test | Direct helper output contains `RESULT_SEG`, `classes=2`, and nonempty raw/overlay/mask files | Model/helper versions are mismatched or output files did not complete |
| Board benchmark | Measures capture, preprocess, inference, postprocess, and Qt overlay time separately | “Can run” is not enough to prove it will meet the inspection beat |
| Result acceptance | GOOD/BAD/UNCERTAIN output is checked against real part photos, not only synthetic or Severstal data | The system may look functional but fail on the target hardware object |

### 5. Good / Base / Bad Cases

```text
Good: Capture one centered ROI, run the four-class INT8 classifier once, run the two-class INT8 UNet once,
then fuse both results and save the raw/overlay/mask evidence under the same history transaction.
```

```text
Base: Keep the stable board filename `defect_unet_test_decoder_head_int8.onnx`, but verify its SHA256 matches the current
`scratch_unet_decoder_head_int8.onnx` content and require `RESULT_SEG classes=2`.
```

```text
Bad: Run classification and segmentation on every camera frame on MP157, then judge the board too slow without separating model cost from UI and camera cost.
```

### 6. Tests Required

- Before changing classification inference logic, run the model project's focused classifier tests and the Qt module static contract test.
- Before replacing segmentation output handling, make the static contract test fail on fixed `MODEL_CLASS_COUNT=6`, then implement `tensor(float) [1,C,H,W]` validation and rerun it.
- Before board deployment, evaluate FP32 and INT8 on the same `datasets_unet_2parts/test` split and verify the INT8 output remains `[1,2,224,224]`.
- On the board, wait for the synchronous helper process to exit, require each reported raw/overlay/mask path to exist and be nonempty, then run `sync`; fixed sleep is not completion proof.
- Record classification-side hashes before a segmentation-only deployment and prove they are unchanged afterward.
- Update the model status document and this workspace's STM32MP157 deployment notes whenever the trained model, class order, dataset source, model size, or runtime policy changes.

### 7. Wrong vs Correct

#### Wrong

```text
pred_class == 1 means BAD, so draw a red box and send the part to the bad tray.
```

#### Correct

```text
ImageFolder currently maps bad=0 and good=1. Use BAD_CLASS_INDEX or is_bad_prediction(pred_class), then show result_name(pred_class) so label-order changes are centralized and testable.
```

#### Wrong

```text
The new ONNX exists, so copying only the model is enough even though `defect_segment.cpp` still assumes six output channels.
```

#### Correct

```text
Validate the real-part FP32 and INT8 metrics, update `defect_segment.cpp` to derive `[1,C,H,W]` dynamically, cross-build the ARM helper,
back up the old helper/model, atomically replace both, and accept the deployment only after board-side `classes=2`, file, hash, and service checks pass.
```

---

## Qt Detection Information Full Explanation Contract

### 1. Scope / Trigger

- Trigger: board-side history detail screens show cloud review reasons, fused model explanations, UNet hints, model raw output lines, upload status, or image-retention diagnostics.
- Trigger: text can be longer than the 1024x600 detail panel can hold.
- Affected screen: `20_uvc_camera/qt_camera_display/qml/Main.qml` history detail page.
- The fixed right-side history panel must remain readable without truncating the only copy of cloud/operator text.

### 2. Signatures

| UI / Function | Required Marker |
|---|---|
| Short summary state | `historyAnalysisSummaryText(record)` |
| Full explanation text | `historyFullAnalysisText(record)` |
| Open control | `查看完整说明` |
| Full explanation overlay | `historyAnalysisDetailOverlay` |
| Scroll owner | `analysisDetailFlickable` |
| Visibility state | `historyAnalysisDetailVisible` |
| Static contract test | `test_qt_kms_overlay_assets.sh` checks these markers |

### 3. Contracts

| Boundary | Contract |
|---|---|
| Fixed panel | The compact history detail panel shows a short summary only. It may use `maximumLineCount`, tight `lineHeight`, and `clip: true`, but it must not be the only place containing the complete text. |
| Full text access | Any long cloud review reason, UNet hint, fused reason, model raw output, or upload diagnostic must be available through a full explanation overlay or page. |
| Scroll behavior | The full explanation surface must use a bounded scroll owner such as `Flickable`; long text must be readable to the end on the 1024x600 board screen. |
| Meaning preservation | Short text may compress wording, but must not hide result-changing facts such as cloud corrected result, board original result, operator reason, or UNet defect hint. |
| Part label display | Narrow panels use compact part names. `gasket` and `gasket_good/gasket_bad` display as `波形垫圈`; `washer` displays as `平垫圈`; `splitwasher` displays as `弹性垫圈`. Do not show the misleading literal `垫片` for legacy `gasket`. |
| Binary verification | QML resource compression may be disabled for deploy verification when marker strings must be checked with `strings` in the built binary. |

### 4. Validation & Error Matrix

| Check | Good Result | Bad Result | Required Action |
|---|---|---|---|
| Short panel content | Shows result, confidence, defect hint, and `查看完整说明` | Shows a long paragraph clipped at the bottom | Move long text to the full explanation surface and keep a short summary |
| Full overlay | `historyAnalysisDetailOverlay` opens and contains scrollable `analysisDetailFlickable` | No way to read hidden cloud reason | Add a full explanation overlay/page before shipping |
| Cloud reason | Full text includes `云端修正` and the operator-entered reason | Only first line appears in the fixed panel | Preserve full cloud review text in `historyFullAnalysisText()` |
| Model raw text | Raw classification/segmentation details remain available for diagnostics | Raw text is removed completely to fit the card | Put raw lines in the full explanation, not the compact card |
| Small-screen label | Part name fits as `波形垫圈`, `平垫圈`, or `弹性垫圈` | `gasket` becomes `垫片` or text overflows the result panel | Apply compact identity mapping before rendering |

### 5. Good / Base / Bad Cases

| Case | Example | Expected Result |
|---|---|---|
| Good cloud correction | Cloud reason is 300+ Chinese characters | Compact panel shows short summary; `查看完整说明` opens a scrollable full explanation containing the full reason |
| Good model diagnostics | UNet and classification raw output are both present | Operator sees a human summary first, and diagnostics remain available in full explanation |
| Base short record | No cloud correction and only normal model output | Compact panel is sufficient, but full explanation still opens with available fields |
| Bad clipped text | The fixed panel directly renders full cloud reason with `clip: true` | The operator cannot understand why cloud changed the result |
| Bad misleading label | Legacy `gasket` shows as `垫片` | User thinks the wrong physical part was detected |

### 6. Tests Required

- Run `./test_qt_kms_overlay_assets.sh` after any change to history detail QML, QML resource packaging, or compact label mapping.
- Assert the test checks `historyAnalysisSummaryText`, `historyFullAnalysisText`, `historyAnalysisDetailOverlay`, `analysisDetailFlickable`, and `查看完整说明`.
- On the board, open a record with a long cloud review reason:
  - assert the compact panel does not overflow or hide surrounding UI;
  - tap `查看完整说明`;
  - assert the full explanation scrolls to the last line;
  - assert cloud correction result, board original result, operator reason, UNet hint, and model raw output are present.
- When verifying deployed binaries, run `strings /root/qt_camera_display/qt_camera_display | grep -E 'historyAnalysisDetailOverlay|查看完整说明|波形垫圈'`.

### 7. Wrong vs Correct

#### Wrong

```qml
Text {
    clip: true
    text: root.historyFullAnalysisText(root.selectedHistoryRecord)
}
```

This clips the only copy of the full cloud/model explanation.

#### Correct

```qml
Text {
    maximumLineCount: 4
    text: root.historyAnalysisSummaryText(root.selectedHistoryRecord)
}

Flickable {
    id: analysisDetailFlickable
    contentHeight: fullAnalysisText.height
    Text {
        id: fullAnalysisText
        text: root.historyFullAnalysisText(root.selectedHistoryRecord)
    }
}
```

#### Wrong

```text
gasket -> 垫片
```

#### Correct

```text
gasket -> 波形垫圈
washer -> 平垫圈
splitwasher -> 弹性垫圈
```

---

## Scenario: Qt Alarm And Settings Detail Overlay Geometry Contract

Use this convention when a fixed STM32MP157 Qt/QML card shows a compact alarm, maintenance, device-health, visual-detection strategy, F4 boundary, camera/storage setting, or any setting summary with a `查看全部`, `查看完整说明`, or `查看详情` control.

### 1. Scope / Trigger

- Trigger: adding a detail button to `20_uvc_camera/qt_camera_display/qml/Main.qml` cards such as `告警维护`, `处理建议`, `参数设置`, `视觉检测策略`, `F4接入边界`, or device-health panels.
- Trigger: moving complete text from a compact card into a secondary page, overlay, or `Flickable`.
- Trigger: a board photo shows a `查看详情` button covering the last summary row, status text, or another card element.
- Trigger: two detail entries reuse one overlay and one scroll owner, and opening the second detail starts in the middle because the first detail was previously scrolled.
- Goal: compact cards must keep essential status visible and tappable, while complete explanations remain reachable without geometry overlap or stale scroll position.

### 2. Signatures

| UI / Function | Required Marker / Shape |
|---|---|
| Alarm detail opener | `openAlarmAdviceDetail()` or an equivalent page-specific opener |
| Alarm full text | `alarmAdviceDetailFlickable` owns the scrollable full advice text |
| Settings detail opener | `openSettingsDetail(detailKey)` |
| Settings full text | `settingsDetailFlickable` owns the scrollable visual-strategy or F4-boundary text |
| Scroll reset | `settingsDetailFlickable.contentY = 0` after changing detail content |
| Compact card geometry | Summary rows end before a reserved bottom action row |
| Bottom action row | Left status text has bounded width; right detail button has fixed width/height and right margin |
| Static contract | `test_qt_kms_overlay_assets.sh` checks detail markers, removed obsolete rows, reserved button geometry, and scroll reset |

### 3. Contracts

| Boundary | Contract |
|---|---|
| Card capacity calculation | Before adding a button or one more summary line, calculate the vertical budget with real QML values: `summaryEndY = summaryY + rowHeight * rowCount + spacing * (rowCount - 1)`. The action row must start after `summaryEndY + safeGap` and must still fit before `card.height - bottomMargin`. |
| Button reservation | A detail button is not allowed to float over existing content. Reserve an explicit row or column for it. In narrow cards, use left status text plus right fixed-size button instead of stacking the button on top of the summary. |
| Summary vs full text | The card shows only short operator-critical facts. Long maintenance advice, cloud upload explanations, visual-detection strategy, and F4 electrical/control boundaries belong in a scrollable detail overlay/page. |
| Shared overlay state | When different entries share one detail overlay or one `Flickable`, each opener must set the title/body first, show the overlay, and reset `contentY` to `0` on the next event loop tick with `Qt.callLater()` or an equivalent post-layout hook. |
| Independent scroll expectation | Scrolling one detail entry must not affect the initial position of another entry. Every detail open operation starts from the top unless the product intentionally stores per-entry scroll state. |
| Obsolete configuration removal | Do not keep unused rows just to fill a card. If the current hardware plan has no fill light, remove `补光` from `相机、光源与存储` and use the freed budget for active camera, storage, or upload settings. |
| Device-health truth source | Device health cards should show real board checks. For the 4G card, prefer the actual 4G/PPP online probe state instead of a generic or placeholder configuration label. |
| Cloud-contract wording | Parameter detail text should be based on the MP157-to-cloud data contract: include record identity, source/annotated images, local retention, COS upload, retry/offline behavior, and backend-visible state when those are part of the screen's responsibility. |
| F4 boundary wording | F4 detail text should make ownership clear: MP157 displays and uploads inspection results, while F4 and the linked sensors/actuators own motion, weight, inductance, limit, emergency-stop, or sorting control as applicable. Qt must not imply it directly controls the F4 motion path unless that control path exists. |
| `clip` limitation | `clip: true` can prevent drawing outside the card, but it is not evidence that the button, final line, or status text is visible. Fit must be proven by geometry and board inspection. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning | Required Action |
|---|---|---|---|
| Summary/action geometry | Last summary row, bottom status text, and `查看详情` button have non-overlapping rectangles | The new control covers content on the 1024x600 LCD | Reduce summary rows, reserve an action row, or move more text into detail |
| Button touch area | Detail button is fully inside the card and remains at least the designed fixed width/height | The fit fix made the button hard to tap | Rebalance card rows before shrinking the button |
| Detail full text | Long alarm advice, visual strategy, or F4 boundary text is readable to the final line in a `Flickable` | The compact card is still the only copy of required text | Add or repair the full detail overlay/page |
| Scroll reset | Open detail A, scroll halfway, close it, open detail B; detail B starts at the top | Shared `Flickable.contentY` leaked between detail entries | Reset contentY in every opener after content is assigned |
| Removed obsolete row | `相机、光源与存储` does not show `补光` when no fill-light plan exists | UI advertises an inactive configuration | Remove the row or mark it only in a future/disabled spec, not in the live card |
| 4G health label | Device health shows online/offline from the 4G probe path | Health card is a placeholder setting, not real status | Wire the card to the existing device-health controller/probe result |
| Static test | `./test_qt_kms_overlay_assets.sh` finds layout markers and scroll-reset markers | A future edit can reintroduce the overlap or stale scroll bug silently | Add marker checks for the changed card/overlay |
| Board inspection | On the deployed 1024x600 LCD, no detail button covers text and each detail opens from the first line | Source inspection missed board font metrics or runtime state | Fix QML geometry and retest on the board |

### 5. Good / Base / Bad Cases

```qml
// Good: fixed-height summary rows end before the reserved bottom action row.
Column {
    id: settingsVisionSummaryColumn
    x: 14
    y: 38
    spacing: 4
    Repeater {
        // Four rows fit the card; the complete explanation lives in the detail overlay.
        model: root.settingsVisionSummaryRows()
        delegate: Text {
            width: settingsVisionSummaryColumn.width
            height: 18
            maximumLineCount: 1
            elide: Text.ElideRight
        }
    }
}

Text {
    id: settingsVisionStatusText
    x: 14
    y: 132
    width: parent.width - 120
    maximumLineCount: 1
    elide: Text.ElideRight
}

Rectangle {
    id: settingsVisionDetailButton
    width: 86
    height: 26
    anchors.right: parent.right
    anchors.rightMargin: 14
    y: 132
}
```

```qml
// Good: a shared settings detail overlay always opens from the top.
function openSettingsDetail(detailKey) {
    settingsDetailTitle = root.settingsDetailTitle(detailKey)
    settingsDetailText = root.settingsDetailBody(detailKey)
    settingsDetailVisible = true
    Qt.callLater(function() {
        settingsDetailFlickable.contentY = 0
    })
}
```

```qml
// Bad: five summary rows plus a bottom-right button can overlap in a 170px card.
Column {
    y: 38
    spacing: 7
    Repeater { model: root.settingsF4BoundaryRows() }
}

Rectangle {
    anchors.right: parent.right
    anchors.bottom: parent.bottom
    anchors.bottomMargin: 10
}
```

### 6. Tests Required

- Run `./test_qt_kms_overlay_assets.sh` after changing alarm advice, device health, parameter settings cards, detail overlays, or QML scroll owners.
- Add static assertions for every fragile marker introduced by the change: detail opener, detail `Flickable`, action-row geometry marker, removed inactive row, and `contentY = 0`.
- Run `git diff --check` after editing QML, shell tests, README, plans, or spec files.
- For QML changes, cross-build `qt_camera_display`, verify `rcc -name qml` reruns, and confirm the ARM binary contains detail markers such as `查看详情`, `settingsDetailFlickable`, or `alarmAdviceDetailFlickable`.
- On the board, perform the overlap regression manually: open the changed page, inspect all cards at the 1024x600 LCD size, tap the detail button, scroll to the middle, close, open a different detail, and confirm it starts at the first line.

### 7. Wrong vs Correct

#### Wrong

```text
The card still has empty-looking space, so place `查看详情` at the bottom-right without recalculating the summary height.
```

#### Correct

```text
Calculate the summary rows and reserve a bottom action row first. If the rows and button do not fit together, shorten the summary and move full content into the detail overlay.
```

#### Wrong

```text
Two detail buttons share one Flickable, so the second detail can keep the first detail's scroll position.
```

#### Correct

```text
Every detail opener resets the shared Flickable to the top after replacing its content, so each entry starts from the first line.
```

#### Wrong

```text
Keep `补光` in the settings card because the UI has room for one more line.
```

#### Correct

```text
Do not show inactive hardware configuration. Remove `补光` until the fill-light hardware and control path are actually planned.
```

---

## Scenario: MP157-F407 Actuator Axis Contract

### 1. Scope / Trigger

- Trigger: changing `20_uvc_camera/qt_camera_display/qml/Main.qml`, `20_uvc_camera/qt_camera_display/main.cpp`, `docs/stm32mp157-f407-binary-protocol.md`, `docs/stm32mp157-f407-auto-detect-debug-roadmap.md`, or F407 `binary_protocol_service` / `camera_motor_service` files around automatic ROI fine tuning, manual motor control, or stepper parameter settings.
- Hardware decision: the former camera forward/backward motor channel is now the camera lateral axis. Forward/backward fine tuning belongs to the conveyor motor.

### 2. Signatures

| Boundary | Signature / Marker |
|---|---|
| MP157 position command | `sendF4ActuatorPositionMove(actuator, direction, mode, speedRpm, stepsValue, flags)` |
| MP157 velocity command | `sendF4ActuatorVelocityMove(actuator, direction, speedRpm, flags)` |
| Conveyor fine tune | `autoVisionFineTuneConveyor(errorY)` sends `ACTUATOR_POS_MOVE actuator=0` |
| Lateral fine tune | `autoVisionFineTuneLateral(errorX)` sends `ACTUATOR_POS_MOVE actuator=1` |
| Fine tune step scaling | `autoVisionFineTuneStepsForError(errorPixels, motorMinStep, axisName)` converts ROI pixel error into the `steps` field |
| Lateral return offset | `autoVisionLateralReturnOffsetSteps` stores signed successful lateral movement in the current automatic cycle |
| Lateral return command | `autoVisionRequestLateralReturnToBeltCenter()` sends reverse `ACTUATOR_POS_MOVE actuator=1` only when `autoVisionLateralReturnOffsetSteps !== 0` |
| Stepper role mapping | `camera_lateral` maps to F4 role id `2`; legacy `camera_forward` may map to the same role only for compatibility |
| F4 dispatch | `BINARY_PROTOCOL_ACTUATOR_CAMERA_LATERAL == 1` dispatches to `CameraMotorService_RequestLateral...()` |
| F4 text debug | `CAMLAT LEFT/RIGHT` is the primary text command; `CAMFWD` is a compatibility alias only |

### 3. Contracts

| Area | Contract |
|---|---|
| Actuator numbering | Keep protocol numbers stable: `0=conveyor`, `1=camera lateral`, `2=camera Z`, `0xFF=all stop`. Do not renumber legacy frames. |
| Address defaults | Conveyor uses `UART4 PC10/PC11 addr=0x01`; camera lateral uses `USART6 PC6/PC7 addr=0x03`; camera Z uses `USART6 PC6/PC7 addr=0x02`. |
| Automatic fine tuning | After Z down and focus settle, `errorY` belongs to conveyor forward/backward fine tune, while `errorX` belongs to camera lateral left/right fine tune. |
| Lateral direction sign | Camera lateral motion changes the image in the opposite direction. When `errorX > 0`, the part is right of the ROI center and MP157 must send `direction=1` so the camera moves right and the image shifts left toward center. |
| Fine tune steps | ROI fine tune must not keep `steps` fixed at `motor.minStep`. Compute `steps` from the amount that `abs(errorX/errorY)` exceeds the ROI tolerance, use the configured pixel scale, clamp it to a bounded maximum, and show `steps`, `minStep`, and `direction` in the operator status text so no-motion or wrong-direction symptoms can be diagnosed. |
| No-improvement escalation | When repeated LOCATE results on the same axis show no meaningful error reduction, increase the next fine tune step within the configured cap. This distinguishes "small step was invisible" from "F4 or the motor never moved." |
| Lateral return to belt baseline | Lateral fine tuning moves the camera relative to the conveyor. Accumulate only successful `fine-tune-lateral` movements after F4 completion, then after model detection and Z-up send one reverse lateral position command before `MODEL_READY/ARM_JOB_START`. If the current cycle did not move lateral, skip this return command. |
| Manual UI | The three motor pages must display conveyor, camera lateral, and camera Z. Conveyor/lateral pages use continuous velocity mode plus explicit stop; Z uses fixed-step position mode. |
| Settings UI | The second stepper settings page must be named camera lateral. Saved JSON may keep numeric user fields, but visible name/role/serial should come from normalized defaults. |
| Docs | Core protocol and roadmap docs must not describe actuator `1` as camera forward/backward except when explicitly naming legacy compatibility aliases. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning | Required Action |
|---|---|---|---|
| Static contract | `./test_qt_kms_overlay_assets.sh` finds `cameraLateralMotor.address = 3`, `autoVisionFineTuneConveyor`, `autoVisionFineTuneLateral`, `ACT_CAMERA_LATERAL`, and `lateral_addr=3` | UI, protocol docs, or F4 code drifted back to the old axis model | Update all layers together before deploying |
| Manual lateral move | QML sends `ACTUATOR_VEL_MOVE actuator=1 direction=0/1`, F4 calls `CameraMotorService_RequestLateralJog()` | Manual page still controls the old forward/backward abstraction | Rename UI text and dispatch to lateral service |
| Automatic Y fine tune | QML sends `ACTUATOR_POS_MOVE actuator=0` when `errorY` exceeds tolerance | Conveyor is not doing forward/backward correction after Z down | Route Y-axis correction through conveyor settings |
| Automatic X fine tune | QML sends `ACTUATOR_POS_MOVE actuator=1` when `errorX` exceeds tolerance | Lateral fine tuning is missing or uses the wrong actuator | Route X-axis correction through camera lateral settings |
| Step scaling regression | `./test_qt_kms_overlay_assets.sh` rejects `var steps = Math.max(1, Math.floor(Number(motor.minStep || 1)))` inside conveyor/lateral fine tune blocks and requires `autoVisionFineTuneStepsForError(errorY/errorX, ...)` | The UI can keep printing "fine tuning" while the motor only receives a barely visible fixed minimum step | Restore pixel-error-based step scaling and include the operator-visible `steps/minStep` values |
| Lateral direction regression | `./test_qt_kms_overlay_assets.sh` rejects `var direction = errorX > 0 ? 0 : 1` and requires `errorX > 0 ? 1 : 0` | The part appears right of ROI but the camera moves in the direction that pushes the image farther away | Restore the camera-motion/image-motion inversion in QML or adjust F4 runtime direction mapping after proving QML sends the intended direction |
| Lateral return regression | `./test_qt_kms_overlay_assets.sh` finds `autoVisionLateralReturnOffsetSteps`, successful fine-tune commit, `lateral-return`, and the pre-arm return gate | Camera can remain shifted after one part, so the green ROI no longer aligns with the black conveyor baseline on the next cycle | Accumulate successful lateral moves and send a reverse return before starting the arm flow; skip return when offset is zero |
| Hardware no-motion triage | During field testing, the bottom prompt shows larger `steps` when `errorX/errorY` is large; if the ROI error still never changes, F4 logs must show whether `ACTUATOR_POS_MOVE actuator=0/1` arrived and whether EMM42 returned motion status | The issue is no longer MP157 step sizing; it is likely F4 firmware, motor address, enable, direction, wiring, power, common ground, or driver response | Check F4 receive logs, `CAMINFO`, motor addresses `0x01/0x03`, EMM42 enable, direction mapping, and physical wiring |
| F4 runtime config | `CAMINFO` prints `lateral_addr=3, z_addr=2` | F4 still runs old firmware or MP157 sent stale stepper settings | Rebuild/burn F4 and re-send stepper settings |

### 5. Good / Base / Bad Cases

```qml
// Good: Y-axis ROI error is handled by the conveyor.
autoVisionFineTuneConveyor(errorY)
steps = autoVisionFineTuneStepsForError(errorY, minStep, "conveyor")
deviceHealth.sendF4ActuatorPositionMove(0, direction, 0, speed, steps, 0)
```

```qml
// Good: X-axis ROI error is handled by the camera lateral axis.
autoVisionFineTuneLateral(errorX)
steps = autoVisionFineTuneStepsForError(errorX, minStep, "lateral")
direction = errorX > 0 ? 1 : 0
recordAutoVisionPendingLateralFineTune(direction, steps)
deviceHealth.sendF4ActuatorPositionMove(1, direction, 0, speed, steps, 0)
```

```qml
// Good: after Z-up, return the camera to the conveyor baseline only if lateral actually moved.
if (autoVisionLateralReturnOffsetSteps !== 0) {
    autoVisionRequestLateralReturnToBeltCenter()
} else {
    autoVisionStartF4ArmInspectionAfterZUp()
}
```

```text
Base: legacy JSON role `camera_forward` may still map to role id 2 so old config files do not break immediately, but all new UI labels and docs must say camera lateral.
```

```text
Bad: after Z down, sending every small ROI offset to actuator=1 and documenting it as camera forward/backward. This leaves conveyor Y correction unused and makes the UI lie about the physical axis.
```

```text
Bad: after Z down, always send `steps=minStep` even when `errorX/errorY` is far outside the ROI tolerance. On real hardware this can look like "fine tune attempt N" while the part never visibly moves.
```

```text
Bad: leave the camera at the lateral fine-tuned position after model detection, then start the next cycle with the green ROI no longer aligned to the conveyor edges.
```

### 6. Tests Required

- Run `cd 20_uvc_camera/qt_camera_display && ./test_qt_kms_overlay_assets.sh` after any QML, settings, protocol-doc, or F4 motor-service edit.
- Run `git diff --check` in the Windows repository and in `E:/hal/bisai_f407_project`.
- Run the F4 host protocol test when changing `binary_protocol_service.c/.h` or its payload tests.
- On hardware, verify the exact commands: `ACTUATOR_POS_MOVE actuator=0` changes conveyor position, `ACTUATOR_POS_MOVE actuator=1` changes lateral position, and `CAMINFO` reports `lateral_addr=3, z_addr=2`.
- On hardware, force a part outside the ROI center after Z down and confirm the bottom status shows `steps` larger than `minStep` for large `errorX/errorY`; if `steps` grows but the ROI error remains unchanged, inspect F4 receive logs and motor wiring instead of changing MP157 scaling again.
- On hardware, force a lateral fine tune, finish detection, and confirm Z-up is followed by a reverse `ACTUATOR_POS_MOVE actuator=1` only when `autoVisionLateralReturnOffsetSteps` is nonzero; after that the green ROI should again roughly align with both black conveyor edges.

### 7. Wrong vs Correct

#### Wrong

```text
Z down -> ROI still offset -> actuator=1 forward/backward fine tune.
```

#### Correct

```text
Z down -> ROI still offset -> errorY uses conveyor actuator=0; errorX uses camera lateral actuator=1.
```

#### Wrong

```qml
var steps = Math.max(1, Math.floor(Number(motor.minStep || 1)))
deviceHealth.sendF4ActuatorPositionMove(0, direction, 0, speed, steps, 0)
```

#### Correct

```qml
var minStep = Math.max(1, Math.floor(Number(motor.minStep || 1)))
var steps = autoVisionFineTuneStepsForError(errorY, minStep, "conveyor")
deviceHealth.sendF4ActuatorPositionMove(0, direction, 0, speed, steps, 0)
```

#### Wrong

```text
Lateral fine tune moved the camera for this part, then Z-up immediately starts the arm flow and leaves the next camera view shifted from the conveyor.
```

#### Correct

```text
After Z-up, if the current cycle accumulated a successful lateral offset, send the reverse lateral position move first; if the offset is zero, skip the return and start the arm flow directly.
```

## Scenario: ROI Realtime Fine Tune Stop Safety Contract

### 1. Scope / Trigger

- Trigger: changing `20_uvc_camera/qt_camera_display/qml/Main.qml`, `test_qt_kms_overlay_assets.sh`, `main.cpp`, `docs/stm32mp157-f407-binary-protocol.md`, or F407 `binary_protocol_service.c` around ROI realtime fine tuning, dropped LOCATE frames, actuator velocity moves, timeout handling, or automatic model-detect handoff.
- Field bug learned: showing "ROI realtime fine tune timeout" on the MP157 screen is not enough. The timeout branch must actually send a stop frame that F4 accepts even when local state and F4 cycle state have drifted.

### 2. Signatures

| Boundary | Signature / Marker |
|---|---|
| MP157 realtime stop helper | `autoVisionStopRealtimeFineTune(reasonText, nextStage, forceAllActuators)` |
| MP157 dropped-frame branch | `handleAutoVisionFineTuneLocateFinished()` invalid LOCATE result branch |
| MP157 timeout property | `property int autoVisionRealtimeTuneTimeoutMs: 10000` |
| MP157 all-actuator stop | `deviceHealth.sendF4ActuatorStopNow(255, 0)` |
| F4 stop command | `ACTUATOR_STOP 0x51` payload `cycle_id, actuator, flags` |
| F4 all-actuator value | `BINARY_PROTOCOL_ACTUATOR_ALL == 0xFF` |
| F4 safety log marker | `ACTUATOR_STOP ignores cycle mismatch for safety` |

### 3. Contracts

| Area | Contract |
|---|---|
| Dropped LOCATE frames | A dropped or invalid LOCATE frame during realtime fine tune counts toward `autoVisionRealtimeTuneTimeoutMs`; do not reset the realtime fine-tune start timestamp on dropped frames. |
| Dropped-frame motion safety | If a dropped frame occurs while `autoVisionRealtimeFineTuneAxis` and `autoVisionRealtimeFineTuneSpeedRpm` show an active velocity move, MP157 must stop the current actuator before waiting for the next LOCATE result. |
| Timeout handoff | When elapsed realtime fine-tune time reaches 10000 ms, MP157 must stop micro-adjustment and enter automatic model detection via the `detect` stage. It must not stay in fine tune, focus settle, or manual review. |
| All-axis timeout stop | Timeout-to-detect branches must call `autoVisionStopRealtimeFineTune(..., "detect", true)` so QML sends `ACTUATOR_STOP_NOW actuator=255`. This handles stale local axis state and unknown physical motor state. |
| Resume after non-timeout drop | A single dropped frame before timeout should use `nextStage="resume"` after stopping the current actuator, preserving the same realtime fine-tune session and elapsed timer. |
| F4 cycle mismatch safety | F4 `ACTUATOR_STOP` must validate payload length, `flags`, and `actuator`, but it must not reject a valid stop solely because `cycle_id` mismatches. It should log the mismatch and still stop the requested actuator(s). |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning | Required Action |
|---|---|---|---|
| Static QML test | `./test_qt_kms_overlay_assets.sh` finds dropped-frame elapsed timeout checks, dropped-frame current-axis stop, `"detect"` timeout handoff, and `forceAllActuators` support | A future edit can reintroduce blind motion during dropped frames or timeout | Restore the dropped-frame stop block and timeout all-axis stop call |
| F4 source check | `rg "BINARY_PROTOCOL_ACTUATOR_ALL|ignores cycle mismatch for safety" E:\hal\bisai_f407_project\User\App\binary_protocol_service.c` finds both markers | MP157 may send all-axis STOP but F4 can still reject it due cycle mismatch | Restore F4 safety-stop behavior and rebuild/download F4 |
| Board binary marker | `sha256sum /root/qt_camera_display/qt_camera_display` equals the freshly built VM binary; `strings` finds `forceAllActuators` | Board is still running old QML embedded in an old binary | Rebuild Qt, replace `/root/qt_camera_display/qt_camera_display`, restart service |
| Field dropped-frame test | Occluding the target during realtime fine tune stops current motion and keeps elapsed time increasing | MP157 still lets velocity mode continue without visual feedback | Inspect `autoVisionStopRealtimeFineTune(..., "resume")` and F4 STOP logs |
| Field timeout test | Around 10 seconds after realtime fine tune starts, F4 receives all-axis STOP and MP157 enters model detection | Timeout is only a status message, not a control action | Check MP157 STOP_NOW write path, F4 cycle mismatch behavior, and board binary freshness |

### 5. Good / Base / Bad Cases

```qml
// Good: dropped frames count toward total timeout and stop active motion before resuming.
if (elapsedMs >= autoVisionRealtimeTuneTimeoutMs) {
    autoVisionStopRealtimeFineTune("ROI realtime timeout during dropped frame", "detect", true)
    return
}
if (autoVisionRealtimeFineTuneAxis !== "" && autoVisionRealtimeFineTuneSpeedRpm > 0) {
    autoVisionStopRealtimeFineTune("ROI dropped frame, stop current actuator", "resume")
    return
}
```

```c
/* Good: ACTUATOR_STOP remains a safety command even when cycle IDs drift. */
if ((payload.cycle_id != 0U) && (BinaryProtocolService_IsActiveCycle(payload.cycle_id) == 0U))
{
    my_printf(&huart1, "[WARN][PROTO] ACTUATOR_STOP ignores cycle mismatch for safety...\r\n");
}
/* Continue stopping requested actuator(s). */
```

```text
Bad: the QML timeout text says "enter model detection", but the STOP target is only the remembered axis and F4 rejects the stop because the cycle ID no longer matches.
```

### 6. Tests Required

- Run `cd 20_uvc_camera/qt_camera_display && ./test_qt_kms_overlay_assets.sh` after changing realtime fine-tune QML, timeout handling, dropped-frame handling, or F4 stop contracts.
- Run `git diff --check` in the Windows repository and in `E:/hal/bisai_f407_project` after editing MP157 or F4 files.
- For QML changes, cross-build `qt_camera_display`, verify the produced binary is ARM 32-bit, deploy it to the board, and verify the board hash matches the VM build hash.
- For F4 `ACTUATOR_STOP` changes, compile and download the F4 firmware manually, then confirm F4 logs show all-axis STOP and the cycle-mismatch safety warning only when a mismatch is intentionally forced.

### 7. Wrong vs Correct

#### Wrong

```text
Dropped LOCATE frame -> keep current ACTUATOR_VEL_MOVE running -> wait for target to reappear.
```

#### Correct

```text
Dropped LOCATE frame -> elapsed time still counts -> stop current actuator -> resume LOCATE polling without resetting the 10 second timer.
```

#### Wrong

```text
ACTUATOR_STOP with a stale cycle_id returns CYCLE_MISMATCH before stopping the motor.
```

#### Correct

```text
ACTUATOR_STOP validates flags and actuator, logs stale cycle_id, then stops the requested actuator(s) because stop is a safety command.
```

## Common Mistakes

- Forgetting to close the file descriptor on an error path.
- Changing ioctl command numbers in the driver without updating the user app.
- Accepting invalid timing values such as `0` or negative milliseconds.
- Returning success after a failed `write`, `read`, or `ioctl`.
- Compiling an STM32MP157 Qt application in a fresh shell without first sourcing the ST OpenSTLinux Qt/Wayland SDK environment script.
- Letting a non-Qt helper build script inherit ST SDK `CC`; use a script-specific variable such as `OVERLAY_CC` when the helper expects a plain compiler executable path.
- Treating `TextInput` plus `IntValidator` as proof that a touchscreen operator can enter arbitrary numeric values; on the board, critical numeric settings need in-app digit, backspace, and clear controls.
- Truncating an F4 calibration reply in C++ and then showing the only copy in a one-line QML label; preserve the diagnostic reply and put the full text in a scrollable result area.
- Reading `/proc/mounts` or other procfs files in Qt with ordinary file-size/EOF assumptions; use a stream parser such as POSIX `fopen/fscanf/fclose` and prove the result against shell output.
- Claiming QML touch support after only seeing buttons on the LCD; always prove Qt opened the Goodix input event node and confirm a human tap changes the visible status text.
- Treating a stuck-looking `minicom` cursor as proof of UART/RS485 receive failure before checking newline, line wrap, local echo, and raw RX hex output.
- Forgetting that this workspace's “检测缺陷模型” points to `D:\model_picture`, or treating the Severstal segmentation flow-validation model as the final real-part MP157 inspection model.
- Parsing a create-record JSON response with a generic `"id"` matcher; use a top-level record parser and a regression fixture with nested `part.id/device.id` so a new save cannot be registered under an old cloud record.
- Treating a correct COS upload timestamp as proof that the Qt screen clock is correct. The Qt top-bar clock comes from QML `new Date()` inside the `qt_camera_display` process, so always verify `/proc/$(pidof qt_camera_display)/environ` contains `TZ=CST-8`; fix `main.cpp` and the Qt startup scripts, not `defect-cos-upload`, backend time formatting, or QML `+8` hour arithmetic.
- Treating correct text content as sufficient on the 1024x600 Qt screen. Dynamic model names, fused reasons, cloud status, and diagnostic lines must have a display budget: short helper, fixed width with elide, bounded wrap, compact line height, and board-screen verification.
- Clipping the only copy of a cloud review reason or model diagnostic in a fixed-height history panel; keep short summaries in the panel and put full text in a scrollable detail overlay/page.
- Treating an added metric, button, or chart row as a data-only change. Fixed 1024x600 QML panels need a capacity calculation first; if the required height does not fit, use split columns, scrolling, summarization, or a detail page instead of relying on `clip: true`.
- Adding `查看全部` or `查看详情` to a fixed card without reserving a bottom action row; the button can cover the final summary/status line even when `clip: true` hides the overflow.
- Reusing one `Flickable` for multiple detail entries without resetting `contentY`; scrolling one detail can make the next detail open in the middle.
