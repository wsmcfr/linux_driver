# Embedded Linux Configuration And Deployment Workflow

> Scope: STM32MP157 Linux kernel, device-tree, Buildroot rootfs, NFS deployment, local Windows GitHub upload, and VM Git commands.

---

## Scenario: Local Windows GitHub Upload Via GitHub CLI

### 1. Scope / Trigger

- Trigger: Pushing commits from the local Windows checkout at `C:\Users\caofengrui\Desktop\linux` to GitHub.
- Trigger: Any upload path that asks for a browser, popup, or manual approval during `git push`.
- Goal: Local GitHub uploads must use GitHub CLI as the Git credential provider so pushes do not require an extra approval click.
- Repository: `https://github.com/wsmcfr/bendi_linux.git`.
- Active GitHub account: `wsmcfr`.

### 2. Signatures

| Operation | Command Signature |
|---|---|
| Check GitHub CLI version | `gh --version` |
| Check GitHub CLI login | `gh auth status` |
| Bind Git credential helper to GitHub CLI | `gh auth setup-git` |
| Verify GitHub helper for HTTPS remotes | `git config --show-origin --get-all credential.https://github.com.helper` |
| Verify repository identity | `gh repo view --json nameWithOwner,url,defaultBranchRef` |
| Verify push path without upload | `git push --dry-run origin <branch>` |
| Push local branch | `git push origin <branch>` |

### 3. Contracts

| Area | Contract |
|---|---|
| Credential provider | For local Windows pushes to `github.com`, Git must resolve `credential.https://github.com.helper` to `gh.exe auth git-credential`. Do not rely on an interactive Git Credential Manager approval popup as the normal path. |
| Remote protocol | Keep the local repository remote as HTTPS for the GitHub CLI credential helper path: `https://github.com/wsmcfr/bendi_linux.git`. |
| Login account | `gh auth status` must show the active account as `wsmcfr` before pushing project work. |
| Permission scope | The GitHub CLI token must include `repo` scope for repository push operations. |
| Pre-push proof | Before the first push after credential changes, run `git push --dry-run origin <branch>` and require exit code `0`. |
| Local-only secrets | If a file must keep a real local-only secret, push only a sanitized placeholder version such as `<VM_PASSWORD>` and leave the real value as an uncommitted local working-tree difference after the push. Never commit the real secret. |

### 4. Validation & Error Matrix

| Check | Good Result | Bad Result | Required Action |
|---|---|---|---|
| GitHub CLI availability | `gh --version` prints a version | `gh` is not recognized | Install or repair GitHub CLI before attempting local GitHub upload |
| GitHub login | `gh auth status` shows `Logged in to github.com account wsmcfr` | Not logged in, wrong account, or missing token | Run `gh auth login` or switch account before pushing |
| Credential helper | `credential.https://github.com.helper` contains `gh.exe auth git-credential` | Only generic `manager` appears, or push asks for approval | Run `gh auth setup-git`, then recheck the helper |
| Dry-run push | `git push --dry-run origin <branch>` exits `0` | Authentication prompt, approval popup, or permission error | Fix GitHub CLI auth/helper setup before real push |
| Secret-safe commit | `git show HEAD:<path>` contains placeholders only | Commit contains a real password/token/private key | Stop; rewrite or revert before pushing |
| Local secret preservation | Working tree may show sanitized placeholder changed back to the local real value | Real value is staged or committed | Unstage it and stage only the sanitized version |

### 5. Good/Base/Bad Cases

| Case | Example | Expected Result |
|---|---|---|
| Good GitHub CLI push | `gh auth setup-git && git push --dry-run origin main && git push origin main` | Push succeeds through GitHub CLI credentials without manual approval |
| Good secret-safe upload | Commit `AGENTS.md` with `<VM_PASSWORD>`, then restore the local real value after push | GitHub history stays sanitized, local machine keeps the usable password |
| Base existing remote | `origin` remains `https://github.com/wsmcfr/bendi_linux.git` | GitHub CLI credential helper can serve the HTTPS remote |
| Bad interactive path | Push triggers an approval popup every time | The helper path is not using GitHub CLI; run `gh auth setup-git` |
| Bad secret commit | Commit `AGENTS.md` with the real VM password | Secret is exposed in GitHub history and must be reverted or history-cleaned |

### 6. Tests Required

- Local GitHub setup:
  - Assert `gh --version` exits `0`.
  - Assert `gh auth status` reports active account `wsmcfr`.
  - Assert `git config --show-origin --get-all credential.https://github.com.helper` includes `gh.exe auth git-credential`.
  - Assert `gh repo view --json nameWithOwner,url,defaultBranchRef` resolves `wsmcfr/bendi_linux`.
- Push verification:
  - Assert `git push --dry-run origin <branch>` exits `0` before claiming the GitHub upload path works.
  - After pushing, assert `git status --short --branch` shows the branch aligned with `origin/<branch>` except for intentional local-only secret differences.
- Secret safety:
  - Assert `git diff --cached` and `git show HEAD:<path>` do not contain real passwords or tokens before and after push.
  - If local-only secrets are restored after push, assert `git diff -- <path>` shows only the placeholder-to-local-secret difference.

### 7. Wrong vs Correct

#### Wrong

```powershell
# Push depends on Git Credential Manager and requires an approval popup.
git push origin main

# Real local-only secrets are staged and committed.
git add AGENTS.md
git commit -m "docs: add vm connection details"
git push origin main
```

#### Correct

```powershell
# Bind github.com Git credentials to GitHub CLI once on the local Windows machine.
gh auth setup-git

# Confirm the GitHub CLI credential helper is the github.com-specific path.
git config --show-origin --get-all credential.https://github.com.helper

# Verify the upload route before the real push.
git push --dry-run origin main

# Push only sanitized committed content.
git push origin main
```

```powershell
# For local-only secrets, keep the remote version sanitized and leave the real value local.
git show HEAD:AGENTS.md | Select-String -Pattern 'VM_PASSWORD'
Select-String -Path C:\Users\caofengrui\Desktop\linux\AGENTS.md -Pattern '虚拟机密码'
```

---

## Scenario: STM32MP157 Kernel And Rootfs Configuration

### 1. Scope / Trigger

- Trigger: Any task that enables Linux kernel Kconfig symbols, Buildroot packages, UVC USB camera support, USB Type-C/host support, GPU `galcore`, or deploys a rebuilt root filesystem.
- Board: 正点原子 STM32MP157 development board.
- VM user: `cfr`.
- VM host alias: `cfr-vm`.
- Kernel tree: `/home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31`.
- Buildroot tree: `/home/cfr/linux/buildroot/buildroot-2020.02.6`.
- NFS rootfs: `/home/cfr/linux/nfs/rootfs`.

### 2. Signatures

| Operation | Command Signature |
|---|---|
| Enter VM | `ssh cfr-vm` |
| Kernel menuconfig | `cd /home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31 && make ARCH=arm CROSS_COMPILE=arm-ostl-linux-gnueabi- menuconfig` |
| Kernel menuconfig save path | `./arch/arm/configs/stm32mp1_atk_defconfig` |
| Buildroot menuconfig | `cd /home/cfr/linux/buildroot/buildroot-2020.02.6 && make menuconfig` |
| Buildroot menuconfig save path | `./configs/stm32mp1_atk_defconfig` |
| Buildroot clean output build | `make O=/home/cfr/linux/buildroot/buildroot-2020.02.6/output-uvc -j8` |
| Rootfs deploy from standard Buildroot images | `cd output/images && sudo tar -axvf rootfs.tar -C /home/cfr/linux/nfs/rootfs` |
| Rootfs deploy from current clean output | `cd /home/cfr/linux/buildroot/buildroot-2020.02.6/output-uvc/images && sudo tar -axvf rootfs.tar -C /home/cfr/linux/nfs/rootfs` |
| Compatible current branch check | `git rev-parse --abbrev-ref HEAD` |

### 3. Contracts

| Area | Contract |
|---|---|
| Kernel Kconfig persistence | Every symbol enabled through kernel `make menuconfig` must be saved from the menuconfig UI to `./arch/arm/configs/stm32mp1_atk_defconfig`; `.config` alone is not the durable board config. |
| Buildroot Kconfig persistence | Every package enabled through Buildroot `make menuconfig` must be saved from the menuconfig UI to `./configs/stm32mp1_atk_defconfig`; `.config` alone is not the durable board rootfs config. |
| Rootfs deployment | The generated `rootfs.tar` must be extracted into `/home/cfr/linux/nfs/rootfs` with `sudo tar -axvf`; use `output/images` for the default Buildroot output and `output-uvc/images` for the current isolated UVC build. Extracting as an ordinary user can leave wrong ownership or fail on root-owned paths. |
| No-password sudo | If `sudo` asks for a password, stop and report the exact command for the human to run; do not claim deployment to root-owned rootfs paths succeeded. |
| Temporary runtime package | If full rootfs extraction is blocked by sudo, use `/home/cfr/linux/nfs/rootfs/tmp/<name>` only as an explicit temporary runtime package, not as the final rootfs deployment. |
| VM Git compatibility | The VM uses Git `2.17.1`; do not use commands introduced later, including `git switch`, `git restore`, and `git branch --show-current`. |
| Power-correlation triage | When independent peripherals fail together, such as Goodix I2C `-ENXIO` and Quectel USB disconnects in the same boot, follow `Scenario: Cross-Device Software/Hardware Fault Triage` before changing unrelated driver logic. |

### 4. Validation & Error Matrix

| Check | Good Result | Bad Result | Required Action |
|---|---|---|---|
| Kernel defconfig saved | `arch/arm/configs/stm32mp1_atk_defconfig` contains the non-default `CONFIG_*` symbols, and regenerated `.config` keeps the required final state | Only the build-local `.config` changed | Reopen/save menuconfig or update defconfig before claiming config is persistent |
| Buildroot defconfig saved | `configs/stm32mp1_atk_defconfig` contains the non-default `BR2_PACKAGE_*` symbols, and `make olddefconfig` keeps the required final package state in `.config` | Only the build-local `.config` changed | Save through menuconfig or `make savedefconfig` to the defconfig path |
| Rootfs deploy | `/home/cfr/linux/nfs/rootfs/usr/bin/...` and libraries match generated rootfs | `Permission denied`, sudo prompt, or files only under `/tmp` | Report blocked deployment or ask human to run sudo extraction |
| GStreamer UVC tools | `gst-launch-1.0`, `gst-inspect-1.0`, `v4l2-ctl`, `libgstvideo4linux2.so`, `libgstfbdevsink.so` exist | Missing command or plugin | Recheck Buildroot package symbols and rebuild |
| VM Git command | Command works under Git 2.17.1 | `git: 'switch' is not a git command` or unknown option | Replace with compatible command |
| Cross-device power retest | Touch probe succeeds at `0x5d`, touch IRQ count increases, `ppp0` stays online, and `dmesg` does not keep adding 4G USB disconnects after adequate power is supplied | Touch and 4G remain unstable even after power is corrected | Continue separate I2C and USB debugging only after power and load are ruled out |

### 5. Good/Base/Bad Cases

| Case | Example | Expected Result |
|---|---|---|
| Good kernel config | Enable UVC in `make menuconfig`, then save to `./arch/arm/configs/stm32mp1_atk_defconfig` | Future clean kernel config keeps UVC settings |
| Good Buildroot config | Enable `v4l2-ctl`, GStreamer tools, `v4l2src`, `fbdevsink`, then save to `./configs/stm32mp1_atk_defconfig` | Future rootfs rebuild keeps camera display tools |
| Base temporary deployment | Copy a temporary runtime package to `/home/cfr/linux/nfs/rootfs/tmp/uvc-rootfs` when sudo is blocked | Useful for board testing, but documented as temporary |
| Bad config persistence | Enable symbols in `.config` only and do not save defconfig | Next clean build can silently lose the setting |
| Bad Git compatibility | Use `git switch -c fix/x` on the VM | Fails on Git 2.17.1 |
| Bad power diagnosis | Treat simultaneous touch I2C failures and 4G USB re-enumeration as two unrelated software bugs | Misses the board-level power or USB-load root cause |

### 6. Tests Required

- Kernel:
  - Assert `make ARCH=arm CROSS_COMPILE=<prefix> olddefconfig` keeps required `CONFIG_*`.
  - Assert `make ARCH=arm CROSS_COMPILE=<prefix> dtbs` succeeds after device-tree edits.
  - Assert `make ARCH=arm CROSS_COMPILE=<prefix> uImage LOADADDR=0xC2000040 -j8` succeeds when kernel image changes.
- Buildroot:
  - Assert `make olddefconfig` keeps required `BR2_PACKAGE_*`; remember that `savedefconfig` omits symbols whose value already matches the Kconfig default.
  - Assert `make -j8` or `make O=<output-dir> -j8` exits `0`.
  - Assert generated target contains `usr/bin/gst-launch-1.0`, `usr/bin/v4l2-ctl`, and required plugins.
- Board:
  - Assert `lsusb` sees the UVC camera.
  - Assert `/dev/video*` exists.
  - Assert `v4l2-ctl -d /dev/video0 --list-formats-ext` prints supported formats.
  - Assert raw or MJPEG GStreamer pipeline displays on `/dev/fb0`.
  - If touch and 4G fail together, assert adequate board power before editing touch address logic, PPP routing, APN, or Qt health display.

### 7. Wrong vs Correct

#### Wrong

```bash
git switch -c fix/uvc
git branch --show-current
make menuconfig
# Save only .config, then claim the board defconfig is updated.
cd output/images
tar -axvf rootfs.tar -C /home/cfr/linux/nfs/rootfs
```

#### Correct

```bash
git checkout -b fix/uvc
git rev-parse --abbrev-ref HEAD

cd /home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31
make ARCH=arm CROSS_COMPILE=arm-ostl-linux-gnueabi- menuconfig
# In the menuconfig Save dialog, save to:
# ./arch/arm/configs/stm32mp1_atk_defconfig

cd /home/cfr/linux/buildroot/buildroot-2020.02.6
make menuconfig
# In the menuconfig Save dialog, save to:
# ./configs/stm32mp1_atk_defconfig

cd output/images
sudo tar -axvf rootfs.tar -C /home/cfr/linux/nfs/rootfs
```

---

## Scenario: Cross-Device Software/Hardware Fault Triage

### 1. Scope / Trigger

- Trigger: Two or more independent peripherals fail in the same boot or during the same user workflow.
- Trigger: A failure crosses different buses or software layers, such as Goodix I2C touch `-ENXIO` and Quectel USB 4G `usb disconnect`.
- Trigger: A fix appears after changing a physical condition, such as supplying enough power, reconnecting a cable, reducing USB load, or changing the hub.
- Goal: Do not classify the fault as software-only or hardware-only until both tracks have evidence.
- Confirmed lesson date: `2026-05-20`; after adequate power was supplied, touch input worked and the 4G module stopped intermittent disconnects.
- Board platform: 正点原子 STM32MP157, Linux `5.4.31`.

### 2. Signatures

| Track | Command / Evidence Signature |
|---|---|
| Kernel multi-device log | `dmesg | grep -Ei "Goodix|atk-gt9147|read touch data failed|usb .*disconnect|ttyUSB|option|ppp|i2c"` |
| BusyBox live log | `cat /dev/kmsg | grep -Ei "Goodix|read touch data failed|usb .*disconnect|ttyUSB|ppp"` |
| Touch input node | `cat /proc/bus/input/devices | grep -A6 -Ei "Goodix|gt9|touch"` |
| Touch interrupt proof | `cat /proc/interrupts | grep -Ei "goodix|gt9|touch"` before and after a human tap |
| Touch event proof | `hexdump -C /dev/input/eventX` or `evtest /dev/input/eventX` when available |
| 4G USB nodes | `ls /dev/ttyUSB* 2>/dev/null` and `dmesg | grep -Ei "ttyUSB[0-9].*(attached|disconnect)"` |
| 4G PPP state | `ifconfig ppp0 2>/dev/null; ip route show; cat /tmp/4g-ppp.log 2>/dev/null` |
| USB runtime power | `cat /sys/bus/usb/devices/2-1/power/control /sys/bus/usb/devices/2-1.7/power/control 2>/dev/null` |
| Board power evidence | External supply voltage/current rating, measured 5V/3.3V under load, USB hub supply mode, cable/contact condition, and active peripheral list |
| Recent software delta | `git log --oneline --decorate -n 10` and `git diff --stat` in the touched driver/script/UI repository |

### 3. Contracts

| Area | Contract |
|---|---|
| Evidence first | Collect software logs and hardware state before editing drivers, PPP scripts, Qt health display, APN, route rules, or device-tree nodes. |
| Common-dependency rule | If unrelated peripherals fail together, inspect common dependencies first: board input power, PMIC rails, USB hub power, cable voltage drop, connector contact, ground, reset lines, clocks, and shared rootfs/init timing. |
| Software track | The software track must prove whether the fault is in driver probe/runtime, kernel config, device nodes, init scripts, PPP route/DNS, Qt input discovery, or recent code changes. |
| Hardware track | The hardware track must prove whether the fault is in supply margin, peak current, USB hub/backfeed behavior, cable/contact quality, SIM/antenna, touch ribbon cable, I2C pull-ups, or peripheral load combination. |
| No single-track diagnosis | Do not keep changing one driver or one UI script when logs show another bus or another peripheral failing at the same time. |
| Power retest gate | When power is a plausible common dependency, rerun the same software checks after supplying adequate power and reducing optional load before making more software changes. |
| Result classification | If adequate power makes all affected peripherals stable with the same software image, record the root cause as board-level power/load, not as a software fix. |
| Residual-fault rule | If adequate power fixes one peripheral but not another, split the investigation after documenting which symptoms disappeared and which remain. |
| Documentation | Update the module README and this code-spec when a hardware condition explains a software-looking log signature. |

### 4. Validation & Error Matrix

| Check | Good Result | Bad Result | Required Action |
|---|---|---|---|
| Symptom grouping | One timeline shows which touch, 4G, camera, display, or storage symptoms occur together | Each symptom is debugged in isolation with no shared boot timeline | Build a single timeline from `dmesg`, `/dev/kmsg`, app logs, and human-observed events |
| Software evidence | Logs identify exact failing layer, such as Goodix `-ENXIO`, USB detach, missing `ppp0`, unchanged default route, or Qt input FD missing | Diagnosis says "stuck" or "broken" with no command output | Collect the signatures above before editing code |
| Hardware evidence | Power source rating, rail/load behavior, USB hub supply, cable/contact, and active peripherals are checked | Only scripts and drivers are changed while current-hungry peripherals are attached | Reduce load, improve supply, or use powered hub, then retest the same software image |
| Power retest | After adequate power, Goodix initializes at `0x5d`, touch events work, `ttyUSB*` stops re-enumerating, and `ppp0` remains online | Failures continue with stable power and known-good cables | Continue separate I2C, USB, PPP, or Qt debugging with power ruled out |
| 4G transport vs route | `ttyUSB*` stays present and `ppp0` has an IP before route/DNS diagnosis | USB disconnects continue, but APN/DNS/UI code is changed | Fix transport power/hub/cable/module contact before network-layer changes |
| Touch driver vs Qt input | Kernel input node and interrupts prove touch hardware state before Qt is blamed | Qt is changed while Goodix still has I2C `-ENXIO` or no event node | Fix/prove kernel touch path first, then inspect Qt input discovery |
| Recent code delta | Recent software changes are reviewed and either correlated with logs or ruled out | A power/root-cause retest is skipped because a recent commit is suspected | Treat recent code as one hypothesis, not the only hypothesis |
| Documentation | README/spec records whether the final cause was power/load, software, or mixed | The same failure repeats because the final lesson stays only in chat | Update module docs and code-spec with signatures and retest commands |

### 5. Good/Base/Bad Cases

| Case | Example | Expected Result |
|---|---|---|
| Good power-root-cause retest | Touch I2C `-6` and 4G USB disconnects occur together, then both disappear after adequate power is supplied | Classify as board power/load root cause and stop rewriting touch/PPP logic |
| Good software-root-cause retest | Power rails are stable, USB nodes do not detach, but `ppp0` is up with the old `eth0` default route first | Fix route priority, not power or touch code |
| Good split diagnosis | Power stabilizes 4G USB but touch still has `-ENXIO` | Continue Goodix address/reset/I2C/pull-up diagnosis and document the 4G symptom as resolved |
| Base intermittent symptom | One isolated I2C read failure appears while every other peripheral remains stable | Use the per-driver recovery contract first, but still note power/load if repeats correlate with USB load |
| Bad software tunnel vision | Keep changing Goodix fallback address or Qt input code while `usb 2-1.7: USB disconnect` repeats | Misses the shared supply/load problem |
| Bad hardware-only diagnosis | Blame power without checking `/tmp/4g-ppp.log`, route table, `dmesg`, or recent code changes | Can miss real script, route, kernel config, or Qt input bugs |
| Bad UI masking | Change Qt to hide "4G offline" or "touch unavailable" while the kernel transport is disappearing | The UI becomes less honest and the root cause remains |

### 6. Tests Required

- Software evidence:
  - Assert `dmesg | grep -Ei "Goodix|atk-gt9147|read touch data failed|usb .*disconnect|ttyUSB|ppp|i2c"` is captured before the first fix attempt.
  - Assert `/tmp/4g-ppp.log`, `ifconfig ppp0`, and `ip route show` are checked before changing APN, DNS, PPP options, or Qt cloud status.
  - Assert `/proc/bus/input/devices`, `/proc/interrupts`, and `/dev/input/eventX` are checked before changing Qt touch handling.
  - Assert recent code changes are reviewed with `git log --oneline -n 10` and `git diff --stat` in the affected repository.
- Hardware evidence:
  - Assert the board power source rating and measured under-load voltage are recorded when multiple peripherals fail together.
  - Assert USB hub supply mode, cable/contact state, 4G module peak-current demand, SIM/antenna, touch ribbon cable, and I2C pull-ups are checked when relevant.
  - Assert optional high-current peripherals are disconnected or moved to a powered hub for one controlled retest.
- Retest:
  - Assert the same software image is retested after adequate power is supplied.
  - Assert touch success includes Goodix probe at `0x5d`, visible input node, interrupt count change or event output, and a human tap affecting the UI.
  - Assert 4G success includes stable `ttyUSB*`, `ppp0` with IP, usable route, and no fast-growing `usb disconnect` logs during observation.
  - Assert the final diagnosis states `software`, `hardware`, or `mixed`, with the evidence that selected that label.

### 7. Wrong vs Correct

#### Wrong

```bash
# Wrong: debug only one software layer while another bus is also failing.
dmesg | grep -i "read touch data failed"
vim 15_iictouch/iictouch.c
make
```

```bash
# Wrong: hide the symptom in UI before proving the modem transport is stable.
grep -n "4G未连接" -r 20_uvc_camera/qt_camera_display
vim Main.qml
```

#### Correct

```bash
# Correct: collect one cross-device timeline before deciding the fault class.
dmesg | grep -Ei "Goodix|atk-gt9147|read touch data failed|usb .*disconnect|ttyUSB|ppp|i2c"
cat /tmp/4g-ppp.log 2>/dev/null
ifconfig ppp0 2>/dev/null
ip route show
cat /proc/bus/input/devices | grep -A6 -Ei "Goodix|gt9|touch"
cat /proc/interrupts | grep -Ei "goodix|gt9|touch"
cat /sys/bus/usb/devices/2-1/power/control /sys/bus/usb/devices/2-1.7/power/control 2>/dev/null
```

```bash
# Correct: retest the same software image after improving the common dependency.
# If touch and 4G both become stable only after adequate power, record power/load as the root cause.
dmesg | grep -Ei "Goodix|read touch data failed|usb .*disconnect|ttyUSB"
ifconfig ppp0
cat /proc/interrupts | grep -Ei "goodix|gt9|touch"
```

---

## Scenario: STM32MP157 Board SSH From VM

### 1. Scope / Trigger

- Trigger: Any task that says to enter the Linux virtual machine and verify on the STM32MP157 board.
- Trigger: Any board-side validation for Qt camera display, SD-card save/remove, UVC camera, 4G PPP, kernel logs, or NFS rootfs runtime.
- Board: 正点原子 STM32MP157 development board at `192.168.1.250`.
- VM user: `cfr`; VM host alias: `cfr-vm`.

### 2. Signatures

| Operation | Command Signature |
|---|---|
| Enter VM from Windows/local shell | `ssh cfr-vm` |
| SSH from VM to board | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250` |
| One-shot board command from Windows via VM | `ssh cfr-vm "ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 '<board-command>'"` |
| Board identity check | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 'echo BOARD_OK; uname -a'` |
| Qt/KMS board status check | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 '/root/qt_camera_display/run_qt_kms_overlay_display.sh status'` |
| SD-card mount check | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 "mount | grep ' /mnt/sdcard '; df -h /mnt/sdcard"` |

### 3. Contracts

| Area | Contract |
|---|---|
| Connection origin | Board SSH is normally run from inside `cfr-vm`, not directly from Windows. If using a one-shot Windows command, nest the board SSH inside `ssh cfr-vm "..."`. |
| Private key | Always pass `-i /home/cfr/.ssh/id_ed25519_github` for board SSH. Do not rely on SSH automatically picking the right key. |
| Key selection | Always pass `-o IdentitiesOnly=yes` so SSH does not try unrelated identities and fail before reaching the intended key. |
| Board user/IP | Use `root@192.168.1.250` for the board unless the human explicitly says the board IP changed. |
| Failure handling | If direct `ssh root@192.168.1.250` fails, do not treat the board as unreachable until the explicit key command above has been tried from the VM. |
| Quoting discipline | For multi-hop commands from Windows PowerShell, keep remote board pipelines inside the VM-side quoted string, use an outer single-quoted PowerShell string when the remote command contains `grep` alternation or Chinese patterns, or split complex checks into several simple SSH commands to avoid PowerShell interpreting board-side pipes. Never trust a deployment confirmation if PowerShell prints a local error for a board-side `grep`, `|`, `$()`, or alternation pattern. |

### 4. Validation & Error Matrix

| Check | Good Result | Bad Result | Required Action |
|---|---|---|---|
| `ssh cfr-vm` | VM shell accepts commands as user `cfr` | Host alias not found or password/auth failure | Fix local VM SSH first; do not debug board connection yet |
| Explicit board SSH | Prints `BOARD_OK` and board `uname -a` | Permission denied, too many authentication failures, or timeout | Confirm the command includes both `-i /home/cfr/.ssh/id_ed25519_github` and `-o IdentitiesOnly=yes`; then check board power/network |
| Board IP | `root@192.168.1.250` responds | Connection timeout | Check board Ethernet, VM network, and whether the board IP changed |
| Nested one-shot command | Board command output appears in local terminal | Local PowerShell errors such as treating `pidof`, `storage`, Chinese grep patterns, or pipe fragments as local commands | Simplify quoting, split the command into upload/install/check steps, or run `ssh cfr-vm` interactively first, then run board SSH inside the VM |
| Runtime status | Board status command reports expected PIDs/log paths | Command not found or old process state | Confirm NFS rootfs deployment path and restart the relevant board service/script |

### 5. Good/Base/Bad Cases

| Case | Example | Expected Result |
|---|---|---|
| Good explicit key login | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 'echo BOARD_OK'` | Logs into the board from VM and prints `BOARD_OK` |
| Good nested local check | `ssh cfr-vm "ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 'uname -a'"` | Lets the local Codex session verify the board without manually opening an interactive VM shell |
| Base direct login | `ssh root@192.168.1.250` | May fail because SSH tries the wrong identity first; this is not the supported project path |
| Bad missing identity constraint | `ssh -i /home/cfr/.ssh/id_ed25519_github root@192.168.1.250` | Can still try extra identities and fail unexpectedly on some SSH configurations |
| Bad local interpretation | A Windows command where `| grep ...` is outside the remote quotes | PowerShell may run part of the board command locally and produce misleading errors |
| Bad deployment confirmation | A nested one-shot command installs a file and then runs `grep -n "A\|B"`; PowerShell reports `B` is not recognized | The install/check command did not complete as written. Re-run as separate `scp`, `install`, `sha256sum`, and simple `grep` commands |

### 6. Tests Required

- Before claiming board access works, run:
  - `ssh cfr-vm "ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 'echo BOARD_OK; uname -a'"`
- Before board-side Qt/SD-card validation, run:
  - `ssh cfr-vm "ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 '/root/qt_camera_display/run_qt_kms_overlay_display.sh status'"`
  - `ssh cfr-vm "ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 \"mount | grep ' /mnt/sdcard '; df -h /mnt/sdcard\""`
- When a nested command contains multiple pipes, variables, or command substitutions, prefer entering `ssh cfr-vm` first and running the board command from the VM shell to avoid local quoting errors.
- For deployment verification, prefer a sequence of simple commands: `scp` to VM, `scp` VM-to-board, `install`, `sha256sum`, then one grep/check per command. Do not combine install and complex grep confirmation into one fragile PowerShell string.
- If a PowerShell one-shot check must include remote `grep`, use an outer single-quoted local command and avoid regex alternation; run separate literal checks such as `grep -n CLOUD_TIME_OFFSET /root/app` and `grep -n wall_clock /root/app`.

### 7. Wrong vs Correct

#### Wrong

```bash
# Wrong: this can fail by selecting the wrong SSH identity, even when the board is online.
ssh root@192.168.1.250
```

```powershell
# Wrong: the local shell can steal parts of the board command if quoting is broken.
ssh cfr-vm "ssh root@192.168.1.250 'pidof qt_camera_display' | grep 123"
```

```powershell
# Wrong: PowerShell can split grep alternation before the board command finishes.
ssh cfr-vm "ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 'install -m 0755 /tmp/new /root/app; grep -n \"CLOUD_TIME_OFFSET\|二次换算\|wall_clock\" /root/app'"
```

#### Correct

```bash
# Correct: run this inside cfr-vm for normal board access.
ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250
```

```powershell
# Correct: one-shot board check from the local Codex/Windows shell through the VM.
ssh cfr-vm "ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 'echo BOARD_OK; uname -a'"
```

```powershell
# Correct: run deployment and checks as separate, simple VM-side commands.
ssh cfr-vm 'ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 "install -m 0755 /tmp/new /root/app; sha256sum /root/app"'
ssh cfr-vm 'ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 "grep -n CLOUD_TIME_OFFSET /root/app"'
ssh cfr-vm 'ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 "grep -n wall_clock /root/app"'
```

---

## Scenario: Kernel Built-In Preference And UVC Autostart

### 1. Scope / Trigger

- Trigger: A driver or framework has been proven stable on the STM32MP157 board and is needed during every boot, especially USB host, UVC camera, V4L2/media, framebuffer display, or other board-essential hardware.
- Trigger: A runtime package or init script is used to start camera display automatically on the NFS rootfs.
- Board output: RGB LCD framebuffer at `/dev/fb0`.
- Camera input: UVC USB camera at `/dev/video0` or `/dev/video1`.

### 2. Signatures

| Operation | Command Signature |
|---|---|
| Ask before built-in conversion | Confirm with the user before changing a stable driver from `=m` to `=y` |
| Kernel built-in verification | `make ARCH=arm CROSS_COMPILE=arm-ostl-linux-gnueabi- stm32mp1_atk_defconfig && make ARCH=arm CROSS_COMPILE=arm-ostl-linux-gnueabi- olddefconfig` |
| Kernel image build | `make ARCH=arm CROSS_COMPILE=arm-ostl-linux-gnueabi- uImage LOADADDR=0xC2000040 -j8` |
| Deploy kernel image | `cp arch/arm/boot/uImage /home/cfr/linux/tftpboot/uImage` |
| Deploy device tree | `cp arch/arm/boot/dts/stm32mp157d-atk.dtb /home/cfr/linux/tftpboot/stm32mp157d-atk.dtb` |
| Persistent UVC runtime path | `/root/uvc-rootfs` |
| Persistent framebuffer preview binary | `/root/uvc_fb_preview` |
| UVC autostart script | `/etc/init.d/S90uvc-camera` |
| GPU module autoload | `modprobe galcore` from `S90uvc-camera` |
| Manual camera restart | `/etc/init.d/S90uvc-camera restart` |

### 3. Contracts

| Area | Contract |
|---|---|
| Built-in preference | If a kernel driver/framework is always needed and has been validated, prefer compiling it into the kernel (`=y`) instead of leaving it as a module (`=m`). |
| User confirmation gate | Never change a driver from `=m` to `=y` without explicit user confirmation, because built-in drivers change boot-time behavior and require a new `uImage`. |
| Dependency closure | When converting UVC to built-in, convert the dependency chain too: `CONFIG_MEDIA_SUPPORT=y`, `CONFIG_VIDEO_DEV=y`, `CONFIG_VIDEO_V4L2=y`, `CONFIG_USB_VIDEO_CLASS=y`, `CONFIG_VIDEOBUF2_CORE=y`, `CONFIG_VIDEOBUF2_V4L2=y`, `CONFIG_VIDEOBUF2_MEMOPS=y`, and `CONFIG_VIDEOBUF2_VMALLOC=y`. |
| GPU policy | Keep `CONFIG_GPU_GALCORE=m` unless the user explicitly asks to try `=y`; vendor GPU drivers are easier to recover and debug as modules. |
| Autostart policy | Let `/etc/init.d/S90uvc-camera` load `galcore` and start `/root/uvc_fb_preview`; camera display does not require GPU, but the script may load GPU for the rest of the graphics stack. |
| `/tmp` policy | Do not pre-deploy required boot files under `/home/cfr/linux/nfs/rootfs/tmp`, because the board mounts `/tmp` as `tmpfs` and hides those files after boot. Use `/root`, `/opt`, `/usr/bin`, or `/lib/modules/$(uname -r)` instead. |
| Init script behavior | Init scripts must wait for `/dev/fb0` and `/dev/video*`, avoid blocking boot forever, log to `/var/log/uvc-camera.log`, and support `start`, `stop`, `restart`, and `status`. |

### 4. Validation & Error Matrix

| Check | Good Result | Bad Result | Required Action |
|---|---|---|---|
| User confirmation | The user explicitly approved built-in conversion | AI silently changes `=m` to `=y` | Stop and ask before changing Kconfig persistence |
| Built-in UVC final `.config` | Required UVC/media/videobuf2 symbols are `=y` after `olddefconfig` | Requested symbols fall back to `=m` | Check parent dependencies such as `CONFIG_MEDIA_SUPPORT` |
| Kernel deployment | `/home/cfr/linux/tftpboot/uImage` timestamp matches the new build | Defconfig changed but old `uImage` still boots | Rebuild `uImage` and copy it to `tftpboot` |
| Runtime path | Board sees `/root/uvc_fb_preview` and `/root/uvc-rootfs` after boot | Files exist only in NFS rootfs `/tmp` before boot | Move runtime files to persistent paths |
| GPU autoload | `lsmod | grep galcore` and `/dev/galcore` succeed | `modprobe galcore` fails | Check `modules.dep`, module path, and `dmesg` |
| Camera autostart | `ps | grep uvc_fb_preview` and RGB screen show camera frame | No process or no frame | Check `/var/log/uvc-camera.log`, `/dev/video*`, and `/dev/fb0` |

### 5. Good/Base/Bad Cases

| Case | Example | Expected Result |
|---|---|---|
| Good built-in conversion | User confirms UVC should be built in; `MEDIA_SUPPORT`, V4L2, UVC, and videobuf2 are all saved as `=y` | UVC camera enumerates without loading `uvcvideo.ko` |
| Good GPU policy | `CONFIG_GPU_GALCORE=m`, module installed under `/lib/modules/5.4.31/...`, `S90uvc-camera` runs `modprobe galcore` | `/dev/galcore` appears while keeping GPU recoverable as a module |
| Base autostart | `/etc/init.d/S90uvc-camera restart` starts `/root/uvc_fb_preview -d /dev/video0 -f /dev/fb0 -w 640 -h 480` | Camera frame appears on RGB screen |
| Bad `/tmp` deployment | Put `uvc-rootfs` or `galcore.ko` only in `/home/cfr/linux/nfs/rootfs/tmp` | Board boot hides it behind tmpfs and commands are missing |
| Bad dependency conversion | Change `CONFIG_USB_VIDEO_CLASS=y` but leave `CONFIG_MEDIA_SUPPORT=m` | `olddefconfig` downgrades UVC back to module |

### 6. Tests Required

- Kernel:
  - Assert `arch/arm/configs/stm32mp1_atk_defconfig` and regenerated `.config` both keep required built-in UVC symbols as `=y`.
  - Assert `make ARCH=arm CROSS_COMPILE=arm-ostl-linux-gnueabi- uImage LOADADDR=0xC2000040 -j8` exits `0`.
  - Assert `/home/cfr/linux/tftpboot/uImage` is updated after the build.
- Rootfs:
  - Assert `/home/cfr/linux/nfs/rootfs/etc/init.d/S90uvc-camera` is executable.
  - Assert `/home/cfr/linux/nfs/rootfs/root/uvc_fb_preview` is executable.
  - Assert `/home/cfr/linux/nfs/rootfs/root/uvc-rootfs/usr/bin/v4l2-ctl` exists.
  - Assert `modules.dep` contains `kernel/drivers/gpu/drm/gcnano-driver-6.4.3/galcore.ko:`.
- Board:
  - Assert `dmesg | grep -Ei 'uvc|video|galcore|gcnano|gpu'` shows UVC and Galcore initialization.
  - Assert `ls /dev/video*`, `ls -l /dev/fb0`, and `ls -l /dev/galcore` succeed.
  - Assert `/etc/init.d/S90uvc-camera status` reports the preview process running.
  - Assert the RGB screen shows the camera frame.

### 7. Wrong vs Correct

#### Wrong

```bash
# No user confirmation, and only the leaf symbol is changed.
CONFIG_USB_VIDEO_CLASS=y
CONFIG_MEDIA_SUPPORT=m

# Runtime is placed in /tmp, which is hidden by tmpfs after boot.
cp uvc_fb_preview /home/cfr/linux/nfs/rootfs/tmp/uvc_fb_preview
cp -a uvc-rootfs /home/cfr/linux/nfs/rootfs/tmp/uvc-rootfs
```

#### Correct

```bash
# After user confirmation, convert the full dependency chain and save defconfig.
CONFIG_MEDIA_SUPPORT=y
CONFIG_VIDEO_DEV=y
CONFIG_VIDEO_V4L2=y
CONFIG_USB_VIDEO_CLASS=y
CONFIG_VIDEOBUF2_CORE=y
CONFIG_VIDEOBUF2_V4L2=y
CONFIG_VIDEOBUF2_MEMOPS=y
CONFIG_VIDEOBUF2_VMALLOC=y
CONFIG_GPU_GALCORE=m

make ARCH=arm CROSS_COMPILE=arm-ostl-linux-gnueabi- olddefconfig
make ARCH=arm CROSS_COMPILE=arm-ostl-linux-gnueabi- uImage LOADADDR=0xC2000040 -j8
cp arch/arm/boot/uImage /home/cfr/linux/tftpboot/uImage

# Deploy boot-required runtime to persistent paths.
cp uvc_fb_preview /home/cfr/linux/nfs/rootfs/root/uvc_fb_preview
cp -a uvc-rootfs /home/cfr/linux/nfs/rootfs/root/uvc-rootfs
cp S90uvc-camera /home/cfr/linux/nfs/rootfs/etc/init.d/S90uvc-camera
```

---

## Current UVC / USB / GPU Configuration Record

| Layer | Configured Items |
|---|---|
| Kernel device tree | USB PHY tuning, USB regulators, Type-C controller nodes, `usbh_ehci`, `usbotg_hs`, `usbphyc`, `usbphyc_port0`, and `usbphyc_port1` enabled for the ATK board. |
| Kernel media/UVC | `CONFIG_MEDIA_SUPPORT=y`, `CONFIG_VIDEO_DEV=y`, `CONFIG_VIDEO_V4L2=y`, `CONFIG_MEDIA_USB_SUPPORT=y`, `CONFIG_USB_VIDEO_CLASS=y`, and required videobuf2 symbols are built into the kernel. |
| Kernel Type-C | `CONFIG_TYPEC`, `CONFIG_TYPEC_TCPM`, `CONFIG_TYPEC_FUSB302`, `CONFIG_TYPEC_STUSB`, and `CONFIG_USB_ROLE_SWITCH` enabled. |
| Kernel GPU | Official `gcnano-driver-6.4.3` added under DRM, with `CONFIG_GPU_GALCORE=m`; `S90uvc-camera` autoloads it with `modprobe galcore`. |
| Buildroot tools | `BR2_PACKAGE_LIBV4L`, `BR2_PACKAGE_LIBV4L_UTILS`, `BR2_PACKAGE_GSTREAMER1`, GStreamer tools, `v4l2src`, `videoconvert`, `videoscale`, `fbdevsink`, `kmssink`, and JPEG support enabled. |
| Persistent runtime | `/home/cfr/linux/nfs/rootfs/root/uvc-rootfs`, `/home/cfr/linux/nfs/rootfs/root/uvc_fb_preview`, and `/home/cfr/linux/nfs/rootfs/etc/init.d/S90uvc-camera` are installed for boot-time camera preview. |
| Gotcha | `/home/cfr/linux/nfs/rootfs/tmp` is not a safe persistent boot path because the board mounts `/tmp` as `tmpfs`. |

## Scenario: Qt UVC Preview Safe Fallback And Zero-Copy Target

### 1. Scope / Trigger

- Trigger: Implementing or debugging the STM32MP157 Qt industrial inspection UI with UVC camera preview.
- Trigger: Comparing CPU framebuffer preview, Qt Quick preview, QtMultimedia preview, or any hardware video display path.
- Board: 正点原子 STM32MP157 with RGB LCD, UVC camera at `/dev/video0`, GPU via `galcore`.

### 2. Signatures

| Operation | Command Signature |
|---|---|
| Build Qt camera UI | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ./build_qt_camera_display.sh` |
| Deploy Qt camera UI | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && sudo ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` |
| Run safe preview fallback | `/root/qt_camera_display/run_qt_camera_display.sh` |
| Run higher-quality fallback test | `CAMERA_WIDTH=640 CAMERA_HEIGHT=480 CAMERA_FPS=15 /root/qt_camera_display/run_qt_camera_display.sh` |
| Run validated GStreamer GL video route | `VIDEO_BACKEND=gst-gl /root/qt_camera_display/run_qt_camera_display.sh` |
| CPU sample | `pid=$(pidof qt_camera_display | awk '{print $1}')` followed by `/proc/stat` and `/proc/$pid/stat` 5-second delta sampling |
| Zero-copy route notes | `20_uvc_camera/qt_camera_display/zero_copy_hardware_video_plan.md` |

### 3. Contracts

| Area | Contract |
|---|---|
| Safe fallback implementation | The current stable Qt preview uses custom `V4L2VideoItem`, not QtMultimedia `Camera + VideoOutput`. |
| Safe fallback default | Default capture is `320x240@10fps` to keep CPU low while preserving a working UI/camera bring-up path. |
| Measurement record | `320x240@10fps` measured about `5.9% CPU`; `640x480@15fps` measured about `42.0% CPU`; old `/root/uvc_fb_preview` measured about `44%~49% CPU`. |
| Do not overclaim | Low CPU in the safe fallback is caused by lower resolution and frame rate, not by a completed zero-copy video path. |
| Forbidden stable route | Do not treat QtMultimedia `Camera + VideoOutput` as stable on this board until the `galcore` crash is fixed. It triggered `QSGRenderThread -> galcore _UserMemoryAttach -> dma_map_sg` kernel Oops. |
| Validated standalone GL route | `v4l2src io-mode=dmabuf ! glupload ! glimagesink` is the current low-CPU route for observing camera quality; it does not show the Qt industrial UI. `qmlglsink` is already integrated only as an mmap bridge, not as a stable UVC DMABUF zero-copy path. |
| KMS current status | `v4l2src io-mode=dmabuf ! kmssink driver-name=stm` opens KMS but fails negotiation because current STM32MP157 planes expose RGB formats, not YUYV/NV12. |
| qmlglsink integration status | `qmlglsink` can embed ordinary GL textures and `VIDEO_BACKEND=qt-gst` works with the mmap bridge, but UVC DMABUF textures crash in Vivante userspace when drawn by Qt scene graph. |
| Final target | Keep the mmap `qmlglsink` bridge only as a visible UI integration fallback. For the low-CPU production route, prioritize Wayland surface composition, KMS plane composition, or a custom Qt GL item that owns DMABUF import/draw behavior inside the Qt GL context. |

### 4. Validation & Error Matrix

| Check | Good Result | Bad Result | Required Action |
|---|---|---|---|
| Safe fallback display | Qt UI and UVC video show without kernel Oops | Black video, no `/dev/video0`, or V4L2 format error | Check `run_qt_camera_display.sh`, `v4l2-ctl --list-formats-ext`, and V4L2 fallback status text |
| CPU claim | Report CPU with capture size and frame rate | Report a single CPU number without resolution/fps | Re-run 5-second average and record `WIDTHxHEIGHT@FPS` |
| QtMultimedia route | Only used for controlled debugging | `Camera + VideoOutput` used as production path and crashes `galcore` | Switch back to `V4L2VideoItem` fallback or validate a new zero-copy route |
| Standalone GL route | `VIDEO_BACKEND=gst-gl` shows `640x480@15fps` with low CPU and no Oops | Process exits, black screen, CPU remains near `40%`, or Oops appears | Check `glupload`, `glimagesink`, `/dev/galcore`, and `/tmp/qt-camera-gst-gl.log` |
| Qt integration route | `gst-inspect-1.0 qmlglsink` succeeds and QML embeds `GstGLVideoItem` | `qmlglsink` missing from rootfs | Enable `BR2_PACKAGE_GST1_PLUGINS_GOOD_PLUGIN_QMLGL`, rebuild, and deploy the plugin before Qt integration |
| qmlglsink DMABUF route | Not used as a production route; if tested, it is a controlled crash reproduction | `GST_IO_MODE=dmabuf VIDEO_BACKEND=qt-gst` exits `139`, with core in `libGAL.so:gcoTEXTURE_GetMipMap()` | Stop treating qmlglsink+DMABUF as the mainline; pivot to Wayland, KMS plane, or custom Qt GL item |
| qmlglsink mmap bridge | `VIDEO_BACKEND=qt-gst` shows Qt UI and camera at `640x480@15fps` without Oops | Black video, `not-negotiated`, or CPU claimed as zero-copy | Check `videoconvert`, `glupload`, `qmlglsink`, and report CPU as mmap bridge, not zero-copy |

### 5. Good/Base/Bad Cases

| Case | Example | Expected Result |
|---|---|---|
| Good final path | Wayland/KMS/custom Qt GL route displays `640x480@15fps` or higher with CPU below `15%` | High-quality preview, Qt industrial UI retained, and no `galcore` Oops or userspace SIGSEGV |
| Good standalone check | `VIDEO_BACKEND=gst-gl /root/qt_camera_display/run_qt_camera_display.sh` | Camera video appears at `640x480@15fps` with about `2%` CPU and no Oops in a 10-minute run |
| Base qmlglsink bridge | `VIDEO_BACKEND=qt-gst /root/qt_camera_display/run_qt_camera_display.sh` | Qt UI and video appear together, but CPU is about `36.2%` at `640x480@15fps` because it uses mmap and `videoconvert` |
| Base fallback | `V4L2VideoItem` at `320x240@10fps` | Stable UI/camera demo, low CPU, low image quality |
| Base quality test | `V4L2VideoItem` at `640x480@15fps` | Better image quality, CPU around old framebuffer preview level |
| Bad claim | Calling `320x240@10fps` fallback a GPU zero-copy solution | Misleading performance conclusion; document as fallback only |
| Bad qmlglsink claim | Calling `GST_IO_MODE=dmabuf VIDEO_BACKEND=qt-gst` the final route after seeing qmlglsink load | It crashes with SIGSEGV in Vivante `libGAL`; plugin availability does not mean DMABUF scene-graph rendering is stable |
| Bad route | Re-enabling QML `Camera + VideoOutput` without fixing `galcore` | Kernel Oops in `dma_cache_maint_page` through `galcore` |
| Bad KMS claim | Calling `kmssink` done after `videoconvert ! BGRA ! kmssink` works | This is a CPU color-conversion baseline, not the final zero-copy route |

### 6. Tests Required

- Safe fallback:
  - Assert `/root/qt_camera_display/run_qt_camera_display.sh` logs the capture size and frame rate.
  - Assert the screen shows UVC preview and Qt UI.
  - Assert 5-second CPU sample includes the process name and capture configuration.
- Zero-copy candidate:
  - Assert `v4l2-ctl -d /dev/video0 --list-formats-ext` records supported formats.
  - Assert `gst-inspect-1.0 waylandsink`, `gst-inspect-1.0 kmssink`, or `gst-inspect-1.0 qmlglsink` depending on the chosen route.
  - Assert standalone GStreamer GL video display works with `VIDEO_BACKEND=gst-gl` before integrating with Qt.
  - Assert Buildroot and rootfs expose `qmlglsink` before changing QML to use `GstGLVideoItem`.
  - Assert `VIDEO_BACKEND=qt-gst` is reported as an mmap bridge unless `GST_IO_MODE=dmabuf` is explicitly passed.
  - Assert `GST_IO_MODE=dmabuf VIDEO_BACKEND=qt-gst` is not used as a success criterion; if reproduced, collect core/backtrace and stop.
  - Assert Wayland/KMS/custom-GL candidates record the exact composition mechanism, resolution, fps, CPU, and 10-minute Oops/SIGSEGV status.
  - Assert no `Internal error: Oops` appears in kernel logs during at least 10 minutes of preview.

### 7. Wrong vs Correct

#### Wrong

```bash
# This is stable fallback, not final zero-copy.
/root/qt_camera_display/run_qt_camera_display.sh
# Then claim the GPU video optimization is complete because CPU is about 5.9%.
```

#### Correct

```bash
# Record fallback result with resolution and fps.
/root/qt_camera_display/run_qt_camera_display.sh

# Continue zero-copy validation separately.
v4l2-ctl -d /dev/video0 --list-formats-ext
VIDEO_BACKEND=gst-gl /root/qt_camera_display/run_qt_camera_display.sh
gst-inspect-1.0 qmlglsink
```

#### Wrong

```bash
# qmlglsink exists, but this route is known to crash in userspace on this board.
GST_IO_MODE=dmabuf VIDEO_BACKEND=qt-gst /root/qt_camera_display/run_qt_camera_display.sh
# Do not claim this is the integrated zero-copy route after only checking gst-inspect.
```

#### Correct

```bash
# Use the visible bridge only for UI integration and demos; record it as mmap/high-CPU.
VIDEO_BACKEND=qt-gst /root/qt_camera_display/run_qt_camera_display.sh

# Keep the low-CPU baseline separate.
VIDEO_BACKEND=gst-gl /root/qt_camera_display/run_qt_camera_display.sh

# Move production investigation to a route that does not make Qt scene graph
# draw UVC DMABUF-imported textures through qmlglsink.
gst-inspect-1.0 waylandsink
gst-inspect-1.0 kmssink
```

---

## Scenario: Qt Quick GStreamer GL Integration Limits And Next Mainlines

### 1. Scope / Trigger

- Trigger: Integrating UVC video into the STM32MP157 Qt Quick industrial inspection UI through GStreamer GL.
- Trigger: Debugging `qmlglsink`, `glimagesink`, Wayland/KMS video composition, or a custom Qt GL item that imports camera buffers.
- Trigger: A route uses UVC DMABUF, Vivante/Galcore, Qt scene graph, or GStreamer GL memory in the same display pipeline.

### 2. Signatures

| Operation | Command Signature |
|---|---|
| Visible Qt bridge | `VIDEO_BACKEND=qt-gst /root/qt_camera_display/run_qt_camera_display.sh` |
| qmlglsink DMABUF reproduction only | `GST_IO_MODE=dmabuf VIDEO_BACKEND=qt-gst /root/qt_camera_display/run_qt_camera_display.sh` |
| Standalone low-CPU GL baseline | `VIDEO_BACKEND=gst-gl /root/qt_camera_display/run_qt_camera_display.sh` |
| Inspect qmlglsink | `LD_LIBRARY_PATH=/usr/lib:/usr/lib/pulseaudio:/vendor/lib:/lib GST_PLUGIN_PATH=/usr/lib/gstreamer-1.0 gst-inspect-1.0 qmlglsink` |
| Inspect Wayland sink | `gst-inspect-1.0 waylandsink` |
| Inspect KMS sink | `gst-inspect-1.0 kmssink` |
| KMS RGB baseline | `gst-launch-1.0 -v v4l2src device=/dev/video0 io-mode=dmabuf ! video/x-raw,format=YUY2,width=640,height=480,framerate=15/1 ! videoconvert ! video/x-raw,format=BGRA ! kmssink driver-name=stm sync=false` |
| Core/backtrace for userspace SIGSEGV | `ulimit -c unlimited` before running the route, then inspect core with `arm-ostl-linux-gnueabi-gdb` from the ST SDK |

### 3. Contracts

| Area | Contract |
|---|---|
| qmlglsink availability | `gst-inspect-1.0 qmlglsink` only proves the plugin and QML type exist; it does not prove UVC DMABUF textures are safe in Qt scene graph. |
| qmlglsink mmap bridge | The default `qt-gst` bridge uses `v4l2src io-mode=mmap ! videoconvert ! glupload ! glcolorconvert ! gleffects_identity ! qmlglsink`; it is valid for visible UI integration but must be reported as a high-CPU bridge. |
| qmlglsink DMABUF limit | `qmlglsink + UVC DMABUF` is a controlled reproduction path only. On the current STM32MP157/Vivante stack it exits `139`, with core/backtrace pointing at `libGAL.so:gcoTEXTURE_GetMipMap()`. |
| Standalone GL baseline | `v4l2src io-mode=dmabuf ! glupload ! glimagesink` remains the low-CPU visual quality baseline, measured around `2%` CPU for `640x480@15fps` in a 10-minute run. |
| Wayland mainline | Prefer Wayland when a compositor can own surface composition, because video and Qt UI do not need to share the same Qt scene graph texture path. |
| KMS mainline | Prefer KMS plane only after confirming plane formats and ownership; current STM32MP157 planes expose RGB formats and reject direct YUYV/NV12 camera scanout. |
| Custom Qt GL mainline | Only attempt a custom Qt GL item when it can import and draw DMABUF/EGLImage inside the Qt render context with explicit lifetime and synchronization control. |
| Reporting | Every result must name route, io-mode, memory type, resolution, fps, CPU sample duration, and kernel/userspace crash status. |

### 4. Validation & Error Matrix

| Check | Good Result | Bad Result | Required Action |
|---|---|---|---|
| qmlglsink mmap bridge | UI and camera appear together; CPU is recorded around `36.2%` at `640x480@15fps` | Claimed as zero-copy or low-CPU final path | Relabel as bridge/fallback and continue mainline investigation |
| qmlglsink DMABUF route | Used only to reproduce and document SIGSEGV | Repeatedly retried as the production path | Stop, collect core/backtrace once, and pivot routes |
| Standalone GL baseline | `gst-gl` runs 10 minutes, low CPU, no Oops/SIGSEGV | Low CPU not reproduced or screen black | Recheck `glupload`, `glimagesink`, `galcore`, camera caps, and display ownership |
| Wayland candidate | Weston/compositor runs; Qt and video surfaces compose; CPU below `15%`; no Oops/SIGSEGV | Missing compositor, missing `waylandsink`, or video surface not shown | Add rootfs packages/config first; do not edit Qt app until standalone surface test works |
| KMS candidate | Plane format and rectangle selection are explicit; video and UI can coexist | Only `videoconvert ! BGRA ! kmssink` works | Treat as CPU RGB baseline, not zero-copy |
| Custom Qt GL item | Buffer import, sync, draw, and cleanup are explicit in code and verified with long-run tests | It reuses QtMultimedia/Vivante video nodes or qmlglsink DMABUF path | Reject the implementation and return to route planning |

### 5. Good/Base/Bad Cases

| Case | Example | Expected Result |
|---|---|---|
| Good Wayland target | Qt uses `QT_QPA_PLATFORM=wayland-egl`; video uses `waylandsink` with DMABUF/EGL path | Qt UI and video are separate surfaces composed without Qt scene graph drawing the camera DMABUF texture |
| Good KMS target | A supported RGB/DRM buffer or plane path displays video in a chosen rectangle while UI remains visible | CPU below `15%`, explicit plane/format proof, no display ownership conflict |
| Good custom Qt GL target | A Qt Quick item imports DMABUF/EGLImage inside the Qt GL context, uses explicit fences/sync, and releases buffers deterministically | Single-process UI, low CPU, no `libGAL` SIGSEGV in a 10-minute run |
| Base bridge | `VIDEO_BACKEND=qt-gst` | Good for demos and UI layout verification, but CPU remains around `36.2%` |
| Base baseline | `VIDEO_BACKEND=gst-gl` | Low CPU camera view without Qt UI; keep as performance benchmark |
| Bad route | `GST_IO_MODE=dmabuf VIDEO_BACKEND=qt-gst` as final route | Known userspace crash in Vivante `libGAL` |
| Bad claim | External `glimagesink render-rectangle` process merely stays alive beside Qt EGLFS | Process liveness is not composition proof; framebuffer or screen evidence is required |

### 6. Tests Required

- Assert `gst-inspect-1.0 qmlglsink`, `gst-inspect-1.0 glupload`, and `gst-inspect-1.0 glimagesink` before any qmlglsink integration test.
- Assert `VIDEO_BACKEND=qt-gst` shows video in the QML video panel and record CPU as the mmap bridge.
- Assert `GST_IO_MODE=dmabuf VIDEO_BACKEND=qt-gst` is not accepted as passing unless it survives at least 10 minutes with no SIGSEGV; current expected result is fail/reproduce.
- Assert Wayland candidates start compositor first, then test a standalone video surface, then run Qt as a Wayland client.
- Assert KMS candidates record `modetest -M stm` plane formats and prove the exact plane supports the intended buffer format.
- Assert custom Qt GL candidates include cleanup tests: stop/start preview repeatedly, release `/dev/video0`, and verify no stale GL/GStreamer objects remain.

### 7. Wrong vs Correct

#### Wrong

```bash
# Wrong: qmlglsink exists, so assume DMABUF zero-copy is integrated.
gst-inspect-1.0 qmlglsink
GST_IO_MODE=dmabuf VIDEO_BACKEND=qt-gst /root/qt_camera_display/run_qt_camera_display.sh
```

#### Correct

```bash
# Correct: keep the visible bridge and the low-CPU baseline separate.
VIDEO_BACKEND=qt-gst /root/qt_camera_display/run_qt_camera_display.sh
VIDEO_BACKEND=gst-gl /root/qt_camera_display/run_qt_camera_display.sh

# Correct: move the next production investigation away from qmlglsink+DMABUF.
gst-inspect-1.0 waylandsink
gst-inspect-1.0 kmssink
modetest -M stm
```

---

## Scenario: Visible KMS Fallback And Low-CPU Display Probing

### 1. Scope / Trigger

- Trigger: Debugging STM32MP157 UVC camera preview when `glimagesink` or a GL/GStreamer route has low CPU but the LCD is black.
- Trigger: Comparing visible KMS fallback, optimized `videoconvert`, MJPEG decode, `glimagesink`, `gldownload`, and Wayland/Weston candidates.
- Board: 正点原子 STM32MP157 RGB LCD at `1024x600`, UVC camera at `/dev/video0`, DRM driver `stm`, GPU via `galcore`.

### 2. Signatures

| Operation | Command Signature |
|---|---|
| SSH from VM to board | `ssh -i ~/.ssh/id_ed25519_github root@192.168.1.250` |
| Disable framebuffer console overlay | `echo 0 > /sys/class/vtconsole/vtcon1/bind` |
| Clear RGB LCD framebuffer | `dd if=/dev/zero of=/dev/fb0 bs=1228800 count=1` |
| Visible KMS BGRA baseline | `nohup gst-launch-1.0 -v v4l2src device=/dev/video0 io-mode=dmabuf ! video/x-raw,format=YUY2,width=640,height=480,framerate=15/1 ! videoconvert ! video/x-raw,format=BGRA,width=640,height=480,framerate=15/1 ! kmssink driver-name=stm sync=false >/tmp/gst-kms-visible.log 2>&1 < /dev/null &` |
| Visible fast KMS 15fps candidate | `nohup gst-launch-1.0 -v v4l2src device=/dev/video0 io-mode=dmabuf ! video/x-raw,format=YUY2,width=640,height=480,framerate=15/1 ! videoconvert dither=none chroma-mode=none matrix-mode=input-only chroma-resampler=nearest n-threads=2 ! video/x-raw,format=BGRA,width=640,height=480,framerate=15/1 ! kmssink driver-name=stm sync=false >/tmp/gst-vc-fast3.log 2>&1 < /dev/null &` |
| Current visible low-CPU fallback | `nohup gst-launch-1.0 -v v4l2src device=/dev/video0 io-mode=dmabuf ! video/x-raw,format=YUY2,width=640,height=480,framerate=10/1 ! videoconvert dither=none chroma-mode=none matrix-mode=input-only chroma-resampler=nearest n-threads=2 ! video/x-raw,format=BGRA,width=640,height=480,framerate=10/1 ! kmssink driver-name=stm sync=false >/tmp/gst-fast-kms-10fps.log 2>&1 < /dev/null &` |
| Lower visible KMS fallback | `nohup gst-launch-1.0 -v v4l2src device=/dev/video0 io-mode=mmap ! video/x-raw,format=YUY2,width=640,height=480,framerate=10/1 ! videoconvert dither=none chroma-mode=none matrix-mode=input-only chroma-resampler=nearest n-threads=1 ! video/x-raw,format=BGRA,width=640,height=480,framerate=10/1 ! kmssink driver-name=stm sync=false >/tmp/gst-mmap-kms-10fps-n1.log 2>&1 < /dev/null &` |
| Current recommended 640 fallback | `nohup gst-launch-1.0 -q v4l2src device=/dev/video0 io-mode=mmap ! 'video/x-raw,format=YUY2,width=640,height=480,framerate=10/1' ! videorate drop-only=true max-rate=6 skip-to-first=true silent=true ! 'video/x-raw,format=YUY2,width=640,height=480,framerate=6/1' ! videoconvert dither=none chroma-mode=none matrix-mode=input-only chroma-resampler=nearest alpha-mode=set alpha-value=1 n-threads=1 qos=false ! 'video/x-raw,format=BGRA,width=640,height=480,framerate=6/1' ! kmssink driver-name=stm sync=false async=false enable-last-sample=false qos=false show-preroll-frame=false processing-deadline=0 max-lateness=-1 'render-rectangle=<0,0,1024,600>' >/tmp/gst640-src10-drop6-bgra-fullrect.log 2>&1 < /dev/null &` |
| Low-resolution fullscreen KMS fallback | `nohup gst-launch-1.0 -q v4l2src device=/dev/video0 io-mode=mmap ! video/x-raw,format=YUY2,width=320,height=240,framerate=10/1 ! videoconvert dither=none chroma-mode=none matrix-mode=input-only chroma-resampler=nearest n-threads=1 ! video/x-raw,format=BGRA,width=320,height=240,framerate=10/1 ! kmssink driver-name=stm sync=false render-rectangle=<0,0,1024,600> >/tmp/gst-kms-320x240-10fps-n1-full.log 2>&1 < /dev/null &` |
| MJPEG KMS test | `nohup gst-launch-1.0 -v v4l2src device=/dev/video0 io-mode=mmap ! image/jpeg,width=640,height=480,framerate=15/1 ! jpegparse ! jpegdec idct-method=ifast ! videoconvert ! video/x-raw,format=BGRA,width=640,height=480,framerate=15/1 ! kmssink driver-name=stm sync=false >/tmp/gst-mjpeg-kms.log 2>&1 < /dev/null &` |
| Temporary direct KMS NEON probe | `nohup /tmp/uvc_kms_probe -w 640 -h 480 -r 10 -m neon >/tmp/uvc-kms-probe-neon-640x480-10.log 2>&1 < /dev/null &` |
| CPU sample for active GStreamer route | Read `/proc/stat` and every `/proc/$pid/task/*/stat` before/after a 5-30 second interval, using the non-zombie `gst-launch-1.0` PID. Sum all task `utime+stime` values when comparing multi-threaded GStreamer routes. |
| CPU sample for temporary direct probe | Use the same `/proc/stat` and `/proc/$pid/task/*/stat` all-thread method with `pidof uvc_kms_probe`; record the probe mode, requested fps, actual fps from the log, sample duration, and whether the current GStreamer fallback was restored afterward. |
| Crash check | `dmesg | grep -Ei 'Oops|galcore|dma_map_sg|segfault|gcoTEXTURE|Internal error' | tail -n 30` |

### 3. Contracts

| Area | Contract |
|---|---|
| Background process policy | Long-running camera/display pipelines must run in the background with `nohup ... >/tmp/<route>.log 2>&1 < /dev/null &`; do not run noisy GStreamer pipelines in the LCD foreground console. |
| Visible proof | A route is not accepted as visible just because `gst-launch-1.0` stays alive. Human LCD confirmation or a known visible KMS path is required. |
| Framebuffer console overlay | Disable `vtcon1` and clear `/dev/fb0` before display experiments, otherwise old console text can remain over the image or confuse visual checks. |
| Current visible fallback | The lower stable visible fallback is `640x480@10fps` with `v4l2src io-mode=mmap`, fast `videoconvert n-threads=1`, and `kmssink`, measured around `11.3% CPU` in a 30-second sample and no new `galcore`/Oops logs. |
| Low-resolution fallback | `320x240` YUYV capture with fast `videoconvert` to `BGRA` and `kmssink render-rectangle=<0,0,1024,600>` requests a full-display rectangle, but LTDC scaler behavior must be visually verified. All-thread CPU samples measured about `3.3%` at `5fps`, `6.5%` at `10fps` over 30 seconds, and `9.9%` at `15fps`. Treat it as a low-CPU preview fallback; image detail is below `640x480` and still needs human acceptance for the target inspection task. |
| 640x480 KMS all-thread status | With all GStreamer task CPU summed, correct-color `640x480` KMS conversion is much heavier than earlier main-thread-only samples: `10fps` fast `BGRA` is about `22.4%~23.4%`, `15fps` is about `35.5%`, and direct `YUY2 -> kmssink` still fails `not-negotiated`. |
| 640x480 stable low-CPU fallback | Use `640x480` YUYV capture at `10fps`, insert `videorate drop-only=true max-rate=6` before `videoconvert`, then convert only `6fps` to `BGRA` for `kmssink`. Observed all-thread samples measured about `11.2%` over 30 seconds and `13.8%` over a later 20-second check; `10fps -> 8fps` had one short `14.5%` sample but a 30-second sample rose to `18.4%`, so it is not stable below `<15%`. |
| 640x480 lower-fps fallback | Keeping camera input at `640x480@10fps` and dropping before color conversion measured about `11.8%` at `5fps`, `9.4%` at `4fps`, and `7.2%` at `3fps` in 15-second all-thread samples. These are valid low-CPU preview fallbacks only when the user accepts visibly lower motion smoothness; restore `drop 6fps` after experiments unless the user chooses a lower rate. |
| 15fps visible KMS status | `640x480@15fps` visible KMS with correct color remains above the `<15%` target. In all-thread samples, fast `BGRA` measured about `35.5%`; older lighter samples must not be compared unless the CPU measurement method matches. |
| Capture-mode tuning | `io-mode=mmap` is slightly lower than `io-mode=dmabuf` for CPU RGB-conversion KMS routes. At `640x480@10fps`, `mmap + BGRA` measured about `22.4%~23.4%` while `dmabuf + BGRA` measured about `23.4%`. At `320x240@10fps` with a requested full-display render rectangle, `mmap + BGRA + n-threads=1` measured `6.4%~6.5%`, `n-threads=2` measured `6.6%`, `dmabuf + BGRA` measured `6.8%`, and `BGRx` failed with `not-negotiated`. |
| Frame-rate lower bound | Correct-color `640x480@5fps` with fast KMS measured about `11.1%~11.2% CPU` using the all-thread method; older `5.5%~5.7%` values used a lighter process-stat method and must be treated as historical only. |
| 15fps near-target status | `mmap + n-threads=3` at `640x480@15fps` produced one short `14.5%` sample, but a 30-second sample measured `17.2%`; do not claim stable `<15%` until long-run data confirms it. |
| Camera source formats | The current UVC camera exposes only `YUYV` and `MJPEG`; KMS planes expose only RGB-like formats, so a correct visible color route must either decode MJPEG in software or convert YUYV to RGB before KMS. |
| Invalid low-CPU color shortcut | `videoconvert matrix-mode=none` can reduce `640x480@15fps` KMS CPU to about `7.0%`, but human LCD confirmation showed abnormal colors. `matrix-mode=output-only` also showed abnormal colors and measured about `35.1% CPU`. Do not use either for normal color preview. |
| Explicit KMS plane result | Forcing primary plane `33` allows `BGRx` to negotiate but still measures about `17.8% CPU`; `BGRA` on plane `33` is about `17.4%`; `RGBx` and `plane-id=36 + BGRx` remain `not-negotiated`. |
| Generic grayscale result | `YUYV -> GRAY8 -> BGRA -> kmssink` through two `videoconvert` elements measured about `21.6%~22.2% CPU`; generic grayscale conversion is worse than fast color BGRA. |
| Native framebuffer prototype | `/root/uvc_fb_preview_native -w 640 -h 480 -r 15 -p` avoids full-screen scaling and sets camera fps, but still measured about `21.6% CPU`; a LUT attempt measured about `22.6%~22.7%` and was rejected. |
| RGB565 KMS plugin experiment | Adding `DRM_FORMAT_RGB565 -> GST_VIDEO_FORMAT_RGB16` to `gstkmsutils.c` made `gst-inspect-1.0 kmssink` advertise `RGB16`, but the measured routes did not improve: `640x480@10fps RGB16` stayed around `24.5%~25.0%`, `15fps` around `36.8%`, and `10fps -> drop 6fps RGB16` around `14.7%`. Do not keep RGB565 as the preferred KMS color route. |
| ORC experiment | Enabling `BR2_PACKAGE_ORC=y` and rebuilding/deploying `gst1-plugins-base` produced `-Dorc=enabled` and `HAVE_ORC=1`, but visible KMS CPU did not materially improve: `640x480@10fps BGRA` stayed around `23%`, `15fps` around `34.6%`, and `10fps -> drop 6fps` around `13.9%`. Treat ORC as useful package hygiene, not the missing low-CPU breakthrough. |
| Kernel hardware-conversion limit | The STM32 LTDC driver `drivers/gpu/drm/stm/ltdc.c` maps only RGB-like DRM formats (`AR24/XR24/RG24/RG16/AR15/AR12/C8`) and returns `PF_NONE` for YUYV/NV12; the tree contains STM32 DCMI/CEC media drivers but no STM32MP1 DMA2D/Chrom-ART V4L2 mem2mem color converter. Generic `VIDEO_VIM2M` or `VIDEO_MEM2MEM_DEINTERLACE` are not hardware YUYV-to-RGB solutions for this board. |
| LTDC rectangle/scaling limit | `ltdc_plane_atomic_check()` accepts a destination rectangle no smaller than the source, but `ltdc_plane_atomic_update()` programs layer coordinates from the source rectangle and does not configure a scaler. Do not describe `render-rectangle` as proven hardware scaling unless the LCD is visually checked and the driver behavior is revalidated. |
| GL sink status | `v4l2src io-mode=dmabuf ! glupload ! glimagesink` can negotiate GLMemory and consume about `1.9%~2.1% CPU`, but when launched through SSH/background or VT handoff in the current rootfs it produced a black LCD. Treat it as a low-CPU processing benchmark, not a visible route, until LCD visibility is proven again. |
| GL window status | Forcing `GST_GL_WINDOW=gbm` with or without `GST_GL_API=gles2` failed with `EGL_BAD_PARAMETER`. The deployed `libgstopengl.so` exposes X11/Wayland-related strings, not a usable GBM window backend, so GL display needs a compositor/window environment before it can be a visible route. |
| GL download status | `glupload ! glcolorconvert ! gldownload ! kmssink` variants were either black, required CPU conversion, or triggered userspace `SIGSEGV`; do not promote them to the production route without a fresh clean reproduction. |
| MJPEG status | MJPEG capture plus `jpegdec idct-method=ifast` works but software decode is too expensive: all-thread samples measured about `13.7%` at `320x240@10fps`, `20.9%` at `320x240@15fps`, and `50.4%` at `640x480@10fps`. Do not use MJPEG as the low-CPU display route on this board. |
| Native framebuffer status | `/root/uvc_fb_preview_native` is not better than KMS for `640x480`: `-r 10 -p` measured about `28.2%`, `-r 10` full scaling measured about `45.0%`, and `-r 5 -p` measured about `14.1%`. Prefer KMS `10fps -> 6fps`, observed about `11.2%~13.8%`, for the 640x480 low-CPU fallback. |
| Wayland dependency | `waylandsink` and Qt `libqwayland-egl.so` exist, but the active rootfs lacks a Weston compositor binary and `drm-backend.so`; Wayland surface composition requires enabling/deploying Weston first. Buildroot has `BR2_PACKAGE_WAYLAND=y` and `BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_WAYLAND=y`, but Weston is not enabled. In this Buildroot version the standard Weston DRM backend depends on Mesa EGL, so STM32MP1/Gcnano may require ST OpenSTLinux Weston packaging or a Buildroot provider/config patch rather than only toggling `BR2_PACKAGE_WESTON=y`. |
| Custom NEON direct KMS status | A temporary `/tmp/uvc_kms_probe` using V4L2 mmap, a DRM dumb XRGB8888 framebuffer, and custom NEON YUYV-to-XRGB conversion measured `640x480@10fps` at `13.5%` over 10 seconds and `11.2%` over 30 seconds, while preserving a real 10fps display instead of dropping to 6fps. Same-session retest with the extended probe measured direct `XRGB8888` at `13.3%` over 30 seconds. On 2026-05-01 the user confirmed the direct KMS NEON display color was normal, and a visible-candidate run lasted more than 10 minutes (`frames=6477`) with CPU samples around `13.4%~13.7%` and no new `Oops`/`galcore`/`dma_map_sg`/`segfault` logs. The same probe measured `640x480@15fps` at about `20.9%`, so it is not a completed `<15%` 15fps route. A requested `12fps` fell back to actual `10/1` according to `VIDIOC_S_PARM`. Treat NEON direct KMS as the best current 10fps visible candidate, not as proof that 15fps is solved. |
| Custom NEON overlay plane status | The temporary `/tmp/uvc_kms_probe_overlay_rect` experiment has been promoted into project source as `uvc_kms_overlay.c` plus `run_qt_kms_overlay_display.sh`. It uses V4L2 mmap, a `640x480` DRM dumb `ARGB8888` buffer, alpha byte `255`, and `drmModeSetPlane(... plane-id=36 ...)` without calling `drmModeSetCrtc`. The earlier 5-minute probe measured `13.5%~13.8%`; the project-binary route reached `frames=5700` and measured overlay `13.5%/13.6%` plus Qt `2.3%/2.5%` in two 10-second samples. The user confirmed overlay color and Qt/video layout are normal. This is now the preferred integration-oriented 10fps route. |
| Rejected direct KMS micro-optimizations | Direct `RGB565` dumb framebuffer was accepted by KMS (`pitch=2048`) but measured `13.7%` over 30 seconds at `640x480@10fps`, slightly worse than direct `XRGB8888` in the same session. Cached staging plus row `memcpy` also did not help: `XRGB8888 + staging` measured `15.2%`, and `RGB565 + staging` measured `14.4%`. Do not repeat RGB565 or staging-copy as the next breakthrough path unless the conversion code or memory attributes materially change. |
| Next buildable optimization | Buildroot contains `package/libyuv`, and the target toolchain is `cortex-a7 + neon-vfpv4 + hard-float`, but `BR2_PACKAGE_LIBYUV` is not enabled. Since direct KMS RGB565 and cached staging were rejected, the overlay-plane route is now the integration baseline; next experiments should focus on longer start/stop stability and converter-core work such as tighter NEON packing, LUT-assisted BT.601, or libyuv comparison. |

### 4. Validation & Error Matrix

| Check | Good Result | Bad Result | Required Action |
|---|---|---|---|
| Process cleanup | Only one non-zombie `gst-launch-1.0` remains | Multiple active GStreamer processes or stuck display ownership | Kill non-zombie display processes before the next route; ignore zombie PIDs for CPU but record them. |
| LCD visibility | User confirms camera image on LCD | Black LCD while pipeline is PLAYING | Mark route as not visible; restore `gst-fast-kms-10fps` or visible BGRA KMS baseline. |
| CPU target | `640x480@15fps` visible route below `15%` | `15fps` visible route remains around `18%~22%` | Continue Wayland/Weston, custom KMS/NEON, or custom GL investigation. |
| Direct KMS NEON candidate | `640x480@10fps` remains visible and all-thread CPU is in the same range as the drop-6 fallback while keeping actual 10fps | `15fps` remains around `20%`, actual fps silently falls back, or LCD color/position is unacceptable | Keep it as a 10fps candidate only; record actual fps from logs and continue targeted conversion/memory-write experiments. |
| Color correctness | Normal color preview or explicitly accepted grayscale | False color, obvious color channel corruption, or striping | Reject the route for color preview even if CPU is low. |
| Kernel hardware path | Driver source exposes a real YUV/RGB converter, scaler, or V4L2 mem2mem hardware block for STM32MP1 | Only LTDC RGB planes, DCMI capture, DMA/MDMA copy engines, or generic sample mem2mem drivers are present | Do not spend time enabling unrelated Kconfig symbols; pivot to Wayland/GL/NEON conversion. |
| Stability | No new `Oops`, `dma_map_sg`, `gcoTEXTURE`, or `SIGSEGV` | Kernel Oops or userspace crash | Stop the route, save log/core if present, and pivot away from that route. |
| Wayland readiness | `weston` and `drm-backend.so` available, compositor starts | Only `waylandsink` library exists | Enable/deploy Weston before testing `waylandsink`; do not expect `waylandsink` to work alone. |
| Current fallback recovery | `gst-mmap-kms-10fps-n1` shows camera and CPU stays around `11%` | No visible image or CPU jumps toward `40%` | Restore clean KMS baseline and recheck `/dev/video0`, `/dev/dri/card0`, and `vtcon1`. |

### 5. Good/Base/Bad Cases

| Case | Example | Expected Result |
|---|---|---|
| Good current fallback | mmap fast KMS at `640x480@10fps`, `n-threads=1` | Visible camera, about `11.3% CPU`, no new crash logs. |
| Good low-CPU fallback | mmap fast KMS at `320x240@10fps`, `n-threads=1`, `render-rectangle=<0,0,1024,600>` | Low-detail KMS preview, about `6.5% CPU` with all GStreamer tasks summed; actual display size must be visually confirmed on the LCD. |
| Good 640x480 fallback | mmap KMS at `640x480`, source `10fps`, `videorate drop-only max-rate=6` before `videoconvert`, `BGRA`, fullscreen render rectangle | Correct-color 640x480 preview observed about `11.2%~13.8% CPU`; motion smoothness is about `6fps`. |
| Good direct NEON 10fps candidate | `/tmp/uvc_kms_probe -w 640 -h 480 -r 10 -m neon` with V4L2 mmap and a DRM XRGB8888 dumb framebuffer | Preserves actual `10/1` camera fps and measured `11.2%` best-case over a 30-second all-thread sample; a later same-session retest measured `13.3%`. The user confirmed normal color, and a 10-minute-plus visible-candidate run ended at `frames=6477` with all-thread CPU samples around `13.4%~13.7%` and no new crash logs. |
| Good overlay-plane NEON 10fps route | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart` | Starts Qt EGLFS plus project `uvc_kms_overlay` on STM overlay plane `36`, default rectangle `177,73,640,480`, and preserves actual `10/1` camera fps. The user confirmed normal color and integrated layout; project-binary route reached `frames=5700` and measured overlay `13.5%/13.6%` plus Qt `2.3%/2.5%`. |
| Bad direct RGB565 optimization | `/tmp/uvc_kms_probe_stage -w 640 -h 480 -r 10 -m neon -F rgb565 -s direct` | KMS accepts `DRM_FORMAT_RGB565` and halves framebuffer pitch, but CPU measured `13.7%` over 30 seconds, not better than direct `XRGB8888`; keep it as a rejected direct KMS micro-optimization. |
| Bad staging-copy optimization | `/tmp/uvc_kms_probe_stage -w 640 -h 480 -r 10 -m neon -F xrgb8888 -s staging` or `-F rgb565 -s staging` | Cached staging plus row copy measured `15.2%` for XRGB8888 and `14.4%` for RGB565 over 30 seconds; the extra copy outweighed any benefit from cached conversion writes. |
| Lower-motion 640x480 fallback | mmap KMS at `640x480`, source `10fps`, `videorate drop-only max-rate=4` before `videoconvert` | Correct-color preview measured about `9.4% CPU`; use only if the user accepts roughly `4fps` motion. |
| Extreme low-CPU fallback | mmap fast KMS at `640x480@5fps` | Visible camera, about `11.1%~11.2% CPU` with all GStreamer tasks summed; motion smoothness is reduced. |
| Base visible 15fps | Default KMS BGRA at `640x480@15fps` | Visible camera, about `21.6% CPU`; useful as a recovery baseline, not final. |
| Better but still short | Fast KMS BGRA at `640x480@15fps` | Visible camera, about `18.0% CPU`; below old Qt safe path but above target. |
| Better direct 10fps but still not 15fps | Direct NEON KMS at `640x480@15fps` | Measured about `20.9% CPU`; do not claim the production 15fps target is met. |
| Explicit plane not enough | `plane-id=33 + BGRx/BGRA` at `640x480@15fps` | Correct-color KMS route remains about `17.4%~17.8% CPU`; not enough to reach `<15%`. |
| Bad generic grayscale | `YUYV -> GRAY8 -> BGRA -> kmssink` | About `21.6%~22.2% CPU`; if grayscale is needed, write a dedicated Y-plane path instead of chaining generic converters. |
| Bad framebuffer shortcut | Native-size framebuffer prototype at `640x480@15fps` | About `21.6% CPU`; not lower than fast KMS because CPU still converts and writes framebuffer memory. |
| Bad GL visibility claim | `glimagesink` PLAYING with low CPU but black LCD | Not accepted as display success. |
| Bad GL backend assumption | `GST_GL_WINDOW=gbm` on the current rootfs | Fails with `EGL_BAD_PARAMETER`; do not assume a GBM window backend is available just because OpenGL plugins exist. |
| Bad GL download claim | `gldownload` route reports low CPU but LCD black or route crashes | Not accepted; record log and restore visible baseline. |
| Bad MJPEG fallback | MJPEG decode through `jpegdec ! videoconvert ! kmssink` | Software decode is worse than YUYV for this CPU budget: `320x240@15fps` already measured about `20.9%`, and `640x480@10fps` measured about `50.4%`. |
| Bad color shortcut | `videoconvert matrix-mode=none` or `matrix-mode=output-only` at `640x480@15fps` | `none` is low CPU but color abnormal; `output-only` is color abnormal and high CPU. Neither is a valid color preview route. |
| Bad kernel toggle assumption | Enable `VIDEO_VIM2M`, `VIDEO_MEM2MEM_DEINTERLACE`, or DMA/MDMA and expect hardware YUYV-to-RGB | These are sample/generic/copy paths in this kernel context and do not make LTDC scan out YUYV or perform color conversion. |

### 6. Tests Required

- Before each route:
  - Assert `vtcon1` is disabled or intentionally enabled, and clear `/dev/fb0` if console artifacts are present.
  - Assert only one non-zombie display process owns the camera/display.
  - Run display pipelines with `nohup ... &` and log to `/tmp/<route>.log`.
- For every candidate:
  - Record exact pipeline, memory type/caps, resolution, fps, PID, CPU sample duration, and whether a human confirmed LCD visibility.
  - Check `dmesg` for `Oops`, `galcore`, `dma_map_sg`, `segfault`, `gcoTEXTURE`, and `Internal error`.
  - If the route is invisible, restore `gst-fast-kms-10fps` before continuing.
- For direct KMS or custom NEON probes:
  - Assert the probe is a temporary `/tmp` binary or an isolated branch artifact, not a replacement for the Qt app or boot script.
  - Assert the current GStreamer fallback is killed only for the duration of the test, then restored with `nohup ... >/tmp/gst640-src10-drop6-bgra-fullrect.log 2>&1 < /dev/null &`.
  - Assert the log records requested fps and actual fps; reject intermediate fps experiments if `VIDIOC_S_PARM` falls back silently.
  - Assert CPU samples use the same all-thread method as GStreamer route comparisons.
  - Assert human LCD confirmation is collected before promoting a direct probe to a visible fallback.
  - Assert RGB565 or staging-copy experiments are compared against same-session direct XRGB8888 samples, because the fallback CPU can drift around the historical `11.2%~14.2%` range.
  - Assert overlay-plane experiments use `ARGB8888` or another format explicitly listed by plane `36`; `XR24/XRGB8888` is supported on primary plane `33` but not on overlay plane `36`.
  - Assert overlay-plane probes disable their plane on exit and then restore the `drop6` GStreamer fallback; `modetest -M stm -p` should show the fallback owns plane `36` again after recovery.
- For Wayland:
  - Assert `weston` and `drm-backend.so` exist before testing `waylandsink`.
  - Start compositor first, then test a standalone `waylandsink` video surface, then integrate Qt.

### 7. Wrong vs Correct

#### Wrong

```bash
# Wrong: this may leave logs on the LCD console and does not prove clean visibility.
GST_REGISTRY_REBUILD=0 VIDEO_BACKEND=gst-gl /root/qt_camera_display/run_qt_camera_display.sh

# Wrong: treating a live low-CPU gst-launch process as a visible display route.
pidof gst-launch-1.0
```

#### Wrong

```bash
# Wrong: a temporary probe number is useful, but it is not a permanent route yet.
nohup /tmp/uvc_kms_probe -w 640 -h 480 -r 10 -m neon \
  >/tmp/uvc-kms-probe-neon-640x480-10.log 2>&1 < /dev/null &
# Then leave the probe running or claim 15fps is solved from the 10fps result.
```

#### Correct

```bash
# Correct: separate logs from the LCD and use a known visible 640x480 recovery route.
echo 0 > /sys/class/vtconsole/vtcon1/bind
dd if=/dev/zero of=/dev/fb0 bs=1228800 count=1
nohup gst-launch-1.0 -v \
  v4l2src device=/dev/video0 io-mode=mmap ! \
  'video/x-raw,format=YUY2,width=640,height=480,framerate=10/1' ! \
  videorate drop-only=true max-rate=6 skip-to-first=true silent=true ! \
  'video/x-raw,format=YUY2,width=640,height=480,framerate=6/1' ! \
  videoconvert dither=none chroma-mode=none matrix-mode=input-only chroma-resampler=nearest alpha-mode=set alpha-value=1 n-threads=1 qos=false ! \
  'video/x-raw,format=BGRA,width=640,height=480,framerate=6/1' ! \
  kmssink driver-name=stm sync=false async=false enable-last-sample=false qos=false show-preroll-frame=false processing-deadline=0 max-lateness=-1 render-rectangle='<0,0,1024,600>' \
  >/tmp/gst640-src10-drop6-bgra-fullrect.log 2>&1 < /dev/null &
```

#### Correct

```bash
# Correct: stop the current fallback only during the temporary direct probe,
# measure with the all-thread CPU method, read the actual fps from the log,
# kill the probe, and restore the known visible fallback afterward.
nohup /tmp/uvc_kms_probe -w 640 -h 480 -r 10 -m neon \
  >/tmp/uvc-kms-probe-neon-640x480-10.log 2>&1 < /dev/null &
pid=$(pidof uvc_kms_probe | awk '{print $1}')
# sample /proc/$pid/task/*/stat here
kill "$pid"

nohup gst-launch-1.0 -q \
  v4l2src device=/dev/video0 io-mode=mmap ! \
  'video/x-raw,format=YUY2,width=640,height=480,framerate=10/1' ! \
  videorate drop-only=true max-rate=6 skip-to-first=true silent=true ! \
  'video/x-raw,format=YUY2,width=640,height=480,framerate=6/1' ! \
  videoconvert dither=none chroma-mode=none matrix-mode=input-only chroma-resampler=nearest alpha-mode=set alpha-value=1 n-threads=1 qos=false ! \
  'video/x-raw,format=BGRA,width=640,height=480,framerate=6/1' ! \
  kmssink driver-name=stm sync=false async=false enable-last-sample=false qos=false show-preroll-frame=false processing-deadline=0 max-lateness=-1 render-rectangle='<0,0,1024,600>' \
  >/tmp/gst640-src10-drop6-bgra-fullrect.log 2>&1 < /dev/null &

# Correct: keep this as the lower-detail 320x240 safety fallback.
nohup gst-launch-1.0 -v \
  v4l2src device=/dev/video0 io-mode=mmap ! \
  'video/x-raw,format=YUY2,width=320,height=240,framerate=10/1' ! \
  videoconvert dither=none chroma-mode=none matrix-mode=input-only chroma-resampler=nearest n-threads=1 ! \
  'video/x-raw,format=BGRA,width=320,height=240,framerate=10/1' ! \
  kmssink driver-name=stm sync=false render-rectangle='<0,0,1024,600>' \
  >/tmp/gst-kms-320x240-10fps-n1-full.log 2>&1 < /dev/null &
```

---

## Scenario: Non-Interactive Qt Board Deployment When VM Sudo Is Blocked

### 1. Scope / Trigger

- Trigger: A local or nested non-interactive SSH command tries to deploy the Qt camera display into `/home/cfr/linux/nfs/rootfs` with `sudo`, and `sudo` fails because no interactive TTY or askpass program is available.
- Trigger: The Qt/KMS application must be deployed immediately to the currently running STM32MP157 board, but the persistent NFS rootfs path is root-owned and cannot be written by the current non-interactive VM command.
- Trigger: Windows local files under `C:\Users\caofengrui\Desktop\linux` have been edited and must first be synchronized into the VM repository `/home/cfr/linux/Linux_Drivers` before cross-compiling.
- Trigger: `deploy_qt_camera_display.sh` is not usable for the current quick upload because it checks extra deploy artifacts, such as classifier/UNet ONNX models, that are already present on the board runtime directory but missing from the VM default source paths.
- Known failure text: `sudo: 没有终端存在，且未指定 askpass 程序` or `sudo: no tty present and no askpass program specified`.
- Incident pattern: `/home/cfr/linux/nfs/rootfs` and `/home/cfr/linux/nfs/rootfs/root` are root-owned; a command such as `ssh cfr-vm 'cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && sudo ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs ...'` cannot collect the VM sudo password.
- Incident pattern: Windows `scp` can hang before transferring any file. For small controlled file sets, prefer a Git Bash `tar | ssh | tar` stream and prove the result with `sha256sum`.

### 2. Signatures

| Operation | Command Signature |
|---|---|
| Check local changed upload set | `git status --short -- <target-files>` |
| Check VM target hash | `ssh <vm> 'cd /home/cfr/linux/Linux_Drivers && sha256sum <target-files>'` |
| Backup VM target files before overwrite | `ssh <vm> 'cd /home/cfr/linux/Linux_Drivers && mkdir -p /tmp/<backup-name> && cp --parents <target-files> /tmp/<backup-name>/'` |
| Sync Windows target files to VM with tar stream | `cd /c/Users/caofengrui/Desktop/linux && tar -cf - <target-files> \| ssh <vm> 'tar -xf - -C /home/cfr/linux/Linux_Drivers'` |
| Verify Windows-to-VM sync | Compare Windows `Get-FileHash -Algorithm SHA256 <target-files>` with VM `sha256sum <target-files>` |
| Static Qt/KMS contract test | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && sh ./test_qt_kms_overlay_assets.sh` |
| Build Qt binary with embedded QML | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ./build_qt_camera_display.sh` |
| Verify VM build markers | `strings build-mp157/qt_camera_display \| grep -E '<feature-marker-1>|<feature-marker-2>'` |
| Failing persistent deploy shape | `ssh cfr-vm 'cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && sudo ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs'` |
| Model-blocked deploy shape | `./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` fails with `找不到 INT8 ONNX 模型` or `找不到 UNet INT8 ONNX 模型` |
| Enter VM for direct board deploy | `ssh cfr-vm` |
| Check board runtime directory | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 'mount \| grep " on / type nfs "; find /root/qt_camera_display -maxdepth 2 -type f \| sort \| sed -n "1,120p"'` |
| Stop current board app before overwrite | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 '/root/qt_camera_display/run_qt_kms_overlay_display.sh stop || true'` |
| Create timestamped board backup | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 'ts=$(date +%Y%m%d-%H%M%S); mkdir -p /root/qt_camera_display/backup-$ts; cp -a /root/qt_camera_display/qt_camera_display /root/qt_camera_display/backup-$ts/ 2>/dev/null || true; cp -a /root/qt_camera_display/qml /root/qt_camera_display/backup-$ts/ 2>/dev/null || true'` |
| Copy Qt binary from VM to board | `scp -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes build-mp157/qt_camera_display root@192.168.1.250:/root/qt_camera_display/qt_camera_display` |
| Safer staged board binary copy | `scp -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes build-mp157/qt_camera_display root@192.168.1.250:/root/qt_camera_display/qt_camera_display.new` |
| Atomic board binary replace | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 'cd /root/qt_camera_display && ts=$(date +%Y%m%d-%H%M%S); cp -a qt_camera_display qt_camera_display.bak-<task>-$ts; chmod 755 qt_camera_display.new; mv -f qt_camera_display.new qt_camera_display; sync'` |
| Copy QML/runtime assets from VM to board | `scp -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes -r qml root@192.168.1.250:/root/qt_camera_display/` |
| Verify VM-board binary checksum | `sha256sum build-mp157/qt_camera_display` and board `sha256sum /root/qt_camera_display/qt_camera_display` must match |
| Restart direct board runtime | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 '/root/qt_camera_display/run_qt_kms_overlay_display.sh restart'` |
| Check direct board runtime status | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 '/root/qt_camera_display/run_qt_kms_overlay_display.sh status'` |
| Check runtime environment | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 'pid=$(pidof qt_camera_display | awk "{print \\$1}"); tr "\\0" "\\n" </proc/$pid/environ | grep -E "^(VIDEO_BACKEND|QT_QPA_PLATFORM|QT_QPA_GENERIC_PLUGINS|TZ)="; test -S /tmp/uvc-kms-overlay-control.sock; mount | grep " /mnt/sdcard "'` |
| Check deployment logs | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 'tail -n 220 /tmp/qt-kms-overlay-shell.log \| grep -Ei "F4|heartbeat|STATUS|CAL|uart|serial|ERROR" \| tail -n 80 || true'` |

### 3. Contracts

| Area | Contract |
|---|---|
| Windows-to-VM sync scope | Synchronize only the files required for the current task. Do not copy the whole Windows checkout over `/home/cfr/linux/Linux_Drivers`, because the VM repository may contain unrelated local changes and build artifacts. |
| Windows-to-VM backup | Before overwriting VM files, create a `/tmp/<task>_backup_<timestamp>/` backup with `cp --parents` so the old VM state can be inspected or restored manually. |
| Windows-to-VM proof | A file is not considered synchronized until Windows SHA256 and VM `sha256sum` match for every target file. If `scp` hangs or times out, stop the stuck local `scp` process and use the Git Bash `tar | ssh | tar` stream. |
| Build-before-board | Source changes, especially `Main.qml`, are not deployable by themselves. `Main.qml` is embedded through Qt resources; run the Qt build and deploy the relinked `build-mp157/qt_camera_display` binary. |
| Model-blocked deploy fallback | `deploy_qt_camera_display.sh` is the preferred full deploy path. If it fails only because model source paths are missing, and the board already contains the required `/root/qt_camera_display/models/*`, `/root/qt_camera_display/uvc_kms_overlay`, run scripts, and helper binaries, a binary-only board upload is valid for Qt C++/QML changes. |
| Binary-only boundary | Binary-only upload is valid only when the change is inside the Qt executable or embedded QML. If the change touches run scripts, overlay helper, ONNX models, upload helpers, init scripts, or filesystem QML loading, use the full deploy path or copy those exact artifacts too. |
| Persistent rootfs boundary | Writing `/home/cfr/linux/nfs/rootfs` is a persistent NFS rootfs deployment. It requires `sudo` or a human-run interactive command when paths are root-owned. |
| Direct board boundary | Copying files directly to `root@192.168.1.250:/root/qt_camera_display/` updates the currently booted board. If `mount` shows `/` is NFS-mounted from `/home/cfr/linux/nfs/rootfs`, that write also lands in the live NFS rootfs, but it still does not update source control, rebuild artifacts, or the full deploy script's model/source assumptions. |
| Reporting rule | If the sudo deploy command failed, report "direct board deployment" or "runtime-only board deployment"; do not claim `/home/cfr/linux/nfs/rootfs` was updated. |
| Human handoff | If the user needs persistence across rootfs rebuilds or a clean board boot from NFS, provide the exact `sudo ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` command for the human to run interactively on the VM. |
| Overwrite safety | Always stop `qt_camera_display` before replacing the board binary. If the old process still holds the executable, `scp` or `install` can fail with `Text file busy` or leave the old process running. |
| Backup safety | Before overwriting board-side runtime files, create a rollback copy such as `/root/qt_camera_display/qt_camera_display.bak-<task>-<timestamp>` or `/root/qt_camera_display/backup-<timestamp>/` and copy the old binary and QML assets there when they exist. |
| Atomic replace | Prefer copying the new binary to `qt_camera_display.new`, then `chmod 755`, `mv -f`, and `sync` on the board. This avoids leaving a truncated executable at the live path if transfer fails. |
| SSH key policy | VM-to-board SSH and SCP must use `-i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes`; add `-o StrictHostKeyChecking=no` only when the automation context needs first-connection tolerance. |
| Verification policy | Direct board deployment is accepted only after board-side proof: new binary marker or checksum, Qt PID, overlay PID, expected environment, control socket, SD-card mount when photo save/delete is part of the feature, and no stale fallback process conflict. |
| Serial ownership warning | When validating UART/RS485 behavior, do not run `uartapp` against `/dev/ttySTM2` while `qt_camera_display` owns the same port. Use application logs or stop Qt before standalone serial probes. |

### 4. Validation & Error Matrix

| Check | Good Result | Bad Result | Required Action |
|---|---|---|---|
| Windows-to-VM transfer | Windows and VM SHA256 hashes match for all target files | Hash mismatch, `scp` timeout, or stale VM file | Stop stuck transfer if needed, use `tar | ssh | tar`, and recheck hashes before building |
| Static contract | `sh ./test_qt_kms_overlay_assets.sh` prints `PASS` | Missing marker, script failure, or old test script | Fix the code/test mismatch before building or deploying |
| VM build | `./build_qt_camera_display.sh` exits `0` and `file` reports ARM EABI executable | Build fails or host x86 binary is produced | Fix SDK/build environment before board upload |
| Full deploy model inputs | `deploy_qt_camera_display.sh` completes, or model env vars point to valid ONNX/labels files | Fails with missing classifier/UNet model while board already has those runtime files | For Qt-only changes, proceed with binary-only board upload and state that full deploy was model-blocked |
| VM sudo deploy | `sudo ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` completes in an interactive VM shell | `sudo: no tty present...`, Chinese no-tty error, password prompt, or `Permission denied` | Treat NFS rootfs deployment as blocked; either ask the human to run the sudo command or use direct board runtime deployment and label it runtime-only |
| Existing board runtime | `/root/qt_camera_display` contains `qt_camera_display`, `uvc_kms_overlay`, run scripts, models, and helper binaries | Runtime directory is missing or incomplete | Do not use binary-only fallback; run full deploy with valid inputs or restore the runtime package first |
| Process stop before copy | `pidof qt_camera_display` is empty before copying the binary | Qt PID still exists, or copy fails with `Text file busy` | Run `/root/qt_camera_display/run_qt_kms_overlay_display.sh stop`, then recheck PIDs before overwriting |
| Board backup | `/root/qt_camera_display/backup-<timestamp>/` contains the previous binary/QML when they existed | No backup was made before replacing runtime files | Create the backup before retrying the copy; if already overwritten, record that rollback depends on source control or rebuild artifacts |
| Direct copy | Board `/root/qt_camera_display/qt_camera_display` timestamp/checksum matches the VM build artifact | SCP fails, file is missing, or checksum does not match | Fix SSH key/options, board path, or build output path before restart |
| Marker proof | `strings /root/qt_camera_display/qt_camera_display | grep '<feature-marker>'` or `sha256sum` proves the expected build is on the board | Marker absent or checksum is old | Rebuild on VM, recopy, and do not accept runtime validation from an old binary |
| Runtime status | `status` reports one Qt PID and one official `uvc_kms_overlay` PID | Missing PID, only fallback `gst-launch-1.0`, or stale temporary probe | Restart through the control script and inspect `/tmp/qt-kms-overlay-shell.log` and `/tmp/uvc-kms-overlay.log` |
| Runtime environment | `VIDEO_BACKEND=kms-overlay`, `QT_QPA_PLATFORM=eglfs`, touch plugin env, `TZ=CST-8`, and `/tmp/uvc-kms-overlay-control.sock` are present | Env mismatch, missing socket, or wrong touch event path | Fix `run_qt_kms_overlay_display.sh` or QML/runtime assets before declaring the board deployed |
| Feature log check | Logs show the new code path or expected warning, such as F4 heartbeat attempts | Logs show heartbeat lost or no feature marker | Treat upload as successful but feature integration as a separate hardware/protocol issue; inspect RS485/F4 without racing Qt on the serial port |
| SD-card path for photo features | `mount | grep ' /mnt/sdcard '` succeeds and photo files can be listed or deleted from the expected path | SD card not mounted or file delete only updates UI state | Mount/repair SD path first; photo deletion features must verify the actual file disappeared |

### 5. Good/Base/Bad Cases

| Case | Example | Expected Result |
|---|---|---|
| Good persistent deployment | Human runs `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && sudo ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` in an interactive VM shell | NFS rootfs is updated and the claim may say persistent rootfs deployment succeeded |
| Good Windows-to-VM sync | Backup VM files, stream only the changed target files with `tar | ssh | tar`, then compare Windows and VM SHA256 | VM build uses exactly the Windows-edited files and unrelated VM changes are not overwritten |
| Good direct runtime deployment | VM stops board app, backs up `/root/qt_camera_display`, SCPs the new binary/QML to the board, restarts, and verifies PIDs/env/socket/SD mount | Current board runtime is updated; final report explicitly says this was direct board deployment, not NFS rootfs update |
| Good model-blocked Qt-only upload | Full deploy fails because a VM model path is missing, but board already has models and helpers; upload only `build-mp157/qt_camera_display.new`, atomically replace it, restart, and compare SHA256 | New Qt/QML behavior reaches the board without disturbing models, overlay helper, COS credentials, or scripts |
| Base fallback | Non-interactive sudo fails and the user only needs a quick board check | Use direct board deployment, record the persistence limitation, and provide the human-run sudo command for later |
| Bad false persistence claim | `sudo` failed, then files were SCPed to `root@192.168.1.250:/root/qt_camera_display/`, but the report says `/home/cfr/linux/nfs/rootfs` was written | Future reboots or rootfs rebuilds can lose the change; correct the report and mark the deployment runtime-only |
| Bad source-only upload | Copy `qml/Main.qml` or `main.cpp` to the board but do not rebuild `qt_camera_display` | Board still runs the old embedded QML/C++ binary |
| Bad forced full deploy | Invent placeholder model paths or delete model checks just to make `deploy_qt_camera_display.sh` pass | Board runtime can lose required detection assets or diverge from the deployment contract |
| Bad overwrite order | SCP the binary while `qt_camera_display` is still running | Copy can fail or validation can test the old process; stop first, verify PID is gone, then copy |
| Bad verification | Only checking that SSH/SCP returned `0` | Deployment can still be old or partially restarted; require marker/checksum and board runtime status proof |

### 6. Tests Required

- Windows-to-VM sync checks:
  - Assert `git status --short -- <target-files>` shows only the intended upload set for the current task.
  - Assert the VM files are backed up before overwrite.
  - Assert Windows SHA256 and VM `sha256sum` match after the tar-stream sync.
- VM build/static checks:
  - Assert `./test_qt_kms_overlay_assets.sh` passes before deployment.
  - Assert `./build_qt_camera_display.sh` succeeds and `file build-mp157/qt_camera_display` reports an ARM target binary.
  - Assert `strings build-mp157/qt_camera_display | grep -E '<feature-marker>'` sees the expected new markers when the feature has stable strings.
- Direct board deployment checks:
  - Assert the board app is stopped before overwriting: `pidof qt_camera_display` should be empty.
  - Assert a timestamped backup exists, either under `/root/qt_camera_display/backup-<timestamp>/` or as `qt_camera_display.bak-<task>-<timestamp>`.
  - Assert `sha256sum build-mp157/qt_camera_display` on the VM matches `sha256sum /root/qt_camera_display/qt_camera_display` on the board, or assert a deliberate `strings` marker from the changed feature exists on the board binary.
  - Assert `/root/qt_camera_display/run_qt_kms_overlay_display.sh status` reports the expected Qt and overlay PIDs after restart.
  - Assert `/proc/<qtpid>/environ` contains `VIDEO_BACKEND=kms-overlay`, `QT_QPA_PLATFORM=eglfs`, expected touch variables, and `TZ=CST-8`.
  - Assert `/tmp/uvc-kms-overlay-control.sock` exists.
  - Assert logs in `/tmp/qt-kms-overlay-shell.log` show the new path or a meaningful runtime result. For serial/F4 work, a `f4-heartbeat-lost` log after successful binary upload is a hardware/protocol validation failure, not an upload failure.
  - For photo save/delete work, assert `/mnt/sdcard` is mounted and the target photo file exists before delete and disappears after delete.
- Reporting checks:
  - If non-interactive VM sudo failed, the final report must include the limitation: direct board deployment succeeded for the current runtime, but `/home/cfr/linux/nfs/rootfs` was not written.
  - If the full deploy script was model-blocked but binary-only upload succeeded, report both facts: the Qt executable is on the board, while the full rootfs deploy script still needs valid `DEFECT_MODEL_SRC`, `DEFECT_LABELS_SRC`, and `DEFECT_UNET_MODEL_SRC` for a clean full deployment.

### 7. Wrong vs Correct

#### Wrong

```bash
# Wrong: sudo failed, so this did not update the persistent NFS rootfs.
ssh cfr-vm 'cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && sudo ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs'
echo "NFS rootfs deployed"
```

#### Wrong

```bash
# Wrong: overwrites the board binary without stopping the running Qt process or creating a rollback point.
scp -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes \
  build-mp157/qt_camera_display \
  root@192.168.1.250:/root/qt_camera_display/qt_camera_display
```

#### Correct

```bash
# Correct: first make the VM repository exactly match the Windows-edited task files.
# Run from Git Bash on Windows.
cd /c/Users/caofengrui/Desktop/linux
tar -cf - \
  20_uvc_camera/README.md \
  20_uvc_camera/qt_camera_display/README.md \
  20_uvc_camera/qt_camera_display/main.cpp \
  20_uvc_camera/qt_camera_display/qml/Main.qml \
  20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh \
| ssh -i /c/Users/caofengrui/Desktop/linux/.codex_tmp/cfr_vm_key_sandbox \
    -o IdentitiesOnly=yes \
    -o UserKnownHostsFile=/c/Users/caofengrui/Desktop/linux/.codex_tmp/known_hosts \
    cfr@192.168.234.130 \
    'tar -xf - -C /home/cfr/linux/Linux_Drivers'
```

```bash
# Correct: when non-interactive VM sudo is blocked, label this as a direct board runtime deployment.
cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display
./test_qt_kms_overlay_assets.sh
./build_qt_camera_display.sh

ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes \
  root@192.168.1.250 \
  '/root/qt_camera_display/run_qt_kms_overlay_display.sh stop || true'

ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes \
  root@192.168.1.250 \
  'ts=$(date +%Y%m%d-%H%M%S); mkdir -p /root/qt_camera_display/backup-$ts; cp -a /root/qt_camera_display/qt_camera_display /root/qt_camera_display/backup-$ts/ 2>/dev/null || true; cp -a /root/qt_camera_display/qml /root/qt_camera_display/backup-$ts/ 2>/dev/null || true'

scp -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes \
  build-mp157/qt_camera_display \
  root@192.168.1.250:/root/qt_camera_display/qt_camera_display.new

ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes \
  root@192.168.1.250 \
  'cd /root/qt_camera_display && ts=$(date +%Y%m%d-%H%M%S); cp -a qt_camera_display qt_camera_display.bak-qt-$ts; chmod 755 qt_camera_display.new; mv -f qt_camera_display.new qt_camera_display; sync'

ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes \
  root@192.168.1.250 \
  '/root/qt_camera_display/run_qt_kms_overlay_display.sh restart && /root/qt_camera_display/run_qt_kms_overlay_display.sh status'

sha256sum build-mp157/qt_camera_display
ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes \
  root@192.168.1.250 \
  'sha256sum /root/qt_camera_display/qt_camera_display; strings /root/qt_camera_display/qt_camera_display | grep -E "<feature-marker>" | head'
```

---

## Scenario: Qt EGLFS Plus KMS Overlay Plane Runtime

### 1. Scope / Trigger

- Trigger: Promoting the temporary `/tmp/uvc_kms_probe_overlay_rect` experiment into maintainable project source, deployment artifacts, or board start/stop controls.
- Trigger: Running the confirmed Qt EGLFS UI plus KMS overlay video route on STM32MP157.
- Board route: Qt owns the primary UI surface; `/root/qt_camera_display/uvc_kms_overlay` owns STM overlay plane `36`.
- Camera: UVC `/dev/video0`, supported source formats are `YUYV` and `MJPEG`; the production overlay route uses `YUYV`.

### 2. Signatures

| Operation | Command Signature |
|---|---|
| Build overlay helper | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ./build_uvc_kms_overlay.sh` |
| Build Qt UI | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ./build_qt_camera_display.sh` |
| Static asset contract | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ./test_qt_kms_overlay_assets.sh` |
| Deploy to NFS rootfs | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && sudo ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` |
| Board start | `/root/qt_camera_display/run_qt_kms_overlay_display.sh start` |
| Board stop | `/root/qt_camera_display/run_qt_kms_overlay_display.sh stop` |
| Board restart | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart` |
| Board status | `/root/qt_camera_display/run_qt_kms_overlay_display.sh status` |
| Board fallback recovery | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restore-fallback` |
| Board default init route | `/etc/init.d/S90uvc-camera restart && /etc/init.d/S90uvc-camera status` |
| Touch event discovery | `grep -A6 -i "Goodix" /proc/bus/input/devices | sed -n 's/^H: Handlers=.*\(event[0-9][0-9]*\).*/\/dev\/input\/\1/p' | head -n 1` |
| Qt touch FD check | `qtpid=$(pidof qt_camera_display); ls -l /proc/$qtpid/fd | grep /dev/input/event` |
| Manual touch acceptance | Tap the QML `开始`, `暂停`, `继续`, and `停止` buttons and confirm the workflow/status text changes on the LCD. |
| Overlay helper direct form | `nohup /root/qt_camera_display/uvc_kms_overlay -d /dev/video0 -w 640 -h 480 -r 10 -m neon -F argb8888 -P 36 -x 177 -y 73 -W 640 -H 480 >/tmp/uvc-kms-overlay.log 2>&1 < /dev/null &` |
| ROI overlay static contract | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ./test_qt_kms_overlay_assets.sh` must require `draw_roi_overlay_row`, `copy_yuyv_frame_to_rgb24`, and `yuyv_map`, and must reject `refresh_clean_rgb24_snapshot` and post-frame `draw_roi_overlay_box(kms,...)` calls. |
| Board ROI binary proof | `strings /root/qt_camera_display/uvc_kms_overlay \| grep -E 'draw_roi_overlay_row|copy_yuyv_frame_to_rgb24|yuyv_map|clean_rgb24|refresh_clean'` must show the row/YUYV markers and no old per-frame `clean_rgb24` path. |
| Board ROI CPU smoke check | `top -bn1 \| grep -E 'uvc_kms_overlay|qt_camera_display|CPU:'` after restart must be captured before claiming ROI drawing did not regress preview performance. |

### 3. Contracts

| Area | Contract |
|---|---|
| Source ownership | `uvc_kms_overlay.c` is the maintained project source for the custom V4L2 mmap + NEON YUYV-to-ARGB8888 + DRM plane helper. Do not keep editing `/tmp/uvc_kms_probe_overlay_rect.c` as the source of truth. |
| Qt backend | `VIDEO_BACKEND=kms-overlay` starts the Qt UI shell only; Qt must not open `/dev/video0` in this mode. |
| Display ownership | The overlay helper uses `drmModeSetPlane` on plane `36` and must not call `drmModeSetCrtc`, so it can coexist with Qt EGLFS on the primary UI plane. |
| Default geometry | Default placement is `plane=36`, `x=177`, `y=73`, `w=640`, `h=480`; this matches the user-accepted LCD placement and the QML reserved video frame. |
| Default camera mode | Default capture is `640x480@10fps`; actual fps must be read from overlay logs because unsupported requests can silently fall back. |
| Pixel format | Use `-F argb8888` for plane `36`; historical tests showed `XR24/XRGB8888` belongs to primary plane `33`, not this overlay plane. |
| ROI display contract | The model observation ROI is a display overlay only. For a center `300x300` ROI, draw the border inside the YUYV conversion row path, for example through `draw_roi_overlay_row_xrgb()` / `draw_roi_overlay_row_rgb565()`, immediately after each destination row is converted. Do not convert the full frame and then draw the box afterward on the same single framebuffer; that creates a visible cover/redraw flicker during LCD scanout. |
| ROI input contract | Saved/detection images must not be copied from the ROI-decorated KMS framebuffer. Keep the current dequeued V4L2 `YUYV` buffer address in `latest_frame.yuyv_map` and generate JPG/PNG/detect RGB data from that raw source with `copy_yuyv_frame_to_rgb24()`. |
| ROI performance contract | Normal preview must not allocate or fill a full-frame RGB24 snapshot on every camera frame just to keep model input clean. A `640x480` RGB24 copy is about 900 KiB per frame; at `10fps` it is enough to add visible CPU/memory-bandwidth cost on Cortex-A7. Only convert full-frame RGB24 on explicit save/detect requests. |
| Background policy | Both Qt and overlay display processes must be started by the control script through `nohup ... >/tmp/<route>.log 2>&1 < /dev/null &`. |
| Default init policy | `/etc/init.d/S90uvc-camera` defaults to `UVC_BACKEND=${UVC_BACKEND:-qt-kms-overlay}` for the industrial Qt UI. It must load `galcore`, call `run_qt_kms_overlay_display.sh start` with `SKIP_UVC_INIT_STOP=1`, and report both Qt and overlay PIDs in `status`. |
| Failure recovery | If overlay or Qt fails to stay alive during `start`, the control script must stop partial processes and restore the recommended `640x480@10fps -> drop6 -> BGRA -> kmssink` fallback. |
| Legacy cleanup | `start` and `restore-fallback` must clear old `gst-launch-1.0`, `uvc_fb_preview`, `uvc_kms_probe*`, `qt_camera_display`, and `uvc_kms_overlay` processes before starting a new visible route. |
| Status reporting | `status` must print Qt PID, project overlay PID, GStreamer fallback PID, and legacy temporary probe PIDs. New sessions must still run `pidof gst-launch-1.0` or `status` before changing the display route. |
| Qt touch input policy | EGLFS input must be enabled with `QT_QPA_EGLFS_DISABLE_INPUT=0`. The runtime script must explicitly load `evdevtouch` through `QT_QPA_GENERIC_PLUGINS=evdevtouch:/dev/input/eventX` and set `QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=/dev/input/eventX`, where `eventX` is discovered from the Goodix block in `/proc/bus/input/devices`. Do not hard-code `event2`, because input ordering can change when the UVC camera or gpio-keys appear first. |
| QML control boundary | The `开始/暂停/继续/停止` buttons belong to the Qt primary UI surface, not the KMS video plane. Place them in a non-video panel such as the right result panel. In `kms-overlay` mode, button handlers may update QML workflow/status state, but must not kill `uvc_kms_overlay` or the Qt process; process lifecycle remains the shell script's job. |
| Touch proof policy | Seeing buttons on the LCD is not enough. Verification must prove Qt opened the Goodix event node, the Qt log names the touch event path, and a human tap changes the QML status/workflow text. |
| Performance claim | Treat this as the best current integration-oriented `640x480@10fps` route, not as proof that `640x480@15fps <15%` is solved. The project-binary route reached `frames=5700`; two 10-second samples measured overlay `13.5%/13.6%` and Qt `2.3%/2.5%`. |

### 4. Validation & Error Matrix

| Check | Good Result | Bad Result | Required Action |
|---|---|---|---|
| Build helper | `build-mp157/uvc_kms_overlay` is an ARM hard-float executable linked with `libdrm` | Missing `xf86drm.h`, missing cross compiler, or x86 binary | Fix `BR_OUTPUT`, `SYSROOT`, `CC`, and include paths before deploying |
| Static contract | `test_qt_kms_overlay_assets.sh` prints `PASS` | Missing `drmModeSetPlane`, `nohup`, `VIDEO_BACKEND=kms-overlay`, or control verbs | Fix source/scripts before board testing |
| Start status | `status` shows one `qt_camera_display` PID, one `uvc_kms_overlay` PID, no `gst-launch-1.0`, and no legacy probe PID | Old `/tmp/uvc_kms_probe_overlay_rect` still running or project overlay missing | Run `restart`; if it fails, inspect `/tmp/uvc-kms-overlay.log` and fallback status |
| Logs | Overlay log shows `display target: plane 36` and increasing `frames=...`; Qt log shows `backend=kms-overlay` | Overlay exits, Qt opens camera, or fps/format mismatch | Restore fallback, then fix the relevant script or helper args |
| Default init route | `/etc/init.d/S90uvc-camera status` reports `qt <pid>, overlay <pid>` after restart | Init status reports old `fb-preview`, only one PID, or no Qt/overlay PIDs | Update `S90uvc-camera`, redeploy it to NFS rootfs, and rerun `restart/status` |
| Touch event discovery | Runtime log prints `touch input: /dev/input/eventX` and `/proc/$qtpid/fd` contains that node | Log says Goodix not found or Qt has no `/dev/input/event*` FD | Fix the Goodix event parser or set `TOUCH_DEV=/dev/input/eventX`; do not accept visual-only touch claims |
| Touch button behavior | Human taps `开始/暂停/继续/停止` and sees the workflow/status text change | Taps do nothing, coordinates are wrong, or the process stops unexpectedly | Check `QT_QPA_GENERIC_PLUGINS`, `QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS`, button `MouseArea`, and KMS-mode action handling |
| Crash check | `dmesg` has no new `Oops`, `dma_map_sg`, `gcoTEXTURE`, `segfault`, or `Internal error` | Any new kernel or Vivante userspace crash appears | Stop the route, preserve logs, and do not keep retrying the same failing path |
| LCD layout | User confirms video is visually integrated with the Qt reserved frame | Video covers text, is offset, or appears as an unrelated floating image | Adjust `KMS_OVERLAY_X/Y/W/H` and matching QML layout together |
| ROI static markers | Static contract requires `draw_roi_overlay_row`, `copy_yuyv_frame_to_rgb24`, and `yuyv_map`; it rejects `refresh_clean_rgb24_snapshot` and post-frame `draw_roi_overlay_box(kms,...)` | ROI may flicker, preview may stutter, or detection input may include display-only graphics | Move ROI drawing into per-row conversion and generate saved/detect RGB from the raw YUYV buffer |
| ROI binary proof | Board binary strings show `draw_roi_overlay_row_xrgb`, `draw_roi_overlay_row_rgb565`, `copy_yuyv_frame_to_rgb24`, and `yuyv_map`, and do not show old `clean_rgb24`/`refresh_clean` symbols | Board still runs an old binary or VM compiled stale source | Resync sources to `cfr-vm`, rebuild, redeploy, and restart before LCD acceptance |
| ROI visual acceptance | Human observes center `300x300` ROI without obvious flashing after `run_qt_kms_overlay_display.sh restart` | Border visibly blinks or appears intermittently | Check for post-frame drawing, single-buffer scanout timing, or old binary deployment |
| ROI performance smoke | Board `top -bn1` sample after restart shows the overlay process remains in the expected low CPU range for `640x480@10fps`; record the sample when changing hot-loop drawing | Overlay CPU jumps after adding display annotations | Remove per-frame full-image copies, reduce overlay pixels, or move work out of the preview hot loop |
| Detection image cleanliness | Latest `/tmp/qt-defect-detect/*.jpg` is generated from raw YUYV and does not include the green ROI border | The classifier may learn or react to the display overlay instead of the part | Stop reading decorated `fb_map` for model input; use `yuyv_map` -> RGB24 conversion |
| Fallback recovery | `restore-fallback` starts `gst-launch-1.0` with `/tmp/gst640-src10-drop6-bgra-fullrect.log` | No visible image or multiple display owners remain | Kill legacy display processes, restore fallback manually, then fix control script |

### 5. Good/Base/Bad Cases

| Case | Example | Expected Result |
|---|---|---|
| Good official route | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart` | Qt UI and camera video coexist; `status` shows project `uvc_kms_overlay`, no `/tmp` probe, and no fallback `gst-launch-1.0`. |
| Good touch-enabled route | `run_qt_camera_display.sh` logs `touch input: /dev/input/event2` on the current board and `ls -l /proc/$(pidof qt_camera_display)/fd` shows `/dev/input/event2` | Qt QML buttons can receive Goodix touch events; final acceptance still requires tapping all four buttons on the LCD. |
| Good default boot route | `/etc/init.d/S90uvc-camera restart && /etc/init.d/S90uvc-camera status` | Starts the same Qt + KMS overlay route as the manual script and reports both Qt and overlay PIDs. |
| Good direct helper test | `nohup /root/qt_camera_display/uvc_kms_overlay ... -P 36 -x 177 -y 73 -W 640 -H 480 >/tmp/uvc-kms-overlay.log 2>&1 < /dev/null &` | Useful for isolated overlay debugging when Qt is not part of the test; still must be killed or controlled afterward. |
| Base recovery | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restore-fallback` | Restores the known visible drop-6 GStreamer route after a failed overlay/Qt test. |
| Bad source of truth | Editing `/tmp/uvc_kms_probe_overlay_rect.c` and copying binaries by hand | The project cannot reproduce or deploy the route; move changes into `uvc_kms_overlay.c` and rebuild. |
| Bad touch assumption | `QT_QPA_EGLFS_DISABLE_INPUT=0` is set but Qt has no `/dev/input/eventX` FD | EGLFS input was not actually wired; explicitly load `evdevtouch` and verify the FD. |
| Bad overlay control | QML `暂停` or `停止` kills `uvc_kms_overlay` directly | The UI can tear down the visible video plane unexpectedly; keep process control in `run_qt_kms_overlay_display.sh`. |
| Bad foreground route | Running overlay or GStreamer display commands without `nohup` and `/tmp` log redirection | Console output can pollute the LCD and the session owns the visible process. |
| Bad 15fps claim | Reporting project overlay `640x480@10fps` CPU as the final `15fps` solution | The current route is a correct-color, integrated 10fps candidate; 15fps optimization remains open. |
| Good ROI display route | Draw ROI border in `draw_roi_overlay_row_xrgb()` / `draw_roi_overlay_row_rgb565()` as part of each row conversion | The border is present in the displayed video frame without a full-frame post-draw pass. |
| Good ROI model-input route | `SAVE_DETECT` stores the current `yuyv_map` source and calls `copy_yuyv_frame_to_rgb24()` only when a save/detect request arrives | Detection JPG is clean and preview does not pay a per-frame RGB24 snapshot cost. |
| Bad ROI flicker route | Convert the whole frame to `kms->fb_map`, then call `draw_roi_overlay_box(kms,...)` afterward | On a single framebuffer, scanout can show the frame before the border is redrawn, causing visible flashing. |
| Bad ROI performance route | Keep a `refresh_clean_rgb24_snapshot()` call in the capture loop to allocate/fill a full RGB24 clean frame every preview frame | Cortex-A7 spends memory bandwidth on data only needed for occasional save/detect requests, causing visible stutter. |

### 6. Tests Required

- Local/VM:
  - Assert `/bin/sh -n build_uvc_kms_overlay.sh run_qt_kms_overlay_display.sh run_qt_camera_display.sh test_qt_kms_overlay_assets.sh` succeeds.
  - Assert `./test_qt_kms_overlay_assets.sh` succeeds.
  - Assert `./build_uvc_kms_overlay.sh` succeeds and `file build-mp157/uvc_kms_overlay` reports ARM EABI.
  - Assert `./build_qt_camera_display.sh` succeeds after QML or backend changes.
  - After changing ROI drawing or detection-image capture, assert `test_qt_kms_overlay_assets.sh` rejects the old hot-loop `refresh_clean_rgb24_snapshot` pattern and the old post-frame `draw_roi_overlay_box(kms,...)` pattern.
- Deployment:
  - Assert `/root/qt_camera_display/qt_camera_display`, `/root/qt_camera_display/uvc_kms_overlay`, and both run scripts are executable on the board.
  - If `sudo ./deploy_qt_camera_display.sh` is blocked by VM sudo, deploy via root SSH/SCP and record it as a direct board deployment, not a persistent rootfs rebuild.
- Board:
  - Before starting, assert current state with `pidof gst-launch-1.0`, `pidof qt_camera_display`, `pidof uvc_kms_overlay`, and `pidof uvc_kms_probe_overlay_rect`.
  - Run `restart`, then assert `status` reports only the official Qt and overlay processes for this route.
  - Run `/etc/init.d/S90uvc-camera restart` after changing default boot behavior, then assert `/etc/init.d/S90uvc-camera status` reports both Qt and overlay PIDs.
  - Assert the Qt log contains `touch input: /dev/input/eventX` and `ls -l /proc/$(pidof qt_camera_display)/fd | grep /dev/input/event` shows the same Goodix event node.
  - Tap the four QML buttons `开始`, `暂停`, `继续`, and `停止` on the LCD; accept the touch feature only after a human confirms the status/workflow text changes for each button.
  - Sample Qt and overlay CPU with the all-thread `/proc/$pid/task/*/stat` method.
  - After changing ROI or any preview annotation, run `strings /root/qt_camera_display/uvc_kms_overlay | grep -E 'draw_roi_overlay_row|copy_yuyv_frame_to_rgb24|yuyv_map|clean_rgb24|refresh_clean'` and verify the deployed binary contains the row/YUYV markers without the old per-frame snapshot path.
  - After changing ROI or any preview annotation, collect human LCD confirmation that the ROI is stable and not visibly blinking, then record a `top -bn1` CPU sample for `uvc_kms_overlay`.
  - After changing detection-image capture, generate or reuse a `/tmp/qt-defect-detect/*.jpg`, run `defect-classify` on it, and confirm the JPG is a clean camera frame without the display ROI border.
  - Check `/tmp/uvc-kms-overlay.log`, `/tmp/qt-kms-overlay-shell.log`, and `dmesg` crash keywords.
  - Collect human LCD confirmation after layout or geometry changes.

### 7. Wrong vs Correct

#### Wrong

```bash
# Wrong: this leaves the maintained project source out of the loop.
nohup /tmp/uvc_kms_probe_overlay_rect -w 640 -h 480 -r 10 -m neon -F argb8888 -P 36 \
  -x 177 -y 73 -W 640 -H 480 >/tmp/uvc-kms-overlay-rect-integrated.log 2>&1 < /dev/null &
```

#### Wrong

```bash
# Wrong: status ignores the temporary probe and can hide a display owner conflict.
pidof uvc_kms_overlay
pidof qt_camera_display
```

#### Wrong

```bash
# Wrong: the touchscreen may still be disconnected from Qt even when EGLFS input is not disabled.
export QT_QPA_EGLFS_DISABLE_INPUT=0
/root/qt_camera_display/run_qt_kms_overlay_display.sh restart
echo "touch should work"
```

#### Wrong

```c
/* Wrong: this draws the ROI after the whole frame has already been written.
 * On a single scanout framebuffer the LCD can show the no-border frame first,
 * then the border later, which appears as visible flicker.
 */
convert_yuyv_to_xrgb_center(kms->fb_map, ...);
draw_roi_overlay_box(kms, cam->width, cam->height, output_format);
```

#### Wrong

```c
/* Wrong: this pays a full 640x480 RGB24 copy every preview frame only so an
 * occasional detect/save command can avoid the display overlay.
 */
refresh_clean_rgb24_snapshot(&latest);
poll_control_server(control, &latest);
```

#### Correct

```bash
cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display
./test_qt_kms_overlay_assets.sh
./build_uvc_kms_overlay.sh
./build_qt_camera_display.sh

/root/qt_camera_display/run_qt_kms_overlay_display.sh restart
/root/qt_camera_display/run_qt_kms_overlay_display.sh status
```

#### Correct

```bash
# Correct: prove the Goodix event node is wired to Qt before claiming touch support.
/root/qt_camera_display/run_qt_kms_overlay_display.sh restart
qtpid=$(pidof qt_camera_display)
grep -n "touch input" /tmp/qt-kms-overlay-shell.log
ls -l /proc/$qtpid/fd | grep /dev/input/event

# Then collect human LCD confirmation by tapping all four QML buttons and checking status text changes.
```

#### Correct

```c
/* Correct: display-only ROI pixels are written in the same row pass that
 * writes the converted camera pixels, minimizing the visible redraw window.
 */
convert_yuyv_to_xrgb_center(...)
{
    /* ...convert one YUYV row... */
    draw_roi_overlay_row_xrgb(dst_line, y, src_width, src_height);
}
```

#### Correct

```c
/* Correct: keep the raw YUYV source for the current dequeued buffer and
 * convert it only when SAVE_DETECT/SAVE_DUAL actually needs an image file.
 */
latest.yuyv_map = cam->buffers[buf.index].start;
latest.yuyv_size = cam->buffers[buf.index].length;
rgb = copy_yuyv_frame_to_rgb24(&latest);
```

#### Correct

```bash
# Correct: after a failed start or bad layout test, return to the known visible line.
/root/qt_camera_display/run_qt_kms_overlay_display.sh restore-fallback
```

---

## Scenario: STM32MP157 Boot Display Order And Fbcon Cursor

### 1. Scope / Trigger

- Trigger: The LCD shows a blinking `_` cursor after the Linux logo is hidden.
- Trigger: The UVC camera image appears before the Qt splash screen or before the Qt main UI.
- Trigger: A reboot behaves differently from manually killing and restarting `qt_camera_display` or `uvc_kms_overlay`.
- Trigger: Any change to kernel logo, framebuffer console, Buildroot init scripts, Qt splash QML, or KMS overlay visibility.
- Board route: U-Boot loads `uImage` and `stm32mp157d-atk.dtb` from `/home/cfr/linux/tftpboot`; the board mounts NFS rootfs from `/home/cfr/linux/nfs/rootfs`; Qt runs from `/root/qt_camera_display`.

### 2. Signatures

| Operation | Command Signature |
|---|---|
| Kernel source tree | `cd /home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31` |
| Disable Linux logo in defconfig | `grep -n "CONFIG_LOGO" arch/arm/configs/stm32mp1_atk_defconfig` must show `# CONFIG_LOGO is not set` |
| Disable fbcon cursor by kernel default | `grep -n "global_cursor_default" drivers/tty/vt/vt.c` must show `int global_cursor_default = 0;` |
| Kernel build with explicit toolchain path | `PATH=/usr/local/arm/gcc-arm-9.2-2019.12-x86_64-arm-none-linux-gnueabihf/bin:$PATH make uImage LOADADDR=0xC2000040 -j8` |
| Device-tree build | `make dtbs` |
| Kernel/TFTP deploy | `sudo cp arch/arm/boot/uImage arch/arm/boot/dts/stm32mp157d-atk.dtb /home/cfr/linux/tftpboot/ -f` |
| Qt/overlay static contract | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ./test_qt_kms_overlay_assets.sh` |
| Build overlay helper | `./build_uvc_kms_overlay.sh` |
| Build Qt binary with embedded QML | `./build_qt_camera_display.sh` |
| Deploy Qt route to NFS rootfs | `sudo ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` |
| Board reboot | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 reboot` |
| Board cursor proof | `cat /sys/class/graphics/fbcon/cursor_blink` |
| Board Qt/overlay status | `/root/qt_camera_display/run_qt_kms_overlay_display.sh status` |
| Overlay initial hidden proof | `grep -a "initial-visible" /tmp/uvc-kms-overlay.log | tail -n 1` |
| Qt splash/control proof | `grep -a "boot overlay visible\|overlay visibility requested" /tmp/qt-kms-overlay-shell.log | tail -n 30` |
| New kernel proof | `uname -a` must show a build timestamp newer than the kernel change |

### 3. Contracts

| Area | Contract |
|---|---|
| Layer ownership | The blinking `_` after hiding the Linux logo is framebuffer console cursor state, not a Qt bug. If it appears before `/etc/init.d/S05display-quiet` can run, fix the kernel default in `drivers/tty/vt/vt.c`; rootfs scripts can only be a late fallback. |
| Kernel cursor default | `global_cursor_default` must default to `0` for the polished LCD boot route. A bootargs-only or rootfs-only solution is not enough for the earliest visible cursor window. |
| Logo config | Hiding the penguin logo requires `# CONFIG_LOGO is not set` in `arch/arm/configs/stm32mp1_atk_defconfig`, but that setting does not disable the fbcon cursor. Treat logo and cursor as separate controls. |
| Defconfig persistence | Kernel menuconfig changes must be saved to `./arch/arm/configs/stm32mp1_atk_defconfig`; `.config` alone is not persistent. |
| Kernel deployment | Editing kernel source or defconfig is not deployment. The new `uImage` and `stm32mp157d-atk.dtb` must be copied to `/home/cfr/linux/tftpboot/`, then the board must reboot from that image. |
| Non-interactive kernel builds | Non-interactive VM SSH commands must prepend `/usr/local/arm/gcc-arm-9.2-2019.12-x86_64-arm-none-linux-gnueabihf/bin` to `PATH`; otherwise `make uImage` can fail with `arm-none-linux-gnueabihf-gcc: Command not found`. |
| Init fallback | Keep `/etc/init.d/S05display-quiet` deployed before `S90uvc-camera` to write `/sys/class/graphics/fbcon/cursor_blink=0` and clear `/dev/tty0`. This is a fallback, not the root cause fix for the first cursor frame. |
| Qt-first boot policy | The boot display route must start Qt first, wait for a QML boot/splash readiness marker, then start `uvc_kms_overlay` with `-V 0`. Do not start the camera overlay first and hope QML later covers it. |
| Overlay initial visibility | `uvc_kms_overlay` must support `-V 0` and call `drmModeSetPlane(... fb=0, crtc=0 ...)` during hidden startup. The overlay log must report `initial-visible=0` during normal boot. |
| Splash restore policy | QML must hide the overlay at boot and restore it only after the splash has faded out. Because the overlay socket can appear after QML, QML must retry `VISIBLE 1` for a bounded time instead of sending once and giving up. |
| QML resource rebuild | `Main.qml` is embedded through `qml.qrc`; changing QML on the VM is not enough. The Qt build must run `rcc -name qml` and relink `qt_camera_display`, then deploy the new binary. |
| Log interpretation | If manual process restart briefly shows the splash but reboot does not, suspect boot ordering, stale embedded QML, or rootfs/TFTP deployment drift before changing the visual design. |
| Human acceptance | `cursor_blink=0`, `initial-visible=0`, and PIDs prove the software state; the earliest LCD frame still needs human visual confirmation after a reboot. |

### 4. Validation & Error Matrix

| Check | Good Result | Bad Result | Required Action |
|---|---|---|---|
| Linux logo config | `# CONFIG_LOGO is not set` | Penguin logo still appears | Save the kernel config to `stm32mp1_atk_defconfig`, rebuild `uImage`, and deploy to TFTP |
| Fbcon cursor default source | `drivers/tty/vt/vt.c` contains `int global_cursor_default = 0;` | Source still defaults to `-1` or `1` | Change the kernel default; do not rely only on `S05display-quiet` |
| New kernel image | `uname -a` build time matches the new `uImage` deployment | Board still runs an older kernel build number/time | Copy `uImage`/DTB to TFTP and reboot the board |
| Kernel build toolchain | `make uImage LOADADDR=0xC2000040 -j8` exits `0` | `arm-none-linux-gnueabihf-gcc: Command not found` | Prepend the ARM GCC toolchain path in the SSH command |
| Runtime cursor node | `cat /sys/class/graphics/fbcon/cursor_blink` prints `0` | Prints `1` after boot | Check `vt.c` default, `S05display-quiet`, and whether the running kernel is new |
| Qt-first script order | `run_qt_kms_overlay_display.sh` starts `start_qt_shell` before `start_overlay` | Overlay starts before Qt | Reorder the script and update the static test |
| QML embedded update | Build log shows `rcc -name qml`, and board binary includes the new splash/control strings | Board still runs old splash or old overlay restore behavior | Rebuild `qt_camera_display` and redeploy the binary, not only `Main.qml` |
| Overlay initial hidden | Overlay log contains `initial-visible=0` | Camera picture appears before Qt or log shows `initial-visible=1` | Ensure script passes `-V 0` and overlay source implements hidden plane startup |
| Overlay restore | Qt log eventually shows `boot overlay visible true` or overlay visible restore success after splash | Camera never returns or appears during splash | Check QML retry timer, overlay control socket path, and `setOverlayVisible()` result |
| Full reboot behavior | After `reboot`, no `_` cursor and Qt splash appears before camera | Manual restart is correct but reboot is wrong | Treat as init/kernel/deployment ordering bug, not as a Qt-only bug |

### 5. Good/Base/Bad Cases

| Case | Example | Expected Result |
|---|---|---|
| Good polished boot | New kernel has `global_cursor_default=0`; `CONFIG_LOGO` disabled; `S05display-quiet` deployed; Qt starts before overlay; overlay starts with `-V 0` | No penguin logo, no `_` cursor, Qt splash appears first, camera appears after splash |
| Good late fallback | `S05display-quiet` writes `cursor_blink=0` | Removes any later fbcon cursor if sysfs appears after init starts |
| Good stale-QML catch | QML changed, build log shows `rcc -name qml`, and board binary is redeployed | Board uses the new splash animation and overlay restore logic |
| Base manual restart | Killing and restarting the display route briefly shows the splash | Useful symptom evidence, but not sufficient proof that reboot boot order is fixed |
| Bad rootfs-only cursor fix | Only write `cursor_blink=0` in `/etc/init.d/S05display-quiet` | The first cursor frame can still appear before init scripts run |
| Bad Qt-only fix | Add a splash overlay in QML but start `uvc_kms_overlay` first | Camera can appear before Qt on real reboot |
| Bad source-only fix | Edit `vt.c` or `Main.qml` but do not rebuild/deploy `uImage` or `qt_camera_display` | Board behavior does not change even though source looks correct |

### 6. Tests Required

- Kernel:
  - Assert `grep -n "global_cursor_default" drivers/tty/vt/vt.c` shows `int global_cursor_default = 0;`.
  - Assert `grep -n "CONFIG_LOGO" arch/arm/configs/stm32mp1_atk_defconfig` shows `# CONFIG_LOGO is not set`.
  - Assert `PATH=/usr/local/arm/gcc-arm-9.2-2019.12-x86_64-arm-none-linux-gnueabihf/bin:$PATH make uImage LOADADDR=0xC2000040 -j8` exits `0`.
  - Assert `make dtbs` exits `0`.
  - Assert TFTP `uImage` timestamp changes after `sudo cp`.
- Qt/rootfs:
  - Assert `./test_qt_kms_overlay_assets.sh` exits `0`.
  - Assert `./build_uvc_kms_overlay.sh` exits `0`.
  - Assert `./build_qt_camera_display.sh` logs `rcc -name qml` after QML changes.
  - Assert `sudo ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` deploys the new binary and init scripts.
- Board:
  - Reboot, wait for SSH, then assert `uname -a` shows the new kernel build time.
  - Assert `cat /sys/class/graphics/fbcon/cursor_blink` prints `0`.
  - Assert `/root/qt_camera_display/run_qt_kms_overlay_display.sh status` reports both Qt and overlay PIDs and no fallback PID.
  - Assert `/tmp/uvc-kms-overlay.log` contains `initial-visible=0`.
  - Assert `/tmp/qt-kms-overlay-shell.log` contains `boot overlay visible false` and later overlay restore evidence.
  - Get human confirmation from the LCD that no `_` appears and the camera does not precede the Qt splash.

### 7. Wrong vs Correct

#### Wrong

```bash
# Wrong: hides only the penguin logo, then assumes the blinking cursor is also gone.
scripts/config --disable LOGO
make uImage LOADADDR=0xC2000040 -j8
```

#### Wrong

```sh
# Wrong: camera overlay starts first, so it can win the first visible frame during real boot.
start_stack()
{
    stop_legacy_display
    start_overlay
    start_qt_shell
}
```

#### Wrong

```bash
# Wrong: QML source changed, but qml.qrc embeds it into the executable.
scp qml/Main.qml root@192.168.1.250:/root/qt_camera_display/qml/Main.qml
reboot
```

#### Correct

```c
/*
 * Correct: make the kernel default match the desired LCD product boot behavior.
 * Rootfs init scripts may still write cursor_blink=0 later, but they are not the first-frame fix.
 */
int global_cursor_default = 0;
module_param(global_cursor_default, int, S_IRUGO | S_IWUSR);
```

#### Correct

```sh
# Correct: Qt owns the first product-visible frame; camera overlay starts hidden.
start_stack()
{
    stop_legacy_display
    hide_display_console
    start_qt_shell
    start_overlay   # overlay command line includes -V 0
}
```

#### Correct

```bash
cd /home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31
PATH=/usr/local/arm/gcc-arm-9.2-2019.12-x86_64-arm-none-linux-gnueabihf/bin:$PATH \
  make uImage LOADADDR=0xC2000040 -j8
make dtbs
sudo cp arch/arm/boot/uImage arch/arm/boot/dts/stm32mp157d-atk.dtb /home/cfr/linux/tftpboot/ -f

cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display
./test_qt_kms_overlay_assets.sh
./build_uvc_kms_overlay.sh
./build_qt_camera_display.sh
sudo ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs
```

---

## Scenario: STM32MP157 4G Modem PPP Dial-Up

### 1. Scope / Trigger

- Trigger: Adding, debugging, or preserving USB 4G modem support on the STM32MP157 board.
- Trigger: Enabling kernel USB serial drivers, Buildroot `pppd`, or board-side PPP dial scripts.
- Confirmed board test: `/etc/ppp/quectel/ppp-on &` created `ppp0`, received local IP `10.144.240.122`, DNS `222.85.85.85/222.88.88.88`, and pinged `www.baidu.com` after adding the PPP default route.
- Important gotcha: `pppd` can report `not replacing existing default route via 192.168.1.1`; in that state `ppp0` is up, but traffic still follows `eth0` until the route table is changed.

### 2. Signatures

| Operation | Command Signature |
|---|---|
| Kernel tree | `cd /home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31` |
| Kernel build | `PATH=/usr/local/arm/gcc-arm-9.2-2019.12-x86_64-arm-none-linux-gnueabihf/bin:$PATH make uImage LOADADDR=0xC2000040 -j8` |
| Kernel deploy | `sudo cp arch/arm/boot/uImage arch/arm/boot/dts/stm32mp157d-atk.dtb /home/cfr/linux/tftpboot/ -f` |
| Buildroot tree | `cd /home/cfr/linux/buildroot/buildroot-2020.02.6` |
| Buildroot build | `make -j8` |
| Rootfs deploy | `cd output/images && sudo tar -axvf rootfs.tar -C /home/cfr/linux/nfs/rootfs` |
| Board dial path | `cd /etc/ppp/quectel && ./ppp-on &` |
| Managed board dial | `/usr/bin/4g-ppp {start|stop|restart|status|test|sim-status|monitor|monitor-start|monitor-stop}` |
| Board time sync | `/usr/bin/board-time-sync {once|status}` |
| Login timezone | `/etc/profile.d/board-timezone.sh` exports `TZ=CST-8` for interactive shells |
| Qt UI timezone proof | `tr '\0' '\n' < /proc/$(pidof qt_camera_display)/environ | grep '^TZ='` |
| Boot autostart | `/etc/init.d/S80ppp-4g {start|stop|restart|status|test}` |
| USB runtime power proof | `cat /sys/bus/usb/devices/2-1/power/control /sys/bus/usb/devices/2-1.7/power/control 2>/dev/null` |
| SIM monitor state | `cat /var/run/4g-ppp.state` |
| SIM status probe | `4g-ppp stop; 4g-ppp sim-status` |
| SIM long-no-card interval | `SIM_LONG_NO_CARD_INTERVAL=<seconds> 4g-ppp monitor-start` |
| USB disconnect signature | `usb 2-1.7: USB disconnect` followed by `ttyUSB0..3 disconnected/attached` |
| HTTPS proof | `curl -I https://cloud.tencent.com` |
| Board interface check | `ifconfig ppp0` |
| Board route check | `ip route show` |
| Confirmed route fix | `route add default gw <current-ppp0-local-ip>`; the successful test used `10.144.240.122` |
| Connectivity proof | `ping www.baidu.com` |

### 3. Contracts

| Area | Contract |
|---|---|
| Kernel USB serial | `CONFIG_USB_SERIAL=y`, `CONFIG_USB_SERIAL_WWAN=y`, and `CONFIG_USB_SERIAL_OPTION=y` must be saved in `arch/arm/configs/stm32mp1_atk_defconfig`; `.config` alone is not persistent. |
| Kernel PPP | `CONFIG_PPP=y`, `CONFIG_PPP_ASYNC=y`, `CONFIG_PPP_SYNC_TTY=y`, `CONFIG_PPPOE=y`, `CONFIG_PPP_DEFLATE=y`, `CONFIG_PPP_BSDCOMP=y`, `CONFIG_PPP_FILTER=y`, `CONFIG_PPP_MPPE=y`, and required crypto/checksum dependencies must be present when PPP dial-up is expected to work from the board. |
| USB modem IDs | `drivers/usb/serial/option.c` is the source of truth for USB option driver binding. Keep new VID/PID entries and interface-skip rules there, then rebuild `uImage`. |
| Quectel zero packet | `drivers/usb/serial/usb_wwan.c` must use `idProduct`, not `iProduct`, when comparing USB product IDs. `iProduct` is only a string descriptor index and will not match the modem PID correctly. |
| Buildroot PPP tools | Buildroot must include `BR2_PACKAGE_PPPD=y`; the deployed rootfs must contain `/usr/sbin/pppd`, `/usr/sbin/chat`, and `/etc/ppp/quectel`. |
| Buildroot HTTPS/COS tools | Tencent COS upload requires DNS, correct board time, HTTPS/TLS, CA roots, and an upload client. The confirmed rootfs enables `BR2_PACKAGE_LIBCURL=y`, `BR2_PACKAGE_LIBCURL_CURL=y`, `BR2_PACKAGE_LIBCURL_OPENSSL=y`, `BR2_PACKAGE_OPENSSL=y`, and `BR2_PACKAGE_CA_CERTIFICATES=y`. |
| Rootfs overlay persistence | 4G PPP scripts and board time sync scripts must live under Buildroot overlay `board/stm32mp1_atk/rootfs_overlay`, with `BR2_ROOTFS_OVERLAY="board/stm32mp1_atk/rootfs_overlay"` saved in `configs/stm32mp1_atk_defconfig`; copying scripts only into the current NFS rootfs is not enough for future eMMC/SD burning. |
| Dial device | The confirmed dial script uses `/dev/ttyUSB2` from `quectel_options`; do not change it unless `dmesg` or modem documentation proves another AT command port. |
| APN | The confirmed CTNET dialer sends `AT+CGDCONT=1,"IP","CTNET"` and `ATD*99#`. Preserve this for the tested China Telecom SIM path. |
| Dialer initialization | Current EC20F chat dialers must not send `OK ATP`; this module returns `ERROR` for `ATP`, causing `Connect script failed` before APN setup. The expected sequence is `ATE`, `ATH`, `AT+CGDCONT=1,"IP","CTNET"`, `ATD*99#`, then `CONNECT`. |
| SIM hotplug hardware boundary | On the current 4G module schematic, U22 exposes `USIM_PRESENT`, but it is not connected to the Nano SIM socket detect switch or to an MP157 GPIO. Software therefore cannot promise instant card-insert detection while doing zero polling. Default scripts must treat `SIM_PRESENT_WIRED=0` as the board contract. |
| SIM adaptive recovery | The board-side `4g-ppp` script must support `sim-status`, `monitor`, `monitor-start`, and `monitor-stop`. `4g-ppp start` must keep the old one-shot dial behavior and then start the background monitor, so existing `/etc/init.d/S80ppp-4g start` gains SIM hotplug recovery without changing its command shape. |
| SIM polling budget | When no SIM is detected, the monitor must use a short recent-no-card interval first, then fall back to `SIM_LONG_NO_CARD_INTERVAL` for long no-card periods. The current default recent-no-card window and long interval are both `8` seconds. During no-card waits, prefer passive UART URC listening and only run active `AT+CPIN?` as the low-frequency fallback. |
| SIM AT port detection | Keep `/dev/ttyUSB2` as the confirmed PPP dial device from `quectel_options`, but do not hard-bind SIM status probing to that single port. `4g-ppp sim-status` and the monitor must support `AT_TTY_CANDIDATES` and record `at_tty=` in `/var/run/4g-ppp.state`, because after SIM/USB hotplug the responsive AT management port can differ from the PPP data port or recover later. |
| SIM CME 13 recovery | Treat `AT+CPIN? -> +CME ERROR: 13` as `sim_error`, not `unknown`. On the current board this was the observed raw response after SIM removal/reinsertion even though `/dev/ttyUSB2` and `/dev/ttyUSB3` responded to `AT/OK`. The monitor must first use a short `SIM_ERROR_FAST_WAIT_SECONDS` lightweight `AT+CPIN?` READY confirmation window so naturally recovered inserts can reconnect without a full RF/SIM reset. Only if READY does not appear should it attempt `AT+CFUN=0/1` under `SIM_CFUN_ON_SIM_ERROR=1`. `sim_error` uses the short `SIM_CFUN_SIM_ERROR_COOLDOWN_SECONDS` cooldown, while long no-card recovery still uses `SIM_CFUN_COOLDOWN_SECONDS`. After PPP returns online, the script must clear the SIM recovery stamps so a second manual hotplug test is not blocked by the previous CFUN cooldown. |
| SIM hotplug debug evidence | For every SIM hotplug recovery bug, collect an absolute-time sequence from `/var/log/4g-ppp.log` before changing code: `LCP TermReq`/`Modem hangup`, `pppd stopped`, `sim_error` or `no_sim`, fast READY wait start/timeout, CFUN attempt or cooldown skip, `AT+CPIN? -> READY`, `开始 4G PPP 拨号`, and `ppp0 已上线`. Also capture `/var/run/4g-ppp.state`, `ls -l /var/run/4g-ppp*.last`, `ifconfig ppp0`, and `route -n`. Do not diagnose from `ppp0 还没有本地 IP，不能添加默认路由` alone; that line is a consequence while PPP/SIM is still offline. |
| Online SIM/link health check | Do not wait several minutes for PPP/LCP to notice SIM removal. While `ppp0` is online, the monitor should every `SIM_ONLINE_SIM_CHECK_INTERVAL` default `30` seconds probe a non-`PPP_TTY` auxiliary AT port when available; if it sees `no_sim` or `sim_error`, stop `pppd` and write the offline state. If no auxiliary AT port exists, it must still run a bounded `ping -I ppp0` link check and write `link_offline` on failure rather than waiting for peer LCP termination. Never fall back to plain ping, because eth0/NFS can make that succeed while 4G is offline. |
| SIM insert fast path | When the monitor sees a SIM insert/READY URC or a low-frequency poll finds `AT+CPIN? -> READY`, it may temporarily query every `SIM_FAST_RETRY_SECONDS` second for up to `SIM_FAST_WAIT_SECONDS` seconds, then restart PPP after SIM is ready. This short high-frequency window is allowed because it only happens after an insert candidate, not during long no-card idle. |
| SIM and antenna prerequisite | The EC20/Quectel modem must have a valid SIM, stable card-slot contact, sufficient module power, and a connected antenna before PPP diagnosis. Missing antenna or registration failure can still allow AT commands, `CONNECT`, LCP, and CHAP to succeed, then fail at IPCP with `Modem hangup`; in that signature, verify SIM, antenna, signal, and registration before changing APN or PPP scripts. |
| USB runtime power | The board-side `4g-ppp` script must set the EC20/Quectel USB device and its parent hub `power/control` files to `on` before start/status/test and while waiting for `/dev/ttyUSB2`. The confirmed live paths are `/sys/bus/usb/devices/2-1/power/control` and `/sys/bus/usb/devices/2-1.7/power/control`; allow `USB_POWER_CONTROLS` override for boards where the USB topology changes. |
| USB disconnect diagnosis | If logs show `usb 2-1.7: USB disconnect` and all `ttyUSB*` nodes detach/reattach, treat UI network offline as a true modem transport loss. First disable runtime suspend and check power/hub/cable/module contact; do not start by changing APN, DNS, cloud health, or Qt display logic. |
| Route priority | If `pppd` does not replace the old `eth0` default route, add a PPP default route before declaring networking broken. Use the current `ppp0` local IP shown by `ifconfig ppp0`; preserve the LAN/NFS route when the board still depends on `eth0`. |
| NFS vs burned rootfs route policy | In NFS boot, keep `eth0` available for LAN/NFS and add the 4G default route after `ppp0` appears. In eMMC/SD boot, if `eth0` is not needed and `pppd defaultroute` already created a `ppp0` default route, no extra route override is required. |
| Boot-time clock discipline | After `4g-ppp start` confirms `ppp0` has an IP and the PPP default route is available, call `/usr/bin/board-time-sync once` once. The time sync script must set `/etc/TZ` to `CST-8`, prefer `ntpd -n -q -p`, fall back to BusyBox `rdate`, then fall back to an HTTP `Date` header, and write the corrected system time back to RTC with `hwclock -w -u`. Time sync failure should be logged but must not turn a successful PPP dial into a failed dial. Scripts that need Beijing business time must explicitly export `TZ=CST-8`; interactive shells must get the same export from `/etc/profile.d/board-timezone.sh` because a BusyBox login shell may otherwise print UTC with bare `date`. Qt UI processes are their own environment boundary: `qt_camera_display` must inherit or set `TZ=CST-8` before `QGuiApplication`, otherwise QML `new Date()` can still show UTC even when upload timestamps are correct. |

### 4. Validation & Error Matrix

| Check | Good Result | Bad Result | Required Action |
|---|---|---|---|
| USB enumeration | `dmesg` shows the modem and `/dev/ttyUSB*` nodes appear | No `ttyUSB*` nodes | Recheck kernel `USB_SERIAL_OPTION`, VID/PID entries, USB host power, and cable |
| USB runtime power | After `4g-ppp status` or `4g-ppp test`, existing modem/hub `power/control` files show `on` | Files remain `auto`, or `usb 2-1.7: USB disconnect` keeps increasing quickly | Fix `4g-ppp` runtime power setup; if already `on`, inspect 4G module current, USB hub, cable, and connector |
| PPP tools | `which pppd` and `which chat` both succeed | Command missing | Rebuild Buildroot with `BR2_PACKAGE_PPPD=y` and redeploy `rootfs.tar` |
| SIM hotplug monitor | `4g-ppp monitor-start; cat /var/run/4g-ppp.state` shows monitor state, and long no-card logs use `SIM_LONG_NO_CARD_INTERVAL` | Monitor is not running after `4g-ppp start`, or long no-card mode still checks every second | Fix `4g-ppp` monitor startup and interval handling before changing APN, ttyUSB, or Qt network status |
| SIM recognition | `4g-ppp stop; 4g-ppp sim-status` eventually shows `at_tty=...`, `ready`, and/or `+CPIN: READY` after card insertion | No `at_tty=`, `AT+CPIN? -> +CME ERROR: 10`, `AT+CPIN? -> +CME ERROR: 13`, `no_sim`, `sim_error`, or registration stays at `0,2` after reinsertion | Let the monitor run its adaptive recovery; then check `AT_TTY_CANDIDATES`, USB enumeration, card orientation/contact, SIM validity, power, and optional module reset. Do not keep restarting PPP while SIM is not READY |
| Online SIM removal display | Pulling SIM while PPP is online makes the monitor write `no_sim`/`sim_error` or `link_offline` and stop `pppd` within about `SIM_ONLINE_SIM_CHECK_INTERVAL` | UI remains online until peer LCP terminates minutes later | Fix monitor online SIM/link health detection before changing Qt network display logic |
| Dial script | `./ppp-on &` reaches `CONNECT`, `CHAP authentication succeeded`, and `Using interface ppp0` | Chat fails at `ATP` with `ERROR` before `AT+CGDCONT` | Remove `OK ATP` from `quectel_ppp_dialer` in both the live rootfs and Buildroot overlay, then retry |
| Antenna/signal prerequisite | After `CONNECT` and `CHAP authentication succeeded`, IPCP assigns a local IP and DNS | `sent [IPCP ConfReq ...]` is immediately followed by `Modem hangup`; `ppp0` disappears before getting an IP | Check that the EC20/Quectel SIM was inserted before power-on and the antenna is physically connected, then verify `AT+CSQ`, `AT+CPIN?`, `AT+CGREG?`/`AT+CEREG?`, `AT+CGATT?`, and `AT+CEER`; do not rewrite the PPP scripts for this signature until hardware signal is confirmed |
| PPP interface | `ifconfig ppp0` shows `UP POINTOPOINT RUNNING` and an `inet addr` | No `ppp0` or no IP address | Inspect `pppd` output, kernel PPP symbols, and serial port choice |
| Route table | `ip route show` has a default route through `ppp0` before or above the `eth0` default | Only `default via 192.168.1.1 dev eth0` remains | Add the confirmed PPP default route or adjust route metrics |
| Managed script | `4g-ppp start` creates `ppp0`, then `4g-ppp status` shows the interface and route table | Script only exists in NFS rootfs but not `rootfs.tar` | Move scripts into the Buildroot overlay and rebuild rootfs |
| DNS/IP proof | `ping www.baidu.com` resolves and receives replies | IP works but hostname fails | Check `usepeerdns`, `/etc/resolv.conf`, and peer DNS addresses |
| HTTPS/COS prerequisite | `curl --version` lists `OpenSSL` and `https`; `curl -I https://cloud.tencent.com` returns HTTP status such as `HTTP/1.1 200 OK` | BusyBox `wget` says `not an http or ftp url` for HTTPS, or `curl: not found`, or certificate verification fails | Enable/deploy `libcurl`, `curl`, `OpenSSL`, and `ca-certificates`; verify `date` before COS signing |
| Board time | `board-time-sync status` shows `/etc/TZ=CST-8`, Beijing local time, UTC `date -u`, readable `hwclock -r`, and recent sync logs; `TZ=CST-8 date` shows the same Beijing hour; a new login shell has `TZ=CST-8` | Time is 1970, far from current time, business scripts run bare `date` and get UTC, or an interactive shell has empty `TZ` | Run `board-time-sync once` after PPP is online; inspect `/var/log/board-time-sync.log`, route, DNS, curl, rdate, HTTP Date sources, whether the calling script exports `TZ=CST-8`, and whether `/etc/profile.d/board-timezone.sh` is installed before changing COS upload code |
| Interactive `date` display | `echo "$TZ"` prints `CST-8`, bare `date` prints `CST`, and `date -u` prints UTC eight hours behind Beijing time | `board-time-sync status` is correct but current shell `TZ` is empty and bare `date` prints UTC | This is a login environment issue, not a clock-sync issue. Source `. /etc/profile.d/board-timezone.sh` in the current shell, then reconnect or verify `/etc/profile` loads `/etc/profile.d/*.sh` |
| Qt visible clock | `/proc/$(pidof qt_camera_display)/environ` contains `TZ=CST-8`, and the LCD top bar matches `TZ=CST-8 date` | `board-time-sync status` and COS JSON are correct, but Qt top bar remains eight hours behind | This is a Qt process environment issue. Redeploy `qt_camera_display`, `run_qt_camera_display.sh`, and `run_qt_kms_overlay_display.sh`; do not change COS upload or server timestamp code |
| Kernel build | `make uImage LOADADDR=0xC2000040 -j8` exits `0` | `arm-none-linux-gnueabihf-gcc: Command not found` | Add the GCC toolchain path explicitly in non-interactive SSH commands |

### 5. Good/Base/Bad Cases

| Case | Example | Expected Result |
|---|---|---|
| Good modem bring-up | `cd /etc/ppp/quectel && ./ppp-on &` followed by `ifconfig ppp0` | PPP negotiates CHAP/IPCP and exposes `ppp0` with a carrier IP. |
| Good EC20F dialer | `nl -ba /etc/ppp/quectel/quectel_ppp_dialer` shows no `OK ATP` line | The dialer proceeds from `ATH` to `AT+CGDCONT=1,"IP","CTNET"` and then `ATD*99#`. |
| Good managed bring-up | `4g-ppp restart && 4g-ppp test` | The script starts pppd, starts the monitor, waits for `ppp0`, adds the current PPP default route if needed, and verifies connectivity. |
| Good SIM monitor | `4g-ppp monitor-start; cat /var/run/4g-ppp.state` | Background monitor records state and uses low-frequency long-no-card polling when no SIM is present. |
| Good USB power guard | `4g-ppp status; cat /sys/bus/usb/devices/2-1/power/control /sys/bus/usb/devices/2-1.7/power/control` | Existing files print `on`; 30-second observation does not add new `usb 2-1.7: USB disconnect` entries. |
| Good boot-time sync | `4g-ppp restart; board-time-sync status; TZ=CST-8 date; date -u; hwclock -r` | PPP becomes online, `/etc/TZ` is `CST-8`, explicit-TZ `date` shows Beijing time, `date -u` shows UTC, and RTC can be read after `hwclock -w -u`. |
| Good interactive display | `. /etc/profile.d/board-timezone.sh; echo "$TZ"; date; date -u` | Current shell gets `TZ=CST-8`, bare `date` shows CST Beijing time, and `date -u` remains UTC. |
| Good Qt display | `qtpid=$(pidof qt_camera_display); tr '\0' '\n' < /proc/$qtpid/environ | grep '^TZ='; TZ=CST-8 date` | The running Qt process has `TZ=CST-8`, so QML `new Date()` shows Beijing local time without QML hour arithmetic. |
| Good COS prerequisite | `curl --version` shows `libcurl/7.72.0 OpenSSL/1.1.1g` and `curl -I https://cloud.tencent.com` returns `HTTP/1.1 200 OK` | Board can reach Tencent Cloud over HTTPS and has CA roots for future COS upload. |
| Good route recovery | `route add default gw <current-ppp0-local-ip>` after PPP reports it did not replace the `eth0` default route | `ip route show` lists the PPP default route first, and `ping www.baidu.com` succeeds. |
| Base LAN preservation | Keep `eth0` up while adding the PPP default route | NFS/SSH on the local LAN can remain reachable while internet traffic uses 4G. |
| Bad diagnosis | `ppp0` is up, but ping fails, so assume PPP negotiation failed | The real issue can be the unchanged default route via `192.168.1.1`. |
| Bad antenna diagnosis | `ATD*99#` reaches `CONNECT` and CHAP succeeds, but IPCP ends with `Modem hangup`, so assume `ppp-on`, `quectel_options`, or `4g-ppp` is broken | This exact case was caused by the EC20/Quectel antenna not being inserted; confirm antenna/signal/registration before editing scripts. |
| Bad no-card polling | Leave the board with no SIM and check `AT+CPIN?` every second forever | This steals AT-port time and adds useless load; long no-card mode must fall back to `SIM_LONG_NO_CARD_INTERVAL`. |
| Bad instant-detect promise | Promise immediate card-insert detection while `USIM_PRESENT` is not wired to the Nano SIM socket | Without a hardware presence line, worst-case discovery time is the long-no-card polling interval unless the modem emits a usable URC. |
| Bad SIM diagnosis | `AT+CPIN?` returns `+CME ERROR: 10` or `+CME ERROR: 13`, so assume PPP scripts or APN are wrong | First use `4g-ppp monitor-start` and `4g-ppp sim-status`; then inspect card orientation/contact, SIM validity, module power, and reset options before changing APN or PPP scripts. |
| Bad USB disconnect diagnosis | `usb 2-1.7: USB disconnect` repeats, so change cloud health or force Qt to show online | The modem transport is genuinely disappearing. Keep USB runtime power on, then check hardware supply/hub/cable before UI or APN changes. |
| Bad ATP diagnosis | Chat exits at `ATP ERROR`, so change APN or `/dev/ttyUSB2` | Current EC20F does not accept `ATP`; delete `OK ATP` and preserve the confirmed `CTNET` APN and `/dev/ttyUSB2`. |
| Bad COS diagnosis | `ping cloud.tencent.com` works, but HTTPS upload fails, so assume 4G is broken | ICMP/DNS can be fine while curl, CA certificates, time, or COS signing is missing. |
| Bad clock workaround | Upload time is wrong, so add or subtract eight hours inside `defect-cos-upload` | Fix system UTC and `/etc/TZ` through `board-time-sync`; business upload code should export `TZ=CST-8`, read Beijing wall clock, and append `+08:00`, not compensate for a broken system clock. |
| Bad login-time diagnosis | After boot, run bare `date`, see `UTC`, and conclude the network time sync failed | First compare `board-time-sync status`, `date -u`, `TZ=CST-8 date`, and `echo "$TZ"`. If only the current shell has empty `TZ`, install/source `/etc/profile.d/board-timezone.sh` instead of changing sync or upload code. |
| Bad Qt-time diagnosis | Upload detail time is correct, but Qt top bar is UTC, so edit backend/API/frontend cloud formatting | Qt top-bar time is local to the Qt process. Check `/proc/$(pidof qt_camera_display)/environ`, then fix the Qt entry/start scripts. |
| Bad USB PID comparison | Compare `desc->iProduct` to `0x9003` or `0x9215` in `usb_wwan.c` | The comparison checks a string index, not the USB product ID; use `desc->idProduct`. |
| Bad config persistence | Enable USB serial or PPP only in `.config` | Future clean kernel builds can lose 4G modem support. |
| Bad script persistence | Copy `/usr/bin/4g-ppp` directly into the live NFS rootfs only | The script works now but is absent from a freshly generated/burned `rootfs.tar`. |

### 6. Tests Required

- Kernel:
  - Assert `arch/arm/configs/stm32mp1_atk_defconfig` contains the required `CONFIG_USB_SERIAL*` and `CONFIG_PPP*` symbols.
  - Assert `git diff --check` passes after editing `option.c` or `usb_wwan.c`.
  - Assert `make uImage LOADADDR=0xC2000040 -j8` succeeds with the ARM GCC toolchain in `PATH`.
- Buildroot/rootfs:
  - Assert `configs/stm32mp1_atk_defconfig` contains `BR2_PACKAGE_PPPD=y`.
  - Assert `configs/stm32mp1_atk_defconfig` contains `BR2_PACKAGE_LIBCURL=y`, `BR2_PACKAGE_LIBCURL_CURL=y`, `BR2_PACKAGE_LIBCURL_OPENSSL=y`, `BR2_PACKAGE_OPENSSL=y`, and `BR2_PACKAGE_CA_CERTIFICATES=y` before building COS upload support.
  - Assert `configs/stm32mp1_atk_defconfig` contains `BR2_ROOTFS_OVERLAY="board/stm32mp1_atk/rootfs_overlay"` when 4G PPP scripts must survive a rootfs rebuild or eMMC/SD burning.
  - Assert `make -j8` exits `0` as ordinary user `cfr`; do not use `sudo make`.
  - Assert deployed NFS rootfs contains `/usr/sbin/pppd`, `/usr/sbin/chat`, `/etc/ppp/quectel`, `/usr/bin/curl`, `/usr/lib/libcurl.so.4`, and `/etc/ssl/certs/ca-certificates.crt`.
  - Assert generated `output/images/rootfs.tar` contains `./usr/bin/4g-ppp`, `./usr/bin/board-time-sync`, `./etc/init.d/S80ppp-4g`, and `./etc/ppp/quectel/*`.
- Board:
  - Assert `/dev/ttyUSB2` exists before dialing.
  - Assert `4g-ppp start` starts the background monitor path used by `/etc/init.d/S80ppp-4g start`; `cat /var/run/4g-ppp.state` must show state, detail, `ppp_tty=/dev/ttyUSB2`, and `at_tty=...` when an AT port is responsive.
  - Assert `4g-ppp stop; 4g-ppp sim-status` can probe `AT_TTY_CANDIDATES` and query `AT+CPIN?` when pppd is not occupying the modem.
  - Assert `AT+CPIN? -> +CME ERROR: 13` is classified as `sim_error` and does not remain `unknown`.
  - Assert online SIM removal can switch offline in about 30 seconds via auxiliary AT-port SIM status or `ping -I ppp0` link failure.
  - Assert long no-card behavior uses `SIM_LONG_NO_CARD_INTERVAL` rather than one-second polling, because current hardware leaves `USIM_PRESENT` unwired.
  - Assert after reinserting SIM, monitor either wakes from a UART URC or detects `AT+CPIN? -> READY` on the next low-frequency poll, then enters the short fast-confirm window before restarting PPP.
  - Assert the EC20/Quectel antenna is physically connected before treating IPCP `Modem hangup` as a software failure.
  - Assert `4g-ppp status` or `4g-ppp test` writes existing USB runtime power controls to `on`: `/sys/bus/usb/devices/2-1/power/control` and `/sys/bus/usb/devices/2-1.7/power/control` on the current board.
  - If `usb 2-1.7: USB disconnect` was observed, count disconnect logs before and after a 30-second observation with `power/control=on`; if disconnects continue, document it as a hardware power/hub/cable issue rather than a PPP route issue.
  - Assert `/etc/ppp/quectel/quectel_ppp_dialer` does not contain `OK ATP` for the current EC20F path.
  - Prefer `4g-ppp start`; if testing the lower-level path, run `cd /etc/ppp/quectel && ./ppp-on &` and preserve the visible `pppd` log until IPCP completes.
  - Assert `ifconfig ppp0` shows `UP POINTOPOINT RUNNING` with a local IP.
  - Run `ip route show`; if the old `eth0` default remains first, add the PPP default route.
  - Assert `ping www.baidu.com` receives replies.
  - Assert `cat /etc/resolv.conf` contains operator DNS or another valid nameserver.
  - Assert `date` is correct enough for HTTPS/COS signing.
  - Assert `board-time-sync once` succeeds when PPP internet is available, then `board-time-sync status` shows `/etc/TZ=CST-8`, a Beijing local time, UTC time, RTC output, and `/var/log/board-time-sync.log`.
  - Assert `. /etc/profile.d/board-timezone.sh; echo "$TZ"; date; date -u` proves interactive shells can display Beijing local time without changing UTC.
  - Assert `curl --version` reports `OpenSSL` and `https`, then `curl -I https://cloud.tencent.com` returns an HTTP status.

### 7. Wrong vs Correct

#### Wrong

```bash
# Wrong: ppp0 is up, but the default route still points to eth0.
ifconfig ppp0
ping www.baidu.com
echo "4G failed"
```

#### Correct

```bash
cd /etc/ppp/quectel
./ppp-on &

ifconfig ppp0
ip route show

# Confirmed board-side fix from the successful test.
# Replace 10.144.240.122 with the current ppp0 local IP if IPCP assigns a different value.
route add default gw 10.144.240.122

ip route show
ping www.baidu.com
```

#### Correct

```bash
# Correct: use the managed script for normal boot/manual use.
4g-ppp restart
4g-ppp status
4g-ppp test
curl -I https://cloud.tencent.com
```

#### Wrong

```c
/* Wrong: iProduct is a string descriptor index, not the USB product ID. */
if (desc->idVendor == cpu_to_le16(0x05c6) &&
	desc->iProduct == cpu_to_le16(0x9003))
	urb->transfer_flags |= URB_ZERO_PACKET;
```

#### Correct

```c
/* Correct: idProduct is the real USB product ID used for modem matching. */
if (desc->idVendor == cpu_to_le16(0x05c6) &&
	desc->idProduct == cpu_to_le16(0x9003))
	urb->transfer_flags |= URB_ZERO_PACKET;
```

---

## Scenario: STM32MP157 Detection Image Upload To Cloud COS

### 1. Scope / Trigger

- Trigger: STM32MP157 board needs to upload SD-card detection images and structured detection records to the cloud project at `D:\yunfuwu`.
- Trigger: Any work that touches EC20 PPP networking, board-side `curl` upload flow, cloud detection-record creation, COS pre-signed upload, or front-end image preview behavior.
- Confirmed manual test date: `2026-05-06`.
- Confirmed board-side success path: `PPM file on SD card -> POST /api/v1/records -> POST /api/v1/uploads/cos/prepare -> curl PUT to COS -> POST /api/v1/records/{record_id}/files -> GET /api/v1/records/{record_id}`.
- Confirmed cloud target: `http://139.9.35.72`.

### 2. Signatures

| Operation | Command / API Signature |
|---|---|
| Board login to cloud | `POST /api/v1/auth/login` |
| Query cloud device id | `GET /api/v1/devices?limit=100` |
| Query cloud part id | `GET /api/v1/parts?limit=100` |
| Create detection record | `POST /api/v1/records` |
| Prepare COS upload | `POST /api/v1/uploads/cos/prepare` |
| Upload object to COS | `curl -X PUT "$UPLOAD_URL" -H "Content-Type: <type>" -H "Authorization: $AUTH_HEADER" -T "$FILE"` |
| Register uploaded file object | `POST /api/v1/records/{record_id}/files` |
| Verify record detail | `GET /api/v1/records/{record_id}` |
| Current board-side source image example | `/mnt/sdcard/images/uvc_20260506_142528_000268.ppm` |

### 3. Contracts

| Area | Contract |
|---|---|
| Authentication | Business APIs require a logged-in account with a valid company context. Unauthenticated access returns `401 missing_token`. |
| Device identity | Detection uploads must reference an existing cloud MP157 `device_id`. Do not create or upload records under STM32F4 as a standalone cloud device. |
| Part identity | Detection uploads must reference an existing cloud `part_id`. |
| Record-first flow | A board image upload must always create the detection record first, obtain `record_id`, then request COS upload for each file. |
| COS prepare response | `enabled=true` is the required gate before any real object upload. If `enabled=false`, stop and treat it as a server-side COS configuration/signing problem. |
| Header consistency | `content_type` sent to `/api/v1/uploads/cos/prepare` must exactly match the later `curl PUT` `Content-Type` header. |
| File object registration | After COS `PUT` succeeds, the board must register `bucket_name`, `region`, `object_key`, `content_type`, `size_bytes`, and `etag` through `/api/v1/records/{record_id}/files`. |
| Preview semantics | `preview_url` in the detection-record detail comes from backend COS access URL generation. A successful upload is not the same as a browser-previewable image. |
| Previewable production formats | Final production uploads for record-detail preview must use browser-displayable formats such as `image/jpeg` or `image/png`. |
| Temporary debug format | `image/x-portable-pixmap` / `.ppm` is acceptable only as a temporary transport-format validation case. It must not be treated as the final UI-facing image format. |
| Board business timestamps | `captured_at`, `detected_at`, and file `uploaded_at` must be generated from the board's Beijing wall clock plus an explicit `+08:00` offset. After `board-time-sync` has corrected system UTC, scripts should explicitly export `TZ=CST-8` before calling `date`; before clock sync is fixed, do not use `TZ=CST-8 date` as a band-aid on a board whose numeric wall-clock value is already Beijing time but whose zone label prints `UTC`, because that double-converts `13:xx` into `21:xx`. Always compare `board-time-sync status`, `TZ=CST-8 date`, `date -u`, server nginx access time, raw request JSON, and record detail before deciding which layer is wrong. |

### 4. Validation & Error Matrix

| Check | Good Result | Bad Result | Required Action |
|---|---|---|---|
| Login | `/api/v1/auth/login` returns session JSON and cookie file is written | `401`, invalid credentials, or no cookie file | Fix account/password or company-context issue before all other steps |
| Record creation | `/api/v1/records` returns `id`, `record_no`, and empty `files` list | Validation error, missing `part_id`, missing `device_id`, or duplicate `record_no` | Fix payload fields before touching COS |
| COS prepare | `/api/v1/uploads/cos/prepare` returns `enabled=true`, `upload_url`, `bucket_name`, `region`, `object_key` | `enabled=false`, 4xx payload validation, or signing failure | Stop and fix server-side COS configuration or request payload |
| Object upload | COS `PUT` returns `HTTP_CODE=200` and `ETag` header | `403` signature/header mismatch, `5xx`, timeout, or no `ETag` | Refresh pre-signed URL or fix `Content-Type`/request headers |
| File registration | `/api/v1/records/{record_id}/files` returns file object JSON with `preview_url` | 4xx payload validation, missing object metadata, or auth failure | Fix registration payload using actual COS response metadata |
| Round-trip detail | `/api/v1/records/{record_id}` shows a non-empty `files` array and updated `uploaded_at` | `files=[]` or stale `uploaded_at` | Re-run file registration; do not assume COS `PUT` alone finished the workflow |
| Timestamp round-trip | `/api/v1/records/{record_id}` returns `captured_at/uploaded_at` in the same hour as `TZ=CST-8 date` and nginx access time converted to Beijing time | Business time is exactly +8 hours from Beijing wall clock, such as Beijing `13:xx` but record `21:xx`; or exactly -8 hours, such as record `05:xx` | Inspect board request JSON and `board-time-sync status` before changing frontend/backend; remove double conversion or missing `TZ=CST-8` export in the board script. Do not "fix" this by adding or subtracting 8 hours in the frontend or backend response layer |
| Preview behavior | Browser can render `preview_url` for `jpg/png` | `preview_url` exists but browser shows a broken image for `ppm` | Treat this as a format/display compatibility issue, not an upload failure |

### 5. Good/Base/Bad Cases

| Case | Example | Expected Result |
|---|---|---|
| Good production upload | `source.jpg` with `content_type=image/jpeg` | COS upload succeeds and front-end detail page can preview the image |
| Good temporary validation | `uvc_20260506_142528_000268.ppm` with `content_type=image/x-portable-pixmap` | COS upload, file registration, and record round-trip succeed; preview compatibility remains format-dependent |
| Base metadata-only test | Create record and prepare COS upload without doing the actual `PUT` | Verifies account, company context, ids, and server-side COS signing path |
| Good timestamp upload | `TZ=CST-8 date` shows `13:12`, record detail shows `captured_at=2026-05-08T13:12:51` and file `uploaded_at=2026-05-08T13:13:xx` | Business times match the board Beijing wall clock and the server access log hour converted to Beijing time |
| Bad placeholder payload | Send `"record_id": RECORD_ID` in JSON | FastAPI returns JSON decode/validation failure because the placeholder is not a JSON number |
| Bad header mismatch | Prepare with `image/jpeg` but upload with `image/x-portable-pixmap` | COS can reject with signature/header mismatch |
| Bad final-format assumption | Treat `.ppm` as the permanent UI image format because upload succeeded once | Cloud storage works, but browser preview can still fail on the detail page |
| Bad timestamp conversion | Before network clock sync, bare `date` numerically shows `13:07 UTC`, script runs `TZ=CST-8 date`, and uploads `captured_at=21:07+08:00` | The displayed record is 8 hours ahead even though server and board requests happened around 13:xx. The bug is board timestamp generation, not frontend display |

### 6. Tests Required

- Cloud API:
  - Assert `GET /health` returns `{"status":"ok"}` before starting the upload workflow.
  - Assert `/api/v1/auth/login` succeeds and writes a cookie file.
  - Assert `/api/v1/devices?limit=100` returns the target MP157 device id.
  - Assert `/api/v1/parts?limit=100` returns the target part id.
- Board upload chain:
  - Assert `/api/v1/records` returns `record_id`.
  - Assert `/api/v1/uploads/cos/prepare` returns `enabled=true`.
  - Assert COS `PUT` returns `HTTP_CODE=200`.
  - Assert `/tmp/source_upload_headers.txt` contains an `ETag:` header.
  - Assert `/api/v1/records/{record_id}/files` returns a non-empty `preview_url`.
  - Assert `/api/v1/records/{record_id}` shows `files[0].object_key` matching the prepared object key.
  - Assert `/api/v1/records/{record_id}` shows `captured_at/uploaded_at` matching the board wall-clock hour, not a second time-zone conversion.
- Preview-format acceptance:
  - Assert temporary `ppm` upload is treated only as transport-chain proof.
  - Assert final UI acceptance uses `jpg` or `png`, and a human verifies the cloud detail page can display the uploaded image.

### 7. Wrong vs Correct

#### Wrong

```bash
# Wrong: board wall clock already prints the intended local time, so this adds 8 hours.
TZ=CST-8 date '+%Y-%m-%dT%H:%M:%S%z'
```

#### Correct

```bash
# Correct: keep the board wall-clock value and only attach the intended business offset.
printf '%s+08:00\n' "$(date '+%Y-%m-%dT%H:%M:%S')"
```

#### Wrong

```bash
# Wrong: placeholder is copied literally, so JSON is invalid for the actual API call.
curl -b /tmp/cloud_cookie.txt \
  -H "Content-Type: application/json" \
  -X POST "http://139.9.35.72/api/v1/uploads/cos/prepare" \
  -d '{
    "record_id": RECORD_ID,
    "file_kind": "source",
    "file_name": "source.jpg",
    "content_type": "image/jpeg"
  }'
```

#### Wrong

```bash
# Wrong: assume COS upload success means the front-end can preview every file format.
curl -X PUT "$UPLOAD_URL" -H "Content-Type: image/x-portable-pixmap" -T "$FILE"
echo "upload done, preview done"
```

#### Correct

```bash
# Correct: use the real record id returned by /api/v1/records.
curl -b /tmp/cloud_cookie.txt \
  -H "Content-Type: application/json" \
  -X POST "http://139.9.35.72/api/v1/uploads/cos/prepare" \
  -d '{
    "record_id": 4,
    "file_kind": "source",
    "file_name": "uvc_20260506_142528_000268.ppm",
    "content_type": "image/x-portable-pixmap"
  }'
```

#### Correct

```bash
# Correct: transport validation may use ppm, but final previewable uploads should be jpg/png.
curl -X PUT "$UPLOAD_URL" \
  -H "Content-Type: image/jpeg" \
  -H "Authorization: $AUTH_HEADER" \
  -T "/mnt/sdcard/images/source.jpg"

curl -b /tmp/cloud_cookie.txt \
  -H "Content-Type: application/json" \
  -X POST "http://139.9.35.72/api/v1/records/$RECORD_ID/files" \
  -d "{
    \"file_kind\": \"source\",
    \"storage_provider\": \"cos\",
    \"bucket_name\": \"$BUCKET_NAME\",
    \"region\": \"$REGION\",
    \"object_key\": \"$OBJECT_KEY\",
    \"content_type\": \"image/jpeg\",
    \"size_bytes\": $SIZE_BYTES,
    \"etag\": $ETAG_JSON
  }"
```

---

## Scenario: STM32MP157 Dual-Model Detection History And COS Upload

### 1. Scope / Trigger

- Trigger: Any change to `20_uvc_camera/qt_camera_display` that modifies the Detect button, MobileNetV3-Small classification, UNet segmentation, overlay capture, COS upload, or local detection history.
- Trigger: Any change that replaces manual "save image" behavior with model-produced detection evidence.
- Goal: One human Detect action must produce one coherent history record and one cloud record containing both model results and all model evidence images.
- Board runtime directory: `/root/qt_camera_display`.
- Board image directory: `/mnt/sdcard/images`.
- Confirmed model flow date: `2026-05-18`.

### 2. Signatures

| Operation | Command / API Signature |
|---|---|
| Overlay source capture | `SAVE_DETECT <image-dir>` over `/tmp/uvc-kms-overlay-control.sock` |
| Classification command | `/root/qt_camera_display/defect-classify --image <source.jpg> --model <classifier.onnx> --labels <labels.json>` |
| Segmentation command | `/root/qt_camera_display/defect-segment --image <source.jpg> --model <unet.onnx> --output-dir /mnt/sdcard/images` |
| Segment result line | `RESULT_SEG status=<OK|NG> defect_pixels=<n> ... raw_path=<jpg> overlay_path=<jpg> mask_path=<png>` |
| COS upload command | `/root/qt_camera_display/defect-cos-upload --jpg <source.jpg> --annotated <raw.jpg> --annotated <overlay.jpg> --annotated <mask.png>` |
| SSH self-test | `cd /root/qt_camera_display && ./qt_camera_display --detect-self-test` |
| Local history file | `/mnt/sdcard/images/upload_history.json` |
| Board deployment input | `DEFECT_UNET_MODEL_SRC=<host-unet.onnx> ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` |

### 3. Contracts

| Area | Contract |
|---|---|
| Model order | The Detect action must run MobileNetV3-Small classification first, persist that result in memory, then run UNet segmentation. Do not run both models at the same time unless the history schema is explicitly redesigned for parallel outputs. |
| Capture ownership | Detect owns the source image capture. The UI must not expose a separate production "save image" button that creates cloud/history records outside the model flow. |
| Source image | `SAVE_DETECT` produces exactly one source JPG for the classification and segmentation input. Detection images must come from the camera source path, not from a UI-decorated framebuffer. |
| Segmentation images | UNet must return all model evidence images as annotated outputs: raw JPG, overlay JPG, and mask PNG. These are all uploaded with `file_kind=annotated`. |
| Upload arguments | `defect-cos-upload` must accept repeated `--annotated <jpg/png>`. Legacy `--png` may remain only as compatibility input and must be normalized into annotated. |
| Content type | The upload script must derive `image/jpeg` for `.jpg/.jpeg` and `image/png` for `.png`, then use the same value in COS prepare, COS PUT, and file registration. |
| One action, one history entry | One Detect click or one `--detect-self-test` run appends one `upload_history.json` entry containing the source, every annotated image, classification result, segmentation result, cloud identifiers, and upload status. |
| History schema | New records must use `source_path`, `source_size_bytes`, `annotated_images[]`, `classification_result`, `segmentation_result`, `result_text`, `workflow_text`, `record_id`, `record_no`, `upload_status`, and `upload_time`. `jpg_path/png_path` may be written/read only as old-client compatibility fields. |
| UI state | QML must call the asynchronous Detect path and show one busy state for the whole pipeline. It must not synchronously call `saveCurrentFrameToSdCard()` from the main thread for the production Detect button. |
| Deployment | `deploy_qt_camera_display.sh` must install `defect-segment`, the UNet model, ONNX Runtime libraries, and the updated upload script into the board runtime directory or NFS rootfs before board validation. |

Example history payload shape:

```json
{
  "source_path": "/mnt/sdcard/images/uvc_20260518_151407_005847.jpg",
  "source_size_bytes": "39699",
  "annotated_images": [
    {
      "label": "UNet原图",
      "path": "/mnt/sdcard/images/segment_20260518_151415_403_raw.jpg",
      "size_bytes": "23661"
    },
    {
      "label": "UNet叠加图",
      "path": "/mnt/sdcard/images/segment_20260518_151415_403_overlay.jpg",
      "size_bytes": "23661"
    },
    {
      "label": "UNet掩膜图",
      "path": "/mnt/sdcard/images/segment_20260518_151415_403_mask.png",
      "size_bytes": "226"
    }
  ],
  "classification_result": "RESULT status=BAD class=gasket_bad confidence=0.8824 ...",
  "segmentation_result": "RESULT_SEG status=OK defect_pixels=0 ...",
  "upload_status": "上传成功 ID 55 MP157-20260518-151416",
  "workflow_text": "分类BAD；UNet未见缺陷；云端已归档"
}
```

### 4. Validation & Error Matrix

| Check | Good Result | Bad Result | Required Action |
|---|---|---|---|
| Detect entrypoint | QML exposes Detect and optional manual-page "detect current frame"; no production "save image" button remains | UI still exposes "保存图片" or calls `saveCurrentFrameToSdCard()` for production capture | Remove the UI entrypoint or demote it to self-test-only compatibility |
| Capture command | `SAVE_DETECT <dir>` returns one non-empty JPG under `/mnt/sdcard/images` | Capture fails, path outside the allowed image directory, or creates old JPG/PNG pair | Fix overlay command handling and path allowlist before model debugging |
| Classification result | `defect-classify` exits `0` and prints a `RESULT status=GOOD|BAD ... image=<source.jpg>` line | No `RESULT`, timeout, missing model, or missing labels | Treat detection as failed; do not run upload with incomplete model evidence |
| Segmentation result | `defect-segment` exits `0`, prints `RESULT_SEG`, and writes raw/overlay/mask paths | Missing `RESULT_SEG`, missing one output image, or output not in `/mnt/sdcard/images` | Fail the Detect action and surface the exact segment error |
| Annotated upload | Upload log includes every annotated path and cloud detail shows `1 + N` files | Only source or only the first result image is uploaded | Fix repeated `--annotated` parsing, loop state, or file registration |
| Dynamic content type | Raw/overlay register as `image/jpeg`; mask registers as `image/png` | Mask uploaded as JPEG or all files use one hard-coded type | Derive content type per file and keep prepare/PUT/register values identical |
| Local history | Latest JSON entry contains source plus three annotated images and both result lines | Multiple Detect actions collapse into one entry, one Detect action creates multiple entries, or result fields are lost | Fix the in-memory bundle and append logic before changing UI rendering |
| Board verification | `./qt_camera_display --detect-self-test` prints `upload_status=OK` and appends a record | SSH command times out or returns only library warnings | First verify VM/board SSH key path, then rerun with enough timeout and inspect running processes/history |

### 5. Good/Base/Bad Cases

| Case | Example | Expected Result |
|---|---|---|
| Good Detect click | User taps Detect once | Source JPG is captured, classification runs, UNet runs, COS upload stores source + 3 annotated files, and one local history entry appears |
| Good self-test | `./qt_camera_display --detect-self-test` | Prints one `RESULT ... upload_status=OK` line and appends one history record |
| Good compatibility | Old history entry contains only `jpg_path/png_path` | History model can still display it, but new records use `source_path/annotated_images` |
| Base upload script self-test | `defect-cos-upload --self-test` or static contract test | Verifies repeated annotated parsing and MIME detection without needing a full camera run |
| Bad manual save design | Detect only classifies, while a separate Save button uploads images | History and cloud data can describe different frames or missing model outputs |
| Bad upload design | `--png <mask.png>` is the only result-image argument | Raw and overlay images are dropped; PNG-only naming hides JPEG annotated files |
| Bad verification | Check only that files exist on SD card | Does not prove model order, cloud registration, MIME type, or history schema |
| Bad SSH diagnosis | Nested Windows PowerShell command times out, so assume the board detection pipeline failed | First verify the VM key and a simple `BOARD_OK`; a bad local key or quoting can be the real issue |

### 6. Tests Required

- Static contract tests:
  - Assert `test_qt_kms_overlay_assets.sh` rejects QML production text `"保存图片"`, `requestSaveCurrentFrameToSdCard()`, and `"save-image"` in the main page.
  - Assert `main.cpp` contains repeated `--annotated` argument construction and stores `classification_result` and `segmentation_result`.
  - Assert `defect-cos-upload` contains `--annotated`, `annotated_index`, and per-file content-type detection.
  - Assert deployment script installs `defect-segment` and the UNet model.
- Build/deploy:
  - Assert `build_qt_camera_display.sh` exits `0` after QML/C++ changes.
  - Assert `build_defect_segment.sh` exits `0` after UNet program changes.
  - Assert the board runtime contains executable `/root/qt_camera_display/defect-segment`.
  - Assert the board runtime contains `/root/qt_camera_display/models/defect_unet_test_decoder_head_int8.onnx`.
- Board runtime:
  - Assert `cd /root/qt_camera_display && ./qt_camera_display --detect-self-test` exits `0`.
  - Assert the output line contains `RESULT`, `segment_status=OK`, `source_path=`, and `upload_status=OK`.
  - Assert the latest `upload_history.json` record contains exactly one `source_path`, three `annotated_images`, `classification_result`, and `segmentation_result`.
  - Assert source/raw/overlay file headers are JPEG (`ff d8`) and mask header is PNG (`89 50 4e 47 0d 0a 1a 0a`) when header tools are available.
  - Assert cloud record detail contains one `file_kind=source` and all expected `file_kind=annotated` files with correct MIME types.

### 7. Wrong vs Correct

#### Wrong

```qml
// Wrong: production UI lets a manual save create records outside the model pipeline.
Button {
    text: "保存图片"
    onClicked: storageController.requestSaveCurrentFrameToSdCard()
}
```

```bash
# Wrong: upload supports only one hard-coded result image and one hard-coded type.
defect-cos-upload --jpg "$SOURCE" --png "$MASK"
CONTENT_TYPE="image/png"
```

```cpp
// Wrong: segmentation starts before the classification result is captured into the history bundle.
startClassifyAsync(sourcePath);
startSegmentAsync(sourcePath);
appendHistory(sourcePath, segmentPaths);
```

#### Correct

```qml
// Correct: one Detect action owns capture, both model runs, upload, and history creation.
Button {
    text: storageController.detectInProgress ? "检测中..." : "检测"
    enabled: !storageController.detectInProgress
    onClicked: storageController.requestDetectCurrentFrame()
}
```

```bash
# Correct: every model result image is uploaded as annotated with its own content type.
defect-cos-upload \
  --jpg "$SOURCE" \
  --annotated "$SEG_RAW_JPG" \
  --annotated "$SEG_OVERLAY_JPG" \
  --annotated "$SEG_MASK_PNG"
```

```cpp
// Correct: the bundle keeps both model outputs before one history/cloud record is finalized.
bundle.classificationResult = classificationResultLine;
bundle.segmentationResult = segmentationResultLine;
bundle.annotatedPaths << rawPath << overlayPath << maskPath;
appendDetectionHistory(bundle, uploadResult);
```
