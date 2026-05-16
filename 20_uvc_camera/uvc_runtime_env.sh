#!/bin/sh
#
# 作用：
#   source 本文件后，可以在当前 shell 中直接使用 /root/uvc-rootfs 里的
#   gst-launch-1.0、gst-inspect-1.0、v4l2-ctl 和 GStreamer 插件。
#
# 使用：
#   . /root/uvc-rootfs/uvc_runtime_env.sh
#   gst-inspect-1.0 v4l2src

# UVC_ROOT 指向部署到开发板 /root 下的用户态运行包根目录，避免 /tmp 被 tmpfs 覆盖。
UVC_ROOT="${UVC_ROOT:-/root/uvc-rootfs}"

# PATH 增加运行包里的命令行工具目录。
export PATH="$UVC_ROOT/usr/bin:$UVC_ROOT/bin:$PATH"

# LD_LIBRARY_PATH 增加运行包里的共享库目录。
export LD_LIBRARY_PATH="$UVC_ROOT/lib:$UVC_ROOT/usr/lib:${LD_LIBRARY_PATH:-}"

# GST_PLUGIN_PATH 指向 GStreamer 插件目录。
export GST_PLUGIN_PATH="$UVC_ROOT/usr/lib/gstreamer-1.0"

# GST_PLUGIN_SCANNER 指向 GStreamer 插件扫描辅助程序。
export GST_PLUGIN_SCANNER="$UVC_ROOT/usr/libexec/gstreamer-1.0/gst-plugin-scanner"

# GST_REGISTRY 指向可写缓存文件，避免默认路径不可写。
export GST_REGISTRY="/tmp/gst-registry-uvc.bin"

echo "UVC/GStreamer 运行环境已加载：$UVC_ROOT"
