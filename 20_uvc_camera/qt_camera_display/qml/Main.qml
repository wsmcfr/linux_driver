/*
 * Main.qml
 *
 * 作用：
 *   1024x600 工业缺陷检测主界面原型。
 *   当前版本只接入 UVC 摄像头实时预览，检测结果、统计和状态先用演示数据占位；
 *   后续可把这些属性替换为 C++ 控制器、串口数据或检测服务输出。
 */

import QtQuick 2.12
import IndustrialCamera 1.0

Rectangle {
    id: root

    /* width/height 是 7 寸 RGB 屏目标分辨率；QQuickView 会按窗口尺寸拉伸根对象。 */
    width: 1024
    height: 600

    /* 背景使用中性深灰，避免界面整体偏蓝，同时凸显视频和状态色。 */
    color: "#101214"

    /* cameraDeviceName 来自 C++ 的 context property，默认是 /dev/video0。 */
    property string cameraDeviceName: cameraDeviceId

    /* captureWidth/captureHeight/captureFps 来自 C++ 命令行参数，用于控制 CPU 占用。 */
    property int captureWidth: cameraCaptureWidth
    property int captureHeight: cameraCaptureHeight
    property int captureFps: cameraCaptureFps

    /* videoBackend 来自 C++，qt-safe 使用 V4L2VideoItem，gst-qml 使用 qmlglsink。 */
    property string videoBackend: cameraVideoBackend

    /* gstStatusText 保存 C++ 创建 GStreamer 管线的状态，便于 GL 后端失败时显示原因。 */
    property string gstStatusText: cameraGstStatusText

    /* usingGstVideo 表示当前画面区是否由 GstGLVideoItem 承载视频。 */
    property bool usingGstVideo: videoBackend === "gst-qml"

    /* usingKmsOverlay 表示摄像头画面由外部 KMS overlay plane 绘制，Qt 不占用 /dev/video0。 */
    property bool usingKmsOverlay: videoBackend === "kms-overlay"

    /* workflowState 表示当前产线状态；按钮会改变该文本，后续可接真实状态机。 */
    property string workflowState: "定位预览"

    /* storageState 表示检测流程、SD 卡安全卸载和告警快照的最近一次执行结果。 */
    property string storageState: "SD卡就绪"

    /* saveImageBusy 保留给旧 SSH 保存自检状态同步；首页已经不再暴露独立保存图片按钮。 */
    property bool saveImageBusy: false

    /* detectImageBusy 表示当前帧检测后台任务正在执行，用于禁止重复检测和提示按钮状态。 */
    property bool detectImageBusy: false

    /* detectState 保存检测按钮最近一次结果摘要；初始为等待用户手动触发。 */
    property string detectState: "等待检测"

    /* detectStatus 保存检测输出 GOOD/BAD/REVIEW/WAIT/ERROR 状态，决定结果色和主结果文案。 */
    property string detectStatus: "WAIT"

    /* detectPartName 保存模型识别出的零件名称，由类别名前缀提取，例如 gasket_good -> gasket。 */
    property string detectPartName: "未检测"

    /* detectClassName 保存模型输出的具体类别名，例如 washer_good 或 gasket_bad。 */
    property string detectClassName: "未检测"

    /* detectConfidenceText 保存模型输出置信度的百分制显示文本。 */
    property string detectConfidenceText: "--%"

    /* detectConfidencePercentText 保存进度条旁边显示的百分制置信度文本。 */
    property string detectConfidencePercentText: "--%"

    /* detectConfidenceRatio 保存 0~1 置信度比例，用于进度条宽度。 */
    property real detectConfidenceRatio: 0.0

    /* detectBadTotalText 保存 bad 类别总概率，辅助现场判断低置信度坏品趋势。 */
    property string detectBadTotalText: "坏品 --%"

    /* detectGoodTotalText 保存 good 类别总概率，辅助现场判断低置信度良品趋势。 */
    property string detectGoodTotalText: "良品 --%"

    /* detectTimeText 保存两个模型串行完成后的总检测耗时。 */
    property string detectTimeText: "耗时 -- ms"

    /* storageToastVisible 表示底部 SD 卡操作提示是否显示；Timer 到时后自动隐藏。 */
    property bool storageToastVisible: false

    /* activePage 保存左侧导航当前页面；home 是实时检测首页，history/stats/manual/settings/alarm 是全屏功能页。 */
    property string activePage: "home"

    /* historyPageVisible 给历史记录大页面做显隐判断，避免每个控件重复写字符串比较。 */
    property bool historyPageVisible: activePage === "history"

    /* statsPageVisible 给统计分析页面做显隐判断，统计页打开时需要隐藏实时画面和 overlay 视频层。 */
    property bool statsPageVisible: activePage === "stats"

    /* manualPageVisible 给手动控制页面做显隐判断，手动页打开时同样隐藏首页实时画面和 overlay 视频层。 */
    property bool manualPageVisible: activePage === "manual"

    /* settingsPageVisible 给参数设置页面做显隐判断；参数页打开时隐藏首页视频层，避免 KMS plane 遮挡控件。 */
    property bool settingsPageVisible: activePage === "settings"

    /* alarmPageVisible 给告警维护页面做显隐判断；告警页打开时隐藏首页视频层，突出故障处理信息。 */
    property bool alarmPageVisible: activePage === "alarm"

    /* manualMode 表示当前是否允许调试级手动动作；第一版由界面模拟，后续由 F4 状态覆盖。 */
    property bool manualMode: false

    /* manualBeltState 保存传送带模拟状态，便于按钮点击后立即给现场人员反馈。 */
    property string manualBeltState: "停止"

    /* manualBeltSpeed 保存传送带速度档位，第一版只作为界面显示和后续串口命令参数占位。 */
    property string manualBeltSpeed: "低速"

    /* manualArmState 保存机械臂模拟状态，后续会替换为 F4 ACK 或状态帧。 */
    property string manualArmState: "待机"

    /* manualLightState 保存光源组合状态，默认背光常亮符合当前检测演示流程。 */
    property string manualLightState: "背光常亮"

    /* manualActuatorState 保存夹爪模拟状态，第一版不直接驱动真实执行器。 */
    property string manualActuatorState: "夹爪关闭"

    /* manualReviewMark 保存人工复核标记，第一版只写界面状态，不回写检测记录。 */
    property string manualReviewMark: "未标记"

    /* manualLastAckText 保存最近一次手动命令响应文字，后续由 motionController 的 ACK 信号更新。 */
    property string manualLastAckText: "F4控制器待接入"

    /* manualEmergencyStop 表示急停模拟状态；为 true 时禁止除停止、刷新和清故障外的手动动作。 */
    property bool manualEmergencyStop: false

    /* manualArmHomeOk 表示机械臂是否已完成回零；抓取和放置动作会依赖这个安全前提。 */
    property bool manualArmHomeOk: false

    /* manualTopLightEnabled 表示上方补光是否开启，第一版只改变界面状态。 */
    property bool manualTopLightEnabled: false

    /* manualLightLevel 表示补光亮度档位，后续可作为串口命令中的亮度参数。 */
    property int manualLightLevel: 2

    /* settingsPartType 保存当前工艺参数对应的零件类型，第一版只影响界面摘要，后续可写入配置文件。 */
    property string settingsPartType: "平垫圈A"

    /* settingsDecisionThreshold 保存良坏分类置信度阈值，单位为千分比，便于和串口协议定点数保持一致。 */
    property int settingsDecisionThreshold: 850

    /* settingsReviewThreshold 保存低于该值时进入待复核的阈值，避免低置信度样本被强行分拣。 */
    property int settingsReviewThreshold: 650

    /* settingsFineCenterPx 保存精定位像素阈值，满足连续稳定后才允许抓拍检测图。 */
    property int settingsFineCenterPx: 8

    /* settingsStableFrames 保存连续稳定帧数，用于抑制传送带抖动造成的误触发。 */
    property int settingsStableFrames: 4

    /* settingsPulsePerPx 保存像素到编码器脉冲的标定比例，后续同步给 F4 做微调。 */
    property int settingsPulsePerPx: 12

    /* settingsBeltSpeed 保存参数页配置的自动输送低速档，单位 mm/s。 */
    property int settingsBeltSpeed: 40

    /* settingsSortTimeoutMs 保存分拣动作超时时间，超时后应进入告警维护流程。 */
    property int settingsSortTimeoutMs: 1500

    /* settingsAutoUpload 表示检测结果是否自动触发 COS 上传，当前只作为界面配置状态。 */
    property bool settingsAutoUpload: true

    /* settingsLastActionText 保存参数页最近一次应用、保存或恢复默认的结果提示。 */
    property string settingsLastActionText: "参数未修改"

    /* alarmCurrentCode 保存当前主告警码；第一版使用模拟值，后续由 F4/MP157 告警帧覆盖。 */
    property string alarmCurrentCode: "0x0007"

    /* alarmCurrentTitle 保存当前主告警名称，用于告警维护页顶部醒目展示。 */
    property string alarmCurrentTitle: "急停按下"

    /* alarmCurrentLevel 保存当前告警等级，影响页面颜色和处理建议优先级。 */
    property string alarmCurrentLevel: "严重"

    /* alarmCurrentTime 保存当前告警发生时间，第一版启动时使用固定演示值。 */
    property string alarmCurrentTime: "09:42:16"

    /* alarmAcknowledged 表示操作员是否已确认当前告警；确认不等于故障清除。 */
    property bool alarmAcknowledged: false

    /* alarmCleared 表示当前模拟告警是否已清除；真实接入后以 F4 状态帧为准。 */
    property bool alarmCleared: false

    /* historyDetailVisible 表示历史页是否进入“查看详情”二级页面；false 时只显示第一层检测记录列表。 */
    property bool historyDetailVisible: false

    /* selectedHistoryIndex 保存当前正在查看的历史记录索引；-1 表示尚未选择记录。 */
    property int selectedHistoryIndex: uploadHistory.count > 0 ? uploadHistory.count - 1 : -1

    /* selectedHistoryImageIndex 保存详情页左侧图片轮播当前页，多张图时左右滑动会改变它。 */
    property int selectedHistoryImageIndex: 0

    /* selectedHistoryRecord 保存当前选中记录的完整详情字段，QML 详情页统一从这里取值。 */
    property var selectedHistoryRecord: uploadHistory.entryAt(selectedHistoryIndex)

    /* historyAnalysisDetailVisible 表示是否打开检测信息完整说明浮层，解决云端长文在小面板中显示不全。 */
    property bool historyAnalysisDetailVisible: false

    /* dxPixels 表示视觉中心偏差演示值，后续由 tracking_service 写入。 */
    property int dxPixels: 3

    /* dailyTotal/dailyGood/dailyBad 是底部统计演示值，后续由记录服务写入。 */
    property int dailyTotal: 1256
    property int dailyGood: 1218
    property int dailyBad: 38

    /* currentTimeText 保存顶部状态栏时间，由 Timer 每秒刷新。 */
    property string currentTimeText: Qt.formatDateTime(new Date(), "hh:mm:ss")

    /* accentGreen 是良品、在线、通过状态的统一颜色。 */
    property color accentGreen: "#35d07f"

    /* accentAmber 是定位、等待、告警预警状态的统一颜色。 */
    property color accentAmber: "#f4b942"

    /* accentRed 是坏品、故障、离线状态的统一颜色。 */
    property color accentRed: "#ef5b5b"

    /* panelColor 是主要内容面板背景色，保持低反差但清楚分区。 */
    property color panelColor: "#191c1f"

    /* borderColor 是面板边框颜色，避免大面积卡片产生厚重感。 */
    property color borderColor: "#30363b"

    /* splashOverlayVisible 表示开机启动自检覆盖层是否还需要显示，启动完成淡出后设为 false。 */
    property bool splashOverlayVisible: true

    /* splashFadingOut 表示启动覆盖层正在淡出，淡出期间仍保持 visible，避免主界面提前闪出来。 */
    property bool splashFadingOut: false

    /* bootOverlayRestoreFinished 表示 Qt 启动遮罩是否已经完成淡出并进入主界面；相机在线回调必须等它为 true 后才能显示 KMS 视频层。 */
    property bool bootOverlayRestoreFinished: false

    /* bootOverlayRestoreAttempts 记录启动结束后恢复 KMS 视频层的重试次数，避免 overlay socket 晚于 Qt splash 就绪。 */
    property int bootOverlayRestoreAttempts: 0

    /* bootOverlayRestoreMaxAttempts 限制重试次数，避免 overlay 异常时 QML 一直发送控制命令。 */
    property int bootOverlayRestoreMaxAttempts: 16

    /* splashStageIndex 保存当前启动阶段索引，Timer 会按阶段推进。 */
    property int splashStageIndex: 0

    /* splashProgressValue 保存启动进度百分比，Behavior 会让进度条平滑过渡。 */
    property real splashProgressValue: 18

    /*
     * splashStageModel 保存启动自检阶段。
     * label 是大状态文字；detail 是说明文字；percent 是该阶段完成后的进度值。
     */
    property var splashStageModel: [
        {"label": "加载相机", "detail": "初始化 UVC 采集与 KMS 显示链路", "percent": 18},
        {"label": "初始化检测模型", "detail": "准备 ROI、分类阈值与结果融合参数", "percent": 42},
        {"label": "连接运动控制", "detail": "等待 STM32F4 心跳、急停与回零状态", "percent": 64},
        {"label": "挂载存储", "detail": "检查 SD 卡图片目录与本地历史记录", "percent": 84},
        {"label": "进入检测界面", "detail": "恢复实时检测面板并接管触摸操作", "percent": 100}
    ]

    /*
     * cameraIsActive 的作用：
     *   统一判断当前视频后端是否已准备好显示画面。
     *
     * 返回值：
     *   gst-qml 后端返回 Loader 是否已经加载 GstGLVideoItem；
     *   qt-safe 后端返回 V4L2VideoItem 是否已经收到视频帧。
     */
    function cameraIsActive() {
        if (usingGstVideo) {
            return gstVideoLoader.status === Loader.Ready
        }
        if (usingKmsOverlay) {
            return deviceHealth.cameraStatusText === "在线"
        }
        return cameraView.active
    }

    /*
     * cameraStatusText 的作用：
     *   把当前视频后端状态转换成界面可读中文。
     *
     * 返回值：
     *   返回“在线/GL在线/等待/失败原因”等状态文本。
     */
    function cameraStatusText() {
        if (usingGstVideo) {
            if (cameraIsActive()) {
                return "GL在线"
            }
            if (gstStatusText.length > 0) {
                return gstStatusText
            }
            return "GL等待"
        }

        if (usingKmsOverlay) {
            return deviceHealth.cameraStatusText
        }

        if (cameraView.active) {
            return "在线"
        }

        var status = cameraView.statusText
        if (status.indexOf("失败") >= 0 || status.indexOf("不支持") >= 0 || status.indexOf("不是") >= 0) {
            return status
        }

        if (status.length > 0) {
            return status
        }

        return "等待"
    }

    /*
     * cameraDotColor 的作用：
     *   根据当前视频后端状态选择状态点颜色。
     *
     * 返回值：
     *   在线返回绿色，错误返回红色，其它等待状态返回黄色。
     */
    function cameraDotColor() {
        if (usingKmsOverlay) {
            return deviceHealth.cameraStatusColor
        }

        if (cameraIsActive()) {
            return accentGreen
        }

        var status = usingGstVideo ? gstStatusText : cameraView.statusText
        if (status.indexOf("失败") >= 0 || status.indexOf("不支持") >= 0 || status.indexOf("不是") >= 0) {
            return accentRed
        }
        return accentAmber
    }

    /*
     * currentSplashStage 的作用：
     *   读取当前启动阶段对象，并在索引越界时回退到第一阶段，避免启动动画因异常索引断掉。
     *
     * 返回值：
     *   返回 splashStageModel 中的当前阶段对象。
     */
    function currentSplashStage() {
        if (splashStageIndex < 0 || splashStageIndex >= splashStageModel.length) {
            return splashStageModel[0]
        }

        return splashStageModel[splashStageIndex]
    }

    /*
     * splashStageColor 的作用：
     *   根据启动阶段选择状态色，前期使用绿色表示正常初始化，运动控制和存储阶段使用琥珀色表示等待外设。
     *
     * 返回值：
     *   返回当前阶段在启动画面中使用的主色。
     */
    function splashStageColor() {
        if (splashStageIndex === 2 || splashStageIndex === 3) {
            return accentAmber
        }

        return accentGreen
    }

    /*
     * advanceSplashStage 的作用：
     *   推进开机启动动画阶段，并同步更新进度值。
     *
     * 主要流程：
     *   1. 启动层已关闭时直接返回。
     *   2. 还没到最后一阶段时推进索引，并把进度设到该阶段目标值。
     *   3. 到达最后一阶段后进入淡出流程。
     *
     * 返回值：
     *   无返回值；函数只修改启动动画状态。
     */
    function advanceSplashStage() {
        if (!splashOverlayVisible || splashFadingOut) {
            return
        }

        if (splashStageIndex < splashStageModel.length - 1) {
            splashStageIndex += 1
            splashProgressValue = currentSplashStage().percent
            return
        }

        finishSplashAnimation()
    }

    /*
     * finishSplashAnimation 的作用：
     *   结束开机启动动画，让覆盖层先淡出再真正隐藏。
     *
     * 返回值：
     *   无返回值；函数会启动 splashHideTimer 完成最后隐藏动作。
     */
    function finishSplashAnimation() {
        splashProgressValue = 100
        splashFadingOut = true
        splashHideTimer.restart()
    }

    /*
     * setBootOverlayVisible 的作用：
     *   在开机启动动画阶段统一控制 KMS overlay 视频层是否显示。
     *
     * 主要流程：
     *   1. 只有 kms-overlay 后端才需要控制外部视频 plane。
     *   2. visible 为 false 时隐藏摄像头画面，避免视频层抢在 Qt 启动画面前盖住屏幕。
     *   3. visible 为 true 时在启动动画完全淡出后恢复实时摄像头画面。
     *
     * 参数：
     *   visible 表示目标视频层可见状态。
     *
     * 返回值：
     *   无返回值；控制失败只写日志，不阻塞 Qt 界面启动。
     */
    function setBootOverlayVisible(visible) {
        if (!root.usingKmsOverlay) {
            return ""
        }

        var result = visible
            ? storageController.setOverlayVisible(true)
            : storageController.setOverlayVisible(false)
        console.log("boot overlay visible", visible, result)
        return result
    }

    /*
     * startBootOverlayRestore 的作用：
     *   启动动画结束后恢复外部 KMS 摄像头视频层。
     *
     * 主要流程：
     *   1. 先立即发送一次 VISIBLE 1，覆盖 overlay 已经就绪的正常路径。
     *   2. 如果返回内容表现为 socket 未连接、切换失败或无结果，则启动短周期定时器继续重试。
     *   3. 达到最大重试次数后停止，避免 overlay 异常时一直占用 Qt 事件循环。
     *
     * 返回值：
     *   无返回值；恢复结果通过 console.log 写入板端 Qt 日志。
     */
    function startBootOverlayRestore() {
        if (!root.usingKmsOverlay) {
            return
        }

        root.bootOverlayRestoreFinished = true
        root.bootOverlayRestoreAttempts = 0
        var result = root.setBootOverlayVisible(true)
        if (root.overlayRestoreNeedsRetry(result)) {
            bootOverlayRestoreTimer.restart()
        }
    }

    /*
     * overlayRestoreNeedsRetry 的作用：
     *   根据 C++ 控制器返回文本判断是否需要再次发送 VISIBLE 1。
     *
     * 参数：
     *   result 是 storageController.setOverlayVisible 返回的可读结果。
     *
     * 返回值：
     *   true 表示 overlay socket 尚未就绪或切换失败，需要重试；false 表示本次恢复已完成。
     */
    function overlayRestoreNeedsRetry(result) {
        var text = String(result)
        if (text.length <= 0) {
            return true
        }
        return text.indexOf("失败") >= 0
            || text.indexOf("未连接") >= 0
            || text.indexOf("没有返回") >= 0
            || text.indexOf("failed") >= 0
            || text.indexOf("error") >= 0
    }

    /*
     * goodRateText 的作用：
     *   根据今日总数和良品数计算良率显示文本。
     *
     * 参数：
     *   无，直接读取 dailyTotal 和 dailyGood。
     *
     * 返回值：
     *   返回带百分号的良率字符串；总数为 0 时返回 0.00%。
     */
    function goodRateText() {
        if (dailyTotal <= 0) {
            return "0.00%"
        }
        return (dailyGood * 100.0 / dailyTotal).toFixed(2) + "%"
    }

    /*
     * setWorkflowState 的作用：
     *   响应底部操作按钮，切换界面上的流程状态演示值。
     *
     * 参数：
     *   text 是新的状态文本。
     *
     * 返回值：
     *   无返回值；函数会更新 workflowState。
     */
    function setWorkflowState(text) {
        workflowState = text
    }

    /*
     * handleControlAction 的作用：
     *   统一处理“开始、暂停、继续、停止”四个触摸按钮。
     *
     * 主要流程：
     *   1. 先更新右侧结果面板中的流程状态，让触摸操作有即时反馈。
     *   2. 如果当前是 Qt 自采集安全预览，则同步控制 V4L2VideoItem 的 running 状态。
     *   3. 如果当前是 KMS overlay 后端，则只更新流程状态，避免 QML 误杀外部 overlay 进程和自身 Qt 进程。
     *
     * 参数：
     *   action 是按钮动作标识，取值为 start、pause、resume 或 stop。
     *   stateText 是显示在界面上的流程状态文本。
     *
     * 返回值：
     *   无返回值；函数会更新 workflowState，并在 qt-safe 后端下控制 cameraView.running。
     */
    function handleControlAction(action, stateText) {
        workflowState = stateText

        if (root.usingKmsOverlay || root.usingGstVideo) {
            return
        }

        if (action === "pause" || action === "stop") {
            cameraView.running = false
        } else if (action === "start" || action === "resume") {
            cameraView.running = true
        }
    }

    /*
     * handleStorageAction 的作用：
     *   统一处理“安全卸载”等真实 SD 卡按钮。
     *
     * 主要流程：
     *   1. 首页已经不再提供独立保存图片，检测按钮会自动保存两种模型的结果图。
     *   2. 安全卸载仍调用同步脚本，但检测或旧保存自检进行中禁止安全卸载，避免写文件时卸载 SD 卡。
     *
     * 参数：
     *   action 是 safe-remove。
     *
     * 返回值：
     *   无返回值；函数会更新 storageState，并短暂显示底部提示条。
     */
    function handleStorageAction(action) {
        if (action === "safe-remove") {
            if (detectImageBusy || storageController.detectInProgress
                    || saveImageBusy || storageController.saveInProgress) {
                storageState = "检测图片写入中，暂不能安全卸载"
                showStorageToast()
                return
            }

            storageState = storageController.safeRemoveSdCard()
            showStorageToast()
            return
        }

        storageState = "未知SD卡操作"
        showStorageToast()
    }

    /*
     * handleDetectAction 的作用：
     *   处理首页“检测”按钮点击，异步请求 C++ 保存当前帧并串行调用分类和 UNet。
     *
     * 主要流程：
     *   1. 如果检测线程已经在运行，直接提示等待，避免重复启动多个 ONNX Runtime 进程。
     *   2. 设置检测中状态，让结果面板和按钮立即给出反馈。
     *   3. 调用 storageController.requestDetectCurrentFrame()，实际保存和推理都在 C++ 后台线程完成。
     *
     * 返回值：
     *   无返回值；检测完成后由 onDetectCurrentFrameFinished 更新界面。
     */
    function handleDetectAction() {
        if (detectImageBusy || storageController.detectInProgress) {
            storageState = "正在检测当前帧，请等待完成"
            showStorageToast()
            return
        }

        detectImageBusy = true
        detectStatus = "WAIT"
        detectState = "检测中..."
        detectPartName = "当前帧"
        detectClassName = "当前帧"
        detectConfidenceText = "--%"
        detectConfidencePercentText = "--%"
        detectConfidenceRatio = 0.0
        detectBadTotalText = "坏品 --%"
        detectGoodTotalText = "良品 --%"
        detectTimeText = "耗时 -- ms"
        storageState = "正在检测当前帧..."
        storageController.requestDetectCurrentFrame()
        showStorageToast()
    }

    /*
     * resultTokenValue 的作用：
     *   从 defect-classify 输出的 RESULT 行中提取 key=value 字段。
     *
     * 参数：
     *   text 是完整 RESULT 行。
     *   key 是字段名，例如 status、class、confidence。
     *
     * 返回值：
     *   找到时返回 value；找不到时返回空字符串。
     */
    function resultTokenValue(text, key) {
        if (!text || text.length <= 0) {
            return ""
        }

        var marker = key + "="
        var markerIndex = text.indexOf(marker)
        if (markerIndex < 0) {
            return ""
        }

        var valueStart = markerIndex + marker.length
        var valueEnd = valueStart
        while (valueEnd < text.length
               && text.charAt(valueEnd) !== " "
               && text.charAt(valueEnd) !== ";") {
            valueEnd += 1
        }

        return text.substring(valueStart, valueEnd)
    }

    /*
     * probabilityToText 的作用：
     *   把 0~1 概率转换成界面使用的百分制文本。
     *
     * 参数：
     *   valueText 是 RESULT 行里的小数文本。
     *   prefix 是可选中文前缀，例如“坏品”。
     *
     * 返回值：
     *   返回 “96%” 或 “坏品 96%”。
     */
    function probabilityToText(valueText, prefix) {
        var value = Number(valueText)
        if (isNaN(value)) {
            return prefix.length > 0 ? prefix + " --%" : "--%"
        }

        var score = Math.max(0, Math.min(100, Math.round(value * 100)))
        return prefix.length > 0 ? prefix + " " + score + "%" : score + "%"
    }

    /*
     * probabilityToRatio 的作用：
     *   把 RESULT 概率文本转换成 0~1 进度条比例。
     *
     * 参数：
     *   valueText 是 RESULT 行里的小数文本。
     *
     * 返回值：
     *   返回 0~1 数值；解析失败返回 0。
     */
    function probabilityToRatio(valueText) {
        var value = Number(valueText)
        if (isNaN(value)) {
            return 0.0
        }
        return Math.max(0.0, Math.min(1.0, value))
    }

    /*
     * formatDetectTime 的作用：
     *   把 defect-classify 输出的毫秒耗时整理成短文本。
     *
     * 参数：
     *   timeText 是 RESULT 行里的 time_ms 文本。
     *
     * 返回值：
     *   成功返回 “耗时 12 ms”；失败返回 “耗时 -- ms”。
     */
    function formatDetectTime(timeText) {
        var value = Number(timeText)
        if (isNaN(value)) {
            return "耗时 -- ms"
        }
        return "耗时 " + Math.round(value) + " ms"
    }

    /*
     * detectPartNameFromClass 的作用：
     *   从模型类别名提取零件名称，让首页零件栏跟随模型输出而不是固定显示演示零件。
     *
     * 主要流程：
     *   1. 空类别返回“未知零件”。
     *   2. 对 `gasket_good`、`washer_bad` 这类命名，去掉最后的 good/bad 后缀。
     *   3. 已知垫圈类编码转换成标准具体零件类型，避免把历史训练标签 `gasket` 显示成“垫片”。
     *   4. 其它类别名原样返回，避免未来新增类别时被错误截断。
     *
     * 参数：
     *   classText 是 RESULT 行里的 class 字段。
     *
     * 返回值：
     *   返回用于右侧结果面板展示的零件名称。
     */
    function detectPartNameFromClass(classText) {
        if (!classText || classText.length <= 0) {
            return "未知零件"
        }

        if (classText.length > 5 && classText.substring(classText.length - 5) === "_good") {
            classText = classText.substring(0, classText.length - 5)
        }

        if (classText.length > 4 && classText.substring(classText.length - 4) === "_bad") {
            classText = classText.substring(0, classText.length - 4)
        }

        if (classText === "gasket" || classText === "wave_washer") {
            return "波形垫圈"
        }
        if (classText === "washer") {
            return "平垫圈"
        }
        if (classText === "splitwasher") {
            return "弹性垫圈"
        }

        return classText
    }

    /*
     * handleDetectFailureText 的作用：
     *   把 C++ 返回的检测失败文本统一转换成首页错误状态。
     *
     * 主要流程：
     *   1. 主状态切换成 ERROR，让结果卡片使用故障色。
     *   2. 清空置信度、good/bad 总概率和耗时，避免保留上一次检测的数值误导现场。
     *
     * 参数：
     *   resultText 是 C++ 返回的“检测失败：...”文本。
     *
     * 返回值：
     *   无返回值；函数只更新 root 的检测属性。
     */
    function handleDetectFailureText(resultText) {
        detectStatus = "ERROR"
        detectState = resultText
        detectPartName = "检测失败"
        detectClassName = "检测失败"
        detectConfidenceText = "--%"
        detectConfidencePercentText = "--%"
        detectConfidenceRatio = 0.0
        detectBadTotalText = "坏品 --%"
        detectGoodTotalText = "良品 --%"
        detectTimeText = "耗时 -- ms"
    }

    /*
     * updateDetectClassificationFields 的作用：
     *   解析第一个分类模型 RESULT，并立即刷新零件类型、类别、GOOD/BAD 和置信度。
     *
     * 主要流程：
     *   1. 从 RESULT 行提取 status/class/confidence/bad_total/good_total。
     *   2. 根据 class 字段提取零件名称，让“零件”和“类别”在分类结束后马上显示。
     *   3. 不修改 detectTimeText，因为双模型总耗时必须等 UNet 完成后才准确。
     *
     * 参数：
     *   resultText 是 detectClassificationReady 或 detectModelsReady 携带的 RESULT 文本。
     *
     * 返回值：
     *   RESULT 格式正确返回 true；格式错误返回 false。
     */
    function updateDetectClassificationFields(resultText) {
        if (resultText.indexOf("RESULT ") !== 0) {
            return false
        }

        var statusText = resultTokenValue(resultText, "status")
        var classText = resultTokenValue(resultText, "class")
        var confidenceText = resultTokenValue(resultText, "confidence")
        var badTotalText = resultTokenValue(resultText, "bad_total")
        var goodTotalText = resultTokenValue(resultText, "good_total")

        detectStatus = statusText.length > 0 ? statusText : "WAIT"
        detectPartName = detectPartNameFromClass(classText)
        detectClassName = classText.length > 0 ? classText : "未知类别"
        detectConfidenceText = probabilityToText(confidenceText, "")
        detectConfidencePercentText = detectConfidenceText
        detectConfidenceRatio = probabilityToRatio(confidenceText)
        detectBadTotalText = probabilityToText(badTotalText, "坏品")
        detectGoodTotalText = probabilityToText(goodTotalText, "良品")
        detectState = detectStatus === "BAD" ? "模型判定坏品" : "模型判定良品"
        return true
    }

    /*
     * updateDetectFusedFields 的作用：
     *   在分类和 UNet 都完成后，用综合判定覆盖单个分类模型的 GOOD/BAD 状态。
     *
     * 主要流程：
     *   1. 从 RESULT 行读取 fused_status、fused_result 和 fused_reason。
     *   2. fused_status 存在时更新首页主状态，确保 UNet 检出缺陷时不会继续显示良品。
     *   3. 保留分类模型给出的零件名、类别和概率，便于现场看清两个模型各自证据。
     *
     * 参数：
     *   resultText 是 detectModelsReady 或最终 detectCurrentFrameFinished 携带的 RESULT 文本。
     *
     * 返回值：
     *   读取到 fused_status 时返回 true；旧结果没有该字段时返回 false。
     */
    function updateDetectFusedFields(resultText) {
        var fusedStatusText = resultTokenValue(resultText, "fused_status")
        var fusedReasonText = resultTokenValue(resultText, "fused_reason")

        if (fusedStatusText.length <= 0) {
            return false
        }

        detectStatus = fusedStatusText
        if (fusedStatusText === "BAD") {
            detectState = fusedReasonText.length > 0 ? "综合判定坏品：" + fusedReasonText : "综合判定坏品"
        } else if (fusedStatusText === "GOOD") {
            detectState = fusedReasonText.length > 0 ? "综合判定良品：" + fusedReasonText : "综合判定良品"
        } else {
            detectState = fusedReasonText.length > 0 ? "综合判定待复核：" + fusedReasonText : "综合判定待复核"
        }

        return true
    }

    /*
     * updateDetectModelTimeFields 的作用：
     *   解析双模型完成后的 RESULT，并刷新“耗时”字段。
     *
     * 主要流程：
     *   1. 优先读取 total_time_ms，代表分类 + UNet 两个模型串行总耗时。
     *   2. 如果旧结果没有 total_time_ms，再回退到分类 time_ms，兼容旧自检输出。
     *
     * 参数：
     *   resultText 是 detectModelsReady 或最终 detectCurrentFrameFinished 携带的 RESULT 文本。
     *
     * 返回值：
     *   无返回值；函数只更新 detectTimeText。
     */
    function updateDetectModelTimeFields(resultText) {
        var timeText = resultTokenValue(resultText, "total_time_ms")
        if (timeText.length <= 0) {
            timeText = resultTokenValue(resultText, "time_ms")
        }

        detectTimeText = formatDetectTime(timeText)
    }

    /*
     * handleDetectResultText 的作用：
     *   解析 C++ 返回的检测结果，并刷新首页结果面板。
     *
     * 主要流程：
     *   1. 失败文本以“检测失败”开头时进入 ERROR 状态。
     *   2. 成功 RESULT 行复用分类字段刷新逻辑。
     *   3. 如果结果已经包含 total_time_ms，则刷新双模型耗时。
     *
     * 参数：
     *   resultText 是 detectCurrentFrameFinished 信号传来的文本。
     *
     * 返回值：
     *   无返回值；函数只更新 root 的检测属性。
     */
    function handleDetectResultText(resultText) {
        if (!updateDetectClassificationFields(resultText)) {
            handleDetectFailureText(resultText)
            return
        }

        updateDetectFusedFields(resultText)
        updateDetectModelTimeFields(resultText)
    }

    /*
     * showStorageToast 的作用：
     *   显示底部 SD 卡操作提示，并重新启动自动隐藏倒计时。
     *
     * 主要流程：
     *   1. 先把 storageToastVisible 置为 true，让底部提示条出现。
     *   2. 重启 storageToastTimer，保证连续点击时每次都从头计时。
     *
     * 返回值：
     *   无返回值；函数只改变界面提示状态。
     */
    function showStorageToast() {
        storageToastVisible = true
        storageToastTimer.stop()
        storageToastTimer.start()
    }

    /*
     * appendManualCommandLog 的作用：
     *   把手动控制页的每一次操作写入最新在前的命令日志，方便现场联调时追溯点击顺序和禁止原因。
     *
     * 主要流程：
     *   1. 使用当前板端时间生成 hh:mm:ss 文本，避免日志行过长。
     *   2. 把命令名称、目标对象和结果插入 ListModel 首行。
     *   3. 日志最多保留 8 条，避免 1024x600 屏幕上的列表无限增长影响性能和可读性。
     *
     * 参数：
     *   commandText 是按钮或动作名称。
     *   targetText 是命令面向的设备或安全域。
     *   resultText 是执行结果、模拟 ACK 或禁止原因。
     *
     * 返回值：
     *   无返回值；函数只更新 manualCommandLog 模型。
     */
    function appendManualCommandLog(commandText, targetText, resultText) {
        manualCommandLog.insert(0, {
            "time": Qt.formatDateTime(new Date(), "hh:mm:ss"),
            "command": commandText,
            "target": targetText,
            "result": resultText
        })

        while (manualCommandLog.count > 8) {
            manualCommandLog.remove(manualCommandLog.count - 1)
        }
    }

    /*
     * manualActionAllowed 的作用：
     *   集中判断某个手动动作当前是否允许执行，让按钮置灰条件和点击保护分支使用同一套安全规则。
     *
     * 主要流程：
     *   1. 检测当前帧、刷新状态、进入手动、停止和清故障属于安全动作，可以在未进入手动模式时执行。
     *   2. 其它运动或执行器动作必须先进入手动模式。
     *   3. 急停状态下只允许停止、刷新状态和清故障。
     *   4. 抓取和放置类动作要求机械臂已经回零。
     *
     * 参数：
     *   action 是手动控制动作标识。
     *
     * 返回值：
     *   返回 true 表示按钮可以执行；返回 false 表示界面应置灰或点击时给出禁止原因。
     */
    function manualActionAllowed(action) {
        if (action === "enter-manual" || action === "stop" || action === "detect-frame"
                || action === "refresh" || action === "clear-alarm" || action === "emergency-toggle") {
            return true
        }

        if (!manualMode) {
            return false
        }

        if (manualEmergencyStop) {
            return false
        }

        if ((action === "arm-pick" || action === "arm-good" || action === "arm-bad") && !manualArmHomeOk) {
            return false
        }

        return true
    }

    /*
     * handleManualAction 的作用：
     *   统一处理手动控制页所有按钮点击；第一版只更新 QML 模拟状态、日志和提示，不直接控制真实硬件。
     *
     * 主要流程：
     *   1. 先执行手动模式、急停和回零等安全保护判断。
     *   2. 检测当前帧动作复用首页双模型检测链路，避免只保存无检测结果的图片。
     *   3. 其它动作只改变模拟状态和命令日志，后续会在这里替换为 motionController.sendManualCommand(action)。
     *
     * 参数：
     *   action 是动作标识，例如 belt-forward、arm-home、detect-frame。
     *   label 是界面显示的按钮文字，用于日志和提示。
     *
     * 返回值：
     *   无返回值；函数会更新手动页状态、storageState 和命令日志。
     */
    function handleManualAction(action, label) {
        if (!manualMode && action !== "enter-manual" && action !== "stop"
                && action !== "detect-frame" && action !== "refresh"
                && action !== "clear-alarm" && action !== "emergency-toggle") {
            manualLastAckText = "请先进入手动模式"
            storageState = manualLastAckText
            appendManualCommandLog(label, "安全联锁", manualLastAckText)
            showStorageToast()
            return
        }

        if (manualEmergencyStop && action !== "stop" && action !== "refresh"
                && action !== "clear-alarm" && action !== "emergency-toggle") {
            manualLastAckText = "急停中，禁止执行运动命令"
            storageState = manualLastAckText
            appendManualCommandLog(label, "急停保护", manualLastAckText)
            showStorageToast()
            return
        }

        if ((action === "arm-pick" || action === "arm-good" || action === "arm-bad") && !manualArmHomeOk) {
            manualLastAckText = "机械臂未回零，禁止抓取和放置"
            storageState = manualLastAckText
            appendManualCommandLog(label, "回零联锁", manualLastAckText)
            showStorageToast()
            return
        }

        if (action === "detect-frame") {
            handleDetectAction()
            manualLastAckText = "已请求双模型检测当前帧"
            appendManualCommandLog(label, "检测辅助", manualLastAckText)
            return
        }

        if (action === "enter-manual") {
            manualMode = !manualMode
            manualLastAckText = manualMode ? "已进入手动模式" : "已返回自动模式"
            if (!manualMode) {
                manualBeltState = "停止"
                manualArmState = "待机"
                manualActuatorState = "夹爪关闭"
            }
            storageState = manualLastAckText
            appendManualCommandLog(label, "模式切换", manualLastAckText)
            showStorageToast()
            return
        }

        if (action === "belt-forward") {
            manualBeltState = "正向点动"
            manualLastAckText = "模拟ACK：传送带正向点动 " + manualBeltSpeed
        } else if (action === "belt-reverse") {
            manualBeltState = "反向点动"
            manualLastAckText = "模拟ACK：传送带反向点动 " + manualBeltSpeed
        } else if (action === "belt-speed-low") {
            manualBeltSpeed = "低速"
            manualLastAckText = "模拟ACK：速度档位低速"
        } else if (action === "belt-speed-mid") {
            manualBeltSpeed = "中速"
            manualLastAckText = "模拟ACK：速度档位中速"
        } else if (action === "belt-speed-high") {
            manualBeltSpeed = "高速"
            manualLastAckText = "模拟ACK：速度档位高速"
        } else if (action === "stop") {
            manualBeltState = "停止"
            manualArmState = "待机"
            manualActuatorState = "夹爪关闭"
            manualLastAckText = "模拟ACK：全部手动动作停止"
        } else if (action === "arm-home") {
            manualArmHomeOk = true
            manualArmState = "已回零"
            manualLastAckText = "模拟ACK：机械臂回零完成"
        } else if (action === "arm-standby") {
            manualArmState = "待机位"
            manualLastAckText = "模拟ACK：机械臂到待机位"
        } else if (action === "arm-pick") {
            manualArmState = "抓取测试"
            manualActuatorState = "夹爪闭合"
            manualLastAckText = "模拟ACK：抓取测试完成"
        } else if (action === "arm-good") {
            manualArmState = "放良品位"
            manualActuatorState = "夹爪打开"
            manualLastAckText = "模拟ACK：已放置到良品位"
        } else if (action === "arm-bad") {
            manualArmState = "放坏品位"
            manualActuatorState = "夹爪打开"
            manualLastAckText = "模拟ACK：已放置到坏品位"
        } else if (action === "actuator-on") {
            manualActuatorState = "夹爪闭合"
            manualLastAckText = "模拟ACK：夹爪已闭合"
        } else if (action === "actuator-off") {
            manualActuatorState = "夹爪打开"
            manualLastAckText = "模拟ACK：夹爪已打开"
        } else if (action === "toplight-toggle") {
            manualTopLightEnabled = !manualTopLightEnabled
            manualLightState = manualTopLightEnabled ? "背光常亮 + 补光开启" : "背光常亮"
            manualLastAckText = manualTopLightEnabled ? "模拟ACK：补光已开启" : "模拟ACK：补光已关闭"
        } else if (action === "light-low") {
            manualLightLevel = 1
            manualLastAckText = "模拟ACK：亮度 1 档"
        } else if (action === "light-mid") {
            manualLightLevel = 2
            manualLastAckText = "模拟ACK：亮度 2 档"
        } else if (action === "light-high") {
            manualLightLevel = 3
            manualLastAckText = "模拟ACK：亮度 3 档"
        } else if (action === "mark-good") {
            manualReviewMark = "GOOD"
            manualLastAckText = "人工标记：GOOD"
        } else if (action === "mark-bad") {
            manualReviewMark = "BAD"
            manualLastAckText = "人工标记：BAD"
        } else if (action === "mark-uncertain") {
            manualReviewMark = "UNCERTAIN"
            manualLastAckText = "人工标记：UNCERTAIN"
        } else if (action === "emergency-toggle") {
            manualEmergencyStop = !manualEmergencyStop
            if (manualEmergencyStop) {
                manualBeltState = "停止"
                manualArmState = "急停保持"
                manualActuatorState = "夹爪关闭"
            }
            manualLastAckText = manualEmergencyStop ? "急停已按下，运动禁止" : "急停已释放，等待清故障"
        } else if (action === "clear-alarm") {
            manualEmergencyStop = false
            manualLastAckText = "模拟ACK：故障已清除"
        } else if (action === "refresh") {
            manualLastAckText = "模拟状态刷新完成"
        } else {
            manualLastAckText = "未知手动动作"
        }

        storageState = manualLastAckText
        appendManualCommandLog(label, "手动控制", manualLastAckText)
        showStorageToast()
    }

    /*
     * settingsThresholdText 的作用：
     *   把千分比阈值转换成界面上更直观的百分比文本。
     *
     * 参数：
     *   value 是 0~1000 范围内的定点阈值。
     *
     * 返回值：
     *   返回一位小数百分比，便于现场人员理解当前判定灵敏度。
     */
    function settingsThresholdText(value) {
        return (value / 10.0).toFixed(1) + "%"
    }

    /*
     * settingsSummaryText 的作用：
     *   生成参数设置页右侧摘要，让操作员应用参数前能快速确认关键值。
     *
     * 返回值：
     *   返回包含零件类型、判定阈值、居中阈值和上传策略的短文本。
     */
    function settingsSummaryText() {
        return settingsPartType
                + "  判定" + settingsThresholdText(settingsDecisionThreshold)
                + "  定位" + settingsFineCenterPx + "px"
                + "  上传" + (settingsAutoUpload ? "自动" : "手动")
    }

    /*
     * changeSettingValue 的作用：
     *   统一处理参数页的加减按钮，保证每个参数都按固定步长变化并被限制在安全范围内。
     *
     * 参数：
     *   key 是参数名，delta 是本次变化量。
     *
     * 返回值：
     *   无返回值；函数会更新对应 QML 参数和最近操作提示。
     */
    function changeSettingValue(key, delta) {
        if (key === "decision") {
            settingsDecisionThreshold = Math.max(500, Math.min(990, settingsDecisionThreshold + delta))
            settingsLastActionText = "已调整良坏阈值：" + settingsThresholdText(settingsDecisionThreshold)
        } else if (key === "review") {
            settingsReviewThreshold = Math.max(300, Math.min(settingsDecisionThreshold - 50, settingsReviewThreshold + delta))
            settingsLastActionText = "已调整复核阈值：" + settingsThresholdText(settingsReviewThreshold)
        } else if (key === "fine") {
            settingsFineCenterPx = Math.max(3, Math.min(30, settingsFineCenterPx + delta))
            settingsLastActionText = "已调整精定位阈值：" + settingsFineCenterPx + " px"
        } else if (key === "stable") {
            settingsStableFrames = Math.max(1, Math.min(10, settingsStableFrames + delta))
            settingsLastActionText = "已调整稳定帧数：" + settingsStableFrames + " 帧"
        } else if (key === "pulse") {
            settingsPulsePerPx = Math.max(1, Math.min(80, settingsPulsePerPx + delta))
            settingsLastActionText = "已调整标定比例：" + settingsPulsePerPx + " pulse/px"
        } else if (key === "belt") {
            settingsBeltSpeed = Math.max(10, Math.min(120, settingsBeltSpeed + delta))
            settingsLastActionText = "已调整低速档：" + settingsBeltSpeed + " mm/s"
        } else if (key === "timeout") {
            settingsSortTimeoutMs = Math.max(500, Math.min(5000, settingsSortTimeoutMs + delta))
            settingsLastActionText = "已调整分拣超时：" + settingsSortTimeoutMs + " ms"
        }

        storageState = settingsLastActionText
        showStorageToast()
    }

    /*
     * settingsApplyAction 的作用：
     *   统一处理参数页“应用、保存、恢复默认、导出摘要”等操作。
     *
     * 主要流程：
     *   1. 应用参数只更新界面状态，后续接入时再通过 CMD_PARAM_SYNC 下发给 F4。
     *   2. 保存配置第一版只给出路径提示，真实持久化后续放到 C++ 控制器。
     *   3. 恢复默认会把关键参数回到比赛演示推荐值。
     *
     * 参数：
     *   action 是 apply、save、reset 或 export。
     *
     * 返回值：
     *   无返回值；函数会更新 settingsLastActionText 和底部提示条。
     */
    function settingsApplyAction(action) {
        if (action === "apply") {
            settingsLastActionText = "已应用，待 F4 同步"
        } else if (action === "save") {
            settingsLastActionText = "保存目标：/mnt/sdcard/config/defect_ui_config.json"
        } else if (action === "reset") {
            settingsPartType = "平垫圈A"
            settingsDecisionThreshold = 850
            settingsReviewThreshold = 650
            settingsFineCenterPx = 8
            settingsStableFrames = 4
            settingsPulsePerPx = 12
            settingsBeltSpeed = 40
            settingsSortTimeoutMs = 1500
            settingsAutoUpload = true
            settingsLastActionText = "已恢复比赛演示默认参数"
        } else if (action === "export") {
            settingsLastActionText = "诊断摘要：" + settingsSummaryText()
        } else if (action === "upload-toggle") {
            settingsAutoUpload = !settingsAutoUpload
            settingsLastActionText = settingsAutoUpload ? "已切换为自动上传" : "已切换为手动上传"
        } else if (action === "part-next") {
            if (settingsPartType === "平垫圈A") {
                settingsPartType = "异形垫片B"
            } else if (settingsPartType === "异形垫片B") {
                settingsPartType = "冲压片C"
            } else {
                settingsPartType = "平垫圈A"
            }
            settingsLastActionText = "已切换零件类型：" + settingsPartType
        }

        storageState = settingsLastActionText
        showStorageToast()
    }

    /*
     * alarmLevelColor 的作用：
     *   根据告警等级和清除状态返回统一状态色，避免只靠文字判断告警状态。
     *
     * 返回值：
     *   已清除返回绿色，严重返回红色，预警返回黄色，其它返回蓝色。
     */
    function alarmLevelColor() {
        if (alarmCleared) {
            return accentGreen
        }
        if (alarmCurrentLevel === "严重") {
            return accentRed
        }
        if (alarmCurrentLevel === "预警") {
            return accentAmber
        }
        return "#5aa7ff"
    }

    /*
     * alarmCurrentStatusText 的作用：
     *   把当前告警确认/清除状态组合成短文本，用于页头和当前告警卡片显示。
     *
     * 返回值：
     *   返回“已清除/已确认/待确认”三类状态。
     */
    function alarmCurrentStatusText() {
        if (alarmCleared) {
            return "已清除"
        }
        if (alarmAcknowledged) {
            return "已确认，等待复位"
        }
        return "待确认"
    }

    /*
     * alarmSnapshotText 的作用：
     *   组装保存到 SD 卡的告警诊断文本，让 SSH 打开快照时能直接看到关键状态。
     *
     * 主要流程：
     *   1. 写入保存时间、当前告警码、等级、确认/清除状态。
     *   2. 写入相机、KMS、SD 卡、云端和手动控制等跨页面状态。
     *   3. 写入最近告警历史，便于现场复盘按钮点击前后的处理轨迹。
     *
     * 返回值：
     *   返回多行 UTF-8 文本，由 C++ 控制器落盘到 /mnt/sdcard/logs/qt_alarm_snapshot.txt。
     */
    function alarmSnapshotText() {
        var summary = statsSummary()
        var lines = [
            "STM32MP157 Qt Alarm Snapshot",
            "snapshot_time=" + Qt.formatDateTime(new Date(), "yyyy-MM-dd hh:mm:ss"),
            "alarm_code=" + alarmCurrentCode,
            "alarm_title=" + alarmCurrentTitle,
            "alarm_level=" + alarmCurrentLevel,
            "alarm_started_at=" + alarmCurrentTime,
            "alarm_status=" + alarmCurrentStatusText(),
            "alarm_acknowledged=" + (alarmAcknowledged ? "true" : "false"),
            "alarm_cleared=" + (alarmCleared ? "true" : "false"),
            "camera_status=" + cameraStatusText(),
            "video_backend=" + videoBackend,
            "kms_overlay=" + (usingKmsOverlay ? "true" : "false"),
            "manual_mode=" + (manualMode ? "true" : "false"),
            "manual_emergency_stop=" + (manualEmergencyStop ? "true" : "false"),
            "manual_arm_home_ok=" + (manualArmHomeOk ? "true" : "false"),
            "storage_state=" + storageState,
            "settings_summary=" + settingsSummaryText(),
            "upload_total=" + summary.total,
            "upload_success=" + summary.uploadSuccess,
            "upload_failed=" + summary.uploadFailed,
            "latest_upload_time=" + summary.latestTime,
            "latest_upload_status=" + summary.latestStatus,
            "",
            "[recent_alarm_history]"
        ]

        for (var i = 0; i < alarmHistoryModel.count; ++i) {
            var entry = alarmHistoryModel.get(i)
            lines.push(entry.time + " " + entry.code + " " + entry.level
                       + " " + entry.title + " " + entry.status)
        }

        return lines.join("\n") + "\n"
    }

    /*
     * handleAlarmAction 的作用：
     *   统一处理告警维护页的确认、清故障、刷新和保存诊断操作。
     *
     * 主要流程：
     *   1. 确认告警只改变操作员确认状态，不代表 F4 已释放联锁。
     *   2. 清故障会同步释放手动页模拟急停状态，保持两个页面状态一致。
     *   3. 保存诊断调用 C++ 控制器写入 /mnt/sdcard/logs/qt_alarm_snapshot.txt。
     *
     * 参数：
     *   action 是 ack、clear、refresh 或 snapshot。
     *
     * 返回值：
     *   无返回值；函数会更新告警状态、历史模型和底部提示条。
     */
    function handleAlarmAction(action) {
        var resultText = ""

        if (action === "ack") {
            alarmAcknowledged = true
            resultText = "已确认告警：" + alarmCurrentCode
        } else if (action === "clear") {
            alarmAcknowledged = true
            alarmCleared = true
            manualEmergencyStop = false
            resultText = "清故障：等待 F4 复核"
        } else if (action === "refresh") {
            deviceHealth.refreshAllStatus()
            resultText = "已刷新设备健康状态"
        } else if (action === "snapshot") {
            resultText = storageController.saveAlarmSnapshotToSdCard(alarmSnapshotText())
        } else {
            resultText = "未知告警维护动作"
        }

        alarmHistoryModel.insert(0, {
            "time": Qt.formatDateTime(new Date(), "hh:mm:ss"),
            "code": alarmCurrentCode,
            "level": alarmCleared ? "记录" : alarmCurrentLevel,
            "title": resultText,
            "status": alarmCurrentStatusText()
        })

        while (alarmHistoryModel.count > 8) {
            alarmHistoryModel.remove(alarmHistoryModel.count - 1)
        }

        storageState = resultText
        showStorageToast()
    }

    /*
     * switchPage 的作用：
     *   处理左侧导航页面切换，并在非首页功能页打开时隐藏 KMS 实时视频层。
     *
     * 主要流程：
     *   1. 更新 activePage，让对应页面的 QML 内容显示。
     *   2. 每次进入历史页时，如果已有检测记录，停留在检测历史列表并聚焦最新一条卡片。
     *   3. KMS overlay 模式下，非首页需要隐藏视频 plane，返回首页时也必须等 Qt 启动遮罩结束后再恢复。
     *
     * 参数：
     *   pageName 是目标页面名称，目前支持 home、history、stats、manual、settings 和 alarm。
     *
     * 返回值：
     *   无返回值；函数负责更新页面状态和 overlay 可见性。
     */
    function switchPage(pageName) {
        if (pageName !== "home" && pageName !== "history" && pageName !== "stats"
                && pageName !== "manual" && pageName !== "settings" && pageName !== "alarm") {
            return
        }

        activePage = pageName

        if (pageName === "history") {
            focusLatestHistoryListRecord()
        } else {
            historyDetailVisible = false
            historyAnalysisDetailVisible = false
            selectedHistoryRecord = uploadHistory.entryAt(selectedHistoryIndex)
        }

        if (root.usingKmsOverlay) {
            storageController.setOverlayVisible(pageName === "home"
                                                && root.bootOverlayRestoreFinished
                                                && !root.splashOverlayVisible)
        }
    }

    /*
     * positionHistoryListAtSelected 的作用：
     *   把横向历史列表滚动到当前选中的检测卡片。
     *
     * 主要流程：
     *   1. 检查 historyListView 是否已经创建，避免页面初始化阶段访问空对象。
     *   2. 使用 Qt.callLater 延后滚动，让 ListView 在 visible/model 更新后再计算位置。
     *   3. 使用 ListView.Contain，只保证最新卡片进入可视区域，不强行居中造成跳动。
     *
     * 返回值：
     *   无返回值；函数只影响历史列表的滚动位置。
     */
    function positionHistoryListAtSelected() {
        if (selectedHistoryIndex < 0 || selectedHistoryIndex >= uploadHistory.count) {
            return
        }

        if (!historyListView) {
            return
        }

        Qt.callLater(function() {
            if (selectedHistoryIndex >= 0 && selectedHistoryIndex < uploadHistory.count) {
                historyListView.positionViewAtIndex(selectedHistoryIndex, ListView.Contain)
            }
        })
    }

    /*
     * focusLatestHistoryListRecord 的作用：
     *   进入历史记录页时停留在检测历史列表，并自动选中最新一条检测卡片。
     *
     * 主要流程：
     *   1. 没有历史记录时清空选中索引，并保持列表空状态。
     *   2. 有历史记录时选中最后一条，因为 UploadHistoryModel 追加顺序是旧在前、新在后。
     *   3. 关闭详情层并滚动横向列表，让最新卡片像现场标注那样直接出现在可视区域。
     *
     * 返回值：
     *   无返回值；函数只更新历史页列表层状态。
     */
    function focusLatestHistoryListRecord() {
        if (uploadHistory.count <= 0) {
            selectedHistoryIndex = -1
            selectedHistoryImageIndex = 0
            selectedHistoryRecord = uploadHistory.entryAt(selectedHistoryIndex)
            historyDetailVisible = false
            historyAnalysisDetailVisible = false
            return
        }

        selectedHistoryIndex = uploadHistory.count - 1
        selectedHistoryImageIndex = 0
        selectedHistoryRecord = uploadHistory.entryAt(selectedHistoryIndex)
        historyDetailVisible = false
        historyAnalysisDetailVisible = false
        positionHistoryListAtSelected()
    }

    /*
     * showHistoryDetail 的作用：
     *   响应历史卡片中的“查看”按钮，切换详情页选中记录。
     *
     * 参数：
     *   index 是用户点击的历史记录索引。
     *
     * 返回值：
     *   无返回值；函数会刷新详情记录和图片轮播页。
     */
    function showHistoryDetail(index) {
        if (index < 0 || index >= uploadHistory.count) {
            return
        }

        selectedHistoryIndex = index
        selectedHistoryImageIndex = 0
        selectedHistoryRecord = uploadHistory.entryAt(selectedHistoryIndex)
        historyDetailVisible = true
        historyAnalysisDetailVisible = false
    }

    /*
     * deleteHistoryRecord 的作用：
     *   响应历史卡片中的“删除”按钮，删除这条历史记录和它对应的 source/annotated 图片文件。
     *
     * 主要流程：
     *   1. 先校验 index，避免列表滑动时误传已经失效的记录编号。
     *   2. 调用 C++ 模型 removeRecord()，由 C++ 负责写回 JSON 并删除实体图片。
     *   3. 删除成功后选中同位置的新记录；如果删的是最后一条，就选中新的最后一条。
     *   4. 删除到空列表时关闭详情页，并把选中索引恢复为 -1。
     *
     * 参数：
     *   index 是用户点击删除按钮时对应的历史记录索引。
     *
     * 返回值：
     *   无返回值；函数会刷新历史页状态，并用底部提示条显示删除结果。
     */
    function deleteHistoryRecord(index) {
        if (index < 0 || index >= uploadHistory.count) {
            storageState = "删除失败：记录不存在"
            showStorageToast()
            return
        }

        if (storageController.retryUploadInProgress) {
            storageState = "重新发送中，暂不能删除历史记录"
            showStorageToast()
            return
        }

        var nextIndex = index
        var resultText = uploadHistory.removeRecord(index)
        storageState = resultText

        if (resultText.indexOf("删除失败") === 0) {
            if (uploadHistory.count > 0) {
                selectedHistoryIndex = Math.min(index, uploadHistory.count - 1)
                selectedHistoryImageIndex = 0
                selectedHistoryRecord = uploadHistory.entryAt(selectedHistoryIndex)
            }
            showStorageToast()
            return
        }

        if (uploadHistory.count <= 0) {
            selectedHistoryIndex = -1
            selectedHistoryImageIndex = 0
            selectedHistoryRecord = uploadHistory.entryAt(selectedHistoryIndex)
            historyDetailVisible = false
            showStorageToast()
            return
        }

        if (nextIndex >= uploadHistory.count) {
            nextIndex = uploadHistory.count - 1
        }

        selectedHistoryIndex = nextIndex
        selectedHistoryImageIndex = 0
        selectedHistoryRecord = uploadHistory.entryAt(selectedHistoryIndex)
        positionHistoryListAtSelected()
        showStorageToast()
    }

    /*
     * retryUploadHistoryRecord 的作用：
     *   响应历史详情页的“重新发送”按钮，把上传失败记录的本地图片重新提交到云端。
     *
     * 主要流程：
     *   1. 校验 index，避免删除或切换后重发不存在的记录。
     *   2. 检查后台重发忙状态，防止用户连续点击造成多条重复云端记录。
     *   3. 调用 C++ 控制器在后台执行 defect-cos-upload；结果由 retryUploadFinished 信号回填。
     *
     * 参数：
     *   index 是当前详情页对应的历史记录索引。
     *
     * 返回值：
     *   无返回值；函数通过 storageState 和底部提示条反馈启动状态。
     */
    function retryUploadHistoryRecord(index) {
        if (index < 0 || index >= uploadHistory.count) {
            storageState = "重新发送失败：记录不存在"
            showStorageToast()
            return
        }

        if (storageController.retryUploadInProgress) {
            storageState = "重新发送中，请等待完成"
            showStorageToast()
            return
        }

        storageState = "正在重新发送历史图片..."
        showStorageToast()
        storageController.retryUploadRecord(index)
    }

    /*
     * backToHistoryList 的作用：
     *   从某条检测记录详情页返回历史列表页。
     *
     * 主要流程：
     *   只关闭 historyDetailVisible，不改变 selectedHistoryIndex，这样用户回到列表时仍能看到刚才查看的卡片高亮。
     *
     * 返回值：
     *   无返回值；函数只改变历史页内部层级状态。
     */
    function backToHistoryList() {
        historyDetailVisible = false
        historyAnalysisDetailVisible = false
        positionHistoryListAtSelected()
    }

    /*
     * historyImages 的作用：
     *   读取当前历史记录中的图片数组，并保证空记录时返回空数组。
     *
     * 返回值：
     *   返回形如 [{label, path, sizeBytes}, ...] 的数组。
     */
    function historyImages() {
        if (!selectedHistoryRecord || !selectedHistoryRecord.images) {
            return []
        }

        return selectedHistoryRecord.images
    }

    /*
     * currentHistoryImageText 的作用：
     *   生成详情页左侧图片页码和类型文字。
     *
     * 返回值：
     *   无图片时返回“暂无图片”；有图片时返回“原始图片 1/2”这类文本。
     */
    function currentHistoryImageText() {
        var images = historyImages()

        if (images.length <= 0) {
            return "暂无图片"
        }

        var index = selectedHistoryImageIndex
        if (index < 0 || index >= images.length) {
            index = 0
        }

        return images[index].label + "  " + (index + 1) + "/" + images.length
    }

    /*
     * historyDatePart 的作用：
     *   把上传时间拆成日期，给第一层历史卡片大字展示。
     *
     * 参数：
     *   timestamp 是 UploadHistoryModel 提供的 uploadTime，例如 2026-05-08 19:06:59。
     *
     * 返回值：
     *   有空格时返回日期部分；异常或空值时返回“未知日期”。
     */
    function historyDatePart(timestamp) {
        if (!timestamp || timestamp.length <= 0) {
            return "未知日期"
        }

        var parts = timestamp.split(" ")
        return parts.length > 0 ? parts[0] : timestamp
    }

    /*
     * historyTimePart 的作用：
     *   把上传时间拆成时分秒，避免第一层卡片把完整时间挤成省略号。
     *
     * 参数：
     *   timestamp 是 UploadHistoryModel 提供的 uploadTime。
     *
     * 返回值：
     *   有时分秒时返回时间部分；缺失时返回空字符串。
     */
    function historyTimePart(timestamp) {
        if (!timestamp || timestamp.length <= 0) {
            return ""
        }

        var parts = timestamp.split(" ")
        return parts.length > 1 ? parts[1] : ""
    }

    /*
     * tokenValue 的作用：
     *   从脚本文本中提取 key=value 短字段，兼容旧历史记录中的原始 upload_status。
     *
     * 参数：
     *   text 是上传状态文本。
     *   key 是 record_id、record_no 这类字段名。
     *
     * 返回值：
     *   找到字段时返回字段值；找不到时返回空字符串。
     */
    function tokenValue(text, key) {
        if (!text || text.length <= 0) {
            return ""
        }

        var pattern = new RegExp(key + "=([^\\s;]+)")
        var match = text.match(pattern)
        return match && match.length > 1 ? match[1] : ""
    }

    /*
     * cloudStatusSummary 的作用：
     *   把 upload_status 原始文本转换成适合 1024x600 屏幕展示的短摘要。
     *
     * 主要流程：
     *   1. 优先从文本中提取 record_id 和 record_no。
     *   2. 含“失败”时显示失败摘要。
     *   3. 含 record 字段时认为云端登记成功，旧乱码记录也能显示干净信息。
     *
     * 参数：
     *   rawStatus 是历史 JSON 中保存的上传状态。
     *
     * 返回值：
     *   返回短中文状态，不直接显示脚本原始长输出。
     */
    function cloudStatusSummary(rawStatus) {
        var recordId = tokenValue(rawStatus, "record_id")
        var recordNo = tokenValue(rawStatus, "record_no")

        if (rawStatus && rawStatus.indexOf("失败") >= 0) {
            return "上传失败，请查看日志"
        }

        if (recordId.length > 0 || recordNo.length > 0) {
            var summary = "上传成功"
            if (recordId.length > 0) {
                summary += "  ID " + recordId
            }
            if (recordNo.length > 0) {
                summary += "  " + recordNo
            }
            return summary
        }

        if (rawStatus && rawStatus.indexOf("成功") >= 0) {
            return "上传成功"
        }

        return rawStatus && rawStatus.length > 0 ? rawStatus : "未返回云端状态"
    }

    /*
     * historyCloudRecordNoText 的作用：
     *   生成历史详情页显示的云端记录编号，避免把空 record_no 直接显示成空白。
     *
     * 参数：
     *   record 是 UploadHistoryModel 提供的当前历史记录。
     *
     * 返回值：
     *   有云端编号时返回编号；只有本地记录时返回“未返回”。
     */
    function historyCloudRecordNoText(record) {
        if (record && record.recordNo && record.recordNo.length > 0) {
            return record.recordNo
        }

        return "未返回"
    }

    /*
     * compactHomeClassText 的作用：
     *   把模型输出的完整类别名压缩成首页右侧窄面板可读的短类别。
     *
     * 主要流程：
     *   1. 空类别和占位类别原样返回，避免检测前显示异常。
     *   2. 删除类别名前缀中的 good/bad 后缀，只保留零件族和良坏方向。
     *   3. 常见垫圈类别转换成标准具体零件类型，让 182px 结果面板也能完整显示。
     *
     * 参数：
     *   classText 是分类模型输出的类别名，例如 splitwasher_good。
     *
     * 返回值：
     *   返回适合首页显示的短类别文本。
     */
    function compactHomeClassText(classText) {
        var rawText = classText ? String(classText) : ""
        if (rawText.length <= 0 || rawText === "未检测" || rawText === "检测失败" || rawText === "未知类别") {
            return rawText.length > 0 ? rawText : "未检测"
        }

        var isGood = rawText.indexOf("_good") >= 0 || rawText.indexOf("good") >= 0
        var isBad = rawText.indexOf("_bad") >= 0 || rawText.indexOf("bad") >= 0
        var baseText = rawText.replace(/_good/g, "").replace(/_bad/g, "")

        if (baseText === "splitwasher") {
            baseText = "弹性垫圈"
        } else if (baseText === "washer") {
            baseText = "平垫圈"
        } else if (baseText === "gasket") {
            baseText = "波形垫圈"
        } else if (baseText === "wave_washer") {
            baseText = "波形垫圈"
        }

        if (isGood) {
            return baseText + "-良"
        }
        if (isBad) {
            return baseText + "-坏"
        }

        return baseText
    }

    /*
     * compactHomeModelText 的作用：
     *   把首页模型结果长句压缩成“分类/UNet/综合”短结论，避免右侧面板显示不全。
     *
     * 主要流程：
     *   1. 检测中、等待检测等短状态直接显示。
     *   2. 分类初判阶段显示“分类良/分类坏”，提醒还在等 UNet。
     *   3. 综合阶段显示“综合良/综合坏/待复核”，详细原因保留在历史详情里。
     *
     * 参数：
     *   stateText 是 detectState 保存的当前检测状态。
     *
     * 返回值：
     *   返回适合首页窄栏显示的模型结果短文本。
     */
    function compactHomeModelText(stateText) {
        var rawText = stateText ? String(stateText) : ""
        if (rawText.length <= 0) {
            return "等待检测"
        }
        if (rawText === "等待检测" || rawText === "检测中..." || rawText === "等待综合判定") {
            return rawText
        }
        if (rawText.indexOf("分类模型判定坏品") >= 0 || rawText.indexOf("模型判定坏品") >= 0) {
            return "分类坏，等UNet"
        }
        if (rawText.indexOf("分类模型判定良品") >= 0 || rawText.indexOf("模型判定良品") >= 0) {
            return "分类良，等UNet"
        }
        if (rawText.indexOf("综合判定坏品") >= 0) {
            return "综合坏品"
        }
        if (rawText.indexOf("综合判定良品") >= 0) {
            return "综合良品"
        }
        if (rawText.indexOf("综合判定待复核") >= 0 || rawText.indexOf("待复核") >= 0) {
            return "综合待复核"
        }
        if (rawText.length > 8) {
            return rawText.substring(0, 8)
        }

        return rawText
    }

    /*
     * historyConfidenceSummaryText 的作用：
     *   把分类模型 confidence 原始小数转换成普通人能看懂的可信度说明。
     *
     * 主要流程：
     *   1. 从 classificationResult 中提取 confidence 和 status。
     *   2. 把 0~1 的小数转换成百分比。
     *   3. 根据 GOOD/BAD 状态解释“分类模型更倾向于良品还是坏品”。
     *
     * 参数：
     *   record 是当前历史记录，里面可能包含 classificationResult。
     *
     * 返回值：
     *   返回一段中文短句；旧记录没有模型结果时返回缺省说明。
     */
    function historyConfidenceSummaryText(record) {
        var resultText = record && record.classificationResult ? record.classificationResult : ""
        var confidenceText = resultTokenValue(resultText, "confidence")
        var statusText = resultTokenValue(resultText, "status")
        var confidenceValue = Number(confidenceText)

        if (isNaN(confidenceValue)) {
            return "暂无模型可信度数据，可查看左侧原图人工判断。"
        }

        var percentText = Math.max(0, Math.min(100, confidenceValue * 100)).toFixed(1) + "%"
        if (statusText === "BAD") {
            return "分类模型倾向于坏品，可信度 " + percentText + "。"
        }
        if (statusText === "GOOD") {
            return "分类模型倾向于良品，可信度 " + percentText + "，最终结论仍以分类和UNet综合判定为准。"
        }

        return "模型已给出判断，可信度 " + percentText + "。"
    }

    /*
     * historyReadableInspectionText 的作用：
     *   把历史记录的最终结果转换成面向操作员的检测结论。
     *
     * 参数：
     *   record 是当前历史记录，resultText 保存“良品”或“待复核”等界面结果。
     *
     * 返回值：
     *   返回不包含模型内部字段名的中文结论。
     */
    function historyReadableInspectionText(record) {
        var resultText = record && record.resultText ? record.resultText : ""

        if (resultText === "良品") {
            if (record && record.cloudReviewResult && record.cloudReviewResult.length > 0) {
                return "云端复核后确认该零件为良品，板端原始结论为" + record.boardResultText + "。"
            }

            return "分类和UNet均未发现缺陷，系统认为该零件可以进入良品流程。"
        }

        if (resultText === "坏品") {
            if (record && record.cloudReviewResult && record.cloudReviewResult.length > 0) {
                return "云端复核后确认该零件为坏品，板端原始结论为" + record.boardResultText + "。"
            }

            return "系统认为该零件存在缺陷，应进入坏品流程。"
        }

        if (resultText === "待复核") {
            if (record && record.cloudReviewResult && record.cloudReviewResult.length > 0) {
                return "云端复核后仍要求人工复核，板端原始结论为" + record.boardResultText + "。"
            }

            return "分类或UNet发现可疑缺陷，建议人工复核后再分拣。"
        }

        return "该记录需要结合图片和云端状态确认。"
    }

    /*
     * historyCloudReviewResultText 的作用：
     *   把云端 good/bad/review 复核结果转换成中文显示文本。
     *
     * 参数：
     *   record 是当前历史记录详情。
     *
     * 返回值：
     *   返回“良品”“坏品”“待复核”或“未修正”。
     */
    function historyCloudReviewResultText(record) {
        var resultText = record && record.cloudReviewResult ? record.cloudReviewResult : ""

        if (resultText === "good") {
            return "良品"
        }
        if (resultText === "bad") {
            return "坏品"
        }
        if (resultText === "review") {
            return "待复核"
        }

        return "未修正"
    }

    /*
     * historyCloudReviewText 的作用：
     *   生成历史详情里显示的云端修正说明。
     *
     * 主要流程：
     *   1. 旧记录或未被云端修正的记录显示“暂无云端复核修正”。
     *   2. 有云端回写时显示修正结果、板端原始结论、原因、操作人和时间。
     *   3. 原因来自云端点击“修正板端结果”按钮后的弹窗输入，不能为空。
     *
     * 参数：
     *   record 是当前历史记录详情。
     *
     * 返回值：
     *   返回适合检测信息区展示的一行中文说明。
     */
    function historyCloudReviewText(record) {
        if (!record || !record.cloudReviewResult || record.cloudReviewResult.length <= 0) {
            return "暂无云端复核修正，本地结论以双模型综合判定为准。"
        }

        var text = "云端改为" + historyCloudReviewResultText(record)
        var boardText = record.boardResultText && record.boardResultText.length > 0 ? record.boardResultText : "未知"
        text += "，板端原始结论：" + boardText

        if (record.cloudReviewText && record.cloudReviewText.length > 0) {
            text += "，原因：" + record.cloudReviewText
        }
        if (record.cloudReviewOperator && record.cloudReviewOperator.length > 0) {
            text += "，操作人：" + record.cloudReviewOperator
        }
        if (record.cloudReviewTime && record.cloudReviewTime.length > 0) {
            text += "，时间：" + record.cloudReviewTime
        }

        return text + "。"
    }

    /*
     * historyResultFillColor 的作用：
     *   按历史记录最终展示结论生成结果卡片背景色。
     *
     * 参数：
     *   record 是当前历史记录详情。
     *
     * 返回值：
     *   良品为绿色背景，坏品为红色背景，其它为黄色复核背景。
     */
    function historyResultFillColor(record) {
        var resultText = record && record.resultText ? record.resultText : ""

        if (resultText === "良品") {
            return "#173524"
        }
        if (resultText === "坏品") {
            return "#3a1b1f"
        }

        return "#3a2a1c"
    }

    /*
     * historyResultBorderColor 的作用：
     *   按历史记录最终展示结论生成结果卡片边框色。
     *
     * 参数：
     *   record 是当前历史记录详情。
     *
     * 返回值：
     *   良品绿色，坏品红色，待复核黄色。
     */
    function historyResultBorderColor(record) {
        var resultText = record && record.resultText ? record.resultText : ""

        if (resultText === "良品") {
            return root.accentGreen
        }
        if (resultText === "坏品") {
            return root.accentRed
        }

        return root.accentAmber
    }

    /*
     * historyDefectHintText 的作用：
     *   把 UNet 分割结果转换成左侧检测图的查看提示。
     *
     * 主要流程：
     *   1. 从 segmentationResult 中读取 status 和 defect_pixels。
     *   2. defect_pixels 大于 0 或 status=NG 时，提示红色区域代表疑似缺陷。
     *   3. 没有缺陷像素时，提示当前检测图未标出明显缺陷区域。
     *
     * 参数：
     *   record 是当前历史记录，里面可能包含 segmentationResult。
     *
     * 返回值：
     *   返回适合历史详情页展示的中文提示。
     */
    function historyDefectHintText(record) {
        var segmentText = record && record.segmentationResult ? record.segmentationResult : ""
        var statusText = resultTokenValue(segmentText, "status")
        var defectPixelsText = resultTokenValue(segmentText, "defect_pixels")
        var defectPixels = Number(defectPixelsText)

        if (segmentText.length <= 0) {
            return "暂无缺陷区域数据，左侧图片仅作为本地留档。"
        }

        if (statusText === "NG" || (!isNaN(defectPixels) && defectPixels > 0)) {
            return "检测图中的红色区域是系统标出的疑似缺陷位置，建议重点查看。"
        }

        return "检测图未标出明显缺陷区域，可结合原图做最终确认。"
    }

    /*
     * historyUploadFailed 的作用：
     *   判断当前历史记录是否需要显示“重新发送”按钮。
     *
     * 参数：
     *   record 是当前历史记录详情。
     *
     * 返回值：
     *   uploadStatus 含“失败”时返回 true；即使失败过程中已经创建过 record_id，也允许重新发送本地图片。
     */
    function historyUploadFailed(record) {
        if (!record) {
            return false
        }

        return record.uploadStatus && record.uploadStatus.indexOf("失败") >= 0
    }

    /*
     * historyImageArchiveText 的作用：
     *   把旧的本地路径展示区转换成操作员可读的检测信息说明。
     *
     * 参数：
     *   record 是当前历史记录详情。
     *
     * 返回值：
     *   返回图片数量和用途说明，不直接显示本地文件路径。
     */
    function historyImageArchiveText(record) {
        if (!record || record.imageCount <= 0) {
            return "暂无本地图片，建议重新检测。"
        }

        return "本机保留 " + record.imageCount + " 张图片，左侧可切换复核。"
    }

    /*
     * compactHistoryInfoLine 的作用：
     *   把历史详情检测信息中的多余空白压掉，防止小面板里出现空行和末尾截断。
     *
     * 主要流程：
     *   1. 把换行、制表符和连续空格统一压缩成一个空格。
     *   2. 去掉句首句尾空白，保留原本的中文结论含义。
     *
     * 参数：
     *   lineText 是某一条检测说明。
     *
     * 返回值：
     *   返回单段紧凑文本，供 Text.Wrap 在固定宽度内自然换行。
     */
    function compactHistoryInfoLine(lineText) {
        if (!lineText) {
            return ""
        }

        return String(lineText).replace(/\s+/g, " ").replace(/^\s+|\s+$/g, "")
    }

    /*
     * historyAnalysisSummaryText 的作用：
     *   生成历史详情固定高度小面板里的短摘要，避免云端长文直接把内容挤出面板。
     *
     * 主要流程：
     *   1. 优先显示最终检测结论，让操作员先理解结果。
     *   2. 有模型可信度时补一条短可信度说明。
     *   3. 最后提示可以打开完整说明查看全部云端和模型文本。
     *
     * 参数：
     *   record 是当前历史记录详情。
     *
     * 返回值：
     *   返回适合 1024x600 右侧卡片显示的 2 到 3 行摘要。
     */
    function historyAnalysisSummaryText(record) {
        var lines = []

        lines.push("结论：" + historyReadableInspectionText(record))
        if (record && record.classificationResult && record.classificationResult.length > 0) {
            lines.push("可信度：" + historyConfidenceSummaryText(record))
        } else {
            lines.push("可信度：暂无模型可信度数据。")
        }
        lines.push("详情：点击查看完整说明，可读完云端修正、UNet 提示和原始模型输出。")

        return compactHistoryInfoLine(lines.join(" "))
    }

    /*
     * historyPartNameText 的作用：
     *   从历史记录自身的分类结果中还原零件类型，避免查看旧记录时误用当前首页的零件状态。
     *
     * 主要流程：
     *   1. 优先读取 classification_result 里的 class 字段。
     *   2. 复用 detectPartNameFromClass() 的垫圈类标准名称映射。
     *   3. 没有分类字段时返回“见云端记录”，提示操作员以云端详情为准。
     *
     * 参数：
     *   record 是当前历史记录详情。
     *
     * 返回值：
     *   返回历史记录对应的具体零件类型显示名。
     */
    function historyPartNameText(record) {
        if (record && record.classificationResult && record.classificationResult.length > 0) {
            var classText = resultTokenValue(record.classificationResult, "class")
            if (classText.length > 0) {
                return detectPartNameFromClass(classText)
            }
        }

        return "见云端记录"
    }

    /*
     * historyFullAnalysisText 的作用：
     *   生成可滚动浮层中的完整检测说明，保留短摘要之外的所有排障信息。
     *
     * 主要流程：
     *   1. 用分行文本展示零件身份、检测结论、可信度、缺陷提示和图片留档。
     *   2. 云端修正说明单独成行，避免被固定小面板截断。
     *   3. 附加分类和 UNet 原始输出，便于现场排查模型标签和置信度。
     *
     * 参数：
     *   record 是当前历史记录详情。
     *
     * 返回值：
     *   返回多行完整文本，供 Flickable 内的 Text 组件滚动阅读。
     */
    function historyFullAnalysisText(record) {
        var lines = []

        lines.push("零件类型：" + historyPartNameText(record))
        lines.push("检测结论：" + historyReadableInspectionText(record))
        lines.push(record && record.classificationResult && record.classificationResult.length > 0
                   ? "可信度：" + historyConfidenceSummaryText(record)
                   : "可信度：暂无模型可信度数据。")
        lines.push("缺陷提示：" + historyDefectHintText(record))
        lines.push("图片留档：" + historyImageArchiveText(record))
        lines.push("云端修正：" + historyCloudReviewText(record))
        lines.push("云端状态：" + cloudStatusSummary(record ? record.uploadStatus : ""))

        if (record && record.classificationResult && record.classificationResult.length > 0) {
            lines.push("分类模型原始输出：" + compactHistoryInfoLine(record.classificationResult))
        }
        if (record && record.segmentationResult && record.segmentationResult.length > 0) {
            lines.push("UNet 原始输出：" + compactHistoryInfoLine(record.segmentationResult))
        }

        return lines.join("\n")
    }

    /*
     * historyUploadAdviceText 的作用：
     *   根据云端状态给出历史详情里的处理建议。
     *
     * 参数：
     *   record 是当前历史记录详情。
     *
     * 返回值：
     *   上传失败提示重新发送；上传成功提示可按云端编号追溯。
     */
    function historyUploadAdviceText(record) {
        if (historyUploadFailed(record)) {
            return "云端上传失败，可点击上方重新发送，把本地留档图片再次提交到云端。"
        }

        if (isUploadSuccess(record)) {
            return "云端已归档，可按记录ID或云端编号追溯该样本。"
        }

        return "云端状态未确认，可先查看本地检测图片并保留样本。"
    }

    /*
     * formatBytes 的作用：
     *   把图片字节数转换成适合嵌入式屏幕显示的 KB/MB 文本。
     *
     * 参数：
     *   bytes 是文件字节数。
     *
     * 返回值：
     *   返回 0 B、123.4 KB 或 1.23 MB。
     */
    function formatBytes(bytes) {
        if (!bytes || bytes <= 0) {
            return "0 B"
        }

        if (bytes < 1024 * 1024) {
            return (bytes / 1024.0).toFixed(1) + " KB"
        }

        return (bytes / 1024.0 / 1024.0).toFixed(2) + " MB"
    }

    /*
     * latestHistoryText 的作用：
     *   为首页底部最近记录区域生成一行历史摘要。
     *
     * 返回值：
     *   有历史时显示最新检测时间和云端记录号；无历史时显示暂无检测。
     */
    function latestHistoryText() {
        if (uploadHistory.count <= 0) {
            return "最近记录：暂无检测记录"
        }

        var record = uploadHistory.latestEntry()
        var recordName = record.recordNo && record.recordNo.length > 0 ? record.recordNo : "本地记录"

        return "最近检测：" + record.uploadTime + "  |  " + record.resultText + "  |  " + recordName
    }

    /*
     * isUploadSuccess 的作用：
     *   判断一条历史记录是否已经完成云端上传，统计页用它计算上传成功率。
     *
     * 参数：
     *   record 是 uploadHistory.entryAt() 返回的历史记录对象。
     *
     * 返回值：
     *   有 recordId/recordNo 或上传状态包含“成功”时返回 true；否则返回 false。
     */
    function isUploadSuccess(record) {
        if (!record) {
            return false
        }

        if (record.recordId && record.recordId.length > 0) {
            return true
        }

        if (record.recordNo && record.recordNo.length > 0) {
            return true
        }

        return record.uploadStatus && record.uploadStatus.indexOf("成功") >= 0
    }

    /*
     * isGoodRecord 的作用：
     *   判断一条历史记录是否为良品，统计页用它计算良品数和良率。
     *
     * 参数：
     *   record 是 uploadHistory.entryAt() 返回的历史记录对象。
     *
     * 返回值：
     *   resultText 等于“良品”时返回 true；其它待复核、失败或空记录都不算良品。
     */
    function isGoodRecord(record) {
        return record && record.resultText === "良品"
    }

    /*
     * isBadRecord 的作用：
     *   判断一条历史记录是否为坏品，统计页用它统计云端修正后的坏品数量。
     *
     * 参数：
     *   record 是 uploadHistory.entryAt() 返回的历史记录对象。
     *
     * 返回值：
     *   resultText 等于“坏品”时返回 true；其它记录返回 false。
     */
    function isBadRecord(record) {
        return record && record.resultText === "坏品"
    }

    /*
     * historyRecordBytes 的作用：
     *   返回一条历史记录所有图片的总字节数，兼容旧记录只有 jpg/png 两个大小字段的格式。
     *
     * 参数：
     *   record 是 uploadHistory.entryAt() 返回的历史记录对象。
     *
     * 返回值：
     *   返回 source 和所有 annotated 图片大小之和。
     */
    function historyRecordBytes(record) {
        if (!record) {
            return 0
        }

        if (record.totalSizeBytes && record.totalSizeBytes > 0) {
            return record.totalSizeBytes
        }

        var jpgBytes = record.jpgSizeBytes ? record.jpgSizeBytes : 0
        var pngBytes = record.pngSizeBytes ? record.pngSizeBytes : 0
        return jpgBytes + pngBytes
    }

    /*
     * percentText 的作用：
     *   把分子和分母格式化成百分比文本，避免多个统计卡重复写除零判断。
     *
     * 参数：
     *   value 是分子。
     *   total 是分母。
     *
     * 返回值：
     *   total 为 0 时返回 0.0%；否则保留一位小数。
     */
    function percentText(value, total) {
        if (!total || total <= 0) {
            return "0.0%"
        }

        return (value * 100.0 / total).toFixed(1) + "%"
    }

    /*
     * statsSummary 的作用：
     *   汇总检测历史中的核心生产统计，供 KPI 卡、分布条和云端状态区复用。
     *
     * 主要流程：
     *   1. 遍历 uploadHistory 的每条记录。
     *   2. 统计良品、坏品、待复核、上传成功、上传失败、图片数量和文件大小。
     *   3. 保存最近一条记录的时间、云端编号和状态摘要，便于右侧云端健康区展示。
     *
     * 返回值：
     *   返回一个普通 JS 对象，包含 total、good、bad、review、uploadSuccess、uploadFailed 等字段。
     */
    function statsSummary() {
        var summary = {
            total: uploadHistory.count,
            good: 0,
            bad: 0,
            review: 0,
            uploadSuccess: 0,
            uploadFailed: 0,
            imageCount: 0,
            totalBytes: 0,
            maxBytes: 0,
            latestTime: "暂无记录",
            latestRecordNo: "暂无云端编号",
            latestStatus: "暂无上传状态"
        }

        for (var i = 0; i < uploadHistory.count; ++i) {
            var record = uploadHistory.entryAt(i)
            var recordBytes = historyRecordBytes(record)

            if (isGoodRecord(record)) {
                summary.good += 1
            } else if (isBadRecord(record)) {
                summary.bad += 1
            } else {
                summary.review += 1
            }

            if (isUploadSuccess(record)) {
                summary.uploadSuccess += 1
            } else {
                summary.uploadFailed += 1
            }

            summary.imageCount += record.imageCount ? record.imageCount : 0
            summary.totalBytes += recordBytes
            summary.maxBytes = Math.max(summary.maxBytes, recordBytes)

            if (i === uploadHistory.count - 1) {
                summary.latestTime = record.uploadTime && record.uploadTime.length > 0 ? record.uploadTime : "暂无记录"
                summary.latestRecordNo = record.recordNo && record.recordNo.length > 0 ? record.recordNo : "本地记录"
                summary.latestStatus = cloudStatusSummary(record.uploadStatus)
            }
        }

        return summary
    }

    /*
     * statsRecentBars 的作用：
     *   生成最近 8 条记录的轻量柱状图数据，避免嵌入式 Qt 引入额外图表库。
     *
     * 返回值：
     *   返回数组，每项包含 label、value、color 和 index；空历史返回空数组。
     */
    function statsRecentBars() {
        var bars = []
        var start = Math.max(0, uploadHistory.count - 8)

        for (var i = start; i < uploadHistory.count; ++i) {
            var record = uploadHistory.entryAt(i)
            var imageCount = record.imageCount ? record.imageCount : 0

            bars.push({
                label: historyTimePart(record.uploadTime),
                value: Math.max(1, imageCount),
                color: isUploadSuccess(record) ? root.accentGreen : root.accentAmber,
                index: i
            })
        }

        return bars
    }

    /*
     * statsDistributionBars 的作用：
     *   生成良品、坏品、待复核、上传成功、上传失败五条水平分布条的数据。
     *
     * 返回值：
     *   返回数组，每项包含 name、value、total、percent 和 color。
     */
    function statsDistributionBars() {
        var summary = statsSummary()

        return [
            {"name": "良品", "value": summary.good, "total": summary.total, "percent": percentText(summary.good, summary.total), "color": root.accentGreen},
            {"name": "坏品", "value": summary.bad, "total": summary.total, "percent": percentText(summary.bad, summary.total), "color": root.accentRed},
            {"name": "待复核", "value": summary.review, "total": summary.total, "percent": percentText(summary.review, summary.total), "color": root.accentAmber},
            {"name": "上传成功", "value": summary.uploadSuccess, "total": summary.total, "percent": percentText(summary.uploadSuccess, summary.total), "color": "#5aa7ff"},
            {"name": "上传失败", "value": summary.uploadFailed, "total": summary.total, "percent": percentText(summary.uploadFailed, summary.total), "color": root.accentRed}
        ]
    }

    /*
     * statsDistributionLeftBars 的作用：
     *   生成“分布概览”左列数据，只放检测结果类指标。
     *   左列固定为良品、坏品、待复核三项，避免五条统计全部纵向堆叠后超出 160px 面板。
     *
     * 返回值：
     *   返回数组，每项格式与 statsDistributionBars() 相同，供左侧 Repeater 直接渲染。
     */
    function statsDistributionLeftBars() {
        return statsDistributionBars().slice(0, 3)
    }

    /*
     * statsDistributionRightBars 的作用：
     *   生成“分布概览”右列数据，只放上传链路类指标。
     *   右列固定为上传成功和上传失败两项，让现场人员能单独判断云端链路健康。
     *
     * 返回值：
     *   返回数组，每项格式与 statsDistributionBars() 相同，供右侧 Repeater 直接渲染。
     */
    function statsDistributionRightBars() {
        return statsDistributionBars().slice(3, 5)
    }

    /*
     * statsRecentRows 的作用：
     *   生成统计页底部最近记录表数据，最新记录排在最上方。
     *   最近记录区域已经改成 ListView 竖向滑动，所以这里保留全部历史记录，
     *   由 ListView 只实例化可见行，避免记录较多时把 1024x600 屏幕撑满。
     *
     * 返回值：
     *   返回数组，每项包含原始 index 和界面需要显示的短字段。
     */
    function statsRecentRows() {
        var rows = []

        for (var i = uploadHistory.count - 1; i >= 0; --i) {
            var record = uploadHistory.entryAt(i)

            rows.push({
                index: i,
                time: historyTimePart(record.uploadTime),
                result: record.resultText && record.resultText.length > 0 ? record.resultText : "待复核",
                recordNo: record.recordNo && record.recordNo.length > 0 ? record.recordNo : "本地记录",
                images: record.imageCount ? record.imageCount : 0,
                status: cloudStatusSummary(record.uploadStatus),
                statusOk: isUploadSuccess(record)
            })
        }

        return rows
    }

    /*
     * openHistoryDetailFromStats 的作用：
     *   让统计页最近记录表可以跳转到历史详情页，复用已有详情展示和图片轮播。
     *
     * 参数：
     *   index 是 uploadHistory 中的记录索引。
     *
     * 返回值：
     *   无返回值；索引非法时忽略点击。
     */
    function openHistoryDetailFromStats(index) {
        if (index < 0 || index >= uploadHistory.count) {
            return
        }

        root.switchPage("history")
        root.showHistoryDetail(index)
    }

    /* manualCommandLog 保存最近 8 条手动控制命令，页面启动时先给一条待接入提示。 */
    ListModel {
        id: manualCommandLog

        ListElement {
            time: "--:--:--"
            command: "手动控制"
            target: "F4控制器"
            result: "待接入"
        }
    }

    /* alarmHistoryModel 保存告警维护页最近处理记录，第一版由 QML 模拟生成，后续可接日志控制器。 */
    ListModel {
        id: alarmHistoryModel

        ListElement {
            time: "09:42:16"
            code: "0x0007"
            level: "严重"
            title: "急停按下"
            status: "待确认"
        }

        ListElement {
            time: "09:31:08"
            code: "0x0009"
            level: "预警"
            title: "图像抓拍失败后已重试"
            status: "已恢复"
        }

        ListElement {
            time: "09:18:22"
            code: "0x0011"
            level: "预警"
            title: "COS 上传失败，保留本地图片"
            status: "待排查"
        }
    }

    /* clockTimer 每秒更新时间，同时轻微改变 dx 演示值，让界面保持实时感。 */
    Timer {
        id: clockTimer
        interval: 1000
        repeat: true
        running: true

        onTriggered: {
            currentTimeText = Qt.formatDateTime(new Date(), "hh:mm:ss")
            dxPixels = ((dxPixels + 5) % 19) - 9
        }
    }

    /* storageToastTimer 控制 SD 卡提示的生命周期，避免结果文字一直占用界面空间。 */
    Timer {
        id: storageToastTimer
        interval: 3500
        repeat: false
        running: false

        onTriggered: {
            storageToastVisible = false
        }
    }

    /* splashStageTimer 控制开机自检阶段推进，只负责状态节奏，不做真实硬件判定。 */
    Timer {
        id: splashStageTimer
        interval: 620
        repeat: true
        running: root.splashOverlayVisible && !root.splashFadingOut

        onTriggered: {
            root.advanceSplashStage()
        }
    }

    /* splashHideTimer 等待淡出动画完成后再关闭覆盖层，避免 visible 过早变 false 导致跳屏。 */
    Timer {
        id: splashHideTimer
        interval: 360
        repeat: false
        running: false

        onTriggered: {
            root.splashOverlayVisible = false
            root.splashFadingOut = false
            root.startBootOverlayRestore()
        }
    }

    /* bootOverlayRestoreTimer 在 overlay socket 晚于 Qt splash 建立时重试恢复视频层。 */
    Timer {
        id: bootOverlayRestoreTimer
        interval: 300
        repeat: false
        running: false

        onTriggered: {
            root.bootOverlayRestoreAttempts += 1
            var result = root.setBootOverlayVisible(true)
            if (root.bootOverlayRestoreAttempts < root.bootOverlayRestoreMaxAttempts
                    && root.overlayRestoreNeedsRetry(result)) {
                bootOverlayRestoreTimer.restart()
            }
        }
    }

    /*
     * Component.onCompleted 在 QML 根对象加载完成后执行一次。
     * 这里二次隐藏 overlay 视频层，用于覆盖手动重启 Qt 但 overlay 仍在运行的场景。
     */
    Component.onCompleted: {
        root.setBootOverlayVisible(false)
    }

    Connections {
        target: uploadHistory

        /*
         * countChanged 的作用：
         *   当 C++ 检测控制器追加检测历史后，让历史页默认跟到最新记录。
         */
        onCountChanged: {
            if (uploadHistory.count > 0) {
                selectedHistoryIndex = uploadHistory.count - 1
                selectedHistoryImageIndex = 0
                selectedHistoryRecord = uploadHistory.entryAt(selectedHistoryIndex)
            }
        }

        /*
         * onDataChanged 的作用：
         *   云端复核回写会原地修改某条历史记录，详情页保存的是 entryAt() 的快照。
         *   如果当前打开的详情正好被云端修正，必须重新读取该行，才能立刻显示云端原因。
         */
        onDataChanged: {
            if (root.selectedHistoryIndex >= 0
                    && root.selectedHistoryIndex < uploadHistory.count) {
                root.selectedHistoryRecord = uploadHistory.entryAt(root.selectedHistoryIndex)
            }
        }
    }

    Connections {
        target: deviceHealth

        /*
         * onCameraStatusChanged 的作用：
         *   USB 摄像头热拔插后，overlay 进程可能被后台 restart-overlay 重新初始化。
         *   当健康检测确认相机重新在线时，必须等 Qt 启动遮罩完全结束后才能发送 VISIBLE 1 恢复画面；
         *   如果当前在历史、统计、手动、参数或告警页，则保持视频层隐藏，避免覆盖功能页面。
         */
        onCameraStatusChanged: {
            if (!root.usingKmsOverlay || deviceHealth.cameraStatusText !== "在线") {
                return
            }

            if (root.bootOverlayRestoreFinished && !root.splashOverlayVisible && root.activePage === "home") {
                root.setBootOverlayVisible(true)
            }
        }
    }

    Connections {
        target: storageController

        /*
         * onSaveInProgressChanged 的作用：
         *   同步 C++ 后台保存忙状态，让按钮文案和置灰状态准确反映真实线程生命周期。
         */
        onSaveInProgressChanged: {
            saveImageBusy = storageController.saveInProgress
        }

        /*
         * onDetectInProgressChanged 的作用：
         *   同步 C++ 后台检测忙状态，让检测按钮和状态文案反映真实线程生命周期。
         */
        onDetectInProgressChanged: {
            detectImageBusy = storageController.detectInProgress
        }

        /*
         * onSaveCurrentFrameFinished 的作用：
         *   接收旧 SSH 保存自检任务最终结果，并恢复兼容状态标志。
         */
        onSaveCurrentFrameFinished: {
            saveImageBusy = false
            storageState = resultText
            showStorageToast()
        }

        /*
         * onDetectClassificationReady 的作用：
         *   第一个分类模型完成后立刻刷新零件、类别、分类初判和置信度。
         */
        onDetectClassificationReady: {
            if (updateDetectClassificationFields(resultText)) {
                detectState = detectStatus === "BAD"
                              ? "分类模型判定坏品，等待UNet综合"
                              : "分类模型判定良品，等待UNet综合"
                detectStatus = "REVIEW"
                storageState = "分类完成，正在运行UNet..."
                showStorageToast()
            }
        }

        /*
         * onDetectModelsReady 的作用：
         *   两个模型都完成后立刻刷新双模型总耗时，此时 COS 上传仍可能在后台继续。
         */
        onDetectModelsReady: {
            if (updateDetectClassificationFields(resultText)) {
                updateDetectFusedFields(resultText)
                updateDetectModelTimeFields(resultText)
                storageState = "综合判定完成，正在上传..."
                showStorageToast()
            }
        }

        /*
         * onDetectCurrentFrameFinished 的作用：
         *   接收 C++ 后台检测任务最终结果，恢复忙状态并显示上传/历史记录最终状态。
         */
        onDetectCurrentFrameFinished: {
            detectImageBusy = false
            handleDetectResultText(resultText)
            storageState = resultText
            showStorageToast()
        }

        /*
         * onRetryUploadFinished 的作用：
         *   接收历史图片重新发送的最终结果，并刷新当前详情页绑定的记录快照。
         *
         * 主要流程：
         *   1. 重发成功时 C++ 会把该记录移动到本地历史末尾，并把 upload_time 改成本次重发时间。
         *   2. 当前详情页要同步选中末尾记录，否则还会读旧 row，界面时间和云端记录会对不上。
         *   3. 重发失败时记录顺序不变，继续刷新原 row 详情。
         */
        onRetryUploadFinished: {
            if (resultText.indexOf("重新发送成功") === 0 && uploadHistory.count > 0) {
                root.selectedHistoryIndex = uploadHistory.count - 1
                root.selectedHistoryImageIndex = 0
                root.selectedHistoryRecord = uploadHistory.entryAt(root.selectedHistoryIndex)
            } else if (row === root.selectedHistoryIndex) {
                root.selectedHistoryRecord = uploadHistory.entryAt(root.selectedHistoryIndex)
            }

            storageState = resultText
            showStorageToast()
        }
    }

    /* 顶部状态栏：显示系统时间、通信状态、相机状态、背光常亮和云端状态。 */
    Rectangle {
        id: topBar
        x: 0
        y: 0
        width: root.width
        height: 56
        color: "#151719"
        border.color: root.borderColor
        border.width: 1

        Text {
            id: titleText
            x: 18
            anchors.verticalCenter: parent.verticalCenter
            text: "工业缺陷检测"
            color: "#f2f4f5"
            font.pixelSize: 22
            font.bold: true
        }

        Text {
            id: timeText
            x: 170
            anchors.verticalCenter: parent.verticalCenter
            text: currentTimeText
            color: "#cfd5d9"
            font.pixelSize: 18
        }

        Row {
            id: statusRow
            anchors.right: parent.right
            anchors.rightMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            spacing: 20

            Repeater {
                model: [
                    {"name": "网络", "value": deviceHealth.networkStatusText, "dot": deviceHealth.networkStatusColor},
                    {"name": "相机", "value": root.cameraStatusText(), "dot": root.cameraDotColor()},
                    {"name": "F4", "value": deviceHealth.f4StatusText, "dot": deviceHealth.f4StatusColor},
                    {"name": "机械臂", "value": "待命", "dot": root.accentGreen},
                    {"name": "背光", "value": "常亮", "dot": root.accentGreen},
                    {"name": "云端", "value": deviceHealth.cloudStatusText, "dot": deviceHealth.cloudStatusColor}
                ]

                Row {
                    spacing: 6

                    Rectangle {
                        width: 9
                        height: 9
                        radius: 4
                        anchors.verticalCenter: parent.verticalCenter
                        color: modelData.dot
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.name + ":" + modelData.value
                        color: "#dce1e4"
                        font.pixelSize: 14
                    }
                }
            }
        }
    }

    /* 左侧导航栏：当前只做视觉占位，不切换页面，后续可接 StackView。 */
    Rectangle {
        id: navPanel
        x: 0
        y: topBar.height
        width: 160
        height: root.height - topBar.height
        color: "#17191b"
        border.color: root.borderColor
        border.width: 1

        Column {
            x: 12
            y: 18
            width: parent.width - 24
            spacing: 10

            Repeater {
                model: [
                    {"text": "首页", "page": "home"},
                    {"text": "历史记录", "page": "history"},
                    {"text": "统计分析", "page": "stats"},
                    {"text": "手动控制", "page": "manual"},
                    {"text": "参数设置", "page": "settings"},
                    {"text": "告警维护", "page": "alarm"}
                ]

                Rectangle {
                    width: parent.width
                    height: 42
                    radius: 6
                    color: root.activePage === modelData.page ? "#24483a" : "transparent"
                    border.color: root.activePage === modelData.page ? root.accentGreen : "#2b3034"
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: modelData.text
                        color: root.activePage === modelData.page ? "#ffffff" : "#b8c0c4"
                        font.pixelSize: 16
                        font.bold: root.activePage === modelData.page
                    }

                    MouseArea {
                        anchors.fill: parent

                        onClicked: {
                            root.switchPage(modelData.page)
                        }
                    }
                }
            }
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 16
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            text: "v" + appVersion
            color: "#747d83"
            font.pixelSize: 13
        }
    }

    /* 实时画面区：VideoOutput 显示 UVC 摄像头，叠加中心线、ROI 和演示检测框。 */
    Rectangle {
        id: videoPanel
        x: 176
        y: 72
        width: root.usingKmsOverlay ? 642 : 560
        height: root.usingKmsOverlay ? 482 : 330
        radius: 8
        color: "#050606"
        border.color: root.borderColor
        border.width: 1
        clip: true
        visible: !root.historyPageVisible && !root.statsPageVisible && !root.manualPageVisible
                 && !root.settingsPageVisible && !root.alarmPageVisible

        V4L2VideoItem {
            id: cameraView

            /* anchors.fill 让视频纹理填满实时画面区，再由外层 clip 裁掉超出部分。 */
            anchors.fill: parent
            anchors.margins: 1

            /* GL 或 KMS overlay 后端接管画面时隐藏安全预览，避免两个后端同时占用 /dev/video0。 */
            visible: !root.usingGstVideo && !root.usingKmsOverlay

            /* device 由 C++ 命令行参数传入，默认是 /dev/video0。 */
            device: root.cameraDeviceName

            /* captureWidth/captureHeight 控制 V4L2 采集尺寸，降低尺寸可明显减少 CPU 拷贝和纹理上传压力。 */
            captureWidth: root.captureWidth
            captureHeight: root.captureHeight

            /* fps 越高，每秒拷贝和上传纹理的次数越多；默认从 10fps 开始压低 CPU。 */
            fps: root.captureFps

            /* running 只在安全预览后端为 true，GL/KMS 后端由外部路径独占摄像头。 */
            running: !root.usingGstVideo && !root.usingKmsOverlay
        }

        Loader {
            id: gstVideoLoader

            /* gstVideoLoader 只在 gst-qml 后端加载，避免缺少 qmlglsink 时影响 qt-safe 回退。 */
            active: root.usingGstVideo
            anchors.fill: parent
            anchors.margins: 1
            source: root.usingGstVideo ? "qrc:/qml/GstVideoSurface.qml" : ""
            visible: root.usingGstVideo
        }

        /* 未出图时显示状态文字，避免黑屏时无法判断是程序未启动还是相机未就绪。 */
        Rectangle {
            anchors.fill: parent
            color: "#050606"
            opacity: root.cameraIsActive() ? 0.0 : 0.82
            visible: opacity > 0

            Text {
                anchors.centerIn: parent
                text: "UVC " + root.cameraStatusText() + "\n" + root.cameraDeviceName + "\n" + root.videoBackend
                color: "#dbe1e4"
                font.pixelSize: 20
                horizontalAlignment: Text.AlignHCenter
                lineHeight: 1.35
            }
        }

        /* 垂直中心线：用 Rectangle 而不是 Canvas，保证叠加层也由 Qt Quick/GPU 合成。 */
        Rectangle {
            width: 2
            height: parent.height - 44
            x: parent.width / 2 - 1
            y: 22
            color: root.accentGreen
            opacity: 0.8
            visible: !root.usingKmsOverlay
        }

        /* 水平中心线：用于后续视觉居中调试。 */
        Rectangle {
            width: parent.width - 44
            height: 2
            x: 22
            y: parent.height / 2 - 1
            color: root.accentGreen
            opacity: 0.8
            visible: !root.usingKmsOverlay
        }

        /* ROI 外框：模拟检测窗口，后续可由检测服务动态调整。 */
        Rectangle {
            width: 330
            height: 190
            anchors.centerIn: parent
            color: "transparent"
            border.color: "#f4d35e"
            border.width: 2
            radius: 4
            opacity: 0.9
            visible: !root.usingKmsOverlay
        }

        /* 缺陷框演示：当前只是固定位置，用于展示后续叠加效果。 */
        Rectangle {
            width: 72
            height: 42
            x: parent.width / 2 + 72
            y: parent.height / 2 - 56
            color: "transparent"
            border.color: root.accentRed
            border.width: 2
            radius: 3
            opacity: 0.85
            visible: !root.usingKmsOverlay
        }

        Text {
            x: 14
            y: 12
            text: "实时画面  " + root.cameraDeviceName + "  " + root.captureWidth + "x" + root.captureHeight + "@" + root.captureFps
            color: "#eef3f4"
            font.pixelSize: 15
            font.bold: true
            visible: !root.usingKmsOverlay
        }

        Text {
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 12
            text: "dx=" + (dxPixels >= 0 ? "+" : "") + dxPixels + " px"
            color: Math.abs(dxPixels) <= 5 ? root.accentGreen : root.accentAmber
            font.pixelSize: 18
            font.bold: true
            visible: !root.usingKmsOverlay
        }
    }

    /* 右侧结果面板：展示当前零件和多模态检测结果，先使用静态演示值。 */
    Rectangle {
        id: resultPanel
        x: root.usingKmsOverlay ? 826 : 752
        y: 72
        width: root.usingKmsOverlay ? 182 : 256
        height: root.usingKmsOverlay ? 482 : 330
        radius: 8
        color: root.panelColor
        border.color: root.borderColor
        border.width: 1
        visible: !root.historyPageVisible && !root.statsPageVisible && !root.manualPageVisible
                 && !root.settingsPageVisible && !root.alarmPageVisible

        Text {
            x: 16
            y: 14
            text: "当前结果"
            color: "#f1f4f5"
            font.pixelSize: root.usingKmsOverlay ? 18 : 20
            font.bold: true
        }

        Column {
            x: root.usingKmsOverlay ? 12 : 16
            y: 52
            width: parent.width - (root.usingKmsOverlay ? 24 : 32)
            spacing: root.usingKmsOverlay ? 6 : 8

            Repeater {
                model: root.usingKmsOverlay ? [
                    {"name": "零件", "value": root.detectPartName},
                    {"name": "状态", "value": root.workflowState},
                    {"name": "偏差", "value": (root.dxPixels >= 0 ? "+" : "") + root.dxPixels + " px"},
                    {"name": "类别", "value": root.compactHomeClassText(root.detectClassName)},
                    {"name": "模型", "value": root.compactHomeModelText(root.detectState)},
                    {"name": "耗时", "value": root.detectTimeText}
                ] : [
                    {"name": "当前零件", "value": root.detectPartName},
                    {"name": "流程状态", "value": root.workflowState},
                    {"name": "视觉偏差", "value": (root.dxPixels >= 0 ? "+" : "") + root.dxPixels + " px"},
                    {"name": "模型类别", "value": root.compactHomeClassText(root.detectClassName)},
                    {"name": "检测状态", "value": root.compactHomeModelText(root.detectState)},
                    {"name": "推理耗时", "value": root.detectTimeText}
                ]

                Row {
                    width: parent.width
                    height: root.usingKmsOverlay ? 22 : 22

                    Text {
                        width: root.usingKmsOverlay ? 48 : 86
                        text: modelData.name
                        color: "#929ca2"
                        font.pixelSize: root.usingKmsOverlay ? 13 : 14
                        elide: Text.ElideRight
                    }

                    Text {
                        width: parent.width - (root.usingKmsOverlay ? 48 : 86)
                        text: modelData.value
                        color: modelData.value === "待接入" || modelData.value === "等待检测"
                               || modelData.value === "检测中..." ? root.accentAmber : "#edf2f3"
                        font.pixelSize: root.usingKmsOverlay ? 13 : 15
                        font.bold: true
                        elide: Text.ElideRight
                    }
                }
            }
        }

        Rectangle {
            x: root.usingKmsOverlay ? 12 : 16
            y: root.usingKmsOverlay ? 224 : 228
            width: parent.width - (root.usingKmsOverlay ? 24 : 32)
            height: root.usingKmsOverlay ? 38 : 46
            radius: 8
            color: root.detectStatus === "BAD" ? "#3a1b1f"
                   : root.detectStatus === "ERROR" || root.detectStatus === "REVIEW" ? "#33251a"
                   : root.detectStatus === "WAIT" ? "#252a2e" : "#173524"
            border.color: root.detectStatus === "BAD" ? root.accentRed
                          : root.detectStatus === "ERROR" || root.detectStatus === "WAIT" || root.detectStatus === "REVIEW"
                            ? root.accentAmber : root.accentGreen
            border.width: 1

            Text {
                anchors.centerIn: parent
                text: root.detectStatus === "BAD" ? (root.usingKmsOverlay ? "检测：坏品" : "模型检测：坏品")
                      : root.detectStatus === "GOOD" ? (root.usingKmsOverlay ? "检测：良品" : "模型检测：良品")
                      : root.detectStatus === "REVIEW" && root.detectImageBusy ? "等待综合判定"
                      : root.detectStatus === "REVIEW" ? "检测：待复核"
                      : root.detectStatus === "ERROR" ? "检测失败" : "等待检测"
                color: "#ffffff"
                font.pixelSize: root.usingKmsOverlay ? 17 : 22
                font.bold: true
            }
        }

        Row {
            x: 16
            y: 286
            width: parent.width - 32
            height: 18
            spacing: 10
            visible: !root.usingKmsOverlay

            Text {
                text: "置信度"
                color: "#929ca2"
                font.pixelSize: 13
            }

            Rectangle {
                width: 128
                height: 8
                radius: 4
                anchors.verticalCenter: parent.verticalCenter
                color: "#2a3034"

                Rectangle {
                    width: parent.width * root.detectConfidenceRatio
                    height: parent.height
                    radius: 4
                    color: root.detectStatus === "BAD" ? root.accentRed
                           : root.detectStatus === "ERROR" || root.detectStatus === "WAIT" || root.detectStatus === "REVIEW"
                             ? root.accentAmber : root.accentGreen
                }
            }

            Text {
                text: root.detectConfidenceText
                color: "#dfe5e7"
                font.pixelSize: 13
            }
        }

        Column {
            x: 12
            y: 270
            width: parent.width - 24
            spacing: 4
            visible: root.usingKmsOverlay

            Text {
                width: parent.width
                text: "置信度  " + root.detectConfidencePercentText
                color: "#dfe5e7"
                font.pixelSize: 13
                font.bold: true
                elide: Text.ElideRight
            }

            Rectangle {
                width: parent.width
                height: 8
                radius: 4
                color: "#2a3034"

                Rectangle {
                    width: parent.width * root.detectConfidenceRatio
                    height: parent.height
                    radius: 4
                    color: root.detectStatus === "BAD" ? root.accentRed
                           : root.detectStatus === "ERROR" || root.detectStatus === "WAIT" || root.detectStatus === "REVIEW"
                             ? root.accentAmber : root.accentGreen
                }
            }

            Text {
                width: parent.width
                text: root.detectBadTotalText + "  " + root.detectGoodTotalText
                color: "#aeb7bc"
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }

        /* storageControls 是真实操作区，检测会自动保存两种模型的图片和结果，安全卸载仍走脚本链路。 */
        Column {
            id: storageControls
            x: 12
            y: root.usingKmsOverlay ? 328 : parent.height - 134
            width: parent.width - 24
            height: root.usingKmsOverlay ? 66 : childrenRect.height
            spacing: 6
            visible: root.usingKmsOverlay

            Repeater {
                model: [
                    {"text": "检测", "action": "detect", "color": root.detectStatus === "BAD" ? root.accentRed : root.accentGreen},
                    {"text": "安全卸载", "action": "safe-remove", "color": root.accentAmber}
                ]

                Rectangle {
                    property bool storageActionBusy: root.saveImageBusy || storageController.saveInProgress
                    property bool detectActionBusy: root.detectImageBusy || storageController.detectInProgress
                    property bool actionEnabled: modelData.action === "detect"
                                                 ? !detectActionBusy
                                                 : !detectActionBusy && !storageActionBusy

                    width: storageControls.width
                    height: 30
                    radius: 6
                    color: actionEnabled ? (storageButtonMouse.pressed ? "#2d3338" : "#22272b") : "#171b1e"
                    border.color: actionEnabled ? modelData.color : "#3a4147"
                    border.width: 1
                    opacity: actionEnabled ? 1.0 : 0.55

                    Text {
                        anchors.centerIn: parent
                        text: modelData.action === "detect" && detectActionBusy ? "检测中..."
                              : modelData.text
                        color: "#ffffff"
                        font.pixelSize: 12
                        font.bold: true
                    }

                    MouseArea {
                        id: storageButtonMouse
                        anchors.fill: parent
                        enabled: parent.actionEnabled

                        onClicked: {
                            if (modelData.action === "detect") {
                                root.handleDetectAction()
                            } else {
                                root.handleStorageAction(modelData.action)
                            }
                        }
                    }
                }
            }
        }

        /* overlayControls 是 KMS overlay 模式专用触摸控制区，放在右侧结果面板底部，避免遮挡视频 plane。 */
        Grid {
            id: overlayControls
            x: 12
            y: root.usingKmsOverlay ? storageControls.y + storageControls.height + 10 : parent.height - 68
            width: parent.width - 24
            height: 58
            columns: 2
            rowSpacing: 6
            columnSpacing: 8
            visible: root.usingKmsOverlay

            Repeater {
                model: [
                    {"text": "开始", "action": "start", "state": "定位预览", "color": root.accentGreen},
                    {"text": "暂停", "action": "pause", "state": "暂停", "color": root.accentAmber},
                    {"text": "继续", "action": "resume", "state": "继续检测", "color": "#5aa7ff"},
                    {"text": "停止", "action": "stop", "state": "停止", "color": root.accentRed}
                ]

                Rectangle {
                    width: (overlayControls.width - overlayControls.columnSpacing) / 2
                    height: 24
                    radius: 6
                    color: overlayButtonMouse.pressed ? "#2d3338" : "#22272b"
                    border.color: modelData.color
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: modelData.text
                        color: "#ffffff"
                        font.pixelSize: 12
                        font.bold: true
                    }

                    MouseArea {
                        id: overlayButtonMouse
                        anchors.fill: parent

                        onClicked: {
                            root.handleControlAction(modelData.action, modelData.state)
                        }
                    }
                }
            }
        }
    }

    /* storageToast 是底部横向提示条，利用 KMS overlay 模式下画面和右侧面板下方的空白区域显示保存/卸载结果。 */
    Rectangle {
        id: storageToast
        x: 176
        y: root.height - 38
        width: root.width - 192
        height: 28
        radius: 6
        color: storageState.indexOf("失败") >= 0 ? "#3a1b1f" : "#16291f"
        border.color: storageState.indexOf("失败") >= 0 ? root.accentRed : root.accentGreen
        border.width: 1
        opacity: storageToastVisible ? 1.0 : 0.0
        visible: opacity > 0.01

        Behavior on opacity {
            NumberAnimation {
                duration: 160
            }
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: storageState
            color: storageState.indexOf("失败") >= 0 ? "#ffd6dc" : "#d9ffe8"
            font.pixelSize: 14
            font.bold: true
            elide: Text.ElideMiddle
            verticalAlignment: Text.AlignVCenter
        }
    }

    /* historyPage 是检测历史记录界面：默认只显示检测记录列表，点击“查看”后进入单条记录详情。 */
    Rectangle {
        id: historyPage
        x: 176
        y: 72
        width: root.width - 192
        height: root.height - 88
        radius: 8
        color: "#171a1d"
        border.color: root.borderColor
        border.width: 1
        visible: root.historyPageVisible
        clip: true

        Rectangle {
            id: historyHeader
            x: 16
            y: 12
            width: parent.width - 32
            height: 34
            color: "transparent"

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: root.historyDetailVisible ? "记录详情" : "检测历史"
                color: "#f1f4f5"
                font.pixelSize: 20
                font.bold: true
            }

            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                Rectangle {
                    width: 94
                    height: 26
                    radius: 6
                    color: "#20332a"
                    border.color: root.accentGreen
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "记录 " + uploadHistory.count
                        color: "#d9ffe8"
                        font.pixelSize: 13
                        font.bold: true
                    }
                }

                Rectangle {
                    width: 86
                    height: 26
                    radius: 6
                    color: backListMouse.pressed ? "#26323a" : "#1f262b"
                    border.color: "#5aa7ff"
                    border.width: 1
                    visible: root.historyDetailVisible

                    Text {
                        anchors.centerIn: parent
                        text: "返回列表"
                        color: "#eef6ff"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    MouseArea {
                        id: backListMouse
                        anchors.fill: parent

                        onClicked: {
                            root.backToHistoryList()
                        }
                    }
                }

                Rectangle {
                    width: 86
                    height: 26
                    radius: 6
                    color: "#22272b"
                    border.color: "#3c444a"
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "返回首页"
                        color: "#eef3f4"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    MouseArea {
                        anchors.fill: parent

                        onClicked: {
                            root.switchPage("home")
                        }
                    }
                }
            }
        }

        Rectangle {
            id: historyListPanel
            x: 16
            y: 54
            width: parent.width - 32
            height: parent.height - 70
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1
            visible: !root.historyDetailVisible
            clip: true

            Text {
                x: 18
                y: 16
                text: "每次检测按时间独立保存，左右滑动查看更多记录"
                color: "#aeb9bf"
                font.pixelSize: 14
                font.bold: true
            }

            ListView {
                id: historyListView
                x: 18
                y: 54
                width: parent.width - 36
                height: parent.height - 72
                orientation: ListView.Horizontal
                spacing: 14
                clip: true
                model: uploadHistory
                boundsBehavior: Flickable.StopAtBounds
                highlightRangeMode: ListView.NoHighlightRange
                interactive: uploadHistory.count > 0
                cacheBuffer: width * 2
                flickDeceleration: 1800
                maximumFlickVelocity: 1100
                highlightMoveDuration: 180

                delegate: Rectangle {
                    width: 226
                    height: historyListView.height - 8
                    radius: 8
                    color: root.selectedHistoryIndex === index ? "#1f3a2f" : "#20262a"
                    border.color: root.selectedHistoryIndex === index ? root.accentGreen : "#3a444b"
                    border.width: 1
                    clip: true

                    Text {
                        x: 14
                        y: 12
                        width: parent.width - 28
                        text: root.historyDatePart(uploadTime)
                        color: "#f4f7f8"
                        font.pixelSize: 20
                        font.bold: true
                    }

                    Text {
                        x: 14
                        y: 40
                        width: parent.width - 28
                        text: root.historyTimePart(uploadTime)
                        color: root.accentGreen
                        font.pixelSize: 18
                        font.bold: true
                    }

                    Rectangle {
                        x: 14
                        y: 72
                        width: parent.width - 28
                        height: 58
                        radius: 7
                        color: "#171c20"
                        border.color: "#344047"
                        border.width: 1

                        Text {
                            x: 10
                            y: 8
                            text: "云端编号"
                            color: "#8f9aa1"
                            font.pixelSize: 12
                        }

                        Text {
                            x: 10
                            y: 28
                            width: parent.width - 20
                            text: recordNo && recordNo.length > 0 ? recordNo : "本地检测记录"
                            color: "#eef3f4"
                            font.pixelSize: 13
                            font.bold: true
                            elide: Text.ElideMiddle
                        }
                    }

                    Row {
                        x: 14
                        y: 144
                        width: parent.width - 28
                        spacing: 8

                        Rectangle {
                            width: 64
                            height: 28
                            radius: 6
                            color: resultText === "良品" ? "#173524" : (resultText === "坏品" ? "#3a1b1f" : "#3a301a")
                            border.color: resultText === "良品" ? root.accentGreen : (resultText === "坏品" ? root.accentRed : root.accentAmber)
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: resultText
                                color: "#ffffff"
                                font.pixelSize: 13
                                font.bold: true
                            }
                        }

                        Rectangle {
                            width: parent.width - 72
                            height: 28
                            radius: 6
                            color: "#252c31"
                            border.color: "#3c464e"
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: imageCount + " 张图片"
                                color: "#cfd7db"
                                font.pixelSize: 13
                                font.bold: true
                            }
                        }
                    }

                    Rectangle {
                        x: 14
                        y: 188
                        width: parent.width - 28
                        height: 64
                        radius: 7
                        color: "#171c20"
                        border.color: "#344047"
                        border.width: 1

                        Column {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 5

                            Text {
                                width: parent.width
                                text: "上传状态"
                                color: "#8f9aa1"
                                font.pixelSize: 12
                            }

                            Text {
                                width: parent.width
                                text: root.cloudStatusSummary(uploadStatus)
                                color: "#dfffea"
                                font.pixelSize: 13
                                font.bold: true
                                elide: Text.ElideRight
                            }
                        }
                    }

                    Rectangle {
                        x: 14
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 56
                        width: parent.width - 28
                        height: 32
                        radius: 7
                        color: deleteMouse.pressed ? "#4a2528" : "#332125"
                        border.color: root.accentRed
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "删除"
                            color: "#ffecec"
                            font.pixelSize: 14
                            font.bold: true
                        }

                        MouseArea {
                            id: deleteMouse
                            anchors.fill: parent

                            onClicked: {
                                root.deleteHistoryRecord(index)
                            }
                        }
                    }

                    Rectangle {
                        x: 14
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 14
                        width: parent.width - 28
                        height: 34
                        radius: 7
                        color: viewMouse.pressed ? "#2f373d" : "#263039"
                        border.color: "#5aa7ff"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "查看"
                            color: "#ffffff"
                            font.pixelSize: 14
                            font.bold: true
                        }

                        MouseArea {
                            id: viewMouse
                            anchors.fill: parent

                            onClicked: {
                                root.showHistoryDetail(index)
                            }
                        }
                    }

                    MouseArea {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 96

                        onClicked: {
                            selectedHistoryIndex = index
                            selectedHistoryRecord = uploadHistory.entryAt(index)
                        }
                    }
                }

                Rectangle {
                    anchors.fill: parent
                    radius: 8
                    color: "#141719"
                    border.color: "#30363b"
                    border.width: 1
                    visible: uploadHistory.count <= 0

                    Text {
                        anchors.centerIn: parent
                        text: "暂无检测记录"
                        color: "#909aa0"
                        font.pixelSize: 20
                        font.bold: true
                    }
                }
            }
        }

        Rectangle {
            id: historyDetailPanel
            x: 16
            y: 54
            width: parent.width - 32
            height: parent.height - 70
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1
            visible: root.historyDetailVisible && uploadHistory.count > 0 && root.selectedHistoryIndex >= 0
            clip: true

            Rectangle {
                id: historyImagePanel
                x: 14
                y: 14
                width: 454
                height: parent.height - 28
                radius: 8
                color: "#070808"
                border.color: "#30363b"
                border.width: 1
                clip: true

                ListView {
                    id: imageCarousel
                    anchors.fill: parent
                    anchors.margins: 10
                    model: root.historyImages()
                    currentIndex: root.selectedHistoryImageIndex
                    orientation: ListView.Horizontal
                    snapMode: ListView.SnapOneItem
                    highlightRangeMode: ListView.StrictlyEnforceRange
                    preferredHighlightBegin: 0
                    preferredHighlightEnd: width
                    boundsBehavior: Flickable.StopAtBounds
                    interactive: root.historyImages().length > 1
                    cacheBuffer: width * 2
                    flickDeceleration: 1500
                    maximumFlickVelocity: 900
                    highlightMoveDuration: 180
                    clip: true

                    delegate: Image {
                        width: imageCarousel.width
                        height: imageCarousel.height
                        source: "file://" + modelData.path
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        asynchronous: true
                    }

                    onCurrentIndexChanged: {
                        root.selectedHistoryImageIndex = currentIndex
                    }
                }

                Rectangle {
                    x: 12
                    y: 10
                    width: 152
                    height: 28
                    radius: 6
                    color: "#171d22"
                    border.color: "#3d464d"
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: root.currentHistoryImageText()
                        color: "#f4f7f8"
                        font.pixelSize: 12
                        font.bold: true
                    }
                }

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 12
                    spacing: 8

                    Repeater {
                        model: root.historyImages().length

                        Rectangle {
                            width: 28
                            height: 7
                            radius: 3
                            color: index === root.selectedHistoryImageIndex ? root.accentGreen : "#56616a"
                        }
                    }
                }
            }

            Column {
                id: detailInfoColumn
                x: 486
                y: 14
                width: parent.width - 500
                height: parent.height - 28
                spacing: 8

                Rectangle {
                    width: parent.width
                    height: 54
                    radius: 8
                    color: root.historyResultFillColor(root.selectedHistoryRecord)
                    border.color: root.historyResultBorderColor(root.selectedHistoryRecord)
                    border.width: 1

                    Text {
                        x: 12
                        y: 7
                        text: "检测结果"
                        color: "#b9c4c9"
                        font.pixelSize: 12
                    }

                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 14
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.selectedHistoryRecord.resultText
                        color: "#ffffff"
                        font.pixelSize: 24
                        font.bold: true
                    }
                }

                Grid {
                    id: historyMetricGrid
                    width: parent.width
                    columns: 2
                    rowSpacing: 7
                    columnSpacing: 7

                    Repeater {
                        model: [
                            {"name": "上传时间", "value": root.selectedHistoryRecord.uploadTime},
                            {"name": "记录ID", "value": root.selectedHistoryRecord.recordId && root.selectedHistoryRecord.recordId.length > 0 ? root.selectedHistoryRecord.recordId : "本地"},
                            {"name": "图片数量", "value": root.selectedHistoryRecord.imageCount + " 张"},
                            {"name": "云端编号", "value": root.historyCloudRecordNoText(root.selectedHistoryRecord)}
                        ]

                        Rectangle {
                            width: (detailInfoColumn.width - 7) / 2
                            height: 44
                            radius: 7
                            color: "#20262a"
                            border.color: "#343c42"
                            border.width: 1

                            Text {
                                x: 9
                                y: 5
                                text: modelData.name
                                color: "#8f9aa1"
                                font.pixelSize: 11
                            }

                            Text {
                                x: 9
                                y: 22
                                width: parent.width - 18
                                text: modelData.value
                                color: "#eef3f4"
                                font.pixelSize: 12
                                font.bold: true
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 82
                    radius: 8
                    color: root.selectedHistoryRecord.uploadStatus
                           && root.selectedHistoryRecord.uploadStatus.indexOf("失败") >= 0 ? "#3a1b1f" : "#16291f"
                    border.color: root.selectedHistoryRecord.uploadStatus
                                  && root.selectedHistoryRecord.uploadStatus.indexOf("失败") >= 0 ? root.accentRed : root.accentGreen
                    border.width: 1

                    Text {
                        x: 12
                        y: 7
                        text: "云端状态"
                        color: "#93a0a6"
                        font.pixelSize: 12
                    }

                    Text {
                        x: 12
                        y: 26
                        width: root.historyUploadFailed(root.selectedHistoryRecord) ? parent.width - 132 : parent.width - 24
                        text: root.cloudStatusSummary(root.selectedHistoryRecord.uploadStatus)
                        color: "#edf7f0"
                        font.pixelSize: 13
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Rectangle {
                        x: parent.width - 112
                        y: 24
                        width: 100
                        height: 32
                        radius: 7
                        visible: root.historyUploadFailed(root.selectedHistoryRecord)
                        color: retryHistoryUploadMouse.pressed ? "#30413a" : "#1f332b"
                        border.color: storageController.retryUploadInProgress ? "#65706a" : root.accentGreen
                        border.width: 1
                        opacity: storageController.retryUploadInProgress ? 0.62 : 1.0

                        Text {
                            anchors.centerIn: parent
                            text: storageController.retryUploadInProgress ? "发送中" : "重新发送"
                            color: "#eafff2"
                            font.pixelSize: 13
                            font.bold: true
                        }

                        MouseArea {
                            id: retryHistoryUploadMouse
                            anchors.fill: parent
                            enabled: !storageController.retryUploadInProgress

                            onClicked: {
                                root.retryUploadHistoryRecord(root.selectedHistoryIndex)
                            }
                        }
                    }

                    Text {
                        x: 12
                        y: 58
                        width: parent.width - 24
                        text: root.historyUploadAdviceText(root.selectedHistoryRecord)
                        color: "#c9d1d5"
                        font.pixelSize: 10
                        elide: Text.ElideRight
                    }
                }

                Rectangle {
                    id: historyAnalysisPanel
                    width: parent.width
                    height: 159
                    radius: 8
                    color: "#20262a"
                    border.color: "#343c42"
                    border.width: 1
                    clip: true

                    Column {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 1

                        Text {
                            width: parent.width
                            text: "检测信息"
                            color: "#f1f4f5"
                            font.pixelSize: 14
                            font.bold: true
                            maximumLineCount: 1
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width
                            text: root.historyAnalysisSummaryText(root.selectedHistoryRecord)
                            color: "#c9d1d5"
                            font.pixelSize: 10
                            wrapMode: Text.Wrap
                            maximumLineCount: 5
                            elide: Text.ElideRight
                            lineHeightMode: Text.ProportionalHeight
                            lineHeight: 0.92
                        }

                        Rectangle {
                            width: parent.width
                            height: 28
                            radius: 7
                            color: historyAnalysisDetailMouse.pressed ? "#30413a" : "#1f332b"
                            border.color: root.accentGreen
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: "查看完整说明"
                                color: "#eafff2"
                                font.pixelSize: 12
                                font.bold: true
                            }

                            MouseArea {
                                id: historyAnalysisDetailMouse
                                anchors.fill: parent

                                onClicked: {
                                    root.historyAnalysisDetailVisible = true
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        id: historyAnalysisDetailOverlay
        anchors.fill: parent
        z: 900
        visible: root.historyAnalysisDetailVisible
        color: "#b0000000"

        MouseArea {
            anchors.fill: parent

            onClicked: {
                root.historyAnalysisDetailVisible = false
            }
        }

        Rectangle {
            width: 620
            height: 438
            anchors.centerIn: parent
            radius: 10
            color: "#20262a"
            border.color: root.accentGreen
            border.width: 1
            clip: true

            MouseArea {
                anchors.fill: parent
            }

            Text {
                x: 18
                y: 14
                text: "检测完整说明"
                color: "#f1f4f5"
                font.pixelSize: 18
                font.bold: true
            }

            Rectangle {
                x: parent.width - 90
                y: 12
                width: 72
                height: 30
                radius: 7
                color: closeAnalysisDetailMouse.pressed ? "#3a1b1f" : "#2a2020"
                border.color: root.accentRed
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "关闭"
                    color: "#ffecef"
                    font.pixelSize: 12
                    font.bold: true
                }

                MouseArea {
                    id: closeAnalysisDetailMouse
                    anchors.fill: parent

                    onClicked: {
                        root.historyAnalysisDetailVisible = false
                    }
                }
            }

            Flickable {
                id: analysisDetailFlickable
                x: 18
                y: 56
                width: parent.width - 36
                height: parent.height - 74
                contentWidth: width
                contentHeight: fullAnalysisText.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Text {
                    id: fullAnalysisText
                    width: analysisDetailFlickable.width
                    text: root.historyFullAnalysisText(root.selectedHistoryRecord)
                    color: "#d7dee2"
                    font.pixelSize: 15
                    lineHeightMode: Text.ProportionalHeight
                    lineHeight: 1.28
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    /* statsPage 是统计分析界面：基于本地检测历史汇总生产、云端和文件状态。 */
    Rectangle {
        id: statsPage
        x: 176
        y: 72
        width: root.width - 192
        height: root.height - 88
        radius: 8
        color: "#171a1d"
        border.color: root.borderColor
        border.width: 1
        visible: root.statsPageVisible
        clip: true

        /* statsHeader 显示页面标题、当前记录数和返回首页入口。 */
        Rectangle {
            id: statsHeader
            x: 16
            y: 12
            width: parent.width - 32
            height: 34
            color: "transparent"

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "统计分析"
                color: "#f1f4f5"
                font.pixelSize: 20
                font.bold: true
            }

            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                Rectangle {
                    width: 114
                    height: 26
                    radius: 6
                    color: "#20332a"
                    border.color: root.accentGreen
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "样本 " + uploadHistory.count
                        color: "#d9ffe8"
                        font.pixelSize: 13
                        font.bold: true
                    }
                }

                Rectangle {
                    width: 86
                    height: 26
                    radius: 6
                    color: statsBackHomeMouse.pressed ? "#26323a" : "#22272b"
                    border.color: "#3c444a"
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "返回首页"
                        color: "#eef3f4"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    MouseArea {
                        id: statsBackHomeMouse
                        anchors.fill: parent

                        onClicked: {
                            root.switchPage("home")
                        }
                    }
                }
            }
        }

        /* statsEmptyText 在没有检测历史时给出明确空状态，避免用户误以为图表未加载。 */
        Text {
            id: statsEmptyText
            anchors.centerIn: parent
            text: "暂无统计数据\n点击首页“检测”后会生成双模型历史与云端上传统计"
            color: "#aeb9bf"
            font.pixelSize: 18
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            lineHeight: 1.35
            visible: uploadHistory.count <= 0
        }

        /* statsContent 承载所有统计卡片；有历史记录时才显示。 */
        Item {
            id: statsContent
            x: 16
            y: 54
            width: parent.width - 32
            height: parent.height - 70
            visible: uploadHistory.count > 0

            /* statsKpiGrid 显示总量、良率、上传成功率和文件规模等核心指标。 */
            Grid {
                id: statsKpiGrid
                x: 0
                y: 0
                width: parent.width
                height: 74
                columns: 5
                rowSpacing: 0
                columnSpacing: 10

                Repeater {
                    model: [
                        {"name": "总记录", "value": root.statsSummary().total, "note": "本地历史", "color": "#f0f4f5"},
                        {"name": "良品", "value": root.statsSummary().good, "note": root.percentText(root.statsSummary().good, root.statsSummary().total), "color": root.accentGreen},
                        {"name": "坏品", "value": root.statsSummary().bad, "note": root.percentText(root.statsSummary().bad, root.statsSummary().total), "color": root.accentRed},
                        {"name": "上传成功率", "value": root.percentText(root.statsSummary().uploadSuccess, root.statsSummary().total), "note": root.statsSummary().uploadSuccess + "/" + root.statsSummary().total, "color": "#5aa7ff"},
                        {"name": "图片总量", "value": root.statsSummary().imageCount, "note": root.formatBytes(root.statsSummary().totalBytes), "color": "#f0f4f5"}
                    ]

                    Rectangle {
                        width: (statsKpiGrid.width - statsKpiGrid.columnSpacing * 4) / 5
                        height: 74
                        radius: 8
                        color: root.panelColor
                        border.color: root.borderColor
                        border.width: 1

                        Text {
                            x: 12
                            y: 9
                            text: modelData.name
                            color: "#8f9aa1"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        Text {
                            x: 12
                            y: 30
                            width: parent.width - 24
                            text: modelData.value
                            color: modelData.color
                            font.pixelSize: 21
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        Text {
                            x: 12
                            y: 56
                            width: parent.width - 24
                            text: modelData.note
                            color: "#aab4ba"
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            /* statsTrendPanel 用轻量柱状图展示最近检测记录，颜色区分上传成功和待排查。 */
            Rectangle {
                id: statsTrendPanel
                x: 0
                y: 84
                width: 500
                height: 160
                radius: 8
                color: root.panelColor
                border.color: root.borderColor
                border.width: 1
                clip: true

                Text {
                    x: 14
                    y: 12
                    text: "最近检测趋势"
                    color: "#f1f4f5"
                    font.pixelSize: 17
                    font.bold: true
                }

                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    y: 15
                    text: "每柱=一次记录"
                    color: "#89949b"
                    font.pixelSize: 12
                }

                Row {
                    id: statsRecentBarRow
                    x: 18
                    y: 42
                    width: parent.width - 36
                    height: 78
                    spacing: 14
                    layoutDirection: Qt.LeftToRight

                    Repeater {
                        model: root.statsRecentBars()

                        Item {
                            width: (statsRecentBarRow.width - statsRecentBarRow.spacing * 7) / 8
                            height: statsRecentBarRow.height

                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.bottom: parent.bottom
                                width: 24
                                height: Math.max(18, Math.min(parent.height - 22, modelData.value * 28))
                                radius: 4
                                color: modelData.color
                                opacity: 0.9
                            }

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: -16
                                text: modelData.label
                                color: "#aab4ba"
                                font.pixelSize: 10
                            }

                            MouseArea {
                                anchors.fill: parent

                                onClicked: {
                                    root.openHistoryDetailFromStats(modelData.index)
                                }
                            }
                        }
                    }
                }

                Text {
                    x: 18
                    y: 140
                    text: "绿色代表已登记云端，黄色代表本地检测后仍需排查上传链路"
                    color: "#7f898f"
                    font.pixelSize: 12
                }
            }

            /* statsDistributionPanel 用水平条形图展示结果和上传状态分布。 */
            Rectangle {
                id: statsDistributionPanel
                x: 514
                y: 84
                width: parent.width - 514
                height: 160
                radius: 8
                color: root.panelColor
                border.color: root.borderColor
                border.width: 1

                Text {
                    x: 14
                    y: 12
                    text: "分布概览"
                    color: "#f1f4f5"
                    font.pixelSize: 17
                    font.bold: true
                }

                /* statsDistributionSplitRow 把五条分布数据拆成左右两列，避免上传失败条在小屏面板底部越界。 */
                Row {
                    id: statsDistributionSplitRow
                    x: 14
                    y: 34
                    width: parent.width - 28
                    height: parent.height - 46
                    spacing: 16

                    /* statsDistributionLeftColumn 显示良品、坏品、待复核三类检测结果分布。 */
                    Column {
                        id: statsDistributionLeftColumn
                        width: (statsDistributionSplitRow.width - statsDistributionSplitRow.spacing) / 2
                        height: parent.height
                        spacing: 6

                        Repeater {
                            model: root.statsDistributionLeftBars()
                            delegate: statsDistributionBarDelegate
                        }
                    }

                    /* statsDistributionRightColumn 显示上传成功和上传失败两类云端链路分布。 */
                    Column {
                        id: statsDistributionRightColumn
                        width: (statsDistributionSplitRow.width - statsDistributionSplitRow.spacing) / 2
                        height: parent.height
                        spacing: 6

                        Repeater {
                            model: root.statsDistributionRightBars()
                            delegate: statsDistributionBarDelegate
                        }
                    }
                }

                /* statsDistributionBarDelegate 是左右两列共用的单条分布条，保证名称、数值和进度条样式一致。 */
                Component {
                    id: statsDistributionBarDelegate

                    Column {
                        width: parent.width
                        height: 34
                        spacing: 4

                        Row {
                            width: parent.width
                            height: 14

                            Text {
                                width: 58
                                text: modelData.name
                                color: "#cfd7db"
                                font.pixelSize: 11
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            Text {
                                width: parent.width - 58
                                text: modelData.value + "  " + modelData.percent
                                color: "#aab4ba"
                                font.pixelSize: 11
                                horizontalAlignment: Text.AlignRight
                                elide: Text.ElideLeft
                            }
                        }

                        Rectangle {
                            width: parent.width
                            height: 8
                            radius: 4
                            color: "#252c31"

                            Rectangle {
                                width: parent.width * (modelData.total > 0 ? modelData.value / modelData.total : 0)
                                height: parent.height
                                radius: 4
                                color: modelData.color
                            }
                        }
                    }
                }
            }

            /* statsRecentPanel 显示最近记录列表，并允许在卡片内竖向滑动查看更多记录。 */
            Rectangle {
                id: statsRecentPanel
                x: 0
                y: 258
                width: 500
                height: parent.height - 258
                radius: 8
                color: root.panelColor
                border.color: root.borderColor
                border.width: 1
                clip: true

                Text {
                    x: 14
                    y: 10
                    text: "最近记录"
                    color: "#f1f4f5"
                    font.pixelSize: 17
                    font.bold: true
                }

                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    y: 14
                    text: "共 " + uploadHistory.count + " 条 · 上下滑动查看更多"
                    color: "#89949b"
                    font.pixelSize: 11
                }

                ListView {
                    id: statsRecentListView
                    x: 14
                    y: 38
                    width: parent.width - 28
                    height: parent.height - 48
                    clip: true
                    model: root.statsRecentRows()
                    spacing: 4
                    boundsBehavior: Flickable.StopAtBounds
                    maximumFlickVelocity: 900
                    flickDeceleration: 2200

                    delegate: Rectangle {
                        width: statsRecentListView.width
                        height: 22
                        radius: 6
                        color: statsRecentRowMouse.pressed ? "#283039" : "#20262a"
                        border.color: modelData.statusOk ? "#315f47" : "#5c4930"
                        border.width: 1

                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 8

                            Text {
                                width: 58
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.time
                                color: "#dce3e6"
                                font.pixelSize: 11
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            Text {
                                width: 50
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.result
                                color: modelData.result === "良品" ? root.accentGreen
                                      : modelData.result === "坏品" ? root.accentRed
                                      : root.accentAmber
                                font.pixelSize: 11
                                font.bold: true
                            }

                            Text {
                                width: 148
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.recordNo
                                color: "#eef3f4"
                                font.pixelSize: 11
                                elide: Text.ElideMiddle
                            }

                            Text {
                                width: 42
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.images + "图"
                                color: "#aab4ba"
                                font.pixelSize: 11
                            }

                            Text {
                                width: parent.width - 330
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.status
                                color: modelData.statusOk ? "#d9ffe8" : "#ffdca8"
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            id: statsRecentRowMouse
                            anchors.fill: parent

                            onClicked: {
                                root.openHistoryDetailFromStats(modelData.index)
                            }
                        }
                    }
                }
            }

            /* statsCloudPanel 汇总云端、COS 和 SD 卡文件状态，便于现场快速判断链路是否健康。 */
            Rectangle {
                id: statsCloudPanel
                x: 514
                y: 258
                width: parent.width - 514
                height: parent.height - 258
                radius: 8
                color: root.panelColor
                border.color: root.borderColor
                border.width: 1

                Text {
                    x: 14
                    y: 10
                    text: "云端与文件状态"
                    color: "#f1f4f5"
                    font.pixelSize: 17
                    font.bold: true
                }

                Column {
                    x: 14
                    y: 38
                    width: parent.width - 28
                    spacing: 3

                    Repeater {
                        model: [
                            {"name": "最近时间", "value": root.statsSummary().latestTime, "color": "#edf2f3"},
                            {"name": "最近编号", "value": root.statsSummary().latestRecordNo, "color": "#edf2f3"},
                            {"name": "云端状态", "value": root.statsSummary().latestStatus, "color": root.statsSummary().uploadFailed > 0 ? root.accentAmber : root.accentGreen},
                            {"name": "平均文件", "value": root.formatBytes(root.statsSummary().totalBytes / Math.max(1, root.statsSummary().imageCount)), "color": "#edf2f3"},
                            {"name": "最大批次", "value": root.formatBytes(root.statsSummary().maxBytes), "color": "#edf2f3"}
                        ]

                        Row {
                            width: parent.width
                            height: 16

                            Text {
                                width: 72
                                text: modelData.name
                                color: "#8f9aa1"
                                font.pixelSize: 11
                                font.bold: true
                            }

                            Text {
                                width: parent.width - 72
                                text: modelData.value
                                color: modelData.color
                                font.pixelSize: 11
                                font.bold: true
                                elide: Text.ElideMiddle
                            }
                        }
                    }
                }

                Rectangle {
                    x: 14
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 14
                    width: parent.width - 28
                    height: 30
                    radius: 6
                    color: root.statsSummary().uploadFailed > 0 ? "#3a301a" : "#173524"
                    border.color: root.statsSummary().uploadFailed > 0 ? root.accentAmber : root.accentGreen
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: root.statsSummary().uploadFailed > 0
                              ? "存在 " + root.statsSummary().uploadFailed + " 条上传待排查"
                              : "检测与上传记录正常"
                        color: "#ffffff"
                        font.pixelSize: 13
                        font.bold: true
                    }
                }
            }
        }
    }

    /* manualPage 是调试级手动控制页面：第一版只做安全交互、模拟状态和命令日志，不绕过 F4 直接控制硬件。 */
    Rectangle {
        id: manualPage
        x: 176
        y: 72
        width: root.width - 192
        height: root.height - 88
        radius: 8
        color: "#171a1d"
        border.color: root.borderColor
        border.width: 1
        visible: root.manualPageVisible
        clip: true

        /* manualHeader 显示页面标题、模式状态、最近 ACK 和返回首页入口。 */
        Rectangle {
            id: manualHeader
            x: 16
            y: 12
            width: parent.width - 32
            height: 38
            color: "transparent"

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "手动控制"
                color: "#f1f4f5"
                font.pixelSize: 22
                font.bold: true
            }

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 116
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                Repeater {
                    model: [
                        {"name": "模式", "value": root.manualMode ? "手动" : "自动", "color": root.manualMode ? root.accentAmber : root.accentGreen},
                        {"name": "最近ACK", "value": root.manualLastAckText, "color": root.manualLastAckText.indexOf("禁止") >= 0 ? root.accentRed : "#dce3e6"}
                    ]

                    Rectangle {
                        width: modelData.name === "模式" ? 92 : 300
                        height: 26
                        radius: 6
                        color: "#20262a"
                        border.color: modelData.color
                        border.width: 1

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name + "：" + modelData.value
                            color: "#eef3f4"
                            font.pixelSize: 12
                            font.bold: true
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Rectangle {
                width: 86
                height: 26
                radius: 6
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                color: manualBackHomeMouse.pressed ? "#26323a" : "#1f262b"
                border.color: "#5aa7ff"
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "返回首页"
                    color: "#eef6ff"
                    font.pixelSize: 13
                    font.bold: true
                }

                MouseArea {
                    id: manualBackHomeMouse
                    anchors.fill: parent

                    onClicked: {
                        root.switchPage("home")
                    }
                }
            }
        }

        /* manualBeltPanel 负责传送带点动、停止和速度档位模拟，真实 PWM 仍由 F4 负责。 */
        Rectangle {
            id: manualBeltPanel
            x: 16
            y: 62
            width: 250
            height: 164
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1

            Text {
                x: 14
                y: 10
                text: "传送带"
                color: "#f1f4f5"
                font.pixelSize: 18
                font.bold: true
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 14
                y: 13
                text: root.manualBeltState + " / " + root.manualBeltSpeed
                color: root.manualBeltState === "停止" ? root.accentGreen : root.accentAmber
                font.pixelSize: 12
                font.bold: true
            }

            Grid {
                id: manualBeltButtonGrid
                x: 14
                y: 42
                width: parent.width - 28
                columns: 2
                rowSpacing: 8
                columnSpacing: 8

                Repeater {
                    model: [
                        {"text": "进入手动", "action": "enter-manual", "color": root.manualMode ? root.accentAmber : root.accentGreen},
                        {"text": "停止", "action": "stop", "color": root.accentRed},
                        {"text": "正向点动", "action": "belt-forward", "color": root.accentGreen},
                        {"text": "反向点动", "action": "belt-reverse", "color": "#5aa7ff"}
                    ]

                    Rectangle {
                        property bool actionAllowed: root.manualActionAllowed(modelData.action)

                        width: (manualBeltButtonGrid.width - manualBeltButtonGrid.columnSpacing) / 2
                        height: 34
                        radius: 6
                        color: actionAllowed ? (manualBeltMouse.pressed ? "#2d3338" : "#22272b") : "#171b1e"
                        border.color: actionAllowed ? modelData.color : "#3a4147"
                        border.width: 1
                        opacity: actionAllowed ? 1.0 : 0.48

                        Text {
                            anchors.centerIn: parent
                            text: modelData.text
                            color: "#ffffff"
                            font.pixelSize: 13
                            font.bold: true
                        }

                        MouseArea {
                            id: manualBeltMouse
                            anchors.fill: parent
                            enabled: parent.actionAllowed

                            onClicked: {
                                root.handleManualAction(modelData.action, modelData.text)
                            }
                        }
                    }
                }
            }

            Row {
                x: 14
                y: 128
                spacing: 8

                Repeater {
                    model: [
                        {"text": "低", "action": "belt-speed-low", "value": "低速"},
                        {"text": "中", "action": "belt-speed-mid", "value": "中速"},
                        {"text": "高", "action": "belt-speed-high", "value": "高速"}
                    ]

                    Rectangle {
                        property bool actionAllowed: root.manualActionAllowed(modelData.action)
                        property bool selectedSpeed: root.manualBeltSpeed === modelData.value

                        width: 68
                        height: 26
                        radius: 6
                        color: selectedSpeed ? "#24483a" : (actionAllowed ? "#22272b" : "#171b1e")
                        border.color: selectedSpeed ? root.accentGreen : (actionAllowed ? "#3a444b" : "#30363b")
                        border.width: 1
                        opacity: actionAllowed ? 1.0 : 0.48

                        Text {
                            anchors.centerIn: parent
                            text: modelData.text
                            color: "#eef3f4"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: parent.actionAllowed

                            onClicked: {
                                root.handleManualAction(modelData.action, "速度" + modelData.text)
                            }
                        }
                    }
                }
            }
        }

        /* manualArmPanel 负责机械臂回零、待机、夹取和分拣位置模拟，夹取类动作受回零状态保护。 */
        Rectangle {
            id: manualArmPanel
            x: 282
            y: 62
            width: 294
            height: 164
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1

            Text {
                x: 14
                y: 10
                text: "机械臂"
                color: "#f1f4f5"
                font.pixelSize: 18
                font.bold: true
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 14
                y: 13
                text: root.manualArmState + " / " + root.manualActuatorState
                color: root.manualArmHomeOk ? root.accentGreen : root.accentAmber
                font.pixelSize: 12
                font.bold: true
            }

            Grid {
                id: manualArmButtonGrid
                x: 14
                y: 42
                width: parent.width - 28
                columns: 3
                rowSpacing: 8
                columnSpacing: 8

                Repeater {
                    model: [
                        {"text": "回零", "action": "arm-home", "color": root.accentGreen},
                        {"text": "待机位", "action": "arm-standby", "color": "#5aa7ff"},
                        {"text": "抓取测试", "action": "arm-pick", "color": root.accentAmber},
                        {"text": "放良品", "action": "arm-good", "color": root.accentGreen},
                        {"text": "放坏品", "action": "arm-bad", "color": root.accentRed},
                        {"text": "夹爪关", "action": "actuator-on", "color": root.accentAmber},
                        {"text": "夹爪开", "action": "actuator-off", "color": "#9aa6ad"}
                    ]

                    Rectangle {
                        property bool actionAllowed: root.manualActionAllowed(modelData.action)

                        width: (manualArmButtonGrid.width - manualArmButtonGrid.columnSpacing * 2) / 3
                        height: 34
                        radius: 6
                        color: actionAllowed ? (manualArmMouse.pressed ? "#2d3338" : "#22272b") : "#171b1e"
                        border.color: actionAllowed ? modelData.color : "#3a4147"
                        border.width: 1
                        opacity: actionAllowed ? 1.0 : 0.48

                        Text {
                            anchors.centerIn: parent
                            text: modelData.text
                            color: "#ffffff"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: manualArmMouse
                            anchors.fill: parent
                            enabled: parent.actionAllowed

                            onClicked: {
                                root.handleManualAction(modelData.action, modelData.text)
                            }
                        }
                    }
                }
            }
        }

        /* manualLightPanel 负责显示背光常亮状态、补光和检测辅助动作，当前帧检测复用首页双模型链路。 */
        Rectangle {
            id: manualLightPanel
            x: 592
            y: 62
            width: 224
            height: 164
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1

            Text {
                x: 14
                y: 10
                text: "光源与辅助"
                color: "#f1f4f5"
                font.pixelSize: 18
                font.bold: true
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 14
                y: 13
                text: "背光常亮 / 亮度 " + root.manualLightLevel
                color: root.accentGreen
                font.pixelSize: 12
                font.bold: true
            }

            Grid {
                id: manualLightButtonGrid
                x: 14
                y: 42
                width: parent.width - 28
                columns: 2
                rowSpacing: 8
                columnSpacing: 8

                Repeater {
                    model: [
                        {"text": root.manualTopLightEnabled ? "补光关" : "补光开", "action": "toplight-toggle", "color": "#5aa7ff"},
                        {"text": "亮度低", "action": "light-low", "color": "#9aa6ad"},
                        {"text": "亮度中", "action": "light-mid", "color": root.accentAmber},
                        {"text": "亮度高", "action": "light-high", "color": root.accentGreen},
                        {"text": "检测当前帧", "action": "detect-frame", "color": root.accentGreen},
                        {"text": "背光常亮", "action": "refresh", "color": root.accentGreen}
                    ]

                    Rectangle {
                        property bool actionAllowed: root.manualActionAllowed(modelData.action)

                        width: (manualLightButtonGrid.width - manualLightButtonGrid.columnSpacing) / 2
                        height: 30
                        radius: 6
                        color: actionAllowed ? (manualLightMouse.pressed ? "#2d3338" : "#22272b") : "#171b1e"
                        border.color: actionAllowed ? modelData.color : "#3a4147"
                        border.width: 1
                        opacity: actionAllowed ? 1.0 : 0.48

                        Text {
                            anchors.centerIn: parent
                            text: modelData.text
                            color: "#ffffff"
                            font.pixelSize: 11
                            font.bold: true
                        }

                        MouseArea {
                            id: manualLightMouse
                            anchors.fill: parent
                            enabled: parent.actionAllowed

                            onClicked: {
                                root.handleManualAction(modelData.action, modelData.text)
                            }
                        }
                    }
                }
            }
        }

        /* manualSafetyPanel 集中展示安全状态和维护动作，提示 MP157 不直接承担实时安全闭环。 */
        Rectangle {
            id: manualSafetyPanel
            x: 16
            y: 242
            width: 300
            height: 250
            radius: 8
            color: root.panelColor
            border.color: root.manualEmergencyStop ? root.accentRed : root.borderColor
            border.width: 1

            Text {
                x: 14
                y: 10
                text: "安全状态"
                color: "#f1f4f5"
                font.pixelSize: 18
                font.bold: true
            }

            Column {
                x: 14
                y: 42
                width: parent.width - 28
                spacing: 8

                Repeater {
                    model: [
                        {"name": "F4控制器", "value": deviceHealth.f4StatusText, "color": deviceHealth.f4StatusColor},
                        {"name": "手动模式", "value": root.manualMode ? "允许" : "未进入", "color": root.manualMode ? root.accentAmber : root.accentGreen},
                        {"name": "急停", "value": root.manualEmergencyStop ? "已按下" : "释放", "color": root.manualEmergencyStop ? root.accentRed : root.accentGreen},
                        {"name": "限位", "value": "未触发", "color": root.accentGreen},
                        {"name": "机械臂回零", "value": root.manualArmHomeOk ? "完成" : "未完成", "color": root.manualArmHomeOk ? root.accentGreen : root.accentAmber},
                        {"name": "复核标记", "value": root.manualReviewMark, "color": "#dce3e6"}
                    ]

                    Row {
                        width: parent.width
                        height: 20
                        spacing: 8

                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            anchors.verticalCenter: parent.verticalCenter
                            color: modelData.color
                        }

                        Text {
                            width: 86
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name
                            color: "#9aa5ab"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        Text {
                            width: parent.width - 110
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.value
                            color: modelData.color
                            font.pixelSize: 12
                            font.bold: true
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Row {
                x: 14
                y: 190
                spacing: 8

                Repeater {
                    model: [
                        {"text": root.manualEmergencyStop ? "释放急停" : "模拟急停", "action": "emergency-toggle", "color": root.accentRed},
                        {"text": "清故障", "action": "clear-alarm", "color": root.accentAmber},
                        {"text": "刷新", "action": "refresh", "color": "#5aa7ff"}
                    ]

                    Rectangle {
                        width: 84
                        height: 34
                        radius: 6
                        color: manualSafetyMouse.pressed ? "#2d3338" : "#22272b"
                        border.color: modelData.color
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: modelData.text
                            color: "#ffffff"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: manualSafetyMouse
                            anchors.fill: parent

                            onClicked: {
                                root.handleManualAction(modelData.action, modelData.text)
                            }
                        }
                    }
                }
            }
        }

        /* manualReviewPanel 是检测辅助标记区，第一版只保存在界面状态中，不写入云端或历史记录。 */
        Rectangle {
            id: manualReviewPanel
            x: 332
            y: 242
            width: 244
            height: 118
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1

            Text {
                x: 14
                y: 10
                text: "人工复核"
                color: "#f1f4f5"
                font.pixelSize: 18
                font.bold: true
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 14
                y: 13
                text: root.manualReviewMark
                color: root.manualReviewMark === "BAD" ? root.accentRed : (root.manualReviewMark === "GOOD" ? root.accentGreen : root.accentAmber)
                font.pixelSize: 12
                font.bold: true
            }

            Row {
                x: 14
                y: 54
                spacing: 8

                Repeater {
                    model: [
                        {"text": "GOOD", "action": "mark-good", "color": root.accentGreen},
                        {"text": "BAD", "action": "mark-bad", "color": root.accentRed},
                        {"text": "待复核", "action": "mark-uncertain", "color": root.accentAmber}
                    ]

                    Rectangle {
                        width: 68
                        height: 36
                        radius: 6
                        color: manualReviewMouse.pressed ? "#2d3338" : "#22272b"
                        border.color: modelData.color
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: modelData.text
                            color: "#ffffff"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: manualReviewMouse
                            anchors.fill: parent

                            onClicked: {
                                root.handleManualAction(modelData.action, modelData.text)
                            }
                        }
                    }
                }
            }
        }

        /* manualCommandLogView 显示最近手动命令，最新命令在最上方，便于调试时确认触摸和联锁逻辑。 */
        Rectangle {
            id: manualLogPanel
            x: 332
            y: 374
            width: 484
            height: 118
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1
            clip: true

            Text {
                x: 14
                y: 10
                text: "命令日志"
                color: "#f1f4f5"
                font.pixelSize: 18
                font.bold: true
            }

            ListView {
                id: manualCommandLogView
                x: 14
                y: 40
                width: parent.width - 28
                height: parent.height - 50
                clip: true
                model: manualCommandLog
                spacing: 4
                boundsBehavior: Flickable.StopAtBounds

                delegate: Rectangle {
                    width: manualCommandLogView.width
                    height: 20
                    radius: 5
                    color: "#20262a"
                    border.color: result.indexOf("禁止") >= 0 || result.indexOf("急停") >= 0 ? "#5c3030" : "#2f3a40"
                    border.width: 1

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8

                        Text {
                            width: 54
                            anchors.verticalCenter: parent.verticalCenter
                            text: time
                            color: "#9aa5ab"
                            font.pixelSize: 10
                            font.bold: true
                        }

                        Text {
                            width: 72
                            anchors.verticalCenter: parent.verticalCenter
                            text: command
                            color: "#eef3f4"
                            font.pixelSize: 10
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        Text {
                            width: 70
                            anchors.verticalCenter: parent.verticalCenter
                            text: target
                            color: "#aeb8be"
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width - 220
                            anchors.verticalCenter: parent.verticalCenter
                            text: result
                            color: result.indexOf("禁止") >= 0 || result.indexOf("急停") >= 0 ? "#ffd6dc" : "#d9ffe8"
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }
    }

    /* settingsPage 是参数设置界面：第一版做现场可调参数和配置摘要，真实持久化/串口同步后续接 C++ 控制器。 */
    Rectangle {
        id: settingsPage
        x: 176
        y: 72
        width: root.width - 192
        height: root.height - 88
        radius: 8
        color: "#171a1d"
        border.color: root.borderColor
        border.width: 1
        visible: root.settingsPageVisible
        clip: true

        /* settingsHeader 显示页面标题、当前摘要和返回首页入口。 */
        Rectangle {
            id: settingsHeader
            x: 16
            y: 12
            width: parent.width - 32
            height: 38
            color: "transparent"

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "参数设置"
                color: "#f1f4f5"
                font.pixelSize: 20
                font.bold: true
            }

            Rectangle {
                x: 108
                anchors.verticalCenter: parent.verticalCenter
                width: 388
                height: 26
                radius: 6
                color: "#20262a"
                border.color: root.accentGreen
                border.width: 1

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: "参数摘要：" + root.settingsSummaryText()
                    color: "#e8f7ee"
                    font.pixelSize: 12
                    font.bold: true
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                width: 86
                height: 26
                radius: 6
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                color: settingsBackHomeMouse.pressed ? "#26323a" : "#1f262b"
                border.color: "#5aa7ff"
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "返回首页"
                    color: "#eef6ff"
                    font.pixelSize: 13
                    font.bold: true
                }

                MouseArea {
                    id: settingsBackHomeMouse
                    anchors.fill: parent

                    onClicked: {
                        root.switchPage("home")
                    }
                }
            }
        }

        /* settingsProcessPanel 负责零件类型、判定阈值和待复核阈值，直接对应检测结果融合策略。 */
        Rectangle {
            id: settingsProcessPanel
            x: 16
            y: 62
            width: 252
            height: 170
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1
            clip: true

            Text {
                x: 14
                y: 10
                text: "工艺与判定"
                color: "#f1f4f5"
                font.pixelSize: 16
                font.bold: true
            }

            Rectangle {
                x: 14
                y: 38
                width: parent.width - 28
                height: 30
                radius: 6
                color: partTypeMouse.pressed ? "#2d3338" : "#22272b"
                border.color: root.accentGreen
                border.width: 1

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: "零件：" + root.settingsPartType
                    color: "#ffffff"
                    font.pixelSize: 13
                    font.bold: true
                }

                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: "切换"
                    color: "#bdebd0"
                    font.pixelSize: 12
                    font.bold: true
                }

                MouseArea {
                    id: partTypeMouse
                    anchors.fill: parent

                    onClicked: {
                        root.settingsApplyAction("part-next")
                    }
                }
            }

            Column {
                x: 14
                y: 78
                width: parent.width - 28
                spacing: 8

                Repeater {
                    model: [
                        {"label": "良坏阈值", "value": root.settingsThresholdText(root.settingsDecisionThreshold), "key": "decision", "step": 10, "note": "判定线"},
                        {"label": "复核阈值", "value": root.settingsThresholdText(root.settingsReviewThreshold), "key": "review", "step": 10, "note": "复核线"}
                    ]

                    Row {
                        width: parent.width
                        height: 34
                        spacing: 6

                        Column {
                            width: 82
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2

                            Text {
                                text: modelData.label
                                color: "#dce3e6"
                                font.pixelSize: 11
                                font.bold: true
                            }

                            Text {
                                text: modelData.note
                                color: "#7f898f"
                                font.pixelSize: 9
                                elide: Text.ElideRight
                            }
                        }

                        Rectangle {
                            width: 56
                            height: 30
                            radius: 6
                            color: "#20262a"
                            border.color: "#3b454b"
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: modelData.value
                                color: "#eef3f4"
                                font.pixelSize: 12
                                font.bold: true
                            }
                        }

                        Rectangle {
                            width: 30
                            height: 30
                            radius: 6
                            color: processMinusMouse.pressed ? "#30363b" : "#22272b"
                            border.color: "#5a6268"
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: "-"
                                color: "#ffffff"
                                font.pixelSize: 16
                                font.bold: true
                            }

                            MouseArea {
                                id: processMinusMouse
                                anchors.fill: parent

                                onClicked: {
                                    root.changeSettingValue(modelData.key, -modelData.step)
                                }
                            }
                        }

                        Rectangle {
                            width: 30
                            height: 30
                            radius: 6
                            color: processPlusMouse.pressed ? "#30363b" : "#22272b"
                            border.color: root.accentGreen
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: "+"
                                color: "#ffffff"
                                font.pixelSize: 16
                                font.bold: true
                            }

                            MouseArea {
                                id: processPlusMouse
                                anchors.fill: parent

                                onClicked: {
                                    root.changeSettingValue(modelData.key, modelData.step)
                                }
                            }
                        }
                    }
                }
            }
        }

        /* settingsVisionPanel 负责视觉居中参数，支撑“先停到中心再检测”的项目核心流程。 */
        Rectangle {
            id: settingsVisionPanel
            x: 284
            y: 62
            width: 252
            height: 170
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1
            clip: true

            Text {
                x: 14
                y: 10
                text: "视觉定位"
                color: "#f1f4f5"
                font.pixelSize: 16
                font.bold: true
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 14
                y: 14
                text: "ROI 固定"
                color: "#9fdcff"
                font.pixelSize: 12
                font.bold: true
            }

            Column {
                x: 14
                y: 38
                width: parent.width - 28
                spacing: 6

                Repeater {
                    model: [
                        {"label": "精定位", "value": root.settingsFineCenterPx + " px", "key": "fine", "step": 1, "hint": "中心容差"},
                        {"label": "稳定帧", "value": root.settingsStableFrames + " 帧", "key": "stable", "step": 1, "hint": "连续帧"},
                        {"label": "脉冲标定", "value": root.settingsPulsePerPx + " p/px", "key": "pulse", "step": 1, "hint": "px->pulse"}
                    ]

                    Row {
                        width: parent.width
                        height: 34
                        spacing: 6

                        Column {
                            width: 78
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2

                            Text {
                                text: modelData.label
                                color: "#dce3e6"
                                font.pixelSize: 11
                                font.bold: true
                            }

                            Text {
                                text: modelData.hint
                                color: "#7f898f"
                                font.pixelSize: 9
                                elide: Text.ElideRight
                            }
                        }

                        Text {
                            width: 60
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.value
                            color: root.accentAmber
                            font.pixelSize: 12
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                        }

                        Rectangle {
                            width: 30
                            height: 30
                            radius: 6
                            color: visionMinusMouse.pressed ? "#30363b" : "#22272b"
                            border.color: "#5a6268"
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: "-"
                                color: "#ffffff"
                                font.pixelSize: 16
                                font.bold: true
                            }

                            MouseArea {
                                id: visionMinusMouse
                                anchors.fill: parent

                                onClicked: {
                                    root.changeSettingValue(modelData.key, -modelData.step)
                                }
                            }
                        }

                        Rectangle {
                            width: 30
                            height: 30
                            radius: 6
                            color: visionPlusMouse.pressed ? "#30363b" : "#22272b"
                            border.color: root.accentGreen
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: "+"
                                color: "#ffffff"
                                font.pixelSize: 16
                                font.bold: true
                            }

                            MouseArea {
                                id: visionPlusMouse
                                anchors.fill: parent

                                onClicked: {
                                    root.changeSettingValue(modelData.key, modelData.step)
                                }
                            }
                        }
                    }
                }
            }
        }

        /* settingsMotionPanel 负责输送与分拣超时参数，后续接入时由 F4 做最终限幅和联锁。 */
        Rectangle {
            id: settingsMotionPanel
            x: 552
            y: 62
            width: 264
            height: 170
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1
            clip: true

            Text {
                x: 14
                y: 10
                text: "输送与分拣"
                color: "#f1f4f5"
                font.pixelSize: 16
                font.bold: true
            }

            Column {
                x: 14
                y: 38
                width: parent.width - 28
                spacing: 8

                Repeater {
                    model: [
                        {"label": "低速档", "value": root.settingsBeltSpeed + " mm/s", "key": "belt", "step": 5, "hint": "居中速度"},
                        {"label": "分拣超时", "value": root.settingsSortTimeoutMs + " ms", "key": "timeout", "step": 100, "hint": "维护告警"}
                    ]

                    Row {
                        width: parent.width
                        height: 36
                        spacing: 6

                        Column {
                            width: 82
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2

                            Text {
                                text: modelData.label
                                color: "#dce3e6"
                                font.pixelSize: 11
                                font.bold: true
                            }

                            Text {
                                text: modelData.hint
                                color: "#7f898f"
                                font.pixelSize: 9
                                elide: Text.ElideRight
                            }
                        }

                        Text {
                            width: 74
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.value
                            color: "#e8f7ee"
                            font.pixelSize: 12
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                        }

                        Rectangle {
                            width: 30
                            height: 30
                            radius: 6
                            color: motionMinusMouse.pressed ? "#30363b" : "#22272b"
                            border.color: "#5a6268"
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: "-"
                                color: "#ffffff"
                                font.pixelSize: 16
                                font.bold: true
                            }

                            MouseArea {
                                id: motionMinusMouse
                                anchors.fill: parent

                                onClicked: {
                                    root.changeSettingValue(modelData.key, -modelData.step)
                                }
                            }
                        }

                        Rectangle {
                            width: 30
                            height: 30
                            radius: 6
                            color: motionPlusMouse.pressed ? "#30363b" : "#22272b"
                            border.color: root.accentGreen
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: "+"
                                color: "#ffffff"
                                font.pixelSize: 16
                                font.bold: true
                            }

                            MouseArea {
                                id: motionPlusMouse
                                anchors.fill: parent

                                onClicked: {
                                    root.changeSettingValue(modelData.key, modelData.step)
                                }
                            }
                        }
                    }
                }
            }

            Text {
                x: 14
                y: 132
                width: parent.width - 28
                text: "F4 负责限幅和联锁"
                color: "#8f9aa1"
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }

        /* settingsStoragePanel 汇总相机、光源、SD 卡和 COS 上传策略，贴合当前已打通的数据路径。 */
        Rectangle {
            id: settingsStoragePanel
            x: 16
            y: 246
            width: 398
            height: 246
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1
            clip: true

            Text {
                x: 14
                y: 10
                text: "相机、光源与存储"
                color: "#f1f4f5"
                font.pixelSize: 16
                font.bold: true
            }

            Column {
                x: 14
                y: 38
                width: parent.width - 28
                spacing: 6

                Repeater {
                    model: [
                        {"name": "相机节点", "value": root.cameraDeviceName, "color": "#eef3f4"},
                        {"name": "采集参数", "value": root.captureWidth + "x" + root.captureHeight + "@" + root.captureFps + "fps", "color": "#eef3f4"},
                        {"name": "视频后端", "value": root.videoBackend, "color": root.usingKmsOverlay ? root.accentGreen : root.accentAmber},
                        {"name": "背光", "value": "常亮", "color": root.accentGreen},
                        {"name": "补光", "value": root.manualLightLevel + " 档", "color": root.accentAmber},
                        {"name": "SD目录", "value": "/mnt/sdcard/images", "color": "#eef3f4"},
                        {"name": "COS上传", "value": root.settingsAutoUpload ? "自动上传" : "本地保存", "color": root.settingsAutoUpload ? root.accentGreen : root.accentAmber}
                    ]

                    Row {
                        width: parent.width
                        height: 17
                        spacing: 8

                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            anchors.verticalCenter: parent.verticalCenter
                            color: modelData.color
                        }

                        Text {
                            width: 66
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name
                            color: "#9aa5ab"
                            font.pixelSize: 11
                            font.bold: true
                        }

                        Text {
                            width: parent.width - 98
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.value
                            color: modelData.color
                            font.pixelSize: 11
                            font.bold: true
                            elide: Text.ElideMiddle
                        }
                    }
                }
            }

            Rectangle {
                x: 14
                y: 204
                width: parent.width - 28
                height: 28
                radius: 6
                color: uploadToggleMouse.pressed ? "#2d3338" : "#22272b"
                border.color: root.settingsAutoUpload ? root.accentGreen : root.accentAmber
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: root.settingsAutoUpload ? "切换为手动上传" : "切换为自动上传"
                    color: "#ffffff"
                    font.pixelSize: 13
                    font.bold: true
                }

                MouseArea {
                    id: uploadToggleMouse
                    anchors.fill: parent

                    onClicked: {
                        root.settingsApplyAction("upload-toggle")
                    }
                }
            }
        }

        /* settingsActionPanel 提供应用、保存、恢复默认和导出摘要操作，是参数页唯一主操作区。 */
        Rectangle {
            id: settingsActionPanel
            x: 430
            y: 246
            width: 386
            height: 246
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1
            clip: true

            Text {
                x: 14
                y: 10
                text: "参数摘要"
                color: "#f1f4f5"
                font.pixelSize: 16
                font.bold: true
            }

            Text {
                x: 14
                y: 38
                width: parent.width - 28
                text: root.settingsSummaryText()
                color: "#dce3e6"
                font.pixelSize: 12
                font.bold: true
                elide: Text.ElideRight
            }

            Rectangle {
                x: 14
                y: 82
                width: parent.width - 28
                height: 34
                radius: 6
                color: "#141719"
                border.color: "#2d343a"
                border.width: 1

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.settingsLastActionText
                    color: "#cfd7db"
                    font.pixelSize: 12
                    font.bold: true
                    elide: Text.ElideRight
                }
            }

            Grid {
                x: 14
                y: 134
                width: parent.width - 28
                columns: 2
                rowSpacing: 8
                columnSpacing: 8

                Repeater {
                    model: [
                        {"text": "应用参数", "action": "apply", "color": root.accentGreen},
                        {"text": "保存配置", "action": "save", "color": "#5aa7ff"},
                        {"text": "恢复默认", "action": "reset", "color": root.accentAmber},
                        {"text": "导出摘要", "action": "export", "color": "#9aa6ad"}
                    ]

                    Rectangle {
                        width: (settingsActionPanel.width - 36) / 2
                        height: 34
                        radius: 6
                        color: settingsActionMouse.pressed ? "#2d3338" : "#22272b"
                        border.color: modelData.color
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: modelData.text
                            color: "#ffffff"
                            font.pixelSize: 13
                            font.bold: true
                        }

                        MouseArea {
                            id: settingsActionMouse
                            anchors.fill: parent

                            onClicked: {
                                root.settingsApplyAction(modelData.action)
                            }
                        }
                    }
                }
            }
        }
    }

    /* alarmPage 是告警维护界面：展示当前告警、设备健康、处理建议和维护日志。 */
    Rectangle {
        id: alarmPage
        x: 176
        y: 72
        width: root.width - 192
        height: root.height - 88
        radius: 8
        color: "#171a1d"
        border.color: root.alarmLevelColor()
        border.width: 1
        visible: root.alarmPageVisible
        clip: true

        /* alarmHeader 显示页面标题、当前告警状态和返回首页入口。 */
        Rectangle {
            id: alarmHeader
            x: 16
            y: 12
            width: parent.width - 32
            height: 38
            color: "transparent"

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "告警维护"
                color: "#f1f4f5"
                font.pixelSize: 20
                font.bold: true
            }

            Rectangle {
                x: 118
                anchors.verticalCenter: parent.verticalCenter
                width: 360
                height: 26
                radius: 6
                color: root.alarmCleared ? "#173524" : "#3a2020"
                border.color: root.alarmLevelColor()
                border.width: 1

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.alarmCurrentCode + " " + root.alarmCurrentTitle + " · " + root.alarmCurrentStatusText()
                    color: "#ffffff"
                    font.pixelSize: 12
                    font.bold: true
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                width: 86
                height: 26
                radius: 6
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                color: alarmBackHomeMouse.pressed ? "#26323a" : "#1f262b"
                border.color: "#5aa7ff"
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "返回首页"
                    color: "#eef6ff"
                    font.pixelSize: 13
                    font.bold: true
                }

                MouseArea {
                    id: alarmBackHomeMouse
                    anchors.fill: parent

                    onClicked: {
                        root.switchPage("home")
                    }
                }
            }
        }

        /* alarmCurrentPanel 用大号状态块显示当前最重要的维护对象。 */
        Rectangle {
            id: alarmCurrentPanel
            x: 16
            y: 62
            width: 386
            height: 194
            radius: 8
            color: root.alarmCleared ? "#17251d" : "#251919"
            border.color: root.alarmLevelColor()
            border.width: 1
            clip: true

            Text {
                x: 14
                y: 10
                text: "当前告警"
                color: "#f1f4f5"
                font.pixelSize: 16
                font.bold: true
            }

            Text {
                x: 14
                y: 42
                text: root.alarmCurrentCode
                color: root.alarmLevelColor()
                font.pixelSize: 24
                font.bold: true
            }

            Text {
                x: 14
                y: 74
                width: 218
                text: root.alarmCurrentTitle
                color: "#ffffff"
                font.pixelSize: 19
                font.bold: true
                elide: Text.ElideRight
            }

            Column {
                x: 14
                y: 108
                width: 218
                spacing: 5

                Repeater {
                    model: [
                        {"name": "等级", "value": root.alarmCurrentLevel, "color": root.alarmLevelColor()},
                        {"name": "发生时间", "value": root.alarmCurrentTime, "color": "#dce3e6"},
                        {"name": "处理状态", "value": root.alarmCurrentStatusText(), "color": root.alarmLevelColor()}
                    ]

                    Row {
                        width: parent.width
                        height: 18

                        Text {
                            width: 64
                            text: modelData.name
                            color: "#9aa5ab"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        Text {
                            width: parent.width - 64
                            text: modelData.value
                            color: modelData.color
                            font.pixelSize: 12
                            font.bold: true
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Column {
                x: parent.width - 128
                y: 42
                width: 114
                spacing: 10

                Repeater {
                    model: [
                        {"text": "确认", "action": "ack", "color": "#5aa7ff"},
                        {"text": "清故障", "action": "clear", "color": root.accentAmber},
                        {"text": "保存诊断", "action": "snapshot", "color": root.accentGreen}
                    ]

                    Rectangle {
                        width: parent.width
                        height: 36
                        radius: 6
                        color: alarmCurrentActionMouse.pressed ? "#2d3338" : "#22272b"
                        border.color: modelData.color
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: modelData.text
                            color: "#ffffff"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: alarmCurrentActionMouse
                            anchors.fill: parent

                            onClicked: {
                                root.handleAlarmAction(modelData.action)
                            }
                        }
                    }
                }
            }
        }

        /* alarmHealthPanel 用矩阵展示设备健康状态，便于判断故障来自相机、F4、SD 卡还是云端。 */
        Rectangle {
            id: alarmHealthPanel
            x: 418
            y: 62
            width: 398
            height: 194
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1
            clip: true

            Text {
                x: 14
                y: 10
                text: "设备健康"
                color: "#f1f4f5"
                font.pixelSize: 16
                font.bold: true
            }

            Grid {
                id: alarmHealthGrid
                x: 14
                y: 38
                width: parent.width - 28
                height: 114
                columns: 3
                rowSpacing: 5
                columnSpacing: 6

                Repeater {
                    model: [
                        {"name": "相机", "value": root.cameraStatusText(), "color": root.cameraDotColor()},
                        {"name": "F4心跳", "value": deviceHealth.f4StatusText, "color": deviceHealth.f4StatusColor},
                        {"name": "机械臂", "value": root.manualArmHomeOk ? "已回零" : "未回零", "color": root.manualArmHomeOk ? root.accentGreen : root.accentAmber},
                        {"name": "急停", "value": root.manualEmergencyStop || !root.alarmCleared ? "需检查" : "释放", "color": root.manualEmergencyStop || !root.alarmCleared ? root.accentRed : root.accentGreen},
                        {"name": "SD卡", "value": deviceHealth.sdcardStatusText, "color": deviceHealth.sdcardStatusColor},
                        {"name": "云端", "value": deviceHealth.cloudStatusText, "color": deviceHealth.cloudStatusColor},
                        {"name": "KMS视频", "value": root.usingKmsOverlay ? deviceHealth.cameraStatusText : "预览/桥接", "color": root.usingKmsOverlay ? deviceHealth.cameraStatusColor : root.accentAmber},
                        {"name": "背光", "value": "常亮", "color": root.accentGreen},
                        {"name": "配置", "value": root.settingsPartType, "color": "#5aa7ff"}
                    ]

                    Rectangle {
                        width: (alarmHealthGrid.width - alarmHealthGrid.columnSpacing * 2) / 3
                        height: 34
                        radius: 6
                        color: "#20262a"
                        border.color: modelData.color
                        border.width: 1

                        Rectangle {
                            x: 8
                            width: 8
                            height: 8
                            radius: 4
                            anchors.verticalCenter: parent.verticalCenter
                            color: modelData.color
                        }

                        Text {
                            x: 22
                            y: 5
                            width: parent.width - 30
                            text: modelData.name
                            color: "#cfd7db"
                            font.pixelSize: 11
                            font.bold: true
                        }

                        Text {
                            x: 22
                            y: 19
                            width: parent.width - 30
                            text: modelData.value
                            color: modelData.color
                            font.pixelSize: 10
                            font.bold: true
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Rectangle {
                x: 14
                y: 154
                width: parent.width - 28
                height: 28
                radius: 6
                color: alarmRefreshMouse.pressed ? "#2d3338" : "#22272b"
                border.color: "#5aa7ff"
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "刷新状态"
                    color: "#ffffff"
                    font.pixelSize: 13
                    font.bold: true
                }

                MouseArea {
                    id: alarmRefreshMouse
                    anchors.fill: parent

                    onClicked: {
                        root.handleAlarmAction("refresh")
                    }
                }
            }
        }

        /* alarmHistoryPanel 显示最近告警和维护动作，最新记录在最上方。 */
        Rectangle {
            id: alarmHistoryPanel
            x: 16
            y: 272
            width: 470
            height: 220
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1
            clip: true

            Text {
                x: 14
                y: 10
                text: "告警历史"
                color: "#f1f4f5"
                font.pixelSize: 16
                font.bold: true
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 14
                y: 14
                text: "最新在上 · 可滑动"
                color: "#89949b"
                font.pixelSize: 11
            }

            ListView {
                id: alarmHistoryListView
                x: 14
                y: 38
                width: parent.width - 28
                height: parent.height - 48
                clip: true
                model: alarmHistoryModel
                spacing: 5
                boundsBehavior: Flickable.StopAtBounds
                maximumFlickVelocity: 900
                flickDeceleration: 2200

                delegate: Rectangle {
                    width: alarmHistoryListView.width
                    height: 28
                    radius: 6
                    color: "#20262a"
                    border.color: level === "严重" ? root.accentRed : (level === "预警" ? root.accentAmber : "#3b454b")
                    border.width: 1

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8

                        Text {
                            width: 54
                            anchors.verticalCenter: parent.verticalCenter
                            text: time
                            color: "#9aa5ab"
                            font.pixelSize: 10
                            font.bold: true
                        }

                        Text {
                            width: 54
                            anchors.verticalCenter: parent.verticalCenter
                            text: code
                            color: "#eef3f4"
                            font.pixelSize: 10
                            font.bold: true
                        }

                        Text {
                            width: 38
                            anchors.verticalCenter: parent.verticalCenter
                            text: level
                            color: level === "严重" ? root.accentRed : (level === "预警" ? root.accentAmber : root.accentGreen)
                            font.pixelSize: 10
                            font.bold: true
                        }

                        Text {
                            width: 150
                            anchors.verticalCenter: parent.verticalCenter
                            text: title
                            color: "#dce3e6"
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width - 338
                            anchors.verticalCenter: parent.verticalCenter
                            text: status
                            color: "#cfd7db"
                            font.pixelSize: 10
                            elide: Text.ElideRight
                            visible: width > 18
                        }
                    }
                }
            }
        }

        /* alarmAdvicePanel 给出故障处理顺序，避免现场只看到故障码却不知道先查哪里。 */
        Rectangle {
            id: alarmAdvicePanel
            x: 502
            y: 272
            width: 314
            height: 220
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1
            clip: true

            Text {
                x: 14
                y: 10
                text: "处理建议"
                color: "#f1f4f5"
                font.pixelSize: 16
                font.bold: true
            }

            Column {
                x: 14
                y: 38
                width: parent.width - 28
                spacing: 6

                Repeater {
                    model: [
                        {"step": "1", "text": "确认急停/安全门"},
                        {"step": "2", "text": "手动页停止并回零"},
                        {"step": "3", "text": "检查相机、SD卡、云端"},
                        {"step": "4", "text": "清故障后单步试运行"}
                    ]

                    Row {
                        width: parent.width
                        height: 26
                        spacing: 8

                        Rectangle {
                            width: 20
                            height: 20
                            radius: 11
                            anchors.verticalCenter: parent.verticalCenter
                            color: "#26323a"
                            border.color: root.accentAmber
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: modelData.step
                                color: "#ffffff"
                                font.pixelSize: 11
                                font.bold: true
                            }
                        }

                        Text {
                            width: parent.width - 30
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.text
                            color: "#dce3e6"
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Text {
                x: 14
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 12
                width: parent.width - 28
                text: "确认不等于解除联锁，最终以 F4 状态为准。"
                color: "#8f9aa1"
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }
    }

    /* 底部统计区：显示节拍、良率、最近记录和操作按钮。 */
    Rectangle {
        id: statsPanel
        x: 176
        y: 418
        width: 832
        height: 166
        radius: 8
        color: root.panelColor
        border.color: root.borderColor
        border.width: 1
        visible: !root.usingKmsOverlay && !root.historyPageVisible && !root.statsPageVisible
                 && !root.manualPageVisible && !root.settingsPageVisible && !root.alarmPageVisible

        Row {
            x: 18
            y: 18
            spacing: 24

            Repeater {
                model: [
                    {"name": "总检测数", "value": root.dailyTotal},
                    {"name": "良品", "value": root.dailyGood},
                    {"name": "坏品", "value": root.dailyBad},
                    {"name": "良率", "value": root.goodRateText()},
                    {"name": "当前节拍", "value": "0.83s"}
                ]

                Column {
                    spacing: 6
                    width: 126

                    Text {
                        text: modelData.name
                        color: "#909aa0"
                        font.pixelSize: 14
                    }

                    Text {
                        text: modelData.value
                        color: modelData.name === "坏品" ? root.accentRed : "#f0f4f5"
                        font.pixelSize: 22
                        font.bold: true
                    }
                }
            }
        }

        Rectangle {
            x: 18
            y: 86
            width: 486
            height: 48
            radius: 8
            color: "#141719"
            border.color: "#2b3034"
            border.width: 1

            Text {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.right: parent.right
                anchors.rightMargin: 16
                text: root.latestHistoryText()
                color: "#d9dee1"
                font.pixelSize: 15
                elide: Text.ElideRight
            }
        }

        Row {
            x: 530
            y: 86
            spacing: 10

            Repeater {
                model: [
                    {"text": "开始", "action": "start", "state": "定位预览", "color": root.accentGreen},
                    {"text": "暂停", "action": "pause", "state": "暂停", "color": root.accentAmber},
                    {"text": "继续", "action": "resume", "state": "继续检测", "color": "#5aa7ff"},
                    {"text": "停止", "action": "stop", "state": "停止", "color": root.accentRed}
                ]

                Rectangle {
                    width: 64
                    height: 48
                    radius: 8
                    color: mouseArea.pressed ? "#2d3338" : "#22272b"
                    border.color: modelData.color
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: modelData.text
                        color: "#ffffff"
                        font.pixelSize: 16
                        font.bold: true
                    }

                    MouseArea {
                        id: mouseArea
                        anchors.fill: parent

                        onClicked: {
                            root.handleControlAction(modelData.action, modelData.state)
                        }
                    }
                }
            }
        }

        Text {
            x: 18
            y: 142
            text: "上传状态：成功    显示路径：Qt Quick / OpenGL ES / " + (Qt.platform.os === "linux" ? "eglfs 或 wayland" : Qt.platform.os)
            color: "#7f898f"
            font.pixelSize: 13
        }
    }

    /* splashOverlay 是最高层级开机启动动画，用于替代内核企鹅和启动空窗。 */
    Rectangle {
        id: splashOverlay
        anchors.fill: parent
        z: 1000
        visible: root.splashOverlayVisible
        opacity: root.splashFadingOut ? 0.0 : 1.0
        color: "#0b0d0f"
        clip: true

        /* opacity 的过渡让启动屏退出时自然淡出，避免直接切到主界面造成闪屏。 */
        Behavior on opacity {
            NumberAnimation {
                duration: 320
                easing.type: Easing.InOutQuad
            }
        }

        /* 启动画面显示期间吸收触摸事件，避免用户在主界面尚未完全呈现时误触按钮。 */
        MouseArea {
            anchors.fill: parent
        }

        /* subtleGrid 使用极低对比度网格暗示工业检测坐标系，不依赖图片资源。 */
        Grid {
            id: subtleGrid
            anchors.centerIn: parent
            width: 880
            height: 420
            columns: 11
            rows: 6
            opacity: 0.16

            Repeater {
                model: 66

                Rectangle {
                    width: subtleGrid.width / subtleGrid.columns
                    height: subtleGrid.height / subtleGrid.rows
                    color: "transparent"
                    border.color: "#263039"
                    border.width: 1
                }
            }
        }

        Text {
            id: splashTitle
            anchors.horizontalCenter: parent.horizontalCenter
            y: 64
            text: "工业缺陷检测系统"
            color: "#f4f7f8"
            font.pixelSize: 34
            font.bold: true
        }

        Text {
            id: splashSubtitle
            anchors.horizontalCenter: parent.horizontalCenter
            y: splashTitle.y + splashTitle.height + 8
            text: "STM32MP157 Vision Inspection Terminal"
            color: "#94a3ad"
            font.pixelSize: 15
        }

        /* splashViewport 模拟相机检测窗口，展示 ROI、扫描线和缺陷框启动自检。 */
        Rectangle {
            id: splashViewport
            anchors.horizontalCenter: parent.horizontalCenter
            y: 158
            width: 430
            height: 232
            radius: 8
            color: "#050606"
            border.color: "#344047"
            border.width: 1
            clip: true

            Rectangle {
                anchors.fill: parent
                anchors.margins: 12
                radius: 6
                color: "transparent"
                border.color: "#20272c"
                border.width: 1
            }

            /* splashScanLine 表示启动时视觉链路正在扫描画面，只移动 y 坐标以降低渲染成本。 */
            Rectangle {
                id: splashScanLine
                x: 26
                y: 24
                width: parent.width - 52
                height: 3
                radius: 2
                color: root.splashStageColor()
                opacity: 0.88

                SequentialAnimation on y {
                    loops: Animation.Infinite
                    running: splashOverlay.visible

                    NumberAnimation {
                        from: 24
                        to: splashViewport.height - 28
                        duration: 1250
                        easing.type: Easing.InOutQuad
                    }

                    NumberAnimation {
                        from: splashViewport.height - 28
                        to: 24
                        duration: 1250
                        easing.type: Easing.InOutQuad
                    }
                }
            }

            Rectangle {
                id: splashRoiBox
                anchors.centerIn: parent
                width: 244
                height: 126
                radius: 4
                color: "transparent"
                border.color: root.splashStageColor()
                border.width: 2
                opacity: 0.78

                SequentialAnimation on opacity {
                    loops: Animation.Infinite
                    running: splashOverlay.visible

                    NumberAnimation {
                        from: 0.56
                        to: 1.0
                        duration: 520
                        easing.type: Easing.InOutQuad
                    }

                    NumberAnimation {
                        from: 1.0
                        to: 0.56
                        duration: 520
                        easing.type: Easing.InOutQuad
                    }
                }
            }

            Rectangle {
                x: splashRoiBox.x + splashRoiBox.width - 72
                y: splashRoiBox.y + 42
                width: 46
                height: 34
                radius: 3
                color: "transparent"
                border.color: root.accentRed
                border.width: 2
                opacity: root.splashStageIndex >= 1 ? 0.78 : 0.0

                Behavior on opacity {
                    NumberAnimation {
                        duration: 220
                        easing.type: Easing.OutQuad
                    }
                }
            }

            Rectangle {
                x: 28
                y: parent.height - 38
                width: 132
                height: 18
                radius: 4
                color: "#101619"
                border.color: "#2f3a40"
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "ROI 320x180"
                    color: "#9aa6ad"
                    font.pixelSize: 10
                }
            }

            Rectangle {
                anchors.right: parent.right
                anchors.rightMargin: 28
                y: parent.height - 38
                width: 106
                height: 18
                radius: 4
                color: "#132119"
                border.color: root.accentGreen
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "SELF CHECK"
                    color: "#d9ffe8"
                    font.pixelSize: 10
                    font.bold: true
                }
            }
        }

        Text {
            id: splashStageText
            anchors.horizontalCenter: parent.horizontalCenter
            y: 420
            text: root.currentSplashStage().label
            color: root.splashStageColor()
            font.pixelSize: 22
            font.bold: true
        }

        Text {
            id: splashStageDetail
            anchors.horizontalCenter: parent.horizontalCenter
            y: splashStageText.y + splashStageText.height + 8
            width: 620
            horizontalAlignment: Text.AlignHCenter
            text: root.currentSplashStage().detail
            color: "#b7c0c6"
            font.pixelSize: 14
            elide: Text.ElideRight
        }

        /* splashProgressTrack 展示真实等待感，避免开机阶段出现黑屏或疑似卡死。 */
        Rectangle {
            id: splashProgressTrack
            anchors.horizontalCenter: parent.horizontalCenter
            y: 486
            width: 520
            height: 12
            radius: 6
            color: "#1a2024"
            border.color: "#303940"
            border.width: 1
            clip: true

            Rectangle {
                id: splashProgressFill
                x: 0
                y: 0
                width: Math.max(8, parent.width * root.splashProgressValue / 100.0)
                height: parent.height
                radius: 6
                color: root.splashStageColor()

                Behavior on width {
                    NumberAnimation {
                        duration: 360
                        easing.type: Easing.OutCubic
                    }
                }
            }
        }

        Text {
            anchors.left: splashProgressTrack.left
            y: splashProgressTrack.y + 22
            text: "启动自检"
            color: "#76828a"
            font.pixelSize: 12
        }

        Text {
            anchors.right: splashProgressTrack.right
            y: splashProgressTrack.y + 22
            text: Math.round(root.splashProgressValue) + "%"
            color: "#dce3e6"
            font.pixelSize: 12
            font.bold: true
        }
    }
}
