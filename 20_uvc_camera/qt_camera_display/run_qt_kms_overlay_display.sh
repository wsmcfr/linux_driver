#!/bin/sh
#
# 作用：
#   以可控的 start/stop/status 方式启动 Qt UI + KMS overlay 摄像头显示链路。
#   Qt eglfs 负责 primary plane 上的工业检测界面；uvc_kms_overlay 负责 overlay plane 36 上的视频。
#
# 主要流程：
#   start   ：停止旧显示进程，后台启动隐藏状态的 overlay 视频进程，再后台启动 Qt kms-overlay 界面壳。
#             Qt 启动画面结束后会通过控制 socket 发送 VISIBLE 1 恢复实时视频层。
#   stop    ：优雅停止 Qt 和 overlay 进程，让 overlay 进程释放 DRM plane。
#   status  ：输出当前进程、日志路径和 plane 参数，便于 SSH 快速确认现场状态。
#   restart ：先 stop 再 start。
#   restore-fallback：停止当前链路后恢复已验证的 GStreamer drop6 可见线。
#
# 返回值：
#   操作成功返回 0；任一关键进程启动失败返回非 0。

set -eu

PATH=/sbin:/bin:/usr/sbin:/usr/bin:$PATH

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
APP_BIN="${APP_BIN:-$SCRIPT_DIR/qt_camera_display}"
OVERLAY_BIN="${OVERLAY_BIN:-$SCRIPT_DIR/uvc_kms_overlay}"
FB_BOOT_SPLASH_BIN="${FB_BOOT_SPLASH_BIN:-$SCRIPT_DIR/fb_boot_splash}"

VIDEO_DEV="${VIDEO_DEV:-/dev/video0}"
FB_DEV="${FB_DEV:-/dev/fb0}"
CAMERA_WIDTH="${CAMERA_WIDTH:-640}"
CAMERA_HEIGHT="${CAMERA_HEIGHT:-480}"
CAMERA_FPS="${CAMERA_FPS:-10}"
FB_BOOT_SPLASH_ENABLE="${FB_BOOT_SPLASH_ENABLE:-1}"

OVERLAY_CONTROL_SOCKET="${OVERLAY_CONTROL_SOCKET:-/tmp/uvc-kms-overlay-control.sock}"

KMS_OVERLAY_PLANE="${KMS_OVERLAY_PLANE:-36}"
KMS_OVERLAY_X="${KMS_OVERLAY_X:-177}"
KMS_OVERLAY_Y="${KMS_OVERLAY_Y:-73}"
KMS_OVERLAY_W="${KMS_OVERLAY_W:-640}"
KMS_OVERLAY_H="${KMS_OVERLAY_H:-480}"

QT_LOG="${QT_LOG:-/tmp/qt-kms-overlay-shell.log}"
OVERLAY_LOG="${OVERLAY_LOG:-/tmp/uvc-kms-overlay.log}"
FALLBACK_LOG="${FALLBACK_LOG:-/tmp/gst640-src10-drop6-bgra-fullrect.log}"

QT_PID_NAME="qt_camera_display"
OVERLAY_PID_NAME="uvc_kms_overlay"
LEGACY_DISPLAY_NAMES="uvc_fb_preview uvc_fb_preview_native gst-launch-1.0 uvc_kms_probe uvc_kms_probe_stage uvc_kms_probe_overlay uvc_kms_probe_overlay_rect"

# Qt 界面默认显示北京时间；CST-8 是 POSIX 写法，含义为 UTC+8。
# 这里设置给 run_qt_camera_display.sh 和 qt_camera_display 继承，避免开机自启动界面仍显示 UTC。
TZ="${TZ:-CST-8}"
export TZ

log_msg()
{
    echo "qt-kms-overlay: $*"
}

require_executable()
{
    if [ ! -x "$1" ]; then
        log_msg "错误：缺少可执行文件：$1" >&2
        return 1
    fi
    return 0
}

load_gpu()
{
    if [ -e /dev/galcore ]; then
        return 0
    fi

    modprobe galcore 2>/dev/null || true
    if [ ! -e /dev/galcore ]; then
        log_msg "错误：/dev/galcore 不存在，Qt eglfs 无法使用 GPU"
        return 1
    fi

    return 0
}

hide_display_console()
{
    # 关闭 framebuffer 控制台光标，避免内核企鹅 logo 关闭后启动空窗只剩 "_" 闪烁。
    if [ -w /sys/class/graphics/fbcon/cursor_blink ]; then
        echo 0 > /sys/class/graphics/fbcon/cursor_blink 2>/dev/null || true
    fi
}

show_boot_splash()
{
    # 在 Qt eglfs 打开屏幕之前先写入静态启动首帧，填补手动 restart 或 S90 等待阶段的黑屏。
    if [ "$FB_BOOT_SPLASH_ENABLE" != "1" ]; then
        return 0
    fi

    # 工具不存在时继续保持旧启动流程，避免半部署状态阻断 Qt 正式界面。
    if [ ! -x "$FB_BOOT_SPLASH_BIN" ]; then
        log_msg "fb_boot_splash not executable: $FB_BOOT_SPLASH_BIN"
        return 0
    fi

    # /dev/fb0 没准备好时由后续 wait_for_node 或 Qt 自己接管，不在这里强行失败。
    if [ ! -e "$FB_DEV" ]; then
        return 0
    fi

    if "$FB_BOOT_SPLASH_BIN" -f "$FB_DEV" -q >/dev/null 2>&1; then
        log_msg "early static splash drawn on $FB_DEV"
    else
        log_msg "early static splash draw failed on $FB_DEV"
    fi

    return 0
}

wait_for_node()
{
    # 等待设备节点出现，避免 Qt 启动太早时 eglfs/DRM/fb0 还没准备好。
    node="$1"
    timeout="${2:-10}"

    while [ "$timeout" -gt 0 ]; do
        if [ -e "$node" ]; then
            return 0
        fi
        sleep 1
        timeout=$((timeout - 1))
    done

    return 1
}

wait_for_process()
{
    # 等待指定进程名出现；启动阶段不用固定 sleep 猜测耗时，降低误判概率。
    name="$1"
    timeout="${2:-8}"

    while [ "$timeout" -gt 0 ]; do
        if pidof "$name" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
        timeout=$((timeout - 1))
    done

    return 1
}

wait_for_qt_boot_surface()
{
    # 等待 QML 根对象完成加载并写出启动层日志。
    # 这个日志出现后再启动 overlay，可让 Qt splash 先占住 LCD，避免摄像头首帧抢先显示。
    timeout="${1:-8}"

    while [ "$timeout" -gt 0 ]; do
        if [ -f "$QT_LOG" ] && grep -q "boot overlay visible false" "$QT_LOG" 2>/dev/null; then
            return 0
        fi

        if ! pidof "$QT_PID_NAME" >/dev/null 2>&1; then
            return 1
        fi

        sleep 1
        timeout=$((timeout - 1))
    done

    log_msg "警告：未在日志中看到 boot overlay visible false，继续延迟启动 overlay"
    return 0
}

send_overlay_command()
{
    # 通过 overlay 控制 socket 发送一行命令，用于启动阶段隐藏/恢复 KMS 视频 plane。
    # 命令失败时返回非 0，由调用者决定是继续等待还是进入兜底路线。
    command="$1"

    if [ ! -S "$OVERLAY_CONTROL_SOCKET" ]; then
        return 1
    fi

    if command -v nc >/dev/null 2>&1; then
        printf '%s\n' "$command" | nc -U "$OVERLAY_CONTROL_SOCKET" >/dev/null 2>&1
        return $?
    fi

    # 精简 Buildroot rootfs 可能没有 nc/socat/python。
    # 启动隐藏由 uvc_kms_overlay 的 -V 0 参数负责；Qt splash 结束后会用 C++ socket 客户端发送 VISIBLE 1。
    return 1
}

wait_for_overlay_control()
{
    # 等待 overlay 控制 socket 出现，说明进程已完成 DRM/V4L2 初始化并可以接收 Qt 控制命令。
    timeout="${1:-5}"

    while [ "$timeout" -gt 0 ]; do
        if [ -S "$OVERLAY_CONTROL_SOCKET" ]; then
            return 0
        fi
        sleep 1
        timeout=$((timeout - 1))
    done

    return 1
}

stop_processes_by_name()
{
    names="$*"

    for name in $names; do
        for pid in $(pidof "$name" 2>/dev/null || true); do
            kill "$pid" 2>/dev/null || true
        done
    done

    sleep 1

    for name in $names; do
        for pid in $(pidof "$name" 2>/dev/null || true); do
            kill -9 "$pid" 2>/dev/null || true
        done
    done
}

stop_legacy_display()
{
    # 这些旧进程可能占用 /dev/video0、primary plane 或 overlay plane，必须先清理。
    if [ "${SKIP_UVC_INIT_STOP:-0}" != "1" ] && [ -x /etc/init.d/S90uvc-camera ]; then
        /etc/init.d/S90uvc-camera stop >/dev/null 2>&1 || true
    fi
    stop_processes_by_name $LEGACY_DISPLAY_NAMES "$QT_PID_NAME" "$OVERLAY_PID_NAME"
}

status_legacy_display()
{
    found=0

    # 列出历史临时探针，避免 status 只看正式进程而漏掉仍占用摄像头或 KMS plane 的旧进程。
    for name in $LEGACY_DISPLAY_NAMES; do
        pids="$(pidof "$name" 2>/dev/null || true)"
        if [ -n "$pids" ]; then
            log_msg "Legacy $name PID: $pids"
            found=1
        fi
    done

    if [ "$found" -eq 0 ]; then
        log_msg "Legacy display PID: none"
    fi
}

restore_fallback_pipeline()
{
    # 当前推荐可见恢复线：640x480@10fps 输入，videorate drop 到 6fps，再 BGRA -> kmssink。
    nohup gst-launch-1.0 -q \
        v4l2src device="$VIDEO_DEV" io-mode=mmap ! \
        'video/x-raw,format=YUY2,width=640,height=480,framerate=10/1' ! \
        videorate drop-only=true max-rate=6 skip-to-first=true silent=true ! \
        'video/x-raw,format=YUY2,width=640,height=480,framerate=6/1' ! \
        videoconvert dither=none chroma-mode=none matrix-mode=input-only chroma-resampler=nearest alpha-mode=set alpha-value=1 n-threads=1 qos=false ! \
        'video/x-raw,format=BGRA,width=640,height=480,framerate=6/1' ! \
        kmssink driver-name=stm sync=false async=false enable-last-sample=false qos=false show-preroll-frame=false processing-deadline=0 max-lateness=-1 'render-rectangle=<0,0,1024,600>' \
        >"$FALLBACK_LOG" 2>&1 < /dev/null &
}

start_overlay()
{
    require_executable "$OVERLAY_BIN"

    # 所有板端显示链路必须后台运行，日志写入 /tmp，避免 SSH/控制台输出污染 LCD。
    nohup "$OVERLAY_BIN" \
        -d "$VIDEO_DEV" \
        -w "$CAMERA_WIDTH" \
        -h "$CAMERA_HEIGHT" \
        -r "$CAMERA_FPS" \
        -S "$OVERLAY_CONTROL_SOCKET" \
        -m neon \
        -F argb8888 \
        -P "$KMS_OVERLAY_PLANE" \
        -x "$KMS_OVERLAY_X" \
        -y "$KMS_OVERLAY_Y" \
        -W "$KMS_OVERLAY_W" \
        -H "$KMS_OVERLAY_H" \
        -V 0 \
        >"$OVERLAY_LOG" 2>&1 < /dev/null &

    if ! wait_for_process "$OVERLAY_PID_NAME" 8; then
        log_msg "错误：overlay 视频进程未保持运行，日志：$OVERLAY_LOG"
        sed -n '1,80p' "$OVERLAY_LOG" 2>/dev/null || true
        return 1
    fi

    if ! wait_for_overlay_control 5; then
        log_msg "错误：overlay 控制 socket 未出现：$OVERLAY_CONTROL_SOCKET"
        sed -n '1,120p' "$OVERLAY_LOG" 2>/dev/null || true
        return 1
    fi

    if ! send_overlay_command "VISIBLE 0"; then
        log_msg "overlay 已用 -V 0 启动，等待 Qt splash 结束后恢复视频层"
    fi

    return 0
}

start_qt_shell()
{
    require_executable "$APP_BIN"
    load_gpu
    hide_display_console
    wait_for_node "$FB_DEV" 10 || true
    show_boot_splash

    # Qt 不打开摄像头；VIDEO_BACKEND=kms-overlay 只绘制 UI 壳和在线状态。
    nohup env \
        VIDEO_BACKEND=kms-overlay \
        VIDEO_DEV="$VIDEO_DEV" \
        CAMERA_WIDTH="$CAMERA_WIDTH" \
        CAMERA_HEIGHT="$CAMERA_HEIGHT" \
        CAMERA_FPS="$CAMERA_FPS" \
        STOP_OLD_PREVIEW=0 \
        GST_REGISTRY_REBUILD=0 \
        QT_QPA_EGLFS_DISABLE_INPUT="${QT_QPA_EGLFS_DISABLE_INPUT:-0}" \
        QT_QPA_EGLFS_ALWAYS_SET_MODE=0 \
        "$SCRIPT_DIR/run_qt_camera_display.sh" \
        >"$QT_LOG" 2>&1 < /dev/null &

    if ! wait_for_process "$QT_PID_NAME" 10; then
        log_msg "错误：Qt 界面进程未保持运行，日志：$QT_LOG"
        sed -n '1,120p' "$QT_LOG" 2>/dev/null || true
        return 1
    fi

    if ! wait_for_qt_boot_surface 8; then
        log_msg "错误：Qt splash 启动阶段未就绪，日志：$QT_LOG"
        sed -n '1,160p' "$QT_LOG" 2>/dev/null || true
        return 1
    fi

    return 0
}

start_stack()
{
    stop_legacy_display
    hide_display_console
    show_boot_splash
    if ! start_qt_shell; then
        log_msg "overlay 启动失败，恢复 GStreamer drop6 可见线"
        stop_processes_by_name "$QT_PID_NAME" "$OVERLAY_PID_NAME"
        restore_fallback_pipeline
        sleep 3
        status_stack
        return 1
    fi
    if ! start_overlay; then
        log_msg "Qt 界面启动失败，恢复 GStreamer drop6 可见线"
        stop_processes_by_name "$QT_PID_NAME" "$OVERLAY_PID_NAME"
        restore_fallback_pipeline
        sleep 3
        status_stack
        return 1
    fi
    status_stack
}

stop_stack()
{
    stop_processes_by_name "$QT_PID_NAME" "$OVERLAY_PID_NAME"
    log_msg "已停止 Qt KMS overlay 链路"
}

restore_fallback()
{
    stop_legacy_display
    restore_fallback_pipeline
    sleep 3
    log_msg "恢复线 PID: $(pidof gst-launch-1.0 2>/dev/null || echo none)"
}

status_stack()
{
    log_msg "Qt PID: $(pidof "$QT_PID_NAME" 2>/dev/null || echo none)"
    log_msg "Overlay PID: $(pidof "$OVERLAY_PID_NAME" 2>/dev/null || echo none)"
    log_msg "GStreamer fallback PID: $(pidof gst-launch-1.0 2>/dev/null || echo none)"
    status_legacy_display
    log_msg "Overlay: plane=$KMS_OVERLAY_PLANE rect=${KMS_OVERLAY_X},${KMS_OVERLAY_Y},${KMS_OVERLAY_W},${KMS_OVERLAY_H}"
    log_msg "Overlay control socket: $OVERLAY_CONTROL_SOCKET"
    log_msg "Logs: qt=$QT_LOG overlay=$OVERLAY_LOG fallback=$FALLBACK_LOG"
}

usage()
{
    echo "用法：$0 {start|stop|restart|status|restore-fallback}"
}

case "${1:-start}" in
    start)
        start_stack
        ;;
    stop)
        stop_stack
        ;;
    restart)
        stop_stack
        start_stack
        ;;
    status)
        status_stack
        ;;
    restore-fallback)
        restore_fallback
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac
