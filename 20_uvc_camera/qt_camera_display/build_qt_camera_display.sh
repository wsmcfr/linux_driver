#!/bin/sh
#
# 作用：
#   在 cfr-vm 虚拟机中使用 ST OpenSTLinux Qt/Wayland SDK 交叉编译
#   qt_camera_display，生成可在 STM32MP157 开发板运行的 ARM 程序。
#
# 使用：
#   cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display
#   ./build_qt_camera_display.sh
#
# 可调变量：
#   SDK_ENV：Qt/Wayland SDK 环境脚本路径；
#   BUILD_DIR：构建输出目录，默认 build-mp157。

# 遇到未处理错误立即退出，避免继续使用不完整的构建结果。
# 注意：ST SDK 的环境脚本会读取若干可选环境变量，不能在 source 前启用 set -u。
set -e

# SDK_ENV 是每个新 shell 编译 STM32MP157 Qt 程序前必须 source 的环境脚本。
SDK_ENV="${SDK_ENV:-/opt/st/stm32mp1/3.1-snapshot/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi}"

# BUILD_DIR 独立保存 qmake 生成文件和目标程序，避免污染源码目录。
BUILD_DIR="${BUILD_DIR:-build-mp157}"

# SCRIPT_DIR 是当前脚本所在目录，用于从任意工作目录调用本脚本。
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

# 确认 SDK 环境脚本存在；不存在时直接报错，避免误用主机 qmake。
if [ ! -f "$SDK_ENV" ]; then
    echo "错误：找不到 Qt/Wayland SDK 环境脚本：$SDK_ENV" >&2
    exit 1
fi

# 加载 ST SDK 交叉编译环境，获得 ARM 编译器、sysroot、qmake 和 pkg-config。
. "$SDK_ENV"

# SDK 环境加载完成后再启用未定义变量检查，保护后续脚本逻辑。
set -u

# 确认 qmake 已经来自 SDK 环境，而不是虚拟机主机 Qt。
if ! command -v qmake >/dev/null 2>&1; then
    echo "错误：SDK 环境中找不到 qmake" >&2
    exit 1
fi

# 打印关键环境，方便核对是否使用了目标 sysroot。
echo "qmake: $(command -v qmake)"
echo "Qt version: $(qmake -query QT_VERSION)"
echo "Target sysroot: ${OECORE_TARGET_SYSROOT:-unknown}"

# 创建并进入构建目录；源码目录保持只放工程文件和脚本。
mkdir -p "$SCRIPT_DIR/$BUILD_DIR"
cd "$SCRIPT_DIR/$BUILD_DIR"

# 运行 qmake 生成 Makefile；CONFIG+=release 对应板端正式运行构建。
qmake "$SCRIPT_DIR/qt_camera_display.pro" CONFIG+=release

# 使用可用 CPU 并行编译；nproc 不存在时退回单线程。
JOBS="$(nproc 2>/dev/null || echo 1)"
make -j"$JOBS"

# 用 file 检查结果架构，确认生成的是 ARM 目标程序而不是 x86_64 程序。
file ./qt_camera_display

echo "构建完成：$SCRIPT_DIR/$BUILD_DIR/qt_camera_display"
