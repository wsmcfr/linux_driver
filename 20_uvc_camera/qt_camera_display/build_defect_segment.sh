#!/bin/sh
#
# 作用：
#   在 cfr-vm 中交叉编译 defect-segment 独立 UNet 分割推理程序。
#   Qt 首页“检测”按钮会在 MobileNetV3-Small 分类完成后调用该程序生成 raw/overlay/mask 结果图。
#
# 使用：
#   cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display
#   ORT_ROOT=/home/cfr/linux/onnxruntime-linux-arm32 ./build_defect_segment.sh
#
# 可调变量：
#   BUILD_DIR：构建输出目录，默认 build-mp157。
#   BR_OUTPUT：Buildroot output 目录，用于取得交叉编译器、sysroot、libjpeg、libpng。
#   DEFECT_CXX：推理程序专用交叉编译器路径。
#   ORT_ROOT：ONNX Runtime ARM 运行库/头文件根目录，必须包含 include/ 和 lib/。
#
# 说明：
#   ONNX Runtime C++ 头文件使用 C++14 语法，因此这里单独使用 -std=c++14。
#   该程序直接链接 libjpeg/libpng，输出历史页和云端可预览的 JPG/PNG 检测结果图。

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-build-mp157}"
BR_OUTPUT="${BR_OUTPUT:-/home/cfr/linux/buildroot/buildroot-2020.02.6/output-uvc}"
SYSROOT="${SYSROOT:-$BR_OUTPUT/host/arm-buildroot-linux-gnueabihf/sysroot}"
DEFECT_CXX="${DEFECT_CXX:-$BR_OUTPUT/host/bin/arm-none-linux-gnueabihf-g++}"
ORT_ROOT="${ORT_ROOT:-$SCRIPT_DIR/onnxruntime-arm}"
SRC="$SCRIPT_DIR/defect_segment.cpp"
OUT_DIR="$SCRIPT_DIR/$BUILD_DIR"
OUT="$OUT_DIR/defect-segment"

if [ ! -f "$SRC" ]; then
    echo "错误：找不到源码：$SRC" >&2
    exit 1
fi

if [ ! -x "$DEFECT_CXX" ]; then
    echo "错误：找不到交叉编译器：$DEFECT_CXX" >&2
    exit 1
fi

if [ ! -f "$ORT_ROOT/include/onnxruntime_cxx_api.h" ]; then
    echo "错误：找不到 ONNX Runtime C++ 头文件：$ORT_ROOT/include/onnxruntime_cxx_api.h" >&2
    echo "请先准备 ARMv7 ONNX Runtime SDK，并设置 ORT_ROOT=/path/to/onnxruntime-arm" >&2
    exit 1
fi

if [ ! -f "$ORT_ROOT/lib/libonnxruntime.so" ]; then
    echo "错误：找不到 ONNX Runtime 运行库：$ORT_ROOT/lib/libonnxruntime.so" >&2
    echo "请先准备 ARMv7 ONNX Runtime SDK，并设置 ORT_ROOT=/path/to/onnxruntime-arm" >&2
    exit 1
fi

if [ ! -f "$SYSROOT/usr/include/jpeglib.h" ]; then
    echo "错误：sysroot 缺少 libjpeg 头文件：$SYSROOT/usr/include/jpeglib.h" >&2
    exit 1
fi

if [ ! -f "$SYSROOT/usr/include/png.h" ]; then
    echo "错误：sysroot 缺少 libpng 头文件：$SYSROOT/usr/include/png.h" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

"$DEFECT_CXX" \
    --sysroot="$SYSROOT" \
    -O2 \
    -g \
    -std=c++14 \
    -Wall \
    -Wextra \
    -I"$ORT_ROOT/include" \
    -I"$SYSROOT/usr/include" \
    "$SRC" \
    -o "$OUT" \
    -L"$ORT_ROOT/lib" \
    -Wl,-rpath,/root/qt_camera_display/lib \
    -Wl,--allow-shlib-undefined \
    -lonnxruntime \
    -ljpeg \
    -lpng \
    -lz \
    -lstdc++ \
    -lpthread \
    -ldl \
    -lm

file "$OUT" 2>/dev/null || true
echo "构建完成：$OUT"
