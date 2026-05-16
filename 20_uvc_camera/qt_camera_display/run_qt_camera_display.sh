#!/bin/sh
#
# 作用：
#   在 STM32MP157 开发板上启动摄像头显示。
#   默认启动 Qt Quick 工业检测界面的安全预览路径；
#   当 VIDEO_BACKEND=gst-gl 时，启动已验证的 GStreamer GL 独立硬件视频路径，
#   用于快速观察 “V4L2 DMABUF -> glupload -> glimagesink” 的画质和 CPU 效果。
#   当 VIDEO_BACKEND=qt-gst 时，启动 Qt Quick 工业检测界面，并把
#   “V4L2 DMABUF -> glupload -> qmlglsink” 嵌入到界面画面区。
#   当 VIDEO_BACKEND=kms-overlay 时，只启动 Qt Quick 界面壳，摄像头画面由
#   外部 uvc_kms_overlay 辅助进程通过 KMS overlay plane 显示。
#
# 使用：
#   /root/qt_camera_display/run_qt_camera_display.sh
#
# 可调变量：
#   VIDEO_BACKEND：显示后端，qt-safe 表示当前 Qt 安全预览，gst-gl 表示 GStreamer GL 独立预览，qt-gst 表示 Qt 内嵌 qmlglsink，kms-overlay 表示外部 KMS overlay 视频；
#   GST_IO_MODE：GStreamer v4l2src 的 io-mode；qt-gst 默认 mmap，gst-gl 默认 dmabuf；
#   VIDEO_DEV：UVC 摄像头节点，默认自动从 /dev/video0 开始选择；
#   CAMERA_WIDTH/CAMERA_HEIGHT/CAMERA_FPS：V4L2 采集参数，Qt 默认 320x240@10fps，gst-gl 默认 640x480@15fps；
#   QT_QPA_PLATFORM：Qt 平台插件，默认 eglfs，可改为 wayland；
#   STOP_OLD_PREVIEW：是否停止旧的 uvc_fb_preview，默认 1。

# 遇到未处理错误立即退出，避免 Qt 在不完整环境中黑屏运行。
set -eu

# PATH 增加常见系统目录，保证 modprobe、killall 等命令可找到。
PATH=/sbin:/bin:/usr/sbin:/usr/bin:$PATH

# SCRIPT_DIR 是当前脚本所在目录，默认程序也放在这个目录。
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

# APP_BIN 是 Qt 可执行文件路径，部署脚本默认放在 /root/qt_camera_display 下。
APP_BIN="${APP_BIN:-$SCRIPT_DIR/qt_camera_display}"

# STOP_OLD_PREVIEW 控制是否先停掉旧 CPU framebuffer 预览。
STOP_OLD_PREVIEW="${STOP_OLD_PREVIEW:-1}"

# VIDEO_BACKEND 控制最终启动哪条显示链路：
# qt-safe 是当前稳定 Qt Quick + 自定义 V4L2VideoItem 安全预览；
# gst-gl 是本轮已验证的低 CPU GStreamer GL 独立显示路径；
# qt-gst 是把同一条 GL 路线通过 qmlglsink 嵌入 Qt Quick 的集成路径；
# kms-overlay 是只启动 Qt UI 的集成壳，视频由外部 KMS plane 进程负责。
VIDEO_BACKEND="${VIDEO_BACKEND:-qt-safe}"

# GST_IO_MODE 控制 GStreamer 后端的 V4L2 取帧模式。
# gst-gl 继续默认 dmabuf，因为独立 glimagesink 已经验证 10 分钟稳定且 CPU 约 2%；
# qt-gst 默认 mmap，因为板端实测 dmabuf 嵌入 qmlglsink 会在 Vivante libGAL 用户态段错误。
if [ "$VIDEO_BACKEND" = "qt-gst" ]; then
    GST_IO_MODE="${GST_IO_MODE:-mmap}"
else
    GST_IO_MODE="${GST_IO_MODE:-dmabuf}"
fi

# VIDEO_DEV 允许用户指定摄像头；未指定时由 pick_video_dev 自动选择。
VIDEO_DEV="${VIDEO_DEV:-}"

# CAMERA_WIDTH/CAMERA_HEIGHT/CAMERA_FPS 控制 V4L2 采集参数。
# Qt 安全预览默认低分辨率，避免现有 CPU 拷贝和 glTexImage2D 上传造成高负载；
# gst-gl/qt-gst 模式默认使用 640x480@15fps，用来观察高清硬件视频路径效果。
# kms-overlay 使用已验证稳定的 640x480@10fps，与外部 uvc_kms_overlay 默认参数保持一致。
if [ "$VIDEO_BACKEND" = "kms-overlay" ]; then
    CAMERA_WIDTH="${CAMERA_WIDTH:-640}"
    CAMERA_HEIGHT="${CAMERA_HEIGHT:-480}"
    CAMERA_FPS="${CAMERA_FPS:-10}"
elif [ "$VIDEO_BACKEND" = "gst-gl" ] || [ "$VIDEO_BACKEND" = "qt-gst" ]; then
    CAMERA_WIDTH="${CAMERA_WIDTH:-640}"
    CAMERA_HEIGHT="${CAMERA_HEIGHT:-480}"
    CAMERA_FPS="${CAMERA_FPS:-15}"
else
    CAMERA_WIDTH="${CAMERA_WIDTH:-320}"
    CAMERA_HEIGHT="${CAMERA_HEIGHT:-240}"
    CAMERA_FPS="${CAMERA_FPS:-10}"
fi

# QT_QPA_PLATFORM 默认使用 eglfs 直连 EGL/GPU；有 Weston 时可设为 wayland。
QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-eglfs}"
export QT_QPA_PLATFORM

# TZ 默认使用北京时间，保证 QML 顶部 new Date() 和保存上传业务时间显示一致。
# POSIX TZ 规则里 CST-8 表示 UTC+8；用户显式传入 TZ 时保持用户设置，方便临时排障。
TZ="${TZ:-CST-8}"
export TZ

# LD_LIBRARY_PATH 增加常见目标库目录；/vendor/lib 保存 Vivante EGL/GLES 真实库文件。
# /usr/lib/pulseaudio 保存 libpulsecommon-13.0.so，QtMultimedia 会间接依赖它。
export LD_LIBRARY_PATH="/usr/lib:/usr/lib/pulseaudio:/vendor/lib:/lib:${LD_LIBRARY_PATH:-}"

# ALSA_CONFIG_PATH/ALSA_PLUGIN_DIR 指向 rootfs 中的 ALSA 配置和插件目录。
# QtMultimedia 的 GStreamer camera service 即使只做预览，也可能间接加载 ALSA。
export ALSA_CONFIG_PATH="${ALSA_CONFIG_PATH:-/usr/share/alsa/alsa.conf}"
export ALSA_PLUGIN_DIR="${ALSA_PLUGIN_DIR:-/usr/lib/alsa-lib}"

# XDG_RUNTIME_DIR 是 Qt/eglfs 和部分多媒体组件保存运行时 socket/cache 的目录。
# 精简 rootfs 通常没有桌面环境负责创建它，因此这里显式创建并限制权限。
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/runtime-root}"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null || true

# QT_PLUGIN_PATH 同时覆盖 ST SDK 布局和 Buildroot Qt 布局，确保能找到 eglfs/wayland 平台插件。
export QT_PLUGIN_PATH="${QT_PLUGIN_PATH:-/usr/lib/plugins:/usr/lib/qt/plugins}"

# QML2_IMPORT_PATH 同时覆盖 ST SDK 的 /usr/lib/qml 和 Buildroot 的 /usr/qml。
export QML2_IMPORT_PATH="${QML2_IMPORT_PATH:-/usr/lib/qml:/usr/qml}"

# FONTCONFIG_PATH/FONTCONFIG_FILE 指向 rootfs 内的字体配置，避免 Qt 文本渲染时报
# "Fontconfig error: Cannot load default config file"。
export FONTCONFIG_PATH="${FONTCONFIG_PATH:-/etc/fonts}"
export FONTCONFIG_FILE="${FONTCONFIG_FILE:-/etc/fonts/fonts.conf}"

# QT_QPA_FONTDIR 是 Qt 的字体目录兜底路径；即使 fontconfig 缓存未生成，也能扫描字体文件。
export QT_QPA_FONTDIR="${QT_QPA_FONTDIR:-/usr/share/fonts}"

# GST_PLUGIN_PATH 指向目标 rootfs 的 GStreamer 插件目录，Qt Multimedia 会通过它找 v4l2/camerabin。
export GST_PLUGIN_PATH="${GST_PLUGIN_PATH:-/usr/lib/gstreamer-1.0}"

# GST_PLUGIN_PATH_1_0/GST_PLUGIN_SYSTEM_PATH_1_0 是 GStreamer 1.x 的显式插件路径。
# 精简 rootfs 从 SDK 叠加插件时，显式指定路径可避免使用编译期默认路径导致插件不可见。
export GST_PLUGIN_PATH_1_0="${GST_PLUGIN_PATH_1_0:-$GST_PLUGIN_PATH}"
export GST_PLUGIN_SYSTEM_PATH_1_0="${GST_PLUGIN_SYSTEM_PATH_1_0:-/usr/lib/gstreamer-1.0}"

# GST_PLUGIN_SCANNER 在存在时显式指定，避免精简 rootfs 中插件扫描器查找失败。
if [ -x /usr/libexec/gstreamer-1.0/gst-plugin-scanner ]; then
    export GST_PLUGIN_SCANNER="${GST_PLUGIN_SCANNER:-/usr/libexec/gstreamer-1.0/gst-plugin-scanner}"
fi

# GST_REGISTRY 放到 /tmp，允许只读或精简 rootfs 也能生成运行时插件缓存。
export GST_REGISTRY="${GST_REGISTRY:-/tmp/gst-registry-qt-camera.bin}"

# rootfs 叠加 Qt/GStreamer 插件后，旧 registry 可能记录了缺失或黑名单状态。
# 默认重建一次缓存，保证 camerabin/v4l2src 等新增插件能被 QtMultimedia 看到。
if [ "${GST_REGISTRY_REBUILD:-1}" = "1" ]; then
    rm -f "$GST_REGISTRY" 2>/dev/null || true
fi

# QT_QUICK_BACKEND 不默认设置；eglfs 下 Qt Quick 默认使用 OpenGL/GPU。
# 若强制设置为 opengl，Qt 5.12 可能会寻找不存在的 scenegraph 插件。
if [ -n "${QT_QUICK_BACKEND:-}" ]; then
    export QT_QUICK_BACKEND
else
    unset QT_QUICK_BACKEND
fi

# QT_OPENGL=es2 匹配 STM32MP157 的 OpenGL ES 驱动。
export QT_OPENGL="${QT_OPENGL:-es2}"

# QT_QUICK_NO_TEXTURE_VIDEOFRAMES 禁止 QtMultimedia 把摄像头帧按纹理句柄零拷贝交给场景图。
# 当前板端日志显示 libimx6vivantevideonode.so 会通过 glTexDirectVIVMap 进入 galcore，
# 并在 _UserMemoryAttach/dma_map_sg 路径触发内核 Oops；这里默认走更保守的帧拷贝路径。
export QT_QUICK_NO_TEXTURE_VIDEOFRAMES="${QT_QUICK_NO_TEXTURE_VIDEOFRAMES:-1}"

# QT_VIDEONODE 指定 Qt Quick 视频节点工厂；egl 比 imx6/vivante 直连节点更保守。
# 如果后续更新 galcore 后需要测试零拷贝，可临时运行：QT_QUICK_NO_TEXTURE_VIDEOFRAMES=0 QT_VIDEONODE=imx6 ...
export QT_VIDEONODE="${QT_VIDEONODE:-egl}"

# 默认允许 eglfs 读取 /dev/input/eventX，这样 7 寸触摸屏可以直接点击 QML 按钮。
# 如现场键盘/XKB 组件异常影响启动，可临时设置 QT_QPA_EGLFS_DISABLE_INPUT=1 回退到纯显示模式。
export QT_QPA_EGLFS_DISABLE_INPUT="${QT_QPA_EGLFS_DISABLE_INPUT:-0}"

# QSG_RENDER_LOOP=threaded 让渲染线程独立，降低 UI 阻塞概率。
export QSG_RENDER_LOOP="${QSG_RENDER_LOOP:-threaded}"

# QT_QPA_EGLFS_INTEGRATION 在 eglfs 下优先使用 Vivante 集成插件。
if [ "$QT_QPA_PLATFORM" = "eglfs" ]; then
    export QT_QPA_EGLFS_INTEGRATION="${QT_QPA_EGLFS_INTEGRATION:-eglfs_viv}"
    export QT_QPA_EGLFS_ALWAYS_SET_MODE="${QT_QPA_EGLFS_ALWAYS_SET_MODE:-1}"
    export QT_QPA_EGLFS_FORCE888="${QT_QPA_EGLFS_FORCE888:-1}"
fi

# log_msg 用于统一输出启动信息，方便串口/SSH 观察。
log_msg()
{
    echo "qt-camera-display: $*"
}

# load_gpu 的作用是加载 galcore 模块，并确认 /dev/galcore 已出现。
load_gpu()
{
    # /dev/galcore 存在说明 GPU 驱动已经可用。
    if [ -e /dev/galcore ]; then
        log_msg "galcore already available"
        return 0
    fi

    # 尝试通过 modprobe 加载 galcore，依赖 rootfs 中 modules.dep 正确。
    if modprobe galcore 2>/dev/null; then
        log_msg "galcore loaded"
    else
        log_msg "galcore modprobe failed"
    fi

    # 再次检查 /dev/galcore；GPU 不可用时直接失败，避免退回 CPU 软件渲染还误认为成功。
    if [ ! -e /dev/galcore ]; then
        log_msg "错误：/dev/galcore 不存在，Qt GPU 路径不可用"
        return 1
    fi

    return 0
}

# stop_old_preview 用于停止旧的预览进程，避免 CPU 高占用和摄像头/显示设备被占用。
stop_old_preview()
{
    # 用户显式设置 STOP_OLD_PREVIEW=0 时保留旧进程，便于特殊调试。
    if [ "$STOP_OLD_PREVIEW" != "1" ]; then
        return 0
    fi

    # 优先调用已有 init 脚本 stop，这样 PID 文件和日志状态也会同步清理。
    if [ -x /etc/init.d/S90uvc-camera ]; then
        /etc/init.d/S90uvc-camera stop >/dev/null 2>&1 || true
    fi

    # killall 作为兜底，处理手动启动的 /root/uvc_fb_preview。
    killall uvc_fb_preview >/dev/null 2>&1 || true

    # 停止上一轮手动启动的 Qt 预览，避免 /dev/video0 被旧进程占用。
    killall qt_camera_display >/dev/null 2>&1 || true

    # 停止上一轮手动启动的 GStreamer 预览，避免新后端无法独占显示面。
    killall gst-launch-1.0 >/dev/null 2>&1 || true
}

# pick_video_dev 在用户没有指定 VIDEO_DEV 时选择第一个存在的 /dev/videoN。
pick_video_dev()
{
    # 用户传入 VIDEO_DEV 时先验证该节点存在。
    if [ -n "$VIDEO_DEV" ]; then
        if [ -e "$VIDEO_DEV" ]; then
            echo "$VIDEO_DEV"
            return 0
        fi
        log_msg "错误：指定的摄像头节点不存在：$VIDEO_DEV" >&2
        return 1
    fi

    # 遍历常见 UVC 节点，兼容摄像头枚举为 /dev/video1 的情况。
    for dev in /dev/video0 /dev/video1 /dev/video2 /dev/video3; do
        if [ -e "$dev" ]; then
            echo "$dev"
            return 0
        fi
    done

    # 没有找到摄像头节点时返回失败。
    log_msg "错误：没有找到 /dev/video0~3" >&2
    return 1
}

# pick_touch_dev 的作用：
#   自动从 Linux input 设备列表中找到 Goodix 触摸屏对应的 /dev/input/eventX。
#
# 主要流程：
#   1. 如果用户显式设置 TOUCH_DEV，就优先使用用户指定的触摸节点。
#   2. 否则扫描 /proc/bus/input/devices，找到名称包含 Goodix 的输入设备。
#   3. 从该设备的 Handlers 行提取 eventX，并转换成 /dev/input/eventX 路径。
#
# 返回值：
#   找到触摸节点时打印路径并返回 0；未找到时不打印内容并返回 1。
pick_touch_dev()
{
    # 用户设置 TOUCH_DEV 时只验证节点存在，不再猜测 event 编号。
    if [ -n "${TOUCH_DEV:-}" ]; then
        if [ -e "$TOUCH_DEV" ]; then
            echo "$TOUCH_DEV"
            return 0
        fi
        log_msg "警告：指定的触摸节点不存在：$TOUCH_DEV" >&2
        return 1
    fi

    # /proc/bus/input/devices 由 input 子系统提供，能稳定关联设备名称和 event 节点。
    if [ ! -r /proc/bus/input/devices ]; then
        return 1
    fi

    # grep 先截出 Goodix 设备块，sed 再从 Handlers=eventX 中提取真实 event 节点。
    grep -A6 -i "Goodix" /proc/bus/input/devices 2>/dev/null | \
        sed -n 's/^H: Handlers=.*\(event[0-9][0-9]*\).*/\/dev\/input\/\1/p' | \
        head -n 1
}

# configure_touch_input 的作用：
#   显式加载 Qt evdevtouch 输入插件，确保 QML MouseArea 能收到触摸事件。
#
# 主要流程：
#   1. 调用 pick_touch_dev 找到当前 Goodix 触摸节点。
#   2. 找到节点时把 evdevtouch 绑定到该 event 设备。
#   3. 未找到节点时仍加载 evdevtouch，让 Qt 自行扫描可用触摸设备。
#
# 返回值：
#   无返回值；函数只设置当前进程环境变量。
configure_touch_input()
{
    # TOUCH_EVENT_DEV 保存自动识别到的触摸节点，例如 /dev/input/event2。
    TOUCH_EVENT_DEV="$(pick_touch_dev || true)"

    # 找到 Goodix 节点时显式指定路径，避免 Qt eglfs 未自动扫描 input 设备。
    if [ -n "$TOUCH_EVENT_DEV" ]; then
        export QT_QPA_GENERIC_PLUGINS="${QT_QPA_GENERIC_PLUGINS:-evdevtouch:$TOUCH_EVENT_DEV}"
        export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS="${QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS:-$TOUCH_EVENT_DEV}"
        log_msg "touch input: $TOUCH_EVENT_DEV"
        return 0
    fi

    # 未找到 Goodix 时仍加载 evdevtouch，保留其它兼容触摸屏自动扫描的机会。
    export QT_QPA_GENERIC_PLUGINS="${QT_QPA_GENERIC_PLUGINS:-evdevtouch}"
    log_msg "touch input: Goodix event node not found, fallback to evdevtouch auto scan"
}

# require_command 的作用是确认某个命令存在，避免后续 exec 失败时日志不清楚。
require_command()
{
    # $1 是需要检查的命令名称，例如 gst-launch-1.0。
    if ! command -v "$1" >/dev/null 2>&1; then
        log_msg "错误：找不到命令：$1"
        return 1
    fi

    return 0
}

# run_gst_gl_preview 的作用：
#   启动已经在板端验证过的 GStreamer GL 硬件视频预览路线。
#
# 主要流程：
#   1. 使用 v4l2src 以 GST_IO_MODE 指定方式从 UVC 摄像头取 YUYV 帧。
#   2. 使用 glupload 把视频帧交给 GStreamer GL 路径。
#   3. 使用 glimagesink 直接显示到当前 EGL/GPU 显示面。
#
# 参数：
#   $1 是已经选中的摄像头节点。
#
# 返回值：
#   exec 成功后本 shell 被 gst-launch-1.0 替换；失败则返回非 0。
run_gst_gl_preview()
{
    SELECTED_VIDEO="$1"

    require_command gst-launch-1.0

    log_msg "backend=gst-gl, route=v4l2src(${GST_IO_MODE})->glupload->glimagesink, video=$SELECTED_VIDEO, capture=${CAMERA_WIDTH}x${CAMERA_HEIGHT}@${CAMERA_FPS}"

    # exec 让 gst-launch-1.0 成为前台主进程，便于 init 脚本或 SSH 看到真实退出码。
    exec gst-launch-1.0 -v \
        v4l2src device="$SELECTED_VIDEO" io-mode="$GST_IO_MODE" ! \
        "video/x-raw,format=YUY2,width=${CAMERA_WIDTH},height=${CAMERA_HEIGHT},framerate=${CAMERA_FPS}/1" ! \
        glupload ! \
        glimagesink sync=false
}

# run_qt_safe_preview 的作用：
#   启动当前稳定的 Qt Quick 工业检测界面安全预览路径。
#
# 参数：
#   $1 是已经选中的摄像头节点。
#
# 返回值：
#   exec 成功后本 shell 被 qt_camera_display 替换；失败则返回非 0。
run_qt_safe_preview()
{
    SELECTED_VIDEO="$1"

    # 确认 Qt 可执行文件存在且可执行。
    if [ ! -x "$APP_BIN" ]; then
        log_msg "错误：Qt 程序不存在或不可执行：$APP_BIN"
        exit 1
    fi

    # 输出最终启动参数，方便确认当前确实走 eglfs/wayland + OpenGL ES。
    log_msg "backend=qt-safe, platform=$QT_QPA_PLATFORM, quick=${QT_QUICK_BACKEND:-default}, opengl=$QT_OPENGL, video=$SELECTED_VIDEO, capture=${CAMERA_WIDTH}x${CAMERA_HEIGHT}@${CAMERA_FPS}"

    # exec 用 Qt 程序替换当前 shell，退出码直接传递给调用者或 init 脚本。
    exec "$APP_BIN" --camera "$SELECTED_VIDEO" --width "$CAMERA_WIDTH" --height "$CAMERA_HEIGHT" --fps "$CAMERA_FPS"
}

# run_qt_gst_preview 的作用：
#   启动 Qt Quick 工业检测界面，并让 qmlglsink 把 GStreamer GL 视频画面嵌入实时画面区。
#   mmap 模式会在 C++ 管线里插入 videoconvert->RGBA；dmabuf 模式会插入 glcolorconvert
#   和 gleffects_identity，让 GPU 先重新渲染纹理，避开直接把 DMABUF 纹理交给 Qt scene graph 的崩溃点。
#
# 参数：
#   $1 是已经选中的摄像头节点。
#
# 返回值：
#   exec 成功后本 shell 被 qt_camera_display 替换；失败则返回非 0。
run_qt_gst_preview()
{
    SELECTED_VIDEO="$1"

    # 确认 Qt 可执行文件存在且可执行。
    if [ ! -x "$APP_BIN" ]; then
        log_msg "错误：Qt 程序不存在或不可执行：$APP_BIN"
        exit 1
    fi

    # 输出最终启动参数，确认当前走 Qt Quick + qmlglsink 集成路线。
    log_msg "backend=qt-gst, platform=$QT_QPA_PLATFORM, route=v4l2src(${GST_IO_MODE})->glupload->glcolorconvert->gleffects_identity->qmlglsink, video=$SELECTED_VIDEO, capture=${CAMERA_WIDTH}x${CAMERA_HEIGHT}@${CAMERA_FPS}"

    # exec 用 Qt 程序替换当前 shell，--video-backend=gst-qml 由 C++ 创建 GStreamer 管线。
    exec "$APP_BIN" \
        --camera "$SELECTED_VIDEO" \
        --width "$CAMERA_WIDTH" \
        --height "$CAMERA_HEIGHT" \
        --fps "$CAMERA_FPS" \
        --video-backend gst-qml \
        --gst-io-mode "$GST_IO_MODE"
}

# run_qt_kms_overlay_shell 的作用：
#   启动 Qt Quick 工业检测界面，但不让 Qt 打开 /dev/video0。
#   摄像头画面由外部 DRM/KMS overlay plane 进程绘制，Qt 只显示状态和边框壳。
#
# 参数：
#   $1 是已经选中的摄像头节点，仅用于界面状态展示，不会被 Qt 后端打开。
#
# 返回值：
#   exec 成功后本 shell 被 qt_camera_display 替换；失败则返回非 0。
run_qt_kms_overlay_shell()
{
    SELECTED_VIDEO="$1"

    # 确认 Qt 可执行文件存在且可执行。
    if [ ! -x "$APP_BIN" ]; then
        log_msg "错误：Qt 程序不存在或不可执行：$APP_BIN"
        exit 1
    fi

    # 输出最终启动参数，便于和外部 overlay plane 进程日志配对排查。
    log_msg "backend=kms-overlay, platform=$QT_QPA_PLATFORM, external-plane=36, video=$SELECTED_VIDEO, capture=${CAMERA_WIDTH}x${CAMERA_HEIGHT}@${CAMERA_FPS}"

    # exec 用 Qt 程序替换当前 shell；--video-backend=kms-overlay 会让 QML 关闭内部 V4L2VideoItem。
    exec "$APP_BIN" \
        --camera "$SELECTED_VIDEO" \
        --width "$CAMERA_WIDTH" \
        --height "$CAMERA_HEIGHT" \
        --fps "$CAMERA_FPS" \
        --video-backend kms-overlay
}

# 先停旧预览，再配置触摸输入、加载 GPU，最后选择摄像头节点。
stop_old_preview
configure_touch_input
load_gpu
SELECTED_VIDEO="$(pick_video_dev)"

# 根据 VIDEO_BACKEND 选择最终显示链路。
case "$VIDEO_BACKEND" in
    qt-safe)
        run_qt_safe_preview "$SELECTED_VIDEO"
        ;;
    gst-gl)
        run_gst_gl_preview "$SELECTED_VIDEO"
        ;;
    qt-gst)
        run_qt_gst_preview "$SELECTED_VIDEO"
        ;;
    kms-overlay)
        run_qt_kms_overlay_shell "$SELECTED_VIDEO"
        ;;
    *)
        log_msg "错误：未知 VIDEO_BACKEND=$VIDEO_BACKEND，可选值：qt-safe、gst-gl、qt-gst、kms-overlay"
        exit 1
        ;;
esac
