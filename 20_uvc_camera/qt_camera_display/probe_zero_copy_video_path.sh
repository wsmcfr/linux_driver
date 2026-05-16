#!/bin/sh
#
# 作用：
#   在 STM32MP157 开发板上探测 UVC 摄像头到硬件显示端的零拷贝/硬件视频链路。
#   脚本会收集 V4L2、DRM/KMS、Wayland、GStreamer 插件和内核日志证据，
#   并优先短时验证 “v4l2src io-mode=dmabuf -> kmssink” 这条独立硬件显示路径。
#
# 主要流程：
#   1. 自动选择 /dev/video0~3 中存在的 UVC 节点，或使用 VIDEO_DEV 指定节点。
#   2. 记录 /dev/video*、/dev/dri/card0、/dev/fb0、/dev/galcore 等硬件节点状态。
#   3. 记录 v4l2-ctl 与 gst-inspect 输出，确认摄像头格式和 sink 插件能力。
#   4. 先用 fakesink 验证 V4L2 DMABUF 捕获是否能跑，再用 kmssink 验证硬件显示。
#   5. 每条短时管线运行时采样 gst-launch-1.0 进程 CPU，并检查 dmesg 是否出现 Oops。
#
# 可调变量：
#   VIDEO_DEV：指定摄像头节点，默认自动选择第一个 /dev/videoN。
#   CAMERA_WIDTH/CAMERA_HEIGHT/CAMERA_FPS：测试分辨率和帧率，默认 640x480@15fps。
#   PIPELINE_SECONDS：每条显示管线运行秒数，默认 10 秒。
#   CPU_SAMPLE_SECONDS：CPU 采样窗口，默认 5 秒。
#   KMS_EXTRA：传给 kmssink 的额外属性，默认指定 STM32 DRM 驱动名 driver-name=stm。
#   RUN_KMS_CONVERT_TEST：设为 1 时运行 videoconvert->BGRA->kmssink 基线测试，默认启用。
#   RUN_WAYLAND_TEST：设为 1 时才运行 waylandsink 短时测试。
#   RUN_GL_TEST：设为 1 时运行 glupload->glimagesink 短时测试，默认启用。
#   STOP_EXISTING_PREVIEW：设为 1 时先停止旧 Qt/Framebuffer/GStreamer 预览，默认启用。
#   LOG_DIR：日志目录，默认 /root/qt_camera_display/logs。

# set -u 用于尽早发现变量名拼写错误；不使用 set -e，避免某个探测命令失败后丢失后续证据。
set -u

# PATH 加入常见系统目录，保证 modprobe、dmesg、gst-launch-1.0 等命令可被找到。
PATH=/sbin:/bin:/usr/sbin:/usr/bin:$PATH
export PATH

# 这些库路径与 run_qt_camera_display.sh 保持一致，确保 GStreamer 能找到目标 rootfs 中的插件和 Vivante 库。
export LD_LIBRARY_PATH="/usr/lib:/usr/lib/pulseaudio:/vendor/lib:/lib:${LD_LIBRARY_PATH:-}"
export GST_PLUGIN_PATH="${GST_PLUGIN_PATH:-/usr/lib/gstreamer-1.0}"
export GST_PLUGIN_PATH_1_0="${GST_PLUGIN_PATH_1_0:-$GST_PLUGIN_PATH}"
export GST_PLUGIN_SYSTEM_PATH_1_0="${GST_PLUGIN_SYSTEM_PATH_1_0:-/usr/lib/gstreamer-1.0}"
export GST_REGISTRY="${GST_REGISTRY:-/tmp/gst-registry-zero-copy-probe.bin}"

# XDG_RUNTIME_DIR 是 Wayland/GL 相关组件的运行时目录；即使本轮主测 KMS，也先准备好目录。
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/runtime-root}"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null || true

# 默认测试参数选择 640x480@15fps，对齐当前计划中的最低高清目标。
VIDEO_DEV="${VIDEO_DEV:-}"
CAMERA_WIDTH="${CAMERA_WIDTH:-640}"
CAMERA_HEIGHT="${CAMERA_HEIGHT:-480}"
CAMERA_FPS="${CAMERA_FPS:-15}"
PIPELINE_SECONDS="${PIPELINE_SECONDS:-10}"
CPU_SAMPLE_SECONDS="${CPU_SAMPLE_SECONDS:-5}"
RUN_KMS_CONVERT_TEST="${RUN_KMS_CONVERT_TEST:-1}"
RUN_WAYLAND_TEST="${RUN_WAYLAND_TEST:-0}"
RUN_GL_TEST="${RUN_GL_TEST:-1}"
STOP_EXISTING_PREVIEW="${STOP_EXISTING_PREVIEW:-1}"
KMS_EXTRA="${KMS_EXTRA:-driver-name=stm}"
LOG_DIR="${LOG_DIR:-/root/qt_camera_display/logs}"

# RUN_ID 用时间戳区分每次探测日志，避免覆盖上一轮证据。
RUN_ID="$(date +%Y%m%d-%H%M%S 2>/dev/null || echo manual)"

# LOG_FILE 保存完整命令输出，便于从串口复制关键结论后再回看细节。
LOG_FILE="$LOG_DIR/zero-copy-probe-$RUN_ID.log"

# 创建日志目录；失败时直接退回 /tmp，避免 rootfs 权限问题导致脚本无法运行。
if ! mkdir -p "$LOG_DIR" 2>/dev/null; then
    LOG_DIR="/tmp"
    LOG_FILE="$LOG_DIR/zero-copy-probe-$RUN_ID.log"
fi

# 说明：
#   下面的 log/section/run_cmd 等函数使用 shell 函数实现，是为了让所有输出同时进入屏幕和日志。

# log 的作用：
#   把一行带时间戳的状态同时输出到终端和日志文件。
# 参数：
#   $* 是要记录的状态文本。
# 返回值：
#   始终返回 0；即使 tee 失败，也不影响后续探测。
log()
{
    printf '%s %s\n' "$(date '+%H:%M:%S' 2>/dev/null || echo time)" "$*" | tee -a "$LOG_FILE"
}

# section 的作用：
#   在日志中打印清晰分隔标题，方便后续定位某一类证据。
# 参数：
#   $* 是章节标题。
# 返回值：
#   始终返回 0。
section()
{
    {
        printf '\n'
        printf '========== %s ==========\n' "$*"
    } | tee -a "$LOG_FILE"
}

# run_cmd 的作用：
#   记录并执行一条只读探测命令，把标准输出和标准错误都写入日志。
# 参数：
#   $* 是交给 sh -c 执行的命令字符串。
# 返回值：
#   返回被执行命令的退出码，但调用方通常只记录结果，不因失败停止。
run_cmd()
{
    log "cmd: $*"
    sh -c "$*" >>"$LOG_FILE" 2>&1
    rc=$?
    log "rc=$rc"
    return "$rc"
}

# has_cmd 的作用：
#   判断目标命令是否存在。
# 参数：
#   $1 是命令名称。
# 返回值：
#   命令存在返回 0；不存在返回 1。
has_cmd()
{
    command -v "$1" >/dev/null 2>&1
}

# pick_video_dev 的作用：
#   选择要测试的 UVC 摄像头节点。
# 主要流程：
#   1. 用户指定 VIDEO_DEV 时优先使用并验证它存在。
#   2. 用户未指定时从 /dev/video0 到 /dev/video3 顺序选择第一个存在节点。
# 返回值：
#   成功时打印设备路径并返回 0；失败时返回 1。
pick_video_dev()
{
    if [ -n "$VIDEO_DEV" ]; then
        if [ -e "$VIDEO_DEV" ]; then
            printf '%s\n' "$VIDEO_DEV"
            return 0
        fi

        log "错误：指定的 VIDEO_DEV 不存在：$VIDEO_DEV"
        return 1
    fi

    for dev in /dev/video0 /dev/video1 /dev/video2 /dev/video3; do
        if [ -e "$dev" ]; then
            printf '%s\n' "$dev"
            return 0
        fi
    done

    log "错误：没有找到 /dev/video0~3"
    return 1
}

# read_cpu_total 的作用：
#   读取 /proc/stat 中所有 CPU 时间片总和，用于计算进程 CPU 占比。
# 返回值：
#   打印总时间片；读取失败时打印 0。
read_cpu_total()
{
    awk '/^cpu /{sum=0; for (i=2; i<=NF; i++) sum += $i; print sum; exit}' /proc/stat 2>/dev/null || echo 0
}

# read_proc_ticks 的作用：
#   读取指定进程的用户态和内核态时间片。
# 参数：
#   $1 是进程 PID。
# 返回值：
#   打印 utime+stime；进程已退出时打印 0。
read_proc_ticks()
{
    pid="$1"
    awk '{print $14 + $15}' "/proc/$pid/stat" 2>/dev/null || echo 0
}

# sample_cpu 的作用：
#   对指定 PID 做一段时间 CPU 平均占用采样。
# 参数：
#   $1 是 PID；$2 是日志标签。
# 返回值：
#   进程存在并完成采样返回 0；进程提前退出返回 1。
sample_cpu()
{
    pid="$1"
    label="$2"

    if [ ! -r "/proc/$pid/stat" ]; then
        log "$label CPU: 进程已经退出，无法采样"
        return 1
    fi

    t1="$(read_cpu_total)"
    p1="$(read_proc_ticks "$pid")"
    sleep "$CPU_SAMPLE_SECONDS"

    if [ ! -r "/proc/$pid/stat" ]; then
        log "$label CPU: 采样期间进程退出"
        return 1
    fi

    t2="$(read_cpu_total)"
    p2="$(read_proc_ticks "$pid")"

    awk -v label="$label" -v p1="$p1" -v p2="$p2" -v t1="$t1" -v t2="$t2" \
        'BEGIN {
            if (t2 > t1) {
                printf("%s CPU: %.1f%%\n", label, (p2 - p1) * 100 / (t2 - t1));
            } else {
                printf("%s CPU: 无法计算\n", label);
            }
        }' | tee -a "$LOG_FILE"

    return 0
}

# run_pipeline 的作用：
#   后台运行一条 GStreamer 管线，短时观察退出码、CPU 和内核日志。
# 参数：
#   $1 是路线名称；$2 是完整 gst-launch 命令。
# 返回值：
#   管线正常启动并完成短时采样返回 0；启动失败或提前退出返回非 0。
run_pipeline()
{
    name="$1"
    pipeline="$2"

    section "PIPELINE: $name"
    log "pipeline: $pipeline"

    # 每次管线前记录关键内核日志尾部，便于对比测试前后是否新增 Oops。
    run_cmd "dmesg | grep -Ei 'Oops|galcore|dma_map_sg|uvc|video|drm|kms' | tail -80"

    sh -c "$pipeline" >>"$LOG_FILE" 2>&1 &
    pid=$!
    log "$name pid=$pid"

    # 给 gst-launch 一点启动时间；如果这里已经退出，多半是 caps、插件或设备错误。
    sleep 2
    if ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid"
        rc=$?
        log "$name 提前退出，rc=$rc"
        run_cmd "dmesg | grep -Ei 'Oops|galcore|dma_map_sg|uvc|video|drm|kms' | tail -80"
        return "$rc"
    fi

    sample_cpu "$pid" "$name"

    # 采样结束后继续留出短暂显示时间，方便肉眼确认屏幕是否有画面。
    remain=$((PIPELINE_SECONDS - CPU_SAMPLE_SECONDS - 2))
    if [ "$remain" -gt 0 ]; then
        sleep "$remain"
    fi

    # 短测结束主动停止 gst-launch，避免占住 /dev/video0 或 KMS plane。
    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        sleep 1
    fi

    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null || true
    fi

    wait "$pid" 2>/dev/null
    rc=$?
    log "$name stop rc=$rc"
    run_cmd "dmesg | grep -Ei 'Oops|galcore|dma_map_sg|uvc|video|drm|kms' | tail -120"
    return 0
}

# inspect_element 的作用：
#   对单个 GStreamer element 执行 gst-inspect，缺失时明确记录。
# 参数：
#   $1 是 element 名称，例如 v4l2src 或 kmssink。
# 返回值：
#   element 存在返回 0；缺失返回 1。
inspect_element()
{
    element="$1"

    if ! has_cmd gst-inspect-1.0; then
        log "错误：gst-inspect-1.0 不存在"
        return 1
    fi

    section "GST INSPECT: $element"
    gst-inspect-1.0 "$element" >>"$LOG_FILE" 2>&1
    rc=$?
    log "gst-inspect-1.0 $element rc=$rc"
    return "$rc"
}

# stop_existing_preview 的作用：
#   在硬件视频管线测试前停止可能占用 /dev/video0、/dev/dri/card0 或 /dev/fb0 的旧预览进程。
# 主要流程：
#   1. 如果存在 S90uvc-camera init 脚本，先调用 stop，清理旧 framebuffer 预览的状态文件。
#   2. 再用 killall 兜底停止 qt_camera_display、uvc_fb_preview 和遗留 gst-launch-1.0。
# 返回值：
#   始终返回 0；停止失败会记录日志，但不阻断后续探测。
stop_existing_preview()
{
    if [ "$STOP_EXISTING_PREVIEW" != "1" ]; then
        log "保留现有预览进程：STOP_EXISTING_PREVIEW=$STOP_EXISTING_PREVIEW"
        return 0
    fi

    section "STOP EXISTING PREVIEW"

    if [ -x /etc/init.d/S90uvc-camera ]; then
        run_cmd "/etc/init.d/S90uvc-camera stop"
    fi

    run_cmd "killall qt_camera_display 2>/dev/null || true"
    run_cmd "killall uvc_fb_preview 2>/dev/null || true"
    run_cmd "killall gst-launch-1.0 2>/dev/null || true"
    run_cmd "ps | grep -E 'qt_camera_display|uvc_fb_preview|gst-launch' | grep -v grep || true"
}

section "ZERO COPY HARDWARE VIDEO PROBE"
log "log_file=$LOG_FILE"
log "target=${CAMERA_WIDTH}x${CAMERA_HEIGHT}@${CAMERA_FPS}, pipeline_seconds=$PIPELINE_SECONDS, cpu_sample_seconds=$CPU_SAMPLE_SECONDS"

if ! SELECTED_VIDEO="$(pick_video_dev)"; then
    log "没有摄像头节点，停止管线测试；日志仍保留基础环境信息。"
    SELECTED_VIDEO=""
fi

stop_existing_preview

section "DEVICE AND KERNEL"
run_cmd "uname -a"
run_cmd "cat /proc/cmdline"
run_cmd "ls -l /dev/video* /dev/fb0 /dev/dri/card0 /dev/galcore 2>&1"
run_cmd "lsmod 2>/dev/null || true"
run_cmd "cat /sys/class/drm/card0-*/status 2>/dev/null || true"
run_cmd "dmesg | grep -Ei 'uvc|video|drm|kms|galcore|Oops|dma_map_sg' | tail -160"

section "V4L2 FORMATS"
if [ -n "$SELECTED_VIDEO" ] && has_cmd v4l2-ctl; then
    run_cmd "v4l2-ctl -d '$SELECTED_VIDEO' --list-formats-ext"
else
    log "跳过 v4l2-ctl：没有摄像头节点或 v4l2-ctl 不存在"
fi

inspect_element v4l2src || true
inspect_element kmssink || true
inspect_element waylandsink || true
inspect_element qmlglsink || true
inspect_element glupload || true
inspect_element glimagesink || true

if [ -z "$SELECTED_VIDEO" ]; then
    log "没有可用摄像头节点，本轮只完成环境探测。"
    log "完整日志：$LOG_FILE"
    exit 1
fi

if ! has_cmd gst-launch-1.0; then
    log "错误：gst-launch-1.0 不存在，无法运行管线。"
    log "完整日志：$LOG_FILE"
    exit 1
fi

# CAPS 固定为 YUY2，避免 videoconvert 插入 CPU 像素转换后掩盖零拷贝路线真实成本。
CAPS="video/x-raw,format=YUY2,width=${CAMERA_WIDTH},height=${CAMERA_HEIGHT},framerate=${CAMERA_FPS}/1"

# 先用 fakesink 验证 V4L2 DMABUF 捕获；它不验证显示，只验证 UVC/export 路径。
run_pipeline "dmabuf-fakesink" \
    "gst-launch-1.0 -v v4l2src device=$SELECTED_VIDEO io-mode=dmabuf ! $CAPS ! fakesink sync=false"

# 如果 DMABUF 捕获失败，再用 mmap-fakesink 记录基线，帮助判断失败点在 DMABUF 还是摄像头格式。
run_pipeline "mmap-fakesink-baseline" \
    "gst-launch-1.0 -v v4l2src device=$SELECTED_VIDEO io-mode=mmap ! $CAPS ! fakesink sync=false"

# KMS 是当前优先路线；只有 /dev/dri/card0 存在时才尝试，避免无 DRM 设备时误判插件失败。
if [ -e /dev/dri/card0 ]; then
    run_pipeline "dmabuf-kmssink" \
        "gst-launch-1.0 -v v4l2src device=$SELECTED_VIDEO io-mode=dmabuf ! $CAPS ! kmssink sync=false $KMS_EXTRA"
else
    log "跳过 dmabuf-kmssink：/dev/dri/card0 不存在"
fi

# STM32MP157 当前 KMS plane 只暴露 RGB 类格式时，YUY2 直连 kmssink 会 not-negotiated。
# 这个基线用 CPU videoconvert 转 BGRA，再交给 KMS 扫描输出，用于量化“硬件显示但非零拷贝”的成本。
if [ "$RUN_KMS_CONVERT_TEST" = "1" ] && [ -e /dev/dri/card0 ]; then
    run_pipeline "dmabuf-videoconvert-bgra-kmssink" \
        "gst-launch-1.0 -v v4l2src device=$SELECTED_VIDEO io-mode=dmabuf ! $CAPS ! videoconvert ! video/x-raw,format=BGRA,width=${CAMERA_WIDTH},height=${CAMERA_HEIGHT},framerate=${CAMERA_FPS}/1 ! kmssink sync=false $KMS_EXTRA"
else
    log "跳过 videoconvert->BGRA->kmssink 基线：RUN_KMS_CONVERT_TEST=$RUN_KMS_CONVERT_TEST 或 /dev/dri/card0 不存在"
fi

# Wayland 需要 compositor；默认只 inspect，不自动启动测试，避免没有 WAYLAND_DISPLAY 时卡住。
if [ "$RUN_WAYLAND_TEST" = "1" ]; then
    run_pipeline "dmabuf-waylandsink" \
        "gst-launch-1.0 -v v4l2src device=$SELECTED_VIDEO io-mode=dmabuf ! $CAPS ! waylandsink fullscreen=true sync=false"
else
    log "跳过 waylandsink 管线：RUN_WAYLAND_TEST 未设为 1"
fi

# GL sink 是当前最有希望的低 CPU 路线；仍保留开关，便于排查 galcore 风险时单独关闭。
if [ "$RUN_GL_TEST" = "1" ]; then
    run_pipeline "dmabuf-glimagesink" \
        "gst-launch-1.0 -v v4l2src device=$SELECTED_VIDEO io-mode=dmabuf ! $CAPS ! glupload ! glimagesink sync=false"
else
    log "跳过 glimagesink 管线：RUN_GL_TEST 未设为 1"
fi

section "SUMMARY HINTS"
log "请重点查看 dmabuf-fakesink 和 dmabuf-kmssink 的 rc、CPU、dmesg 是否出现 Oops。"
log "如果 dmabuf-fakesink 失败而 mmap-fakesink 成功，优先查 UVC/V4L2 DMABUF 能力。"
log "如果 dmabuf-kmssink not-negotiated，同时 BGRA KMS 基线可跑，说明 KMS plane 不支持摄像头原始 YUYV。"
log "如果 dmabuf-glimagesink CPU 低且无 Oops，下一步优先补 qmlglsink 并嵌入 Qt Quick。"
log "完整日志：$LOG_FILE"
