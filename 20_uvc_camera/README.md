# UVC USB 摄像头 Framebuffer 预览示例

这个目录提供一个轻量测试程序：直接从 `/dev/video0` 读取 UVC 摄像头的 YUYV 数据，再写入 `/dev/fb0`，用于在 GStreamer/Qt 环境完整构建之前先验证 USB 摄像头、LCD framebuffer 和内核 UVC 驱动链路。

## 模块文档总览

| 项目 | 内容 |
|---|---|
| 模块目的 | 验证 STM32MP157 上 USB UVC 摄像头、V4L2、framebuffer、GStreamer、GPU galcore 和开机自启链路，为后续 Qt 工业检测界面提供摄像头输入基础。 |
| 模块目录 | `20_uvc_camera/` |
| 板端输入 | UVC 摄像头，默认 `/dev/video0`，必要时自动尝试 `/dev/video1` 等节点。 |
| 板端输出 | RGB LCD framebuffer `/dev/fb0`，后续 Qt 路线还会使用 `/dev/galcore` 和 DRM/KMS。 |
| 当前稳定路线 | `S90uvc-camera` 默认启动 Qt + KMS overlay；本目录的 `uvc_fb_preview` 保留为 framebuffer 验证和兜底路线。 |
| 早期静态首帧 | `fb_boot_splash` 在 Qt/GPU 启动前直接写 `/dev/fb0`，显示与 Qt 启动画面第一帧风格一致的静态启动图，减少纯黑屏空窗。 |
| Qt 检测交互 | Qt 首页只保留 `检测` 和 `安全卸载`；`检测` 在后台线程保存当前帧、运行分类模型、运行 UNet、上传 COS 并写历史。分类模型完成后立即显示零件类型和类别，UNet 完成后立即显示双模型耗时，上传完成后才写入云端状态和历史记录。 |

## 修改文件清单

| 路径 | 修改原因 |
|---|---|
| `20_uvc_camera/uvc_fb_preview.c` | 新增轻量 V4L2 framebuffer 预览程序，用于在 Qt/GStreamer 完整环境前先验证摄像头采集、格式转换和 LCD 输出链路。 |
| `20_uvc_camera/Makefile` | 提供本模块本地编译和部署入口，方便在虚拟机内生成 `uvc_fb_preview` 并复制到 NFS rootfs。 |
| `20_uvc_camera/S05display-quiet` | Buildroot 早期 init 脚本，尽早关闭 framebuffer console 光标、清理 `/dev/tty0` 残留，并在 `/dev/fb0` 已可用时调用 `fb_boot_splash` 绘制静态首帧。 |
| `20_uvc_camera/S90uvc-camera` | 增加 Buildroot SysV init 自启脚本，启动时加载 `galcore`，等待 `/dev/fb0` 和 `/dev/video*`，在等待 Qt/KMS 前再次绘制静态首帧，默认进入 Qt + KMS overlay 后端，并保留 framebuffer/GStreamer 兜底后端。 |
| `20_uvc_camera/install_uvc_autostart.sh` | 把 `uvc_fb_preview`、UVC/GStreamer 运行包和 `S90uvc-camera` 安装到 NFS rootfs 的持久路径，避免放在 `/tmp` 后被 tmpfs 覆盖。 |
| `20_uvc_camera/run_gst_fbdev.sh` | 提供板端 GStreamer raw/MJPEG framebuffer 显示脚本，用于对照官方路线和排查摄像头格式。 |
| `20_uvc_camera/uvc_runtime_env.sh` | 设置板端轻量 GStreamer/libv4l 运行环境变量，确保 `gst-launch-1.0`、插件和动态库能被找到。 |
| `20_uvc_camera/buildroot_uvc_gstreamer.fragment` | 记录 Buildroot 中 UVC/GStreamer/v4l2 工具的配置片段，避免 rootfs 重建后丢失工具。 |
| `20_uvc_camera/buildroot_qt_gpu.fragment` | 记录 Qt/GPU/GStreamer 用户态依赖配置片段，为 Qt 摄像头界面准备运行库。 |
| `20_uvc_camera/README.md` | 记录本模块目的、文件路径、使用方法、性能结论、验证证据和后续路线。 |
| 虚拟机内核仓库：`/home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31/arch/arm/configs/stm32mp1_atk_defconfig` | 持久化 UVC、V4L2/media、USB Type-C/host、videobuf2 和 galcore 相关内核配置。 |
| 虚拟机内核仓库：`/home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31/drivers/gpu/drm/gcnano-driver-6.4.3/` | 集成并保留官方 gcnano/galcore GPU 驱动，供 Qt Quick 和 OpenGL ES 使用。 |
| 虚拟机 Buildroot：`/home/cfr/linux/buildroot/buildroot-2020.02.6/configs/stm32mp1_atk_defconfig` | 持久化 `v4l2-ctl`、GStreamer、v4l2src、fbdevsink、kmssink、libv4l 等 rootfs 工具。 |
| NFS rootfs：`/home/cfr/linux/nfs/rootfs/root/uvc_fb_preview` | 板端实际运行的 framebuffer 预览程序持久路径。 |
| NFS rootfs：`/home/cfr/linux/nfs/rootfs/root/uvc-rootfs/` | 板端实际运行的轻量 UVC/GStreamer runtime 持久路径。 |
| NFS rootfs：`/home/cfr/linux/nfs/rootfs/root/qt_camera_display/fb_boot_splash` | 板端实际运行的早期静态启动图绘制器。 |
| NFS rootfs：`/home/cfr/linux/nfs/rootfs/etc/init.d/S05display-quiet` | 板端早期显示静默入口，开发板重启后由 Buildroot `rcS` 在 S90 前调用。 |
| NFS rootfs：`/home/cfr/linux/nfs/rootfs/etc/init.d/S90uvc-camera` | 板端开机自启入口，开发板重启后由 Buildroot `rcS` 调用。 |
| `20_uvc_camera/qt_camera_display/main.cpp` | Qt 控制器保留旧后台保存自检入口，同时正式检测入口在后台完成当前帧保存、分类、UNet、COS 上传和历史记录写入，避免阻塞主界面触摸。 |
| `20_uvc_camera/qt_camera_display/qml/Main.qml` | 首页不再暴露独立 `保存图片` 按钮；`检测` 按钮改为分阶段刷新结果，分类完成先显示零件/类别，UNet 完成再显示双模型耗时。 |
| `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh` | 增加静态契约，禁止 QML 主线程直接同步调用 `saveCurrentFrameToSdCard()`。 |

## 使用流程

| 阶段 | 命令 | 预期结果 |
|---|---|---|
| 本地编译 framebuffer 预览 | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera && make` | 生成 `uvc_fb_preview`。 |
| 部署基础预览 | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera && ./install_uvc_autostart.sh` | 程序、运行包和 `S90uvc-camera` 被复制到 NFS rootfs 的持久路径。 |
| 板端手动预览 | `/root/uvc_fb_preview -d /dev/video0 -f /dev/fb0 -w 640 -h 480` | RGB 屏显示摄像头画面。 |
| 板端低 CPU 原型 | `/root/uvc_fb_preview -d /dev/video0 -f /dev/fb0 -w 640 -h 480 -r 15 -p` | 原始 640x480 居中绘制，用于排查缩放开销。 |
| GStreamer 环境检查 | `. /root/uvc-rootfs/uvc_runtime_env.sh && gst-inspect-1.0 v4l2src && v4l2-ctl -d /dev/video0 --list-formats-ext` | 能看到 GStreamer 插件和摄像头支持格式。 |
| GStreamer raw 显示 | `/root/uvc-rootfs/run_gst_fbdev.sh raw /dev/video0 /dev/fb0 640 480` | raw 路线显示摄像头。 |
| GStreamer MJPEG 显示 | `/root/uvc-rootfs/run_gst_fbdev.sh mjpeg /dev/video0 /dev/fb0 640 480` | MJPEG 路线显示摄像头，便于对照摄像头输出格式。 |
| 手动绘制早期静态首帧 | `/root/qt_camera_display/fb_boot_splash -f /dev/fb0` | LCD 显示深色工业检测启动首帧，包含英文标题、检测窗口、ROI 框、扫描线和 18% 初始进度。 |
| 开机脚本控制 | `/etc/init.d/S90uvc-camera restart && /etc/init.d/S90uvc-camera status` | 默认启动 Qt + KMS overlay，或按 `UVC_BACKEND` 启动指定后端。 |

## 修改记录

| 时间 | 修改点 | 结果 |
|---|---|---|
| 2026-04-29 | 新增 UVC framebuffer 预览程序和 Buildroot/GStreamer 配置片段 | 能在完整 Qt 环境前验证 `/dev/video0` 到 `/dev/fb0` 的基本链路。 |
| 2026-04-29 | 整理 `install_uvc_autostart.sh` 和 `S90uvc-camera` | 板端开机可自动启动摄像头显示，且不再依赖会被 tmpfs 覆盖的 `/tmp`。 |
| 2026-04-30 | 增加 Qt/GPU 运行环境记录和 Qt 摄像头界面入口 | 摄像头模块开始服务于 Qt 工业检测界面。 |
| 2026-05-01 | 默认显示路线切到 Qt + KMS overlay | 开机后优先启动正式 UI+视频路线，framebuffer 预览保留为排障兜底。 |
| 2026-05-16 | 新增早期显示静默脚本 | `S05display-quiet` 在 `S90uvc-camera` 前关闭 fbcon 光标并清屏，配合 Qt 启动画面减少 `_` 闪烁空窗；彻底消除最早期内核光标仍建议配合 bootargs `vt.global_cursor_default=0`。 |
| 2026-05-16 | 新增早期静态首帧 | `fb_boot_splash` 直接写 `/dev/fb0`，`S05display-quiet`、`S90uvc-camera` 和 `run_qt_kms_overlay_display.sh` 在 Qt 启动前调用它，让等待 GPU、摄像头和 Qt 的阶段显示静态启动图而不是纯黑屏。 |
| 2026-05-16 | 修复 Qt 告警维护保存诊断无文件 | `保存诊断` 不再只显示目标路径，Qt 控制器会真实写入 `/mnt/sdcard/logs/qt_alarm_snapshot.txt` 并 `fsync`；SSH 可用 `test -s`、`wc -c`、`tail` 直接验证。 |
| 2026-05-16 | 修复保存图片期间界面卡住 | QML 不再同步等待 `saveCurrentFrameToSdCard()`；改为 `requestSaveCurrentFrameToSdCard()` 后台保存，完成后通过信号回填结果。 |
| 2026-05-18 | 修复检测结果显示被上传阻塞 | Qt 检测链路拆出 `detectClassificationReady` 和 `detectModelsReady` 阶段信号；首页零件类型/类别在第一个分类模型完成后显示，双模型耗时在 UNet 完成后显示，不再等 COS 上传完成。 |

## 硬件资源

| 资源 | 用途 | 注意事项 |
|---|---|---|
| UVC USB 摄像头 | 视频采集输入 | 插入后应能通过 `lsusb`、`/dev/video*` 和 `v4l2-ctl` 看到设备。 |
| `/dev/video0` 或 `/dev/video1` | V4L2 摄像头节点 | 脚本默认 `/dev/video0`，必要时自动挑选可用 video 节点。 |
| `/dev/fb0` | RGB LCD framebuffer 输出 | framebuffer 预览、早期静态首帧和部分兜底显示路线依赖它。 |
| `/dev/galcore` | Vivante/galcore GPU 设备 | Qt Quick/eglfs 和 GL 路线依赖它，`S90uvc-camera` 会尝试 `modprobe galcore`。 |
| DRM/KMS plane | Qt + KMS overlay 正式路线的视频平面 | 具体 overlay 参数在 `qt_camera_display/run_qt_kms_overlay_display.sh` 中维护。 |

## 验证证据

| 验证项 | 已记录结果 |
|---|---|
| 旧 framebuffer 预览 | `/root/uvc_fb_preview` 约 `44%~49% CPU`。 |
| 原始尺寸 framebuffer 原型 | `/root/uvc_fb_preview -r 15 -p` 约 `21.6% CPU`。 |
| Buildroot UVC/GStreamer 配置 | `olddefconfig` 验证 GStreamer、UVC、fbdev/kmssink、libv4l 配置保留。 |
| UVC built-in 配置 | 内核 UVC/media/videobuf2 配置被记录为持久配置，UVC 不依赖临时模块手动加载。 |
| 开机路径 | `S90uvc-camera` 支持 `start/stop/restart/status`，并把日志写到 `/var/log/uvc-camera.log`。 |

## 新版测试与验证矩阵

| Test goal | Run location | Command | Expected result | Failure triage |
|---|---|---|---|---|
| 摄像头枚举 | 开发板 | `lsusb; ls -l /dev/video*; dmesg | grep -Ei "uvc|video"` | 能看到 USB 摄像头和 `/dev/video0` 或其它 video 节点。 | 若没有节点，先查 USB 供电、线缆、内核 UVC 配置和 `dmesg` 枚举错误。 |
| framebuffer 节点 | 开发板 | `ls -l /dev/fb0; fbset 2>/dev/null || true` | `/dev/fb0` 存在，LCD 已初始化。 | 若缺失，先查 LCD 设备树、framebuffer 驱动和启动日志。 |
| GPU 节点 | 开发板 | `modprobe galcore; ls -l /dev/galcore` | `/dev/galcore` 存在。 | 若不存在，查 `galcore.ko` 是否在 rootfs、内核版本是否匹配、`dmesg` 是否有 galcore 错误。 |
| V4L2 格式 | 开发板 | `. /root/uvc-rootfs/uvc_runtime_env.sh && v4l2-ctl -d /dev/video0 --list-formats-ext` | 输出 YUYV/MJPEG 等摄像头格式和分辨率。 | 若 `v4l2-ctl` 缺失，先查 Buildroot/rootfs 是否部署 UVC runtime。 |
| 早期显示静默 | 开发板 | `/etc/init.d/S05display-quiet start; cat /sys/class/graphics/fbcon/cursor_blink` | 输出 `0`，LCD 不再持续显示 fbcon `_` 闪烁光标。 | 若节点不存在或仍输出 `1`，先查 framebuffer console 是否启用、脚本权限和当前 bootargs；最早期光标需用 `vt.global_cursor_default=0` 彻底处理。 |
| 早期静态首帧 | 开发板 | `/root/qt_camera_display/fb_boot_splash -f /dev/fb0; echo $?` | 返回 `0`，LCD 显示工业检测静态首帧；这一步不需要 `/dev/galcore`、Qt runtime 或摄像头节点。 | 若返回非 0，先查 `/dev/fb0` 是否存在、像素格式是否为 16/24/32 bpp、程序是否可执行；若显示后又被黑屏覆盖，查后续脚本或 Qt 是否清屏。 |
| framebuffer 兜底显示 | 开发板 | `/root/uvc_fb_preview -d /dev/video0 -f /dev/fb0 -w 640 -h 480 -r 15 -p -n 120` | LCD 显示约 120 帧后程序退出。 | 若画面黑屏，查摄像头格式是否被驱动接受、`/dev/fb0` 像素格式和日志错误。 |
| GStreamer raw 显示 | 开发板 | `/root/uvc-rootfs/run_gst_fbdev.sh raw /dev/video0 /dev/fb0 640 480` | LCD 显示 raw 视频。 | 若 not-negotiated，先按 `v4l2-ctl` 输出调整格式、宽高或切到 MJPEG。 |
| GStreamer MJPEG 显示 | 开发板 | `/root/uvc-rootfs/run_gst_fbdev.sh mjpeg /dev/video0 /dev/fb0 640 480` | 摄像头支持 MJPEG 时可显示。 | 若 `jpegdec` 缺失，查 GStreamer 插件部署。 |
| 开机脚本状态 | 开发板 | `/etc/init.d/S90uvc-camera restart; /etc/init.d/S90uvc-camera status; tail -n 120 /var/log/uvc-camera.log` | 显示预览运行中，日志记录所选后端、video 节点和 PID。 | 若启动后退出，先看日志中的 missing node、galcore required、脚本不可执行。 |
| Qt + KMS overlay 正式路线 | 开发板 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart; /root/qt_camera_display/run_qt_kms_overlay_display.sh status` | Qt PID 和 `uvc_kms_overlay` PID 同时存在，LCD 显示 UI + 视频。 | 若只剩一个 PID，查 `/tmp/qt_camera_display.log`、`/tmp/uvc_kms_overlay.log` 和 plane 占用。 |
| 检测阶段显示响应 | 开发板屏幕 | 点击 `检测` 后观察右侧结果面板，同时点击左侧 `历史记录`、`统计分析`、`手动控制`、`参数设置` 或 `告警维护` | 页面应立即响应触摸；分类完成后零件类型/类别先显示，UNet 完成后双模型耗时显示，不能等 COS 上传完成才一起显示；重复 `检测` 和 `安全卸载` 暂时不可点。 | 若页面切换卡住，查 `Main.qml` 是否又直接同步调用检测函数；若零件/类别仍等上传后显示，确认 Qt 二进制包含 `detectClassificationReady` 和 `detectModelsReady`。 |
| CPU 占用对照 | 开发板 | `top -b -n 2 | grep -E "uvc_fb_preview|uvc_kms_overlay|qt_camera_display"` | 能看到对应进程 CPU 样本，用于对比路线。 | 若没有进程，说明显示链路未运行，先回到 status 和日志。 |

## 读写/数据路径验证

| 数据路径 | 读验证 | 写验证 | 通过标准 |
|---|---|---|---|
| UVC 摄像头输入 | `v4l2-ctl -d /dev/video0 --list-formats-ext` | 不向摄像头写普通文件数据，只通过 V4L2 ioctl 设置格式和启动流 | 能列出格式，预览程序能持续取帧。 |
| 早期静态首帧输出 | `ls -l /root/qt_camera_display/fb_boot_splash /dev/fb0` | `/root/qt_camera_display/fb_boot_splash -f /dev/fb0` | LCD 显示静态启动首帧，命令返回 `0`，不依赖摄像头、Qt 或 GPU。 |
| framebuffer 兜底输出 | `fbset` 或观察 LCD 当前画面 | `/root/uvc_fb_preview ... -f /dev/fb0` | LCD 出现实时画面，程序退出后无崩溃日志。 |
| GStreamer 显示链路 | `gst-inspect-1.0 v4l2src fbdevsink` | `/root/uvc-rootfs/run_gst_fbdev.sh raw ...` | pipeline 能运行并把视频写到显示设备。 |
| Qt + KMS overlay | `run_qt_kms_overlay_display.sh status` | `run_qt_kms_overlay_display.sh restart` | Qt UI 进程和 overlay 视频进程同时存在。 |
| Qt 检测 UI 响应 | 屏幕观察 `检测中...`、分类结果和双模型耗时并切换左侧页面 | `检测` 调用后台检测任务，分类完成、UNet 完成和上传完成分别通过信号回到 QML | 检测和上传期间主界面仍响应触摸；零件类型/类别不等待上传，双模型耗时不等待上传；只有重复检测和安全卸载临时禁用。 |
| 图片/日志落盘 | `stat -c %s <file>` 连续两次一致 | 实际拍照/日志程序写入 `/mnt/sdcard/...` 后 `sync` | 文件存在、大小稳定，再执行 `sdcard-safe-remove`。 |
| 告警诊断快照 | `test -s /mnt/sdcard/logs/qt_alarm_snapshot.txt; tail -n 30 /mnt/sdcard/logs/qt_alarm_snapshot.txt` | Qt 告警维护页点击 `保存诊断`，或执行 `/root/qt_camera_display/qt_camera_display --alarm-snapshot-self-test` | 文件非空，内容包含 `alarm_code=`、`camera_status=` 和 `[recent_alarm_history]`；写入由 Qt 控制器执行 `fsync`。 |

## 图片和视频保存到 SD 卡的等待规则

当前 `uvc_fb_preview` 和 KMS overlay 主要用于实时预览，不直接保存 JPEG。如果后续把拍照、截图或检测图片保存到 `/mnt/sdcard`，必须按下面顺序验证完成，避免断电或拔卡后出现 FAT 脏标记：

当前模块没有内置拍照命令；下面片段用于等待“实际拍照/截图程序已经开始写入的文件”完成：

```sh
mkdir -p /mnt/sdcard/images
file=/mnt/sdcard/images/uvc_test.jpg
# 先运行实际拍照或截图程序，让它把图片保存到 "$file"。
while true; do
    s1=$(stat -c %s "$file" 2>/dev/null || echo 0)
    sleep 1
    s2=$(stat -c %s "$file" 2>/dev/null || echo 0)
    [ "$s1" = "$s2" ] && [ "$s1" -gt 0 ] && break
done
sync
sdcard-safe-remove
```

| 步骤 | 必须满足的完成条件 |
|---|---|
| 生产者完成 | 拍照/截图/录像进程退出，或应用明确返回“保存完成”。 |
| 文件存在 | `test -s /mnt/sdcard/images/uvc_test.jpg` 成功，文件大小大于 0。 |
| 大小稳定 | `stat -c %s` 连续两次结果一致；`sleep` 只是轮询间隔，不是完成证明。 |
| 数据刷盘 | 应用若有 `fsync` 则以应用完成事件为准；shell 测试至少执行 `sync`。 |
| 安全卸载 | 可移动 SD 卡执行 `sdcard-safe-remove`，看到提示后再拔卡或断电。 |

如果保存的是视频文件，除大小稳定外，还要优先等待录像进程正常退出，因为容器尾部索引通常在退出阶段写入。

## 编译与部署

```bash
cd /home/cfr/linux/Linux_Drivers/20_uvc_camera
make
make deploy
```

`make deploy` 会把程序复制到 `/home/cfr/linux/nfs/rootfs/tmp/uvc_fb_preview`。由于开发板启动后 `/tmp` 会被 `tmpfs` 覆盖，正式使用前应运行安装脚本把程序和运行包复制到 `/root`：

```bash
cd /home/cfr/linux/Linux_Drivers/20_uvc_camera
./install_uvc_autostart.sh
```

开发板通过 NFS 启动后手动执行：

```bash
/root/uvc_fb_preview -d /dev/video0 -f /dev/fb0 -w 640 -h 480
```

低 CPU 原型参数：

```bash
/root/uvc_fb_preview -d /dev/video0 -f /dev/fb0 -w 640 -h 480 -r 15 -p
```

| 参数 | 作用 |
|---|---|
| `-r 15` | 请求摄像头输出 `15fps`，便于和 GStreamer/KMS 路线同条件对比 |
| `-p` | 原始 `640x480` 居中绘制，不做全屏等比缩放，用于验证缩放开销是否是 CPU 瓶颈 |

## 官方 GStreamer 路线

Buildroot 中启用 `v4l2-ctl`、`gst-launch-1.0`、`v4l2src`、`videoconvert`、`fbdevsink` 后，可按正点原子快速体验手册的思路测试。当前虚拟机已生成 `/home/cfr/linux/buildroot/buildroot-2020.02.6/output-uvc`，先临时解包到 NFS rootfs 的 `/tmp/uvc-rootfs`，再通过 `install_uvc_autostart.sh` 安装到开发板启动后可见的 `/root/uvc-rootfs`。

配置片段保存在：

```bash
/home/cfr/linux/Linux_Drivers/20_uvc_camera/buildroot_uvc_gstreamer.fragment
```

开发板上先加载运行环境：

```bash
. /root/uvc-rootfs/uvc_runtime_env.sh
gst-inspect-1.0 v4l2src
gst-inspect-1.0 fbdevsink
v4l2-ctl -d /dev/video0 --list-formats-ext
```

raw 模式显示：

```bash
/root/uvc-rootfs/run_gst_fbdev.sh raw /dev/video0 /dev/fb0 640 480
```

MJPEG 模式显示：

```bash
/root/uvc-rootfs/run_gst_fbdev.sh mjpeg /dev/video0 /dev/fb0 640 480
```

## 开机自启

安装脚本会把 `S90uvc-camera` 放到 NFS rootfs 的 `/etc/init.d/`。Qt 部署脚本还会把 `S05display-quiet` 放到同一目录。开发板启动时，Buildroot 的 `rcS` 会按文件名顺序先执行 `S05display-quiet start`，再执行 `S90uvc-camera start`，脚本会自动：

| 步骤 | 作用 |
|---|---|
| `S05display-quiet` | 关闭 fbcon 光标并清理 LCD 当前虚拟终端残留文本 |
| `fb_boot_splash` | 在 `/dev/fb0` 可用后写入启动动画第一帧风格的静态图，填补 Qt 出画前的黑屏 |
| `modprobe galcore` | 自动加载 GPU 内核模块，生成 `/dev/galcore` |
| 等待 `/dev/fb0` | 确认 RGB framebuffer 屏幕可用 |
| 等待 `/dev/video0` | 确认 UVC 摄像头枚举完成 |
| 启动 Qt + KMS overlay | 默认进入 Qt 工业界面；overlay 视频层启动时先隐藏，Qt splash 完成后再恢复摄像头画面 |

手动控制命令：

```bash
/etc/init.d/S90uvc-camera start
/etc/init.d/S90uvc-camera stop
/etc/init.d/S90uvc-camera restart
/etc/init.d/S90uvc-camera status
```

## Qt GPU 界面

如果要从 CPU framebuffer 预览切换到 Qt 界面，使用：

```bash
cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display
./build_qt_camera_display.sh
./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs
```

开发板上运行：

```bash
/root/qt_camera_display/run_qt_camera_display.sh
```

这个脚本会先停止旧的 `/root/uvc_fb_preview`，再加载 `galcore`，并使用 `QT_QPA_PLATFORM=eglfs`、`QT_OPENGL=es2` 启动 Qt Quick 界面。当前 Qt 界面合成会走 GPU；摄像头预览使用自定义 V4L2 安全路径，不再使用 QtMultimedia `Camera + VideoOutput`。

## Qt + KMS overlay 正式链路

当前已把临时 `/tmp/uvc_kms_probe_overlay_rect` 探针整理为项目内源码和板端控制入口：

| 文件 | 作用 |
|---|---|
| `qt_camera_display/uvc_kms_overlay.c` | V4L2 mmap 采集、NEON `YUYV -> ARGB8888` 转换、DRM overlay plane 显示 |
| `qt_camera_display/build_uvc_kms_overlay.sh` | 在 VM 中交叉编译 `build-mp157/uvc_kms_overlay` |
| `qt_camera_display/run_qt_kms_overlay_display.sh` | 板端 `start/stop/restart/status/restore-fallback` 控制脚本 |
| `qt_camera_display/test_qt_kms_overlay_assets.sh` | 静态检查 overlay 源码、构建脚本和控制脚本的关键契约 |

VM 构建：

```bash
cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display
./test_qt_kms_overlay_assets.sh
./build_uvc_kms_overlay.sh
./build_qt_camera_display.sh
```

板端启动正式 overlay+Qt 链路：

```bash
/root/qt_camera_display/run_qt_kms_overlay_display.sh restart
/root/qt_camera_display/run_qt_kms_overlay_display.sh status
```

默认参数为 `640x480@10fps`、overlay plane `36`、矩形 `177,73,640,480`。脚本内部所有显示进程都通过 `nohup ... >/tmp/<route>.log 2>&1 < /dev/null &` 后台运行；启动失败时会恢复 `640x480@10fps -> drop6 -> BGRA -> kmssink` 可见线。

当前记录：

| 路线 | 实测结果 |
|---|---|
| 旧 `/root/uvc_fb_preview` | 约 `44%~49% CPU` |
| `/root/uvc_fb_preview -r 15 -p` 原始尺寸原型 | 约 `21.6% CPU`，不如 fast KMS |
| Qt 自定义 V4L2 安全预览，`640x480@15fps` | 约 `42.0% CPU` |
| Qt 自定义 V4L2 安全预览，`320x240@10fps` | 约 `5.9% CPU`，但画质较差 |
| Qt EGLFS + KMS overlay，`640x480@10fps` | 项目正式二进制运行到 `frames=5700`；两次 10 秒样本 overlay `13.5%/13.6%`，Qt `2.3%/2.5%`；已确认画面融合正常，无新增 crash 关键字 |

其中 Qt 安全预览的低 CPU 来自降分辨率和降帧率；KMS overlay 链路已经打通 UI+视频共存，但仍是 `640x480@10fps` 的 RGB 转换方案，不能宣称 `15fps` 目标完成。后续继续验证更高帧率的零拷贝/硬件视频显示链路，详见：

```bash
20_uvc_camera/qt_camera_display/zero_copy_hardware_video_plan.md
```
