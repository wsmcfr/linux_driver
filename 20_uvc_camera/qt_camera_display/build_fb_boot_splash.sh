#!/bin/sh
#
# 作用：
#   在 cfr-vm 中交叉编译 fb_boot_splash 早期静态启动图绘制器。
#   该程序只依赖 libc、Linux framebuffer 头文件和 mmap，启动时可在 Qt/GPU 之前直接写 /dev/fb0。
#
# 主要流程：
#   1. 定位 Buildroot output-uvc 交叉编译器和目标 sysroot。
#   2. 使用 SPLASH_CC 编译 fb_boot_splash.c，避免继承 ST Qt SDK 导出的复杂 CC 字符串。
#   3. 输出 ARM 可执行程序到 build-mp157/fb_boot_splash。
#
# 可调变量：
#   BR_OUTPUT：Buildroot output 目录，默认使用当前项目约定 output-uvc。
#   BUILD_DIR：构建输出目录，默认 build-mp157。
#   SPLASH_CC：splash 专用交叉编译器路径，默认来自 BR_OUTPUT/host/bin。

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-build-mp157}"
BR_OUTPUT="${BR_OUTPUT:-/home/cfr/linux/buildroot/buildroot-2020.02.6/output-uvc}"
SYSROOT="${SYSROOT:-$BR_OUTPUT/host/arm-buildroot-linux-gnueabihf/sysroot}"
SPLASH_CC="${SPLASH_CC:-$BR_OUTPUT/host/bin/arm-none-linux-gnueabihf-gcc}"
CC="$SPLASH_CC"
SRC="$SCRIPT_DIR/fb_boot_splash.c"
OUT_DIR="$SCRIPT_DIR/$BUILD_DIR"
OUT="$OUT_DIR/fb_boot_splash"

if [ ! -f "$SRC" ]; then
    echo "错误：找不到源码：$SRC" >&2
    exit 1
fi

if [ ! -x "$CC" ]; then
    echo "错误：找不到交叉编译器：$CC" >&2
    exit 1
fi

if [ ! -f "$SYSROOT/usr/include/linux/fb.h" ]; then
    echo "错误：sysroot 缺少 framebuffer 头文件：$SYSROOT/usr/include/linux/fb.h" >&2
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
    "$SRC" \
    -o "$OUT"

file "$OUT" 2>/dev/null || true
echo "构建完成：$OUT"
