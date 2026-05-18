#!/bin/sh
#
# 作用：
#   把交叉编译生成的 qt_camera_display 和板端启动脚本复制到 NFS rootfs。
#
# 使用：
#   cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display
#   ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs
#
# 说明：
#   本脚本只部署本项目文件，不复制 Qt 运行库。目标 rootfs 仍需已经包含
#   Qt5Core、Qt5Quick、Qt5Multimedia、eglfs/wayland 平台插件和 galcore 驱动。

# 遇到未处理错误立即退出，避免半部署状态被误认为成功。
set -eu

# ROOTFS 是开发板 NFS 根文件系统路径；不传参数时使用项目约定路径。
ROOTFS="${1:-/home/cfr/linux/nfs/rootfs}"

# BUILD_DIR 是 build_qt_camera_display.sh 的输出目录。
BUILD_DIR="${BUILD_DIR:-build-mp157}"

# SCRIPT_DIR 是当前脚本所在目录，用于定位构建产物和运行脚本。
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

# APP_SRC 是待部署的 ARM 可执行程序。
APP_SRC="$SCRIPT_DIR/$BUILD_DIR/qt_camera_display"

# OVERLAY_SRC 是 KMS overlay 视频辅助进程，负责低 CPU 摄像头 plane 显示。
OVERLAY_SRC="$SCRIPT_DIR/$BUILD_DIR/uvc_kms_overlay"

# FB_SPLASH_SRC 是早期静态启动首帧绘制器，负责在 Qt/GPU 启动前先写 /dev/fb0。
FB_SPLASH_SRC="$SCRIPT_DIR/$BUILD_DIR/fb_boot_splash"

# DEFECT_CLASSIFY_SRC 是 MobileNetV3-Small INT8 ONNX 独立推理程序。
DEFECT_CLASSIFY_SRC="$SCRIPT_DIR/$BUILD_DIR/defect-classify"

# DEFECT_SEGMENT_SRC 是 UNet INT8 ONNX 独立分割推理程序。
DEFECT_SEGMENT_SRC="$SCRIPT_DIR/$BUILD_DIR/defect-segment"

# OVERLAY_RUN_SRC 是板端 start/stop/status 控制脚本。
OVERLAY_RUN_SRC="$SCRIPT_DIR/run_qt_kms_overlay_display.sh"

# PROBE_SRC 是板端零拷贝/硬件视频链路探测脚本，用于在集成 Qt 前独立验证 GStreamer/KMS 路线。
PROBE_SRC="$SCRIPT_DIR/probe_zero_copy_video_path.sh"

# COS_UPLOAD_SRC 是检测流程自动上传 source/annotated 图片到云端 COS 的板端脚本。
COS_UPLOAD_SRC="$SCRIPT_DIR/defect-cos-upload"

# DEFECT_MODEL_SRC 是待部署的 INT8 ONNX 模型路径，可通过环境变量覆盖。
DEFECT_MODEL_SRC="${DEFECT_MODEL_SRC:-/mnt/d/model_picture/checkpoints_classify/defect_classifier_static_mixed_int8.onnx}"

# DEFECT_LABELS_SRC 是待部署的类别映射路径，可通过环境变量覆盖。
DEFECT_LABELS_SRC="${DEFECT_LABELS_SRC:-/mnt/d/model_picture/checkpoints_classify/defect_classifier_static_mixed_int8_labels.json}"

# DEFECT_UNET_MODEL_SRC 是待部署的 UNet INT8 分割模型路径，可通过环境变量覆盖。
DEFECT_UNET_MODEL_SRC="${DEFECT_UNET_MODEL_SRC:-/mnt/d/model_picture/checkpoints_unet_test/defect_unet_test_decoder_head_int8.onnx}"

# ORT_ROOT 是可选 ONNX Runtime ARM SDK 根目录；存在 libonnxruntime.so 时部署到板端 lib 目录。
ORT_ROOT="${ORT_ROOT:-$SCRIPT_DIR/onnxruntime-arm}"

# DISPLAY_QUIET_INIT_SRC 是开机早期显示静默脚本，负责尽早关闭 fbcon 光标。
DISPLAY_QUIET_INIT_SRC="$SCRIPT_DIR/../S05display-quiet"

# UVC_INIT_SRC 是正式开机自启动脚本，负责启动 Qt + KMS overlay 工业界面。
UVC_INIT_SRC="$SCRIPT_DIR/../S90uvc-camera"

# INSTALL_DIR 是开发板启动后可见的持久目录。
INSTALL_DIR="$ROOTFS/root/qt_camera_display"

# MODEL_INSTALL_DIR 是板端模型与 labels JSON 安装目录。
MODEL_INSTALL_DIR="$INSTALL_DIR/models"

# LIB_INSTALL_DIR 是板端私有动态库目录，run 脚本通过 LD_LIBRARY_PATH 加载。
LIB_INSTALL_DIR="$INSTALL_DIR/lib"

# BOARD_COS_UPLOAD_ENV_FILE 是板端运行时 defect-cos-upload 默认读取的私有账号配置路径。
# 如果部署时设置 CLOUD_UPLOAD_ENV_FILE，则会把该板端绝对路径映射到当前 NFS rootfs 下。
BOARD_COS_UPLOAD_ENV_FILE="${CLOUD_UPLOAD_ENV_FILE:-/root/qt_camera_display/cos-upload.env}"

# COS_UPLOAD_ENV_TARGET 是在虚拟机/NFS rootfs 中实际写入的账号配置文件位置。
case "$BOARD_COS_UPLOAD_ENV_FILE" in
    /root/qt_camera_display/cos-upload.env)
        COS_UPLOAD_ENV_TARGET="$INSTALL_DIR/cos-upload.env"
        ;;
    /*)
        COS_UPLOAD_ENV_TARGET="$ROOTFS$BOARD_COS_UPLOAD_ENV_FILE"
        ;;
    *)
        echo "错误：CLOUD_UPLOAD_ENV_FILE 必须是板端绝对路径：$BOARD_COS_UPLOAD_ENV_FILE" >&2
        exit 1
        ;;
esac

shell_single_quote()
{
    # shell_single_quote 的作用：
    #   把账号、密码等任意短字符串转换成单引号 shell 字面量内容。
    # 主要流程：
    #   1. 原样输出普通字符。
    #   2. 遇到单引号时转换成 POSIX shell 可解析的 '\'' 片段。
    # 参数：
    #   $1 是需要写入 cos-upload.env 的原始字符串。
    # 返回值：
    #   stdout 输出已转义内容；函数本身不保存任何文件。
    printf '%s' "$1" | sed "s/'/'\\\\''/g"
}

write_cos_upload_env_file()
{
    # write_cos_upload_env_file 的作用：
    #   部署时可选生成板端私有上传账号配置文件，让检测按钮默认能登录云端。
    # 主要流程：
    #   1. 如果没有提供 CLOUD_ACCOUNT/CLOUD_PASSWORD，则保留现有配置并跳过生成。
    #   2. 如果只提供其中一个变量，则直接报错，避免写出半配置。
    #   3. 使用临时文件生成 shell 变量，再用 sudo install -m 600 安装到 rootfs。
    # 参数：
    #   无显式参数；读取部署环境变量 CLOUD_ACCOUNT、CLOUD_PASSWORD、CLOUD_DEVICE_ID、CLOUD_PART_ID。
    # 返回值：
    #   成功返回 0；配置不完整或安装失败时随 set -e 退出。
    if [ -z "${CLOUD_ACCOUNT:-}" ] && [ -z "${CLOUD_PASSWORD:-}" ]; then
        echo "未设置 CLOUD_ACCOUNT/CLOUD_PASSWORD，跳过生成默认上传账号配置。"
        echo "如需让检测按钮自动上传，可在部署时设置这两个环境变量后重跑本脚本。"
        return 0
    fi

    if [ -z "${CLOUD_ACCOUNT:-}" ] || [ -z "${CLOUD_PASSWORD:-}" ]; then
        echo "错误：生成默认上传账号配置需要同时设置 CLOUD_ACCOUNT 和 CLOUD_PASSWORD" >&2
        exit 1
    fi

    tmp_env="$(mktemp)"
    {
        echo "# 本文件由 deploy_qt_camera_display.sh 生成，只保存在板端/rootfs 本地，不提交到 Git。"
        printf "CLOUD_ACCOUNT='"
        shell_single_quote "$CLOUD_ACCOUNT"
        printf "'\n"
        printf "CLOUD_PASSWORD='"
        shell_single_quote "$CLOUD_PASSWORD"
        printf "'\n"

        if [ -n "${CLOUD_DEVICE_ID:-}" ]; then
            printf "CLOUD_DEVICE_ID='"
            shell_single_quote "$CLOUD_DEVICE_ID"
            printf "'\n"
        fi

        if [ -n "${CLOUD_PART_ID:-}" ]; then
            printf "CLOUD_PART_ID='"
            shell_single_quote "$CLOUD_PART_ID"
            printf "'\n"
        fi
    } > "$tmp_env"

    sudo mkdir -p "$(dirname "$COS_UPLOAD_ENV_TARGET")"
    sudo install -m 600 -o root -g root "$tmp_env" "$COS_UPLOAD_ENV_TARGET"
    rm -f "$tmp_env"

    echo "默认上传账号配置已写入：$COS_UPLOAD_ENV_TARGET"
    echo "板端 defect-cos-upload 默认读取：$BOARD_COS_UPLOAD_ENV_FILE"
}

# 检查 rootfs 目录是否存在，避免 sudo cp 写到错误路径。
if [ ! -d "$ROOTFS" ]; then
    echo "错误：NFS rootfs 不存在：$ROOTFS" >&2
    exit 1
fi

# 检查构建产物是否存在；不存在时提示先编译。
if [ ! -f "$APP_SRC" ]; then
    echo "错误：找不到构建产物：$APP_SRC" >&2
    echo "请先执行：./build_qt_camera_display.sh" >&2
    exit 1
fi

# 检查 overlay 辅助进程是否已构建；它是 kms-overlay 集成路线的必要运行文件。
if [ ! -f "$OVERLAY_SRC" ]; then
    echo "错误：找不到 KMS overlay 构建产物：$OVERLAY_SRC" >&2
    echo "请先执行：./build_uvc_kms_overlay.sh" >&2
    exit 1
fi

# 检查早期静态启动图绘制器是否已构建；它用于填补 Qt 启动画面前的黑屏时间。
if [ ! -f "$FB_SPLASH_SRC" ]; then
    echo "错误：找不到早期静态启动图构建产物：$FB_SPLASH_SRC" >&2
    echo "请先执行：./build_fb_boot_splash.sh" >&2
    exit 1
fi

if [ ! -f "$DEFECT_CLASSIFY_SRC" ]; then
    echo "错误：找不到缺陷分类推理程序：$DEFECT_CLASSIFY_SRC" >&2
    echo "请先执行：ORT_ROOT=/path/to/onnxruntime-arm ./build_defect_classify.sh" >&2
    exit 1
fi

if [ ! -f "$DEFECT_SEGMENT_SRC" ]; then
    echo "错误：找不到 UNet 分割推理程序：$DEFECT_SEGMENT_SRC" >&2
    echo "请先执行：ORT_ROOT=/path/to/onnxruntime-arm ./build_defect_segment.sh" >&2
    exit 1
fi

if [ ! -f "$OVERLAY_RUN_SRC" ]; then
    echo "错误：找不到 KMS overlay 控制脚本：$OVERLAY_RUN_SRC" >&2
    exit 1
fi

# 检查探测脚本是否存在；它不参与编译，但部署后可以直接在串口或 SSH 中运行。
if [ ! -f "$PROBE_SRC" ]; then
    echo "错误：找不到零拷贝探测脚本：$PROBE_SRC" >&2
    exit 1
fi

# 检查 COS 上传脚本是否存在；检测流程会在本地图片落盘后调用它。
if [ ! -f "$COS_UPLOAD_SRC" ]; then
    echo "错误：找不到 COS 上传脚本：$COS_UPLOAD_SRC" >&2
    exit 1
fi

if [ ! -f "$DEFECT_MODEL_SRC" ]; then
    echo "错误：找不到 INT8 ONNX 模型：$DEFECT_MODEL_SRC" >&2
    echo "可设置 DEFECT_MODEL_SRC=/path/to/defect_classifier_static_mixed_int8.onnx 后重跑部署。" >&2
    exit 1
fi

if [ ! -f "$DEFECT_LABELS_SRC" ]; then
    echo "错误：找不到 labels JSON：$DEFECT_LABELS_SRC" >&2
    echo "可设置 DEFECT_LABELS_SRC=/path/to/defect_classifier_static_mixed_int8_labels.json 后重跑部署。" >&2
    exit 1
fi

if [ ! -f "$DEFECT_UNET_MODEL_SRC" ]; then
    echo "错误：找不到 UNet INT8 ONNX 模型：$DEFECT_UNET_MODEL_SRC" >&2
    echo "可设置 DEFECT_UNET_MODEL_SRC=/path/to/defect_unet_test_decoder_head_int8.onnx 后重跑部署。" >&2
    exit 1
fi

if [ ! -f "$DISPLAY_QUIET_INIT_SRC" ]; then
    echo "错误：找不到早期显示静默脚本：$DISPLAY_QUIET_INIT_SRC" >&2
    exit 1
fi

if [ ! -f "$UVC_INIT_SRC" ]; then
    echo "错误：找不到开机自启动脚本：$UVC_INIT_SRC" >&2
    exit 1
fi

# 创建安装目录；NFS rootfs 通常需要 sudo 才能写入 root 目录。
sudo mkdir -p "$INSTALL_DIR"
sudo mkdir -p "$MODEL_INSTALL_DIR"
sudo mkdir -p "$LIB_INSTALL_DIR"
sudo mkdir -p "$ROOTFS/etc/init.d"

# 复制 Qt 可执行程序和运行脚本。
sudo cp "$APP_SRC" "$INSTALL_DIR/qt_camera_display"
sudo cp "$OVERLAY_SRC" "$INSTALL_DIR/uvc_kms_overlay"
sudo cp "$FB_SPLASH_SRC" "$INSTALL_DIR/fb_boot_splash"
sudo cp "$DEFECT_CLASSIFY_SRC" "$INSTALL_DIR/defect-classify"
sudo cp "$DEFECT_SEGMENT_SRC" "$INSTALL_DIR/defect-segment"
sudo cp "$SCRIPT_DIR/run_qt_camera_display.sh" "$INSTALL_DIR/run_qt_camera_display.sh"
sudo cp "$OVERLAY_RUN_SRC" "$INSTALL_DIR/run_qt_kms_overlay_display.sh"
sudo cp "$PROBE_SRC" "$INSTALL_DIR/probe_zero_copy_video_path.sh"
sudo cp "$COS_UPLOAD_SRC" "$INSTALL_DIR/defect-cos-upload"
sudo cp "$DEFECT_MODEL_SRC" "$MODEL_INSTALL_DIR/defect_classifier_static_mixed_int8.onnx"
sudo cp "$DEFECT_LABELS_SRC" "$MODEL_INSTALL_DIR/defect_classifier_static_mixed_int8_labels.json"
sudo cp "$DEFECT_UNET_MODEL_SRC" "$MODEL_INSTALL_DIR/defect_unet_test_decoder_head_int8.onnx"
sudo cp "$DISPLAY_QUIET_INIT_SRC" "$ROOTFS/etc/init.d/S05display-quiet"
sudo cp "$UVC_INIT_SRC" "$ROOTFS/etc/init.d/S90uvc-camera"

# 如果 ORT_ROOT/lib/libonnxruntime.so 存在，则一起部署到私有 lib 目录；缺失时提示用户手动补运行库。
if [ -f "$ORT_ROOT/lib/libonnxruntime.so" ]; then
    ORT_REAL_LIB="$(readlink -f "$ORT_ROOT/lib/libonnxruntime.so")"
    ORT_REAL_NAME="$(basename "$ORT_REAL_LIB")"
    sudo cp "$ORT_REAL_LIB" "$LIB_INSTALL_DIR/$ORT_REAL_NAME"
    sudo ln -sf "$ORT_REAL_NAME" "$LIB_INSTALL_DIR/libonnxruntime.so"
else
    echo "提示：未找到 $ORT_ROOT/lib/libonnxruntime.so，未复制 ONNX Runtime 运行库。"
    echo "      若 rootfs 全局没有 libonnxruntime.so，检测按钮会启动失败。"
fi

# 设置可执行权限，确保开发板 root 用户可以直接运行。
sudo chmod 755 "$INSTALL_DIR/qt_camera_display"
sudo chmod 755 "$INSTALL_DIR/uvc_kms_overlay"
sudo chmod 755 "$INSTALL_DIR/fb_boot_splash"
sudo chmod 755 "$INSTALL_DIR/defect-classify"
sudo chmod 755 "$INSTALL_DIR/defect-segment"
sudo chmod 755 "$INSTALL_DIR/run_qt_camera_display.sh"
sudo chmod 755 "$INSTALL_DIR/run_qt_kms_overlay_display.sh"
sudo chmod 755 "$INSTALL_DIR/probe_zero_copy_video_path.sh"
sudo chmod 755 "$INSTALL_DIR/defect-cos-upload"
sudo chmod 644 "$MODEL_INSTALL_DIR/defect_classifier_static_mixed_int8.onnx"
sudo chmod 644 "$MODEL_INSTALL_DIR/defect_classifier_static_mixed_int8_labels.json"
sudo chmod 644 "$MODEL_INSTALL_DIR/defect_unet_test_decoder_head_int8.onnx"
if [ -n "${ORT_REAL_NAME:-}" ] && [ -f "$LIB_INSTALL_DIR/$ORT_REAL_NAME" ]; then
    sudo chmod 755 "$LIB_INSTALL_DIR/$ORT_REAL_NAME"
fi
sudo chmod 755 "$ROOTFS/etc/init.d/S05display-quiet"
sudo chmod 755 "$ROOTFS/etc/init.d/S90uvc-camera"

# 可选写入默认上传账号配置；没有部署环境变量时不覆盖板端已有配置。
write_cos_upload_env_file

# 打印部署结果和板端启动命令。
echo "部署完成：$INSTALL_DIR"
echo "开发板运行："
echo "  /root/qt_camera_display/run_qt_camera_display.sh"
echo "KMS overlay 低 CPU 集成路线："
echo "  /root/qt_camera_display/run_qt_kms_overlay_display.sh start"
echo "手动验证当前帧分类程序："
echo "  LD_LIBRARY_PATH=/root/qt_camera_display/lib:\$LD_LIBRARY_PATH /root/qt_camera_display/defect-classify --image /tmp/test.jpg"
echo "手动验证当前帧分割程序："
echo "  LD_LIBRARY_PATH=/root/qt_camera_display/lib:\$LD_LIBRARY_PATH /root/qt_camera_display/defect-segment --image /tmp/test.jpg --output-dir /mnt/sdcard/images"
echo "零拷贝/硬件视频探测："
echo "  /root/qt_camera_display/probe_zero_copy_video_path.sh"
