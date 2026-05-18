#!/bin/sh
#
# 作用：
#   对 Qt + KMS overlay 正规化交付物做轻量静态检查。
#   这个脚本不访问开发板硬件，只验证源码、构建脚本和运行控制脚本的关键契约是否存在。
#
# 主要流程：
#   1. 确认可维护的 KMS overlay C 源码已经进入项目目录。
#   2. 确认构建脚本会使用交叉编译器和 libdrm 生成板端程序。
#   3. 确认运行控制脚本使用 nohup 后台启动 overlay 与 Qt，并保留 start/stop/status 控制面。
#   4. 确认 KMS overlay 界面保留触摸控制按钮，并且启动脚本默认允许 Qt 接收触摸事件。
#
# 返回值：
#   所有检查通过返回 0；任一契约缺失返回 1。

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

require_file()
{
    path="$SCRIPT_DIR/$1"
    [ -f "$path" ] || fail "缺少文件：$1"
}

require_grep()
{
    pattern="$1"
    file="$2"
    grep -Eq -- "$pattern" "$SCRIPT_DIR/$file" || fail "$file 缺少模式：$pattern"
}

require_file "uvc_kms_overlay.c"
require_file "build_uvc_kms_overlay.sh"
require_file "run_qt_kms_overlay_display.sh"
require_file "fb_boot_splash.c"
require_file "build_fb_boot_splash.sh"
require_file "qml/Main.qml"
require_file "../S90uvc-camera"
require_file "../S05display-quiet"
require_file "main.cpp"
require_file "defect-cos-upload"
require_file "defect_classify.cpp"
require_file "build_defect_classify.sh"
require_file "defect_segment.cpp"
require_file "build_defect_segment.sh"
require_file "deploy_qt_camera_display.sh"

require_grep "drmModeSetPlane" "uvc_kms_overlay.c"
require_grep "x dst-x" "uvc_kms_overlay.c"
require_grep "KMS_OVERLAY_X" "run_qt_kms_overlay_display.sh"
require_grep "KMS_OVERLAY_Y" "run_qt_kms_overlay_display.sh"
require_grep "KMS_OVERLAY_W" "run_qt_kms_overlay_display.sh"
require_grep "KMS_OVERLAY_H" "run_qt_kms_overlay_display.sh"
require_grep "FB_BOOT_SPLASH_BIN" "run_qt_kms_overlay_display.sh"
require_grep "show_boot_splash" "run_qt_kms_overlay_display.sh"
require_grep "fb_boot_splash" "run_qt_kms_overlay_display.sh"
require_grep 'nohup "\$OVERLAY_BIN"' "run_qt_kms_overlay_display.sh"
require_grep "VIDEO_BACKEND=kms-overlay" "run_qt_kms_overlay_display.sh"
require_grep "start\\|stop\\|restart\\|restart-overlay\\|status" "run_qt_kms_overlay_display.sh"
require_grep "restart_overlay_only" "run_qt_kms_overlay_display.sh"
require_grep "overlay 已单独重启并保持隐藏" "run_qt_kms_overlay_display.sh"
require_grep "hide_display_console" "run_qt_kms_overlay_display.sh"
require_grep "fbcon/cursor_blink" "run_qt_kms_overlay_display.sh"
require_grep "wait_for_node" "run_qt_kms_overlay_display.sh"
require_grep "send_overlay_command" "run_qt_kms_overlay_display.sh"
require_grep "VISIBLE 0" "run_qt_kms_overlay_display.sh"
require_grep "VISIBLE 1" "run_qt_kms_overlay_display.sh"
require_grep "-V 0" "run_qt_kms_overlay_display.sh"
require_grep "hide_display_console" "../S90uvc-camera"
require_grep "fbcon/cursor_blink" "../S90uvc-camera"
require_grep "show_boot_splash" "../S90uvc-camera"
require_grep "fb_boot_splash" "../S90uvc-camera"
require_grep "fbcon/cursor_blink" "../S05display-quiet"
require_grep "/dev/tty0" "../S05display-quiet"
require_grep "show_boot_splash" "../S05display-quiet"
require_grep "fb_boot_splash" "../S05display-quiet"
require_grep "S05display-quiet" "deploy_qt_camera_display.sh"
require_grep "S90uvc-camera" "deploy_qt_camera_display.sh"
require_grep "FB_SPLASH_SRC" "deploy_qt_camera_display.sh"
require_grep "fb_boot_splash" "deploy_qt_camera_display.sh"
require_grep "uvc_kms_overlay.c" "build_uvc_kms_overlay.sh"
require_grep "ldrm" "build_uvc_kms_overlay.sh"
require_grep "OVERLAY_CC" "build_uvc_kms_overlay.sh"
require_grep "fb_boot_splash.c" "build_fb_boot_splash.sh"
require_grep "SPLASH_CC" "build_fb_boot_splash.sh"
if grep -Eq 'CC="\$\{CC:-' "$SCRIPT_DIR/build_fb_boot_splash.sh"; then
    fail "build_fb_boot_splash.sh 不能继承外部 CC，避免 ST Qt SDK 的 CC='编译器 参数' 破坏 splash 构建"
fi
if grep -Eq 'CC="\$\{CC:-' "$SCRIPT_DIR/build_uvc_kms_overlay.sh"; then
    fail "build_uvc_kms_overlay.sh 不能继承外部 CC，避免 ST Qt SDK 的 CC='编译器 参数' 破坏 overlay 构建"
fi

start_stack_block="$(sed -n '/^start_stack()$/,/^}$/p' "$SCRIPT_DIR/run_qt_kms_overlay_display.sh")"
qt_start_line="$(printf '%s\n' "$start_stack_block" | grep -n 'if ! start_qt_shell' | tail -n 1 | cut -d: -f1)"
overlay_start_line="$(printf '%s\n' "$start_stack_block" | grep -n 'if ! start_overlay' | tail -n 1 | cut -d: -f1)"
if [ -z "$qt_start_line" ] || [ -z "$overlay_start_line" ] || [ "$qt_start_line" -ge "$overlay_start_line" ]; then
    fail "run_qt_kms_overlay_display.sh 必须先启动 overlay 并隐藏视频 plane，再启动 Qt splash，避免摄像头画面抢在启动动画前显示"
fi
require_grep "wait_for_qt_boot_surface" "run_qt_kms_overlay_display.sh"
require_grep "boot overlay visible false" "run_qt_kms_overlay_display.sh"

require_grep "id: overlayControls" "qml/Main.qml"
require_grep "开始" "qml/Main.qml"
require_grep "暂停" "qml/Main.qml"
require_grep "继续" "qml/Main.qml"
require_grep "停止" "qml/Main.qml"
require_grep "安全卸载" "qml/Main.qml"
require_grep "检测" "qml/Main.qml"
require_grep "detectImageBusy" "qml/Main.qml"
require_grep "requestDetectCurrentFrame" "qml/Main.qml"
require_grep "detectClassificationReady" "qml/Main.qml"
require_grep "detectModelsReady" "qml/Main.qml"
require_grep "detectCurrentFrameFinished" "qml/Main.qml"
require_grep "detectStatus" "qml/Main.qml"
require_grep "detectConfidenceText" "qml/Main.qml"
require_grep "handleDetectAction" "qml/Main.qml"
require_grep "storageToastTimer" "qml/Main.qml"
require_grep "storageToastVisible" "qml/Main.qml"
require_grep "showStorageToast" "qml/Main.qml"
require_grep "historyPageVisible" "qml/Main.qml"
require_grep "historyDetailVisible" "qml/Main.qml"
require_grep "historyListView" "qml/Main.qml"
require_grep "historyListPanel" "qml/Main.qml"
require_grep "historyDetailPanel" "qml/Main.qml"
require_grep "backToHistoryList" "qml/Main.qml"
require_grep "deleteHistoryRecord" "qml/Main.qml"
require_grep "cloudStatusSummary" "qml/Main.qml"
require_grep "historyMetricGrid" "qml/Main.qml"
require_grep "historyAnalysisPanel" "qml/Main.qml"
require_grep "historyReadableInspectionText" "qml/Main.qml"
require_grep "historyConfidenceSummaryText" "qml/Main.qml"
require_grep "检测结论" "qml/Main.qml"
require_grep "可信度" "qml/Main.qml"
require_grep "缺陷提示" "qml/Main.qml"
require_grep "原始图片" "main.cpp"
require_grep "本地图片位置" "qml/Main.qml"
if grep -Eq '分类原图' "$SCRIPT_DIR/main.cpp" "$SCRIPT_DIR/qml/Main.qml"; then
    fail "历史详情面向操作员展示时不能继续使用“分类原图”，应显示为“原始图片”"
fi
history_detail_metrics="$(sed -n '/id: historyMetricGrid/,/id: historyAnalysisPanel/p' "$SCRIPT_DIR/qml/Main.qml")"
if echo "$history_detail_metrics" | grep -Eq '分类图|检测图总量|流程状态'; then
    fail "历史详情右侧指标只允许展示上传时间、记录ID、图片数量和云端编号，不能继续显示分类图、检测图总量或流程状态"
fi
require_grep "statsPageVisible" "qml/Main.qml"
require_grep "statsSummary" "qml/Main.qml"
require_grep "statsRecentBars" "qml/Main.qml"
require_grep "statsDistributionBars" "qml/Main.qml"
require_grep "statsRecentRows" "qml/Main.qml"
require_grep "openHistoryDetailFromStats" "qml/Main.qml"
require_grep "id: statsPage" "qml/Main.qml"
require_grep "id: statsKpiGrid" "qml/Main.qml"
require_grep "deviceHealth.networkStatusText" "qml/Main.qml"
require_grep "deviceHealth.cameraStatusText" "qml/Main.qml"
require_grep "deviceHealth.f4StatusText" "qml/Main.qml"
require_grep "onCameraStatusChanged" "qml/Main.qml"
require_grep "bootOverlayRestoreFinished" "qml/Main.qml"
require_grep "id: statsTrendPanel" "qml/Main.qml"
require_grep "id: statsDistributionPanel" "qml/Main.qml"
require_grep "id: statsCloudPanel" "qml/Main.qml"
require_grep "id: statsRecentPanel" "qml/Main.qml"
require_grep "id: statsRecentListView" "qml/Main.qml"
require_grep "manualPageVisible" "qml/Main.qml"
require_grep "manualMode" "qml/Main.qml"
require_grep "manualCommandLog" "qml/Main.qml"
require_grep "handleManualAction" "qml/Main.qml"
require_grep "appendManualCommandLog" "qml/Main.qml"
require_grep "id: manualPage" "qml/Main.qml"
require_grep "id: manualBeltPanel" "qml/Main.qml"
require_grep "id: manualArmPanel" "qml/Main.qml"
require_grep "id: manualLightPanel" "qml/Main.qml"
require_grep "id: manualSafetyPanel" "qml/Main.qml"
require_grep "id: manualCommandLogView" "qml/Main.qml"
require_grep "夹爪开" "qml/Main.qml"
require_grep "夹爪关" "qml/Main.qml"
require_grep "背光常亮" "qml/Main.qml"
require_grep "settingsPageVisible" "qml/Main.qml"
require_grep "alarmPageVisible" "qml/Main.qml"
require_grep "splashOverlayVisible" "qml/Main.qml"
require_grep "splashStageIndex" "qml/Main.qml"
require_grep "splashStageModel" "qml/Main.qml"
require_grep "advanceSplashStage" "qml/Main.qml"
require_grep "finishSplashAnimation" "qml/Main.qml"
require_grep "storageController.setOverlayVisible\\(false\\)" "qml/Main.qml"
require_grep "storageController.setOverlayVisible\\(true\\)" "qml/Main.qml"
require_grep "startBootOverlayRestore" "qml/Main.qml"
require_grep "bootOverlayRestoreTimer" "qml/Main.qml"
require_grep "root.bootOverlayRestoreFinished = true" "qml/Main.qml"
require_grep "root.bootOverlayRestoreFinished && !root.splashOverlayVisible && root.activePage === \"home\"" "qml/Main.qml"
switch_page_block="$(sed -n '/^    function switchPage(pageName)/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$switch_page_block" | grep -q 'pageName === "home"' ||
        ! printf '%s\n' "$switch_page_block" | grep -q 'root.bootOverlayRestoreFinished' ||
        ! printf '%s\n' "$switch_page_block" | grep -q '!root.splashOverlayVisible'; then
    fail "switchPage() 返回首页恢复 overlay 时必须同时检查 pageName、bootOverlayRestoreFinished 和 splashOverlayVisible，避免摄像头早于 Qt 启动画面显示"
fi
require_grep "id: splashOverlay" "qml/Main.qml"
require_grep "id: splashScanLine" "qml/Main.qml"
require_grep "id: splashProgressFill" "qml/Main.qml"
require_grep "工业缺陷检测系统" "qml/Main.qml"
require_grep "STM32MP157 Vision Inspection Terminal" "qml/Main.qml"
require_grep "加载相机" "qml/Main.qml"
require_grep "初始化检测模型" "qml/Main.qml"
require_grep "连接运动控制" "qml/Main.qml"
require_grep "挂载存储" "qml/Main.qml"
require_grep "进入检测界面" "qml/Main.qml"
require_grep "settingsApplyAction" "qml/Main.qml"
require_grep "id: settingsPage" "qml/Main.qml"
require_grep "id: settingsProcessPanel" "qml/Main.qml"
require_grep "id: settingsVisionPanel" "qml/Main.qml"
require_grep "id: settingsMotionPanel" "qml/Main.qml"
require_grep "id: settingsStoragePanel" "qml/Main.qml"
require_grep "id: settingsActionPanel" "qml/Main.qml"
require_grep "参数摘要" "qml/Main.qml"
require_grep "恢复默认" "qml/Main.qml"
require_grep "alarmHistoryModel" "qml/Main.qml"
require_grep "handleAlarmAction" "qml/Main.qml"
require_grep "id: alarmPage" "qml/Main.qml"
require_grep "id: alarmCurrentPanel" "qml/Main.qml"
require_grep "id: alarmHealthPanel" "qml/Main.qml"
require_grep "id: alarmHistoryPanel" "qml/Main.qml"
require_grep "id: alarmAdvicePanel" "qml/Main.qml"
require_grep "0x0007" "qml/Main.qml"
require_grep "保存诊断" "qml/Main.qml"
if grep -Eq "吸盘|背光关|背光开|backlight-toggle|manualBacklightEnabled" "$SCRIPT_DIR/qml/Main.qml"; then
    fail "qml/Main.qml 手动控制页显示必须符合硬件事实：背光常亮，末端执行器只显示夹爪，不显示吸盘或背光开关"
fi
require_grep "上下滑动查看更多" "qml/Main.qml"
require_grep "uploadHistory" "qml/Main.qml"
require_grep "selectedHistoryIndex" "qml/Main.qml"
require_grep "selectedHistoryRecord" "qml/Main.qml"
require_grep "imageCarousel" "qml/Main.qml"
require_grep "flickDeceleration" "qml/Main.qml"
require_grep "maximumFlickVelocity" "qml/Main.qml"
require_grep "setOverlayVisible" "qml/Main.qml"
require_grep "safeRemoveSdCard" "qml/Main.qml"
require_grep "saveAlarmSnapshotToSdCard" "qml/Main.qml"
require_grep "/mnt/sdcard/logs/qt_alarm_snapshot.txt" "qml/Main.qml"
require_grep "MouseArea" "qml/Main.qml"
require_grep "DEFAULT_BOARD_TIME_ZONE" "main.cpp"
require_grep "qputenv\\(\"TZ\", DEFAULT_BOARD_TIME_ZONE\\)" "main.cpp"
require_grep "CameraStorageController" "main.cpp"
require_grep "DeviceHealthController" "main.cpp"
require_grep "networkStatusText" "main.cpp"
require_grep "cameraStatusText" "main.cpp"
require_grep "f4StatusText" "main.cpp"
require_grep "startNetworkProbe" "main.cpp"
require_grep "handleNetworkProcessError" "main.cpp"
require_grep "handleCloudProcessError" "main.cpp"
require_grep "m_healthTimer\\.setInterval\\(8000\\)" "main.cpp"
require_grep "refreshOverlayCameraStatus" "main.cpp"
require_grep "queryOverlayStatus" "main.cpp"
require_grep "applyOverlayStatusReply" "main.cpp"
require_grep "相机画面停滞" "main.cpp"
require_grep "restart-overlay" "main.cpp"
require_grep "startF4Probe" "main.cpp"
require_grep "startDetachedOverlay" "main.cpp"
require_grep "saveCurrentFrameToSdCard" "main.cpp"
require_grep "requestSaveCurrentFrameToSdCard" "main.cpp"
require_grep "saveCurrentFrameFinished" "main.cpp"
require_grep "requestDetectCurrentFrame" "main.cpp"
require_grep "detectClassificationReady" "main.cpp"
require_grep "detectModelsReady" "main.cpp"
require_grep "detectCurrentFrameFinished" "main.cpp"
require_grep "detectInProgress" "main.cpp"
require_grep "SAVE_DETECT" "main.cpp"
require_grep "DEFAULT_DEFECT_CLASSIFY_BIN" "main.cpp"
require_grep "DEFAULT_DEFECT_CLASSIFY_MODEL" "main.cpp"
require_grep "DEFAULT_DEFECT_CLASSIFY_LABELS" "main.cpp"
require_grep "DEFAULT_DEFECT_SEGMENT_BIN" "main.cpp"
require_grep "DEFAULT_DEFECT_SEGMENT_MODEL" "main.cpp"
require_grep "defect_unet_test_decoder_head_int8.onnx" "main.cpp"
require_grep "runDefectSegment" "main.cpp"
require_grep "appendDetectHistoryRecord" "main.cpp"
require_grep "detectCurrentFrameForSelfTest" "main.cpp"
require_grep "detect-self-test" "main.cpp"
require_grep "run_detect_self_test" "main.cpp"
require_grep "annotatedPaths" "main.cpp"
require_grep "sourcePath" "main.cpp"
require_grep "source_size_bytes" "main.cpp"
require_grep "classification_result" "main.cpp"
require_grep "segmentation_result" "main.cpp"
require_grep "safeRemoveSdCard" "main.cpp"
require_grep "saveAlarmSnapshotToSdCard" "main.cpp"
require_grep "DEFAULT_SDCARD_LOG_DIR" "main.cpp"
require_grep "DEFAULT_ALARM_SNAPSHOT_FILE" "main.cpp"
require_grep "storage action alarm-snapshot" "main.cpp"
require_grep "alarm-snapshot-self-test" "main.cpp"
require_grep "run_alarm_snapshot_self_test" "main.cpp"
require_grep "fsync" "main.cpp"
require_grep "UploadHistoryModel" "main.cpp"
require_grep "appendUploadHistoryRecord" "main.cpp"
require_grep "removeRecord" "main.cpp"
require_grep "removeHistoryImageFiles" "main.cpp"
require_grep "compactUploadStatus" "main.cpp"
require_grep "uploadHistory" "main.cpp"
require_grep "upload_history.json" "main.cpp"
require_grep "setOverlayVisible" "main.cpp"
require_grep "storage-self-test" "main.cpp"
require_grep "storage action save-image" "main.cpp"
require_grep "std::fopen\\(\"/proc/mounts\", \"r\"\\)" "main.cpp"
require_grep "sdcard-safe-remove" "main.cpp"

device_health_block="$(sed -n '/^class DeviceHealthController : public QObject$/,/^};$/p' "$SCRIPT_DIR/main.cpp")"
if printf '%s\n' "$device_health_block" | grep -q 'waitForStarted'; then
    fail "DeviceHealthController 健康检测不能调用 waitForStarted，否则 4G/云端刷新可能短暂卡住 QML 主线程"
fi

network_probe_block="$(sed -n '/^    void startNetworkProbe()$/,/^    }$/p' "$SCRIPT_DIR/main.cpp")"
if printf '%s\n' "$network_probe_block" | grep -q 'setNetworkStatus(QStringLiteral("检测中")'; then
    fail "4G 周期刷新不能每轮先显示“检测中”，应保留上一轮稳定状态，避免顶部网络状态在“检测中/在线”之间闪烁"
fi

cloud_probe_block="$(sed -n '/^    void startCloudProbe()$/,/^    }$/p' "$SCRIPT_DIR/main.cpp")"
if printf '%s\n' "$cloud_probe_block" | grep -q 'setCloudStatus(QStringLiteral("检测中")'; then
    fail "云端周期刷新不能每轮先显示“检测中”，应保留上一轮稳定状态，避免云端状态在“检测中/已连接”之间闪烁"
fi

require_grep "defect_classify.cpp" "build_defect_classify.sh"
require_grep "defect_segment.cpp" "build_defect_segment.sh"
require_grep "onnxruntime_cxx_api.h" "defect_classify.cpp"
require_grep "onnxruntime_cxx_api.h" "defect_segment.cpp"
require_grep "defect_classifier_static_mixed_int8.onnx" "defect_classify.cpp"
require_grep "defect_unet_test_decoder_head_int8.onnx" "defect_segment.cpp"
require_grep "RESULT_SEG" "defect_segment.cpp"
require_grep "overlay_path" "defect_segment.cpp"
require_grep "mask_path" "defect_segment.cpp"
require_grep "defect_classifier_static_mixed_int8.onnx" "deploy_qt_camera_display.sh"
require_grep "defect_classifier_static_mixed_int8_labels.json" "deploy_qt_camera_display.sh"
require_grep "defect_unet_test_decoder_head_int8.onnx" "deploy_qt_camera_display.sh"
require_grep "defect-classify" "deploy_qt_camera_display.sh"
require_grep "defect-segment" "deploy_qt_camera_display.sh"
require_grep "libonnxruntime.so" "deploy_qt_camera_display.sh"

if grep -Eq '"保存图片"|requestSaveCurrentFrameToSdCard\(\)|"save-image"' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "qml/Main.qml 首页不应再暴露独立保存图片按钮；检测按钮要替代保存并生成历史记录"
fi

require_grep 'arguments << QStringLiteral\("--annotated"\) << path' "main.cpp"
require_grep 'bundle->annotatedPaths << rawPath << overlayPath << maskPath' "main.cpp"

if grep -Eq 'currentIndex:[[:space:]]*root\.selectedHistoryIndex' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "historyListView 不能把 currentIndex 绑定到 selectedHistoryIndex，否则点选卡片会触发 ListView 自动滚动"
fi

if grep -Eq 'highlightRangeMode:[[:space:]]*ListView\.ApplyRange' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "historyListView 不能使用 ApplyRange 强制高亮范围，否则选中历史卡片会把列表拉到当前项"
fi

if grep -Eq 'storageState[[:space:]]*=[[:space:]]*storageController\.saveCurrentFrameToSdCard[[:space:]]*\(' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "QML 不能在主线程同步调用 saveCurrentFrameToSdCard()，检测流程必须通过 requestDetectCurrentFrame() 后台执行"
fi
require_grep "detectPartName" "qml/Main.qml"
require_grep "detectConfidencePercentText" "qml/Main.qml"
require_grep "total_time_ms" "qml/Main.qml"
if grep -Eq '/1000' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "qml/Main.qml 首页检测置信度不能继续显示千分制 /1000，必须改为 0-100 百分制"
fi
require_grep "uvc-kms-overlay-control.sock" "uvc_kms_overlay.c"
require_grep "工业缺陷检测系统" "fb_boot_splash.c"
require_grep "STM32MP157 Vision Inspection Terminal" "fb_boot_splash.c"
require_grep "/dev/fb0" "fb_boot_splash.c"
require_grep "FBIOGET_VSCREENINFO" "fb_boot_splash.c"
require_grep "mmap" "fb_boot_splash.c"
require_grep "draw_splash" "fb_boot_splash.c"
require_grep "msync" "fb_boot_splash.c"
require_grep "VISIBLE " "uvc_kms_overlay.c"
require_grep "STATUS" "uvc_kms_overlay.c"
require_grep "handle_status_command" "uvc_kms_overlay.c"
require_grep "set_kms_plane_visible" "uvc_kms_overlay.c"
require_grep "initial_visible" "uvc_kms_overlay.c"
require_grep "初始视频层可见性" "uvc_kms_overlay.c"
require_grep "SAVE " "uvc_kms_overlay.c"
require_grep "SAVE_DUAL " "uvc_kms_overlay.c"
require_grep "SAVE_DETECT " "uvc_kms_overlay.c"
require_grep "write_latest_frame_as_jpeg" "uvc_kms_overlay.c"
require_grep "/tmp/qt-defect-detect" "uvc_kms_overlay.c"
require_grep "DEFAULT_DETECT_ROI_SIZE" "uvc_kms_overlay.c"
require_grep "draw_roi_overlay_row" "uvc_kms_overlay.c"
require_grep "copy_yuyv_frame_to_rgb24" "uvc_kms_overlay.c"
require_grep "yuyv_map" "uvc_kms_overlay.c"
require_grep "P6" "uvc_kms_overlay.c"
require_grep "write_rgb24_as_jpeg" "uvc_kms_overlay.c"
require_grep "write_rgb24_as_png" "uvc_kms_overlay.c"
require_grep "write_latest_frame_as_jpeg_and_png" "uvc_kms_overlay.c"
require_grep "jpeglib.h" "uvc_kms_overlay.c"
require_grep "png.h" "uvc_kms_overlay.c"
require_grep "fsync" "uvc_kms_overlay.c"
require_grep "ljpeg" "build_uvc_kms_overlay.sh"
require_grep "lpng" "build_uvc_kms_overlay.sh"

if grep -Eq 'refresh_clean_rgb24_snapshot' "$SCRIPT_DIR/uvc_kms_overlay.c"; then
    fail "ROI 框不能通过每帧 clean_rgb24 快照规避检测污染；这会增加内存带宽并造成预览卡顿"
fi

if grep -Eq 'draw_roi_overlay_box\(kms,' "$SCRIPT_DIR/uvc_kms_overlay.c"; then
    fail "ROI 框不能在整帧转换完成后再补画，否则单 framebuffer 扫描时会肉眼闪烁"
fi

require_grep "defect-cos-upload" "main.cpp"
require_grep "--annotated" "main.cpp"
require_grep "cloudResultFromClassificationResult" "main.cpp"
require_grep "CLOUD_RESULT" "main.cpp"
require_grep "total_time_ms" "main.cpp"
require_grep "source" "defect-cos-upload"
require_grep "annotated" "defect-cos-upload"
require_grep "--annotated" "defect-cos-upload"
require_grep "validate_cloud_result" "defect-cos-upload"
require_grep "detect_content_type" "defect-cos-upload"
require_grep "ANNOTATED_FILES" "defect-cos-upload"
require_grep "annotated_index" "defect-cos-upload"
require_grep "uploads/cos/prepare" "defect-cos-upload"
require_grep 'records/\$record_id/files' "defect-cos-upload"
require_grep "CLOUD_UPLOAD_ENV_FILE" "defect-cos-upload"
require_grep "cos-upload.env" "defect-cos-upload"
require_grep "CLOUD_UPLOAD_ENV_FILE" "deploy_qt_camera_display.sh"
require_grep "cos-upload.env" "deploy_qt_camera_display.sh"
require_grep "install -m 600" "deploy_qt_camera_display.sh"
require_grep "QT_QPA_EGLFS_DISABLE_INPUT=\"\\$\\{QT_QPA_EGLFS_DISABLE_INPUT:-0\\}\"" "run_qt_camera_display.sh"
require_grep "pick_touch_dev" "run_qt_camera_display.sh"
require_grep "QT_QPA_GENERIC_PLUGINS" "run_qt_camera_display.sh"
require_grep "QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS" "run_qt_camera_display.sh"
require_grep "QT_QPA_EGLFS_DISABLE_INPUT=\"\\$\\{QT_QPA_EGLFS_DISABLE_INPUT:-0\\}\"" "run_qt_kms_overlay_display.sh"
require_grep "TZ=\"\\$\\{TZ:-CST-8\\}\"" "run_qt_camera_display.sh"
require_grep "TZ=\"\\$\\{TZ:-CST-8\\}\"" "run_qt_kms_overlay_display.sh"
require_grep "UVC_BACKEND=\"\\$\\{UVC_BACKEND:-qt-kms-overlay\\}\"" "../S90uvc-camera"

# 运行上传脚本内置 JSON 解析自检，复现“创建记录返回新 id，但嵌套 part/device id 把解析结果带回旧记录”的回归场景。
sh "$SCRIPT_DIR/defect-cos-upload" --self-test-json-parser

# 运行上传脚本参数解析自检，确认 --annotated 同时支持 JPG/PNG 且能保留多张检测结果图。
sh "$SCRIPT_DIR/defect-cos-upload" --self-test-args

echo "PASS: Qt KMS overlay assets contract"
