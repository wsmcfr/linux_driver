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

require_fixed_grep()
{
    pattern="$1"
    file="$2"
    grep -Fq -- "$pattern" "$SCRIPT_DIR/$file" || fail "$file 缺少固定文本：$pattern"
}

# 作用：确认指定文件中不存在某个正则模式，用于防止旧协议语义或旧界面文案回流。
require_absent()
{
    pattern="$1"
    file="$2"
    if grep -Eq -- "$pattern" "$SCRIPT_DIR/$file"; then
        fail "$file 不应再包含模式：$pattern"
    fi
}

require_file "uvc_kms_overlay.c"
require_file "build_uvc_kms_overlay.sh"
require_file "run_qt_kms_overlay_display.sh"
require_file "fb_boot_splash.c"
require_file "build_fb_boot_splash.sh"
require_file "generate_boot_splash_asset.py"
require_file "boot_splash.rgb565"
require_file "boot_splash.png"
require_file "qml/Main.qml"
require_file "../S90uvc-camera"
require_file "../S05display-quiet"
require_file "../S91board-review-tunnel"
require_file "main.cpp"
require_file "defect-cos-upload"
require_file "board-review-tunnel.sh"
require_file "cloud_check_board_review_tunnel.sh"
require_file "yunduan-board-review-tunnel-check.service"
require_file "yunduan-board-review-tunnel-check.timer"
require_file "cloud_review_result_api.md"
require_file "defect_classify.cpp"
require_file "build_defect_classify.sh"
require_file "defect_segment.cpp"
require_file "build_defect_segment.sh"
require_file "deploy_qt_camera_display.sh"
require_file "qml.qrc"

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
require_grep "S91board-review-tunnel" "deploy_qt_camera_display.sh"
require_grep "board-review-tunnel.sh" "deploy_qt_camera_display.sh"
require_grep "FB_SPLASH_SRC" "deploy_qt_camera_display.sh"
require_grep "FB_SPLASH_ASSET_SRC" "deploy_qt_camera_display.sh"
require_grep "boot_splash.rgb565" "deploy_qt_camera_display.sh"
require_grep "fb_boot_splash" "deploy_qt_camera_display.sh"
require_grep "ServerAliveInterval=30" "board-review-tunnel.sh"
require_grep "ServerAliveCountMax=3" "board-review-tunnel.sh"
require_grep "ExitOnForwardFailure=yes" "board-review-tunnel.sh"
require_grep "id_ed25519_yunfuwu_tunnel" "board-review-tunnel.sh"
require_grep "CLOUD_HOST=\"\\$\\{CLOUD_HOST:-139\\.9\\.35\\.72\\}\"" "board-review-tunnel.sh"
require_grep "127.0.0.1:\\$\\{REMOTE_PORT\\}:\\$\\{LOCAL_HOST\\}:\\$\\{LOCAL_PORT\\}" "board-review-tunnel.sh"
require_grep "monitor_loop" "board-review-tunnel.sh"
require_grep "CHECK_INTERVAL" "board-review-tunnel.sh"
require_grep "/root/qt_camera_display/board-review-tunnel.sh" "../S91board-review-tunnel"
require_grep "127.0.0.1:18081" "cloud_check_board_review_tunnel.sh"
require_grep "OnUnitActiveSec=60s" "yunduan-board-review-tunnel-check.timer"
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
require_grep "不依赖 QML 控制台输出" "run_qt_kms_overlay_display.sh"
if grep -q "boot overlay visible false" "$SCRIPT_DIR/run_qt_kms_overlay_display.sh"; then
    fail "run_qt_kms_overlay_display.sh 不能再依赖 QML 控制台调试文本判断启动阶段"
fi

require_grep "id: overlayControls" "qml/Main.qml"
require_grep "开始" "qml/Main.qml"
require_grep "暂停" "qml/Main.qml"
require_grep "继续" "qml/Main.qml"
require_grep "停止" "qml/Main.qml"
require_grep "安全卸载" "qml/Main.qml"
require_grep "检测" "qml/Main.qml"
require_grep 'compress="0">qml/Main.qml' "qml.qrc"
require_grep 'compress="0">qml/GstVideoSurface.qml' "qml.qrc"
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
require_grep "formatF4ToastText" "qml/Main.qml"
require_grep "F4:" "qml/Main.qml"
require_grep "manualQueuedStopActuator" "qml/Main.qml"
require_grep "flushManualQueuedStop" "qml/Main.qml"
require_grep "manualMotorActionButtons" "qml/Main.qml"
require_grep "回原位" "qml/Main.qml"
require_grep "stepperMotorRoleAddressSummary" "qml/Main.qml"
require_grep "cameraForwardMotor.address = 3" "main.cpp"
require_grep "cameraZMotor.address = 2" "main.cpp"
f4_command_finished_block="$(sed -n '/onF4CommandFinished:/,/onF4ManualCommandFinished:/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$f4_command_finished_block" | grep -q 'formatF4ToastText'; then
    fail "称重标定命令的底部提示必须通过 formatF4ToastText 加 F4: 前缀"
fi
f4_manual_finished_block="$(sed -n '/onF4ManualCommandFinished:/,/onF4AutoControlFinished:/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$f4_manual_finished_block" | grep -q 'formatF4ToastText'; then
    fail "手动传送带 F4 回包底部提示必须通过 formatF4ToastText 加 F4: 前缀"
fi
f4_auto_finished_block="$(sed -n '/onF4AutoControlFinished:/,/onCloudStatusChanged:/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$f4_auto_finished_block" | grep -q 'formatF4ToastText'; then
    fail "首页自动流程 F4 回包底部提示必须通过 formatF4ToastText 加 F4: 前缀"
fi
f4_detail_changed_block="$(sed -n '/onDetailTextChanged:/,/onCloudStatusChanged:/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$f4_detail_changed_block" | grep -q 'formatF4ToastText'; then
    fail "F4 心跳/故障详情变化必须通过 formatF4ToastText 显示到底部提示"
fi
require_grep "LogFileModel" "main.cpp"
require_grep "logFileModel" "main.cpp"
require_grep "refreshLogFileList" "qml/Main.qml"
require_grep "logPageVisible" "qml/Main.qml"
require_grep "logListView" "qml/Main.qml"
require_grep "openLogDetail" "qml/Main.qml"
require_grep "logDetailOverlay" "qml/Main.qml"
require_grep "logDetailFlickable" "qml/Main.qml"
require_grep "日志查看" "qml/Main.qml"
require_grep "/mnt/sdcard/logs" "README.md"
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
require_grep "historyPartNameText" "qml/Main.qml"
require_grep "detectPartNameFromClass" "qml/Main.qml"
require_grep "compactHomeClassText" "qml/Main.qml"
require_grep "compactHomeModelText" "qml/Main.qml"
require_grep "波形垫圈" "qml/Main.qml"
require_grep "平垫圈" "qml/Main.qml"
require_grep "弹性垫圈" "qml/Main.qml"
if grep -Eq 'baseText = "垫片"|baseText = "平垫"|baseText = "弹垫"' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "首页短类别不能继续显示旧命名“垫片/平垫/弹垫”，应显示具体零件类型名称"
fi
part_name_block="$(sed -n '/function detectPartNameFromClass/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$part_name_block" | grep -q 'classText = classText.substring'; then
    fail "首页零件名必须先剥离 good/bad 后缀，再映射中文具体零件类型"
fi
if ! printf '%s\n' "$part_name_block" | grep -q 'return "波形垫圈"'; then
    fail "首页零件名必须把历史训练编码 gasket 映射成波形垫圈"
fi
require_grep "compactHistoryInfoLine" "qml/Main.qml"
require_grep "focusLatestHistoryListRecord" "qml/Main.qml"
require_grep "retryUploadHistoryRecord" "qml/Main.qml"
require_grep "retryUploadRecord" "main.cpp"
require_grep "updateRecordUploadResult" "main.cpp"
require_grep "refreshedUploadTime" "main.cpp"
require_grep "beginMoveRows" "main.cpp"
require_grep "m_entries.move" "main.cpp"
require_grep "updateRecordUploadResult\\(row," "main.cpp"
require_grep "重新发送" "qml/Main.qml"
require_grep "检测结论" "qml/Main.qml"
require_grep "可信度" "qml/Main.qml"
require_grep "缺陷提示" "qml/Main.qml"
require_grep "原始图片" "main.cpp"
if grep -Eq '分类原图' "$SCRIPT_DIR/main.cpp" "$SCRIPT_DIR/qml/Main.qml"; then
    fail "历史详情面向操作员展示时不能继续使用“分类原图”，应显示为“原始图片”"
fi
if grep -Eq 'text:[[:space:]]*"本地图片位置"' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "历史详情不能再把本地图片路径作为独立展示项，路径信息应收进检测信息区内部说明"
fi
history_detail_metrics="$(sed -n '/id: historyMetricGrid/,/id: historyAnalysisPanel/p' "$SCRIPT_DIR/qml/Main.qml")"
if echo "$history_detail_metrics" | grep -Eq '分类图|检测图总量|流程状态'; then
    fail "历史详情右侧指标只允许展示上传时间、记录ID、图片数量和云端编号，不能继续显示分类图、检测图总量或流程状态"
fi
switch_page_block="$(sed -n '/^    function switchPage(pageName)/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if printf '%s\n' "$switch_page_block" | grep -q 'openLatestHistoryDetail()'; then
    fail "进入历史页不能直接打开记录详情，应停留在检测历史列表并聚焦最新卡片"
fi
if ! printf '%s\n' "$switch_page_block" | grep -q 'focusLatestHistoryListRecord()'; then
    fail "进入历史页必须聚焦最新一条检测卡片，不能停留在旧选中记录"
fi
focus_latest_block="$(sed -n '/^    function focusLatestHistoryListRecord()/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if printf '%s\n' "$focus_latest_block" | grep -q 'historyDetailVisible = true'; then
    fail "focusLatestHistoryListRecord() 只能聚焦检测历史列表，不能切换到记录详情"
fi
if ! printf '%s\n' "$focus_latest_block" | grep -q 'historyDetailVisible = false'; then
    fail "focusLatestHistoryListRecord() 进入历史页时必须保持列表层可见"
fi
if ! grep -q 'positionHistoryListAtSelected' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "进入历史页选中最新记录后必须滚动 historyListView，让最新卡片立即可见"
fi
history_analysis_block="$(awk '
    /id: historyAnalysisPanel/ { in_panel = 1 }
    /id: statsPage/ { in_panel = 0 }
    in_panel { print }
' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$history_analysis_block" | grep -q 'clip: true'; then
    fail "历史详情检测信息面板必须裁剪内容，避免长检测信息越界"
fi
if ! printf '%s\n' "$history_analysis_block" | grep -q 'maximumLineCount'; then
    fail "历史详情检测信息文本必须限制行数，避免模型信息过长挤出面板"
fi
if ! printf '%s\n' "$history_analysis_block" | grep -q 'spacing: 1'; then
    fail "历史详情检测信息面板必须使用紧凑行距，避免空行导致末尾内容被截断"
fi
if ! printf '%s\n' "$history_analysis_block" | grep -q 'lineHeight:'; then
    fail "历史详情检测信息文本必须压缩行高，保证四条检测说明都能显示"
fi
if ! printf '%s\n' "$history_analysis_block" | grep -q 'historyAnalysisSummaryText'; then
    fail "历史详情检测信息必须先显示短摘要，避免云端长文直接塞进固定高度面板"
fi
if ! grep -q 'historyFullAnalysisText' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "历史详情必须保留完整检测说明，供操作员打开详情查看完整云端文字"
fi
if ! grep -q 'historyPartNameText(record)' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "历史完整说明里的零件类型必须从历史记录自身解析，不能误用当前首页零件状态"
fi
if ! grep -q 'historyAnalysisDetailOverlay' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "历史详情必须提供完整说明弹层，不能只靠截断文字"
fi
if ! grep -q 'analysisDetailFlickable' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "完整检测说明弹层必须支持滚动，保证长云端文字可读完"
fi
if ! grep -q '查看完整说明' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "历史详情检测信息面板必须提供查看完整说明入口"
fi
home_result_block="$(sed -n '/id: resultPanel/,/\/\* storageControls/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$home_result_block" | grep -q 'compactHomeClassText(root.detectClassName)'; then
    fail "首页类别必须使用紧凑文本，避免完整模型类别名在窄屏结果面板中显示不全"
fi
if ! printf '%s\n' "$home_result_block" | grep -q 'compactHomeModelText(root.detectState)'; then
    fail "首页模型结果必须使用紧凑文本，避免综合判定长句挤出结果面板"
fi
update_upload_block="$(sed -n '/bool updateRecordUploadResult(int row/,/^    }/p' "$SCRIPT_DIR/main.cpp")"
if ! printf '%s\n' "$update_upload_block" | grep -q 'isUploadStatusSuccess(uploadStatus)'; then
    fail "历史重发必须使用统一上传成功判定，避免 record_id 已生成但短状态不含上传成功时仍显示失败"
fi
if ! printf '%s\n' "$update_upload_block" | grep -q 'm_entries.move(row, lastRow)'; then
    fail "历史重发成功后必须把该记录移动到本地历史数组末尾，列表才会显示为最新记录"
fi
if ! printf '%s\n' "$update_upload_block" | grep -Fq 'm_entries[lastRow].uploadTime = refreshedUploadTime'; then
    fail "历史重发成功后必须把本地 upload_time 更新为本次重发完成时间"
fi
retry_finished_block="$(sed -n '/onRetryUploadFinished:/,/^        }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$retry_finished_block" | grep -q 'root.selectedHistoryIndex = uploadHistory.count - 1'; then
    fail "历史重发成功移动记录后，QML 必须重新选中末尾最新记录"
fi
require_grep "isUploadStatusSuccess" "main.cpp"
require_grep "upload_status=OK" "main.cpp"
require_grep "isUploadStatusFailure" "qml/Main.qml"
require_grep "upload_status=OK" "qml/Main.qml"
require_grep "id: globalStorageToastLayer" "qml/Main.qml"
require_grep "z: 900" "qml/Main.qml"
toast_layer_block="$(sed -n '/id: globalStorageToastLayer/,/id: splashOverlay/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$toast_layer_block" | grep -q 'id: storageToast'; then
    fail "底部操作提示必须放在全局浮层内，避免历史、统计、设置等页面底部控件遮挡"
fi
if ! printf '%s\n' "$toast_layer_block" | grep -q 'anchors.bottom: parent.bottom'; then
    fail "全局操作提示必须锚定根窗口底部，不能依赖某个页面内部坐标"
fi
if ! printf '%s\n' "$toast_layer_block" | grep -q 'visible: true'; then
    fail "全局操作提示外层必须显式常驻可见，提示条自身再用 opacity/visible 控制显示和隐藏"
fi
if printf '%s\n' "$toast_layer_block" | grep -q 'visible: storageToast.visible'; then
    fail "全局操作提示外层必须常驻渲染，不能反向绑定内部 storageToast.visible，否则提示可能完全不显示"
fi
if ! grep -q 'verify_status=warning' "$SCRIPT_DIR/defect-cos-upload"; then
    fail "COS 上传脚本必须把上传完成后的详情回查失败降级为成功诊断，避免云端已有记录但本地历史显示上传失败"
fi
require_grep "statsPageVisible" "qml/Main.qml"
require_grep "statsSummary" "qml/Main.qml"
require_grep "statsRecentBars" "qml/Main.qml"
require_grep "statsDistributionBars" "qml/Main.qml"
require_grep "statsDistributionLeftBars" "qml/Main.qml"
require_grep "statsDistributionRightBars" "qml/Main.qml"
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
require_grep "id: statsDistributionLeftColumn" "qml/Main.qml"
require_grep "id: statsDistributionRightColumn" "qml/Main.qml"
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
require_grep "id: manualAssistPanel" "qml/Main.qml"
require_grep "id: manualSafetyPanel" "qml/Main.qml"
require_grep "id: manualCommandLogView" "qml/Main.qml"
require_grep "manualMotorPopup" "qml/Main.qml"
require_grep "manualMotorPageIndex" "qml/Main.qml"
require_grep "manualSafetyFlickable" "qml/Main.qml"
require_grep "autoVisionRequestZDown" "qml/Main.qml"
require_grep "autoVisionFineTuneForward" "qml/Main.qml"
require_grep "autoVisionRequestZUp" "qml/Main.qml"
require_grep "autoVisionZFocusSettleMs" "qml/Main.qml"
require_grep "autoVisionPostFocusDetectDelayMs" "qml/Main.qml"
require_grep "autoVisionStartDetectDelay" "qml/Main.qml"
require_grep "等待约3秒让摄像头对焦稳定" "qml/Main.qml"
require_grep "zDownFixedSteps" "qml/Main.qml"
require_grep "zUpFixedSteps" "qml/Main.qml"
require_grep "sendManualBeltCommand" "qml/Main.qml"
require_grep "sendF4BeltCommand" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_ACTUATOR_POS_MOVE" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_ACTUATOR_STOP" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_ACTUATOR_VEL_MOVE" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_ACTUATOR_HOME" "main.cpp"
require_grep "sendF4ActuatorPositionMove" "main.cpp"
require_grep "sendF4ActuatorVelocityMove" "main.cpp"
require_grep "sendF4ActuatorStop" "main.cpp"
require_grep "sendF4ActuatorHome" "main.cpp"
require_grep "ACTUATOR_VEL_MOVE" "qml/Main.qml"
require_grep "ACTUATOR_HOME" "qml/Main.qml"
require_grep "sendManualActuatorVelocityMove" "qml/Main.qml"
require_grep "sendStepperActuatorHome" "qml/Main.qml"
require_grep "设当前位置为零点" "qml/Main.qml"
require_absent "每次点击只发送一次位置模式点动命令" "qml/Main.qml"
require_grep "zDownFixedSteps" "main.cpp"
require_grep "zUpFixedSteps" "main.cpp"
require_grep "f4ActuatorCommandFinished" "main.cpp"
require_grep "f4ManualCommandFinished" "main.cpp"
require_grep "onF4ManualCommandFinished" "qml/Main.qml"
require_grep "autoControlBusy" "qml/Main.qml"
require_grep "sendF4AutoControlCommand" "qml/Main.qml"
require_grep "onF4AutoControlFinished" "qml/Main.qml"
require_grep "autoVisionTimer" "qml/Main.qml"
require_grep "autoVisionHasSeenTarget" "qml/Main.qml"
require_grep "autoVisionLostFrames" "qml/Main.qml"
require_grep "autoVisionLostHoldFrames" "qml/Main.qml"
require_grep "目标短暂丢失" "qml/Main.qml"
require_grep "dxPixelsValid" "qml/Main.qml"
require_grep "dxPixelsText" "qml/Main.qml"
if grep -Eq 'root\.autoVision(Timer|DetectDelayTimer)' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "QML Timer 的 id 不能写成 root.autoVisionTimer/root.autoVisionDetectDelayTimer；这些不是 root 属性，会在 BELT_STOP_CENTERED ACK 后触发 TypeError"
fi
if grep -Fq 'dxPixels = ((dxPixels + 5) % 19) - 9' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "首页偏差不能再使用演示滚动值，必须由 LOCATE 真实误差更新"
fi
require_grep "handleAutoVisionLocateFinished" "qml/Main.qml"
require_grep "BELT_MANUAL_SCAN" "qml/Main.qml"
require_grep "BELT_MANUAL_STOP" "qml/Main.qml"
require_grep "QUERY_STATUS" "qml/Main.qml"
require_grep "isAllowedF4BeltCommand" "main.cpp"
require_grep '"name": "位置"' "qml/Main.qml"
require_grep '"value": deviceHealth.locationShortText' "qml/Main.qml"
require_grep "deviceHealth.locationStatusColor" "qml/Main.qml"
require_grep '"name": "定位", "value": deviceHealth.locationStatusText' "qml/Main.qml"
require_grep "locationStatusText" "main.cpp"
require_grep "locationDisplayText" "main.cpp"
require_grep "locationShortText" "main.cpp"
require_grep "locationStatusColor" "main.cpp"
require_grep "DEFAULT_4G_LOCATION_SCRIPT" "main.cpp"
require_grep "4g-location" "main.cpp"
require_grep "ip_ok" "main.cpp"
require_grep "IP定位" "main.cpp"
require_grep "LOCATION_PROBE_TIMEOUT_MS" "main.cpp"
require_grep "m_locationBootProbeDone" "main.cpp"
require_grep "startLocationProbe\\(\\)" "main.cpp"
require_grep "m_locationProcess.start\\(m_locationScript, QStringList\\(\\) << QStringLiteral\\(\"once\"\\)\\)" "main.cpp"
require_grep "parseKeyValueOutput" "main.cpp"
require_grep "22_4g_ppp/4g-location" "README.md"
require_grep "restapi.amap.com/v3/ip" "../../22_4g_ppp/4g-location"
require_grep "AMAP_WEB_KEY" "../../22_4g_ppp/4g-location"
require_grep "amap-web-key" "../../22_4g_ppp/4g-location"
if grep -Eq 'startLocationProbe\\((true|false)\\)|m_locationProcess\.start\(m_locationScript, QStringList\(\) << QStringLiteral\("status"\)\)' "$SCRIPT_DIR/main.cpp"; then
    fail "Qt 位置显示只允许开机单次执行 4g-location once，不能恢复周期 status 定位读取"
fi
if grep -Eq '"value": "检测位"|QGPS|QGPSLOC|gps_waiting|coordsys=gps' "$SCRIPT_DIR/qml/Main.qml" "$SCRIPT_DIR/main.cpp"; then
    fail "顶部位置必须来自高德 IP 省份定位缓存，不能再固定显示检测位，也不能恢复 GPS 坐标定位路径"
fi
if grep -Eq 'QGPS|QGPSLOC|/dev/ttyUSB|AT\\+QGPS|AT\\+QGPSLOC|coordsys=gps' "$SCRIPT_DIR/../../22_4g_ppp/4g-location"; then
    fail "22_4g_ppp/4g-location 当前只允许高德 IP 省份定位，不能访问 GPS、AT 串口或 ttyUSB"
fi
if grep -Eq 'manualBeltSpeed|belt-forward|belt-reverse|belt-speed|正向点动|反向点动|速度档位|夹爪开|夹爪关|manualArmPanel|manualArmButtonGrid|arm-home|arm-standby|arm-pick|arm-good|arm-bad|actuator-on|actuator-off|待F4协议|等待CAM协议|"name": "双轴"' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "手动控制页必须使用新的三轴二进制协议弹窗，不能恢复旧速度档、机械臂、夹爪或待接入假按钮"
fi
if grep -Eq "背光|补光|亮度|manualLight|manualTopLight|backlight|toplight|light-low|light-mid|light-high" "$SCRIPT_DIR/qml/Main.qml"; then
    fail "qml/Main.qml 当前不需要背光/补光/亮度配置，界面和状态栏必须完全删除相关展示"
fi
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
require_grep "settingsDetailVisible" "qml/Main.qml"
require_grep "settingsDetailTitle" "qml/Main.qml"
require_grep "settingsDetailText" "qml/Main.qml"
require_grep "openSettingsDetail" "qml/Main.qml"
require_grep "settingsVisionDetailText" "qml/Main.qml"
require_grep "settingsF4DetailText" "qml/Main.qml"
require_grep "id: settingsDetailOverlay" "qml/Main.qml"
require_grep "id: settingsDetailFlickable" "qml/Main.qml"
require_grep "settingsDetailFlickable.contentY = 0" "qml/Main.qml"
require_grep "查看详情" "qml/Main.qml"
require_grep "record_no" "qml/Main.qml"
require_grep "source/annotated" "qml/Main.qml"
require_grep "断网补传" "qml/Main.qml"
require_grep "/dev/ttySTM2" "qml/Main.qml"
require_grep "LDC1614" "qml/Main.qml"
require_grep "HX711" "qml/Main.qml"
require_grep "Emm42_V5.0" "qml/Main.qml"
require_grep "settingsSupportedPartTypes" "qml/Main.qml"
require_grep "零件与模型判定" "qml/Main.qml"
require_grep "波形垫圈" "qml/Main.qml"
require_grep "平垫圈" "qml/Main.qml"
require_grep "弹性垫圈" "qml/Main.qml"
require_grep "detectSettings" "qml/Main.qml"
require_grep "settingsConfigPath" "qml/Main.qml"
require_grep "settingsRoiSize" "qml/Main.qml"
require_grep "settingsSegmentMinPixels" "qml/Main.qml"
require_grep "settingsOverlayAlpha" "qml/Main.qml"
require_grep "settingsUploadEnabled" "qml/Main.qml"
require_grep "保存成功" "qml/Main.qml"
require_grep "真实检测配置" "qml/Main.qml"
require_grep "参数设置页真实零件" "README.md"
require_grep "/mnt/sdcard/config/defect_ui_config.json" "README.md"
require_grep "--bad-threshold" "README.md"
require_grep "--min-defect-pixels" "README.md"
require_grep "参数摘要" "qml/Main.qml"
require_grep "恢复默认" "qml/Main.qml"
require_grep "DetectSettingsController" "main.cpp"
require_grep "DEFAULT_DETECT_SETTINGS_FILE" "main.cpp"
require_grep "loadSettingsFromDisk" "main.cpp"
require_grep "saveSettingsToDisk" "main.cpp"
require_grep "recordSettingsSummaryToSdCard" "main.cpp"
require_grep "SETTINGS_LOG_PREFIX" "main.cpp"
require_grep "qt_settings" "main.cpp"
require_grep "settingsChanged" "main.cpp"
require_grep "setDetectSettingsController" "main.cpp"
require_grep "detectSettingsController" "main.cpp"
require_grep "detectSettings" "main.cpp"
require_grep "currentSettings" "main.cpp"
require_grep "configPath" "main.cpp"
require_grep "modelThreshold" "main.cpp"
require_grep "reviewThreshold" "main.cpp"
require_grep "roiSize" "main.cpp"
require_grep "segmentMinPixels" "main.cpp"
require_grep "overlayAlpha" "main.cpp"
require_grep "autoUploadEnabled" "main.cpp"
require_grep "manualUploadDisabled" "main.cpp"
require_grep "--bad-threshold" "main.cpp"
require_grep "--min-defect-pixels" "main.cpp"
require_fixed_grep 'QString::number(settings.roiSize)' "main.cpp"
require_fixed_grep 'QString::number(settings.overlayAlpha' "main.cpp"
require_fixed_grep "fusedResultFromModelResults(classificationResult, segmentationResult, settings)" "main.cpp"
require_grep "upload_status=SKIP" "main.cpp"
require_grep "defect_ui_config.json" "main.cpp"
require_grep "settingsLogText" "qml/Main.qml"
require_grep "recordSettingsSummaryToSdCard" "qml/Main.qml"
require_grep "settings-save" "qml/Main.qml"
require_grep "stepperMotorSettings" "main.cpp"
require_grep "StepperMotorSettings" "main.cpp"
require_grep "setStepperMotorValue" "main.cpp"
require_grep "stepper_motors" "main.cpp"
require_grep "min_step" "main.cpp"
require_grep "normal_speed_rpm" "main.cpp"
require_grep "direction" "main.cpp"
require_fixed_grep "BINARY_PROTOCOL_CMD_STEPPER_PARAM_SET = 0x42U" "main.cpp"
require_grep "buildStepperSettingsPayload" "main.cpp"
require_grep "sendF4StepperSettings" "main.cpp"
require_grep "f4StepperSettingsFinished" "main.cpp"
require_grep "STEPPER_PARAM_SET" "main.cpp"
require_grep "acked_cmd" "main.cpp"
require_grep "cycle_id=0" "main.cpp"
require_grep "motor_count=3" "main.cpp"
require_grep "flags=0" "main.cpp"
require_grep "stepperMotorPopupVisible" "qml/Main.qml"
require_grep "stepperMotorPageIndex" "qml/Main.qml"
require_grep "stepperSpeedInputText" "qml/Main.qml"
require_grep "openStepperSpeedEditor" "qml/Main.qml"
require_grep "appendStepperSpeedDigit" "qml/Main.qml"
require_grep "applyStepperSpeedInput" "qml/Main.qml"
require_grep "openStepperMotorPopup" "qml/Main.qml"
require_grep "changeStepperMotorValue" "qml/Main.qml"
require_grep "sendF4StepperSettings" "qml/Main.qml"
require_grep "onF4StepperSettingsFinished" "qml/Main.qml"
require_grep "保存并下发" "qml/Main.qml"
require_grep "id: stepperMotorPopup" "qml/Main.qml"
require_grep "id: stepperSpeedEditor" "qml/Main.qml"
require_grep "0~5000 rpm" "qml/Main.qml"
require_grep "id: stepperMotorPageTabs" "qml/Main.qml"
require_grep "步进电机参数" "qml/Main.qml"
require_grep "传送带电机" "qml/Main.qml"
require_grep "摄像头前后电机" "qml/Main.qml"
require_grep "摄像头上下电机" "qml/Main.qml"
require_grep "最小步长" "qml/Main.qml"
require_grep "常规速度" "qml/Main.qml"
require_grep "应用速度" "qml/Main.qml"
require_grep "方向" "qml/Main.qml"
require_grep "地址" "qml/Main.qml"
require_grep "clampedInt\\(source.normalSpeedRpm, 0, 5000\\)" "main.cpp"
require_grep "步进电机参数弹窗" "README.md"
require_grep "stepper_motors" "README.md"
require_grep "0~5000 rpm" "README.md"
require_grep "STEPPER_PARAM_SET 0x42" "README.md"
require_grep "25 字节" "README.md"
require_grep "sendF4StepperSettings" "README.md"
require_grep "f4StepperSettingsFinished" "README.md"
require_grep "ACK acked_cmd=0x42" "README.md"
require_grep "保存并下发" "README.md"
require_grep "settings-export" "qml/Main.qml"
require_grep "refreshLogFileList" "qml/Main.qml"
require_grep "qt_settings_YYYYMMDD.log" "README.md"
require_grep "参数日志查看验证" "README.md"
require_grep "settings-log-self-test" "README.md"
require_grep "action=保存配置" "README.md"
require_grep "action=导出摘要" "README.md"
require_grep "classify_args" "README.md"
require_grep "segment_args" "README.md"
require_grep "--bad-threshold" "defect_classify.cpp"
require_grep "bad_threshold" "defect_classify.cpp"
require_grep "--min-defect-pixels" "defect_segment.cpp"
require_grep "min_defect_pixels" "defect_segment.cpp"
if grep -Eq '尚未写JSON|尚未下发F4|UI目标值|F4 参数下发协议未落地|当前没有向 F407 下发|只保存不下发 F4|只保存 MP157 参数 JSON|当前只是 MP157 配置保存' "$SCRIPT_DIR/qml/Main.qml" "$SCRIPT_DIR/README.md"; then
    fail "参数设置页已经要求真实配置并下发 F4，不能继续显示“UI目标值/尚未写JSON/尚未下发F4/只保存不下发F4/只保存MP157参数JSON”这类旧占位文案"
fi
if grep -Eq '背光|补光|光源' "$SCRIPT_DIR/README.md"; then
    fail "README 当前也不能继续保留背光、补光或光源文案，避免文档和界面再次漂移"
fi
if grep -Eq '平垫圈A|异形垫片B|冲压片C|旧演示|垫片B' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "参数设置页不能继续显示旧演示零件名，必须只在波形垫圈、平垫圈、弹性垫圈之间切换"
fi
if grep -Eq '低速档|分拣超时|脉冲标定|settingsBeltSpeed|settingsSortTimeoutMs|settingsPulsePerPx' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "参数设置页不能继续把运动、分拣或脉冲标定作为当前可调参数，避免暗示 Qt 已接管 F4"
fi
settings_storage_block="$(sed -n '/id: settingsStoragePanel/,/id: settingsActionPanel/p' "$SCRIPT_DIR/qml/Main.qml")"
if printf '%s\n' "$settings_storage_block" | grep -Eq '背光|补光|光源|亮度'; then
    fail "相机、存储与上传卡片当前不能显示背光、补光、光源或亮度配置"
fi
settings_vision_block="$(sed -n '/id: settingsVisionPanel/,/id: settingsMotionPanel/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$settings_vision_block" | grep -q 'settingsVisionDetailMouse'; then
    fail "视觉检测策略卡片必须在卡片内部提供查看详情入口"
fi
if ! printf '%s\n' "$settings_vision_block" | grep -q 'id: settingsVisionSummaryColumn'; then
    fail "视觉检测策略卡片必须给详情按钮预留布局空间，不能让内容列占到按钮区域"
fi
if ! printf '%s\n' "$settings_vision_block" | grep -q 'y: 132'; then
    fail "视觉检测策略查看详情按钮必须位于底部右侧预留区域，不能覆盖摘要列表"
fi
settings_motion_block="$(sed -n '/id: settingsMotionPanel/,/id: settingsStoragePanel/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$settings_motion_block" | grep -q 'id: settingsMotionSummaryColumn'; then
    fail "F4接入边界卡片必须给详情按钮预留布局空间，不能让内容列占到按钮区域"
fi
if ! printf '%s\n' "$settings_motion_block" | grep -q 'y: 132'; then
    fail "F4接入边界查看详情按钮必须位于底部右侧预留区域，不能覆盖摘要列表"
fi
if printf '%s\n' "$settings_motion_block" | grep -q '"name": "生效状态"'; then
    fail "F4接入边界列表不能再把生效状态作为第 5 行，避免与查看详情按钮重叠"
fi
alarm_advice_block="$(sed -n '/id: alarmAdvicePanel/,/id: alarmAdviceDetailOverlay/p' "$SCRIPT_DIR/qml/Main.qml")"
if printf '%s\n' "$alarm_advice_block" | grep -q 'settingsVisionDetailMouse'; then
    fail "参数设置详情入口不能误插入告警处理建议卡片"
fi
require_grep "alarmHistoryModel" "qml/Main.qml"
require_grep "handleAlarmAction" "qml/Main.qml"
require_grep "id: alarmPage" "qml/Main.qml"
require_grep "id: alarmCurrentPanel" "qml/Main.qml"
require_grep "id: alarmHealthPanel" "qml/Main.qml"
require_grep "id: alarmHistoryPanel" "qml/Main.qml"
require_grep "id: alarmAdvicePanel" "qml/Main.qml"
require_grep "alarmAdviceDetailVisible" "qml/Main.qml"
require_grep "alarmFullAdviceText" "qml/Main.qml"
require_grep "id: alarmAdviceDetailOverlay" "qml/Main.qml"
require_grep "id: alarmAdviceDetailFlickable" "qml/Main.qml"
require_grep "查看全部" "qml/Main.qml"
require_grep '"name": "4G", "value": deviceHealth.networkStatusText' "qml/Main.qml"
alarm_health_block="$(sed -n '/id: alarmHealthGrid/,/id: alarmRefreshMouse/p' "$SCRIPT_DIR/qml/Main.qml")"
if printf '%s\n' "$alarm_health_block" | grep -q '"name": "配置"'; then
    fail "告警页设备健康矩阵不能继续显示配置占位，最后一格必须改为真实 4G 在线状态"
fi
require_grep "ALM-CAM-001" "qml/Main.qml"
require_grep "ALM-SD-001" "qml/Main.qml"
require_grep "ALM-NET-001" "qml/Main.qml"
require_grep "ALM-CLOUD-001" "qml/Main.qml"
require_grep "ALM-F4-001" "qml/Main.qml"
require_grep "保存诊断" "qml/Main.qml"
if grep -Eq "吸盘|背光|补光|亮度|backlight-toggle|manualBacklightEnabled" "$SCRIPT_DIR/qml/Main.qml"; then
    fail "qml/Main.qml 手动控制页显示必须符合硬件事实：不显示吸盘、夹爪、背光、补光或亮度配置"
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
require_grep "recordAlarmIssueToSdCard" "qml/Main.qml"
require_grep "evaluateRuntimeAlarms" "qml/Main.qml"
require_grep "activeAlarmKeys" "qml/Main.qml"
require_grep "buildAlarmLogText" "qml/Main.qml"
require_grep "camera-kms-no-frame" "qml/Main.qml"
require_grep "sdcard-not-writable" "qml/Main.qml"
require_grep "cloud-offline" "qml/Main.qml"
require_grep "f4-heartbeat-lost" "qml/Main.qml"
require_grep "calibrationPopup" "qml/Main.qml"
require_grep "称重标定" "qml/Main.qml"
require_grep "calibrationWeightText" "qml/Main.qml"
require_grep "calibrationKeypadGrid" "qml/Main.qml"
require_grep "appendCalibrationDigit" "qml/Main.qml"
require_grep "backspaceCalibrationDigit" "qml/Main.qml"
require_grep "clearCalibrationWeight" "qml/Main.qml"
require_grep "退格" "qml/Main.qml"
require_grep "calibrationResultFlickable" "qml/Main.qml"
require_grep "contentHeight: calibrationResultTextItem.height" "qml/Main.qml"
require_grep "sendCalibrationCommand" "qml/Main.qml"
require_grep "MP157-F4主链路只发送二进制帧" "qml/Main.qml"
require_grep "model-detect-failed" "qml/Main.qml"
if grep -Eq "/mnt/sdcard/logs/qt_alarm_snapshot\\.txt" "$SCRIPT_DIR/qml/Main.qml"; then
    fail "qml/Main.qml 不应继续提示固定 qt_alarm_snapshot.txt，保存诊断必须由 C++ 返回时间戳文件路径"
fi
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
require_grep "F4_HEARTBEAT_INTERVAL_MS" "main.cpp"
require_grep "120000" "main.cpp"
require_grep "sendF4Command" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_WEIGHT_CALIBRATE" "main.cpp"
require_grep "WEIGHT_CALIBRATE" "main.cpp"
require_grep "knownWeight" "main.cpp"
require_grep "标定克重必须是1~5000g整数" "main.cpp"
require_grep "CRC16-CCITT-FALSE" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_START_CYCLE" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_PAUSE_CYCLE" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_RESUME_CYCLE" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_STOP_CYCLE" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_HEARTBEAT" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_QUERY_STATUS" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_BELT_MANUAL_CONTROL" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_STATUS_REPORT" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_FAULT_REPORT" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_VISION_POS" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_VISION_LOST" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_BELT_STOP_CENTERED" "main.cpp"
require_grep "sendF4BinaryHeartbeat" "main.cpp"
require_grep "sendF4BinaryStatusQuery" "main.cpp"
require_grep "sendF4AutoControlCommand" "main.cpp"
require_grep "sendF4VisionPosition" "main.cpp"
require_grep "sendF4VisionLost" "main.cpp"
require_grep "sendF4BeltStopCentered" "main.cpp"
require_grep "requestAutoVisionLocate" "main.cpp"
require_grep "AUTO_LOCATE_MIN_LUMA_DELTA 12U" "uvc_kms_overlay.c"
require_grep "f4AutoControlFinished" "main.cpp"
require_grep "readF4BinaryReply" "main.cpp"
require_grep "describeF4FaultReport" "main.cpp"
require_grep "STATUS_REPORT cycle=" "main.cpp"
require_grep "BELT_MANUAL_CONTROL" "main.cpp"
require_grep "BELT_MANUAL_SCAN" "main.cpp"
require_grep "BELT_MANUAL_STOP" "main.cpp"
require_grep "ACK重复帧" "main.cpp"
require_grep "ACK未确认执行" "main.cpp"
if grep -Eq 'BELTSCAN\\r\\n|BELTSTOP\\r\\n|BELTINFO\\r\\n|STATUS\\r\\n|QStringLiteral\("BELTSCAN"\)|QStringLiteral\("BELTSTOP"\)|QStringLiteral\("BELTINFO"\)' "$SCRIPT_DIR/main.cpp"; then
    fail "MP157 Qt 不能再向 F4 发送或白名单允许 BELT/STATUS 文本命令；传送带、状态查询和心跳必须走二进制协议语义名"
fi
if grep -Eq 'readF4ReplyText|sendF4SerialCommand|replyUpper\.contains\("ACK"\)|replyUpper\.contains\("OK"\)|replyUpper\.contains\("READY"\)' "$SCRIPT_DIR/main.cpp"; then
    fail "MP157 Qt 不能保留 F4 文本回包成功判定；正确和错误都必须解析二进制 ACK/NACK/STATUS_REPORT/FAULT_REPORT"
fi
if grep -Eq 'sendManualBeltCommand\("BELTSCAN"|sendManualBeltCommand\("BELTSTOP"|sendManualBeltCommand\("BELTINFO"|manualBeltCommandText: "BELTSTOP"' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "QML 手动传送带按钮不能再传旧 ASCII 命令名，必须传 BELT_MANUAL_SCAN/BELT_MANUAL_STOP/QUERY_STATUS"
fi
F4_BINARY_PROTOCOL_SOURCE="/e/hal/bisai_f407_project/User/App/binary_protocol_service.c"
if [ -f "$F4_BINARY_PROTOCOL_SOURCE" ]; then
    grep -q 'BINARY_PROTOCOL_CMD_QUERY_STATUS' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须实现 QUERY_STATUS 二进制状态查询"
    grep -q 'BINARY_PROTOCOL_CMD_BELT_MANUAL_CONTROL' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须实现 BELT_MANUAL_CONTROL 二进制手动传送带命令"
    grep -q 'BINARY_PROTOCOL_CMD_STATUS_REPORT' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须实现 STATUS_REPORT 二进制状态回包"
    grep -q 'BinaryProtocolService_ReportFault' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须用 FAULT_REPORT 二进制帧上报模块故障，不能让 LDC/电机错误刷文本"
    grep -q 'g_binary_protocol_runtime.active_cycle_id == payload.cycle_id' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须识别同一 cycle_id 的重复 START_CYCLE"
    grep -q 'ConveyorMotorService_RequestScan() == 0U' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 重复 START_CYCLE 分支必须重新投递 SCAN，避免只 ACK 不动作"
    grep -q 'BINARY_PROTOCOL_ERROR_STATE_NOT_ALLOWED' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 暂停态重复 START_CYCLE 必须返回 NACK，不允许误恢复旧流程"
    grep -q 'BinaryProtocolService_HandleVisionPos' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须实现 VISION_POS 二进制处理入口"
    grep -q 'BinaryProtocolService_HandleVisionLost' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须实现 VISION_LOST 二进制处理入口"
    grep -q 'BINARY_PROTOCOL_CMD_WEIGHT_CALIBRATE' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须定义 WEIGHT_CALIBRATE 二进制称重标定命令"
    grep -q 'BINARY_PROTOCOL_WEIGHT_CALIBRATION_PAYLOAD_LENGTH' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须定义 5 字节称重标定负载长度"
    grep -q 'BinaryProtocolService_HandleWeightCalibration' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须实现 WEIGHT_CALIBRATE 二进制处理入口"
    grep -q 'WeightService_RequestCalibration' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 WEIGHT_CALIBRATE 必须调用称重服务标定入口"
    grep -A80 'BinaryProtocolService_HandleVisionPos' "$F4_BINARY_PROTOCOL_SOURCE" | grep -q 'BinaryProtocolService_SendAck(payload.cycle_id, frame->sequence, frame->command, 0U)' || fail "F407 VISION_POS 成功后必须返回 ACK，不能静默成功"
    grep -A120 'BinaryProtocolService_HandleVisionLost' "$F4_BINARY_PROTOCOL_SOURCE" | grep -q 'BinaryProtocolService_SendAck(payload.cycle_id, frame->sequence, frame->command, 0U)' || fail "F407 VISION_LOST 成功后必须返回 ACK，不能静默成功"
    if grep -Eq '\[OK\]\[BIN\]|\[ERROR\]\[BIN\]' "$F4_BINARY_PROTOCOL_SOURCE"; then
        fail "F407 二进制协议处理路径不能再输出 [OK][BIN]/[ERROR][BIN] 文本回包，正确/错误必须用 ACK/NACK/STATUS_REPORT"
    fi
fi
F4_UART_COMMAND_SOURCE="/e/hal/bisai_f407_project/User/App/uart_command.c"
if [ -f "$F4_UART_COMMAND_SOURCE" ]; then
    grep -q 'UART_COMMAND_USART1_TEXT_ENABLE' "$F4_UART_COMMAND_SOURCE" || fail "F407 USART1 必须提供文本输出静默开关，MP157 主链路不能再收到 [OK]/[ERROR] 文本日志"
    grep -q '#define UART_COMMAND_USART1_TEXT_ENABLE[[:space:]]*(0U)' "$F4_UART_COMMAND_SOURCE" || fail "F407 USART1 面向 MP157 时文本输出默认必须关闭，只保留二进制协议帧"
fi
F4_CAMERA_MOTOR_SOURCE="/e/hal/bisai_f407_project/User/App/camera_motor_service.c"
if [ -f "$F4_CAMERA_MOTOR_SOURCE" ]; then
    grep -q 'CAMERA_MOTOR_FORWARD_ADDRESS[[:space:]]*(3U)' "$F4_CAMERA_MOTOR_SOURCE" || fail "F407 摄像头前后轴默认地址必须是现场 ID 0x03"
    grep -q 'CAMERA_MOTOR_Z_ADDRESS[[:space:]]*(2U)' "$F4_CAMERA_MOTOR_SOURCE" || fail "F407 摄像头上下轴默认地址必须是现场 ID 0x02"
    grep -q 'CAMERA_MOTOR_COMMAND_QUEUE_LENGTH[[:space:]]*(4U)' "$F4_CAMERA_MOTOR_SOURCE" || fail "F407 摄像头电机队列不能再是长度 1 的覆盖队列"
    grep -q 'xQueueSendToBack' "$F4_CAMERA_MOTOR_SOURCE" || fail "F407 摄像头普通命令必须进入 FIFO，避免 STEPPER_PARAM_SET 被覆盖"
    grep -q 'xQueueSendToFront' "$F4_CAMERA_MOTOR_SOURCE" || fail "F407 摄像头 STOP 必须队首优先，避免连续运动停不下来"
    grep -q 'Runtime config applied' "$F4_CAMERA_MOTOR_SOURCE" || fail "F407 摄像头运行时配置应用后必须有可核对日志"
fi
if grep -Eq 'reply\.left\(96\)|reply\.left\(48\)' "$SCRIPT_DIR/main.cpp"; then
    fail "F4 标定回包不能只截取 96/48 字节，否则 [OK][WEIGHT] Calibration success 详情可能在弹窗中显示不完整"
fi
require_grep "f4CommandFinished" "main.cpp"
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
require_grep "recordAlarmIssueToSdCard" "main.cpp"
require_grep "DEFAULT_SDCARD_LOG_DIR" "main.cpp"
require_grep "ALARM_SNAPSHOT_PREFIX" "main.cpp"
require_grep "ALARM_LOG_PREFIX" "main.cpp"
require_grep "dailyLogFilePath" "main.cpp"
require_grep "sanitizeLogFileToken" "main.cpp"
require_grep "appendTextFileWithFsync" "main.cpp"
require_grep "settings-log-self-test" "main.cpp"
require_grep "run_settings_log_self_test" "main.cpp"
require_grep "LogFileModel settingsLogModel" "main.cpp"
require_grep "settings-summary" "main.cpp"
require_grep "action=保存配置" "main.cpp"
require_grep "action=导出摘要" "main.cpp"
require_grep "classify_args=--roi" "main.cpp"
require_grep "segment_args=--roi" "main.cpp"
require_grep "qt_alarm_snapshot_" "main.cpp"
require_grep "qt_alarm_" "main.cpp"
require_grep "qt_alarm_snapshot_YYYYMMDD" "main.cpp"
require_grep "qt_alarm_YYYYMMDD" "main.cpp"
require_grep "storage action alarm-snapshot" "main.cpp"
require_grep "storage action alarm-log" "main.cpp"
require_grep "alarm-snapshot-self-test" "main.cpp"
require_grep "alarm-log-self-test" "main.cpp"
require_grep "run_alarm_snapshot_self_test" "main.cpp"
require_grep "fsync" "main.cpp"
if grep -Eq "DEFAULT_ALARM_SNAPSHOT_FILE|qt_alarm_snapshot\\.txt" "$SCRIPT_DIR/main.cpp"; then
    fail "main.cpp 不应继续使用固定 qt_alarm_snapshot.txt，必须生成每日 qt_alarm_snapshot_YYYYMMDD.txt"
fi
require_grep "UploadHistoryModel" "main.cpp"
require_grep "appendUploadHistoryRecord" "main.cpp"
require_grep "removeRecord" "main.cpp"
require_grep "removeHistoryImageFiles" "main.cpp"
require_grep "compactUploadStatus" "main.cpp"
require_grep "uploadHistory" "main.cpp"
require_grep "upload_history.json" "main.cpp"
require_grep "upload_history_YYYYMMDD.json" "main.cpp"
require_grep "UPLOAD_HISTORY_DAILY_PREFIX" "main.cpp"
require_grep "dailyHistoryFilePath" "main.cpp"
require_grep "allHistoryFilePaths" "main.cpp"
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
require_grep "AI赋能设计" "fb_boot_splash.c"
require_grep "第九届嵌入式芯片与系统设计竞赛" "fb_boot_splash.c"
require_grep "AI for Design" "fb_boot_splash.c"
require_grep "MP157 Edge AI" "fb_boot_splash.c"
require_grep "Framebuffer Splash" "fb_boot_splash.c"
require_grep "Qt Vision Ready" "fb_boot_splash.c"
require_grep "DEFAULT_SPLASH_ASSET" "fb_boot_splash.c"
require_grep "boot_splash.rgb565" "fb_boot_splash.c"
require_grep "draw_splash_asset" "fb_boot_splash.c"
require_grep "draw_splash_fallback" "fb_boot_splash.c"
require_grep "AI 开机静态启动图预览" "generate_boot_splash_asset.py"
if grep -Eq '18%|LOADING CAMERA|BOOT SELF CHECK' "$SCRIPT_DIR/fb_boot_splash.c"; then
    fail "fb_boot_splash.c 真实早期静态图不能继续保留旧进度条、加载相机或 BOOT SELF CHECK 文案"
fi
require_grep "/dev/fb0" "fb_boot_splash.c"
require_grep "FBIOGET_VSCREENINFO" "fb_boot_splash.c"
require_grep "mmap" "fb_boot_splash.c"
require_grep "draw_splash" "fb_boot_splash.c"
require_grep "msync" "fb_boot_splash.c"
require_grep "VISIBLE " "uvc_kms_overlay.c"
require_grep "STATUS" "uvc_kms_overlay.c"
require_grep "handle_status_command" "uvc_kms_overlay.c"
require_grep "LOCATE" "uvc_kms_overlay.c"
require_grep "handle_locate_command" "uvc_kms_overlay.c"
require_grep "locate_part_in_yuyv_frame" "uvc_kms_overlay.c"
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
require_grep "fusedResultFromModelResults" "main.cpp"
require_grep "cloudResultFromFusedResult" "main.cpp"
require_grep "historyTextFromFusedResult" "main.cpp"
require_grep "uiStatusFromFusedResult" "main.cpp"
require_grep "fused_status" "main.cpp"
require_grep "fused_reason" "main.cpp"
require_grep "综合判定" "qml/Main.qml"
if grep -Eq 'cloudResult = cloudResultFromClassificationResult\(classificationResult\)' "$SCRIPT_DIR/main.cpp"; then
    fail "detectCurrentFrameOnce() 不能只根据分类模型生成 CLOUD_RESULT，必须综合分类和 UNet 分割结果"
fi
if grep -Eq 'cloudResultFromClassificationResult\(classificationResult\)' "$SCRIPT_DIR/main.cpp"; then
    fail "历史重发不能只根据分类模型恢复云端结果，必须综合 classification_result 和 segmentation_result"
fi
require_grep "CLOUD_RESULT" "main.cpp"
require_grep "CLOUD_PART_CODE" "main.cpp"
require_grep "CLOUD_CLASS_LABEL" "main.cpp"
require_grep "partCodeFromClassificationResult" "main.cpp"
require_grep "total_time_ms" "main.cpp"
require_grep "source" "defect-cos-upload"
require_grep "annotated" "defect-cos-upload"
require_grep "--annotated" "defect-cos-upload"
require_grep "validate_cloud_result" "defect-cos-upload"
require_grep "CLOUD_PART_CODE" "defect-cos-upload"
require_grep "CLOUD_CLASS_LABEL" "defect-cos-upload"
require_grep "class_label_to_part_code" "defect-cos-upload"
require_grep "extract_part_id_by_code" "defect-cos-upload"
require_grep "auto_create_part" "defect-cos-upload"
require_grep "part_name" "defect-cos-upload"
require_grep "part_category" "defect-cos-upload"
require_grep "run_auto_create_part_self_test" "defect-cos-upload"
require_grep "device_context" "defect-cos-upload"
require_grep "gasket_good" "defect-cos-upload"
require_grep "gasket_bad" "defect-cos-upload"
require_grep "detect_content_type" "defect-cos-upload"
require_grep "ANNOTATED_FILES" "defect-cos-upload"
require_grep "annotated_index" "defect-cos-upload"
require_grep "uploads/cos/prepare" "defect-cos-upload"
require_grep 'records/\$record_id/files' "defect-cos-upload"
require_grep "CLOUD_UPLOAD_ENV_FILE" "defect-cos-upload"
require_grep "cos-upload.env" "defect-cos-upload"
require_grep "CLOUD_BASE_URL=\"\\$\\{CLOUD_BASE_URL:-http://139\\.9\\.35\\.72\\}\"" "defect-cos-upload"
require_grep "DEFAULT_CLOUD_HEALTH_URL = \"http://139\\.9\\.35\\.72/health\"" "main.cpp"
require_grep "CLOUD_UPLOAD_ENV_FILE" "deploy_qt_camera_display.sh"
require_grep "cos-upload.env" "deploy_qt_camera_display.sh"
require_grep "install -m 600" "deploy_qt_camera_display.sh"
require_grep "QT \\+= .*network|QT \\+= .*quick.*network|QT \\+= .*network.*quick" "qt_camera_display.pro"
require_grep "CloudReviewServer" "main.cpp"
require_grep "BOARD_REVIEW_TOKEN" "main.cpp"
require_grep "BOARD_REVIEW_PORT" "main.cpp"
require_grep "review-result" "main.cpp"
require_grep "applyCloudReviewResult" "main.cpp"
require_grep "cloud_review_result" "main.cpp"
require_grep "cloud_review_text" "main.cpp"
require_grep "board_result_text" "main.cpp"
require_grep "X-Board-Token" "main.cpp"
require_grep "cloudReviewServer" "main.cpp"
require_grep "historyCloudReviewText" "qml/Main.qml"
require_grep "云端修正" "qml/Main.qml"
require_grep "cloud_review_result_api.md" "README.md"
require_grep "POST /api/v1/review-result" "cloud_review_result_api.md"
require_grep "board_review_token" "cloud_review_result_api.md"
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
