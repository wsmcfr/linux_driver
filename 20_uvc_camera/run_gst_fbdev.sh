#!/bin/sh
#
# 作用：
#   在开发板上用 Buildroot 打好的 GStreamer 运行包显示 UVC 摄像头画面。
#   默认从 /dev/video0 取 640x480 raw YUYV/RGB 类视频流，显示到 /dev/fb0。
#
# 参数：
#   $1：模式，raw 或 mjpeg；raw 走 video/x-raw，mjpeg 走 image/jpeg + jpegdec。
#   $2：V4L2 摄像头节点，默认 /dev/video0。
#   $3：framebuffer 节点，默认 /dev/fb0。
#   $4：宽度，默认 640。
#   $5：高度，默认 480。

# UVC_ROOT 指向部署到 /root 的轻量 rootfs 运行包，避免 /tmp 被 tmpfs 覆盖。
UVC_ROOT="${UVC_ROOT:-/root/uvc-rootfs}"

# MODE 决定 GStreamer caps 和是否需要 JPEG 解码。
MODE="${1:-raw}"

# VIDEO_DEV 是 UVC 摄像头枚举出来的视频节点。
VIDEO_DEV="${2:-/dev/video0}"

# FB_DEV 是 LCD framebuffer 节点。
FB_DEV="${3:-/dev/fb0}"

# WIDTH/HEIGHT 是希望摄像头输出的画面尺寸。
WIDTH="${4:-640}"
HEIGHT="${5:-480}"

# PATH 加入运行包里的 gst-launch-1.0 和 v4l2-ctl。
export PATH="$UVC_ROOT/usr/bin:$UVC_ROOT/bin:$PATH"

# LD_LIBRARY_PATH 让程序优先使用运行包里的 GStreamer、GLib、libv4l 等库。
export LD_LIBRARY_PATH="$UVC_ROOT/lib:$UVC_ROOT/usr/lib:${LD_LIBRARY_PATH:-}"

# GST_PLUGIN_PATH 指定运行包里的 GStreamer 插件目录。
export GST_PLUGIN_PATH="$UVC_ROOT/usr/lib/gstreamer-1.0"

# GST_PLUGIN_SCANNER 指定插件扫描器，避免 gst-launch 在精简 rootfs 中找不到 scanner。
export GST_PLUGIN_SCANNER="$UVC_ROOT/usr/libexec/gstreamer-1.0/gst-plugin-scanner"

# GST_REGISTRY 放在 /tmp，允许目标板运行时生成插件缓存。
export GST_REGISTRY="/tmp/gst-registry-uvc.bin"

# 先打印摄像头格式，方便确认应该使用 raw 还是 mjpeg。
v4l2-ctl -d "$VIDEO_DEV" --list-formats-ext

# 根据摄像头输出格式选择 pipeline。
case "$MODE" in
raw)
    exec gst-launch-1.0 -v \
        v4l2src device="$VIDEO_DEV" ! \
        "video/x-raw,width=${WIDTH},height=${HEIGHT}" ! \
        videoconvert ! \
        fbdevsink device="$FB_DEV" sync=false
    ;;
mjpeg)
    exec gst-launch-1.0 -v \
        v4l2src device="$VIDEO_DEV" ! \
        "image/jpeg,width=${WIDTH},height=${HEIGHT}" ! \
        jpegdec ! \
        videoconvert ! \
        fbdevsink device="$FB_DEV" sync=false
    ;;
*)
    echo "错误：模式只能是 raw 或 mjpeg，当前是：$MODE" >&2
    exit 1
    ;;
esac
