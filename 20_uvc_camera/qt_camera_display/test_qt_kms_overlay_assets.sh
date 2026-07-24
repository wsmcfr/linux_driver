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
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"

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

require_repo_grep()
{
    pattern="$1"
    file="$2"
    grep -Eq -- "$pattern" "$REPO_ROOT/$file" || fail "$file 缺少模式：$pattern"
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

# 作用：从 qml/Main.qml 中指定 id 开始截取一小段代码，再检查这段代码是否包含指定模式。
# 主要流程：
#   1. 使用 awk 找到 `id: <qml_id>` 所在位置，并向后保留有限行，避免误扫到其它控件。
#   2. 如果找不到该 id，直接失败，说明测试目标控件已经被改名或删除。
#   3. 在局部代码块中执行正则检查，保证滑动体验参数确实落在对应控件上。
# 关键参数：
#   qml_id 是 QML 控件 id；pattern 是必须命中的正则；message 是失败时给操作员看的原因。
# 返回值：
#   命中时继续执行；未命中时通过 fail() 退出脚本并返回非 0。
require_qml_id_near_grep()
{
    qml_id="$1"
    pattern="$2"
    message="$3"
    block="$(awk -v id_text="id: $qml_id" 'index($0, id_text) { found = 1; n = 0 } found && n < 45 { print; n++ }' "$SCRIPT_DIR/qml/Main.qml")"
    [ -n "$block" ] || fail "qml/Main.qml 找不到 id：$qml_id"
    printf '%s\n' "$block" | grep -Eq -- "$pattern" || fail "$message"
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
require_grep "resetDetectResultPanel" "qml/Main.qml"
reset_detect_panel_block="$(sed -n '/function resetDetectResultPanel/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$reset_detect_panel_block" | grep -q 'detectStatus = "WAIT"'; then
    fail "resetDetectResultPanel() 必须把检测状态重置为 WAIT，避免黑色传送带等待上料时沿用上一轮 GOOD/BAD"
fi
if ! printf '%s\n' "$reset_detect_panel_block" | grep -q 'detectState = reasonText'; then
    fail "resetDetectResultPanel() 必须用调用方传入的 reasonText 刷新模型状态文案"
fi
if ! printf '%s\n' "$reset_detect_panel_block" | grep -q 'detectPartName = partText'; then
    fail "resetDetectResultPanel() 必须重置右侧零件名称，避免空传送带继续显示上一轮波形垫圈"
fi
if ! printf '%s\n' "$reset_detect_panel_block" | grep -q 'detectClassName = classText'; then
    fail "resetDetectResultPanel() 必须重置右侧模型类别，避免旧类别被误认为本轮识别结果"
fi
if ! printf '%s\n' "$reset_detect_panel_block" | grep -q 'detectConfidenceRatio = 0.0'; then
    fail "resetDetectResultPanel() 必须清零置信度进度条，避免旧置信度残留"
fi
if ! printf '%s\n' "$reset_detect_panel_block" | grep -q 'latestModelResultText = ""'; then
    fail "resetDetectResultPanel() 必须清空 latestModelResultText，避免旧模型 RESULT 被自动流程复用"
fi
detect_action_block="$(sed -n '/function handleDetectAction()/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$detect_action_block" | grep -q 'resetDetectResultPanel("检测中...", "当前帧", "当前帧")'; then
    fail "handleDetectAction() 必须复用 resetDetectResultPanel() 清理旧检测结果，避免手动检测残留上一轮状态"
fi
auto_start_block="$(sed -n '/function startAutoVisionLoop()/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$auto_start_block" | grep -q 'resetDetectResultPanel("等待上料", "未检测", "未检测")'; then
    fail "startAutoVisionLoop() 启动等待上料时必须清空右侧旧模型结果，避免黑色传送带旁继续显示上一轮零件"
fi
detect_failure_block="$(sed -n '/function handleDetectFailureText/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$detect_failure_block" | grep -q 'resetDetectResultPanel(resultText, "检测失败", "检测失败")'; then
    fail "handleDetectFailureText() 必须复用 resetDetectResultPanel()，让检测失败时同步清空上一轮模型 RESULT"
fi
require_grep "storageToastTimer" "qml/Main.qml"
require_grep "storageToastVisible" "qml/Main.qml"
require_grep "showStorageToast" "qml/Main.qml"
require_grep "formatF4ToastText" "qml/Main.qml"
require_grep "F4:" "qml/Main.qml"
require_grep "autoCycleRunning" "qml/Main.qml"
require_grep "autoRestartQueued" "qml/Main.qml"
require_grep "stopAutoWorkflowSessionLocally" "qml/Main.qml"
require_grep "markAutoWorkflowAbnormalStopRequired" "qml/Main.qml"
require_grep "queueNextAutoCycleStart" "qml/Main.qml"
require_grep "manualMotorActionButtons" "qml/Main.qml"
require_grep "回原位" "qml/Main.qml"
require_grep "stepperMotorRoleAddressSummary" "qml/Main.qml"
require_grep "cameraLateralMotor.address = 3" "main.cpp"
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
if ! printf '%s\n' "$f4_auto_finished_block" | grep -q 'autoCycleRunning = true'; then
    fail "首页自动流程 ACK 成功后必须标记当前 cycle 已运行，避免自动循环会话和单件状态混淆"
fi
f4_detail_changed_block="$(sed -n '/onDetailTextChanged:/,/onCloudStatusChanged:/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$f4_detail_changed_block" | grep -q 'formatF4ToastText'; then
    fail "F4 心跳/故障详情变化必须通过 formatF4ToastText 显示到底部提示"
fi
f4_final_sort_block="$(sed -n '/onF4FinalSortFinished:/,/onDetailTextChanged:/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$f4_final_sort_block" | grep -q 'queueNextAutoCycleStart'; then
    fail "收到 CYCLE_DONE 后，如果自动循环会话仍启用，QML 必须自动排队启动下一轮 START_CYCLE"
fi
if ! printf '%s\n' "$f4_final_sort_block" | grep -q 'markAutoWorkflowAbnormalStopRequired'; then
    fail "最终分拣失败时必须保留停止入口，避免 F4 仍持有旧 cycle 时首页停止键变灰"
fi
settings_save_block="$(sed -n '/function settingsApplyAction(action)/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$settings_save_block" | grep -q 'detectSettings.loadSettingsFromDisk()'; then
    fail "参数保存成功后必须立即从 defect_ui_config.json 回读，确保界面显示和重启后读取同源"
fi
arm_flow_finished_block="$(sed -n '/onF4ArmInspectionFlowFinished:/,/onF4FinalSortFinished:/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$arm_flow_finished_block" | grep -q 'markAutoWorkflowAbnormalStopRequired'; then
    fail "称重/电感流程异常时必须保留停止入口，避免首页只能点开始却又被 F4 拒绝"
fi
if ! printf '%s\n' "$f4_auto_finished_block" | grep -q '当前流程未停止，请先按停止后再开始新检测'; then
    fail "首页自动流程回包分支必须识别 F4 仍在运行的拒绝原因，并把界面切回可停止状态"
fi
require_grep "autoStartAfterStopRequested" "qml/Main.qml"
require_grep "requestAutoRestartAfterStop" "qml/Main.qml"
require_grep "completeAutoStopAndMaybeRestart" "qml/Main.qml"
always_enabled_button_count="$(grep -c 'property bool actionEnabled: true' "$SCRIPT_DIR/qml/Main.qml")"
if [ "$always_enabled_button_count" -lt 2 ]; then
    fail "首页和 KMS overlay 两组开始/暂停/继续/停止按钮必须始终可点击，具体动作由 handleControlAction 做幂等处理"
fi
if ! printf '%s\n' "$f4_auto_finished_block" | grep -q 'requestAutoRestartAfterStop'; then
    fail "START_CYCLE 被 F4 拒绝为旧流程未停止时，QML 必须自动排队 STOP_CYCLE 后重新 START，而不是让用户反复手动点"
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
nav_panel_block="$(awk '
    /id: navPanel/ { found = 1; n = 0 }
    found && n < 80 { print; n++ }
' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$nav_panel_block" | grep -q 'z: 20'; then
    fail "左侧导航栏必须显式高于普通页面内容，避免后续面板遮住导航点击区域"
fi
if ! printf '%s\n' "$nav_panel_block" | grep -q 'preventStealing: true'; then
    fail "左侧导航栏触摸区域必须禁止手势抢占，避免页面滚动或覆盖层吞掉导航点击"
fi
if ! printf '%s\n' "$nav_panel_block" | grep -q 'root.switchPage(modelData.page)'; then
    fail "左侧导航栏点击后必须真正调用 switchPage() 切换页面"
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
require_grep "autoVisionFineTuneLateral" "qml/Main.qml"
require_grep "autoVisionFineTuneConveyor" "qml/Main.qml"
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
require_grep "sendF4ActuatorStopNow" "main.cpp"
require_grep "sendF4ActuatorHome" "main.cpp"
require_grep "ACTUATOR_VEL_MOVE" "qml/Main.qml"
require_grep "ACTUATOR_HOME" "qml/Main.qml"
require_grep "ACTUATOR_STOP_NOW" "qml/Main.qml"
require_grep "sendManualActuatorVelocityMove" "qml/Main.qml"
require_grep "sendF4ActuatorStopNow" "qml/Main.qml"
require_grep "m_f4ActuatorStopGeneration" "main.cpp"
require_grep "f4ActuatorStopGenerationChanged" "main.cpp"
require_grep "ACTUATOR_STOP 抢占" "main.cpp"
require_absent "manualActuatorStopRetryTimer" "qml/Main.qml"
require_absent "manualStopRetryActuatorId" "qml/Main.qml"
require_grep "sendStepperActuatorHome" "qml/Main.qml"
require_grep "设当前位置为零点" "qml/Main.qml"
require_grep "manualCameraZZeroKnown" "qml/Main.qml"
require_grep "manualCameraZOffsetSteps" "qml/Main.qml"
require_grep "updateManualCameraZOffsetAfterMove" "qml/Main.qml"
require_grep "sendManualActuatorZReturnHome" "qml/Main.qml"
require_grep "回原位按偏移" "qml/Main.qml"
require_grep "上下电机当前位置已经是零点，不再发送回原位命令" "qml/Main.qml"
require_grep "请先在参数设置中对上下电机点击设当前位置为零点" "qml/Main.qml"
require_grep "manualPendingF4Command = \"\"" "qml/Main.qml"
require_grep "F4_ACTUATOR_STOP_NOW_REPEAT_COUNT" "main.cpp"
require_grep "repeat=" "main.cpp"
require_absent "停止已排队" "qml/Main.qml"
require_absent "每次点击只发送一次位置模式点动命令" "qml/Main.qml"
actuator_start_block="$(sed -n '/bool startF4ActuatorCommand/,/bool startF4ActuatorStopNowCommand/p' "$SCRIPT_DIR/main.cpp")"
if ! printf '%s\n' "$actuator_start_block" | grep -q 'stopGenerationAtStart'; then
    fail "普通执行器命令必须捕获 STOP 代际，防止 STOP 已按下后旧运动线程才写入运动帧"
fi
exchange_block="$(sed -n '/static bool exchangeF4BinaryFrame/,/static bool writeF4BinaryFrameWithoutReply/p' "$SCRIPT_DIR/main.cpp")"
position_wait_block="$(sed -n '/static bool runF4ActuatorPositionMoveAndWaitDone/,/static quint32 readLe32Unsigned/p' "$SCRIPT_DIR/main.cpp")"
if ! printf '%s\n%s\n' "$exchange_block" "$position_wait_block" | grep -q 'f4ActuatorStopGenerationChanged'; then
    fail "普通执行器命令写帧前必须检查 STOP 代际，不能让 STOP 之前的旧运动帧晚到覆盖停止"
fi
if ! printf '%s\n%s\n' "$exchange_block" "$position_wait_block" | grep -q 'QMutexLocker writeLocker'; then
    fail "普通运动写帧窗口必须和强制 STOP 使用同一短写锁，避免两个线程同时 flush/write 同一 TTY"
fi
stop_now_block="$(sed -n '/bool startF4ActuatorStopNowCommand/,/^    }/p' "$SCRIPT_DIR/main.cpp")"
if ! printf '%s\n' "$stop_now_block" | grep -q 'fetch_add'; then
    fail "强制 STOP 必须先递增 STOP 代际，让未写出的普通运动命令取消"
fi
z_return_home_block="$(sed -n '/function sendManualActuatorZReturnHome/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if printf '%s\n' "$z_return_home_block" | grep -q 'return sendManualActuatorZFixedMove(1, label)'; then
    fail "回原位不能再直接复用固定上升步数；设当前位置为零点后必须按本地零点偏移决定是否移动"
fi
if ! printf '%s\n' "$z_return_home_block" | grep -q 'manualPendingF4Command !== ""'; then
    fail "回原位判断本地偏移为 0 之前必须先检查手动执行器命令是否仍在途，避免上升/下降未完成时误报已经在零点"
fi
if ! printf '%s\n' "$z_return_home_block" | grep -q '等待上下轴上一条动作完成后再回原位'; then
    fail "回原位被在途上下轴命令阻止时必须给出明确提示，不能继续显示已经在零点"
fi
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
require_grep "LOCAL_READY" "main.cpp"
require_grep "完整自动检测上传线程" "main.cpp"
require_grep "--bad-threshold" "main.cpp"
require_grep "--min-defect-pixels" "main.cpp"
require_fixed_grep 'QString::number(settings.roiSize)' "main.cpp"
require_fixed_grep 'QString::number(settings.overlayAlpha' "main.cpp"
require_fixed_grep "fusedResultFromModelResults(classificationResult, segmentationResult, settings)" "main.cpp"
require_grep "upload_status=LOCAL_READY" "main.cpp"
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
require_grep "scan_speed_rpm" "main.cpp"
require_grep "scanSpeedRpm" "main.cpp"
require_grep "z_motion_timeout_ms" "main.cpp"
require_grep "zMotionTimeoutMs" "main.cpp"
require_grep "f4_arm_result_timeout_ms" "main.cpp"
require_grep "f4ArmResultTimeoutMs" "main.cpp"
require_grep "F4_ARM_ACTIVE_FRAME_TIMEOUT_MAX_MS = 300000" "main.cpp"
require_grep "conveyorTrackSpeedRpm" "qml/Main.qml"
require_grep "conveyorScanSpeedRpm" "qml/Main.qml"
require_grep "cameraZMotionTimeoutMs" "qml/Main.qml"
require_grep "settingsF4ArmResultTimeoutMs" "qml/Main.qml"
require_grep "机械臂等待" "qml/Main.qml"
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
require_grep "record_size=9" "main.cpp"
require_grep "stepperMotorPopupVisible" "qml/Main.qml"
require_grep "stepperMotorPageIndex" "qml/Main.qml"
require_grep "stepperSpeedInputText" "qml/Main.qml"
require_grep "stepperSpeedEditKey" "qml/Main.qml"
require_grep "syncStepperSettingsToF4" "qml/Main.qml"
require_grep "stepperStartupSyncTimer" "qml/Main.qml"
require_grep "openStepperSpeedEditor" "qml/Main.qml"
require_grep "appendStepperSpeedDigit" "qml/Main.qml"
require_grep "applyStepperSpeedInput" "qml/Main.qml"
require_grep "openStepperMotorPopup" "qml/Main.qml"
require_grep "changeStepperMotorValue" "qml/Main.qml"
require_grep "sendF4StepperSettings" "qml/Main.qml"
require_grep "onF4StepperSettingsFinished" "qml/Main.qml"
require_grep "保存并下发" "qml/Main.qml"
require_grep "id: stepperMotorPopup" "qml/Main.qml"
require_grep "stepperMotorSettingsRevision" "qml/Main.qml"
require_grep "stepperStepReplaceOnNextDigit" "qml/Main.qml"
require_grep "id: stepperMotorSettingsFlickable" "qml/Main.qml"
require_grep "contentHeight: stepperMotorSettingsContent.height" "qml/Main.qml"
require_grep "flickableDirection: Flickable.VerticalFlick" "qml/Main.qml"
require_grep "boundsBehavior: Flickable.DragOverBounds" "qml/Main.qml"
require_grep "stepperMotorInputTouchGuard" "qml/Main.qml"
require_grep "id: stepperSpeedEditor" "qml/Main.qml"
require_grep "0~5000 rpm" "qml/Main.qml"
require_grep "id: stepperMotorPageTabs" "qml/Main.qml"
require_grep "步进电机参数" "qml/Main.qml"
require_grep "传送带电机" "qml/Main.qml"
require_grep "摄像头左右电机" "qml/Main.qml"
require_grep "摄像头上下电机" "qml/Main.qml"
require_grep "最小步长" "qml/Main.qml"
require_grep "常规速度" "qml/Main.qml"
require_grep "上料速度" "qml/Main.qml"
require_grep "对中速度" "qml/Main.qml"
require_grep "Z轴超时" "qml/Main.qml"
require_grep "应用速度" "qml/Main.qml"
require_grep "方向" "qml/Main.qml"
require_grep "地址" "qml/Main.qml"
require_grep "clampedInt\\(source.normalSpeedRpm, 0, 5000\\)" "main.cpp"
require_grep "clampedInt\\(source.scanSpeedRpm, 0, 5000\\)" "main.cpp"
require_grep "步进电机参数弹窗" "README.md"
require_grep "摄像头左右" "README.md"
require_grep "stepper_motors" "README.md"
require_grep "scan_speed_rpm" "README.md"
require_grep "z_motion_timeout_ms" "README.md"
require_grep "f4_arm_result_timeout_ms" "README.md"
require_grep "机械臂等待超时" "README.md"
require_grep "0~5000 rpm" "README.md"
require_grep "STEPPER_PARAM_SET 0x42" "README.md"
require_grep "31 字节" "README.md"
require_grep "sendF4StepperSettings" "README.md"
require_grep "f4StepperSettingsFinished" "README.md"
require_grep "ACK acked_cmd=0x42" "README.md"
require_grep "保存并下发" "README.md"
require_grep "开机自动下发" "README.md"
require_grep "自动流程不再固定限制到 40rpm" "README.md"
require_grep "settings-export" "qml/Main.qml"
require_grep "refreshLogFileList" "qml/Main.qml"
require_grep "qt_settings_YYYYMMDD.log" "README.md"
require_grep "参数日志查看验证" "README.md"
require_grep "settings-log-self-test" "README.md"
require_grep "action=保存配置" "README.md"
require_grep "action=导出摘要" "README.md"
if ! grep -q 'root.stepperMotorSettingsRevision += 1' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "DetectSettingsController 发出 settingsChanged 后，QML 必须递增 stepperMotorSettingsRevision，让步进弹窗重新读取最新 zMotionTimeoutMs"
fi
if ! grep -q 'property var motorConfig: root.stepperMotorSettingsRevision' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "步进电机弹窗 motorConfig 必须依赖 stepperMotorSettingsRevision，避免输入 Z 轴超时后仍显示旧的 10 秒"
fi
if ! grep -q '本次自动检测Z轴下降/回升最多等待' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "应用 Z 轴超时后必须直接提示本次自动检测使用的等待毫秒数，方便现场确认不是固定 10 秒"
fi
if ! grep -q 'requestF4ArmInspectionFlow(latestModelResultText, root.settingsF4ArmResultTimeoutMs)' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "Z 轴回升后启动机械臂检测流程时，必须把参数页机械臂等待超时传给 C++，不能继续使用写死等待窗口"
fi
if ! grep -q 'root.settingsF4ArmResultTimeoutMs)' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "最终分拣等待 CYCLE_DONE 时也必须使用参数页机械臂等待超时"
fi
arm_flow_block="$(sed -n '/static bool runF4ArmInspectionFlow/,/static bool runF4FinalSortAndWaitCycleDone/p' "$SCRIPT_DIR/main.cpp")"
final_sort_block="$(sed -n '/static bool runF4FinalSortAndWaitCycleDone/,/static bool sendF4BinaryHeartbeat/p' "$SCRIPT_DIR/main.cpp")"
if ! printf '%s\n' "$arm_flow_block" | grep -q 'activeFrameTimeoutMs'; then
    fail "runF4ArmInspectionFlow() 必须接收 activeFrameTimeoutMs 参数，用参数页数值等待 WEIGHT_RESULT/LDC_RESULT"
fi
if printf '%s\n' "$arm_flow_block" | grep -q 'F4_ARM_ACTIVE_FRAME_TIMEOUT_MS'; then
    fail "runF4ArmInspectionFlow() 不能继续直接使用固定 F4_ARM_ACTIVE_FRAME_TIMEOUT_MS"
fi
if ! printf '%s\n' "$final_sort_block" | grep -q 'activeFrameTimeoutMs'; then
    fail "runF4FinalSortAndWaitCycleDone() 必须接收 activeFrameTimeoutMs 参数，用参数页数值等待 CYCLE_DONE"
fi
if printf '%s\n' "$final_sort_block" | grep -q 'F4_ARM_ACTIVE_FRAME_TIMEOUT_MS'; then
    fail "runF4FinalSortAndWaitCycleDone() 不能继续直接使用固定 F4_ARM_ACTIVE_FRAME_TIMEOUT_MS"
fi
stepper_min_step_minus_block="$(sed -n '/id: stepperMinStepMinusMouse/,/^                            }/p' "$SCRIPT_DIR/qml/Main.qml")"
stepper_min_step_plus_block="$(sed -n '/id: stepperMinStepPlusMouse/,/^                            }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$stepper_min_step_minus_block" | grep -q 'changeStepperMotorValue("minStep", -50)'; then
    fail "步进电机最小步长减号按钮必须每次减少 50 step，不能继续只减少 1 step 或旧的 100 step"
fi
if ! printf '%s\n' "$stepper_min_step_plus_block" | grep -q 'changeStepperMotorValue("minStep", 50)'; then
    fail "步进电机最小步长加号按钮必须每次增加 50 step，不能继续只增加 1 step 或旧的 100 step"
fi
stepper_step_open_block="$(sed -n '/function openStepperStepEditor/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$stepper_step_open_block" | grep -q 'stepperStepReplaceOnNextDigit = stepperStepEditorIsTimeout()'; then
    fail "打开 Z 轴超时数字键盘后，第一次按数字必须替换当前 10 秒，不能追加成 105 秒再被范围拒绝"
fi
stepper_step_append_block="$(sed -n '/function appendStepperStepDigit/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$stepper_step_append_block" | grep -q 'stepperStepReplaceOnNextDigit ? "" : stepperStepInputText'; then
    fail "Z 轴超时数字键第一次输入必须从空文本开始，避免当前 10 秒阻塞输入 1~9 秒"
fi
stepper_step_clear_block="$(sed -n '/function clearStepperStepInput/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if printf '%s\n' "$stepper_step_clear_block" | grep -q 'stepperStepEditorIsTimeout() ? "10" : "0"'; then
    fail "Z 轴超时的清空键不能继续写回 10 秒"
fi
if ! printf '%s\n' "$stepper_step_clear_block" | grep -q 'stepperStepInputText = ""'; then
    fail "Z 轴超时的清空键必须真正清空输入框，方便重新输入任意 1~60 秒"
fi
stepper_input_mouse_block="$(sed -n '/id: stepperMotorSettingsFlickable/,/id: stepperSpeedEditor/p' "$SCRIPT_DIR/qml/Main.qml")"
for mouse_id in \
    stepperSpeedInputMouse \
    stepperScanSpeedInputMouse \
    stepperZDownInputMouse \
    stepperZUpInputMouse \
    stepperZTimeoutInputMouse; do
    if ! printf '%s\n' "$stepper_input_mouse_block" | sed -n "/id: ${mouse_id}/,/onClicked:/p" | grep -q 'preventStealing: true'; then
        fail "步进电机参数弹窗放入 Flickable 后，${mouse_id} 必须设置 preventStealing: true，避免触摸滑动层抢走输入按钮点击"
    fi
done
require_grep "classify_args" "README.md"
require_grep "segment_args" "README.md"
require_grep "--bad-threshold" "defect_classify.cpp"
require_grep "bad_threshold" "defect_classify.cpp"
require_grep "--min-defect-pixels" "defect_segment.cpp"
require_grep "min_defect_pixels" "defect_segment.cpp"
if grep -Eq '尚未写JSON|尚未下发F4|UI目标值|F4 参数下发协议未落地|当前没有向 F407 下发|只保存不下发 F4|只保存 MP157 参数 JSON|当前只是 MP157 配置保存' "$SCRIPT_DIR/qml/Main.qml" "$SCRIPT_DIR/README.md"; then
    fail "参数设置页已经要求真实配置并下发 F4，不能继续显示“UI目标值/尚未写JSON/尚未下发F4/只保存不下发F4/只保存MP157参数JSON”这类旧占位文案"
fi
if grep -Eq '摄像头前后电机|前后轴|前进/后退轴|前进/后退电机' "$SCRIPT_DIR/qml/Main.qml" "$SCRIPT_DIR/README.md"; then
    fail "MP157 当前硬件语义已改为摄像头左右轴，QML/README 不能继续显示旧的前后轴或前进/后退轴文案"
fi
require_repo_grep "ACT_CAMERA_LATERAL" "docs/stm32mp157-f407-binary-protocol.md"
require_repo_grep "lateral_motor_address" "docs/stm32mp157-f407-binary-protocol.md"
require_repo_grep "摄像头左右电机填写 \`3\`" "docs/stm32mp157-f407-binary-protocol.md"
require_repo_grep "传送带前后微调或左右轴微调" "docs/stm32mp157-f407-binary-protocol.md"
require_repo_grep "lateral_addr=3" "docs/stm32mp157-f407-auto-detect-debug-roadmap.md"
require_repo_grep "传送带做前后/Y 微调、用左右轴做 X 微调" "docs/stm32mp157-f407-auto-detect-debug-roadmap.md"
if grep -Eq '摄像头前后轴|前进/后退轴|前进/后退电机|forward_addr' \
    "$REPO_ROOT/docs/stm32mp157-f407-binary-protocol.md" \
    "$REPO_ROOT/docs/stm32mp157-f407-auto-detect-debug-roadmap.md"; then
    fail "核心联调文档必须使用摄像头左右轴语义；旧前后轴或 forward_addr 只能留在 F4 兼容接口文档里"
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
require_grep "uiHorizontalFlickVelocity" "qml/Main.qml"
require_grep "uiHorizontalFlickDeceleration" "qml/Main.qml"
require_grep "uiVerticalFlickVelocity" "qml/Main.qml"
require_grep "uiVerticalFlickDeceleration" "qml/Main.qml"
require_grep "uiCarouselCachePages" "qml/Main.qml"
require_grep "uiListCachePages" "qml/Main.qml"
require_qml_id_near_grep "historyListView" "boundsBehavior:[[:space:]]*Flickable\\.DragOverBounds" "历史记录列表必须使用柔和边界，避免滑到头时硬停顿"
require_qml_id_near_grep "historyListView" "maximumFlickVelocity:[[:space:]]*root\\.uiHorizontalFlickVelocity" "历史记录列表必须使用统一横向滑动速度上限"
require_qml_id_near_grep "historyListView" "flickDeceleration:[[:space:]]*root\\.uiHorizontalFlickDeceleration" "历史记录列表必须使用统一横向滑动减速度"
require_qml_id_near_grep "imageCarousel" "boundsBehavior:[[:space:]]*Flickable\\.DragOverBounds" "历史图片轮播必须使用柔和边界，避免图片滑动到头时顿挫"
require_qml_id_near_grep "imageCarousel" "maximumFlickVelocity:[[:space:]]*root\\.uiHorizontalFlickVelocity" "历史图片轮播必须使用统一横向滑动速度上限"
require_qml_id_near_grep "imageCarousel" "flickDeceleration:[[:space:]]*root\\.uiHorizontalFlickDeceleration" "历史图片轮播必须使用统一横向滑动减速度"
require_qml_id_near_grep "imageCarousel" "cacheBuffer:[[:space:]]*width \\* root\\.uiCarouselCachePages" "历史图片轮播必须预缓存相邻图片，避免边滑边解码卡顿"
require_qml_id_near_grep "imageCarousel" "sourceSize\\.width:[[:space:]]*imageCarousel\\.width" "历史图片轮播必须限制图片解码尺寸，避免大图按原尺寸进入纹理"
require_qml_id_near_grep "imageCarousel" "smooth:[[:space:]]*!imageCarousel\\.moving" "历史图片轮播必须在滑动中关闭平滑缩放，降低 MP157 滑动时的纹理开销"
for smooth_scroll_id in statsRecentListView manualSafetyFlickable manualCommandLogView settingsDetailFlickable stepperMotorSettingsFlickable calibrationResultFlickable alarmHistoryListView alarmAdviceDetailFlickable logListView logDetailFlickable analysisDetailFlickable
do
    require_qml_id_near_grep "$smooth_scroll_id" "boundsBehavior:[[:space:]]*Flickable\\.DragOverBounds" "$smooth_scroll_id 必须使用柔和边界，避免上下滑动到边界时硬停顿"
    require_qml_id_near_grep "$smooth_scroll_id" "maximumFlickVelocity:[[:space:]]*root\\.uiVerticalFlickVelocity" "$smooth_scroll_id 必须使用统一垂直滑动速度上限"
    require_qml_id_near_grep "$smooth_scroll_id" "flickDeceleration:[[:space:]]*root\\.uiVerticalFlickDeceleration" "$smooth_scroll_id 必须使用统一垂直滑动减速度"
done
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
require_grep "BINARY_PROTOCOL_CMD_ARM_JOB_START" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_MODEL_READY" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_FINAL_SORT_RESULT" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_EVENT_REPORT" "main.cpp"
require_grep "BINARY_PROTOCOL_EVENT_ACTUATOR_MOVE_DONE" "main.cpp"
require_grep "BINARY_PROTOCOL_EVENT_ACTUATOR_MOVE_TIMEOUT" "main.cpp"
require_grep "runF4ActuatorPositionMoveAndWaitDone" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_WEIGHT_RESULT" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_LDC_RESULT" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_CYCLE_DONE" "main.cpp"
require_grep "requestF4ArmInspectionFlow" "main.cpp"
require_grep "uploadCompletedInspectionBundle" "main.cpp"
require_grep "CLOUD_WEIGHT_CONTEXT" "main.cpp"
require_grep "CLOUD_LDC_CONTEXT" "main.cpp"
require_grep "CLOUD_F4_FLOW_CONTEXT" "main.cpp"
require_grep "CLOUD_DECISION_CONTEXT" "main.cpp"
require_grep "CLOUD_VISION_CONTEXT" "main.cpp"
require_grep "updateRecordInspectionContexts" "main.cpp"
require_grep "weight_context_json" "main.cpp"
require_grep "ldc_context_json" "main.cpp"
require_grep "f4_flow_context_json" "main.cpp"
require_grep "decision_context_json" "main.cpp"
require_grep "vision_context_json" "main.cpp"
require_grep "FINAL_SORT_RESULT" "main.cpp"
detect_once_block="$(sed -n '/QString detectCurrentFrameOnce(/,/^    QString runDefectClassify(/p' "$SCRIPT_DIR/main.cpp")"
if printf '%s\n' "$detect_once_block" | grep -q 'uploadDetectImagesToCos'; then
    fail "detectCurrentFrameOnce() 只能保存图片和模型结果，自动流程必须等 WEIGHT_RESULT/LDC_RESULT/CYCLE_DONE 收齐后再一次性上传"
fi
detect_locate_guard_line="$(printf '%s\n' "$detect_once_block" | grep -n 'ensureCurrentFrameHasLocateTarget' | head -n 1 | cut -d: -f1)"
detect_save_line="$(printf '%s\n' "$detect_once_block" | grep -n 'sendRawOverlayCommand(QStringLiteral("SAVE_DETECT ' | head -n 1 | cut -d: -f1)"
if [ -z "$detect_locate_guard_line" ] || [ -z "$detect_save_line" ] || [ "$detect_locate_guard_line" -ge "$detect_save_line" ]; then
    fail "detectCurrentFrameOnce() 必须在 SAVE_DETECT 和双模型推理前先执行 LOCATE has_target=1 门禁，避免空 ROI/空皮带被分类模型误识别成零件"
fi
require_grep "检测入口 LOCATE" "main.cpp"
require_grep "has_target" "main.cpp"
detect_locate_guard_block="$(sed -n '/bool ensureCurrentFrameHasLocateTarget/,/^    QString parseTokenValue/p' "$SCRIPT_DIR/main.cpp")"
if ! printf '%s\n' "$detect_locate_guard_block" | grep -q 'parseTokenValue(reply, QStringLiteral("ring"))'; then
    fail "ensureCurrentFrameHasLocateTarget() 必须解析 LOCATE ring 字段，检测入口不能只看 has_target"
fi
if ! printf '%s\n' "$detect_locate_guard_block" | grep -q 'parseTokenValue(reply, QStringLiteral("bbox_w"))'; then
    fail "ensureCurrentFrameHasLocateTarget() 必须解析 bbox_w/bbox_h，便于现场确认 no-ring 候选尺寸"
fi
if ! printf '%s\n' "$detect_locate_guard_block" | grep -q 'parseTokenValue(reply, QStringLiteral("confidence"))'; then
    fail "ensureCurrentFrameHasLocateTarget() 必须解析 confidence，便于区分真实低置信和黑带高置信误报"
fi
if ! printf '%s\n' "$detect_locate_guard_block" | grep -q 'ringText != QStringLiteral("1")'; then
    fail "ensureCurrentFrameHasLocateTarget() 必须要求 has_target=1 后继续满足 ring=1，阻断黑色传送带 no-ring 候选进入模型"
fi
if ! printf '%s\n' "$detect_locate_guard_block" | grep -q '当前 ROI 候选没有中心孔结构'; then
    fail "ensureCurrentFrameHasLocateTarget() 的 no-ring 拦截错误文本必须明确提示中心孔缺失，方便现场判断黑色传送带误报"
fi
retry_upload_block="$(sed -n '/Q_INVOKABLE void retryUploadRecord(int row)/,/workerThread->start();/p' "$SCRIPT_DIR/main.cpp")"
if ! printf '%s\n' "$retry_upload_block" | grep -q 'weightContextJson'; then
    fail "历史重新发送必须复用历史记录里保存的 weight/ldc/f4/decision/vision 上下文，不能只重传图片"
fi
require_grep "knownWeight" "main.cpp"
require_grep "标定克重必须是1~5000g整数" "main.cpp"
require_grep "CRC16-CCITT-FALSE" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_START_CYCLE" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_PAUSE_CYCLE" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_RESUME_CYCLE" "main.cpp"
require_grep "BINARY_PROTOCOL_CMD_STOP_CYCLE" "main.cpp"
require_grep "停止按钮固定发送 cycle_id=0" "main.cpp"
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
require_grep "beltMotor.normalSpeedRpm = 40" "main.cpp"
require_grep "autoVisionNormalizeSpeedRpm" "qml/Main.qml"
require_grep "autoVisionFallbackSpeedRpm" "qml/Main.qml"
require_grep "autoVisionStartFocusSettleBeforeDetect" "qml/Main.qml"
require_grep "focus-settle" "qml/Main.qml"
require_grep "autoVisionStartZMotionWait" "qml/Main.qml"
require_grep "autoVisionEstimateZMoveMs" "qml/Main.qml"
require_grep "autoVisionHandleActuatorMoveDone" "qml/Main.qml"
require_grep "actuator-move-done" "qml/Main.qml"
require_grep "actuator-move-timeout" "qml/Main.qml"
require_grep "estimated-done" "main.cpp"
require_grep "estimated-done" "qml/Main.qml"
require_grep "estimateActuatorPositionMoveFallbackMs" "main.cpp"
require_grep "sendF4ActuatorPositionMoveWithTimeout" "main.cpp"
require_grep "sendF4ActuatorPositionMoveWithTimeout" "qml/Main.qml"
require_grep "mp157-local-estimated-done" "main.cpp"
require_grep "z-motion-down-wait" "qml/Main.qml"
require_grep "z-motion-up-wait" "qml/Main.qml"
require_grep "requestF4ArmInspectionFlow" "qml/Main.qml"
require_grep "onF4ArmInspectionFlowFinished" "qml/Main.qml"
mp157_fallback_block="$(sed -n '/estimateActuatorPositionMoveFallbackMs/,/^    }/p' "$SCRIPT_DIR/main.cpp")"
if ! printf '%s\n' "$mp157_fallback_block" | grep -q 'readLe16(frame, payloadOffset + 5)'; then
    fail "MP157 本地估算必须从 ACTUATOR_POS_MOVE 帧内 speed_rpm 读取速度，不能写死等待时间"
fi
if ! printf '%s\n' "$mp157_fallback_block" | grep -q 'readLe32Unsigned(frame, payloadOffset + 7)'; then
    fail "MP157 本地估算必须从 ACTUATOR_POS_MOVE 帧内 steps 读取步数，参数页改步数后下一次命令要实时生效"
fi
if ! printf '%s\n' "$mp157_fallback_block" | grep -q 'wait_ms='; then
    fail "MP157 本地估算日志必须输出 wait_ms，方便现场核对速度、步数和等待时间"
fi
if ! printf '%s\n' "$mp157_fallback_block" | grep -q 'fallbackMaxWaitMs'; then
    fail "MP157 本地估算必须接收参数页 Z 轴超时时间作为最大等待上限，不能写死 10s 或 65s"
fi
z_down_send_block="$(sed -n '/function autoVisionRequestZDown/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$z_down_send_block" | grep -q 'cameraZMotionTimeoutMs'; then
    fail "Z 下降命令必须从摄像头上下电机参数读取 zMotionTimeoutMs"
fi
if ! printf '%s\n' "$z_down_send_block" | grep -q 'sendF4ActuatorPositionMoveWithTimeout'; then
    fail "Z 下降命令必须使用带参数页超时的发送入口，不能继续走默认固定等待"
fi
z_up_send_block="$(sed -n '/function autoVisionRequestZUp/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$z_up_send_block" | grep -q 'cameraZMotionTimeoutMs'; then
    fail "Z 回升命令必须复用摄像头上下电机参数里的 zMotionTimeoutMs"
fi
if ! printf '%s\n' "$z_up_send_block" | grep -q 'sendF4ActuatorPositionMoveWithTimeout'; then
    fail "Z 回升命令必须使用带参数页超时的发送入口，避免默认等待时间和参数页不一致"
fi
z_recovery_send_block="$(sed -n '/function autoVisionRequestZUpForRecovery/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if printf '%s\n' "$z_recovery_send_block" | grep -q 'sendF4ActuatorPosMove'; then
    fail "停止/恢复回位的 Z 轴回升不能调用不存在的 sendF4ActuatorPosMove()，否则 QML TypeError 后流程会失败"
fi
if ! printf '%s\n' "$z_recovery_send_block" | grep -q 'cameraZMotionTimeoutMs'; then
    fail "停止/恢复回位的 Z 轴回升必须复用 zMotionTimeoutMs，避免回位阶段无本地超时保护"
fi
if ! printf '%s\n' "$z_recovery_send_block" | grep -q 'sendF4ActuatorPositionMoveWithTimeout(2, 1, 0, speed, steps, 0, timeoutMs'; then
    fail "停止/恢复回位的 Z 轴回升必须使用 sendF4ActuatorPositionMoveWithTimeout(2, 1, 0, ...)"
fi
lateral_recovery_send_block="$(sed -n '/function autoVisionRequestLateralReturnForRecovery/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if printf '%s\n' "$lateral_recovery_send_block" | grep -q 'sendF4ActuatorPosMove'; then
    fail "停止/恢复回位的左右轴归中不能调用不存在的 sendF4ActuatorPosMove()，否则 QML TypeError 后流程会失败"
fi
if ! printf '%s\n' "$lateral_recovery_send_block" | grep -q 'autoVisionLateralReturnTimeoutMs'; then
    fail "停止/恢复回位的左右轴归中必须使用 autoVisionLateralReturnTimeoutMs 作为本地超时保护"
fi
if ! printf '%s\n' "$lateral_recovery_send_block" | grep -q 'sendF4ActuatorPositionMoveWithTimeout(1, direction, 0, speed, offsetSteps, 0, timeoutMs'; then
    fail "停止/恢复回位的左右轴归中必须使用 sendF4ActuatorPositionMoveWithTimeout(1, direction, 0, ...)"
fi
z_down_ack_block="$(sed -n '/root.autoVisionActuatorPhase === "z-down"/,/root.autoVisionActuatorPhase.indexOf("fine-tune")/p' "$SCRIPT_DIR/qml/Main.qml")"
if printf '%s\n' "$z_down_ack_block" | grep -q 'autoVisionZFocusSettleMs'; then
    fail "Z 下降 ACK 后不能直接等待 3s 对焦；必须先短稳定并用传送带+左右轴复查 ROI 中心"
fi
if ! printf '%s\n' "$z_down_ack_block" | grep -q 'root.autoVisionActuatorPhase = "z-motion-down-wait"'; then
    fail "Z 下降完成回调必须切到 z-motion-down-wait，再由统一 DONE 处理函数推进"
fi
if printf '%s\n' "$z_down_ack_block" | grep -q 'autoVisionStartZMotionWait'; then
    fail "Z 下降回调中的 ok 已经表示 C++ 等到 F4 DONE，不能再叠加本地估算等待"
fi
if ! printf '%s\n' "$z_down_ack_block" | grep -q 'autoVisionHandleActuatorMoveDone'; then
    fail "Z 下降必须等待 F4 ACTUATOR_MOVE_DONE 事件确认电机真实到位，不能只靠本地估算等待推进"
fi
if printf '%s\n' "$z_down_ack_block" | grep -q 'autoVisionShortSettleMs'; then
    fail "Z 下降 ACK 后不能直接用短稳定替代物理运动完成等待"
fi
fine_tune_block="$(sed -n '/function handleAutoVisionFineTuneLocateFinished/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
settle_timer_block="$(sed -n '/id: autoVisionActuatorSettleTimer/,/Connections {/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$fine_tune_block" | grep -q 'autoVisionStopRealtimeFineTune'; then
    fail "ROI 复查阶段必须进入实时闭环 STOP 收口逻辑，不能继续沿用单次位置微调后直接推进"
fi
if ! printf '%s\n' "$fine_tune_block" | grep -q 'deviceHealth.sendF4ActuatorVelocityMove'; then
    fail "ROI 复查阶段的传送带主轴必须保留 ACTUATOR_VEL_MOVE 实时闭环点动"
fi
require_grep "lateralFineTuneFixedSteps" "main.cpp"
require_grep "lateralFineTuneFixedSteps" "qml/Main.qml"
require_grep "lateralFineTuneFixedSteps" "README.md"
require_grep "cameraLateralFineTuneFixedSteps" "qml/Main.qml"
lateral_realtime_position_block="$(sed -n '/左右轴实时微调位置模式分支/,/return/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$lateral_realtime_position_block" | grep -q 'selectedAxis === "lateral"'; then
    fail "ROI 实时微调必须给左右轴单独建立位置模式分支，不能继续和传送带共用速度模式"
fi
if ! printf '%s\n' "$lateral_realtime_position_block" | grep -q 'sendF4ActuatorPositionMoveWithTimeout(1, desiredDirection, 0, speed, steps, 0, timeoutMs'; then
    fail "左右轴实时微调必须使用 ACTUATOR_POS_MOVE actuator=1 固定步数位置模式，并带本地超时保护"
fi
if ! printf '%s\n' "$lateral_realtime_position_block" | grep -q 'recordAutoVisionPendingLateralFineTune(desiredDirection, steps)'; then
    fail "左右轴位置微调必须先暂存 direction/steps，等 F4 DONE 后才能累计回中偏移"
fi
if printf '%s\n' "$lateral_realtime_position_block" | grep -q 'sendF4ActuatorVelocityMove'; then
    fail "左右轴实时微调禁止再发送 ACTUATOR_VEL_MOVE，避免超时或 STOP 异常后电机持续转动"
fi
if ! printf '%s\n' "$settle_timer_block" | grep -q 'autoVisionRequestFineTuneLocate(true)'; then
    fail "左右轴位置微调 DONE 后继续 LOCATE 时必须保留本轮实时微调总超时起点，不能每走一步重置 10 秒"
fi
fine_tune_request_block="$(sed -n '/function autoVisionRequestFineTuneLocate/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$fine_tune_request_block" | grep -q 'preserveRealtimeSession'; then
    fail "autoVisionRequestFineTuneLocate() 必须支持保留实时微调会话起点，供左右轴位置模式多步复查使用"
fi
if ! printf '%s\n' "$fine_tune_block" | grep -q 'autoVisionRealtimeSelectAxis'; then
    fail "ROI 实时闭环必须按 errorX/errorY 选择主误差轴，不能固定只先调一根轴"
fi
if ! printf '%s\n' "$fine_tune_block" | grep -q 'autoVisionRealtimeSpeedForAxis'; then
    fail "ROI 实时闭环必须按误差档位和无改善次数动态调速"
fi
if ! printf '%s\n' "$fine_tune_block" | grep -q 'autoVisionRealtimeTuneTimeoutMs'; then
    fail "ROI 实时闭环必须限制总微调时长，避免流程拖太久"
fi
fine_tune_timeout_block="$(sed -n '/if (elapsedMs >= autoVisionRealtimeTuneTimeoutMs)/,/selectedAxis = autoVisionRealtimeSelectAxis/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$fine_tune_timeout_block" | grep -q 'autoVisionStopRealtimeFineTune'; then
    fail "ROI 实时闭环超时后必须统一走 STOP 收口，不能继续保持速度微调"
fi
if ! printf '%s\n' "$fine_tune_timeout_block" | grep -q '"detect"'; then
    fail "ROI 实时闭环超时后必须进入自动模型检测流程，不能继续微调、切到对焦等待或停在人工复核"
fi
if ! printf '%s\n' "$fine_tune_timeout_block" | grep -q 'true'; then
    fail "ROI 实时闭环总超时进入模型检测前必须强制发送全部执行器 STOP，避免本地状态轴已清空但物理电机仍在转"
fi
if grep -q '模型检测/人工复核' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "QML 实时微调超时文案必须明确进入自动模型检测流程，不能再写成模型检测/人工复核"
fi
fine_tune_centered_block="$(awk '/if \(xCentered && yCentered\)/{flag=1} flag{print} flag && /return/{exit}' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$fine_tune_centered_block" | grep -q '"focus"'; then
    fail "ROI 实时闭环居中通过后必须进入 focus 对焦稳定阶段"
fi
if ! printf '%s\n' "$fine_tune_centered_block" | grep -q 'true'; then
    fail "ROI 实时闭环居中进入 focus 前也必须强制发送全部执行器 STOP，避免模型检测时左右轴仍在速度模式转动"
fi
fine_tune_speed_fail_block="$(awk '/autoVisionStopRealtimeFineTune\("实时微调速度命令未启动/{flag=1} flag{print} flag && /\)/{exit}' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$fine_tune_speed_fail_block" | grep -q '"detect"'; then
    fail "实时微调速度命令未启动时必须进入自动模型检测流程"
fi
if ! printf '%s\n' "$fine_tune_speed_fail_block" | grep -q 'true'; then
    fail "实时微调速度命令未启动并进入检测前必须强制发送全部执行器 STOP，避免残留速度运动"
fi
require_grep "已硬停电机，等待 F4" "qml/Main.qml"
require_grep "requestImmediateAutoControlInterruption" "qml/Main.qml"
require_fixed_grep "sendF4ActuatorStopNow(255, 0)" "qml/Main.qml"
if ! printf '%s\n' "$fine_tune_block" | grep -q 'autoVisionRealtimeFineTuneLostFrames'; then
    fail "ROI 实时闭环必须区分连续丢目标场景并触发停机收口"
fi
fine_tune_lost_block="$(sed -n '/if (!ok || !result || Number(result.has_target) !== 1/,/autoVisionRealtimeFineTuneLostFrames = 0/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$fine_tune_lost_block" | grep -q 'elapsedMs >= autoVisionRealtimeTuneTimeoutMs'; then
    fail "ROI 实时闭环丢帧分支也必须检查 10 秒总超时，丢帧等待时间不能绕过超时保护"
fi
if ! printf '%s\n' "$fine_tune_lost_block" | grep -q '"detect"'; then
    fail "ROI 实时闭环丢帧累计到总超时后必须进入自动模型检测流程"
fi
if ! printf '%s\n' "$fine_tune_lost_block" | grep -q 'ROI 实时闭环丢帧，已停止当前电机'; then
    fail "ROI 实时闭环单帧丢目标时必须立即停止当前持续运动，不能等目标恢复前继续移动电机"
fi
if ! printf '%s\n' "$fine_tune_lost_block" | grep -q '"resume"'; then
    fail "ROI 实时闭环丢帧但未超时时应 STOP 后继续等待下一帧 LOCATE，不能重置总超时起点"
fi
if printf '%s\n' "$fine_tune_block" | grep -q 'var targetY = Math.round(height / 2)'; then
    fail "Z 下降后的 ROI 复查不能继续只用整帧中心；必须按模型 ROI 中心和 bbox 是否完整在 ROI 内共同判定"
fi
if ! printf '%s\n' "$fine_tune_block" | grep -q 'autoVisionFineTuneModelRoiGeometry(result'; then
    fail "ROI 实时闭环必须先计算中心模型 ROI 几何，避免零件未完全进入绿色 ROI 时直接进入模型检测"
fi
if ! printf '%s\n' "$fine_tune_block" | grep -q 'autoVisionFineTuneBboxContainmentError'; then
    fail "ROI 实时闭环必须检查 bbox 是否完整落在模型 ROI 内，不能只看中心点误差"
fi
if ! printf '%s\n' "$fine_tune_block" | grep -q 'containmentErrorX'; then
    fail "ROI 实时闭环必须把 bbox 左右越界转换成左右轴误差，确保下降后零件半出 ROI 时仍继续左右微调"
fi
if ! grep -q 'property int autoVisionRealtimeTuneTimeoutMs: 10000' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "实时闭环微调总超时必须默认限制为 10000ms，超过 10 秒必须停机并进入模型检测"
fi
if ! grep -q 'property int autoVisionRealtimeCommandGuardMs: 1500' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "实时闭环速度命令必须有 1500ms 本地兜底，避免 ACTUATOR_VEL_MOVE 回调丢失后长期卡住"
fi
if ! grep -q 'property int autoVisionRealtimeStopGuardMs: 1200' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "实时闭环 STOP 收口必须有 1200ms 本地兜底，避免超时后等待 STOP 回调而卡死"
fi
if ! grep -q 'property int autoVisionLateralReturnTimeoutMs: 3000' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "左右轴快速回位超时必须默认限制为 3000ms"
fi
if ! grep -q 'property int autoVisionRealtimeTunePollMs: 90' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "实时闭环 LOCATE 轮询周期必须收紧到 90ms，避免微调反馈仍然过慢"
fi
if ! grep -q 'property int autoVisionRealtimeFineTuneSwitchDeadbandPx: 8' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "实时闭环切轴/反向必须保留 8px 防抖门槛，避免误差边缘来回抽动"
fi
require_grep "autoVisionFineTuneStepsForError" "qml/Main.qml"
conveyor_fine_tune_block="$(sed -n '/function autoVisionFineTuneConveyor/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
lateral_fine_tune_block="$(sed -n '/function autoVisionFineTuneLateral/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! grep -q 'property int autoVisionFineTuneMaxStepMultiplier: 12' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "ROI 微调最大放大倍数必须为 12，避免 minStep=300 时 X/Y 偏差大但单次动作仍偏小"
fi
if ! grep -q 'property int autoVisionFineTuneStepScalePx: 4' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "ROI 微调像素放大比例必须为每 4px 加一档，避免 errorX/errorY 明显偏离但 steps 增长太慢"
fi
if printf '%s\n' "$conveyor_fine_tune_block" | grep -q 'var steps = Math.max(1, Math.floor(Number(motor.minStep || 1)))'; then
    fail "传送带 ROI 微调不能只固定使用 minStep；必须按 errorY 超出死区的像素差放大步数"
fi
if printf '%s\n' "$lateral_fine_tune_block" | grep -q 'var steps = Math.max(1, Math.floor(Number(motor.minStep || 1)))'; then
    fail "左右轴 ROI 微调不能只固定使用 minStep；必须按 errorX 超出死区的像素差放大步数"
fi
if printf '%s\n' "$lateral_fine_tune_block" | grep -q 'var direction = errorX > 0 ? 0 : 1'; then
    fail "左右轴 ROI 微调方向不能把 errorX>0 映射到 direction=0；零件在 ROI 右侧时应发送 direction=1，让相机右移后画面向左回中心"
fi
if ! printf '%s\n' "$lateral_fine_tune_block" | grep -q 'var direction = errorX > 0 ? 1 : 0'; then
    fail "左右轴 ROI 微调必须把 errorX>0 映射到 ACTUATOR_POS_MOVE direction=1"
fi
if ! printf '%s\n' "$conveyor_fine_tune_block" | grep -q 'autoVisionFineTuneStepsForError(errorY'; then
    fail "传送带 ROI 微调必须通过 autoVisionFineTuneStepsForError(errorY, ...) 计算实际 steps"
fi
if ! printf '%s\n' "$lateral_fine_tune_block" | grep -q 'cameraLateralFineTuneFixedSteps()'; then
    fail "左右轴 ROI 微调必须使用参数页 lateralFineTuneFixedSteps 固定步数，避免继续按速度或隐式缩放导致停不住"
fi
require_grep "autoVisionLateralReturnOffsetSteps" "qml/Main.qml"
require_grep "resetAutoVisionLateralReturnState" "qml/Main.qml"
require_grep "autoVisionUpdateRealtimeLateralEstimate" "qml/Main.qml"
require_grep "autoVisionRequestLateralReturnToBeltCenter" "qml/Main.qml"
if ! grep -q 'property real autoVisionLateralReturnSpeedMultiplier: 1.8' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "左右轴快速回位必须有独立的速度倍率，不能继续复用普通微调速度"
fi
start_arm_after_z_block="$(sed -n '/function autoVisionStartF4ArmInspectionAfterZUp/,/function autoVisionRequestZUp/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$start_arm_after_z_block" | grep -q 'autoVisionLateralReturnOffsetSteps !== 0'; then
    fail "Z 轴回升后只有左右轴本轮确实动过时才允许追加左右轴回中，不能无条件发回中动作"
fi
if ! printf '%s\n' "$start_arm_after_z_block" | grep -q 'autoVisionRequestLateralReturnToBeltCenter'; then
    fail "Z 轴回升后启动机械臂前必须先按左右轴累计偏移反向回中"
fi
actuator_finished_block="$(sed -n '/onF4ActuatorCommandFinished:/,/if (root.stepperHomePendingCommand !== "")/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$actuator_finished_block" | grep -q 'ACTUATOR_VEL_MOVE'; then
    fail "执行器完成回调必须单独处理实时闭环 ACTUATOR_VEL_MOVE"
fi
fine_tune_request_block="$(sed -n '/function autoVisionRequestFineTuneLocate/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$fine_tune_request_block" | grep -q 'autoVisionActuatorPhase = ""'; then
    fail "进入 ROI 实时微调前必须清空 Z 轴旧阶段，避免 z-motion-down-wait 截获后续速度和 STOP 回调"
fi
realtime_callback_line="$(printf '%s\n' "$actuator_finished_block" | grep -n 'if (root.autoVisionRealtimeFineTuneActive' | head -n 1 | cut -d: -f1)"
position_phase_line="$(printf '%s\n' "$actuator_finished_block" | grep -n 'if (root.autoVisionActuatorPhase !== "")' | head -n 1 | cut -d: -f1)"
if [ -z "$realtime_callback_line" ] || [ -z "$position_phase_line" ] || [ "$realtime_callback_line" -ge "$position_phase_line" ]; then
    fail "ACTUATOR_VEL_MOVE/STOP_NOW 实时闭环回调必须先于通用位置阶段处理，避免残留阶段吞掉 STOP 收口"
fi
vel_move_realtime_block="$(sed -n '/if (action === "ACTUATOR_VEL_MOVE") {/,/if (action === "ACTUATOR_STOP_NOW") {/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$vel_move_realtime_block" | grep -q 'root.autoVisionCommandBusy = false'; then
    fail "ACTUATOR_VEL_MOVE 成功或失败回调必须释放 autoVisionCommandBusy，否则 autoVisionTimer 不会继续 LOCATE，微调会一直按旧方向运行"
fi
if ! printf '%s\n' "$actuator_finished_block" | grep -q 'ACTUATOR_STOP_NOW'; then
    fail "执行器完成回调必须单独处理实时闭环 STOP_NOW 收口"
fi
if ! printf '%s\n' "$actuator_finished_block" | grep -q 'root.autoVisionActuatorPhase === "lateral-return-fast"'; then
    fail "左右轴快速回中完成回执必须有独立阶段处理，回中完成后才能继续 F4/ESP32S3 机械臂流程"
fi
if ! grep -q 'id: autoVisionRealtimeCommandGuardTimer' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "QML 必须提供实时微调速度命令兜底 Timer，防止速度命令回调丢失后 autoVisionCommandBusy 永远为 true"
fi
if ! grep -q 'id: autoVisionRealtimeStopGuardTimer' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "QML 必须提供实时微调 STOP 收口兜底 Timer，防止 STOP 回调丢失后一直停在等待综合判定前"
fi
if grep -qE 'root\.[A-Za-z0-9_]*Timer\.(stop|start|restart)\(' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "QML 的 Timer id 不能写成 root.xxxTimer.stop()/restart()；id 不是 root 属性，板端会 TypeError 并打断 STOP 收口"
fi
require_fixed_grep "isMp157ToF4RequestCommand" "main.cpp"
require_fixed_grep "isMp157ToF4RequestCommand(reply->command)" "main.cpp"
require_fixed_grep "跳过MP157请求帧/串口回显" "main.cpp"
require_fixed_grep "DEFAULT_F4_SERIAL_BAUD = 57600" "main.cpp"
require_fixed_grep "串口 /dev/ttySTM2 57600" "qml/Main.qml"
if grep -Eq '/dev/ttySTM2[^0-9A-Za-z_]+115200|DEFAULT_F4_SERIAL_BAUD[[:space:]]*=[[:space:]]*115200' "$SCRIPT_DIR/main.cpp" "$SCRIPT_DIR/qml/Main.qml"; then
    fail "MP157-F4 主链路默认波特率必须保持 57600，不能退回 /dev/ttySTM2 115200"
fi
if ! grep -q '已发送 HEARTBEAT，等待F4 ACK刷新在线状态' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "刷新 F4 在线状态按钮必须提示实际发送的是 HEARTBEAT，不能继续误写 STATUS"
fi
stop_realtime_block="$(sed -n '/function autoVisionStopRealtimeFineTune/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
finalize_realtime_block="$(sed -n '/function autoVisionFinalizeRealtimeFineTuneStop/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! grep -q 'property bool autoVisionRealtimeStopInFlight: false' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "ROI 实时微调必须提供独立 STOP 收口互斥标志，避免遗留 LOCATE 结果重复发送全轴 STOP"
fi
if ! printf '%s\n' "$stop_realtime_block" | grep -q 'if (autoVisionRealtimeStopInFlight)'; then
    fail "autoVisionStopRealtimeFineTune() 必须在已有 STOP 收口进行中时直接返回，禁止重复发送 STOP"
fi
if ! printf '%s\n' "$stop_realtime_block" | grep -q 'autoVisionRealtimeFineTuneActive = false'; then
    fail "实时微调开始 STOP 收口时必须立即关闭 active 标志，阻止遗留 LOCATE 回调再次进入停止分支"
fi
if ! printf '%s\n' "$stop_realtime_block" | grep -q 'autoVisionTimer.stop'; then
    fail "实时微调开始 STOP 收口时必须立即停止 LOCATE 定时器，不能等异步 STOP 回调后才停止"
fi
if ! printf '%s\n' "$fine_tune_block" | grep -q 'autoVisionRealtimeCommandGuardTimer.restart'; then
    fail "发送 ACTUATOR_VEL_MOVE 后必须启动速度命令兜底 Timer"
fi
if ! printf '%s\n' "$stop_realtime_block" | grep -q 'autoVisionRealtimeStopGuardTimer.restart'; then
    fail "发送 ACTUATOR_STOP_NOW 后必须启动 STOP 收口兜底 Timer"
fi
if ! printf '%s\n' "$stop_realtime_block" | grep -q 'forceAllActuators'; then
    fail "实时微调 STOP 收口必须支持强制全轴 STOP，用于超时后兜住本地状态和物理电机状态不一致"
fi
if ! printf '%s\n' "$stop_realtime_block" | grep -q 'stopActuator'; then
    fail "实时微调 STOP 收口必须用明确的 stopActuator 选择单轴或全部执行器"
fi
stop_now_cpp_block="$(sed -n '/Q_INVOKABLE bool sendF4ActuatorStopNow/,/return startF4ActuatorStopNowCommand/p' "$SCRIPT_DIR/main.cpp")"
if ! printf '%s\n' "$stop_now_cpp_block" | grep -q 'const quint16 cycleId = 0U'; then
    fail "sendF4ActuatorStopNow() 必须固定使用 cycle_id=0，让强制 STOP 兼容旧 F4 固件和 cycle 漂移"
fi
if ! printf '%s\n' "$stop_now_cpp_block" | grep -q '强制 STOP 使用 cycle_id=0'; then
    fail "sendF4ActuatorStopNow() 必须在注释中说明强制 STOP 使用 cycle_id=0 的安全原因"
fi
if ! printf '%s\n' "$finalize_realtime_block" | grep -q 'autoVisionRealtimeCommandGuardTimer.stop'; then
    fail "实时微调最终收口时必须停止速度命令兜底 Timer"
fi
if ! printf '%s\n' "$finalize_realtime_block" | grep -q 'autoVisionRealtimeStopGuardTimer.stop'; then
    fail "实时微调最终收口时必须停止 STOP 兜底 Timer"
fi
if ! printf '%s\n' "$finalize_realtime_block" | grep -q 'autoVisionCommandBusy = false'; then
    fail "实时微调最终收口必须释放 autoVisionCommandBusy，否则超时后 autoVisionTimer 会一直被挡住"
fi
settle_timer_block="$(sed -n '/id: autoVisionActuatorSettleTimer/,/Connections {/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$settle_timer_block" | grep -q 'autoVisionActuatorPhase === "focus-settle"'; then
    fail "autoVisionActuatorSettleTimer 必须单独处理 focus-settle 阶段"
fi
if ! printf '%s\n' "$settle_timer_block" | grep -q 'autoVisionStartDetectDelay'; then
    fail "focus-settle 结束后必须再进入检测延时，而不是继续 ROI 复查"
fi
z_up_ack_block="$(sed -n '/root.autoVisionActuatorPhase === "z-up"/,/root.autoVisionActuatorPhase = ""/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$z_up_ack_block" | grep -q 'root.autoVisionActuatorPhase = "z-motion-up-wait"'; then
    fail "Z 回升完成回调必须切到 z-motion-up-wait，再由统一 DONE 处理函数推进机械臂流程"
fi
if printf '%s\n' "$z_up_ack_block" | grep -q 'autoVisionStartZMotionWait'; then
    fail "Z 回升回调中的 ok 已经表示 C++ 等到 F4 DONE，不能再叠加本地估算等待"
fi
if ! printf '%s\n' "$z_up_ack_block" | grep -q 'autoVisionHandleActuatorMoveDone'; then
    fail "Z 回升必须等待 F4 ACTUATOR_MOVE_DONE 事件确认电机真实到位，不能只靠本地估算等待启动机械臂"
fi
if printf '%s\n' "$z_up_ack_block" | grep -q 'requestF4ArmInspectionFlow'; then
    fail "Z 回升 ACK 后不能直接启动 F4 机械臂流程，避免检测头未离开零件就抓取"
fi
z_motion_timer_block="$(sed -n '/id: autoVisionActuatorSettleTimer/,/Connections {/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$z_motion_timer_block" | grep -q 'z-motion-down-wait'; then
    fail "Z 轴本地保护定时器必须处理 z-motion-down-wait 超时"
fi
if ! printf '%s\n' "$z_motion_timer_block" | grep -q 'z-motion-up-wait'; then
    fail "Z 轴本地保护定时器必须处理 z-motion-up-wait 超时"
fi
z_motion_down_timer_block="$(sed -n '/root.autoVisionActuatorPhase === "z-motion-down-wait"/,/} else if (root.autoVisionActuatorPhase === "z-motion-up-wait")/p' "$SCRIPT_DIR/qml/Main.qml")"
z_motion_up_timer_block="$(sed -n '/root.autoVisionActuatorPhase === "z-motion-up-wait"/,/} else if (root.autoVisionActuatorPhase === "z-down-skip"/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$z_motion_down_timer_block" | grep -q 'autoVisionRequestFineTuneLocate'; then
    fail "Z 下降参数页超时到时必须继续 ROI 复查，不能再次卡在等待 F4 DONE"
fi
if printf '%s\n' "$z_motion_down_timer_block" | grep -q '禁止进入ROI复查'; then
    fail "Z 下降参数页超时不能再提示禁止进入 ROI 复查"
fi
if ! printf '%s\n' "$z_motion_up_timer_block" | grep -q '未收到F4 ACTUATOR_MOVE_DONE'; then
    fail "Z 回升本地保护超时必须提示未收到 F4 ACTUATOR_MOVE_DONE，不能冒充正常到位"
fi
if printf '%s\n' "$z_motion_up_timer_block" | grep -q 'autoVisionStartF4ArmInspectionAfterZUp'; then
    fail "Z 回升本地保护超时不能启动机械臂；正常推进必须来自 F4 ACTUATOR_MOVE_DONE"
fi
require_grep "AUTO_LOCATE_MIN_LUMA_DELTA 12U" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_MAX_LUMA_DELTA 45U" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_MIN_BODY_LUMA_DELTA" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_BODY_LUMA_DELTA_PERCENT" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_LUMA_HISTOGRAM_BUCKETS 256U" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_BELT_ROW_PERCENTILE" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_BELT_MIN_DARK_ROW_PERCENT" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_BELT_VERTICAL_PADDING_PX" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_BELT_FALLBACK_HEIGHT" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_MIN_SOLID_DENSITY_PERCENT" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_MAX_SOLID_DENSITY_PERCENT" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_CENTER_HOLE_SAMPLE_DIVISOR" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_MAX_DARK_FILL_PERCENT" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_DARK_EDGE_MARGIN_PX" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_MAX_DARK_EDGE_AREA_PERCENT" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_MIN_RING_BACKGROUND_CONTRAST" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_MIN_PART_BBOX_AREA" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_MIN_PART_BBOX_SIDE" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_MIN_ACCEPT_CONFIDENCE" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_MIN_NON_RING_CONFIDENCE" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_RING_REQUIRED_BBOX_SIDE" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_REQUIRE_RING_HOLE_FOR_TARGET" "uvc_kms_overlay.c"
require_grep "LOCATE_DIAG_NONE" "uvc_kms_overlay.c"
require_grep "LOCATE_DIAG_RING" "uvc_kms_overlay.c"
require_grep "auto_locate_record_reject_candidate" "uvc_kms_overlay.c"
require_grep "auto_locate_luma_percentile" "uvc_kms_overlay.c"
require_grep "auto_locate_is_bright_candidate_luma" "uvc_kms_overlay.c"
require_grep "auto_locate_body_threshold_from_delta" "uvc_kms_overlay.c"
require_grep "auto_locate_measure_belt_row" "uvc_kms_overlay.c"
require_grep "auto_locate_find_dark_belt_band" "uvc_kms_overlay.c"
require_grep "auto_locate_expand_belt_band_to_model_roi" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_OVERSIZE_REFINE_MAX_SIDE" "uvc_kms_overlay.c"
require_grep "auto_locate_refine_oversized_ring_candidate" "uvc_kms_overlay.c"
require_absent "luma >= bright_threshold \\|\\| luma <= dark_threshold" "uvc_kms_overlay.c"
require_absent "roi_h = frame->frame_height;" "uvc_kms_overlay.c"
require_grep "auto_locate_component_touches_search_edge" "uvc_kms_overlay.c"
require_grep "auto_locate_component_has_ring_hole" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_MIN_RING_SIDE_BODY_PERCENT" "uvc_kms_overlay.c"
require_grep "AUTO_LOCATE_MIN_RING_BBOX_SIDE" "uvc_kms_overlay.c"
require_grep "ring_top_body" "uvc_kms_overlay.c"
require_grep "ring_bottom_body" "uvc_kms_overlay.c"
require_grep "ring_left_body" "uvc_kms_overlay.c"
require_grep "ring_right_body" "uvc_kms_overlay.c"
if ! grep -q '#define AUTO_LOCATE_MIN_RING_SIDE_BODY_PERCENT 18U' "$SCRIPT_DIR/uvc_kms_overlay.c"; then
    fail "ring 四边主体支撑阈值必须至少收紧到 18%，否则黑色传送带突起和白边仍可能被误判成 ring=1"
fi
ring_hole_block="$(sed -n '/static int auto_locate_component_has_ring_hole/,/^}/p' "$SCRIPT_DIR/uvc_kms_overlay.c")"
if ! printf '%s\n' "$ring_hole_block" | grep -q 'bbox_w < AUTO_LOCATE_MIN_RING_BBOX_SIDE'; then
    fail "ring 检测必须先检查候选 bbox 最小边长，避免小突起靠中心暗窗伪装成垫圈孔"
fi
require_fixed_grep 'result.insert(QStringLiteral("has_ring"), tokenValue(reply, QStringLiteral("ring")).toInt())' "main.cpp"
require_fixed_grep 'result.insert(QStringLiteral("diag"), tokenValue(reply, QStringLiteral("diag")).toInt())' "main.cpp"
require_fixed_grep 'result.insert(QStringLiteral("cand_ring"), tokenValue(reply, QStringLiteral("cand_ring")).toInt())' "main.cpp"
require_fixed_grep 'result.insert(QStringLiteral("cand_conf"), tokenValue(reply, QStringLiteral("cand_conf")).toInt())' "main.cpp"
require_fixed_grep '+ " ring=" + Math.round(hasRing)' "qml/Main.qml"
require_grep "property int autoVisionExpectedPartMaxBboxArea: 45000" "qml/Main.qml"
require_grep "property int autoVisionFirstDetectNonRingConfirmRequired" "qml/Main.qml"
require_grep "property int autoVisionFirstDetectMinNonRingConfidence" "qml/Main.qml"
require_grep "property bool autoVisionAllowNonRingFirstDetect: false" "qml/Main.qml"
require_grep "property int autoVisionFirstDetectMinRingBboxSide: 45" "qml/Main.qml"
require_grep "property int autoVisionFirstDetectMinRingBboxArea: 2000" "qml/Main.qml"
require_grep "autoVisionFirstDetectRequiredFramesForCandidate" "qml/Main.qml"
require_grep "autoVisionRejectSmallRingBeltBumpFirstDetect" "qml/Main.qml"
require_grep "autoVisionCandidateBoxFromText" "qml/Main.qml"
require_grep "autoVisionLastLocateDiagText" "qml/Main.qml"
locate_filter_block="$(sed -n '/ring_hole 检测作为/,/best_score = score/p' "$SCRIPT_DIR/uvc_kms_overlay.c")"
locate_body_block="$(sed -n '/body_threshold = auto_locate_body_threshold_from_delta/,/bbox_w = max_x - min_x/p' "$SCRIPT_DIR/uvc_kms_overlay.c")"
locate_roi_select_block="$(sed -n '/if (!auto_locate_find_dark_belt_band/,/pixel_count = roi_w \* roi_h/p' "$SCRIPT_DIR/uvc_kms_overlay.c")"
locate_pre_ring_filter_block="$(sed -n '/bbox_w = max_x - min_x/,/ring_hole 检测作为/p' "$SCRIPT_DIR/uvc_kms_overlay.c")"
oversized_bbox_filter_block="$(sed -n '/过大 bbox 先尝试环孔局部收缩/,/LOCATE_DIAG_BBOX/p' "$SCRIPT_DIR/uvc_kms_overlay.c")"
if ! printf '%s\n' "$locate_body_block" | grep -q 'start_luma, bright_threshold'; then
    fail "LOCATE 必须继续使用高亮阈值作为连通域种子，避免把灰色皮带整体并入候选"
fi
if ! printf '%s\n' "$locate_body_block" | grep -q 'next_luma, body_threshold'; then
    fail "LOCATE 必须用较低 body_threshold 扩张金属主体，避免银色垫圈阴影把亮环切碎后无法形成 ring"
fi
if ! printf '%s\n' "$locate_roi_select_block" | grep -q 'auto_locate_expand_belt_band_to_model_roi'; then
    fail "LOCATE 搜索带必须强制包含中心 300px 模型 ROI，避免垫圈高光把黑色传送带纵向暗带切断后只扫描零件上半截"
fi
if printf '%s\n' "$locate_pre_ring_filter_block" | grep -q 'LOCATE_DIAG_EDGE'; then
    fail "LOCATE 贴边过滤不能早于 ring_hole 检测，否则真实垫圈与右侧亮边短暂连通时会被 diag=5 一票否决"
fi
if ! printf '%s\n' "$oversized_bbox_filter_block" | grep -q 'auto_locate_refine_oversized_ring_candidate'; then
    fail "LOCATE 过大 bbox 不能在 ring 检测前直接 diag=3 失败，必须先尝试从粘连候选内部收缩出环孔局部 bbox"
fi
if ! printf '%s\n' "$oversized_bbox_filter_block" | grep -q 'oversized_refined'; then
    fail "LOCATE 过大 bbox 收缩结果必须有明确标志，避免收缩失败时误用旧的大 bbox 继续进入模型门禁"
fi
if ! printf '%s\n' "$locate_filter_block" | grep -q 'body_threshold'; then
    fail "LOCATE 中心孔检测必须使用 body_threshold 判断背景，不能只按过高的高亮种子阈值判断 ring"
fi
if ! printf '%s\n' "$locate_filter_block" | grep -q 'if (!has_ring'; then
    fail "LOCATE 不能只把环孔作为加分项；无环孔候选必须经过更高置信度和尺寸门槛，防止黑色传送带反光误触发"
fi
if ! grep -q 'AUTO_LOCATE_REQUIRE_RING_HOLE_FOR_TARGET 0U' "$SCRIPT_DIR/uvc_kms_overlay.c"; then
    fail "LOCATE 不能继续把 ring 作为 overlay 硬门槛；初次识别由 QML 要求 ring，跟踪阶段允许高置信 no-ring 候选避免垫圈高光造成持续丢失"
fi
if printf '%s\n' "$locate_filter_block" | grep -q 'require_ring_hole_for_target > 0U && !has_ring'; then
    fail "overlay 端不能再直接拒绝所有 no-ring 候选，否则垫圈中心孔受曝光影响后会从检测区域内持续丢失"
fi
if grep -q 'autoVisionAllowNonRingFirstDetect: true' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "QML 默认不能允许首次 no-ring 建链；黑色传送带静止反光会利用多帧稳定条件误触发自动流程"
fi
if ! grep -q 'firstDetectConfirmRequired = autoVisionFirstDetectRequiredFramesForCandidate(hasRing, confidence)' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "QML 首次建链必须按 ring/confidence 选择确认帧数，默认 ring=1 才能建链，调试开关打开后 no-ring 才能走更长确认"
fi
if ! grep -q 'if (!autoVisionAllowNonRingFirstDetect) {' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "QML 首次 no-ring 建链必须默认关闭，只允许在明确打开开关后才进入高置信多帧确认"
fi
if ! grep -q 'Number(confidence) < autoVisionFirstDetectMinNonRingConfidence' "$SCRIPT_DIR/qml/Main.qml"; then
    fail "QML 首次 no-ring 候选必须有更高置信度门槛，避免空传送带反光重新触发自动流程"
fi
require_grep "autoVisionRejectBlackBeltCandidate" "qml/Main.qml"
black_belt_guard_block="$(sed -n '/function autoVisionRejectBlackBeltCandidate/,/^    }/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$black_belt_guard_block" | grep -q 'Number(hasRing) === 1'; then
    fail "黑色传送带门禁必须保留 ring=1 候选，不能把真实中心孔目标也拒掉"
fi
if ! printf '%s\n' "$black_belt_guard_block" | grep -q 'Number(diagCode) === 5'; then
    fail "黑色传送带门禁必须识别 diag=5 的贴边候选，避免侧边黑带被当成零件重捕获"
fi
if ! printf '%s\n' "$black_belt_guard_block" | grep -q 'Number(candRing) !== 1'; then
    fail "黑色传送带门禁必须结合 cand_ring=0，避免没有中心孔结构的候选进入跟踪或微调"
fi
if ! printf '%s\n' "$black_belt_guard_block" | grep -q 'bboxH >= Math.floor(autoVisionFineTuneModelRoiSizePx \* 0.70)'; then
    fail "黑色传送带门禁必须识别接近中心 ROI 高度的大块黑带候选"
fi
if ! printf '%s\n' "$black_belt_guard_block" | grep -q 'bboxW >= Math.floor(autoVisionFineTuneModelRoiSizePx \* 0.35)'; then
    fail "黑色传送带门禁必须识别宽度达到中心 ROI 三分之一以上的黑带候选"
fi
if ! printf '%s\n' "$black_belt_guard_block" | grep -q 'mediumNoRingBelt'; then
    fail "黑色传送带门禁必须覆盖 cand_box≈130x179 这类中等尺寸突起，不能只拒绝接近整块 ROI 的大黑带"
fi
locate_tracking_block="$(sed -n '/function handleAutoVisionLocateFinished/,/function autoVisionNormalizeSpeedRpm/p' "$SCRIPT_DIR/qml/Main.qml")"
if ! printf '%s\n' "$locate_tracking_block" | grep -q 'autoVisionRejectBlackBeltCandidate(hasRing, diagCode, candRing'; then
    fail "首页自动视觉跟踪/重捕获必须复用黑色传送带门禁，不能只在首次建链拦截 no-ring 候选"
fi
if ! printf '%s\n' "$locate_tracking_block" | grep -q 'autoVisionRejectBlackBeltCandidate(0, diagCode, candRing'; then
    fail "首页自动视觉在 has_target=0 但 diag/cand_box 指向黑带突起时，也必须标记 black_belt=1，避免现场误以为已经识别到零件"
fi
if ! printf '%s\n' "$locate_tracking_block" | grep -q 'autoVisionRejectSmallRingBeltBumpFirstDetect(hasRing, bboxW, bboxH)'; then
    fail "首页首次建链必须拦截尺寸过小的 ring=1 候选，避免黑色传送带小突起伪装成零件"
fi
if ! printf '%s\n' "$fine_tune_block" | grep -q 'autoVisionRejectBlackBeltCandidate(hasRing, diagCode, candRing'; then
    fail "Z 下降后的 ROI 实时微调必须复用黑色传送带门禁，不能把黑带候选当作零件坐标继续微调或进入模型"
fi
detect_locate_guard_block="$(sed -n '/bool ensureCurrentFrameHasLocateTarget/,/^    QString parseTokenValue/p' "$SCRIPT_DIR/main.cpp")"
if ! printf '%s\n' "$detect_locate_guard_block" | grep -q 'kDetectLocateMinRingBboxSide'; then
    fail "模型入口必须拒绝过小的 ring=1 候选，防止小突起绕过 ring=1 门禁进入双模型"
fi
if ! printf '%s\n' "$detect_locate_guard_block" | grep -q '当前 ROI 候选尺寸过小'; then
    fail "模型入口过小候选拦截必须给出明确中文诊断，方便现场区分突起误检和真实零件"
fi
locate_reply_block="$(sed -n '/LOCATE has_target=/,/send_control_reply(client_fd, "OK", detail)/p' "$SCRIPT_DIR/uvc_kms_overlay.c")"
if ! printf '%s\n' "$locate_reply_block" | grep -q 'diag=%u'; then
    fail "LOCATE 回包必须包含 diag 字段，现场漏检时才能判断是否卡在 ring/bbox/density/threshold"
fi
if ! printf '%s\n' "$locate_reply_block" | grep -q 'roi_y=%u roi_h=%u'; then
    fail "LOCATE 回包必须包含搜索带 roi_y/roi_h，避免真实零件在画面中但不在算法搜索带内时无法排查"
fi
if ! printf '%s\n' "$locate_reply_block" | grep -q 'thr=%u,%u,%u'; then
    fail "LOCATE 回包必须包含 dark/body/bright 阈值，便于判断银色垫圈是否被亮度阈值挡掉"
fi
if ! printf '%s\n' "$locate_reply_block" | grep -q 'cand_box=%dx%d'; then
    fail "LOCATE 回包必须包含最接近候选 cand_box，便于判断 bbox 尺寸、密度或 ring 门槛是否拒绝真实零件"
fi
require_grep "OVERLAY_CONTROL_LOCATE_REPLY_TIMEOUT_MS" "main.cpp"
require_grep "overlayReplyTimeoutMs" "main.cpp"
overlay_control_query_block="$(sed -n '/static QString queryOverlayControlCommand/,/^    static QString queryOverlayStatus/p' "$SCRIPT_DIR/main.cpp")"
if ! printf '%s\n' "$overlay_control_query_block" | grep -q 'while (true)'; then
    fail "自动视觉 LOCATE 必须循环读取 overlay 一整行回复，不能只 read 一次导致 diag/cand_* 长回包被截断"
fi
if printf '%s\n' "$overlay_control_query_block" | grep -q 'char buffer\[256\]'; then
    fail "自动视觉 LOCATE 不能继续使用 256 字节固定短缓冲作为唯一读取窗口，长诊断回包会被截断"
fi
if ! printf '%s\n' "$overlay_control_query_block" | grep -q 'reply.contains'; then
    fail "自动视觉 LOCATE 读取 overlay 回复时必须以换行作为完整回包结束条件"
fi
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
    grep -q 'BINARY_PROTOCOL_CMD_MODEL_READY = 0x32U' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 MODEL_READY 必须使用独立命令 0x32，不能再和 WEIGHT_CALIBRATE 0x30 共用"
    grep -q 'BinaryProtocolService_HandleModelReady' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须实现 MODEL_READY 二进制处理入口"
    grep -q 'BinaryProtocolService_HandleArmJobStart' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须实现 ARM_JOB_START 二进制处理入口"
    grep -q 'BinaryProtocolService_SendWeightResult' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须提供 WEIGHT_RESULT 上报函数"
    grep -q 'BinaryProtocolService_SendLdcResult' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须提供 LDC_RESULT 上报函数"
    grep -q 'BinaryProtocolService_SendCycleDone' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须提供 CYCLE_DONE 上报函数"
    if grep -q 'BINARY_PROTOCOL_CMD_MODEL_READY = BINARY_PROTOCOL_CMD_WEIGHT_CALIBRATE' "$F4_BINARY_PROTOCOL_SOURCE"; then
        fail "F407 不能继续把 MODEL_READY 当作 WEIGHT_CALIBRATE 的历史别名"
    fi
    grep -q 'BINARY_PROTOCOL_WEIGHT_CALIBRATION_PAYLOAD_LENGTH' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须定义 5 字节称重标定负载长度"
    grep -q 'BinaryProtocolService_HandleWeightCalibration' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 必须实现 WEIGHT_CALIBRATE 二进制处理入口"
    grep -q 'WeightService_RequestCalibration' "$F4_BINARY_PROTOCOL_SOURCE" || fail "F407 WEIGHT_CALIBRATE 必须调用称重服务标定入口"
    grep -A80 'BinaryProtocolService_HandleVisionPos' "$F4_BINARY_PROTOCOL_SOURCE" | grep -q 'BinaryProtocolService_SendAck(payload.cycle_id, frame->sequence, frame->command, 0U)' || fail "F407 VISION_POS 成功后必须返回 ACK，不能静默成功"
    grep -A120 'BinaryProtocolService_HandleVisionLost' "$F4_BINARY_PROTOCOL_SOURCE" | grep -q 'BinaryProtocolService_SendAck(payload.cycle_id, frame->sequence, frame->command, 0U)' || fail "F407 VISION_LOST 成功后必须返回 ACK，不能静默成功"
    if grep -Eq '\[OK\]\[BIN\]|\[ERROR\]\[BIN\]' "$F4_BINARY_PROTOCOL_SOURCE"; then
        fail "F407 二进制协议处理路径不能再输出 [OK][BIN]/[ERROR][BIN] 文本回包，正确/错误必须用 ACK/NACK/STATUS_REPORT"
    fi
fi
F4_ROBOT_ARM_SOURCE="/e/hal/bisai_f407_project/User/App/robot_arm_service.c"
if [ -f "$F4_ROBOT_ARM_SOURCE" ]; then
    grep -q '#define ROBOT_ARM_SERVICE_ACTION_TIMEOUT_MS[[:space:]]*(100000U)' "$F4_ROBOT_ARM_SOURCE" || fail "F407 机械臂动作超时当前必须保留 100000ms，用于覆盖真实长动作"
    grep -q '#define ROBOT_ARM_SERVICE_STAGE_COMMAND_PAYLOAD_LEN[[:space:]]*(14U)' "$F4_ROBOT_ARM_SOURCE" || fail "F407 机械臂动作 payload 必须扩展到 14 字节，让 timeout_ms 按 u32 发送"
    grep -q 'uint32_t timeout_ms;' "$F4_ROBOT_ARM_SOURCE" || fail "F407 机械臂发送上下文 timeout_ms 必须是 uint32_t，不能再用 uint16_t"
    grep -q 'RobotArmService_WriteU32Le.*payload\[6\].*timeout_ms' "$F4_ROBOT_ARM_SOURCE" || fail "F407 发给 ESP32S3 的 timeout_ms 必须从 payload[6] 开始按 u32 小端写入"
    grep -q '#define ROBOT_ARM_SERVICE_DONE_EXTRA_TIMEOUT_MS[[:space:]]*(10000U)' "$F4_ROBOT_ARM_SOURCE" || fail "F407 DONE 额外等待余量必须和协议文档保持一致"
fi
require_repo_grep "ESP32S3 回包总原则" "docs/f4_esp32s3_arm_protocol/esp32s3_send_sequence.md"
require_repo_grep "cycle_id.*原样带回" "docs/f4_esp32s3_arm_protocol/esp32s3_send_sequence.md"
require_repo_grep "job_id.*首版.*cycle_id" "docs/f4_esp32s3_arm_protocol/esp32s3_send_sequence.md"
require_repo_grep "part_type.*1.*波形垫圈" "docs/f4_esp32s3_arm_protocol/esp32s3_send_sequence.md"
require_repo_grep "stage_id.*1.*称重" "docs/f4_esp32s3_arm_protocol/esp32s3_send_sequence.md"
require_repo_grep "timeout_ms.*u32.*100000" "docs/f4_esp32s3_arm_protocol/esp32s3_send_sequence.md"
require_repo_grep "#define ARM_LINK_STAGE_COMMAND_PAYLOAD_LENGTH[[:space:]]*\\(14U\\)" "docs/f4_esp32s3_arm_protocol/arm_link_protocol.h"
require_repo_grep "uint32_t timeout_ms" "docs/f4_esp32s3_arm_protocol/arm_link_protocol.h"
require_repo_grep "ArmLinkProtocol_WriteU32Le.*out_payload\\[6\\].*payload->timeout_ms" "docs/f4_esp32s3_arm_protocol/arm_link_protocol.c"
F4_UART_COMMAND_SOURCE="/e/hal/bisai_f407_project/User/App/uart_command.c"
if [ -f "$F4_UART_COMMAND_SOURCE" ]; then
    grep -q 'UART_COMMAND_MP157_TEXT_ENABLE' "$F4_UART_COMMAND_SOURCE" || fail "F407 MP157 主链路必须提供文本输出静默开关，主链路不能再收到 [OK]/[ERROR] 文本日志"
    grep -q '#define UART_COMMAND_MP157_TEXT_ENABLE[[:space:]]*(0U)' "$F4_UART_COMMAND_SOURCE" || fail "F407 MP157 主链路文本输出默认必须关闭，只保留二进制协议帧"
fi
F4_CAMERA_MOTOR_SOURCE="/e/hal/bisai_f407_project/User/App/camera_motor_service.c"
if [ -f "$F4_CAMERA_MOTOR_SOURCE" ]; then
    grep -q 'CAMERA_MOTOR_LATERAL_ADDRESS[[:space:]]*(3U)' "$F4_CAMERA_MOTOR_SOURCE" || fail "F407 摄像头左右轴默认地址必须是现场 ID 0x03"
    grep -q 'CAMERA_MOTOR_Z_ADDRESS[[:space:]]*(2U)' "$F4_CAMERA_MOTOR_SOURCE" || fail "F407 摄像头上下轴默认地址必须是现场 ID 0x02"
    grep -q 'CAMERA_MOTOR_COMMAND_QUEUE_LENGTH[[:space:]]*(4U)' "$F4_CAMERA_MOTOR_SOURCE" || fail "F407 摄像头电机队列不能再是长度 1 的覆盖队列"
    grep -q 'xQueueSendToBack' "$F4_CAMERA_MOTOR_SOURCE" || fail "F407 摄像头普通命令必须进入 FIFO，避免 STEPPER_PARAM_SET 被覆盖"
    grep -q 'xQueueSendToFront' "$F4_CAMERA_MOTOR_SOURCE" || fail "F407 摄像头 STOP 必须队首优先，避免连续运动停不下来"
    grep -q 'Runtime config applied' "$F4_CAMERA_MOTOR_SOURCE" || fail "F407 摄像头运行时配置应用后必须有可核对日志"
    grep -q 'CAMLAT' "$F4_CAMERA_MOTOR_SOURCE" || fail "F407 USART1 调试命令必须提供 CAMLAT 左右轴入口"
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
final_sort_handler_block="$(sed -n '/void handleF4FinalSortFinished(bool ok,/,/^    }/p' "$SCRIPT_DIR/main.cpp")"
if ! printf '%s\n' "$final_sort_handler_block" | grep -q 'm_f4AutoRunning = false'; then
    fail "main.cpp 收到 CYCLE_DONE 后必须清空 m_f4AutoRunning，下一轮 START_CYCLE 才不会复用旧 cycle 状态"
fi
if ! printf '%s\n' "$final_sort_handler_block" | grep -q 'm_f4AutoPaused = false'; then
    fail "main.cpp 收到 CYCLE_DONE 后必须清空 m_f4AutoPaused，避免停止/继续仍绑定到旧 cycle"
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
safe_remove_block="$(sed -n '/Q_INVOKABLE QString safeRemoveSdCard()/,/^    }/p' "$SCRIPT_DIR/main.cpp")"
if ! printf '%s\n' "$safe_remove_block" | grep -q 'setArguments(QStringList() << QStringLiteral("safe-remove"))'; then
    fail "safeRemoveSdCard() 必须调用 sdcard-safe-remove safe-remove，不能只启动裸命令导致板端返回 Usage"
fi

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
require_grep "当前四分类模型只输出平垫圈和弹性垫圈" "qml/Main.qml"
require_grep "GetOutputTypeInfo" "defect_classify.cpp"
require_grep "model_class_count" "defect_classify.cpp"
require_grep 'labels\.size\(\)[[:space:]]*!=[[:space:]]*model_class_count' "defect_classify.cpp"
require_grep "GetElementCount" "defect_classify.cpp"
require_grep "RESULT_SEG" "defect_segment.cpp"
require_grep "overlay_path" "defect_segment.cpp"
require_grep "mask_path" "defect_segment.cpp"
# 新分割模型只有背景和缺陷两个通道，板端程序必须读取 ONNX 输出形状，不能继续依赖旧六类常量。
require_grep "GetOutputTypeInfo" "defect_segment.cpp"
require_grep "GetElementType" "defect_segment.cpp"
require_grep "ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT" "defect_segment.cpp"
require_grep "model_class_count" "defect_segment.cpp"
require_grep "model_output_height" "defect_segment.cpp"
require_grep "model_output_width" "defect_segment.cpp"
# 后处理的元素数、argmax 和调色板都必须使用动态类别数，避免仅输出字段动态而计算仍写死六类。
require_grep 'class_count[[:space:]]*\*[[:space:]]*output_pixels' "defect_segment.cpp"
require_grep 'class_id[[:space:]]*<[[:space:]]*class_count' "defect_segment.cpp"
require_grep 'build_palette\(model_class_count\)' "defect_segment.cpp"
require_grep "defect_classifier_static_mixed_int8.onnx" "deploy_qt_camera_display.sh"
require_grep "defect_classifier_static_mixed_int8_labels.json" "deploy_qt_camera_display.sh"
require_grep "checkpoints_classify_4classes" "deploy_qt_camera_display.sh"
require_grep "defect_unet_test_decoder_head_int8.onnx" "deploy_qt_camera_display.sh"
# 默认源路径必须指向本轮生成的两类分割模型，板端目标文件名仍由部署脚本保持兼容。
require_grep "checkpoints_unet_2parts/scratch_unet_decoder_head_int8.onnx" "deploy_qt_camera_display.sh"
require_grep "defect-classify" "deploy_qt_camera_display.sh"
require_grep "defect-segment" "deploy_qt_camera_display.sh"
require_grep "libonnxruntime.so" "deploy_qt_camera_display.sh"

if grep -Eq 'MODEL_CLASS_COUNT[[:space:]]*=[[:space:]]*6' "$SCRIPT_DIR/defect_classify.cpp"; then
    fail "defect-classify 不能继续把分类类别数写死为 6，必须从 ONNX 输出维度读取并与 labels 数量核对"
fi

# 固定六类会让 [1,2,224,224] 输出被按六个平面越界读取，因此必须在部署新模型前彻底移除。
if grep -Eq 'MODEL_CLASS_COUNT[[:space:]]*=[[:space:]]*6' "$SCRIPT_DIR/defect_segment.cpp"; then
    fail "defect-segment 不能继续把分割类别数写死为 6，必须从 ONNX 输出维度读取"
fi

# 即使移除了旧常量，也不能在 argmax 循环中重新写入字面量 6。
if grep -Eq 'class_id[[:space:]]*<[[:space:]]*6([UuLl]*)' "$SCRIPT_DIR/defect_segment.cpp"; then
    fail "defect-segment 的 argmax 循环不能写死 6 类，必须使用 class_count"
fi

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
require_grep "CLOUD_WEIGHT_CONTEXT" "defect-cos-upload"
require_grep "CLOUD_LDC_CONTEXT" "defect-cos-upload"
require_grep "CLOUD_F4_FLOW_CONTEXT" "defect-cos-upload"
require_grep "CLOUD_DECISION_CONTEXT" "defect-cos-upload"
require_grep "CLOUD_VISION_CONTEXT" "defect-cos-upload"
require_grep "class_label_to_part_code" "defect-cos-upload"
require_grep "extract_part_id_by_code" "defect-cos-upload"
require_grep "auto_create_part" "defect-cos-upload"
require_grep "part_name" "defect-cos-upload"
require_grep "part_category" "defect-cos-upload"
require_grep "run_auto_create_part_self_test" "defect-cos-upload"
require_grep "device_context" "defect-cos-upload"
require_grep "sensor_context" "defect-cos-upload"
require_grep "decision_context" "defect-cos-upload"
require_grep "vision_context" "defect-cos-upload"
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
