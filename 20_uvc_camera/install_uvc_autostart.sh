#!/bin/sh
#
# 作用：
#   在虚拟机中把 UVC 摄像头显示程序、GStreamer 运行包和开机自启脚本
#   安装到 STM32MP157 的 NFS rootfs 持久目录。
#
# 使用：
#   cd /home/cfr/linux/Linux_Drivers/20_uvc_camera
#   ./install_uvc_autostart.sh
#
# 参数：
#   $1：可选，NFS rootfs 路径，默认 /home/cfr/linux/nfs/rootfs。

# 遇到未处理错误立即退出，避免半安装状态被误认为成功。
set -eu

# ROOTFS 是开发板 NFS 根文件系统路径。
ROOTFS="${1:-/home/cfr/linux/nfs/rootfs}"

# KERNEL_RELEASE 是开发板当前内核版本，也是 /lib/modules 下的目录名。
KERNEL_RELEASE="${KERNEL_RELEASE:-5.4.31}"

# SCRIPT_DIR 是本安装脚本所在目录，用于找到同目录的 S90uvc-camera。
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

# UVC_TMP_ROOT 是之前临时生成的 UVC/GStreamer 运行包目录。
UVC_TMP_ROOT="$ROOTFS/tmp/uvc-rootfs"

# PREVIEW_TMP_BIN 是之前临时生成的轻量 framebuffer 预览程序。
PREVIEW_TMP_BIN="$ROOTFS/tmp/uvc_fb_preview"

# GALCORE_TMP_KO 是之前临时放在 /tmp 的 galcore 模块，若存在则安装到标准模块目录。
GALCORE_TMP_KO="$ROOTFS/tmp/galcore.ko"

# GALCORE_DST_DIR 是 galcore.ko 的标准模块目录，便于 modprobe 按 modules.dep 查找。
GALCORE_DST_DIR="$ROOTFS/lib/modules/$KERNEL_RELEASE/kernel/drivers/gpu/drm/gcnano-driver-6.4.3"

# INIT_SCRIPT_DST 是 Buildroot SysV init 会自动调用的启动脚本路径。
INIT_SCRIPT_DST="$ROOTFS/etc/init.d/S90uvc-camera"

# 检查 rootfs 目录是否存在，避免 sudo cp 写到错误路径。
if [ ! -d "$ROOTFS" ]; then
    echo "错误：NFS rootfs 不存在：$ROOTFS" >&2
    exit 1
fi

# 检查开机脚本是否在当前目录，避免安装空文件。
if [ ! -f "$SCRIPT_DIR/S90uvc-camera" ]; then
    echo "错误：找不到 $SCRIPT_DIR/S90uvc-camera" >&2
    exit 1
fi

# 检查临时 UVC 运行包是否存在；它来自 Buildroot output-uvc 的临时部署。
if [ ! -d "$UVC_TMP_ROOT" ]; then
    echo "错误：找不到 UVC 运行包：$UVC_TMP_ROOT" >&2
    echo "请先确认 /home/cfr/linux/nfs/rootfs/tmp/uvc-rootfs 是否存在。" >&2
    exit 1
fi

# 检查轻量预览程序是否存在；开机默认使用它显示到 /dev/fb0。
if [ ! -f "$PREVIEW_TMP_BIN" ]; then
    echo "错误：找不到预览程序：$PREVIEW_TMP_BIN" >&2
    exit 1
fi

# 创建持久目录；/root 不会像 /tmp 一样被 tmpfs 覆盖。
sudo mkdir -p "$ROOTFS/root/uvc-rootfs"
sudo mkdir -p "$ROOTFS/etc/init.d"

# 复制 GStreamer/libv4l 运行包到 /root/uvc-rootfs，供手动或脚本使用。
sudo cp -a "$UVC_TMP_ROOT/." "$ROOTFS/root/uvc-rootfs/"

# 复制轻量 framebuffer 预览程序到 /root。
sudo cp "$PREVIEW_TMP_BIN" "$ROOTFS/root/uvc_fb_preview"

# 安装 SysV init 自启动脚本。
sudo cp "$SCRIPT_DIR/S90uvc-camera" "$INIT_SCRIPT_DST"

# 设置可执行权限，确保 init、手动测试和脚本调用都能运行。
sudo chmod 755 "$ROOTFS/root/uvc_fb_preview"
sudo chmod 755 "$ROOTFS/root/uvc-rootfs/run_gst_fbdev.sh"
sudo chmod 755 "$ROOTFS/root/uvc-rootfs/uvc_runtime_env.sh"
sudo chmod 755 "$INIT_SCRIPT_DST"

# 如果临时 galcore.ko 还没有进入标准模块目录，则一并安装，方便 modprobe galcore。
if [ -f "$GALCORE_TMP_KO" ]; then
    sudo mkdir -p "$GALCORE_DST_DIR"
    sudo cp "$GALCORE_TMP_KO" "$GALCORE_DST_DIR/galcore.ko"
    sudo chmod 644 "$GALCORE_DST_DIR/galcore.ko"
fi

# 更新模块依赖表；缺 modules.order/modules.builtin 时 depmod 可能警告，但 modules.dep 会正常生成。
sudo depmod -b "$ROOTFS" "$KERNEL_RELEASE" || true

# 打印安装结果，方便用户对照开发板上的路径。
echo "安装完成："
echo "  自启动脚本：$INIT_SCRIPT_DST"
echo "  预览程序：$ROOTFS/root/uvc_fb_preview"
echo "  运行包：$ROOTFS/root/uvc-rootfs"
echo "  GPU 模块目录：$GALCORE_DST_DIR"
echo
echo "开发板重启后会自动执行：/etc/init.d/S90uvc-camera start"
echo "也可以手动执行：/etc/init.d/S90uvc-camera restart"
