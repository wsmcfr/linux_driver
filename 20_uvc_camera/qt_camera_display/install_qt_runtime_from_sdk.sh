#!/bin/sh
#
# 作用：
#   从 ST OpenSTLinux Qt/Wayland SDK 的目标 sysroot 中提取 Qt5 运行库、
#   Qt Multimedia 插件、eglfs/wayland 平台插件、QML 模块和 EGL/GLES 用户态库，
#   安装到 STM32MP157 的 NFS rootfs。
#
# 使用：
#   cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display
#   ./install_qt_runtime_from_sdk.sh /home/cfr/linux/nfs/rootfs
#
# 说明：
#   这个脚本用于当前 Buildroot rootfs 尚未完整打入 Qt5 时的快速落地。
#   长期量产版本仍建议在 Buildroot defconfig 中持久启用 Qt5/gcnano/GStreamer。

# 遇到未处理错误立即退出；source SDK 前不启用 set -u，避免 SDK 读取空环境变量时报错。
set -e

# ROOTFS 是开发板 NFS 根文件系统路径。
ROOTFS="${1:-/home/cfr/linux/nfs/rootfs}"

# SDK_ENV 是 ST Qt/Wayland SDK 的环境初始化脚本。
SDK_ENV="${SDK_ENV:-/opt/st/stm32mp1/3.1-snapshot/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi}"

# SCRIPT_DIR 是当前脚本所在目录，用于定位运行脚本。
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

# 检查 SDK 环境脚本是否存在。
if [ ! -f "$SDK_ENV" ]; then
    echo "错误：找不到 SDK 环境脚本：$SDK_ENV" >&2
    exit 1
fi

# 检查 rootfs 是否存在，避免把文件复制到错误路径。
if [ ! -d "$ROOTFS" ]; then
    echo "错误：NFS rootfs 不存在：$ROOTFS" >&2
    exit 1
fi

# 加载 SDK 后得到 OECORE_TARGET_SYSROOT。
. "$SDK_ENV"

# SDK 加载完成后再启用未定义变量检查。
set -u

# SYSROOT 是 SDK 提供的 ARM 目标根目录，里面有 Qt/GStreamer/EGL 运行库。
SYSROOT="$OECORE_TARGET_SYSROOT"

# SUDO_CMD 使用 sudo -S，允许远程非交互 SSH 场景从标准输入读取密码。
SUDO_CMD="${SUDO_CMD:-sudo -S}"

# copy_runtime_font 的作用：
#   把一个字体文件复制到目标 rootfs 的指定字体子目录。
#
# 参数：
#   $1 是源字体文件路径，可以来自 SDK sysroot，也可以来自虚拟机本机 /usr/share/fonts。
#   $2 是目标 rootfs 中 /usr/share/fonts/ 下的相对目录。
#
# 返回值：
#   源字体存在并复制成功返回 0；源字体不存在返回 1，方便调用方继续尝试下一个候选。
copy_runtime_font()
{
    # src 保存待复制字体路径；字体是架构无关数据，可从 x86 虚拟机复制到 ARM rootfs 使用。
    src="$1"

    # rel_dir 保存目标字体分类目录，例如 opentype/noto 或 truetype/dejavu。
    rel_dir="$2"

    # 源字体不存在时返回 1，让外层循环继续尝试其它字体。
    if [ ! -f "$src" ]; then
        return 1
    fi

    # 创建目标目录，rootfs 通常归 root 所有，因此使用 sudo。
    $SUDO_CMD mkdir -p "$ROOTFS/usr/share/fonts/$rel_dir"

    # 复制字体文件；保留文件时间戳，方便后续排查 rootfs 内容来源。
    $SUDO_CMD cp -p "$src" "$ROOTFS/usr/share/fonts/$rel_dir/"

    # 输出安装结果，部署日志里能直接看到最终使用了哪一个字体。
    echo "  OK: 安装字体 $(basename "$src") -> /usr/share/fonts/$rel_dir/"

    return 0
}

# install_font_runtime 的作用：
#   为精简 rootfs 补齐 fontconfig 配置和至少一个可显示中文的字体。
#
# 主要流程：
#   1. 优先复制 SDK sysroot 的 fontconfig 配置；SDK 没有时复制虚拟机本机配置。
#   2. 复制 /usr/share/fontconfig，保证 /etc/fonts/conf.d 中的规则链接能解析。
#   3. 优先安装 NotoSansCJK-Regular.ttc；没有时退回 DroidSansFallbackFull.ttf。
#   4. 额外安装一个拉丁字体兜底，避免英文数字回退异常。
#
# 参数：
#   无，直接使用 ROOTFS、SYSROOT 和 SUDO_CMD。
#
# 返回值：
#   无返回值；缺少 CJK 字体时会打印警告，但不中断 Qt runtime 安装。
install_font_runtime()
{
    # 创建 fontconfig 和字体目录；/var/cache/fontconfig 用于运行时缓存。
    $SUDO_CMD mkdir -p "$ROOTFS/etc"
    $SUDO_CMD mkdir -p "$ROOTFS/usr/share"
    $SUDO_CMD mkdir -p "$ROOTFS/var/cache/fontconfig"

    # 优先复制 SDK 自带 /etc/fonts；部分 ST SDK 精简包没有该目录，因此需要主机兜底。
    if [ -d "$SYSROOT/etc/fonts" ]; then
        $SUDO_CMD rsync -a "$SYSROOT/etc/fonts/" "$ROOTFS/etc/fonts/"
    elif [ -d /etc/fonts ]; then
        $SUDO_CMD rsync -a /etc/fonts/ "$ROOTFS/etc/fonts/"
    fi

    # 若前两步仍没有 fonts.conf，则生成一个最小配置，至少让 Qt 能扫描 /usr/share/fonts。
    if [ ! -f "$ROOTFS/etc/fonts/fonts.conf" ]; then
        tmp_fonts_conf="${TMPDIR:-/tmp}/qt-camera-fonts-conf.$$"
        cat > "$tmp_fonts_conf" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>/usr/share/fonts</dir>
  <cachedir>/var/cache/fontconfig</cachedir>
  <config></config>
</fontconfig>
EOF
        $SUDO_CMD mkdir -p "$ROOTFS/etc/fonts"
        $SUDO_CMD cp "$tmp_fonts_conf" "$ROOTFS/etc/fonts/fonts.conf"
        rm -f "$tmp_fonts_conf"
    fi

    # /etc/fonts/conf.d 里的规则可能引用 /usr/share/fontconfig/conf.avail，因此一起复制。
    if [ -d "$SYSROOT/usr/share/fontconfig" ]; then
        $SUDO_CMD rsync -a "$SYSROOT/usr/share/fontconfig/" "$ROOTFS/usr/share/fontconfig/"
    elif [ -d /usr/share/fontconfig ]; then
        $SUDO_CMD rsync -a /usr/share/fontconfig/ "$ROOTFS/usr/share/fontconfig/"
    fi

    # cjk_installed 记录是否已经放入中文字体；中文界面依赖这个字体正确显示。
    cjk_installed=0

    # 按优先级选择中文字体：Noto Sans CJK 更适合 UI，DroidSansFallback 作为兜底。
    for font in \
        "$SYSROOT/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc" \
        "$SYSROOT/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf" \
        /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc \
        /usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf
    do
        if copy_runtime_font "$font" "opentype/noto"; then
            cjk_installed=1
            break
        fi
    done

    # 安装一个常见拉丁字体；即使 CJK 字体覆盖英文，这个兜底也能提升兼容性。
    for font in \
        "$SYSROOT/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" \
        "$SYSROOT/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf" \
        /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
        /usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf \
        /usr/share/fonts/truetype/noto/NotoMono-Regular.ttf
    do
        if copy_runtime_font "$font" "truetype/ui"; then
            break
        fi
    done

    # 没找到中文字体时继续安装其它 Qt runtime，但明确提示界面中文仍可能不显示。
    if [ "$cjk_installed" != "1" ]; then
        echo "警告：没有找到 Noto/Droid 中文字体，Qt 界面中文可能仍无法显示。" >&2
    fi
}

# 检查关键 Qt 文件是否存在，避免复制一个不完整的 SDK。
for file in \
    usr/lib/libQt5Core.so.5 \
    usr/lib/libQt5Quick.so.5 \
    usr/lib/libQt5Multimedia.so.5 \
    usr/lib/plugins/platforms/libqeglfs.so \
    usr/lib/plugins/mediaservice/libgstcamerabin.so
do
    if [ ! -e "$SYSROOT/$file" ]; then
        echo "错误：SDK sysroot 缺少关键文件：$file" >&2
        exit 1
    fi
done

# 创建目标目录；rootfs 中这些目录通常归 root 所有，因此使用 sudo。
$SUDO_CMD mkdir -p "$ROOTFS/usr/lib"
$SUDO_CMD mkdir -p "$ROOTFS/usr/bin"
$SUDO_CMD mkdir -p "$ROOTFS/usr/share"
$SUDO_CMD mkdir -p "$ROOTFS/usr/lib/plugins"
$SUDO_CMD mkdir -p "$ROOTFS/usr/lib/qml"
$SUDO_CMD mkdir -p "$ROOTFS/usr/lib/gstreamer-1.0"
$SUDO_CMD mkdir -p "$ROOTFS/usr/lib/pulseaudio"
$SUDO_CMD mkdir -p "$ROOTFS/usr/lib/alsa-lib"
$SUDO_CMD mkdir -p "$ROOTFS/usr/libexec"
$SUDO_CMD mkdir -p "$ROOTFS/vendor/lib"
$SUDO_CMD mkdir -p "$ROOTFS/root/qt_camera_display"

# 安装 fontconfig 配置和 UI 字体，解决板端 Fontconfig 缺配置导致文字不显示的问题。
install_font_runtime

# 复制 Qt5 动态库；使用 -a 保留符号链接，避免 so 链接断裂。
$SUDO_CMD rsync -a "$SYSROOT/usr/lib"/libQt5*.so* "$ROOTFS/usr/lib/"

# 复制 Qt 运行依赖中常见的图形、多媒体、GLib、字体和 Wayland/EGL/GLES 库。
$SUDO_CMD rsync -a \
    "$SYSROOT/usr/lib"/libEGL*.so* \
    "$SYSROOT/usr/lib"/libGLES*.so* \
    "$SYSROOT/usr/lib"/libGAL*.so* \
    "$SYSROOT/usr/lib"/libgbm*.so* \
    "$SYSROOT/usr/lib"/libdrm*.so* \
    "$SYSROOT/usr/lib"/libwayland*.so* \
    "$SYSROOT/usr/lib"/libxkbcommon*.so* \
    "$SYSROOT/usr/lib"/libinput*.so* \
    "$SYSROOT/usr/lib"/libgudev*.so* \
    "$SYSROOT/usr/lib"/libevdev*.so* \
    "$SYSROOT/usr/lib"/libmtdev*.so* \
    "$SYSROOT/usr/lib"/libts*.so* \
    "$SYSROOT/usr/lib"/libdbus-1*.so* \
    "$SYSROOT/usr/lib"/liblzma*.so* \
    "$SYSROOT/usr/lib"/libasound*.so* \
    "$SYSROOT/usr/lib"/liborc*.so* \
    "$SYSROOT/usr/lib"/libpulse*.so* \
    "$SYSROOT/usr/lib"/libsndfile*.so* \
    "$SYSROOT/usr/lib"/libFLAC*.so* \
    "$SYSROOT/usr/lib"/libogg*.so* \
    "$SYSROOT/usr/lib"/libvorbis*.so* \
    "$SYSROOT/usr/lib"/libtiff*.so* \
    "$SYSROOT/usr/lib"/libwebp*.so* \
    "$SYSROOT/usr/lib"/libX*.so* \
    "$SYSROOT/usr/lib"/libxcb*.so* \
    "$SYSROOT/usr/lib"/libICE*.so* \
    "$SYSROOT/usr/lib"/libSM*.so* \
    "$SYSROOT/usr/lib"/libstdc++*.so* \
    "$SYSROOT/usr/lib"/libffi*.so* \
    "$SYSROOT/usr/lib"/libglib-2.0*.so* \
    "$SYSROOT/usr/lib"/libgobject-2.0*.so* \
    "$SYSROOT/usr/lib"/libgmodule-2.0*.so* \
    "$SYSROOT/usr/lib"/libgio-2.0*.so* \
    "$SYSROOT/usr/lib"/libpcre*.so* \
    "$SYSROOT/usr/lib"/libpng*.so* \
    "$SYSROOT/usr/lib"/libjpeg*.so* \
    "$SYSROOT/usr/lib"/libfreetype*.so* \
    "$SYSROOT/usr/lib"/libfontconfig*.so* \
    "$SYSROOT/usr/lib"/libexpat*.so* \
    "$SYSROOT/usr/lib"/libharfbuzz*.so* \
    "$ROOTFS/usr/lib/" 2>/dev/null || true

# 复制部分位于 sysroot /lib 下的基础运行库，例如 libgcc_s 和 libudev。
$SUDO_CMD rsync -a \
    "$SYSROOT/lib"/libgcc_s*.so* \
    "$SYSROOT/lib"/libudev*.so* \
    "$SYSROOT/lib"/libuuid*.so* \
    "$SYSROOT/lib"/libmount*.so* \
    "$SYSROOT/lib"/libblkid*.so* \
    "$SYSROOT/lib"/libsystemd*.so* \
    "$SYSROOT/lib"/libcap*.so* \
    "$ROOTFS/usr/lib/" 2>/dev/null || true

# 复制 GStreamer 运行库；Qt Multimedia 的 camerabin 后端会依赖这些库。
$SUDO_CMD rsync -a "$SYSROOT/usr/lib"/libgst*.so* "$ROOTFS/usr/lib/" 2>/dev/null || true
$SUDO_CMD rsync -a "$SYSROOT/usr/lib"/libgstreamer-1.0*.so* "$ROOTFS/usr/lib/" 2>/dev/null || true
$SUDO_CMD rsync -a "$SYSROOT/usr/lib"/liborc*.so* "$ROOTFS/usr/lib/" 2>/dev/null || true

# 复制 Vivante/Nano GPU 用户态真实库；usr/lib 下的 EGL/GLES 符号链接会指向这里。
if [ -d "$SYSROOT/vendor/lib" ]; then
    $SUDO_CMD rsync -a "$SYSROOT/vendor/lib/" "$ROOTFS/vendor/lib/"
fi

# 复制 PulseAudio 私有公共库；libpulse.so.0 通过 SONAME 依赖这里的 libpulsecommon-13.0.so。
if [ -d "$SYSROOT/usr/lib/pulseaudio" ]; then
    $SUDO_CMD rsync -a "$SYSROOT/usr/lib/pulseaudio/" "$ROOTFS/usr/lib/pulseaudio/"
fi

# 复制 ALSA 运行时组件；QtMultimedia 的 GStreamer camera service 会间接依赖 libasound.so.2。
# SDK 没有 /usr/share/alsa 时使用虚拟机本机配置兜底，因为这些配置文件是架构无关文本数据。
if [ -d "$SYSROOT/usr/lib/alsa-lib" ]; then
    $SUDO_CMD rsync -a "$SYSROOT/usr/lib/alsa-lib/" "$ROOTFS/usr/lib/alsa-lib/"
fi

if [ -d "$SYSROOT/usr/share/alsa" ]; then
    $SUDO_CMD rsync -a "$SYSROOT/usr/share/alsa/" "$ROOTFS/usr/share/alsa/"
elif [ -d /usr/share/alsa ]; then
    $SUDO_CMD rsync -a /usr/share/alsa/ "$ROOTFS/usr/share/alsa/"
fi

# 复制 Qt 插件和 QML 模块；平台插件、mediaservice、egldeviceintegration 都在这里。
$SUDO_CMD rsync -a "$SYSROOT/usr/lib/plugins/" "$ROOTFS/usr/lib/plugins/"
$SUDO_CMD rsync -a "$SYSROOT/usr/lib/qml/" "$ROOTFS/usr/lib/qml/"

# 复制 GStreamer 插件；如果 rootfs 已经有 Buildroot 插件，该步骤会补齐 SDK 中的插件。
if [ -d "$SYSROOT/usr/lib/gstreamer-1.0" ]; then
    $SUDO_CMD rsync -a "$SYSROOT/usr/lib/gstreamer-1.0/" "$ROOTFS/usr/lib/gstreamer-1.0/"
fi

# 复制 libexec，例如 gst-plugin-scanner；不存在时跳过。
if [ -d "$SYSROOT/usr/libexec" ]; then
    $SUDO_CMD rsync -a "$SYSROOT/usr/libexec/" "$ROOTFS/usr/libexec/"
fi

# 安装最新 Qt 摄像头启动脚本，确保运行时搜索路径正确。
$SUDO_CMD cp "$SCRIPT_DIR/run_qt_camera_display.sh" "$ROOTFS/root/qt_camera_display/run_qt_camera_display.sh"
$SUDO_CMD chmod 755 "$ROOTFS/root/qt_camera_display/run_qt_camera_display.sh"

# 打印关键文件检查结果，便于部署后立即确认。
echo "Qt runtime 安装完成，关键文件："
for file in \
    usr/lib/libQt5Core.so.5 \
    usr/lib/libQt5Quick.so.5 \
    usr/lib/libQt5Multimedia.so.5 \
    usr/lib/plugins/platforms/libqeglfs.so \
    usr/lib/plugins/platforms/libqwayland-egl.so \
    usr/lib/plugins/mediaservice/libgstcamerabin.so \
    usr/lib/libasound.so.2 \
    usr/lib/liborc-0.4.so.0 \
    usr/lib/libgudev-1.0.so.0 \
    usr/lib/libmount.so.1 \
    usr/lib/libblkid.so.1 \
    etc/fonts/fonts.conf
do
    if [ -e "$ROOTFS/$file" ]; then
        echo "  OK: /$file"
    else
        echo "  MISSING: /$file"
    fi
done
