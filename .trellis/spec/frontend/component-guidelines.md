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

## Qt Upload History Screen Contract

Use this convention when the STM32MP157 Qt camera UI records, displays, or reviews previous `保存图片` upload attempts.

### 1. Scope / Trigger

- Trigger: changing `20_uvc_camera/qt_camera_display/main.cpp`, `qml/Main.qml`, `uvc_kms_overlay.c`, or any save/upload path that should appear in the Qt `历史记录` page.
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
| Required JSON fields | `upload_time`, `result_text`, `workflow_text`, `jpg_path`, `png_path`, `upload_status`, `record_id`, `record_no`, `jpg_size_bytes`, `png_size_bytes` |
| QML list state | `historyDetailVisible == false`, `historyListPanel`, `historyListView` with horizontal `ListView` bound to `uploadHistory` |
| QML detail state | `historyDetailVisible == true`, `historyDetailPanel`, `selectedHistoryRecord`, `imageCarousel`, `backToHistoryList()` |
| Overlay hide/show | `VISIBLE 0` when entering history, `VISIBLE 1` when returning home |
| Cloud status summary | `compactUploadStatus(uploadResult)` in C++ and `cloudStatusSummary(rawStatus)` in QML |

### 3. Contracts

| Area | Contract |
|---|---|
| Append timing | Append a history record after local JPG/PNG save succeeds and the upload helper returns success or failure. Upload failure must still create a local history entry so the saved evidence can be reviewed. |
| Persistence | Write history JSON through a temporary file, flush, `fsync`, close, then rename. The history file lives beside the images so SD-card backup/removal keeps evidence and metadata together. |
| Self-test parity | `--storage-self-test` must use the same `CameraStorageController` and `UploadHistoryModel` path as the screen button, so SSH save tests appear in the same history page. |
| QML navigation | The left `历史记录` item switches `activePage` to `history` and resets `historyDetailVisible=false`; cards are ordered by append order and can scroll horizontally beyond screen width. |
| Delete behavior | A card-level `删除` command deletes both the JSON history entry and the local JPG/PNG files referenced by that entry. The model must write the new JSON successfully before deleting image files; if JSON persistence fails, restore the model row and return a `删除失败：...` message. |
| Delete safety boundary | Only delete regular files under the history file directory, currently `/mnt/sdcard/images`. Skip and log paths outside that directory instead of trying to be clever, because a damaged JSON path must not delete arbitrary board files. |
| Swipe feel | Horizontal history lists and image carousels should set bounded velocity/deceleration and cache neighboring pages so touch swipes feel continuous on STM32MP157. Avoid settings that let a light flick skip several records. |
| Selection without auto-scroll | Selecting a history card must only update visual selection state, such as comparing delegate `index` with `selectedHistoryIndex`. Do not bind the horizontal history `ListView.currentIndex` to `selectedHistoryIndex` or use `ListView.ApplyRange`, because Qt will automatically scroll the list to the selected item and make the UI look like it jumped from the beginning. |
| Two-level layout | The first layer must show only upload record cards and commands such as `查看`; the image carousel and detection details must be hidden until the user taps `查看`. Do not place the list and full detail view in the same visible layer on a 1024x600 screen. |
| Detail layout | The detail view shows the selected upload's images on the left with horizontal swiping, and detection result, cloud record info, file sizes, upload status, and local path on the right. It must provide `返回列表` without returning all the way to `首页`. |
| Overlay ownership | QML must not draw required history content under the KMS overlay video plane. Call `storageController.setOverlayVisible(false)` on history entry and `true` when returning home. |
| Cloud identity | `record_id` and `record_no` are parsed from the upload helper output only after the helper created an isolated cloud record. Do not invent IDs in QML. |
| Display text budget | List cards must not show raw file paths or raw upload script output. Long paths belong only in the detail page with `Text.ElideMiddle`; cloud status must be a short summary such as `上传成功 ID 21 MP157-...`. |
| Legacy status cleanup | Old `upload_status` values may contain mojibake or verbose script output. QML must derive display text from `record_id`/`record_no` tokens when possible instead of rendering the raw string. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning |
|---|---|---|
| Static contract | `./test_qt_kms_overlay_assets.sh` finds `UploadHistoryModel`, `upload_history.json`, `historyDetailVisible`, `historyListPanel`, `historyListView`, `historyDetailPanel`, `imageCarousel`, `backToHistoryList`, `deleteHistoryRecord`, `removeRecord`, `compactUploadStatus`, `cloudStatusSummary`, swipe tuning markers, and `VISIBLE` | The save-history-display-delete contract drifted between C++, QML, overlay, or docs |
| Qt build | `./build_qt_camera_display.sh` recompiles `main.cpp`, regenerates `qrc_qml.cpp`, and outputs an ARM ELF | C++ model, context property, or QML resource packaging is broken |
| Overlay build | `./build_uvc_kms_overlay.sh` outputs an ARM ELF with `VISIBLE` support | History page can still be covered by the live video plane |
| History JSON | `test -s /mnt/sdcard/images/upload_history.json` and `tail -n 40` show the newest paths and status | Save/upload completed but the UI has no durable history source |
| History UI list layer | Touch `历史记录`; only upload cards are visible, with no large image carousel or right-side detection panel | The list and detail layers are still collapsed together and will crowd/truncate text |
| History UI detail layer | Tap `查看`; detail view appears, images swipe on the left, detection/cloud/file data appears on the right, and `返回列表` returns to cards | Navigation, horizontal card overflow, image carousel, or detail-state transition is broken |
| Selection stability | Swipe to the middle of the history list, tap a visible card body, then continue swiping; the tapped card becomes highlighted but the list does not animate back from the first card or snap to the selected card | `currentIndex` or highlight-range binding is still coupled to selection state |
| Cloud status display | New history JSON has a short `upload_status`; old verbose/garbled entries display as clean status using extracted IDs | UI renders raw helper output, causing mojibake and clipped cards |
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
- On the LCD, open `历史记录`; verify the first layer shows only upload cards, then tap `查看`, swipe between JPG and PNG, tap `返回列表`, and finally return to `首页`.
- On the LCD, swipe the history list away from the first card, tap a visible card body to select it, and verify the list does not automatically scroll back or snap to the selected card.
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
| Recent rows helper | `function statsRecentRows()` in `qml/Main.qml` |
| Recent rows view | `statsRecentListView` vertical `ListView` inside `statsRecentPanel` |
| Detail handoff | `openHistoryDetailFromStats(index)` calls `switchPage("history")` and `showHistoryDetail(index)` |
| Static contract | `./test_qt_kms_overlay_assets.sh` checks `statsPageVisible`, `statsSummary`, `statsRecentBars`, `statsDistributionBars`, `statsRecentRows`, `statsRecentListView`, `openHistoryDetailFromStats`, and `statsPage` panel IDs |

### 3. Contracts

| Area | Contract |
|---|---|
| Data ownership | Statistics are derived from local upload history only. Do not add a second statistics file or a live cloud dependency unless the user explicitly asks for cloud-side analytics. |
| Field meaning | `total` means local history record count; `good` means records whose `resultText === "良品"`; `review` covers all non-good records; upload success is inferred from `recordId`, `recordNo`, or a success marker in `uploadStatus`. |
| Empty state | If `uploadHistory.count <= 0`, show a clear empty state telling the operator to save an image first; do not render empty axes or blank panels. |
| Chart implementation | Use lightweight QML rectangles for KPI cards, bars, and distribution strips. Do not introduce Qt Charts or another runtime dependency for this embedded 1024x600 screen without a separate design decision. |
| Screen density | Keep the page data-dense but scannable: top KPI row, middle trend/distribution panels, bottom recent records and cloud/file state panels. Avoid showing raw JSON, long file paths, or script output on the statistics page. |
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
| Current MobileNetV2 segmentation ONNX | `D:\model_picture\checkpoints\defect_unet.onnx` |
| Current MobileNetV3 segmentation ONNX | `D:\model_picture\checkpoints_mobilenetv3\defect_unet_mobilenetv3.onnx` |

### 3. Contracts

| Area | Contract |
|---|---|
| Default project meaning | In this STM32MP157 workspace, “检测缺陷模型” means the Windows model-training project at `D:\model_picture` unless the user explicitly names another path. |
| Current recommended board path | Use MobileNetV3-Small INT8 good/bad classification as the main MP157 runtime path. It answers whether the ROI is defective and should be fast enough for a CPU-only board after measurement. |
| Classification label order | The current ImageFolder-compatible label order is `bad=0`, `good=1`; inference code must read this through `CLASS_NAMES`, `BAD_CLASS_INDEX`, `GOOD_CLASS_INDEX`, `is_bad_prediction()`, `get_class_probability()`, and `result_name()` rather than hand-writing class-number meanings in scattered branches. |
| Classification model state | `datasets_classify` currently has the required directory shape, but the real good/bad image data and trained classification ONNX still need to be produced before claiming a board-ready classifier exists. |
| Segmentation model state | Existing UNet segmentation ONNX files are flow-validation assets from `datasets_severstal`, not final real-part models. The MobileNetV3-small UNet ONNX is about 13.70 MB; the MobileNetV2 UNet ONNX is about 25.27 MB. |
| Board runtime policy | Classification and segmentation may coexist on disk and inside the Qt/inspection workflow, but they should not both run for every frame on STM32MP157. Run classification as the main path, then trigger segmentation only for low-frequency visualization, uncertain samples, saved evidence, or manual review. |
| Documentation policy | When model status changes, update both `D:\model_picture\模型目录评估与MP157部署建议.md` and the STM32MP157 project documents that describe deployment strategy. |
| Severstal boundary | Do not present Severstal-trained segmentation output as proof that the final washer/stamping-part inspection model works; it proves only the code path, export path, and visualization path. |

### 4. Validation & Error Matrix

| Check | Good Result | Failure Meaning |
|---|---|---|
| `git -C D:\model_picture status --short --branch` | Shows the current model-project branch and local modifications before editing | Edits may overwrite unreviewed training or inference changes |
| `Get-ChildItem D:\model_picture\datasets_classify -Recurse` | Shows non-empty `train/val/good/bad` image sets before classification training | Training would create an empty or meaningless classifier |
| Classification mapping tests | `tests.test_infer_classify` passes and proves `bad=0`, `good=1` behavior | The classifier may report defective parts as good or reverse the UI color |
| Segmentation ONNX size check | MobileNetV3 UNet is preferred over MobileNetV2 UNet if segmentation is needed on MP157 | A larger segmentation model may make the UI feel stuck on Cortex-A7 |
| Board benchmark | Measures capture, preprocess, inference, postprocess, and Qt overlay time separately | “Can run” is not enough to prove it will meet the inspection beat |
| Result acceptance | GOOD/BAD/UNCERTAIN output is checked against real part photos, not only synthetic or Severstal data | The system may look functional but fail on the target hardware object |

### 5. Good / Base / Bad Cases

```text
Good: Use MobileNetV3-Small INT8 classification for the normal every-part decision.
If the classifier is uncertain or the operator asks for evidence, run the lighter segmentation model once and display/save the mask overlay.
```

```text
Base: Keep the existing MobileNetV3 UNet ONNX as a visualization and pipeline-validation asset while collecting real good/bad classification data.
```

```text
Bad: Run classification and segmentation on every camera frame on MP157, then judge the board too slow without separating model cost from UI and camera cost.
```

### 6. Tests Required

- Before changing classification inference logic, run `D:\model_picture\defect-unet\python.exe -m unittest tests.test_infer_classify tests.test_infer_camera_onnx -v`.
- Before claiming a classification model is trained, confirm `datasets_classify\train\good`, `datasets_classify\train\bad`, `datasets_classify\val\good`, and `datasets_classify\val\bad` contain real target-part images.
- Before board deployment, export ONNX, quantize to INT8 when possible, copy the chosen model into the NFS rootfs or application model directory, then benchmark on STM32MP157 with the same input size and preprocessing used by the Qt/inspection app.
- Before enabling segmentation in the live workflow, measure segmentation inference time separately from camera capture and Qt rendering, and decide whether it is manual/low-frequency only.
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
The 13.70 MB segmentation ONNX exists, so the final MP157 defect model is ready.
```

#### Correct

```text
The segmentation ONNX proves export/inference/overlay flow only. For the MP157 production decision, first collect real target-part data, train the MobileNetV3-Small good/bad classifier, quantize it, and benchmark it on the board.
```

---

## Common Mistakes

- Forgetting to close the file descriptor on an error path.
- Changing ioctl command numbers in the driver without updating the user app.
- Accepting invalid timing values such as `0` or negative milliseconds.
- Returning success after a failed `write`, `read`, or `ioctl`.
- Compiling an STM32MP157 Qt application in a fresh shell without first sourcing the ST OpenSTLinux Qt/Wayland SDK environment script.
- Letting a non-Qt helper build script inherit ST SDK `CC`; use a script-specific variable such as `OVERLAY_CC` when the helper expects a plain compiler executable path.
- Reading `/proc/mounts` or other procfs files in Qt with ordinary file-size/EOF assumptions; use a stream parser such as POSIX `fopen/fscanf/fclose` and prove the result against shell output.
- Claiming QML touch support after only seeing buttons on the LCD; always prove Qt opened the Goodix input event node and confirm a human tap changes the visible status text.
- Treating a stuck-looking `minicom` cursor as proof of UART/RS485 receive failure before checking newline, line wrap, local echo, and raw RX hex output.
- Forgetting that this workspace's “检测缺陷模型” points to `D:\model_picture`, or treating the Severstal segmentation flow-validation model as the final real-part MP157 inspection model.
- Parsing a create-record JSON response with a generic `"id"` matcher; use a top-level record parser and a regression fixture with nested `part.id/device.id` so a new save cannot be registered under an old cloud record.
- Treating a correct COS upload timestamp as proof that the Qt screen clock is correct. The Qt top-bar clock comes from QML `new Date()` inside the `qt_camera_display` process, so always verify `/proc/$(pidof qt_camera_display)/environ` contains `TZ=CST-8`; fix `main.cpp` and the Qt startup scripts, not `defect-cos-upload`, backend time formatting, or QML `+8` hour arithmetic.
