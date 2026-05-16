#!/bin/sh
#
# 作用：
#   在 cfr-vm 中交叉编译 uvc_kms_overlay 辅助进程。
#   该进程负责 UVC YUYV 采集、NEON 转 ARGB8888、DRM overlay plane 显示。
#
# 主要流程：
#   1. 定位 Buildroot output-uvc 交叉编译器和目标 sysroot。
#   2. 使用 libdrm、libjpeg、libpng 头文件和库编译 uvc_kms_overlay.c。
#   3. 输出 ARM 可执行程序到 build-mp157/uvc_kms_overlay。
#
# 可调变量：
#   BR_OUTPUT：Buildroot output 目录，默认使用当前项目约定 output-uvc。
#   BUILD_DIR：构建输出目录，默认 build-mp157。
#   OVERLAY_CC：overlay 专用交叉编译器路径，默认来自 BR_OUTPUT/host/bin。
#   注意：这里不能直接继承外部 CC，因为 ST Qt SDK 会把 CC 设置成“编译器 + 参数”的整串命令。

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-build-mp157}"
BR_OUTPUT="${BR_OUTPUT:-/home/cfr/linux/buildroot/buildroot-2020.02.6/output-uvc}"
SYSROOT="${SYSROOT:-$BR_OUTPUT/host/arm-buildroot-linux-gnueabihf/sysroot}"
OVERLAY_CC="${OVERLAY_CC:-$BR_OUTPUT/host/bin/arm-none-linux-gnueabihf-gcc}"
CC="$OVERLAY_CC"
SRC="$SCRIPT_DIR/uvc_kms_overlay.c"
OUT_DIR="$SCRIPT_DIR/$BUILD_DIR"
OUT="$OUT_DIR/uvc_kms_overlay"

if [ ! -f "$SRC" ]; then
    echo "错误：找不到源码：$SRC" >&2
    exit 1
fi

if [ ! -x "$CC" ]; then
    echo "错误：找不到交叉编译器：$CC" >&2
    exit 1
fi

if [ ! -f "$SYSROOT/usr/include/xf86drm.h" ] || [ ! -f "$SYSROOT/usr/include/libdrm/drm.h" ]; then
    echo "错误：sysroot 缺少 libdrm 头文件：$SYSROOT/usr/include/xf86drm.h 或 $SYSROOT/usr/include/libdrm/drm.h" >&2
    exit 1
fi

if [ ! -f "$SYSROOT/usr/include/jpeglib.h" ] || [ ! -f "$SYSROOT/usr/include/png.h" ]; then
    echo "错误：sysroot 缺少 libjpeg/libpng 头文件：$SYSROOT/usr/include/jpeglib.h 或 $SYSROOT/usr/include/png.h" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

"$CC" \
    --sysroot="$SYSROOT" \
    -O2 \
    -g \
    -std=gnu11 \
    -Wall \
    -Wextra \
    -I"$SYSROOT/usr/include" \
    -I"$SYSROOT/usr/include/libdrm" \
    "$SRC" \
    -o "$OUT" \
    -ldrm \
    -ljpeg \
    -lpng \
    -lz

file "$OUT" 2>/dev/null || true
echo "构建完成：$OUT"
