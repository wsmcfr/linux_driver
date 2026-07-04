# STM32MP157 SD/TF 卡安全拔卡与热插拔模块文档

## 模块目的

| 项目 | 内容 |
|---|---|
| 模块目的 | 让 STM32MP157 外置 SD/TF 卡支持稳定热插拔、安全卸载、拔卡检测和重新插入自动挂载。 |
| 板级对象 | 正点原子 STM32MP157 开发板外置 SD/TF 卡槽。 |
| 内核设备 | 外置 SD 卡固定按 `mmc0` 处理；脚本会在 `/dev/mmcblk0p3`、`/dev/mmcblk0p2`、`/dev/mmcblk0p1` 中自动选择真正的 FAT/vfat 分区。 |
| 挂载路径 | `/mnt/sdcard`。 |
| 用户命令 | `sdcard-safe-remove safe-remove`、`sdcard-resume`。 |
| 核心根因 | PD10/uSD_DETECT 在 sleep pinctrl 状态被配置成 `ANALOG`，导致物理拔卡后内核仍保留陈旧 `/dev/mmcblk0p1`。 |
| 断电恢复策略 | 不使用 `sync` 挂载参数，避免拖慢图片/日志传输；突然断电后在下次挂载前由 `fsck.fat/fsck.vfat/dosfsck` 自动修复 FAT 脏标记。 |

## 修改文件清单

| 路径 | 修改原因 |
|---|---|
| 虚拟机内核仓库：`/home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31/arch/arm/boot/dts/stm32mp157d-atk.dtsi` | 在 `&sdmmc1` 中启用真实 card-detect GPIO：`cd-gpios = <&gpiod 10 GPIO_ACTIVE_LOW>;`，移除依赖 `broken-cd` 的不可检测拔卡路径。 |
| 虚拟机内核仓库：`/home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31/arch/arm/boot/dts/stm32mp15-pinctrl.dtsi` | 修正 `sdmmc1_cd_sleep_pins_a`，让 PD10/uSD_DETECT 在 sleep 状态保持 `GPIO + bias-pull-up`，避免被切到 `ANALOG` 后失去拔卡检测能力。 |
| 虚拟机编译产物：`/home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31/arch/arm/boot/dts/stm32mp157d-atk.dtb` | 编译后的设备树二进制，不能手改，只能由 DTS/DTSI 编译生成。 |
| TFTP 部署路径：`/home/cfr/linux/tftpboot/stm32mp157d-atk.dtb` | 开发板网络启动实际加载的新设备树，复制后必须重启开发板才能生效。 |
| NFS rootfs：`/home/cfr/linux/nfs/rootfs/etc/init.d/S85sdcard-mount` | 板端 SD 卡挂载服务，支持 `start/stop/restart/status/safe-remove/eject/resume`，负责挂载、卸载和状态管理。 |
| NFS rootfs：`/home/cfr/linux/nfs/rootfs/usr/bin/sdcard-safe-remove` | 用户侧安全拔卡入口；当前板端复用 `S85sdcard-mount` 命令入口，执行时必须传 `safe-remove` 子命令，实际命令为 `sdcard-safe-remove safe-remove`。 |
| NFS rootfs：`/home/cfr/linux/nfs/rootfs/usr/bin/sdcard-resume` | 用户侧恢复命令，用于不拔卡但想清除等待状态并重新挂载。 |
| 本地脚本源码：`21_sdcard_safe_remove/S85sdcard-mount` | 保存板端挂载服务的可追踪源码；挂载前先用 `blkid` 选择外置 SD 上真正的 FAT/vfat 分区，再自动探测 `fsck.vfat`、`fsck.fat`、`dosfsck` 并执行 `-a` 修复。 |
| 本地回归测试：`21_sdcard_safe_remove/test_sdcard_mount_partition_selection.sh` | 模拟格式化后 `/dev/mmcblk0p1`/`p2` 不是数据分区、`/dev/mmcblk0p3` 才是 vfat 的场景，防止脚本再次固定挂错分区。 |
| 虚拟机 Buildroot 配置：`/home/cfr/linux/buildroot/buildroot-2020.02.6/configs/stm32mp1_atk_defconfig` | 持久化启用 `BR2_PACKAGE_DOSFSTOOLS=y` 和 `BR2_PACKAGE_DOSFSTOOLS_FSCK_FAT=y`，让 rootfs 具备 FAT 自动修复工具。 |
| 本地规范：`.trellis/spec/backend/database-guidelines.md` | 沉淀 `STM32MP157 SDMMC1 Card Detect And Safe Remove` 场景，记录签名、契约、错误矩阵、Good/Base/Bad 和测试点。 |
| 本地模块文档：`21_sdcard_safe_remove/README.md` | 汇总本模块改动路径、修改原因、使用方法和验证证据，方便后续直接复用。 |

## 修改原因

| 问题 | 原因 | 修改方式 |
|---|---|---|
| `sdcard-safe-remove` 后物理拔卡，`/dev/mmcblk0p1` 仍存在 | 内核没有收到真实 `mmc0: card 0001 removed` 事件，脚本只是在等一个永远不会来的拔卡状态 | 先修设备树 card-detect 和 pinctrl，不只改脚本超时。 |
| PD10 已注册为 `cd` 但拔卡状态不可靠 | sleep pinctrl 把 PD10 配成 `ANALOG`，低功耗/状态切换后 card-detect 线失去 GPIO 输入能力 | 将 `sdmmc1_cd_sleep_pins_a` 改成 `GPIO + bias-pull-up`。 |
| 自动重新挂载不稳定 | 旧设备节点残留时，脚本无法区分“卡还在”和“内核检测失效” | 以 `/dev/mmcblk0*`、debugfs GPIO、pinctrl 和 `dmesg` 作为诊断依据。 |
| 重新插卡时提示 `Volume was not properly unmounted` | FAT32 分区存在历史脏标记，或 Windows/Linux 之间没有完成安全弹出流程 | 在 Windows 对盘符执行 `chkdsk H: /f`，必要时再执行 `chkdsk H: /f /r`；Windows 显示无错误后必须“安全删除硬件并弹出媒体”，再插回开发板验证。 |
| 突然断电后下次启动仍可能出现 FAT 未正常卸载警告 | 硬断电时 CPU 已停止执行，系统无法在断电之后补做 `umount`；只能在下一次挂载前修复 FAT 状态 | `S85sdcard-mount` 在 `mount -t vfat` 前先执行 `fsck.vfat/fsck.fat/dosfsck -a`，修复成功后再挂载；不启用 `sync` 挂载，避免降低图片传输速度。 |
| rootfs 里没有 `fsck.vfat` 或 `fsck.fat` | Buildroot 默认未启用 `dosfstools` | 在 `stm32mp1_atk_defconfig` 中启用 `BR2_PACKAGE_DOSFSTOOLS` 和 `BR2_PACKAGE_DOSFSTOOLS_FSCK_FAT`，并把 `fsck.fat` 部署到 NFS rootfs。 |
| 格式化后保存图片提示 `/mnt/sdcard 未挂载` | 新格式化工具把 FAT32 数据分区放到 `/dev/mmcblk0p3`，旧脚本固定检查 `/dev/mmcblk0p1`，导致对 loader/boot 分区执行 `fsck.vfat` 并挂载失败 | `S85sdcard-mount` 改为用 `blkid` 自动选择外置 SD 上真正的 FAT/vfat 分区，当前板端实测选择 `/dev/mmcblk0p3`。 |
| 设备树改完只重启服务不生效 | 开发板启动时只加载 tftpboot 下的 DTB | 编译 DTB，复制到 `/home/cfr/linux/tftpboot/stm32mp157d-atk.dtb`，重启开发板。 |

## 使用流程

### 编译和部署设备树

| 步骤 | 命令 | 预期结果 |
|---|---|---|
| 进入内核树 | `cd /home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31` | 位于 Linux 5.4.31 内核源码目录。 |
| 加载交叉编译环境 | `. /opt/st/stm32mp1/3.1-snapshot/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi` | `arm-ostl-linux-gnueabi-` 工具链可用。 |
| 编译 DTB | `make ARCH=arm CROSS_COMPILE=arm-ostl-linux-gnueabi- dtbs` | 生成新的 `arch/arm/boot/dts/stm32mp157d-atk.dtb`。 |
| 部署 DTB | `cp arch/arm/boot/dts/stm32mp157d-atk.dtb /home/cfr/linux/tftpboot/stm32mp157d-atk.dtb` | 网络启动目录更新。 |
| 重启开发板 | 开发板断电/复位重启 | 新设备树生效。 |

### 板端日常使用

| 场景 | 命令 | 预期结果 |
|---|---|---|
| 查看挂载状态 | `/etc/init.d/S85sdcard-mount status` | 插卡时显示实际 FAT/vfat 分区，例如 `/dev/mmcblk0p3 -> /mnt/sdcard` 或 `/dev/mmcblk0p1 -> /mnt/sdcard`。 |
| 安全拔卡 | `sdcard-safe-remove safe-remove` | 先显示“传输完成：SD 卡数据已经同步并卸载”，再提示可以安全拔卡。 |
| 物理拔卡 | 用户在看到“现在可以安全拔卡”后拔出 SD 卡 | 串口出现 `mmc0: card 0001 removed`。 |
| 重新插卡 | 将 SD 卡重新插入卡槽 | 外置 SD 上的 FAT/vfat 分区重新出现并自动挂载到 `/mnt/sdcard`。 |
| 不拔卡恢复 | `sdcard-resume` | 清除等待拔卡状态，并重新挂载当前卡。 |
| 断电后恢复 | 开机自动运行 `/etc/init.d/S85sdcard-mount start` | 脚本先识别外置 SD 上的 FAT/vfat 分区，再执行 `fsck.vfat/fsck.fat/dosfsck -a <分区>`，最后挂载到 `/mnt/sdcard`；挂载参数仍为 `rw,noatime`，不会降低后续图片写入速度。 |
| 查看自动修复日志 | `tail -n 80 /var/log/sdcard-mount.log` | 可看到 `pre-mount FAT repair started`、`clean`、`completed` 或 `failed` 等记录。 |

### Buildroot 启用 FAT 修复工具

| 步骤 | 命令 | 预期结果 |
|---|---|---|
| 进入 Buildroot | `cd /home/cfr/linux/buildroot/buildroot-2020.02.6` | 位于 Buildroot 根目录。 |
| 持久化配置 | `utils/config --file configs/stm32mp1_atk_defconfig --enable BR2_PACKAGE_DOSFSTOOLS --enable BR2_PACKAGE_DOSFSTOOLS_FSCK_FAT --disable BR2_PACKAGE_DOSFSTOOLS_FATLABEL --disable BR2_PACKAGE_DOSFSTOOLS_MKFS_FAT` | defconfig 只启用 FAT 检查工具，不额外启用格式化工具。 |
| 更新当前配置 | `utils/config --file .config --enable BR2_PACKAGE_DOSFSTOOLS --enable BR2_PACKAGE_DOSFSTOOLS_FSCK_FAT --disable BR2_PACKAGE_DOSFSTOOLS_FATLABEL --disable BR2_PACKAGE_DOSFSTOOLS_MKFS_FAT && make olddefconfig` | 当前 Buildroot `.config` 与 defconfig 一致。 |
| 构建工具 | `make dosfstools` | 在 `output/target/sbin/fsck.fat` 生成 ARM 目标文件，并生成 `fsck.vfat`、`dosfsck` 兼容链接。 |
| 部署到 NFS rootfs | `install -m 0755 output/target/sbin/fsck.fat /home/cfr/linux/nfs/rootfs/sbin/fsck.fat && ln -sf fsck.fat /home/cfr/linux/nfs/rootfs/sbin/fsck.vfat && ln -sf fsck.fat /home/cfr/linux/nfs/rootfs/sbin/dosfsck` | 开发板下次启动时脚本能找到 FAT 修复工具。 |

### Windows 修复 FAT32 脏标记

| 步骤 | 命令/操作 | 预期结果 |
|---|---|---|
| 确认盘符 | 在 Windows “此电脑”确认 SD 卡盘符，例如 `H:` | 不要修错磁盘。 |
| 基本修复 | `chkdsk H: /f` | Windows 扫描 FAT32 文件系统；若显示没有问题，也会刷新文件系统状态。 |
| 坏块检查 | `chkdsk H: /f /r` | 可选，耗时更长；用于怀疑 SD 卡物理坏块时。 |
| 安全弹出 | 右下角“安全删除硬件并弹出媒体”，选择 H 盘对应 SD 卡 | Windows 提示可以安全移除后再拔卡。 |
| 回板验证 | 插回开发板，观察串口并运行 `status/mount/df` | 不再出现 FAT 警告，且真实 FAT/vfat 分区挂载到 `/mnt/sdcard`。 |

## 修改记录

| 时间 | 修改点 | 结果 |
|---|---|---|
| 2026-05-01 | 在 `stm32mp157d-atk.dtsi` 中启用 SDMMC1 `cd-gpios = <&gpiod 10 GPIO_ACTIVE_LOW>` | 外置 SD 卡使用真实 card-detect 线检测插拔。 |
| 2026-05-01 | 修正 `stm32mp15-pinctrl.dtsi` 的 `sdmmc1_cd_sleep_pins_a` | PD10/uSD_DETECT 在 sleep 状态保持 GPIO 输入和上拉，不再残留 `/dev/mmcblk0p1`。 |
| 2026-05-01 | 部署 `S85sdcard-mount`、`sdcard-safe-remove`、`sdcard-resume` | 板端支持安全拔卡、等待真实拔出和重新插入自动挂载。 |
| 2026-05-01 | 更新 `.trellis/spec/backend/database-guidelines.md` | 把“先查 card-detect/pinctrl，不能只改脚本”的经验写入规范。 |
| 2026-05-03 | Windows 下执行 `chkdsk H: /f` 和 `chkdsk H: /f /r` 后安全弹出 SD 卡 | 开发板再次插卡后自动挂载正常，`status` 显示 `safe-remove: no pending removal`，后续重新插入不再继续打印 FAT 未正常卸载警告。 |
| 2026-05-03 | `S85sdcard-mount` 增加挂载前 FAT 自动修复 | 下次开机或重新插卡时先执行 `fsck.vfat/fsck.fat/dosfsck -a`，修复突然断电留下的 FAT 脏标记；挂载参数保持 `rw,noatime`，不拖慢图片传输。 |
| 2026-05-03 | Buildroot `stm32mp1_atk_defconfig` 启用 `dosfstools` 的 `fsck.fat` | NFS rootfs 可以部署 `fsck.fat`、`fsck.vfat`、`dosfsck`，供自动挂载脚本调用。 |
| 2026-05-16 | `S85sdcard-mount` 改为自动选择 FAT/vfat 分区 | 解决 SD 卡重新格式化后 FAT32 数据分区变成 `/dev/mmcblk0p3`，旧脚本固定挂 `/dev/mmcblk0p1` 导致 `/mnt/sdcard 未挂载` 的问题。 |
| 2026-05-16 | 新增 `test_sdcard_mount_partition_selection.sh` | 用假 sysfs/dev 和假 `blkid` 回归测试 “p3 是 vfat、p1/p2 不是数据分区” 场景。 |
| 2026-07-05 | 修正文档中的安全拔卡命令写法 | 当前板端 `/usr/bin/sdcard-safe-remove` 复用 `S85sdcard-mount` 的 `case "$1"` 入口，裸命令只会打印 Usage；日常命令和 Qt 调用都应使用 `sdcard-safe-remove safe-remove`。 |

## 硬件资源

| 资源 | 用途 | 契约 |
|---|---|---|
| SDMMC1 | 外置 SD/TF 卡控制器 | 对应外置卡，不能和 eMMC 混淆。 |
| `mmc0` | 外置 SD 卡设备编号 | 外置卡数据分区不固定，脚本必须自动选择 FAT/vfat 分区，当前格式化后实测为 `/dev/mmcblk0p3`。 |
| `mmc1` | 板载 eMMC 设备编号 | 自动挂载脚本不能误操作 eMMC。 |
| PD10/uSD_DETECT | SD 卡插拔检测 GPIO | active-low，移除卡时 raw GPIO 应为高。 |
| `cd-gpios = <&gpiod 10 GPIO_ACTIVE_LOW>;` | 设备树 card-detect 属性 | 用真实 CD 线代替 `broken-cd`。 |
| `sdmmc1_cd_sleep_pins_a` | sleep 状态 pinctrl | 必须保持 `GPIO + bias-pull-up`，禁止改为 `ANALOG`。 |

## 验证证据

| 验证项 | 已确认结果 |
|---|---|
| DTB 编译 | `make ARCH=arm CROSS_COMPILE=arm-ostl-linux-gnueabi- dtbs` 通过。 |
| DTB 部署 | 新 `stm32mp157d-atk.dtb` 已复制到 `/home/cfr/linux/tftpboot/stm32mp157d-atk.dtb` 并重启生效。 |
| 未插卡设备节点 | `ls /dev/mmcblk0* 2>/dev/null || echo "mmcblk0 gone"` 输出 `mmcblk0 gone`。 |
| GPIO debug 状态 | `gpio-58 ... cd ... in hi IRQ ACTIVE LOW`。 |
| pinctrl 状态 | `pin 58 (PD10): 58005000.sdmmc GPIOD:58 function gpio group PD10`。 |
| 插卡挂载 | `/etc/init.d/S85sdcard-mount status` 显示真实 FAT/vfat 分区挂载到 `/mnt/sdcard`，例如 `/dev/mmcblk0p3 -> /mnt/sdcard`。 |
| 安全拔卡 | `sdcard-safe-remove safe-remove` 显示传输完成后拔卡，内核打印 `mmc0: card 0001 removed`。 |
| 重新插卡 | `mount | grep sdcard` 和 `df -h /mnt/sdcard` 确认 FAT32 SD 卡重新挂载。 |
| Windows 修复 FAT 状态 | `chkdsk H: /f` 和 `chkdsk H: /f /r` 均显示“Windows 已扫描文件系统并且没有发现问题”。 |
| FAT 警告恢复 | Windows 安全弹出后插回开发板，`/etc/init.d/S85sdcard-mount status` 显示真实 FAT/vfat 分区挂载到 `/mnt/sdcard` 和 `safe-remove: no pending removal`，再次插卡日志不再重复出现 `Volume was not properly unmounted`。 |
| 挂载前自动修复脚本语法 | `sh -n /etc/init.d/S85sdcard-mount` | shell 语法检查通过。 |
| FAT 修复工具配置 | `grep -E 'BR2_PACKAGE_DOSFSTOOLS|BR2_PACKAGE_DOSFSTOOLS_FSCK_FAT' configs/stm32mp1_atk_defconfig .config` | defconfig 和当前 `.config` 都启用 `dosfstools` 与 `fsck.fat`。 |
| 自动修复工具部署 | `ls -l /home/cfr/linux/nfs/rootfs/sbin/fsck.fat /home/cfr/linux/nfs/rootfs/sbin/fsck.vfat /home/cfr/linux/nfs/rootfs/sbin/dosfsck` | 三个命令存在，其中 `fsck.vfat` 和 `dosfsck` 指向 `fsck.fat`。 |
| 分区选择回归测试 | `sh 21_sdcard_safe_remove/test_sdcard_mount_partition_selection.sh` | 输出 `PASS: FAT partition selection prefers real vfat partition`。 |

## 新版测试与验证矩阵

| Test goal | Run location | Command | Expected result | Failure triage |
|---|---|---|---|---|
| 脚本语法检查 | 开发板或虚拟机 rootfs | `sh -n /etc/init.d/S85sdcard-mount` | 命令退出码为 0。 | 若失败，先修 shell 语法，不要上板重启测试。 |
| FAT 修复工具存在 | 开发板 | `command -v fsck.vfat || command -v fsck.fat || command -v dosfsck` | 至少找到一个 FAT 修复工具。 | 若没有，回 Buildroot 启用 `BR2_PACKAGE_DOSFSTOOLS_FSCK_FAT` 并部署 `/sbin/fsck.fat`。 |
| 挂载前自动修复日志 | 开发板 | `/etc/init.d/S85sdcard-mount restart; tail -n 80 /var/log/sdcard-mount.log` | 日志包含 `pre-mount FAT repair started` 和 clean/completed/skipped 之一。 | 若 skipped，说明工具缺失；若 failed，禁止继续挂载，先在 Windows 或 Linux 上修复 FAT。 |
| 挂载状态 | 开发板 | `/etc/init.d/S85sdcard-mount status; mount | grep /mnt/sdcard; df -h /mnt/sdcard` | 显示真实 FAT/vfat 分区挂载到 `/mnt/sdcard` 和容量，例如 `/dev/mmcblk0p3 -> /mnt/sdcard`。 | 若设备节点不存在，查插卡、PD10 CD GPIO、`dmesg`；若分区识别错误，查 `blkid /dev/mmcblk0p*`。 |
| 分区选择回归 | 虚拟机或本机 Git Bash | `sh 21_sdcard_safe_remove/test_sdcard_mount_partition_selection.sh` | 输出 `PASS: FAT partition selection prefers real vfat partition`。 | 若失败，先查 `find_sd_partition` 是否又固定返回 `p1`，或测试 fake `blkid` 是否没有进入 PATH。 |
| 小文件写读 | 开发板 | `printf "sdcard-ok\n" > /mnt/sdcard/rw-small.txt; sync; cat /mnt/sdcard/rw-small.txt` | 输出 `sdcard-ok`。 | 若只读或写失败，查挂载参数、FAT 状态和卡写保护。 |
| 大文件读写比较 | 开发板 | `SRC=/tmp/sdcard-src-16m.bin; DST=/mnt/sdcard/rw-16m.bin; dd if=/dev/zero of="$SRC" bs=1M count=16; cp "$SRC" "$DST"; sync; cmp "$SRC" "$DST"` | `dd/cp/cmp` 成功，`cmp` 无输出。 | 若慢或失败，查卡质量、供电、FAT 错误；不要给挂载参数加 `sync` 牺牲图片传输速度。 |
| 图片保存等待 | 开发板 | `file=/mnt/sdcard/images/test.jpg; s1=$(stat -c %s "$file"); sleep 1; s2=$(stat -c %s "$file"); [ "$s1" = "$s2" ] && [ "$s1" -gt 0 ]` | 图片存在且连续两次大小一致。 | 若大小还在变，继续等待生产进程结束；若为 0，查拍照程序。 |
| 安全拔卡 | 开发板 | `sdcard-safe-remove safe-remove` | 先同步并卸载，再提示可以安全拔卡。 | 若超时，查是否出现真实 `mmc0: card 0001 removed`，不要只改脚本等待时间。 |
| 重新插卡恢复 | 开发板 | 物理拔卡再插卡后执行 `/etc/init.d/S85sdcard-mount status` | 自动重新挂载，无 pending removal。 | 若不挂载，查 `/dev/mmcblk0*`、CD GPIO、pinctrl 和日志锁文件。 |
| 不拔卡恢复 | 开发板 | `sdcard-resume; /etc/init.d/S85sdcard-mount status` | 清除等待拔卡状态并重新挂载当前卡。 | 若仍 pending，查锁文件和脚本日志。 |

## SD 卡读写和图片保存标准流程

小文件验证：

```sh
printf "sdcard-ok\n" > /mnt/sdcard/rw-small.txt
sync
cat /mnt/sdcard/rw-small.txt
```

大文件验证：

```sh
SRC=/tmp/sdcard-src-16m.bin
DST=/mnt/sdcard/rw-16m.bin
dd if=/dev/zero of="$SRC" bs=1M count=16
cp "$SRC" "$DST"
sync
cmp "$SRC" "$DST"
rm -f "$SRC" "$DST"
sync
```

图片保存等待：

```sh
mkdir -p /mnt/sdcard/images
file=/mnt/sdcard/images/test.jpg
# 先运行实际拍照或截图程序，让它把图片保存到 "$file"。
while true; do
    s1=$(stat -c %s "$file" 2>/dev/null || echo 0)
    sleep 1
    s2=$(stat -c %s "$file" 2>/dev/null || echo 0)
    [ "$s1" = "$s2" ] && [ "$s1" -gt 0 ] && break
done
sync
sdcard-safe-remove safe-remove
```

`MOUNT_OPTIONS` 保持 `rw,noatime`，不要加入 `sync` 挂载参数；图片、日志和视频的性能靠普通异步写入保证，数据一致性靠“生产者退出/文件大小稳定 + sync/fsync + safe-remove”保证。突然断电无法在断电之后自动卸载，只能在下次挂载前由 `fsck.vfat/fsck.fat/dosfsck -a` 修复 FAT 脏标记。

## 排障规则

| 现象 | 先查什么 | 不要先做什么 |
|---|---|---|
| 拔卡后 `/dev/mmcblk0p1` 仍存在 | `/dev/mmcblk0*`、debugfs GPIO、pinctrl、`dmesg` | 不要只加脚本 timeout 或强制 remount。 |
| `sdcard-safe-remove safe-remove` 等待超时 | 是否出现真实 `mmc0: card 0001 removed` | 不要把问题直接归因到 shell 脚本。 |
| 重新插卡出现 FAT 未正常卸载警告 | 先查 `/var/log/sdcard-mount.log` 是否出现 `pre-mount FAT repair`，再查 rootfs 是否存在 `/sbin/fsck.vfat` 或 `/sbin/fsck.fat` | 不要把 FAT 脏标记误判成 card-detect 或自动挂载失败。 |
| 保存图片提示 `/mnt/sdcard 未挂载` 且 `mmcblk0` 存在 | 先执行 `blkid /dev/mmcblk0p1 /dev/mmcblk0p2 /dev/mmcblk0p3`，确认哪个分区是 `TYPE="vfat"` | 不要继续固定挂 `/dev/mmcblk0p1`，格式化工具可能把数据分区放到 `p3`。 |
| 日志显示缺少 `fsck.vfat/fsck.fat/dosfsck` | Buildroot 是否启用 `BR2_PACKAGE_DOSFSTOOLS=y` 和 `BR2_PACKAGE_DOSFSTOOLS_FSCK_FAT=y`，NFS rootfs 是否部署 `/sbin/fsck.fat` | 不要给 `MOUNT_OPTIONS` 增加 `sync` 来掩盖工具缺失，因为会拖慢图片传输。 |
| CD GPIO 注册但读数异常 | PD10 是否在 sleep/runtime 状态被切到 `ANALOG` | 不要只看 `cd-gpios` 存在就认为设备树正确。 |
| 改 DTS 后行为不变 | 是否编译并部署了新的 DTB，开发板是否重启 | 不要只重启 `/etc/init.d/S85sdcard-mount`。 |
