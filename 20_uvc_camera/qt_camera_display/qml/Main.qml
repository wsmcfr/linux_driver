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

    /* autoControlBusy 表示首页开始/暂停/继续/停止命令正在等待 F4 二进制 ACK，忙时禁止重复点击。 */
    property bool autoControlBusy: false

    /* autoPendingAction 保存正在等待 ACK 的首页动作，空字符串表示当前没有自动流程命令在途。 */
    property string autoPendingAction: ""

    /* autoPendingStateText 保存正在等待 ACK 的界面目标状态，ACK 成功后再正式写入 workflowState。 */
    property string autoPendingStateText: ""

    /* autoCycleId 保存 MP157 当前自动检测流程号，由 C++ 在 F4 ACK/NACK 回调中返回。 */
    property int autoCycleId: 0

    /* autoWorkflowRunning 表示 MP157 本地认为 F4 自动检测流程正在运行，用于 UI 状态和后续按钮语义。 */
    property bool autoWorkflowRunning: false

    /* autoWorkflowPaused 表示当前自动流程处于暂停状态，停止后不能继续，只能重新开始。 */
    property bool autoWorkflowPaused: false

    /* autoLastAckText 保存最近一次 F4 二进制自动流程 ACK/NACK 文本，便于现场串口调试核对。 */
    property string autoLastAckText: "自动流程待开始"

    /* autoVisionRunning 表示开始 ACK 后 MP157 正在周期读取 overlay LOCATE 并驱动 F4 对中。 */
    property bool autoVisionRunning: false

    /* autoVisionLocateBusy 表示当前 overlay LOCATE 请求尚未返回，避免 100ms 定时器重复创建线程。 */
    property bool autoVisionLocateBusy: false

    /* autoVisionCommandBusy 表示当前 VISION_POS/VISION_LOST/BELT_STOP_CENTERED 仍在等待 F4 ACK。 */
    property bool autoVisionCommandBusy: false

    /* autoVisionStableFrames 保存连续进入中心死区的帧数，达到 3 帧才发送居中停止。 */
    property int autoVisionStableFrames: 0

    /* autoVisionCenteredSent 表示 BELT_STOP_CENTERED 已经下发，防止同一零件重复停机命令。 */
    property bool autoVisionCenteredSent: false

    /* autoVisionHasSeenTarget 表示本轮自动流程已经至少识别到一次零件，用于区分“等待上料”和“定位中短暂漏检”。 */
    property bool autoVisionHasSeenTarget: false

    /* autoVisionLostFrames 保存已经见过目标后连续漏检的 LOCATE 帧数，用于黑色波形零件短暂漏检防抖。 */
    property int autoVisionLostFrames: 0

    /* autoVisionLostHoldFrames 是短暂漏检保持帧数，100ms 定时下 8 帧约等于 0.8 秒。 */
    property int autoVisionLostHoldFrames: 8

    /* autoVisionLostWarnFrames 是连续漏检告警帧数，超过后仍保持停机等待，但提示现场检查光照和零件位置。 */
    property int autoVisionLostWarnFrames: 18

    /* autoVisionLastFrameId 保存最近一次 LOCATE 帧号，用于居中停止和现场日志对齐。 */
    property int autoVisionLastFrameId: 0

    /* autoVisionLastLostMs 保存最近一次 VISION_LOST 下发时间戳，避免目标未入画时每 100ms 刷屏。 */
    property real autoVisionLastLostMs: 0

    /* autoVisionLastErrorY 保存最近一次真实识别到目标时的 Y 轴偏差，漏检提示会显示它帮助现场判断。 */
    property int autoVisionLastErrorY: 0

    /* autoVisionLastText 保存自动视觉闭环最近一次可读状态，底部提示和调试日志会复用它。 */
    property string autoVisionLastText: "视觉闭环待开始"

    /* autoVisionLocatePurpose 标记当前 LOCATE 用途：center 用于传送带前后居中，fine-tune 用于 Z 轴下降后的前后/左右复查。 */
    property string autoVisionLocatePurpose: "center"

    /* autoVisionActuatorPhase 保存正在等待 F4 到位事件或等待稳定的执行器阶段，空字符串表示当前没有自动执行器动作。 */
    property string autoVisionActuatorPhase: ""

    /* autoVisionFineTuneAttempts 保存 Z 轴下降后已经尝试的前后/左右微调次数，避免定位抖动导致无限移动。 */
    property int autoVisionFineTuneAttempts: 0

    /* autoVisionFineTuneMaxAttempts 是前后/左右微调次数上限，超过后进入模型检测并把结果交给人工复核。 */
    property int autoVisionFineTuneMaxAttempts: 6

    /* autoVisionFineTuneTolerancePx 是 Z 轴下降后 ROI 复查的中心死区，默认复用视觉居中死区。 */
    property int autoVisionFineTuneTolerancePx: 24

    /* autoVisionFineTuneStepScalePx 是微调步数放大比例：误差每超出死区约 4px，就在 minStep 基础上多走一档。 */
    property int autoVisionFineTuneStepScalePx: 4

    /* autoVisionFineTuneMaxStepMultiplier 限制单次微调最多放大到 minStep 的 12 倍，兼顾现场可见动作和防止一次过冲。 */
    property int autoVisionFineTuneMaxStepMultiplier: 12

    /* autoVisionFineTuneProgressDeadbandPx 是判断“误差是否没有改善”的像素死区，避免 1~2px 抖动误触发加档。 */
    property int autoVisionFineTuneProgressDeadbandPx: 2

    /* autoVisionFineTuneLastAxis 记录上一次微调的轴名，conveyor 表示 Y/传送带，lateral 表示 X/左右轴。 */
    property string autoVisionFineTuneLastAxis: ""

    /* autoVisionFineTuneLastAbsError 记录上一次同轴微调后的绝对误差，用于判断当前位置有没有真的被拉回中心。 */
    property int autoVisionFineTuneLastAbsError: 0

    /* autoVisionFineTuneNoImproveCount 记录同一轴连续没有改善的次数，连续无变化时自动把下一次步数再放大。 */
    property int autoVisionFineTuneNoImproveCount: 0

    /* autoVisionLateralReturnOffsetSteps 记录本轮自动检测中左右轴相对皮带基准的累计偏移，正值表示相机向右动过。 */
    property int autoVisionLateralReturnOffsetSteps: 0

    /* autoVisionPendingLateralFineTuneSteps 暂存已经下发、但还没有收到完成回执的左右轴微调步数。 */
    property int autoVisionPendingLateralFineTuneSteps: 0

    /* autoVisionPendingLateralFineTuneDirection 暂存已经下发、但还没有收到完成回执的左右轴微调方向。 */
    property int autoVisionPendingLateralFineTuneDirection: 0

    /* autoVisionZFocusSettleMs 是上下轴下降后的对焦稳定等待时间，单位 ms，现场经验约 3 秒。 */
    property int autoVisionZFocusSettleMs: 3000

    /* autoVisionZMoveStepsPerRev 是 MP157 用来估算 Z 轴本地保护超时的每圈步数；正常完成以 F4 ACTUATOR_MOVE_DONE 事件为准。 */
    property int autoVisionZMoveStepsPerRev: 200

    /* autoVisionZMotionSafetyMs 是 Z 轴估算运动时间之外的安全余量，用于覆盖 F4 转发、驱动器加减速和机构惯性。 */
    property int autoVisionZMotionSafetyMs: 900

    /* autoVisionZMotionMinimumWaitMs 是 Z 轴 ACK 后最短物理等待时间，避免小步数或配置异常时立刻进入 ROI 复查。 */
    property int autoVisionZMotionMinimumWaitMs: 1200

    /* autoVisionZMotionMaximumWaitMs 是 Z 轴缺省最长等待时间；实际自动流程优先读取参数页 zMotionTimeoutMs。 */
    property int autoVisionZMotionMaximumWaitMs: 10000

    /* autoVisionPostFocusDetectDelayMs 是已经完成 Z 轴对焦等待后的短检测延时，给 overlay 刷新一帧。 */
    property int autoVisionPostFocusDetectDelayMs: 300

    /* autoVisionFallbackSpeedRpm 是参数缺失或被设为 0 时的自动流程兜底速度；正常情况直接使用参数页速度。 */
    property int autoVisionFallbackSpeedRpm: 40

    /* autoVisionDefaultDetectDelayMs 是没有执行 Z 轴下探时保留的默认静止检测延时。 */
    property int autoVisionDefaultDetectDelayMs: 2000

    /* autoVisionShortSettleMs 是跳过下探或短步微调后的短机械稳定等待时间，单位 ms。 */
    property int autoVisionShortSettleMs: 450

    /* autoVisionZFocusSettled 表示本轮已经完成 Z 轴下降后的 3 秒对焦等待。 */
    property bool autoVisionZFocusSettled: false

    /* autoVisionNeedsZUp 表示本轮自动检测已经执行过 Z 轴下探，模型检测结束后必须请求回升。 */
    property bool autoVisionNeedsZUp: false

    /* autoVisionDetectFromZFlow 表示当前检测由“居中后下探”自动链路触发，检测完成后需要进入 Z 轴回升阶段。 */
    property bool autoVisionDetectFromZFlow: false

    /* autoVisionPendingZMoveSteps 保存刚下发给 F4 的 Z 轴相对位置步数，ACK 后用于估算物理运动完成时间。 */
    property int autoVisionPendingZMoveSteps: 0

    /* autoVisionPendingZMoveSpeedRpm 保存刚下发给 F4 的 Z 轴速度，ACK 后用于估算物理运动完成时间。 */
    property int autoVisionPendingZMoveSpeedRpm: 0

    /* autoVisionPendingZMoveDirection 保存刚下发给 F4 的 Z 轴方向，0=下降，1=回升，便于日志和等待阶段判断。 */
    property int autoVisionPendingZMoveDirection: 0

    /* autoVisionCenterTolerancePx 是 MP157 侧居中判定死区，必须和 F4 死区保持同量级。 */
    property int autoVisionCenterTolerancePx: 24

    /* autoVisionStableRequiredFrames 是连续居中帧数门槛，过滤单帧误检或运动模糊。 */
    property int autoVisionStableRequiredFrames: 3

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

    /* latestModelResultText 保存最近一次模型 RESULT 行，Z 轴回升后要原样下发给 F4 缓存模型结果。 */
    property string latestModelResultText: ""

    /* latestCompletedUploadResultText 保存完整上传脚本结果，上传失败时也要让 F4 分拣到待复核盘。 */
    property string latestCompletedUploadResultText: ""

    /* latestCompletedCloudResult 保存本轮模型综合结果，上传成功时用于最终分拣，上传失败时仅作为历史追踪。 */
    property string latestCompletedCloudResult: "review"

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

    /* logPageVisible 给日志查看页面做显隐判断；日志页打开时隐藏首页视频层，避免 KMS plane 遮挡列表和弹窗。 */
    property bool logPageVisible: activePage === "logs"

    /* uiHorizontalFlickVelocity 是历史卡片和图片轮播的横向最大滑动速度，数值偏高可以减少“拖不动”的滞涩感。 */
    property int uiHorizontalFlickVelocity: 2400

    /* uiHorizontalFlickDeceleration 是横向滑动松手后的减速度，数值比旧配置低，避免图片轮播刚甩动就突然停下。 */
    property int uiHorizontalFlickDeceleration: 1050

    /* uiVerticalFlickVelocity 是统计、参数、告警和日志等上下滚动区的最大滑动速度，统一后各页面触摸手感一致。 */
    property int uiVerticalFlickVelocity: 2200

    /* uiVerticalFlickDeceleration 是上下滚动区的减速度，降低硬刹车感，同时保持短列表不会滑得过远。 */
    property int uiVerticalFlickDeceleration: 1250

    /* uiCarouselCachePages 表示历史图片轮播额外预缓存的页面数，用来提前准备左右相邻图片，减少边滑边解码。 */
    property int uiCarouselCachePages: 3

    /* uiListCachePages 表示普通列表额外预缓存的屏数，让快速滑动时下一屏委托提前创建，降低短暂停顿。 */
    property int uiListCachePages: 2

    /* manualMode 表示当前是否允许调试级手动动作；当前只保护已接入的传送带和检测辅助动作。 */
    property bool manualMode: false

    /* manualBeltState 保存传送带最近一次真实控制意图和 F4 回执状态。 */
    property string manualBeltState: "停止"

    /* manualBeltCommandText 保存传送带当前使用的 F407 二进制协议语义名，便于现场确认 MP157 没有发送 ASCII 文本或 Emm42 原始帧。 */
    property string manualBeltCommandText: "BELT_MANUAL_STOP"

    /* manualReviewMark 保存人工复核标记，第一版只写界面状态，不回写检测记录。 */
    property string manualReviewMark: "未标记"

    /* manualLastAckText 保存最近一次手动命令响应文字，传送带命令由 F4 串口真实回执更新。 */
    property string manualLastAckText: "F4控制器待接入"

    /* manualPendingF4Command 保存正在等待 F4 回复的手动命令名，空字符串表示当前没有手动串口命令在途。 */
    property string manualPendingF4Command: ""

    /* manualEmergencyStop 表示急停模拟状态；为 true 时禁止除停止、刷新和清故障外的手动动作。 */
    property bool manualEmergencyStop: false

    /* manualMotorPopup 表示三轴手动电机控制弹窗是否打开，marker 用于静态测试确认手动控制不再只有传送带。 */
    property bool manualMotorPopup: false

    /* manualMotorPageIndex 保存三轴手动弹窗当前页，0=传送带，1=摄像头左右，2=摄像头上下。 */
    property int manualMotorPageIndex: 0

    /* manualCameraZZeroKnown 表示 MP157 是否知道上下轴当前零点；只有 ACTUATOR_HOME 成功后才置 true。 */
    property bool manualCameraZZeroKnown: false

    /* manualCameraZOffsetSteps 保存上下轴相对最近一次设零点的本地估算偏移；下降为正，上升为负，单位 step。 */
    property real manualCameraZOffsetSteps: 0

    /* manualPendingActuatorId 保存正在等待回执的手动执行器编号；停止键会清掉它，避免旧 ACK 覆盖停止结果。 */
    property int manualPendingActuatorId: -1

    /* manualPendingZDirection 保存正在等待回执的上下轴方向，0=下降，1=上升；非上下轴命令时为 -1。 */
    property int manualPendingZDirection: -1

    /* manualPendingZSteps 保存正在等待回执的上下轴步数；成功回执后才用于更新本地零点偏移。 */
    property real manualPendingZSteps: 0

    /* manualPendingZReturnHome 表示当前上下轴命令是否由“回原位”触发，成功后偏移直接归零。 */
    property bool manualPendingZReturnHome: false

    /* settingsSupportedPartTypes 保存参数页允许切换的真实零件名称；当前检测链路只按这三类垫圈展示。 */
    property var settingsSupportedPartTypes: ["波形垫圈", "平垫圈", "弹性垫圈"]

    /* settingsConfigPath 保存真实检测配置 JSON 路径，来自 C++ DetectSettingsController。 */
    property string settingsConfigPath: detectSettings.configPath

    /* settingsPartType 保存当前参数页选中的真实零件类型，来自 C++ DetectSettingsController。 */
    property string settingsPartType: detectSettings.partType

    /* settingsDecisionThreshold 保存分类模型判坏阈值的千分比显示值，真实值来自 detectSettings.modelThreshold。 */
    property int settingsDecisionThreshold: Math.round(detectSettings.modelThreshold * 1000)

    /* settingsReviewThreshold 保存低可信复核阈值的千分比显示值，真实值来自 detectSettings.reviewThreshold。 */
    property int settingsReviewThreshold: Math.round(detectSettings.reviewThreshold * 1000)

    /* settingsRoiSize 保存分类和 UNet 共用的中心 ROI 边长，真实值来自 detectSettings.roiSize。 */
    property int settingsRoiSize: detectSettings.roiSize

    /* settingsSegmentMinPixels 保存 UNet 判 NG 的最小缺陷像素数，真实值来自 detectSettings.segmentMinPixels。 */
    property int settingsSegmentMinPixels: detectSettings.segmentMinPixels

    /* settingsOverlayAlpha 保存 UNet overlay 结果图透明度，真实值来自 detectSettings.overlayAlpha。 */
    property real settingsOverlayAlpha: detectSettings.overlayAlpha

    /* settingsUploadEnabled 表示检测完成后是否自动触发 COS 上传，真实值来自 detectSettings.autoUploadEnabled。 */
    property bool settingsUploadEnabled: detectSettings.autoUploadEnabled

    /* settingsF4ArmResultTimeoutMs 保存 MP157 等待 F4 主动机械臂结果帧的最大时间，真实值来自 detectSettings.f4ArmResultTimeoutMs。 */
    property int settingsF4ArmResultTimeoutMs: detectSettings.f4ArmResultTimeoutMs

    /* settingsLastActionText 保存参数页最近一次加载、保存或恢复默认的结果提示。 */
    property string settingsLastActionText: detectSettings.lastStatusText

    /* settingsDetailVisible 表示参数设置页是否打开策略详情浮层，用于承载小卡片放不下的完整说明。 */
    property bool settingsDetailVisible: false

    /* settingsDetailTitle 保存当前参数详情浮层标题，由视觉检测策略或 F4 接入边界入口写入。 */
    property string settingsDetailTitle: ""

    /* settingsDetailText 保存当前参数详情浮层正文，内容来自云端上传契约和当前板端接入边界。 */
    property string settingsDetailText: ""

    /* stepperMotorSettings 保存 C++ DetectSettingsController 暴露的三台步进电机参数，用于三页弹窗显示。 */
    property var stepperMotorSettings: detectSettings.stepperMotorSettings

    /* stepperMotorSettingsRevision 在 C++ 参数变化后递增，强制步进弹窗刷新 QVariantMap 副本。 */
    property int stepperMotorSettingsRevision: 0

    /* stepperMotorPageNames 保存三页固定名称，也作为 QML 资源 marker，便于部署后用 strings 验证。 */
    property var stepperMotorPageNames: ["传送带电机", "摄像头左右电机", "摄像头上下电机"]

    /* stepperMotorPopupVisible 表示步进电机参数弹窗是否打开，避免把完整表单塞进参数页小卡片。 */
    property bool stepperMotorPopupVisible: false

    /* stepperMotorPageIndex 保存步进电机弹窗当前页，0=传送带，1=摄像头左右，2=摄像头上下。 */
    property int stepperMotorPageIndex: 0

    /* stepperMotorResultText 保存最近一次修改提示，提醒用户点击“保存并下发”后同时写 JSON 和通知 F4。 */
    property string stepperMotorResultText: "调整后点击保存并下发；MP157 写入 JSON，并向 F4 下发运行时参数"

    /* stepperSettingsSending 表示步进电机参数正在通过二进制协议下发给 F407，防止重复点击保存。 */
    property bool stepperSettingsSending: false

    /* stepperHomeSending 表示当前正在等待 F4 返回 ACTUATOR_HOME ACK，防止重复把当前位置清零。 */
    property bool stepperHomeSending: false

    /* stepperHomePendingCommand 保存正在等待回执的设零命令名，空字符串表示当前没有设零命令在途。 */
    property string stepperHomePendingCommand: ""

    /* stepperSpeedEditorVisible 表示速度数字键盘是否打开，用于输入 0~5000 rpm 任意整数。 */
    property bool stepperSpeedEditorVisible: false

    /* stepperSpeedInputText 保存速度数字键盘当前输入文本，点击应用后写入 normalSpeedRpm 或 scanSpeedRpm。 */
    property string stepperSpeedInputText: "0"

    /* stepperSpeedEditKey 保存当前速度键盘正在编辑的字段：normalSpeedRpm 或 scanSpeedRpm。 */
    property string stepperSpeedEditKey: "normalSpeedRpm"

    /* stepperStepEditorVisible 表示上下电机固定位置步数或 Z 轴超时数字键盘是否打开。 */
    property bool stepperStepEditorVisible: false

    /* stepperStepInputText 保存固定下探/回升步数或 Z 轴超时数字键盘当前输入文本。 */
    property string stepperStepInputText: "0"

    /* stepperStepReplaceOnNextDigit 表示下一次数字键是否替换当前文本，解决超时默认 10 秒无法直接输入 1~9 秒的问题。 */
    property bool stepperStepReplaceOnNextDigit: false

    /* stepperStepEditKey 保存当前正在编辑的字段：zDownFixedSteps、zUpFixedSteps 或 zMotionTimeoutMs。 */
    property string stepperStepEditKey: ""

    /* calibrationPopupVisible 表示称重标定弹窗是否打开，用于指导用户放置砝码并发起二进制称重标定命令。 */
    property bool calibrationPopupVisible: false

    /* calibrationWeightText 保存称重标定输入框中的克重文本，发送前会校验为 1~5000 的整数。 */
    property string calibrationWeightText: "1000"

    /* calibrationResultText 保存称重标定最近一次发送、成功或失败结果，便于操作员不看串口也能确认状态。 */
    property string calibrationResultText: "放置砝码后输入克重；F4 将按二进制 WEIGHT_CALIBRATE 标定"

    /* calibrationSending 表示当前二进制称重标定命令正在后台写入 F4 串口，发送完成前禁用重复点击。 */
    property bool calibrationSending: false

    /* alarmCurrentCode 保存当前主告警码；运行期由相机、SD 卡、4G、云端、F4 和检测链路真实状态覆盖。 */
    property string alarmCurrentCode: "ALM-INIT"

    /* alarmCurrentTitle 保存当前主告警名称，用于告警维护页顶部醒目展示。 */
    property string alarmCurrentTitle: "等待真实告警"

    /* alarmCurrentLevel 保存当前告警等级，影响页面颜色和处理建议优先级。 */
    property string alarmCurrentLevel: "记录"

    /* alarmCurrentTime 保存当前告警发生时间，启动时先显示占位，真实问题出现后改成发生时间。 */
    property string alarmCurrentTime: "--:--:--"

    /* alarmAcknowledged 表示操作员是否已确认当前告警；确认不等于故障清除。 */
    property bool alarmAcknowledged: false

    /* alarmCleared 表示当前是否没有未处理真实告警；自动检测到新问题时会重新置为 false。 */
    property bool alarmCleared: true

    /* activeAlarmKeys 保存已经记录过的未恢复问题，用于状态反复刷新时去重，避免每 8 秒刷爆 SD 卡日志。 */
    property var activeAlarmKeys: ({})

    /* alarmLastLogResult 保存最近一次自动告警日志落盘结果，便于保存诊断快照时一起记录。 */
    property string alarmLastLogResult: "尚未生成自动告警日志"

    /* alarmAdviceDetailVisible 表示是否打开告警处理建议完整说明浮层，解决右下角建议面板文字显示不全。 */
    property bool alarmAdviceDetailVisible: false

    /* logDetailVisible 表示是否打开日志全文弹窗，用于在固定 1024x600 屏幕中滚动查看完整日志。 */
    property bool logDetailVisible: false

    /* selectedLogIndex 保存当前点击的日志行号；-1 表示尚未选择日志。 */
    property int selectedLogIndex: -1

    /* selectedLogEntry 保存当前选中日志的文件名、路径、大小和修改时间，弹窗标题区直接读取它。 */
    property var selectedLogEntry: logFileModel.entryAt(selectedLogIndex)

    /* selectedLogContent 保存当前日志全文；日志较长时交给 logDetailFlickable 垂直滚动阅读。 */
    property string selectedLogContent: ""

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

    /* dxPixels 表示最近一次真实 LOCATE 计算出的 Y 轴视觉偏差，单位像素，正数表示零件中心在目标线下方。 */
    property int dxPixels: 0

    /* dxPixelsValid 表示 dxPixels 是否来自当前有效目标；漏检或等待上料时为 false，界面显示 -- px。 */
    property bool dxPixelsValid: false

    /* dxPixelsText 统一生成首页偏差文案，避免多个卡片各自拼接导致显示不一致。 */
    property string dxPixelsText: dxPixelsValid ? ((dxPixels >= 0 ? "+" : "") + dxPixels + " px") : "-- px"

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
     *   返回 C++ 控制器的执行结果文本；控制失败交给重试逻辑处理，不阻塞 Qt 界面启动。
     */
    function setBootOverlayVisible(visible) {
        if (!root.usingKmsOverlay) {
            return ""
        }

        var result = visible
            ? storageController.setOverlayVisible(true)
            : storageController.setOverlayVisible(false)
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
     *   无返回值；恢复结果通过 setBootOverlayVisible 的返回文本驱动重试判断。
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
     *   1. 先检查是否已有 F4 自动流程命令在途，避免同一个串口同时下发多条关键命令。
     *   2. 调用 C++ `sendF4AutoControlCommand()` 组装并发送二进制协议帧，QML 不直接拼帧。
     *   3. 命令启动成功后先显示“下发中”，真正的运行、暂停、继续、停止状态等 F4 ACK 回调后再确认。
     *   4. 如果当前是 Qt 自采集安全预览，只有 F4 ACK 成功后才同步控制 V4L2VideoItem 的 running 状态。
     *
     * 参数：
     *   action 是按钮动作标识，取值为 start、pause、resume 或 stop。
     *   stateText 是显示在界面上的流程状态文本。
     *
     * 返回值：
     *   无返回值；函数会更新 workflowState/storageState，并等待 onF4AutoControlFinished 确认结果。
     */
    function handleControlAction(action, stateText) {
        if (autoControlBusy) {
            storageState = formatF4ToastText("自动流程命令下发中，请等待F4回执")
            showStorageToast()
            return
        }

        autoPendingAction = action
        autoPendingStateText = stateText
        autoControlBusy = true
        workflowState = stateText + "下发中"
        storageState = formatF4ToastText("正在下发自动流程：" + stateText)

        if (!deviceHealth.sendF4AutoControlCommand(action)) {
            autoControlBusy = false
            autoPendingAction = ""
            autoPendingStateText = ""
            workflowState = "自动流程命令未启动"
            storageState = formatF4ToastText(autoLastAckText)
            showStorageToast()
            return
        }

        showStorageToast()
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
        latestModelResultText = ""
        storageState = "正在检测当前帧..."
        storageController.requestDetectCurrentFrame()
        showStorageToast()
    }

    /*
     * startAutoVisionLoop 的作用：
     *   在 F4 START_CYCLE 或 RESUME_CYCLE ACK 成功后启动 MP157 视觉闭环。
     *
     * 主要流程：
     *   1. 清空上一轮居中帧数、居中停机标志和丢失节流时间。
     *   2. 设置 workflowState 和底部提示，让现场知道当前进入“找零件并居中”阶段。
     *   3. 启动 autoVisionTimer，每 100ms 请求一次 overlay LOCATE。
     *
     * 返回值：
     *   无返回值；状态通过 autoVisionTimer 和后续信号推进。
     */
    function startAutoVisionLoop() {
        autoVisionRunning = true
        autoVisionLocateBusy = false
        autoVisionCommandBusy = false
        autoVisionStableFrames = 0
        autoVisionCenteredSent = false
        autoVisionHasSeenTarget = false
        autoVisionLostFrames = 0
        autoVisionLastFrameId = 0
        autoVisionLastLostMs = 0
        autoVisionLastErrorY = 0
        autoVisionLocatePurpose = "center"
        autoVisionActuatorPhase = ""
        autoVisionFineTuneAttempts = 0
        resetAutoVisionFineTuneProgress()
        autoVisionZFocusSettled = false
        autoVisionNeedsZUp = false
        autoVisionDetectFromZFlow = false
        autoVisionPendingZMoveSteps = 0
        autoVisionPendingZMoveSpeedRpm = 0
        autoVisionPendingZMoveDirection = 0
        dxPixels = 0
        dxPixelsValid = false
        autoVisionLastText = "自动视觉：等待零件从上方进入 ROI"
        workflowState = "视觉居中"
        storageState = autoVisionLastText
        autoVisionDetectDelayTimer.stop()
        autoVisionActuatorSettleTimer.stop()
        autoVisionTimer.restart()
        showStorageToast()
    }

    /*
     * stopAutoVisionLoop 的作用：
     *   在暂停、停止、居中完成或异常时关闭 MP157 视觉闭环定时器。
     *
     * 参数：
     *   reason 是停止原因，写入 autoVisionLastText 便于现场排查。
     *
     * 返回值：
     *   无返回值；函数只更新本地状态，不直接发送 F4 命令。
     */
    function stopAutoVisionLoop(reason) {
        autoVisionTimer.stop()
        autoVisionDetectDelayTimer.stop()
        autoVisionRunning = false
        autoVisionLocateBusy = false
        autoVisionCommandBusy = false
        autoVisionStableFrames = 0
        autoVisionCenteredSent = false
        autoVisionHasSeenTarget = false
        autoVisionLostFrames = 0
        autoVisionLocatePurpose = "center"
        autoVisionActuatorPhase = ""
        autoVisionFineTuneAttempts = 0
        resetAutoVisionFineTuneProgress()
        resetAutoVisionLateralReturnState()
        autoVisionZFocusSettled = false
        autoVisionDetectFromZFlow = false
        autoVisionPendingZMoveSteps = 0
        autoVisionPendingZMoveSpeedRpm = 0
        autoVisionPendingZMoveDirection = 0
        autoVisionActuatorSettleTimer.stop()
        dxPixelsValid = false
        if (reason && reason.length > 0) {
            autoVisionLastText = reason
        }
    }

    /*
     * handleAutoVisionLocateFinished 的作用：
     *   接收 C++ `requestAutoVisionLocate()` 返回的定位结果，并决定下发 VISION_POS、VISION_LOST 或居中停止。
     *
     * 主要流程：
     *   1. 从未识别到目标时，按 500ms 节流发送 VISION_LOST reason=1，让 F4 保持扫描等待上料。
     *   2. 找到目标时用 center_y 对齐 height/2，因为零件从画面上方进入。
     *   3. 已经识别过目标后，如果黑色波形零件短暂漏检，不再发送 VISION_LOST，避免 F4 重新扫描把零件送走。
     *   4. 未连续居中时发送 VISION_POS，让 F4 根据 Y 轴误差调速。
     *   5. 连续 3 帧进入 ±24px 死区后发送 BELT_STOP_CENTERED，并等待 F4 ACK 后进入 Z 轴下探和对焦流程。
     *
     * 参数：
     *   ok 表示 overlay LOCATE 是否成功返回。
     *   result 是 C++ 解析出的定位结果 map。
     *   detail 是 overlay 原始回复或错误文本。
     *
     * 返回值：
     *   无返回值；函数通过 deviceHealth 的二进制命令接口继续推进。
     */
    function handleAutoVisionLocateFinished(ok, result, detail) {
        autoVisionLocateBusy = false

        if (autoVisionLocatePurpose === "fine-tune") {
            autoVisionLocatePurpose = "center"
            handleAutoVisionFineTuneLocateFinished(ok, result, detail)
            return
        }

        if (!autoVisionRunning || autoWorkflowPaused || autoVisionCenteredSent) {
            return
        }

        if (!ok) {
            autoVisionStableFrames = 0
            dxPixelsValid = false
            autoVisionLastText = "自动视觉定位失败：" + detail
            storageState = autoVisionLastText
            showStorageToast()
            return
        }

        var hasTarget = result && Number(result.has_target) === 1
        var frameId = result ? Number(result.frame_id) : 0
        var width = result ? Number(result.width) : 0
        var height = result ? Number(result.height) : 0
        var centerX = result ? Number(result.center_x) : 0
        var centerY = result ? Number(result.center_y) : 0
        var confidence = result ? Number(result.confidence) : 0

        autoVisionLastFrameId = frameId

        if (!hasTarget || height <= 0) {
            var nowMs = new Date().getTime()
            autoVisionStableFrames = 0
            dxPixelsValid = false

            if (autoVisionHasSeenTarget) {
                autoVisionLostFrames += 1
                workflowState = autoVisionLostFrames >= autoVisionLostWarnFrames ? "等待重识别" : "视觉保持"
                autoVisionLastText = autoVisionLostFrames <= autoVisionLostHoldFrames
                        ? "自动视觉：目标短暂丢失 "
                        : "自动视觉：目标连续丢失但保持停机 "
                autoVisionLastText = autoVisionLastText
                        + autoVisionLostFrames + " 帧，保持停机等待重新识别"
                        + "，上次error=" + autoVisionLastErrorY
                if (autoVisionLostFrames >= autoVisionLostWarnFrames) {
                    autoVisionLastText += "；请检查光照、黑色波形零件边缘和搜索带位置"
                }
                storageState = autoVisionLastText
                return
            }

            autoVisionLostFrames = 0
            autoVisionLastText = "自动视觉：尚未识别到零件，继续等待上方来料"
            storageState = autoVisionLastText

            if (!autoVisionCommandBusy && nowMs - autoVisionLastLostMs >= 500) {
                autoVisionLastLostMs = nowMs
                if (deviceHealth.sendF4VisionLost(1)) {
                    autoVisionCommandBusy = true
                }
            }
            return
        }

        var targetY = Math.round(height / 2)
        var errorY = Math.round(centerY - targetY)
        var absErrorY = Math.abs(errorY)

        autoVisionHasSeenTarget = true
        autoVisionLostFrames = 0
        autoVisionLastErrorY = errorY
        dxPixels = errorY
        dxPixelsValid = true
        workflowState = "视觉居中"

        if (absErrorY <= autoVisionCenterTolerancePx) {
            autoVisionStableFrames += 1
        } else {
            autoVisionStableFrames = 0
        }

        autoVisionLastText = "自动视觉：x=" + Math.round(centerX)
                + " y=" + Math.round(centerY)
                + " target=" + targetY
                + " error=" + errorY
                + " stable=" + autoVisionStableFrames
                + "/" + autoVisionStableRequiredFrames
                + " conf=" + Math.round(confidence)
        storageState = autoVisionLastText

        if (autoVisionCommandBusy) {
            return
        }

        if (autoVisionStableFrames >= autoVisionStableRequiredFrames) {
            autoVisionCenteredSent = true
            autoVisionTimer.stop()
            workflowState = "居中停机"
            if (deviceHealth.sendF4BeltStopCentered(frameId)) {
                autoVisionCommandBusy = true
            } else {
                autoVisionCenteredSent = false
                autoVisionTimer.restart()
            }
            return
        }

        if (deviceHealth.sendF4VisionPosition(result)) {
            autoVisionCommandBusy = true
        }
    }

    /*
     * autoVisionNormalizeSpeedRpm 的作用：
     *   自动检测链路中把参数页速度整理成可下发 F4 的 1~5000 rpm。
     *
     * 主要流程：
     *   1. 读取调用方给出的 speedRpm，并转成整数。
     *   2. 小于等于 0 时回退到 fallbackRpm，避免把 0 传给 F4 后让执行器保持停止。
     *   3. 大于 5000 时只按协议上限截断，不再固定限制到 40rpm。
     *
     * 参数：
     *   speedRpm 是参数页保存的电机速度，单位 rpm。
     *   fallbackRpm 是参数缺失或为 0 时使用的兜底速度。
     *
     * 返回值：
     *   返回 1~5000 的协议合法速度。
     */
    function autoVisionNormalizeSpeedRpm(speedRpm, fallbackRpm) {
        var fallbackSpeed = Math.max(1, Math.min(5000, Math.floor(Number(fallbackRpm || autoVisionFallbackSpeedRpm))))
        var speed = Math.floor(Number(speedRpm || 0))

        if (speed <= 0) {
            return fallbackSpeed
        }

        return Math.max(1, Math.min(speed, 5000))
    }

    /*
     * conveyorScanSpeedRpm 的作用：
     *   返回传送带自动上料扫描速度，也就是尚未检测到零件时 F4 SCAN 阶段使用的速度。
     *
     * 返回值：
     *   返回 1~5000 rpm；如果旧 JSON 没有 scanSpeedRpm，则回退到传送带常规速度。
     */
    function conveyorScanSpeedRpm() {
        var motor = conveyorMotorSetting()
        var fallback = motor.normalSpeedRpm || autoVisionFallbackSpeedRpm
        return autoVisionNormalizeSpeedRpm(motor.scanSpeedRpm || fallback, fallback)
    }

    /*
     * conveyorTrackSpeedRpm 的作用：
     *   返回传送带检测到零件后的视觉对中和短步微调速度。
     *
     * 返回值：
     *   返回 1~5000 rpm，直接来自参数页传送带常规速度。
     */
    function conveyorTrackSpeedRpm() {
        var motor = conveyorMotorSetting()
        return autoVisionNormalizeSpeedRpm(motor.normalSpeedRpm || 0, autoVisionFallbackSpeedRpm)
    }

    /*
     * autoVisionEstimateZMoveMs 的作用：
     *   根据本次 Z 轴相对位置运动的 step 数和 rpm，估算 MP157 在收到 F4 ACK 后还要等待多久。
     *
     * 主要流程：
     *   1. 把 steps、speedRpm 和每圈步数都转成安全整数，避免异常配置导致除零或负等待。
     *   2. 用“步数 / 每圈步数 / rpm”换算基础运动时间，单位从分钟转换为毫秒。
     *   3. 叠加安全余量和短机械稳定时间，因为 F4 ACK 只代表命令已接收，不能代表 Emm42 已经走到位。
     *   4. 最后用最小/最大等待时间限幅，保证小动作不会过早继续，大异常不会无限卡住流程。
     *
     * 参数：
     *   stepsValue 是本次 Z 轴相对位置运动步数，来自 zDownFixedSteps 或 zUpFixedSteps。
     *   speedRpm 是本次 Z 轴运动速度，单位 rpm，来自参数页上下电机常规速度。
     *
     * 返回值：
     *   返回需要等待的毫秒数；这个值是保守估算，不是 F4 确认完成事件。
     */
    function autoVisionEstimateZMoveMs(stepsValue, speedRpm) {
        var steps = Math.max(0, Math.floor(Number(stepsValue || 0)))
        var speed = autoVisionNormalizeSpeedRpm(speedRpm, autoVisionFallbackSpeedRpm)
        var stepsPerRev = Math.max(1, Math.floor(Number(autoVisionZMoveStepsPerRev || 1)))
        var moveMs = Math.ceil((steps * 60000.0) / (stepsPerRev * speed))
        var waitMs = moveMs + autoVisionZMotionSafetyMs + autoVisionShortSettleMs

        waitMs = Math.max(autoVisionZMotionMinimumWaitMs, waitMs)
        waitMs = Math.min(cameraZMotionTimeoutMs(), waitMs)
        return waitMs
    }

    /*
     * autoVisionStartZMotionWait 的作用：
     *   在 Z 轴位置命令发出后启动本地超时保护，避免 F4 到位事件丢失时自动流程永久卡住。
     *
     * 主要流程：
     *   1. 按 direction 选择 z-motion-down-wait 或 z-motion-up-wait 阶段。
     *   2. 用 autoVisionEstimateZMoveMs() 估算保护超时时间，并重启 autoVisionActuatorSettleTimer。
     *   3. 正常路径不靠该定时器推进，必须由 autoVisionHandleActuatorMoveDone() 收到 F4 完成事件后推进。
     *
     * 参数：
     *   direction 为 0 时表示下降，为 1 时表示回升。
     *   stepsValue 是本次 Z 轴移动步数。
     *   speedRpm 是本次 Z 轴移动速度。
     *
     * 返回值：
     *   无返回值；函数只设置阶段和本地保护定时器。
     */
    function autoVisionStartZMotionWait(direction, stepsValue, speedRpm) {
        var movingDown = Math.floor(Number(direction || 0)) === 0
        var waitMs = autoVisionEstimateZMoveMs(stepsValue, speedRpm)
        var waitSeconds = (waitMs / 1000.0).toFixed(1)

        autoVisionActuatorPhase = movingDown ? "z-motion-down-wait" : "z-motion-up-wait"
        workflowState = movingDown ? "Z轴下降到位等待" : "Z轴回升到位等待"
        autoVisionLastText = "等待 F4 ACTUATOR_MOVE_DONE 确认上下电机"
                + (movingDown ? "下降" : "回升")
                + "动作完成，本地保护 " + waitSeconds + " 秒，steps=" + stepsValue
                + "，speed=" + speedRpm + "rpm"
        storageState = autoVisionLastText
        showStorageToast()
        autoVisionActuatorSettleTimer.interval = waitMs
        autoVisionActuatorSettleTimer.restart()
    }

    /*
     * autoVisionHandleActuatorMoveDone 的作用：
     *   在 MP157 C++ 已确认收到 F4 ACTUATOR_MOVE_DONE 后推进自动流程。
     *
     * 主要流程：
     *   1. 停止 Z 轴本地保护定时器，说明正常完成来自 F4 完成事件而不是固定 sleep。
     *   2. Z 下降完成后，先用传送带和左右轴继续复查/微调 ROI 中心。
     *   3. Z 回升完成后，才允许通知 F4/ESP32S3 机械臂抓取零件并进入称重、电感流程。
     *
     * 参数：
     *   detail 是 C++ 返回的 ACK + EVENT_REPORT 诊断文本，必须包含 actuator-move-done；
     *   当 detail 包含 estimated-done 时，表示 F4 使用运动时间估算完成兜底。
     *
     * 返回值：
     *   true 表示当前阶段已处理；false 表示当前阶段不是 Z 轴完成等待。
     */
    function autoVisionHandleActuatorMoveDone(detail) {
        var eventText = String(detail || "")
        var estimatedDone = eventText.indexOf("estimated-done") >= 0

        if (eventText.indexOf("actuator-move-timeout") >= 0) {
            autoVisionActuatorSettleTimer.stop()
            autoVisionLastText = "F4执行器到位超时：" + eventText
            storageState = autoVisionLastText
            workflowState = "执行器超时"
            autoVisionActuatorPhase = ""
            showStorageToast()
            return true
        }

        if (eventText.indexOf("actuator-move-done") < 0) {
            return false
        }

        if (autoVisionActuatorPhase === "z-motion-down-wait") {
            autoVisionActuatorSettleTimer.stop()
            autoVisionNeedsZUp = true
            autoVisionPendingZMoveSteps = 0
            autoVisionPendingZMoveSpeedRpm = 0
            autoVisionPendingZMoveDirection = 0
            autoVisionLastText = (estimatedDone
                    ? "F4估算Z轴下降完成，开始ROI复查并用传送带/左右轴微调："
                    : "F4收到Z轴下降主动到位回包，开始ROI复查并用传送带/左右轴微调：") + eventText
            storageState = autoVisionLastText
            showStorageToast()
            autoVisionRequestFineTuneLocate()
            return true
        }

        if (autoVisionActuatorPhase === "z-motion-up-wait") {
            autoVisionActuatorSettleTimer.stop()
            autoVisionNeedsZUp = false
            autoVisionZFocusSettled = false
            autoVisionDetectFromZFlow = false
            autoVisionPendingZMoveSteps = 0
            autoVisionPendingZMoveSpeedRpm = 0
            autoVisionPendingZMoveDirection = 0
            autoVisionActuatorPhase = ""
            workflowState = "高度已恢复"
            storageState = (estimatedDone
                    ? "F4估算Z轴回升完成，开始通知F4/ESP32S3机械臂流程："
                    : "F4收到Z轴回升主动到位回包，开始通知F4/ESP32S3机械臂流程：") + eventText
            showStorageToast()
            autoVisionStartF4ArmInspectionAfterZUp()
            return true
        }

        return false
    }

    /*
     * autoVisionRequestZDown 的作用：
     *   在 F4 确认传送带居中停机后，请求上下电机按固定步数下降到模型检测高度。
     *
     * 主要流程：
     *   1. 从参数页第三台电机读取 zDownFixedSteps 和 normalSpeedRpm。
     *   2. 如果用户把下探步数设为 0，则跳过 Z 轴运动，直接进入前后/左右复查。
     *   3. 下发 ACTUATOR_POS_MOVE，方向 0 代表下降，实际电机正反由 F4 运行时方向映射负责。
     *
     * 返回值：
     *   true 表示已启动串口命令或无需下探；false 表示命令未能启动。
     */
    function autoVisionRequestZDown() {
        var motor = cameraZMotorSetting()
        var steps = Math.floor(Number(motor.zDownFixedSteps || 0))
        var speed = autoVisionNormalizeSpeedRpm(motor.normalSpeedRpm || 0, autoVisionFallbackSpeedRpm)
        var timeoutMs = cameraZMotionTimeoutMs()

        autoVisionFineTuneAttempts = 0
        resetAutoVisionFineTuneProgress()
        resetAutoVisionLateralReturnState()
        autoVisionZFocusSettled = false
        autoVisionDetectFromZFlow = false
        autoVisionPendingZMoveSteps = 0
        autoVisionPendingZMoveSpeedRpm = 0
        autoVisionPendingZMoveDirection = 0

        if (steps <= 0) {
            autoVisionLastText = "上下电机下探步数为0，跳过Z轴下降，进入ROI复查"
            storageState = autoVisionLastText
            autoVisionActuatorPhase = "z-down-skip"
            autoVisionActuatorSettleTimer.interval = autoVisionShortSettleMs
            autoVisionActuatorSettleTimer.restart()
            showStorageToast()
            return true
        }

        workflowState = "Z轴下降"
        autoVisionActuatorPhase = "z-down"
        autoVisionPendingZMoveSteps = steps
        autoVisionPendingZMoveSpeedRpm = speed
        autoVisionPendingZMoveDirection = 0
        autoVisionLastText = "自动视觉：上下电机下降 " + steps
                + " step，最多等待 " + (timeoutMs / 1000.0).toFixed(1)
                + " 秒后按 MP157 本地估算继续"
        storageState = autoVisionLastText
        showStorageToast()

        if (deviceHealth.sendF4ActuatorPositionMoveWithTimeout(2, 0, 0, speed, steps, 0, timeoutMs)) {
            autoVisionCommandBusy = true
            return true
        }

        autoVisionActuatorPhase = ""
        autoVisionPendingZMoveSteps = 0
        autoVisionPendingZMoveSpeedRpm = 0
        autoVisionPendingZMoveDirection = 0
        autoVisionLastText = "上下电机下降命令未启动"
        storageState = autoVisionLastText
        showStorageToast()
        return false
    }

    /*
     * autoVisionRequestFineTuneLocate 的作用：
     *   在 Z 轴下降对焦稳定或短步微调后重新请求 overlay LOCATE，确认零件是否仍在 ROI 中央。
     *
     * 返回值：
     *   true 表示 LOCATE 请求已启动；false 表示 overlay 请求被拒绝。
     */
    function autoVisionRequestFineTuneLocate() {
        autoVisionLocatePurpose = "fine-tune"
        autoVisionLocateBusy = true
        workflowState = "ROI复查"
        autoVisionLastText = "自动视觉：Z轴下降或短步微调后复查ROI中心"
        storageState = autoVisionLastText
        showStorageToast()

        if (deviceHealth.requestAutoVisionLocate()) {
            return true
        }

        autoVisionLocateBusy = false
        autoVisionLocatePurpose = "center"
        autoVisionLastText = "ROI复查 LOCATE 请求未启动，进入模型检测"
        storageState = autoVisionLastText
        autoVisionStartDetectDelay()
        showStorageToast()
        return false
    }

    /*
     * resetAutoVisionFineTuneProgress 的作用：
     *   清空 ROI 微调的“上一轴误差”和“连续未改善次数”，保证新一轮零件从干净状态开始。
     *
     * 主要流程：
     *   1. 清空上一次微调轴名，避免上一轮 X 轴或 Y 轴结果影响本轮。
     *   2. 清零上一次绝对误差和连续未改善计数。
     *
     * 返回值：
     *   无返回值；函数只更新 QML 自动流程状态变量。
     */
    function resetAutoVisionFineTuneProgress() {
        autoVisionFineTuneLastAxis = ""
        autoVisionFineTuneLastAbsError = 0
        autoVisionFineTuneNoImproveCount = 0
    }

    /*
     * resetAutoVisionLateralReturnState 的作用：
     *   清空本轮自动检测左右轴回中相关状态，保证每个零件只按本轮实际左右微调量回中。
     *
     * 主要流程：
     *   1. 清零左右轴累计偏移，表示相机当前以本轮开始时的位置作为皮带基准。
     *   2. 清空等待 F4 回执的左右轴微调步数和方向，避免上一轮残留被误累计。
     *
     * 返回值：
     *   无返回值；函数只更新 QML 自动流程状态变量。
     */
    function resetAutoVisionLateralReturnState() {
        autoVisionLateralReturnOffsetSteps = 0
        autoVisionPendingLateralFineTuneSteps = 0
        autoVisionPendingLateralFineTuneDirection = 0
    }

    /*
     * recordAutoVisionPendingLateralFineTune 的作用：
     *   在左右轴微调命令写入 F4 前暂存本次 direction/steps，等待完成回执后再真正累计偏移。
     *
     * 主要流程：
     *   1. 把 direction 规范成 0/1，和 ACTUATOR_POS_MOVE 协议一致。
     *   2. 把 steps 规范成正整数，防止无效步数进入回中累计。
     *
     * 参数：
     *   direction 是本次左右轴逻辑方向，0=相机左移，1=相机右移。
     *   steps 是本次左右轴位置微调步数，单位 step。
     *
     * 返回值：
     *   无返回值；函数只暂存待确认的左右轴微调动作。
     */
    function recordAutoVisionPendingLateralFineTune(direction, steps) {
        autoVisionPendingLateralFineTuneDirection = Math.floor(Number(direction || 0)) === 1 ? 1 : 0
        autoVisionPendingLateralFineTuneSteps = Math.max(0, Math.floor(Number(steps || 0)))
    }

    /*
     * clearAutoVisionPendingLateralFineTune 的作用：
     *   清空等待确认的左右轴微调动作，用于命令失败或已经成功累计后的收尾。
     *
     * 返回值：
     *   无返回值；函数只清空 pending 状态。
     */
    function clearAutoVisionPendingLateralFineTune() {
        autoVisionPendingLateralFineTuneSteps = 0
        autoVisionPendingLateralFineTuneDirection = 0
    }

    /*
     * commitAutoVisionLateralFineTuneOffset 的作用：
     *   在 F4 确认左右轴 ACTUATOR_POS_MOVE 完成后，把本次实际微调量计入本轮相机偏移。
     *
     * 主要流程：
     *   1. 读取 pending 中的 direction/steps；没有有效 pending 时直接返回。
     *   2. direction=1 表示相机向右移动，累计偏移加 steps；direction=0 表示相机向左移动，累计偏移减 steps。
     *   3. 累计后清空 pending，避免同一条 F4 回执被重复累计。
     *
     * 返回值：
     *   返回累计后的有符号偏移，单位 step；正值表示相机偏右，负值表示相机偏左。
     */
    function commitAutoVisionLateralFineTuneOffset() {
        var steps = Math.max(0, Math.floor(Number(autoVisionPendingLateralFineTuneSteps || 0)))
        var direction = Math.floor(Number(autoVisionPendingLateralFineTuneDirection || 0)) === 1 ? 1 : 0
        var signedSteps = direction === 1 ? steps : -steps

        if (steps <= 0) {
            clearAutoVisionPendingLateralFineTune()
            return autoVisionLateralReturnOffsetSteps
        }

        autoVisionLateralReturnOffsetSteps = Math.max(-1000000,
                                                       Math.min(1000000,
                                                                Math.floor(Number(autoVisionLateralReturnOffsetSteps || 0)) + signedSteps))
        clearAutoVisionPendingLateralFineTune()
        return autoVisionLateralReturnOffsetSteps
    }

    /*
     * autoVisionFineTuneNoImproveBoost 的作用：
     *   判断同一根轴连续微调后误差是否没有明显变小；如果没有改善，则给下一次 steps 额外加档。
     *
     * 主要流程：
     *   1. 同一轴连续微调时，比较当前绝对误差和上一次绝对误差。
     *   2. 如果当前误差没有比上一次至少减少 autoVisionFineTuneProgressDeadbandPx，就认为本次无明显改善。
     *   3. 无改善次数最多累加到 3 档，避免机构不动时无限增大步数。
     *   4. 轴切换时重置无改善计数，因为 X/Y 对应不同执行器。
     *
     * 参数：
     *   axisName 是当前微调轴名，conveyor 表示传送带/Y 方向，lateral 表示左右轴/X 方向。
     *   absError 是当前 ROI 复查得到的绝对像素误差。
     *
     * 返回值：
     *   返回本次因为连续无改善而追加的倍率档数，范围 0~3。
     */
    function autoVisionFineTuneNoImproveBoost(axisName, absError) {
        var axis = String(axisName || "")
        var currentError = Math.max(0, Math.floor(Number(absError || 0)))
        var deadband = Math.max(0, Math.floor(Number(autoVisionFineTuneProgressDeadbandPx || 0)))

        if (axis.length <= 0) {
            resetAutoVisionFineTuneProgress()
            return 0
        }

        if (autoVisionFineTuneLastAxis === axis) {
            if (currentError >= Math.max(0, autoVisionFineTuneLastAbsError - deadband)) {
                autoVisionFineTuneNoImproveCount = Math.min(3, autoVisionFineTuneNoImproveCount + 1)
            } else {
                autoVisionFineTuneNoImproveCount = 0
            }
        } else {
            autoVisionFineTuneNoImproveCount = 0
        }

        autoVisionFineTuneLastAxis = axis
        autoVisionFineTuneLastAbsError = currentError
        return autoVisionFineTuneNoImproveCount
    }

    /*
     * autoVisionFineTuneStepsForError 的作用：
     *   把 ROI 像素误差转换成实际下发给 F4 的 ACTUATOR_POS_MOVE steps。
     *
     * 主要流程：
     *   1. 先读取参数页的 minStep，作为单次微调的最小脉冲数。
     *   2. 计算 abs(error) 超出中心死区的像素量；只有超出死区的部分才放大步数。
     *   3. 每超出 autoVisionFineTuneStepScalePx 像素，就在 minStep 基础上多走一档。
     *   4. 如果同一轴连续微调后误差没有明显改善，再追加 1~3 档，解决现场“每次都是 steps=15 但画面不变”的问题。
     *   5. 最后把单次微调限制在 minStep 的 autoVisionFineTuneMaxStepMultiplier 倍以内，并限制到 10000 step。
     *
     * 参数：
     *   errorPixels 是当前轴的带符号像素误差，errorY 对应传送带，errorX 对应左右轴。
     *   motorMinStep 是参数页配置的最小步长，单位 step。
     *   axisName 是当前微调轴名，用于判断连续无改善。
     *
     * 返回值：
     *   返回本次应下发的相对移动步数，单位 step，范围 1~10000。
     */
    function autoVisionFineTuneStepsForError(errorPixels, motorMinStep, axisName) {
        var minStep = Math.max(1, Math.min(10000, Math.floor(Number(motorMinStep || 1))))
        var absError = Math.abs(Math.floor(Number(errorPixels || 0)))
        var tolerance = Math.max(0, Math.floor(Number(autoVisionFineTuneTolerancePx || 0)))
        var scalePx = Math.max(1, Math.floor(Number(autoVisionFineTuneStepScalePx || 1)))
        var maxMultiplier = Math.max(1, Math.floor(Number(autoVisionFineTuneMaxStepMultiplier || 1)))
        var excessPx = Math.max(0, absError - tolerance)
        var noImproveBoost = autoVisionFineTuneNoImproveBoost(axisName, absError)
        var multiplier = 1 + Math.ceil(excessPx / scalePx) + noImproveBoost

        multiplier = Math.max(1, Math.min(maxMultiplier, multiplier))
        return Math.max(1, Math.min(10000, minStep * multiplier))
    }

    /*
     * autoVisionStartFocusSettleBeforeDetect 的作用：
     *   ROI 二次对中已经通过后，单独等待 3 秒让摄像头稳定和对焦，然后再进入模型检测。
     *
     * 主要流程：
     *   1. 把自动执行器阶段切到 focus-settle，和 z-down/fine-tune 阶段区分开。
     *   2. 使用 autoVisionZFocusSettleMs 作为聚焦稳定时间，当前固定为 3000ms。
     *   3. 定时器到期后由 autoVisionActuatorSettleTimer 调用 autoVisionStartDetectDelay()。
     *
     * 返回值：
     *   无返回值；函数只启动聚焦稳定定时器。
     */
    function autoVisionStartFocusSettleBeforeDetect() {
        autoVisionActuatorPhase = "focus-settle"
        workflowState = "对焦稳定"
        autoVisionLastText = "ROI已在检测中心，等待约3秒让摄像头对焦稳定后再检测模型"
        storageState = autoVisionLastText
        showStorageToast()
        autoVisionActuatorSettleTimer.interval = autoVisionZFocusSettleMs
        autoVisionActuatorSettleTimer.restart()
    }

    /*
     * autoVisionStartDetectDelay 的作用：
     *   根据本轮是否已经完成 Z 轴 3 秒对焦等待，选择模型检测前的延时。
     *
     * 主要流程：
     *   1. 如果 autoVisionZFocusSettled 为 true，说明刚才已经等待过 3 秒对焦，此时只等 300ms 刷新一帧。
     *   2. 如果没有执行 Z 轴下探或没有完成对焦等待，保留旧的 2000ms 静止等待。
     *   3. 统一重启 autoVisionDetectDelayTimer，避免各分支手写不同延时。
     *
     * 返回值：
     *   无返回值；定时器触发后会调用 handleDetectAction()。
     */
    function autoVisionStartDetectDelay() {
        autoVisionDetectDelayTimer.interval = autoVisionZFocusSettled
                ? autoVisionPostFocusDetectDelayMs
                : autoVisionDefaultDetectDelayMs
        autoVisionDetectDelayTimer.restart()
    }

    /*
     * handleAutoVisionFineTuneLocateFinished 的作用：
     *   处理 Z 轴下降后的 ROI 复查结果，必要时用传送带做前后短步微调、用摄像头左右电机做左右短步微调。
     *
     * 主要流程：
     *   1. 定位失败或无目标时不继续移动，直接检测并把风险写到底部状态。
     *   2. 同时计算 X/Y 两个方向误差，Y 代表传送带前后方向，X 代表左右电机方向。
     *   3. 如果 Y 误差超出死区，优先发送传送带 ACTUATOR_POS_MOVE 微调一次。
     *   4. 如果 Y 已经进死区但 X 误差超出死区，发送左右轴 ACTUATOR_POS_MOVE 微调一次。
     *   5. 达到次数上限后停止继续微调，进入模型检测，避免现场机械反复抖动。
     *
     * 参数：
     *   ok/result/detail 来自 autoVisionLocateFinished。
     *
     * 返回值：
     *   无返回值；函数会继续发传送带/左右轴命令或启动模型检测延时。
     */
    function handleAutoVisionFineTuneLocateFinished(ok, result, detail) {
        autoVisionLocateBusy = false

        if (!ok || !result || Number(result.has_target) !== 1 || Number(result.width) <= 0 || Number(result.height) <= 0) {
            workflowState = "模型检测"
            autoVisionLastText = "ROI复查未稳定返回目标，先进入模型检测：" + detail
            storageState = autoVisionLastText
            autoVisionStartDetectDelay()
            showStorageToast()
            return
        }

        var centerX = Number(result.center_x)
        var centerY = Number(result.center_y)
        var width = Number(result.width)
        var height = Number(result.height)
        var targetX = Math.round(width / 2)
        var targetY = Math.round(height / 2)
        var errorX = Math.round(centerX - targetX)
        var errorY = Math.round(centerY - targetY)
        var absErrorX = Math.abs(errorX)
        var absErrorY = Math.abs(errorY)
        var xCentered = absErrorX <= autoVisionFineTuneTolerancePx
        var yCentered = absErrorY <= autoVisionFineTuneTolerancePx

        dxPixels = yCentered ? errorX : errorY
        dxPixelsValid = true

        if (xCentered && yCentered) {
            autoVisionLastText = "ROI复查通过：errorY=" + errorY
                    + "，errorX=" + errorX + "，进入3秒聚焦稳定"
            storageState = autoVisionLastText
            autoVisionStartFocusSettleBeforeDetect()
            showStorageToast()
            return
        }

        if (autoVisionFineTuneAttempts >= autoVisionFineTuneMaxAttempts) {
            workflowState = "模型检测"
            autoVisionLastText = "ROI复查仍偏移 errorY=" + errorY
                    + "，errorX=" + errorX
                    + "，已达到微调上限 " + autoVisionFineTuneMaxAttempts + " 次，进入模型检测"
            storageState = autoVisionLastText
            autoVisionStartDetectDelay()
            showStorageToast()
            return
        }

        if (!yCentered) {
            autoVisionFineTuneConveyor(errorY)
            return
        }

        autoVisionFineTuneLateral(errorX)
    }

    /*
     * autoVisionFineTuneConveyor 的作用：
     *   根据 ROI 复查的 Y 方向误差请求传送带电机做一次固定步数前后微调。
     *
     * 参数：
     *   errorY 是零件中心 Y 与 ROI 中心的像素差，正值表示零件在画面中心下方。
     *
     * 返回值：
     *   true 表示微调命令已启动；false 表示命令未能启动并改为进入模型检测。
     */
    function autoVisionFineTuneConveyor(errorY) {
        var motor = conveyorMotorSetting()
        var minStep = Math.max(1, Math.floor(Number(motor.minStep || 1)))
        var steps = autoVisionFineTuneStepsForError(errorY, minStep, "conveyor")
        var speed = conveyorTrackSpeedRpm()
        var direction = errorY > 0 ? 0 : 1

        autoVisionFineTuneAttempts += 1
        autoVisionActuatorPhase = "fine-tune-conveyor"
        workflowState = "传送带微调"
        autoVisionLastText = "自动视觉：传送带前后微调第 " + autoVisionFineTuneAttempts
                + " 次，errorY=" + errorY + "，steps=" + steps
                + "，minStep=" + minStep
                + "，direction=" + direction
                + (direction === 0 ? "(后退)" : "(前进)")
                + (autoVisionFineTuneNoImproveCount > 0
                   ? "，无改善加档=" + autoVisionFineTuneNoImproveCount
                   : "")
        storageState = autoVisionLastText
        showStorageToast()

        if (deviceHealth.sendF4ActuatorPositionMove(0, direction, 0, speed, steps, 0)) {
            autoVisionCommandBusy = true
            return true
        }

        autoVisionActuatorPhase = ""
        workflowState = "模型检测"
        autoVisionLastText = "传送带前后微调命令未启动，进入模型检测"
        storageState = autoVisionLastText
        autoVisionStartDetectDelay()
        showStorageToast()
        return false
    }

    /*
     * autoVisionFineTuneLateral 的作用：
     *   根据 ROI 复查的 X 方向误差请求摄像头左右电机做一次固定步数微调。
     *
     * 参数：
     *   errorX 是零件中心 X 与 ROI 中心的像素差，正值表示零件在画面中心右侧。
     *   左右轴移动的是摄像头本体，画面坐标会和相机运动方向相反；
     *   因此零件在右侧时要让相机右移，画面里的零件才会向左回到 ROI 中心。
     *
     * 返回值：
     *   true 表示微调命令已启动；false 表示命令未能启动并改为进入模型检测。
     */
    function autoVisionFineTuneLateral(errorX) {
        var motor = cameraLateralMotorSetting()
        var minStep = Math.max(1, Math.floor(Number(motor.minStep || 1)))
        var steps = autoVisionFineTuneStepsForError(errorX, minStep, "lateral")
        var speed = autoVisionNormalizeSpeedRpm(motor.normalSpeedRpm || 0, autoVisionFallbackSpeedRpm)
        var direction = errorX > 0 ? 1 : 0

        autoVisionFineTuneAttempts += 1
        autoVisionActuatorPhase = "fine-tune-lateral"
        workflowState = "左右微调"
        autoVisionLastText = "自动视觉：左右轴微调第 " + autoVisionFineTuneAttempts
                + " 次，errorX=" + errorX + "，steps=" + steps
                + "，minStep=" + minStep
                + "，direction=" + direction
                + (direction === 1 ? "(相机右移/画面左移)" : "(相机左移/画面右移)")
                + (autoVisionFineTuneNoImproveCount > 0
                   ? "，无改善加档=" + autoVisionFineTuneNoImproveCount
                   : "")
        storageState = autoVisionLastText
        showStorageToast()

        recordAutoVisionPendingLateralFineTune(direction, steps)
        if (deviceHealth.sendF4ActuatorPositionMove(1, direction, 0, speed, steps, 0)) {
            autoVisionCommandBusy = true
            return true
        }

        clearAutoVisionPendingLateralFineTune()
        autoVisionActuatorPhase = ""
        workflowState = "模型检测"
        autoVisionLastText = "左右轴微调命令未启动，进入模型检测"
        storageState = autoVisionLastText
        autoVisionStartDetectDelay()
        showStorageToast()
        return false
    }

    /*
     * autoVisionRequestLateralReturnToBeltCenter 的作用：
     *   本轮自动检测中如果左右轴为了让零件进入 ROI 中心而移动过相机，模型检测和 Z 轴回升后要把相机退回皮带基准位置。
     *
     * 主要流程：
     *   1. 读取 autoVisionLateralReturnOffsetSteps；0 表示本轮没有左右轴净偏移，不发任何回中动作。
     *   2. 正偏移表示相机曾向右移动，回中要发送 direction=0 左移；负偏移表示相机曾向左移动，回中要发送 direction=1 右移。
     *   3. 使用摄像头左右轴参数页 normalSpeedRpm 作为回中速度，步数等于累计偏移绝对值。
     *   4. 只启动命令，不在这里清零；必须等 F4 完成回执后再清零，防止命令失败却误认为已回中。
     *
     * 返回值：
     *   true 表示已启动左右轴回中命令；false 表示无需回中或命令未能启动。
     */
    function autoVisionRequestLateralReturnToBeltCenter() {
        var offsetSteps = Math.floor(Number(autoVisionLateralReturnOffsetSteps || 0))
        var steps = Math.abs(offsetSteps)

        if (steps <= 0) {
            return false
        }

        var motor = cameraLateralMotorSetting()
        var speed = autoVisionNormalizeSpeedRpm(motor.normalSpeedRpm || 0, autoVisionFallbackSpeedRpm)
        var direction = offsetSteps > 0 ? 0 : 1

        workflowState = "左右轴回中"
        autoVisionActuatorPhase = "lateral-return"
        autoVisionLastText = "本轮左右轴曾微调，相机回到皮带基准：offset="
                + offsetSteps + " step，returnSteps=" + steps
                + "，direction=" + direction
                + (direction === 0 ? "(相机左移)" : "(相机右移)")
        storageState = autoVisionLastText
        showStorageToast()

        if (deviceHealth.sendF4ActuatorPositionMove(1, direction, 0, speed, steps, 0)) {
            autoVisionCommandBusy = true
            return true
        }

        autoVisionActuatorPhase = ""
        autoVisionLastText = "左右轴回中命令未启动，仍保留累计偏移 "
                + offsetSteps + " step，请手动回中后再继续自动循环"
        storageState = autoVisionLastText
        showStorageToast()
        return false
    }

    /*
     * autoVisionStartF4ArmInspectionAfterZUp 的作用：
     *   模型检测完成且上下轴已经回升后，通知 F4 启动 ESP32S3 机械臂称重/电感流程。
     *
     * 主要流程：
     *   1. 校验最近一次模型 RESULT 已缓存，否则不能让 F4 记录空模型结果。
     *   2. 调用 requestF4ArmInspectionFlow() 下发 MODEL_READY 和 ARM_JOB_START。
     *   3. 后续等待 onF4ArmInspectionFlowFinished，再执行完整云端上传。
     *
     * 返回值：
     *   true 表示 F4 长流程已启动；false 表示缺少模型结果或 F4 忙。
     */
    function autoVisionStartF4ArmInspectionAfterZUp() {
        if (autoVisionLateralReturnOffsetSteps !== 0) {
            return autoVisionRequestLateralReturnToBeltCenter()
        }

        if (!latestModelResultText || latestModelResultText.indexOf("RESULT ") !== 0) {
            storageState = "模型结果未缓存，不能启动F4机械臂称重/电感流程"
            showStorageToast()
            return false
        }

        workflowState = "机械臂检测"
        storageState = "Z轴已回升，通知F4/ESP32S3抓取零件并放到称重、电感模块，机械臂等待 "
                + settingsF4ArmResultTimeoutText()
        showStorageToast()

        if (!deviceHealth.requestF4ArmInspectionFlow(latestModelResultText, root.settingsF4ArmResultTimeoutMs)) {
            storageState = "F4机械臂检测流程未启动"
            showStorageToast()
            return false
        }

        return true
    }

    /*
     * autoVisionRequestZUp 的作用：
     *   模型检测完成后请求上下电机按固定回升步数返回识别高度。
     *
     * 主要流程：
     *   1. 只在 autoVisionNeedsZUp 为 true 时执行，避免手动检测或跳过下探时误回升。
     *   2. 使用参数页 zUpFixedSteps，方向 1 表示上升，实际正反由 F4 运行时方向映射负责。
     *   3. 如果回升步数为 0，则仅清除本地待回升标志。
     *
     * 返回值：
     *   true 表示命令已启动或无需回升；false 表示命令未能启动。
     */
    function autoVisionRequestZUp() {
        if (!autoVisionNeedsZUp) {
            autoVisionStartF4ArmInspectionAfterZUp()
            return true
        }

        var motor = cameraZMotorSetting()
        var steps = Math.floor(Number(motor.zUpFixedSteps || 0))
        var speed = autoVisionNormalizeSpeedRpm(motor.normalSpeedRpm || 0, autoVisionFallbackSpeedRpm)
        var timeoutMs = cameraZMotionTimeoutMs()

        if (steps <= 0) {
            autoVisionNeedsZUp = false
            autoVisionActuatorPhase = ""
            autoVisionPendingZMoveSteps = 0
            autoVisionPendingZMoveSpeedRpm = 0
            autoVisionPendingZMoveDirection = 0
            autoVisionLastText = "上下电机回升步数为0，本轮不回升"
            storageState = autoVisionLastText
            showStorageToast()
            autoVisionStartF4ArmInspectionAfterZUp()
            return true
        }

        workflowState = "Z轴回升"
        autoVisionActuatorPhase = "z-up"
        autoVisionPendingZMoveSteps = steps
        autoVisionPendingZMoveSpeedRpm = speed
        autoVisionPendingZMoveDirection = 1
        autoVisionLastText = "模型检测完成，上下电机回升 " + steps
                + " step，最多等待 " + (timeoutMs / 1000.0).toFixed(1)
                + " 秒后按 MP157 本地估算继续"
        storageState = autoVisionLastText
        showStorageToast()

        if (deviceHealth.sendF4ActuatorPositionMoveWithTimeout(2, 1, 0, speed, steps, 0, timeoutMs)) {
            autoVisionCommandBusy = true
            return true
        }

        autoVisionActuatorPhase = ""
        autoVisionPendingZMoveSteps = 0
        autoVisionPendingZMoveSpeedRpm = 0
        autoVisionPendingZMoveDirection = 0
        autoVisionLastText = "上下电机回升命令未启动，请手动复位"
        storageState = autoVisionLastText
        showStorageToast()
        return false
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
     * formatF4ToastText 的作用：
     *   把所有 F407 二进制协议回包转换成底部提示条统一格式。
     *
     * 主要流程：
     *   1. 先把输入转换成字符串，避免 C++ 返回空详情时 QML 拼接出 undefined。
     *   2. 如果内容已经带有 `F4:` 或 `F4：` 前缀，就原样返回，防止连续回调重复叠加前缀。
     *   3. 其它 F4 回包统一加 `F4:`，让现场人员一眼知道底部提示来自 F407。
     *
     * 参数：
     *   text 是 ACK、NACK、STATUS_REPORT、FAULT_REPORT 或串口失败原因的中文摘要。
     *
     * 返回值：
     *   返回可以直接放入 storageState 的底部提示文本。
     */
    function formatF4ToastText(text) {
        var detail = (text === undefined || text === null) ? "" : ("" + text)
        if (detail.length === 0) {
            detail = "无回包详情"
        }

        if (detail.indexOf("F4:") === 0 || detail.indexOf("F4：") === 0) {
            return detail
        }

        return "F4: " + detail
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
     * sendManualBeltCommand 的作用：
     *   把手动页传送带按钮转换为 F407 二进制传送带命令，并通过 MP157 的 `/dev/ttySTM2` 真实下发。
     *
     * 主要流程：
     *   1. 记录正在等待的命令，防止同一时间重复点击多个传送带动作。
     *   2. 调用 C++ `DeviceHealthController::sendF4BeltCommand()`，由 C++ 负责二进制组帧、串口互斥和 ACK/NACK/STATUS_REPORT 读取。
     *   3. 如果 C++ 立即拒绝启动线程，则清空在途命令并把失败写入命令日志。
     *
     * 参数：
     *   commandText 是按钮语义名称，例如 BELT_MANUAL_SCAN、BELT_MANUAL_STOP、QUERY_STATUS；底层不会把这些字符串写给 F407。
     *   label 是界面按钮文本，用于写入手动命令日志。
     *   pendingState 是命令发出后界面先显示的等待状态。
     *
     * 返回值：
     *   true 表示命令线程已启动；false 表示命令没有被送入 C++ 串口层。
     */
    function sendManualBeltCommand(commandText, label, pendingState) {
        if (manualPendingF4Command !== "") {
            manualLastAckText = "F4命令发送中：" + manualPendingF4Command
            storageState = formatF4ToastText(manualLastAckText)
            appendManualCommandLog(label, "传送带", manualLastAckText)
            showStorageToast()
            return false
        }

        manualPendingF4Command = commandText
        manualBeltCommandText = commandText
        manualBeltState = pendingState
        manualLastAckText = "正在下发 " + commandText + " 到 F407"
        storageState = formatF4ToastText(manualLastAckText)
        appendManualCommandLog(label, "传送带", manualLastAckText)
        showStorageToast()

        if (!deviceHealth.sendF4BeltCommand(commandText)) {
            manualPendingF4Command = ""
            manualBeltState = "发送失败"
            manualLastAckText = "F4拒绝启动传送带命令：" + commandText
            storageState = formatF4ToastText(manualLastAckText)
            appendManualCommandLog(label, "传送带", manualLastAckText)
            showStorageToast()
            return false
        }

        return true
    }

    /*
     * openManualMotorPopup 的作用：
     *   打开三轴手动电机控制弹窗，并切换到指定电机页。
     *
     * 参数：
     *   pageIndex 是三轴页号，0=传送带，1=摄像头左右，2=摄像头上下。
     *
     * 返回值：
     *   无返回值；函数只更新弹窗显示状态。
     */
    function openManualMotorPopup(pageIndex) {
        var motors = detectSettings.stepperMotorSettings
        var count = motors && motors.length > 0 ? motors.length : 3
        manualMotorPageIndex = Math.max(0, Math.min(count - 1, pageIndex))
        manualMotorPopup = true
        manualLastAckText = "已打开三轴手动控制弹窗：" + stepperMotorPageNames[manualMotorPageIndex]
        storageState = manualLastAckText
        showStorageToast()
    }

    /*
     * manualMotorSetting 的作用：
     *   读取三轴手动弹窗当前页的电机配置。
     *
     * 返回值：
     *   返回当前页电机配置对象；配置缺失时返回空对象。
     */
    function manualMotorSetting() {
        var motors = detectSettings.stepperMotorSettings
        if (!motors || manualMotorPageIndex < 0 || manualMotorPageIndex >= motors.length) {
            return {}
        }
        return motors[manualMotorPageIndex]
    }

    /*
     * manualMotorActionButtons 的作用：
     *   根据当前三轴手动页生成按钮模型，避免上下轴新增“回原位”后仍按固定三按钮宽度布局。
     *
     * 主要流程：
     *   1. 传送带保持后退/停止/前进三按钮，摄像头左右轴保持左移/停止/右移三按钮。
     *   2. 摄像头上下轴增加“回原位”，该按钮复用上升固定步数，便于下降后快速回到识别高度。
     *
     * 返回值：
     *   返回 QML Repeater 可直接使用的数组，每项包含 text、direction、action 和 color。
     */
    function manualMotorActionButtons() {
        if (manualMotorPageIndex === 2) {
            return [
                {"text": "下降", "direction": 0, "action": "move", "color": "#5aa7ff"},
                {"text": "停止", "direction": -1, "action": "stop", "color": root.accentRed},
                {"text": "上升", "direction": 1, "action": "move", "color": root.accentGreen},
                {"text": "回原位", "direction": 1, "action": "return", "color": root.accentAmber}
            ]
        }

        return [
            {"text": manualMotorPopupPanel.negativeLabel, "direction": 0, "action": "move", "color": "#5aa7ff"},
            {"text": "停止", "direction": -1, "action": "stop", "color": root.accentRed},
            {"text": manualMotorPopupPanel.positiveLabel, "direction": 1, "action": "move", "color": root.accentGreen}
        ]
    }

    /*
     * sendManualActuatorVelocityMove 的作用：
     *   把手动弹窗中传送带和摄像头左右轴的方向按钮转换为 ACTUATOR_VEL_MOVE 连续速度命令。
     *
     * 主要流程：
     *   1. 从当前页读取 normalSpeedRpm，速度为 0 时不下发，避免界面显示运动但电机保持停止。
     *   2. 设置 manualPendingF4Command，防止上一条串口 ACK/NACK 未返回时重复点击。
     *   3. 调用 C++ sendF4ActuatorVelocityMove()，F4 收到后保持速度模式运行，直到停止按钮下发 ACTUATOR_STOP。
     *
     * 参数：
     *   actuator 是执行器编号，0=传送带，1=摄像头左右；上下轴不允许连续速度模式。
     *   direction 是方向编号，传送带 0=后退/1=前进，左右轴 0=左移/1=右移。
     *   label 是按钮文本，用于命令日志显示。
     *
     * 返回值：
     *   true 表示命令线程已启动；false 表示命令未能进入 C++ 串口层。
     */
    function sendManualActuatorVelocityMove(actuator, direction, label) {
        var motor = manualMotorSetting()
        var speed = Math.floor(Number(motor.normalSpeedRpm || 0))
        var commandText = "ACTUATOR_VEL_MOVE_" + actuator + "_" + direction

        if (manualPendingF4Command !== "") {
            manualLastAckText = "F4命令发送中：" + manualPendingF4Command
            storageState = formatF4ToastText(manualLastAckText)
            appendManualCommandLog(label, "三轴电机", manualLastAckText)
            showStorageToast()
            return false
        }

        if (speed <= 0) {
            manualLastAckText = "持续运动速度为0，请先在参数设置中把当前电机常规速度改为1~5000 rpm"
            storageState = formatF4ToastText(manualLastAckText)
            appendManualCommandLog(label, stepperMotorPageNames[manualMotorPageIndex], manualLastAckText)
            showStorageToast()
            return false
        }

        manualPendingF4Command = commandText
        manualLastAckText = "正在下发持续" + label + "：actuator=" + actuator + " speed=" + speed + "rpm"
        storageState = formatF4ToastText(manualLastAckText)
        appendManualCommandLog(label, stepperMotorPageNames[manualMotorPageIndex], manualLastAckText)
        showStorageToast()

        if (!deviceHealth.sendF4ActuatorVelocityMove(actuator, direction, speed, 0)) {
            manualPendingF4Command = ""
            manualLastAckText = "F4拒绝启动执行器命令：" + commandText
            storageState = formatF4ToastText(manualLastAckText)
            appendManualCommandLog(label, stepperMotorPageNames[manualMotorPageIndex], manualLastAckText)
            showStorageToast()
            return false
        }

        return true
    }

    /*
     * clearManualPendingMotionContext 的作用：
     *   清空手动执行器命令附带的本地运动上下文。
     *
     * 主要流程：
     *   1. 在命令失败、命令完成或 STOP 抢占时调用。
     *   2. 只清 QML 本地等待信息，不修改 F4 串口层 busy 标志，避免伪造串口空闲状态。
     *
     * 返回值：
     *   无返回值；函数只更新 QML 本地属性。
     */
    function clearManualPendingMotionContext() {
        manualPendingActuatorId = -1
        manualPendingZDirection = -1
        manualPendingZSteps = 0
        manualPendingZReturnHome = false
    }

    /*
     * updateManualCameraZOffsetAfterMove 的作用：
     *   在上下轴位置命令真正成功后，更新 MP157 对“当前点距离设零点”的本地估算。
     *
     * 主要流程：
     *   1. 如果还没有成功设零，说明 MP157 没有可信基准，不更新偏移。
     *   2. 如果本次命令是“回原位”，成功后直接把偏移归零，避免重复点击继续上升。
     *   3. 普通下降让偏移增加，普通上升让偏移减少，后续“回原位”按偏移反向移动。
     *
     * 参数：
     *   direction 是上下轴逻辑方向，0=下降，1=上升。
     *   steps 是本次实际下发的相对移动步数，单位 step。
     *   returnHome 表示本次命令是否由“回原位”触发。
     *
     * 返回值：
     *   无返回值；函数只更新 manualCameraZOffsetSteps。
     */
    function updateManualCameraZOffsetAfterMove(direction, steps, returnHome) {
        var safeSteps = Math.max(0, Math.floor(Number(steps || 0)))

        if (!manualCameraZZeroKnown) {
            return
        }

        if (returnHome) {
            manualCameraZOffsetSteps = 0
            return
        }

        if (direction === 0) {
            manualCameraZOffsetSteps = Math.floor(Number(manualCameraZOffsetSteps || 0)) + safeSteps
        } else {
            manualCameraZOffsetSteps = Math.floor(Number(manualCameraZOffsetSteps || 0)) - safeSteps
        }

        if (Math.abs(manualCameraZOffsetSteps) < 1) {
            manualCameraZOffsetSteps = 0
        }
    }

    /*
     * sendManualActuatorZFixedMove 的作用：
     *   把上下电机手动“下降/上升”按钮转换为固定步数 ACTUATOR_POS_MOVE。
     *
     * 主要流程：
     *   1. direction=0 时读取 zDownFixedSteps，direction=1 时读取 zUpFixedSteps。
     *   2. 如果调用方传入 overrideSteps，则用于“回原位”按本地零点偏移移动，而不是固定上升。
     *   3. 步数为 0 时拒绝下发，让现场先到参数页配置固定下降/上升值。
     *   4. 调用 C++ sendF4ActuatorPositionMove()，F4 只执行一次固定步数位置运动。
     *
     * 参数：
     *   direction 是上下轴方向，0=下降，1=上升。
     *   label 是按钮文本，用于命令日志显示。
     *   overrideSteps 是可选步数覆盖值；未传时使用参数页固定步数。
     *   returnHome 是可选标志，true 表示本次命令用于回到最近一次设零点。
     *
     * 返回值：
     *   true 表示命令线程已启动；false 表示命令未能进入 C++ 串口层。
     */
    function sendManualActuatorZFixedMove(direction, label, overrideSteps, returnHome) {
        var motor = manualMotorSetting()
        var speed = Math.floor(Number(motor.normalSpeedRpm || 0))
        var hasOverrideSteps = overrideSteps !== undefined && overrideSteps !== null
        var steps = hasOverrideSteps
                ? Math.floor(Number(overrideSteps || 0))
                : (direction === 0
                ? Math.floor(Number(motor.zDownFixedSteps || 0))
                : Math.floor(Number(motor.zUpFixedSteps || 0)))
        var returnHomeCommand = returnHome === true
        var commandText = returnHomeCommand ? "ACTUATOR_POS_MOVE_Z_HOME" : ("ACTUATOR_POS_MOVE_Z_" + direction)

        if (manualPendingF4Command !== "") {
            manualLastAckText = "F4命令发送中：" + manualPendingF4Command
            storageState = formatF4ToastText(manualLastAckText)
            appendManualCommandLog(label, "上下电机", manualLastAckText)
            showStorageToast()
            return false
        }

        if (steps <= 0) {
            manualLastAckText = (direction === 0 ? "下降" : "上升")
                    + "固定步数为0，请先在参数设置中配置上下电机"
            storageState = formatF4ToastText(manualLastAckText)
            appendManualCommandLog(label, "上下电机", manualLastAckText)
            showStorageToast()
            return false
        }

        manualPendingF4Command = commandText
        manualPendingActuatorId = 2
        manualPendingZDirection = direction
        manualPendingZSteps = steps
        manualPendingZReturnHome = returnHomeCommand
        manualLastAckText = "正在下发上下电机" + label + "：steps=" + steps + " speed=" + speed + "rpm"
        storageState = formatF4ToastText(manualLastAckText)
        appendManualCommandLog(label, "上下电机", manualLastAckText)
        showStorageToast()

        if (!deviceHealth.sendF4ActuatorPositionMove(2, direction, 0, speed, steps, 0)) {
            manualPendingF4Command = ""
            clearManualPendingMotionContext()
            manualLastAckText = "F4拒绝启动上下电机固定步数命令：" + commandText
            storageState = formatF4ToastText(manualLastAckText)
            appendManualCommandLog(label, "上下电机", manualLastAckText)
            showStorageToast()
            return false
        }

        return true
    }

    /*
     * sendManualActuatorMove 的作用：
     *   统一处理手动弹窗方向按钮，并按执行器类型选择连续速度模式或固定步数位置模式。
     *
     * 分流规则：
     *   1. 传送带和摄像头左右轴使用 ACTUATOR_VEL_MOVE，按一次持续运动，停止键结束。
     *   2. 摄像头上下轴使用 ACTUATOR_POS_MOVE，下降取 zDownFixedSteps，上升取 zUpFixedSteps。
     */
    function sendManualActuatorMove(actuator, direction, label) {
        if (actuator === 2) {
            return sendManualActuatorZFixedMove(direction, label)
        }

        return sendManualActuatorVelocityMove(actuator, direction, label)
    }

    /*
     * sendManualActuatorZReturnHome 的作用：
     *   手动上下轴下降或上升后，按最近一次“设当前位置为零点”记录的本地偏移回到零点。
     *
     * 主要流程：
     *   1. 如果当前还没有成功设零，则拒绝回原位，避免继续盲目上升。
     *   2. 如果上一条上下轴动作还没有完成回执，则拒绝回原位，避免用尚未更新的旧偏移误判已经在零点。
     *   3. 如果本地偏移已经是 0，则只提示当前位置就是零点，不再发送电机命令。
     *   4. 如果偏移为正，说明相对零点下降过，回原位要上升；偏移为负则反向下降。
     *
     * 返回值：
     *   true 表示回原位位置命令已启动；false 表示无需移动、缺少零点或串口层拒绝。
     */
    function sendManualActuatorZReturnHome(label) {
        if (!manualCameraZZeroKnown) {
            manualLastAckText = "请先在参数设置中对上下电机点击设当前位置为零点，再使用回原位"
            storageState = formatF4ToastText(manualLastAckText)
            appendManualCommandLog(label, "上下电机", manualLastAckText)
            showStorageToast()
            return false
        }

        if (manualPendingF4Command !== "") {
            /*
             * 上下轴偏移只在 ACTUATOR_POS_MOVE 成功完成后更新。
             * 如果上一条上升/下降还在等待 ACK、DONE 或 MP157 本地估算完成，
             * 此时 manualCameraZOffsetSteps 仍是旧值，不能据此判断“已经在零点”。
             */
            manualLastAckText = "F4命令发送中：" + manualPendingF4Command
                    + "，等待上下轴上一条动作完成后再回原位；若现场需要立即中断，请先点停止并重新设零"
            storageState = formatF4ToastText(manualLastAckText)
            appendManualCommandLog(label, "上下电机", manualLastAckText)
            showStorageToast()
            return false
        }

        var offsetSteps = Math.floor(Number(manualCameraZOffsetSteps || 0))
        var direction = offsetSteps > 0 ? 1 : 0
        var steps = Math.abs(offsetSteps)

        if (steps <= 0) {
            manualLastAckText = "上下电机当前位置已经是零点，不再发送回原位命令"
            storageState = formatF4ToastText(manualLastAckText)
            appendManualCommandLog(label, "上下电机", manualLastAckText)
            showStorageToast()
            return false
        }

        return sendManualActuatorZFixedMove(direction, label, steps, true)
    }

    /*
     * sendManualActuatorStop 的作用：
     *   把三轴手动停止或模拟急停转换为 ACTUATOR_STOP 二进制命令。
     *   STOP 是安全动作，不等待上一条普通运动 ACK，直接走 C++ 写入即返回通道。
     *
     * 参数：
     *   actuator 是执行器编号，0/1/2 表示单轴，255 表示全部。
     *   label 是按钮文本，用于命令日志显示。
     *
     * 返回值：
     *   true 表示命令线程已启动；false 表示命令未能进入 C++ 串口层。
     */
    function sendManualActuatorStop(actuator, label) {
        var commandText = actuator === 255 ? "ACTUATOR_STOP_ALL" : ("ACTUATOR_STOP_" + actuator)
        var targetText = actuator === 255 ? "全部执行器" : stepperMotorPageNames[Math.max(0, Math.min(2, actuator))]
        var stopTouchesCameraZ = actuator === 2 || actuator === 255

        manualLastAckText = "正在立即写入 " + commandText + " 到 F407，不等待上一条运动ACK"
        storageState = formatF4ToastText(manualLastAckText)
        appendManualCommandLog(label, targetText, manualLastAckText)
        showStorageToast()

        /*
         * STOP 是安全动作，点击后立即取消 QML 本地等待中的手动命令。
         * 这样旧的 ACTUATOR_VEL_MOVE/ACTUATOR_POS_MOVE ACK 即使稍后回来，也不会覆盖“已停止”的现场提示。
         */
        manualPendingF4Command = ""
        clearManualPendingMotionContext()

        if (stopTouchesCameraZ) {
            /*
             * 上下轴在位置运动中被 STOP 打断后，MP157 无法只靠相对步数知道最终停在哪。
             * 因此清掉本地零点可信标志，要求操作者在当前位置重新设零后再使用“回原位”。
             */
            manualCameraZZeroKnown = false
            manualCameraZOffsetSteps = 0
        }

        if (!deviceHealth.sendF4ActuatorStopNow(actuator, 0)) {
            manualLastAckText = "F4拒绝启动停止命令：" + commandText
            storageState = formatF4ToastText(manualLastAckText)
            appendManualCommandLog(label, targetText, manualLastAckText)
            showStorageToast()
            return false
        }

        return true
    }

    /*
     * manualActionAllowed 的作用：
     *   集中判断某个手动动作当前是否允许执行，让按钮置灰条件和点击保护分支使用同一套安全规则。
     *
     * 主要流程：
     *   1. 检测当前帧、刷新状态、进入手动、停止和清故障属于安全动作，可以在未进入手动模式时执行。
     *   2. 其它运动或执行器动作必须先进入手动模式。
     *   3. 急停状态下只允许停止、刷新状态和清故障。
     *   4. 三轴运动按钮只在进入手动模式且未急停时允许点击。
     *
     * 参数：
     *   action 是手动控制动作标识。
     *
     * 返回值：
     *   返回 true 表示按钮可以执行；返回 false 表示界面应置灰或点击时给出禁止原因。
     */
    function manualActionAllowed(action) {
        if (action === "enter-manual" || action === "stop" || action === "detect-frame"
                || action === "refresh" || action === "clear-alarm" || action === "emergency-toggle"
                || action === "open-motor-popup") {
            return true
        }

        if (!manualMode) {
            return false
        }

        if (manualEmergencyStop) {
            return false
        }

        return true
    }

    /*
     * handleManualAction 的作用：
     *   统一处理手动控制页所有按钮点击；传送带会真实下发 F407 二进制 ACK/NACK 协议命令，其它入口只保留检测和标记辅助。
     *
     * 主要流程：
     *   1. 先执行手动模式、急停和回零等安全保护判断。
     *   2. 检测当前帧动作复用首页双模型检测链路，避免只保存无检测结果的图片。
     *   3. 传送带兼容按钮通过 sendManualBeltCommand() 走旧手动命令。
     *   4. 三轴弹窗和模拟急停通过 ACTUATOR_POS_MOVE/ACTUATOR_STOP 走新执行器命令。
     *
     * 参数：
     *   action 是动作标识，例如 belt-scan、belt-info、detect-frame。
     *   label 是界面显示的按钮文字，用于日志和提示。
     *
     * 返回值：
     *   无返回值；函数会更新手动页状态、storageState 和命令日志。
     */
    function handleManualAction(action, label) {
        if (!manualMode && action !== "enter-manual" && action !== "stop"
                && action !== "detect-frame" && action !== "refresh"
                && action !== "clear-alarm" && action !== "emergency-toggle"
                && action !== "open-motor-popup") {
            manualLastAckText = "请先进入手动模式"
            storageState = manualLastAckText
            appendManualCommandLog(label, "安全联锁", manualLastAckText)
            showStorageToast()
            return
        }

        if (manualEmergencyStop && action !== "stop" && action !== "refresh"
                && action !== "clear-alarm" && action !== "emergency-toggle"
                && action !== "open-motor-popup") {
            manualLastAckText = "急停中，禁止执行运动命令"
            storageState = manualLastAckText
            appendManualCommandLog(label, "急停保护", manualLastAckText)
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
            }
            storageState = manualLastAckText
            appendManualCommandLog(label, "模式切换", manualLastAckText)
            showStorageToast()
            return
        }

        if (action === "open-motor-popup") {
            openManualMotorPopup(manualMotorPageIndex)
            appendManualCommandLog(label, "三轴电机", manualLastAckText)
            return
        }

        if (action === "belt-scan") {
            sendManualBeltCommand("BELT_MANUAL_SCAN", label, "巡航下发中")
            return
        } else if (action === "belt-info") {
            sendManualBeltCommand("QUERY_STATUS", label, "查询中")
            return
        } else if (action === "stop") {
            manualBeltState = "停止"
            sendManualBeltCommand("BELT_MANUAL_STOP", label, "停止下发中")
            return
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
                sendManualActuatorStop(255, label)
                return
            }
            manualLastAckText = manualEmergencyStop ? "急停已按下，运动禁止" : "急停已释放，等待清故障"
        } else if (action === "clear-alarm") {
            manualEmergencyStop = false
            manualLastAckText = "界面故障标志已清除，真实联锁仍以 F4 为准"
        } else if (action === "refresh") {
            manualLastAckText = "已刷新界面状态，真实F4状态请看传送带查询或健康心跳"
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
     * settingsF4ArmResultTimeoutText 的作用：
     *   把机械臂主动结果等待超时从毫秒转换成参数页显示的秒数。
     *
     * 返回值：
     *   返回例如 75秒 的短文本，供摘要、按钮和日志复用。
     */
    function settingsF4ArmResultTimeoutText() {
        return Math.floor(root.settingsF4ArmResultTimeoutMs / 1000) + "秒"
    }

    /*
     * settingsSummaryText 的作用：
     *   生成参数设置页右侧摘要，让操作员应用参数前能快速确认关键值。
     *
     * 返回值：
     *   返回包含零件类型、模型阈值、复核阈值和上传策略的短文本。
     */
    function settingsSummaryText() {
        return settingsPartType
                + "  模型" + settingsThresholdText(settingsDecisionThreshold)
                + "  复核" + settingsThresholdText(settingsReviewThreshold)
                + "  ROI" + settingsRoiSize
                + "  UNet>" + settingsSegmentMinPixels + "px"
                + "  电机ID" + stepperMotorCompactSummary()
                + "  带" + conveyorScanSpeedRpm() + "/" + conveyorTrackSpeedRpm() + "rpm"
                + "  臂等" + settingsF4ArmResultTimeoutText()
                + "  上传" + (settingsUploadEnabled ? "自动" : "手动")
    }

    /*
     * settingsLogText 的作用：
     *   生成可写入 /mnt/sdcard/logs/qt_settings_YYYYMMDD.log 的完整参数日志正文。
     *
     * 主要流程：
     *   1. 记录本次动作名称、时间、JSON 路径和 C++ 保存结果。
     *   2. 逐项写出零件、模型阈值、复核阈值、ROI、UNet 像素阈值、overlay 透明度和上传策略。
     *   3. 写出下一次检测会使用的命令行参数，方便日志查看页直接确认真实生效范围。
     *
     * 参数：
     *   actionLabel 是“保存配置”或“导出摘要”等中文动作名。
     *   actionResult 是刚执行完动作后返回给界面的状态文本。
     *
     * 返回值：
     *   返回多行中文文本，由 C++ 追加到每日参数日志。
     */
    function settingsLogText(actionLabel, actionResult) {
        var lines = [
            "STM32MP157 Qt Settings Summary",
            "action=" + actionLabel,
            "time=" + currentTimeText,
            "config_path=" + settingsConfigPath,
            "action_result=" + actionResult,
            "part_type=" + settingsPartType,
            "model_threshold=" + (settingsDecisionThreshold / 1000.0).toFixed(3) + " (" + settingsThresholdText(settingsDecisionThreshold) + ")",
            "review_threshold=" + (settingsReviewThreshold / 1000.0).toFixed(3) + " (" + settingsThresholdText(settingsReviewThreshold) + ")",
            "roi_size=" + settingsRoiSize,
            "segment_min_pixels=" + settingsSegmentMinPixels,
            "overlay_alpha=" + settingsOverlayAlpha.toFixed(2),
            "auto_upload_enabled=" + (settingsUploadEnabled ? "true" : "false"),
            "f4_arm_result_timeout_ms=" + settingsF4ArmResultTimeoutMs,
            stepperMotorLogText(),
            "classify_args=--roi " + settingsRoiSize + " --bad-threshold " + (settingsDecisionThreshold / 1000.0).toFixed(3),
            "segment_args=--roi " + settingsRoiSize + " --alpha " + settingsOverlayAlpha.toFixed(2) + " --min-defect-pixels " + settingsSegmentMinPixels,
            "summary=" + settingsSummaryText()
        ]
        return lines.join("\n")
    }

    /*
     * settingsNextPartType 的作用：
     *   在三种真实垫圈零件之间循环，避免参数页继续出现非真实零件名。
     *
     * 返回值：
     *   返回下一种零件名称；如果当前值异常，回到列表第一项。
     */
    function settingsNextPartType() {
        var index = settingsSupportedPartTypes.indexOf(settingsPartType)
        if (index < 0) {
            return settingsSupportedPartTypes[0]
        }
        return settingsSupportedPartTypes[(index + 1) % settingsSupportedPartTypes.length]
    }

    /*
     * currentStepperMotorSetting 的作用：
     *   返回步进电机弹窗当前页对应的电机参数。
     *
     * 主要流程：
     *   1. 从 detectSettings.stepperMotorSettings 读取 C++ 当前配置，避免使用过期缓存。
     *   2. 如果当前页越界，返回空对象，调用方再显示占位文本。
     *
     * 返回值：
     *   返回包含 name、address、minStep、normalSpeedRpm 和 direction 的对象。
     */
    function currentStepperMotorSetting() {
        /*
         * stepperMotorSettingsRevision 参与依赖跟踪：
         *   C++ settingsChanged 后 QVariantList 会重新生成，QML 旧 map 副本不一定自动刷新。
         *   这里显式读取版本号，让所有调用 currentStepperMotorSetting() 的绑定都能重新取最新对象。
         */
        var revision = stepperMotorSettingsRevision
        var motors = detectSettings.stepperMotorSettings
        if (!motors || stepperMotorPageIndex < 0 || stepperMotorPageIndex >= motors.length) {
            return {}
        }
        return motors[stepperMotorPageIndex]
    }

    /*
     * conveyorMotorSetting 的作用：
     *   读取参数页第一台传送带电机配置，供自动 ROI 前后微调和手动三轴弹窗复用。
     *
     * 返回值：
     *   返回传送带电机配置对象；配置缺失时返回空对象，调用方会使用保守默认值。
     */
    function conveyorMotorSetting() {
        var motors = detectSettings.stepperMotorSettings
        if (!motors || motors.length < 1) {
            return {}
        }
        return motors[0]
    }

    /*
     * cameraLateralMotorSetting 的作用：
     *   读取参数页第二台摄像头左右电机配置，供自动 ROI 左右微调和手动三轴弹窗复用。
     *
     * 返回值：
     *   返回摄像头左右电机配置对象；配置缺失时返回空对象，调用方会使用保守默认值。
     */
    function cameraLateralMotorSetting() {
        var motors = detectSettings.stepperMotorSettings
        if (!motors || motors.length < 2) {
            return {}
        }
        return motors[1]
    }

    /*
     * cameraZMotorSetting 的作用：
     *   读取参数页第三台摄像头上下电机配置，供自动下探/回升和手动三轴弹窗复用。
     *
     * 返回值：
     *   返回摄像头上下电机配置对象；配置缺失时返回空对象，调用方会使用保守默认值。
     */
    function cameraZMotorSetting() {
        var motors = detectSettings.stepperMotorSettings
        if (!motors || motors.length < 3) {
            return {}
        }
        return motors[2]
    }

    /*
     * cameraZMotionTimeoutMs 的作用：
     *   从参数页“摄像头上下电机”的 zMotionTimeoutMs 读取自动 Z 轴下降/回升最大等待时间。
     *
     * 主要流程：
     *   1. 优先读取 DetectSettingsController 暴露的第三台电机参数。
     *   2. 配置缺失时使用 autoVisionZMotionMaximumWaitMs 作为兼容默认值。
     *   3. 最终限制在 1~60 秒，避免误输入导致流程立刻推进或长时间卡住串口线程。
     *
     * 返回值：
     *   返回毫秒数，用于 C++ 等待 F4 ACTUATOR_MOVE_DONE 或 MP157 本地估算完成的最大上限。
     */
    function cameraZMotionTimeoutMs() {
        var motor = cameraZMotorSetting()
        var timeoutMs = Math.floor(Number(motor.zMotionTimeoutMs || autoVisionZMotionMaximumWaitMs))

        return Math.max(1000, Math.min(60000, timeoutMs))
    }

    /*
     * stepperMotorAddressText 的作用：
     *   把电机地址显示成十进制和十六进制双格式，方便对照 F4/Emm42 文档。
     *
     * 参数：
     *   motor 是 currentStepperMotorSetting() 返回的当前电机对象。
     *
     * 返回值：
     *   返回例如 `2 / 0x02` 的地址文本；缺失时返回 `--`。
     */
    function stepperMotorAddressText(motor) {
        if (!motor || motor.address === undefined) {
            return "--"
        }
        return motor.address + " / " + motor.addressHex
    }

    /*
     * stepperMotorDirectionText 的作用：
     *   把方向映射整数转成界面文案。
     *
     * 参数：
     *   direction 是 C++ 保存的方向值，1 表示正向，-1 表示反向。
     *
     * 返回值：
     *   返回“正向”或“反向”。
     */
    function stepperMotorDirectionText(direction) {
        return direction >= 0 ? "正向" : "反向"
    }

    /*
     * stepperMotorCompactSummary 的作用：
     *   生成参数页摘要中的短电机 ID 串，避免在顶部小条中展示完整表单。
     *
     * 返回值：
     *   返回 `1/2/3` 这种短地址组合；没有配置时返回 `--`。
     */
    function stepperMotorCompactSummary() {
        var motors = detectSettings.stepperMotorSettings
        var ids = []
        if (!motors || motors.length === 0) {
            return "--"
        }

        for (var index = 0; index < motors.length; ++index) {
            ids.push(motors[index].address)
        }
        return ids.join("/")
    }

    /*
     * stepperMotorRoleAddressSummary 的作用：
     *   生成保存并下发时展示给现场的三台电机角色地址，避免只看到 F4 ACK 但不知道本次发了哪个 ID。
     *
     * 返回值：
     *   返回 `传送带=1，左右=3，上下=2` 这种摘要；配置缺失时返回占位文本。
     */
    function stepperMotorRoleAddressSummary() {
        var motors = detectSettings.stepperMotorSettings
        if (!motors || motors.length < 3) {
            return "传送带/左右/上下=--"
        }

        return "传送带=" + motors[0].address
                + "，左右=" + motors[1].address
                + "，上下=" + motors[2].address
    }

    /*
     * sendStepperActuatorHome 的作用：
     *   参数设置页请求 F4 把当前页步进电机的当前位置设为新的零点。
     *
     * 主要流程：
     *   1. 使用 stepperMotorPageIndex 作为 actuator 编号，和协议定义 0=传送带、1=左右轴、2=上下轴一致。
     *   2. 设置 stepperHomeSending 和 stepperHomePendingCommand，防止重复点击导致同一电机多次清零。
     *   3. 调用 C++ sendF4ActuatorHome() 发送 ACTUATOR_HOME 0x53，等待 ACK/NACK 后更新参数页提示。
     *
     * 返回值：
     *   true 表示命令线程已启动；false 表示参数非法、串口忙或线程创建失败。
     */
    function sendStepperActuatorHome() {
        var actuator = stepperMotorPageIndex
        var motor = currentStepperMotorSetting()
        var commandText = "ACTUATOR_HOME_" + actuator

        if (stepperHomeSending || stepperHomePendingCommand !== "") {
            stepperMotorResultText = "当前位置设零命令正在等待 F4 回执：" + stepperHomePendingCommand
            storageState = formatF4ToastText(stepperMotorResultText)
            showStorageToast()
            return false
        }

        if (actuator < 0 || actuator > 2) {
            stepperMotorResultText = "当前位置设零失败：当前电机页无效"
            storageState = stepperMotorResultText
            showStorageToast()
            return false
        }

        stepperHomeSending = true
        stepperHomePendingCommand = commandText
        stepperMotorResultText = "正在请求 F4 将 " + (motor.name || ("电机" + actuator)) + " 当前位置设为零点"
        settingsLastActionText = stepperMotorResultText
        storageState = formatF4ToastText(stepperMotorResultText)
        showStorageToast()

        if (!deviceHealth.sendF4ActuatorHome(actuator, 0)) {
            stepperHomeSending = false
            stepperHomePendingCommand = ""
            stepperMotorResultText = "F4拒绝启动当前位置设零命令：" + commandText
            settingsLastActionText = stepperMotorResultText
            storageState = formatF4ToastText(stepperMotorResultText)
            showStorageToast()
            return false
        }

        return true
    }

    /*
     * stepperMotorLogText 的作用：
     *   把三台步进电机参数展开为参数日志字段，便于 SSH 复盘当前保存值。
     *
     * 返回值：
     *   返回多行 key=value 文本，会被 settingsLogText() 合并写入 qt_settings 日志。
     */
    function stepperMotorLogText() {
        var motors = detectSettings.stepperMotorSettings
        var lines = []
        if (!motors || motors.length === 0) {
            return "stepper_motors=missing"
        }

        for (var index = 0; index < motors.length; ++index) {
            var motor = motors[index]
            var prefix = "stepper_motor[" + index + "]"
            lines.push(prefix + ".name=" + motor.name)
            lines.push(prefix + ".role=" + motor.role)
            lines.push(prefix + ".serial=" + motor.serialName)
            lines.push(prefix + ".address=" + motor.address)
            lines.push(prefix + ".min_step=" + motor.minStep)
            lines.push(prefix + ".normal_speed_rpm=" + motor.normalSpeedRpm)
            lines.push(prefix + ".scan_speed_rpm=" + (motor.scanSpeedRpm || 0))
            lines.push(prefix + ".direction=" + motor.direction + " (" + root.stepperMotorDirectionText(motor.direction) + ")")
            lines.push(prefix + ".z_down_fixed_steps=" + (motor.zDownFixedSteps || 0))
            lines.push(prefix + ".z_up_fixed_steps=" + (motor.zUpFixedSteps || 0))
            lines.push(prefix + ".z_motion_timeout_ms=" + (motor.zMotionTimeoutMs || 10000))
        }
        return lines.join("\n")
    }

    /*
     * openStepperMotorPopup 的作用：
     *   打开步进电机参数弹窗，并把页面定位到用户选择的电机。
     *
     * 参数：
     *   pageIndex 是希望打开的页号，非法时回到 0 页。
     *
     * 返回值：
     *   无返回值；函数只更新弹窗显隐、页号和提示文本。
     */
    function openStepperMotorPopup(pageIndex) {
        var motors = detectSettings.stepperMotorSettings
        var maxIndex = motors && motors.length > 0 ? motors.length - 1 : 0
        stepperMotorPageIndex = Math.max(0, Math.min(maxIndex, pageIndex))
        stepperMotorResultText = "保存并下发后会写入 JSON，并发送 STEPPER_PARAM_SET 给 F4；F4 只更新运行内存"
        stepperSpeedEditorVisible = false
        stepperSpeedEditKey = "normalSpeedRpm"
        stepperStepEditorVisible = false
        stepperStepEditKey = ""
        stepperSpeedInputText = "" + (currentStepperMotorSetting().normalSpeedRpm || 0)
        stepperMotorPopupVisible = true
    }

    /*
     * openStepperSpeedEditor 的作用：
     *   打开速度数字键盘，并把当前电机的目标速度字段拷贝到输入框。
     *
     * 参数：
     *   key 是 normalSpeedRpm 或 scanSpeedRpm；scanSpeedRpm 只在传送带页使用。
     *
     * 返回值：
     *   无返回值；函数只更新 stepperSpeedInputText 和 stepperSpeedEditorVisible。
     */
    function openStepperSpeedEditor(key) {
        var motor = currentStepperMotorSetting()
        var editKey = key || "normalSpeedRpm"

        if (editKey !== "normalSpeedRpm" && editKey !== "scanSpeedRpm") {
            stepperMotorResultText = "速度字段无效：" + editKey
            storageState = stepperMotorResultText
            showStorageToast()
            return
        }

        stepperSpeedEditKey = editKey
        stepperSpeedInputText = "" + Math.floor(Number(motor[editKey] || 0))
        stepperStepEditorVisible = false
        stepperSpeedEditorVisible = true
    }

    /*
     * stepperStepEditorIsTimeout 的作用：
     *   判断当前复用数字键盘是否正在编辑 Z 轴最大等待超时。
     *
     * 返回值：
     *   true 表示当前字段为 zMotionTimeoutMs；false 表示当前字段为下探/回升步数。
     */
    function stepperStepEditorIsTimeout() {
        return stepperStepEditKey === "zMotionTimeoutMs"
    }

    /*
     * stepperStepEditorUnitText 的作用：
     *   返回当前数字键盘应该显示的单位文本。
     *
     * 返回值：
     *   Z 轴超时返回“秒”；固定步数返回“step”。
     */
    function stepperStepEditorUnitText() {
        return stepperStepEditorIsTimeout() ? "秒" : "step"
    }

    /*
     * stepperStepEditorMaxValue 的作用：
     *   返回当前数字键盘允许输入的最大整数。
     *
     * 返回值：
     *   Z 轴超时最大 60 秒；固定步数最大 4294967295 step。
     */
    function stepperStepEditorMaxValue() {
        return stepperStepEditorIsTimeout() ? 60 : 4294967295
    }

    /*
     * stepperStepEditorMinValue 的作用：
     *   返回当前数字键盘允许应用的最小整数。
     *
     * 返回值：
     *   Z 轴超时最小 1 秒；固定步数允许 0 step，用于跳过下探或回升。
     */
    function stepperStepEditorMinValue() {
        return stepperStepEditorIsTimeout() ? 1 : 0
    }

    /*
     * stepperStepEditorTitleText 的作用：
     *   根据当前字段生成数字键盘标题。
     *
     * 返回值：
     *   返回可直接显示在弹窗顶部的中文标题。
     */
    function stepperStepEditorTitleText() {
        if (stepperStepEditKey === "zDownFixedSteps") {
            return "下探固定步数"
        }
        if (stepperStepEditKey === "zUpFixedSteps") {
            return "回升固定步数"
        }
        return "Z轴超时等待"
    }

    /*
     * stepperStepEditorRangeText 的作用：
     *   根据当前字段生成范围说明，避免操作者把秒和 step 混淆。
     *
     * 返回值：
     *   返回可换行显示的范围说明。
     */
    function stepperStepEditorRangeText() {
        if (stepperStepEditorIsTimeout()) {
            return "范围 1~60 秒；自动 Z 轴下降或回升未收到 F4 DONE 时，最多等待到该时间后按 MP157 本地估算继续。"
        }
        return "范围 0~4294967295 step，对应张大头42步进电机位置模式 4 字节脉冲数。"
    }

    /*
     * stepperStepEditorPendingText 的作用：
     *   生成当前数字键盘输入待应用提示。
     *
     * 返回值：
     *   返回包含数值和单位的中文提示。
     */
    function stepperStepEditorPendingText() {
        return (stepperStepEditorIsTimeout() ? "Z轴超时待应用：" : "位置步数待应用：")
                + stepperStepInputText + " " + stepperStepEditorUnitText()
    }

    /*
     * openStepperStepEditor 的作用：
     *   打开上下电机固定下探/回升步数或 Z 轴超时数字键盘，并载入当前字段值。
     *
     * 参数：
     *   key 是 zDownFixedSteps、zUpFixedSteps 或 zMotionTimeoutMs。
     *
     * 返回值：
     *   无返回值；字段非法时只更新提示，不打开数字键盘。
     */
    function openStepperStepEditor(key) {
        var motor = currentStepperMotorSetting()
        if (key !== "zDownFixedSteps" && key !== "zUpFixedSteps" && key !== "zMotionTimeoutMs") {
            stepperMotorResultText = "上下电机数字字段无效：" + key
            storageState = stepperMotorResultText
            showStorageToast()
            return
        }

        stepperStepEditKey = key
        if (stepperStepEditorIsTimeout()) {
            stepperStepInputText = "" + Math.max(1, Math.min(60, Math.floor(Number(motor.zMotionTimeoutMs || 10000) / 1000)))
        } else {
            stepperStepInputText = "" + Math.floor(Number(motor[key] || 0))
        }
        stepperStepReplaceOnNextDigit = stepperStepEditorIsTimeout()
        stepperSpeedEditorVisible = false
        stepperStepEditorVisible = true
        stepperMotorResultText = "请输入 " + stepperStepEditorTitleText()
                + "：" + stepperStepEditorMinValue()
                + "~" + stepperStepEditorMaxValue()
                + " " + stepperStepEditorUnitText()
    }

    /*
     * appendStepperSpeedDigit 的作用：
     *   向速度输入框追加一个数字，让触摸屏能输入 0~5000 rpm 任意整数。
     *
     * 主要流程：
     *   1. 把当前文本和新数字拼成候选值，并去掉多余前导零。
     *   2. 允许候选值为 0；大于 5000 时拒绝追加并提示。
     *
     * 参数：
     *   digit 是被点击的数字字符，取值为 "0" 到 "9"。
     *
     * 返回值：
     *   无返回值；函数只更新输入文本和结果提示。
     */
    function appendStepperSpeedDigit(digit) {
        var nextText = (stepperSpeedInputText + digit).replace(/^0+/, "")
        if (nextText.length === 0) {
            nextText = "0"
        }

        var nextValue = parseInt(nextText, 10)
        if (nextValue > 5000) {
            stepperMotorResultText = "常规速度范围是 0~5000 rpm"
            return
        }

        stepperSpeedInputText = nextText
        stepperMotorResultText = "速度待应用：" + stepperSpeedInputText + " rpm"
    }

    /*
     * backspaceStepperSpeedDigit 的作用：
     *   删除速度输入框最后一位，便于触摸屏纠正输入。
     *
     * 返回值：
     *   无返回值；输入为空时回到 0，避免出现不可应用的空速度。
     */
    function backspaceStepperSpeedDigit() {
        stepperSpeedInputText = stepperSpeedInputText.substring(0, Math.max(0, stepperSpeedInputText.length - 1))
        if (stepperSpeedInputText.length === 0) {
            stepperSpeedInputText = "0"
        }
        stepperMotorResultText = "速度待应用：" + stepperSpeedInputText + " rpm"
    }

    /*
     * clearStepperSpeedInput 的作用：
     *   清空并重置常规速度输入为 0，便于快速设置停止速度。
     *
     * 返回值：
     *   无返回值；函数只更新输入文本和结果提示。
     */
    function clearStepperSpeedInput() {
        stepperSpeedInputText = "0"
        stepperMotorResultText = "速度待应用：0 rpm"
    }

    /*
     * applyStepperSpeedInput 的作用：
     *   校验速度输入框并写入当前页电机的 normalSpeedRpm 或 scanSpeedRpm。
     *
     * 主要流程：
     *   1. 只接受 0~5000 的十进制整数。
     *   2. 调用 C++ setStepperMotorValue() 写入内存配置。
     *   3. 提示用户还需要点击保存配置，才能写入 JSON 并在重启后恢复。
     *
     * 返回值：
     *   无返回值；成功后关闭速度数字键盘。
     */
    function applyStepperSpeedInput() {
        var trimmedText = stepperSpeedInputText.replace(/^\s+|\s+$/g, "")
        var parsedSpeed = parseInt(trimmedText, 10)

        if (!/^[0-9]+$/.test(trimmedText) || parsedSpeed < 0 || parsedSpeed > 5000) {
            stepperMotorResultText = "常规速度必须是 0~5000 rpm 的整数"
            storageState = stepperMotorResultText
            showStorageToast()
            return
        }

        if (!detectSettings.setStepperMotorValue(stepperMotorPageIndex, stepperSpeedEditKey, parsedSpeed)) {
            stepperMotorResultText = detectSettings.lastStatusText
            storageState = stepperMotorResultText
            showStorageToast()
            return
        }

        stepperSpeedEditorVisible = false
        stepperSpeedInputText = "" + parsedSpeed
        var speedLabel = stepperSpeedEditKey === "scanSpeedRpm" ? "上料速度" : "常规/对中速度"
        stepperMotorResultText = currentStepperMotorSetting().name + "：" + speedLabel
                + " " + parsedSpeed + " rpm，点击保存并下发写入JSON并通知F4"
        settingsLastActionText = "步进电机" + speedLabel + "已更新为 " + parsedSpeed + " rpm"
        storageState = settingsLastActionText
        showStorageToast()
    }

    /*
     * appendStepperStepDigit 的作用：
     *   向复用数字键盘追加一个数字，让触摸屏可以输入完整 Emm42 步数或 Z 轴超时秒数。
     *
     * 参数：
     *   digit 是被点击的数字字符。
     *
     * 返回值：
     *   无返回值；超过当前字段最大值时拒绝追加。
     */
    function appendStepperStepDigit(digit) {
        var baseText = stepperStepReplaceOnNextDigit ? "" : stepperStepInputText
        var nextText = (baseText + digit).replace(/^0+/, "")
        if (nextText.length === 0) {
            nextText = "0"
        }

        if (!/^[0-9]+$/.test(nextText) || Number(nextText) > stepperStepEditorMaxValue()) {
            stepperMotorResultText = stepperStepEditorTitleText()
                    + "范围是 " + stepperStepEditorMinValue()
                    + "~" + stepperStepEditorMaxValue()
                    + " " + stepperStepEditorUnitText()
            return
        }

        stepperStepReplaceOnNextDigit = false
        stepperStepInputText = nextText
        stepperMotorResultText = stepperStepEditorPendingText()
    }

    /*
     * backspaceStepperStepDigit 的作用：
     *   删除复用数字键盘最后一位，便于触摸屏纠正输入。
     *
     * 返回值：
     *   无返回值；输入为空时回到 0。
     */
    function backspaceStepperStepDigit() {
        stepperStepReplaceOnNextDigit = false
        stepperStepInputText = stepperStepInputText.substring(0, Math.max(0, stepperStepInputText.length - 1))
        if (stepperStepInputText.length === 0) {
            stepperStepInputText = "0"
        }
        stepperMotorResultText = stepperStepEditorPendingText()
    }

    /*
     * clearStepperStepInput 的作用：
     *   清空并重置固定位置步数或 Z 轴超时输入。
     *
     * 返回值：
     *   无返回值；函数只更新输入文本和提示。
     */
    function clearStepperStepInput() {
        stepperStepReplaceOnNextDigit = false
        if (stepperStepEditorIsTimeout()) {
            stepperStepInputText = ""
            stepperMotorResultText = "Z轴超时已清空，请输入1~60秒"
            return
        }

        stepperStepInputText = "0"
        stepperMotorResultText = stepperStepEditorPendingText()
    }

    /*
     * applyStepperStepInput 的作用：
     *   校验复用数字键盘，并写入当前页上下电机步数或 Z 轴超时参数。
     *
     * 主要流程：
     *   1. 步数接受 0~4294967295 十进制整数，超时接受 1~60 秒。
     *   2. 步数调用 C++ setStepperMotorStepValue()，避免 32 位 step 经过 int 截断。
     *   3. 超时调用 C++ setStepperMotorValue("zMotionTimeoutMs")，内部保存为毫秒。
     *   4. 成功后关闭数字键盘，提示用户保存并下发配置。
     *
     * 返回值：
     *   无返回值；成功后关闭位置步数数字键盘。
     */
    function applyStepperStepInput() {
        var trimmedText = stepperStepInputText.replace(/^\s+|\s+$/g, "")
        var parsedValue = Number(trimmedText)
        var editingTimeout = stepperStepEditorIsTimeout()
        var appliedTimeoutMs = editingTimeout ? Math.floor(parsedValue * 1000) : -1

        if (!/^[0-9]+$/.test(trimmedText)
                || parsedValue < stepperStepEditorMinValue()
                || parsedValue > stepperStepEditorMaxValue()) {
            stepperMotorResultText = stepperStepEditorTitleText()
                    + "必须是 " + stepperStepEditorMinValue()
                    + "~" + stepperStepEditorMaxValue()
                    + " " + stepperStepEditorUnitText() + " 的整数"
            storageState = stepperMotorResultText
            showStorageToast()
            return
        }

        if (editingTimeout) {
            if (!detectSettings.setStepperMotorValue(stepperMotorPageIndex, "zMotionTimeoutMs", appliedTimeoutMs)) {
                stepperMotorResultText = detectSettings.lastStatusText
                storageState = stepperMotorResultText
                showStorageToast()
                return
            }
        } else if (!detectSettings.setStepperMotorStepValue(stepperMotorPageIndex, stepperStepEditKey, trimmedText)) {
            stepperMotorResultText = detectSettings.lastStatusText
            storageState = stepperMotorResultText
            showStorageToast()
            return
        }

        stepperStepEditorVisible = false
        var motor = currentStepperMotorSetting()
        var visibleTimeoutMs = editingTimeout
                ? appliedTimeoutMs
                : Math.floor(Number(motor.zMotionTimeoutMs || autoVisionZMotionMaximumWaitMs))
        stepperMotorResultText = motor.name + "：下探 " + (motor.zDownFixedSteps || 0)
                + " step，回升 " + (motor.zUpFixedSteps || 0)
                + " step，Z轴超时 " + (visibleTimeoutMs / 1000.0).toFixed(1)
                + " 秒，本次自动检测Z轴下降/回升最多等待 " + visibleTimeoutMs
                + " ms；点击保存并下发写入JSON并通知F4"
        settingsLastActionText = editingTimeout
                ? "上下电机Z轴超时已更新为 " + (visibleTimeoutMs / 1000.0).toFixed(1)
                    + " 秒，本次自动检测立即使用；保存后重启仍生效"
                : "上下电机固定位置步数已更新"
        storageState = settingsLastActionText
        showStorageToast()
    }

    /*
     * syncStepperSettingsToF4 的作用：
     *   把当前 MP157 参数页里的三台电机参数通过 STEPPER_PARAM_SET 统一同步到 F4 运行内存。
     *
     * 主要流程：
     *   1. 检查是否已有步进参数下发在途，避免重复占用 `/dev/ttySTM2`。
     *   2. 记录触发原因，例如开机自动下发、保存配置或步进弹窗保存并下发。
     *   3. 调用 C++ sendF4StepperSettings() 发送二进制帧，等待 onF4StepperSettingsFinished 更新结果。
     *
     * 参数：
     *   reason 是本次同步来源，写入底部提示，方便现场判断是不是开机同步。
     *
     * 返回值：
     *   true 表示后台发送已启动；false 表示串口忙、参数非法或命令未启动。
     */
    function syncStepperSettingsToF4(reason) {
        var reasonText = reason && reason.length > 0 ? reason : "参数同步"

        if (stepperSettingsSending) {
            stepperMotorResultText = "步进参数正在下发 F4，请等待上一次回执"
            settingsLastActionText = stepperMotorResultText
            storageState = formatF4ToastText(stepperMotorResultText)
            showStorageToast()
            return false
        }

        stepperSettingsSending = true
        stepperMotorResultText = reasonText + "：正在下发三台电机参数，"
                + "上料速度=" + conveyorScanSpeedRpm() + "rpm，"
                + "对中速度=" + conveyorTrackSpeedRpm() + "rpm，"
                + root.stepperMotorRoleAddressSummary()
        settingsLastActionText = stepperMotorResultText
        storageState = formatF4ToastText(stepperMotorResultText)
        showStorageToast()

        if (!deviceHealth.sendF4StepperSettings(detectSettings.stepperMotorSettings)) {
            stepperSettingsSending = false
            stepperMotorResultText = reasonText + "：F4步进参数命令未启动"
            settingsLastActionText = stepperMotorResultText
            storageState = formatF4ToastText(stepperMotorResultText)
            showStorageToast()
            return false
        }

        return true
    }

    /*
     * changeStepperMotorValue 的作用：
     *   处理步进电机弹窗内的加减按钮，把修改写入 C++ 检测配置控制器。
     *
     * 参数：
     *   key 是 address、minStep、normalSpeedRpm、scanSpeedRpm 或 direction。
     *   delta 是要增加或减少的数值；direction 会被当作目标方向值使用。
     *
     * 返回值：
     *   无返回值；成功或失败都会更新 stepperMotorResultText 和底部提示条。
     */
    function changeStepperMotorValue(key, delta) {
        var motor = currentStepperMotorSetting()
        var nextValue = 0

        if (!motor || motor.name === undefined) {
            stepperMotorResultText = "步进电机参数异常：当前页没有电机配置"
            storageState = stepperMotorResultText
            showStorageToast()
            return
        }

        if (key === "address") {
            nextValue = motor.address + delta
        } else if (key === "minStep") {
            nextValue = motor.minStep + delta
        } else if (key === "normalSpeedRpm") {
            nextValue = motor.normalSpeedRpm + delta
        } else if (key === "scanSpeedRpm") {
            nextValue = (motor.scanSpeedRpm || 0) + delta
        } else if (key === "direction") {
            nextValue = delta >= 0 ? 1 : -1
        } else if (key === "zMotionTimeoutMs") {
            nextValue = Math.floor(Number(motor.zMotionTimeoutMs || 10000)) + delta
        } else {
            stepperMotorResultText = "步进电机参数异常：未知字段 " + key
            storageState = stepperMotorResultText
            showStorageToast()
            return
        }

        if (!detectSettings.setStepperMotorValue(stepperMotorPageIndex, key, nextValue)) {
            stepperMotorResultText = detectSettings.lastStatusText
            storageState = stepperMotorResultText
            showStorageToast()
            return
        }

        motor = currentStepperMotorSetting()
        if (key === "normalSpeedRpm" || key === "scanSpeedRpm") {
            stepperSpeedInputText = "" + Math.floor(Number(motor[key] || 0))
        } else if (key === "zMotionTimeoutMs") {
            stepperStepInputText = "" + Math.max(1, Math.min(60, Math.floor(Number(motor.zMotionTimeoutMs || 10000) / 1000)))
        }
        stepperMotorResultText = motor.name + "：地址 " + motor.addressHex
                + "，最小步长 " + motor.minStep + " step"
                + "，常规速度 " + motor.normalSpeedRpm + " rpm"
                + "，上料速度 " + (motor.scanSpeedRpm || 0) + " rpm"
                + "，方向 " + root.stepperMotorDirectionText(motor.direction)
                + "，Z轴超时 " + (Math.floor(Number(motor.zMotionTimeoutMs || 10000)) / 1000.0).toFixed(1) + " 秒"
        settingsLastActionText = "步进电机参数已更新，点击保存配置写入JSON"
        storageState = settingsLastActionText
        showStorageToast()
    }

    /*
     * settingsVisionDetailText 的作用：
     *   生成参数页“视觉检测策略”的完整说明，说明 MP157 当前真正参与云端上报的数据和判定边界。
     *
     * 主要流程：
     *   1. 按检测流水线说明原图保存、MobileNetV3-Small 分类、UNet 分割和综合判定。
     *   2. 按云端契约说明 record_no、part_code、source/annotated 图片和断网补传的关系。
     *   3. 明确当前参数页会写 JSON，并直接影响下一次检测的模型命令行参数。
     *
     * 返回值：
     *   返回多行中文说明，供 settingsDetailFlickable 滚动显示。
     */
    function settingsVisionDetailText() {
        var lines = [
            "[检测流水线]",
            "1. MP157 通过 KMS overlay 保存当前原始帧，原图作为 source 图片留档。",
            "2. MobileNetV3-Small INT8 先输出零件类别、GOOD/BAD 初判和 top1 置信度。",
            "3. UNet INT8 再输出缺陷 mask、overlay 和 raw 结果图，这些结果图作为 annotated 图片登记。",
            "4. 综合规则保持保守：分类判坏或 UNet 检出缺陷像素时，最终结果不能直接判为良品。",
            "5. 低于复核阈值的样本进入人工复核，不在本页伪装成自动分拣参数。",
            "6. 当前 ROI=" + settingsRoiSize + "px，会传给 defect-classify 和 defect-segment 的 --roi。",
            "7. 当前 UNet 像素阈值=" + settingsSegmentMinPixels + "px，会传给 defect-segment 的 --min-defect-pixels。",
            "8. 当前 overlay 透明度=" + settingsOverlayAlpha.toFixed(2) + "，会传给 defect-segment 的 --alpha。",
            "",
            "[云端记录契约]",
            "1. 每次检测必须生成稳定 record_no，断网补传继续复用同一个 record_no，避免云端重复记录。",
            "2. 零件类型只在波形垫圈、平垫圈、弹性垫圈之间归一；wave_washer_good/bad 只能归到同一个 wave_washer。",
            "3. POST /api/v1/records 顶层写 result、confidence_score、vision_context、decision_context 和 device_context。",
            "4. 图片不直接塞进主记录，先创建记录，再申请 COS 预签名地址，最后登记 source/annotated 文件元数据。",
            "5. source/annotated 上传成功后，历史详情才能完整展示原图、标注图、模型输出和云端记录号。",
            "",
            "[当前参数边界]",
            "1. 本页配置会保存到 " + settingsConfigPath + "，下一次启动自动读取。",
            "2. 模型阈值会传给 defect-classify --bad-threshold；复核阈值由 Qt 综合判定阶段使用。",
            "3. ROI、UNet像素阈值和overlay透明度会传给 defect-segment，对本地结果图和 NG 判定真实生效。",
            "4. 自动上传关闭时仍保存 source/annotated 和本地历史，但最终返回 upload_status=SKIP。",
            "5. 真正模型版本以板端部署的 ONNX Runtime、UNet 和 MobileNetV3-Small 模型文件为准。"
        ]
        return lines.join("\n")
    }

    /*
     * settingsF4DetailText 的作用：
     *   生成参数页“F4接入边界”的完整说明，避免把 MP157 页面误解成已经接管运动和分拣硬件。
     *
     * 主要流程：
     *   1. 说明 MP157 与 F4 的串口状态字段，包括设备节点、波特率、心跳、CRC 和帧序号。
     *   2. 说明 F4 负责采集或上报的光电、急停、限位、LDC1614、HX711 和 Emm42_V5.0 状态。
     *   3. 明确 Qt 只通过 F4 高层协议下发步进运行参数，不直接拼 Emm42 底层帧。
     *
     * 返回值：
     *   返回多行中文说明，供 settingsDetailFlickable 滚动显示。
     */
    function settingsF4DetailText() {
        var lines = [
            "[串口接入]",
            "1. MP157 当前通过 /dev/ttySTM2、115200 波特率访问传送带/称重 F407 USART1。",
            "2. 摄像头左右轴和上下轴已通过 F407 ACTUATOR/STEPPER 二进制协议接入，当前主链路设备节点为 /dev/ttySTM2。",
            "3. 每条检测记录建议携带 f4_uart.status、last_frame_seq、last_frame_crc_ok 和 last_frame_at。",
            "4. F4 心跳超时、CRC 错误或串口断开时，只能显示接入异常，不能在 Qt 里假定硬件已经恢复。",
            "",
            "[F4 上下文字段]",
            "1. f4_io 记录 photoelectric_triggered、limit_switch_in、limit_switch_out 和 emergency_stop。",
            "2. LDC1614 作为涡流/电感检测模块，建议上报 I2C 总线、地址、通道、原始码值、基线和判定。",
            "3. HX711 作为称重模块，建议上报 DOUT/SCK 引脚、增益、原始 ADC、净重、稳定状态和过载状态。",
            "4. 张大头 Emm42 闭环步进驱动由 F4 侧串口控制，记录目标速度、实际速度、位置误差、驱动故障和最近命令。",
            "",
            "[控制边界]",
            "1. MP157 负责视觉推理、图片保存、COS 上传、历史补传和云端记录创建。",
            "2. F4 负责运动控制、光电触发、急停限位、传感器采集和执行器联锁。",
            "3. 传送带开放 BELT_MANUAL_CONTROL/QUERY_STATUS，摄像头轴开放 ACTUATOR_POS_MOVE、ACTUATOR_STOP、ACTUATOR_VEL_MOVE 和 ACTUATOR_HOME。",
            "4. 步进电机参数弹窗保存三台 Emm42 的地址、最小步长、常规/对中速度、传送带上料速度和方向，保存或开机都会发送 STEPPER_PARAM_SET 给 F407。",
            "5. 机械臂等待超时只控制 MP157 等待 WEIGHT_RESULT、LDC_RESULT 和 CYCLE_DONE 的窗口，当前为 " + settingsF4ArmResultTimeoutText() + "。",
            "6. 当前 Qt 不直接拼 Emm42 帧，不绕过 F407 执行速度、位置、剔除动作、急停解除或联锁时序。",
            "7. F4 ACK 表示命令被协议层接收，现场仍要用 CAMINFO、QUERY_STATUS 或实际动作确认运行时参数和电机地址。"
        ]
        return lines.join("\n")
    }

    /*
     * openSettingsDetail 的作用：
     *   根据用户点击的参数卡片入口打开对应完整说明浮层。
     *
     * 参数：
     *   detailKey 是 vision 或 f4，用来决定标题和正文来源。
     *
     * 返回值：
     *   无返回值；函数会更新 settingsDetailTitle、settingsDetailText 和 settingsDetailVisible。
     */
    function openSettingsDetail(detailKey) {
        if (detailKey === "vision") {
            settingsDetailTitle = "视觉检测策略详情"
            settingsDetailText = settingsVisionDetailText()
        } else if (detailKey === "f4") {
            settingsDetailTitle = "F4接入边界详情"
            settingsDetailText = settingsF4DetailText()
        } else {
            return
        }
        settingsDetailVisible = true
        Qt.callLater(function() {
            settingsDetailFlickable.contentY = 0
        })
    }

    /*
     * openCalibrationPopup 的作用：
     *   打开称重标定弹窗，并把提示文案复位到当前 F4 串口状态。
     *
     * 主要流程：
     *   1. 设置 calibrationPopupVisible 显示遮罩弹窗。
     *   2. 清除上一次发送状态，保留用户最近使用的克重输入。
     *   3. 立即触发一次 F4 STATUS 刷新，方便用户确认 RS485 链路是否在线。
     *
     * 返回值：
     *   无返回值；弹窗状态由 QML 属性驱动。
     */
    function openCalibrationPopup() {
        calibrationPopupVisible = true
        calibrationSending = false
        calibrationResultText = "F4状态：" + deviceHealth.f4StatusText + "，当前标定入口只走二进制协议"
        deviceHealth.refreshF4StatusNow()
    }

    /*
     * selectCalibrationWeight 的作用：
     *   处理弹窗中的快捷克重按钮，把常用砝码值写入输入框。
     *
     * 参数：
     *   grams 是快捷按钮代表的克重，单位为 g。
     *
     * 返回值：
     *   无返回值；函数只更新 calibrationWeightText 和提示文案。
     */
    function selectCalibrationWeight(grams) {
        calibrationWeightText = "" + grams
        calibrationResultText = "已选择 " + grams + " g，确认砝码放稳后发送"
    }

    /*
     * appendCalibrationDigit 的作用：
     *   处理称重标定弹窗内置数字键盘的数字输入，让触摸屏不依赖系统软键盘也能输入任意克重。
     *
     * 主要流程：
     *   1. 发送中的 CAL 命令不再允许改数值，避免界面显示和已下发命令不一致。
     *   2. 把当前输入和新数字拼成候选值，并去掉前导零，保证显示始终是十进制整数文本。
     *   3. 候选值超过 5000g 时拒绝追加，并在弹窗内提示操作员。
     *
     * 参数：
     *   digit 是被点击的数字字符，取值为 "0" 到 "9"。
     *
     * 返回值：
     *   无返回值；函数只更新 calibrationWeightText 和 calibrationResultText。
     */
    function appendCalibrationDigit(digit) {
        if (calibrationSending) {
            calibrationResultText = "二进制标定命令发送中，暂不能修改克重"
            return
        }

        var nextText = (calibrationWeightText + digit).replace(/^0+/, "")
        if (nextText.length === 0) {
            nextText = "0"
        }

        var nextValue = parseInt(nextText, 10)
        if (nextValue > 5000) {
            calibrationResultText = "克重不能超过5000g"
            return
        }

        calibrationWeightText = nextText
        calibrationResultText = "已输入 " + calibrationWeightText + " g，确认砝码放稳后发送"
    }

    /*
     * backspaceCalibrationDigit 的作用：
     *   处理称重标定数字键盘的退格键，每次删除最右侧一位数字。
     *
     * 主要流程：
     *   1. 发送中的 CAL 命令不允许改输入框。
     *   2. 删除最后一位后允许输入框为空，后续发送时由 sendCalibrationCommand() 统一提示范围错误。
     *
     * 返回值：
     *   无返回值；函数只更新 calibrationWeightText 和提示文案。
     */
    function backspaceCalibrationDigit() {
        if (calibrationSending) {
            calibrationResultText = "二进制标定命令发送中，暂不能修改克重"
            return
        }

        calibrationWeightText = calibrationWeightText.substring(0, Math.max(0, calibrationWeightText.length - 1))
        calibrationResultText = calibrationWeightText.length > 0
            ? "已输入 " + calibrationWeightText + " g，确认砝码放稳后发送"
            : "请输入1~5000g整数克重"
    }

    /*
     * clearCalibrationWeight 的作用：
     *   清空称重标定输入框，方便操作员重新输入任意砝码克重。
     *
     * 返回值：
     *   无返回值；函数只清空 calibrationWeightText 并提示重新输入范围。
     */
    function clearCalibrationWeight() {
        if (calibrationSending) {
            calibrationResultText = "CAL发送中，暂不能修改克重"
            return
        }

        calibrationWeightText = ""
        calibrationResultText = "请输入1~5000g整数克重"
    }

    /*
     * sendCalibrationCommand 的作用：
     *   校验用户输入的标定克重，并通过 C++ DeviceHealthController 发起二进制称重标定命令。
     *
     * 主要流程：
     *   1. 去掉首尾空格后按十进制整数解析。
     *   2. 限制 1~5000g，匹配当前 HX711 服务默认 5kg 量程。
     *   3. 调用 deviceHealth.sendF4Command()，由 C++ 组二进制帧并等待 F4 ACK/NACK。
     *
     * 返回值：
     *   无返回值；结果通过 calibrationResultText 和底部提示条反馈。
     */
    function sendCalibrationCommand() {
        var trimmedText = calibrationWeightText.replace(/^\s+|\s+$/g, "")
        var parsedWeight = parseInt(trimmedText, 10)

        if (calibrationSending) {
            calibrationResultText = "上一条二进制标定命令仍在发送中"
            storageState = formatF4ToastText(calibrationResultText)
            showStorageToast()
            return
        }

        if (!/^[0-9]+$/.test(trimmedText) || parsedWeight < 1 || parsedWeight > 5000) {
            calibrationResultText = "克重必须是1~5000之间的整数"
            storageState = calibrationResultText
            showStorageToast()
            return
        }

        calibrationSending = true
        calibrationWeightText = "" + parsedWeight
        calibrationResultText = "正在发送二进制称重标定命令，克重 " + parsedWeight + " g ..."
        storageState = formatF4ToastText(calibrationResultText)
        showStorageToast()

        if (!deviceHealth.sendF4Command("CAL " + parsedWeight)) {
            calibrationSending = false
        }
    }

    /*
     * changeSettingValue 的作用：
     *   统一处理参数页的加减按钮，直接写入 C++ DetectSettingsController 的真实检测配置。
     *
     * 参数：
     *   key 是参数名，delta 是本次变化量。
     *
     * 返回值：
     *   无返回值；函数会更新对应 QML 参数和最近操作提示。
     */
    function changeSettingValue(key, delta) {
        if (key === "decision") {
            detectSettings.modelThreshold = Math.max(500, Math.min(990, settingsDecisionThreshold + delta)) / 1000.0
            settingsLastActionText = "真实检测配置：模型阈值 " + settingsThresholdText(settingsDecisionThreshold)
        } else if (key === "review") {
            detectSettings.reviewThreshold = Math.max(300, Math.min(settingsDecisionThreshold, settingsReviewThreshold + delta)) / 1000.0
            settingsLastActionText = "真实检测配置：复核阈值 " + settingsThresholdText(settingsReviewThreshold)
        } else if (key === "roi") {
            detectSettings.roiSize = Math.max(160, Math.min(640, settingsRoiSize + delta))
            settingsLastActionText = "真实检测配置：ROI " + settingsRoiSize + "px"
        } else if (key === "segment") {
            detectSettings.segmentMinPixels = Math.max(0, Math.min(50000, settingsSegmentMinPixels + delta))
            settingsLastActionText = "真实检测配置：UNet像素阈值 " + settingsSegmentMinPixels + "px"
        } else if (key === "alpha") {
            detectSettings.overlayAlpha = Math.max(0.0, Math.min(1.0, settingsOverlayAlpha + delta))
            settingsLastActionText = "真实检测配置：overlay透明度 " + settingsOverlayAlpha.toFixed(2)
        } else if (key === "f4-arm-timeout") {
            detectSettings.f4ArmResultTimeoutMs = Math.max(10000, Math.min(300000, settingsF4ArmResultTimeoutMs + delta))
            settingsLastActionText = "真实检测配置：机械臂等待超时 " + settingsF4ArmResultTimeoutText()
                    + "，只影响MP157等待F4主动结果帧"
        }

        storageState = settingsLastActionText
        showStorageToast()
    }

    /*
     * settingsApplyAction 的作用：
     *   统一处理参数页“应用、保存、恢复默认、导出摘要”等操作。
     *
     * 主要流程：
     *   1. 应用参数提示当前内存配置已进入下一次检测，不需要额外下发。
     *   2. 保存配置调用 C++ saveSettingsToDisk() 原子写入 JSON。
     *   3. 恢复默认调用 C++ resetToDefaults()，用户可再保存到 JSON。
     *
     * 参数：
     *   action 是 apply、save、reset 或 export。
     *
     * 返回值：
     *   无返回值；函数会更新 settingsLastActionText 和底部提示条。
     */
    function settingsApplyAction(action) {
        if (action === "apply") {
            settingsLastActionText = "真实检测配置：下一次检测将使用当前内存参数"
        } else if (action === "save") {
            var saveResult = detectSettings.saveSettingsToDisk()
            var saveLogResult = storageController.recordSettingsSummaryToSdCard(
                        "settings-save",
                        settingsLogText("保存配置", saveResult))
            settingsLastActionText = saveResult + "；" + saveLogResult
            refreshLogFileList()
            syncStepperSettingsToF4("保存配置同步F4")
        } else if (action === "reset") {
            settingsLastActionText = detectSettings.resetToDefaults()
        } else if (action === "export") {
            var exportText = "诊断摘要：" + settingsSummaryText()
            var exportLogResult = storageController.recordSettingsSummaryToSdCard(
                        "settings-export",
                        settingsLogText("导出摘要", exportText))
            settingsLastActionText = exportText + "；" + exportLogResult
            refreshLogFileList()
        } else if (action === "upload-toggle") {
            detectSettings.autoUploadEnabled = !settingsUploadEnabled
            settingsLastActionText = settingsUploadEnabled ? "真实检测配置：COS自动上传" : "真实检测配置：仅本地保存"
        } else if (action === "part-next") {
            detectSettings.partType = settingsNextPartType()
            settingsLastActionText = "真实检测配置：零件 " + settingsPartType
        }

        storageState = settingsLastActionText
        showStorageToast()
    }

    /*
     * alarmLevelColorFor 的作用：
     *   按传入告警等级返回统一状态色，供当前告警和历史列表共同使用。
     *
     * 参数：
     *   levelText 是“严重/预警/记录”等中文等级。
     *
     * 返回值：
     *   严重返回红色，预警返回黄色，记录/其它返回蓝色。
     */
    function alarmLevelColorFor(levelText) {
        if (levelText === "严重") {
            return accentRed
        }
        if (levelText === "预警") {
            return accentAmber
        }
        return "#5aa7ff"
    }

    /*
     * alarmLevelColor 的作用：
     *   根据告警等级和清除状态返回当前告警状态色，避免只靠文字判断告警状态。
     *
     * 返回值：
     *   已清除返回绿色；未清除时按 alarmCurrentLevel 返回严重、预警或记录色。
     */
    function alarmLevelColor() {
        if (alarmCleared) {
            return accentGreen
        }
        return alarmLevelColorFor(alarmCurrentLevel)
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
     * alarmSourceAdvice 的作用：
     *   根据告警来源返回现场优先排查步骤，写入告警日志和诊断快照。
     *
     * 参数：
     *   sourceKey 是自动告警来源标识，例如 camera-kms-no-frame、sdcard-not-writable。
     *
     * 返回值：
     *   返回字符串数组，每一项是一条可以直接执行或观察的排查建议。
     */
    function alarmSourceAdvice(sourceKey) {
        if (sourceKey === "camera-kms-no-frame") {
            return [
                "检查 /tmp/uvc-kms-overlay-control.sock 是否存在并能返回 STATUS。",
                "查看 /tmp/uvc_kms_overlay.log 是否有 V4L2、DRM 或相机断开错误。",
                "检查 /dev/video0、USB 摄像头供电和 run_qt_kms_overlay_display.sh restart-overlay。"
            ]
        }
        if (sourceKey === "sdcard-not-writable") {
            return [
                "执行 mount | grep ' /mnt/sdcard ' 确认 SD 卡真实挂载。",
                "执行 df -h /mnt/sdcard 和 echo/readback 测试确认空间与写权限。",
                "只在文件存在、大小非零并完成 fsync/sync 后再安全移除 SD 卡。"
            ]
        }
        if (sourceKey === "network-4g-offline") {
            return [
                "执行 4g-ppp test 查看 PPP、SIM 卡、天线和运营商网络状态。",
                "检查 ppp0 地址、默认路由和 4G USB 模块供电。",
                "确认弱网恢复后再重新检测或重新上传失败历史。"
            ]
        }
        if (sourceKey === "cloud-offline") {
            return [
                "确认 4G 或以太网链路在线后再访问云端 health。",
                "检查 defect-cos-upload 的后端地址、token 和云端服务状态。",
                "保留 /mnt/sdcard/images 本地图片，网络恢复后从历史详情重新发送。"
            ]
        }
        if (sourceKey === "f4-heartbeat-lost") {
            return [
                "检查 /dev/ttySTM2 是否存在，确认 F4 供电、复位和串口线序。",
                "用二进制 HEARTBEAT 帧确认 F4 返回 ACK；状态查询用 QUERY_STATUS，错误只看 NACK 或 FAULT_REPORT。",
                "不要把 QML 清故障当成真实联锁解除，最终以 F4 状态帧为准。"
            ]
        }
        if (sourceKey === "storage-save-failed") {
            return [
                "检查保存或上传返回的中文失败原因，优先确认 SD 卡挂载和剩余空间。",
                "查看 overlay SAVE_DETECT/SAVE_DUAL 是否返回 ERR。",
                "确认目标图片或日志文件非空，并检查 /tmp/qt_camera_display.log。"
            ]
        }
        if (sourceKey === "cloud-upload-failed") {
            return [
                "检查 defect-cos-upload stdout/stderr 中的 HTTP、COS 或 token 错误。",
                "确认 source 与 annotated 图片仍在 /mnt/sdcard/images。",
                "网络恢复后在历史详情点击重新发送。"
            ]
        }
        if (sourceKey === "model-detect-failed") {
            return [
                "检查 defect-classify、defect-segment、ONNX 模型和 labels 文件是否部署。",
                "确认当前帧 source JPG 存在且非空，模型程序可以读取。",
                "用 --detect-self-test 复现实验，并查看模型原始输出。"
            ]
        }
        return [
            "先保存诊断快照，再查看最近设备健康详情和 Qt 日志。",
            "确认问题恢复后再执行清故障或重新检测。"
        ]
    }

    /*
     * alarmFullAdviceText 的作用：
     *   生成“查看全部”浮层里的完整告警处理建议，避免小面板只显示第一句导致现场排查信息不完整。
     *
     * 主要流程：
     *   1. 先写当前告警和设备健康摘要，帮助操作员确认正在处理的对象。
     *   2. 按固定顺序列出相机、SD 卡、4G、云端、F4、保存/上传和模型链路建议。
     *   3. 每组建议复用 alarmSourceAdvice()，保证日志落盘文本和界面完整说明使用同一份排查内容。
     *
     * 返回值：
     *   返回多行文本，供 alarmAdviceDetailFlickable 中的 Text 组件滚动阅读。
     */
    function alarmFullAdviceText() {
        var groups = [
            {"title": "当前告警", "key": alarmCleared ? "runtime-ok" : "current"},
            {"title": "相机/KMS", "key": "camera-kms-no-frame"},
            {"title": "SD 卡", "key": "sdcard-not-writable"},
            {"title": "4G 网络", "key": "network-4g-offline"},
            {"title": "云端上传", "key": "cloud-upload-failed"},
            {"title": "云端 health", "key": "cloud-offline"},
            {"title": "F4 串口", "key": "f4-heartbeat-lost"},
            {"title": "保存链路", "key": "storage-save-failed"},
            {"title": "模型检测", "key": "model-detect-failed"}
        ]
        var lines = [
            "告警码：" + alarmCurrentCode,
            "告警名称：" + alarmCurrentTitle,
            "处理状态：" + alarmCurrentStatusText(),
            "设备健康：" + deviceHealthSummaryText(),
            ""
        ]

        for (var groupIndex = 0; groupIndex < groups.length; ++groupIndex) {
            var group = groups[groupIndex]
            var adviceKey = group.key
            if (adviceKey === "current") {
                if (alarmCurrentCode.indexOf("CAM") >= 0) {
                    adviceKey = "camera-kms-no-frame"
                } else if (alarmCurrentCode.indexOf("SD") >= 0) {
                    adviceKey = "sdcard-not-writable"
                } else if (alarmCurrentCode.indexOf("NET") >= 0) {
                    adviceKey = "network-4g-offline"
                } else if (alarmCurrentCode.indexOf("UPLOAD") >= 0) {
                    adviceKey = "cloud-upload-failed"
                } else if (alarmCurrentCode.indexOf("CLOUD") >= 0) {
                    adviceKey = "cloud-offline"
                } else if (alarmCurrentCode.indexOf("SAVE") >= 0) {
                    adviceKey = "storage-save-failed"
                } else if (alarmCurrentCode.indexOf("F4") >= 0) {
                    adviceKey = "f4-heartbeat-lost"
                } else if (alarmCurrentCode.indexOf("MODEL") >= 0) {
                    adviceKey = "model-detect-failed"
                } else {
                    adviceKey = "runtime-ok"
                }
            }

            lines.push("[" + group.title + "]")
            var advice = alarmSourceAdvice(adviceKey)
            for (var adviceIndex = 0; adviceIndex < advice.length; ++adviceIndex) {
                lines.push((adviceIndex + 1) + ". " + advice[adviceIndex])
            }
            if (groupIndex + 1 < groups.length) {
                lines.push("")
            }
        }

        return lines.join("\n")
    }

    /*
     * activeAlarmKeyCount 的作用：
     *   统计当前仍处于异常状态的问题数量，决定页面是否显示“无未处理告警”。
     *
     * 返回值：
     *   返回 activeAlarmKeys 中值为 true 的数量。
     */
    function activeAlarmKeyCount() {
        var count = 0
        for (var key in activeAlarmKeys) {
            if (activeAlarmKeys[key]) {
                count += 1
            }
        }
        return count
    }

    /*
     * markAlarmRecovered 的作用：
     *   在某个来源恢复正常时解除去重标记，让同一问题下次重新出现时能生成新的告警日志。
     *
     * 参数：
     *   sourceKey 是要解除的告警来源标识。
     *
     * 返回值：
     *   无返回值；函数会在全部问题恢复时更新当前告警状态。
     */
    function markAlarmRecovered(sourceKey) {
        if (!activeAlarmKeys[sourceKey]) {
            return
        }

        activeAlarmKeys[sourceKey] = false
        if (activeAlarmKeyCount() === 0) {
            alarmCleared = true
            alarmAcknowledged = false
            alarmCurrentLevel = "记录"
            alarmCurrentCode = "ALM-OK"
            alarmCurrentTitle = "运行状态未发现新告警"
            alarmCurrentTime = Qt.formatDateTime(new Date(), "hh:mm:ss")
        }
    }

    /*
     * appendAlarmHistory 的作用：
     *   把自动告警或人工维护动作写入告警历史，保持最新记录在最上方。
     *
     * 参数：
     *   timeText 是发生或操作时间。
     *   codeText 是告警码。
     *   levelText 是告警等级。
     *   titleText 是问题或操作摘要。
     *   statusText 是处理状态或 C++ 写文件结果。
     *
     * 返回值：
     *   无返回值；函数最多保留 12 条，避免小屏列表无限增长。
     */
    function appendAlarmHistory(timeText, codeText, levelText, titleText, statusText) {
        alarmHistoryModel.insert(0, {
            "time": timeText,
            "code": codeText,
            "level": levelText,
            "title": titleText,
            "status": statusText
        })

        while (alarmHistoryModel.count > 12) {
            alarmHistoryModel.remove(alarmHistoryModel.count - 1)
        }
    }

    /*
     * deviceHealthSummaryText 的作用：
     *   汇总相机、KMS、SD 卡、4G、云端和 F4 当前状态，写入日志便于 SSH 离线复盘。
     *
     * 返回值：
     *   返回单行中文摘要。
     */
    function deviceHealthSummaryText() {
        return "相机=" + cameraStatusText()
                + "；KMS=" + (usingKmsOverlay ? deviceHealth.cameraStatusText : "非KMS后端")
                + "；SD卡=" + deviceHealth.sdcardStatusText
                + "；4G=" + deviceHealth.networkStatusText
                + "；位置=" + deviceHealth.locationShortText
                + "；定位=" + deviceHealth.locationStatusText
                + "；云端=" + deviceHealth.cloudStatusText
                + "；F4=" + deviceHealth.f4StatusText
                + "；详情=" + deviceHealth.detailText
    }

    /*
     * buildAlarmLogText 的作用：
     *   组装自动告警日志文本，让每个真实问题都有独立可读的现场记录。
     *
     * 参数：
     *   sourceKey 是告警来源标识。
     *   codeText 是告警码。
     *   levelText 是告警等级。
     *   titleText 是问题标题。
     *   detailText 是触发时的原始状态或失败原因。
     *
     * 返回值：
     *   返回多行 UTF-8 文本，由 C++ recordAlarmIssueToSdCard() 写入时间戳日志文件。
     */
    function buildAlarmLogText(sourceKey, codeText, levelText, titleText, detailText) {
        var lines = [
            "STM32MP157 Qt Alarm Log",
            "occurrence_time=" + Qt.formatDateTime(new Date(), "yyyy-MM-dd hh:mm:ss"),
            "source=" + sourceKey,
            "alarm_code=" + codeText,
            "alarm_title=" + titleText,
            "alarm_level=" + levelText,
            "current_status=" + alarmCurrentStatusText(),
            "detail=" + detailText,
            "device_health=" + deviceHealthSummaryText(),
            "storage_state=" + storageState,
            "detect_state=" + detectState,
            "detect_status=" + detectStatus,
            "",
            "[troubleshooting]"
        ]
        var advice = alarmSourceAdvice(sourceKey)
        for (var i = 0; i < advice.length; ++i) {
            lines.push((i + 1) + ". " + advice[i])
        }
        return lines.join("\n") + "\n"
    }

    /*
     * raiseRuntimeAlarm 的作用：
     *   把一个新出现的真实问题升级为当前告警、写入历史并请求 C++ 生成独立告警日志。
     *
     * 主要流程：
     *   1. activeAlarmKeys 对同一来源做未恢复期间去重，避免健康检测周期性重复写日志。
     *   2. 更新当前告警字段，让告警维护页立即显示真实问题。
     *   3. 调用 recordAlarmIssueToSdCard()，追加到当天 qt_alarm_YYYYMMDD.log 文件。
     *
     * 参数：
     *   sourceKey 是告警来源标识。
     *   codeText 是告警码。
     *   levelText 是告警等级。
     *   titleText 是问题标题。
     *   detailText 是触发时的原始状态或失败原因。
     *
     * 返回值：
     *   无返回值；函数会更新告警状态、历史和底部提示。
     */
    function raiseRuntimeAlarm(sourceKey, codeText, levelText, titleText, detailText) {
        if (activeAlarmKeys[sourceKey]) {
            return
        }

        activeAlarmKeys[sourceKey] = true
        alarmCurrentCode = codeText
        alarmCurrentTitle = titleText
        alarmCurrentLevel = levelText
        alarmCurrentTime = Qt.formatDateTime(new Date(), "hh:mm:ss")
        alarmAcknowledged = false
        alarmCleared = false

        var logText = buildAlarmLogText(sourceKey, codeText, levelText, titleText, detailText)
        alarmLastLogResult = storageController.recordAlarmIssueToSdCard(sourceKey, logText)
        appendAlarmHistory(alarmCurrentTime, codeText, levelText, titleText, alarmLastLogResult)
        storageState = alarmLastLogResult
        showStorageToast()
    }

    /*
     * runtimeStatusAbnormal 的作用：
     *   判断健康状态文本是否代表异常，而不是“在线/已连接/可写”等正常状态。
     *
     * 参数：
     *   statusText 是 deviceHealth 或检测链路返回的中文状态。
     *   goodValues 是认为正常的状态文本数组。
     *
     * 返回值：
     *   true 表示状态非空且不属于正常值；false 表示正常或仍为空。
     */
    function runtimeStatusAbnormal(statusText, goodValues) {
        if (statusText.length <= 0 || statusText === "检测中") {
            return false
        }
        for (var i = 0; i < goodValues.length; ++i) {
            if (statusText === goodValues[i]) {
                return false
            }
        }
        return true
    }

    /*
     * evaluateRuntimeAlarms 的作用：
     *   从已有运行状态中识别相机/KMS、SD 卡、4G、云端和 F4 的真实问题。
     *
     * 主要流程：
     *   1. 每次 deviceHealth 状态变化或手动刷新后调用本函数。
     *   2. 正常状态调用 markAlarmRecovered()，异常状态调用 raiseRuntimeAlarm()。
     *   3. 不执行 shell、不访问硬件，只消费 C++ 已异步探测出的状态，保证 QML 不阻塞。
     *
     * 返回值：
     *   无返回值；函数会根据状态更新当前告警和日志。
     */
    function evaluateRuntimeAlarms() {
        if (runtimeStatusAbnormal(cameraStatusText(), ["在线", "GL在线"])) {
            raiseRuntimeAlarm("camera-kms-no-frame",
                              "ALM-CAM-001",
                              "严重",
                              "相机/KMS 无有效帧",
                              cameraStatusText())
        } else {
            markAlarmRecovered("camera-kms-no-frame")
        }

        if (runtimeStatusAbnormal(deviceHealth.sdcardStatusText, ["可写", "已挂载"])) {
            raiseRuntimeAlarm("sdcard-not-writable",
                              "ALM-SD-001",
                              "严重",
                              "SD 卡未挂载或不可写",
                              deviceHealth.sdcardStatusText)
        } else {
            markAlarmRecovered("sdcard-not-writable")
        }

        if (runtimeStatusAbnormal(deviceHealth.networkStatusText, ["在线"])) {
            raiseRuntimeAlarm("network-4g-offline",
                              "ALM-NET-001",
                              "预警",
                              "4G/网络链路异常",
                              deviceHealth.networkStatusText)
        } else {
            markAlarmRecovered("network-4g-offline")
        }

        if (runtimeStatusAbnormal(deviceHealth.cloudStatusText, ["已连接"])) {
            raiseRuntimeAlarm("cloud-offline",
                              "ALM-CLOUD-001",
                              "预警",
                              "云端 health 异常",
                              deviceHealth.cloudStatusText)
        } else {
            markAlarmRecovered("cloud-offline")
        }

        if (runtimeStatusAbnormal(deviceHealth.f4StatusText, ["接入"])) {
            raiseRuntimeAlarm("f4-heartbeat-lost",
                              "ALM-F4-001",
                              "严重",
                              "F4 心跳/串口握手异常",
                              deviceHealth.f4StatusText)
        } else {
            markAlarmRecovered("f4-heartbeat-lost")
        }
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
     *   返回多行 UTF-8 文本，由 C++ 控制器落盘到当天 /mnt/sdcard/logs/qt_alarm_snapshot_YYYYMMDD.txt。
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
            "device_health=" + deviceHealthSummaryText(),
            "last_alarm_log_result=" + alarmLastLogResult,
            "video_backend=" + videoBackend,
            "kms_overlay=" + (usingKmsOverlay ? "true" : "false"),
            "manual_mode=" + (manualMode ? "true" : "false"),
            "manual_emergency_stop=" + (manualEmergencyStop ? "true" : "false"),
            "manual_camera_axis_ready=false",
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
     *   3. 保存诊断调用 C++ 控制器追加当天 /mnt/sdcard/logs/qt_alarm_snapshot_YYYYMMDD.txt。
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
            manualEmergencyStop = false
            evaluateRuntimeAlarms()
            if (activeAlarmKeyCount() > 0) {
                alarmAcknowledged = true
                alarmCleared = false
                resultText = "清故障：仍有真实问题未恢复"
            } else {
                alarmAcknowledged = true
                alarmCleared = true
                resultText = "清故障：等待 F4 复核"
            }
        } else if (action === "refresh") {
            deviceHealth.refreshAllStatus()
            evaluateRuntimeAlarms()
            resultText = "已刷新设备健康状态"
        } else if (action === "snapshot") {
            resultText = storageController.saveAlarmSnapshotToSdCard(alarmSnapshotText())
        } else {
            resultText = "未知告警维护动作"
        }

        appendAlarmHistory(Qt.formatDateTime(new Date(), "hh:mm:ss"),
                           alarmCurrentCode,
                           alarmCleared ? "记录" : alarmCurrentLevel,
                           resultText,
                           alarmCurrentStatusText())

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
     *   pageName 是目标页面名称，目前支持 home、history、stats、manual、settings、alarm 和 logs。
     *
     * 返回值：
     *   无返回值；函数负责更新页面状态和 overlay 可见性。
     */
    function switchPage(pageName) {
        if (pageName !== "home" && pageName !== "history" && pageName !== "stats"
                && pageName !== "manual" && pageName !== "settings"
                && pageName !== "alarm" && pageName !== "logs") {
            return
        }

        activePage = pageName
        alarmAdviceDetailVisible = false
        settingsDetailVisible = false
        stepperMotorPopupVisible = false
        manualMotorPopup = false
        calibrationPopupVisible = false
        stepperStepEditorVisible = false
        logDetailVisible = false

        if (pageName === "history") {
            focusLatestHistoryListRecord()
        } else if (pageName === "logs") {
            refreshLogFileList()
            historyDetailVisible = false
            historyAnalysisDetailVisible = false
            selectedHistoryRecord = uploadHistory.entryAt(selectedHistoryIndex)
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
     * refreshLogFileList 的作用：
     *   刷新日志查看页面中的 SD 卡日志文件列表。
     *
     * 主要流程：
     *   1. 调用 C++ LogFileModel::refresh() 重新扫描 /mnt/sdcard/logs。
     *   2. 如果刷新后没有日志，清空选中状态和弹窗内容。
     *   3. 如果已有选中索引但日志数量变少，把索引压回有效范围，避免 QML 读越界。
     *
     * 返回值：
     *   无返回值；列表内容由 logFileModel 模型自动通知 QML 刷新。
     */
    function refreshLogFileList() {
        logFileModel.refresh()

        if (logFileModel.count <= 0) {
            selectedLogIndex = -1
            selectedLogEntry = logFileModel.entryAt(selectedLogIndex)
            selectedLogContent = ""
            logDetailVisible = false
            return
        }

        if (selectedLogIndex >= logFileModel.count) {
            selectedLogIndex = 0
            selectedLogEntry = logFileModel.entryAt(selectedLogIndex)
        }
    }

    /*
     * openLogDetail 的作用：
     *   打开某个日志文件的全文弹窗。
     *
     * 主要流程：
     *   1. 校验 row，避免列表刷新后点击到无效索引。
     *   2. 保存当前日志摘要，读取完整日志内容。
     *   3. 显示弹窗后把 logDetailFlickable.contentY 归零，避免上一条日志的滚动位置泄漏到下一条。
     *
     * 参数：
     *   row 是 logFileModel 中的日志索引。
     *
     * 返回值：
     *   无返回值；函数会更新 selectedLogEntry、selectedLogContent 和 logDetailVisible。
     */
    function openLogDetail(row) {
        if (row < 0 || row >= logFileModel.count) {
            selectedLogIndex = -1
            selectedLogEntry = logFileModel.entryAt(selectedLogIndex)
            selectedLogContent = "日志读取失败：记录不存在"
            logDetailVisible = true
            return
        }

        selectedLogIndex = row
        selectedLogEntry = logFileModel.entryAt(row)
        selectedLogContent = logFileModel.readLogContent(row)
        logDetailVisible = true
        Qt.callLater(function() {
            logDetailFlickable.contentY = 0
        })
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
     * uploadStatusTokenValue 的作用：
     *   从上传状态文本中读取 upload_status、record_id、record_no 等短字段。
     *
     * 参数：
     *   text 是 C++ 返回的检测 RESULT 或历史 JSON 中保存的 uploadStatus。
     *   key 是需要读取的字段名。
     *
     * 返回值：
     *   找到字段时返回字段值；找不到时返回空字符串。
     */
    function uploadStatusTokenValue(text, key) {
        return tokenValue(text, key)
    }

    /*
     * isUploadStatusSuccess 的作用：
     *   统一判断云端上传是否成功，避免历史页、统计页和提示条各自用不同字符串规则。
     *
     * 参数：
     *   rawStatus 是历史记录里的 uploadStatus 文本。
     *   record 是可选历史记录对象，用于读取 recordId/recordNo 兼容旧记录。
     *
     * 返回值：
     *   明确上传成功返回 true；失败、跳过或未知状态返回 false。
     */
    function isUploadStatusSuccess(rawStatus, record) {
        var statusText = rawStatus ? String(rawStatus) : ""
        var statusToken = uploadStatusTokenValue(statusText, "upload_status").toUpperCase()

        if (statusText.indexOf("upload_status=OK") >= 0) {
            return true
        }
        if (statusToken === "OK") {
            return true
        }
        if (statusToken === "FAIL" || statusToken === "SKIP") {
            return false
        }
        if (statusText.indexOf("上传失败") >= 0) {
            return false
        }
        if (statusText.indexOf("上传成功") >= 0) {
            return true
        }
        if (record && record.recordId && record.recordId.length > 0) {
            return true
        }
        if (record && record.recordNo && record.recordNo.length > 0) {
            return true
        }

        return false
    }

    /*
     * isUploadStatusFailure 的作用：
     *   统一判断上传是否明确失败，避免“云端已成功但回查诊断包含失败字样”时仍显示上传失败。
     *
     * 参数：
     *   rawStatus 是上传状态文本。
     *
     * 返回值：
     *   明确失败返回 true；成功、跳过或未知返回 false。
     */
    function isUploadStatusFailure(rawStatus) {
        var statusText = rawStatus ? String(rawStatus) : ""
        var statusToken = uploadStatusTokenValue(statusText, "upload_status").toUpperCase()

        if (statusToken === "FAIL") {
            return true
        }
        if (statusToken === "OK" || statusToken === "SKIP") {
            return false
        }

        return statusText.indexOf("上传失败") >= 0
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

        if (isUploadStatusFailure(rawStatus)) {
            return "上传失败，请查看日志"
        }

        if (isUploadStatusSuccess(rawStatus, null) || recordId.length > 0 || recordNo.length > 0) {
            var summary = "上传成功"
            if (recordId.length > 0) {
                summary += "  ID " + recordId
            }
            if (recordNo.length > 0) {
                summary += "  " + recordNo
            }
            return summary
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

        return isUploadStatusFailure(record.uploadStatus)
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

        return isUploadStatusSuccess(record.uploadStatus, record)
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

    /* alarmHistoryModel 保存告警维护页最近处理记录，自动告警和人工维护动作都会插入最新一行。 */
    ListModel {
        id: alarmHistoryModel

        ListElement {
            time: "--:--:--"
            code: "ALM-INIT"
            level: "记录"
            title: "等待设备健康检测"
            status: "未发现新告警"
        }
    }

    /* clockTimer 每秒更新时间；视觉偏差由 LOCATE 回调写入，不能再用演示值滚动。 */
    Timer {
        id: clockTimer
        interval: 1000
        repeat: true
        running: true

        onTriggered: {
            currentTimeText = Qt.formatDateTime(new Date(), "hh:mm:ss")
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

    /* autoVisionTimer 周期请求 overlay LOCATE；真正串口发送由定位结果回调决定。 */
    Timer {
        id: autoVisionTimer
        interval: 100
        repeat: true
        running: false

        onTriggered: {
            if (!root.autoVisionRunning
                    || root.autoWorkflowPaused
                    || root.autoVisionCenteredSent
                    || root.autoVisionLocateBusy
                    || root.autoVisionCommandBusy) {
                return
            }

            root.autoVisionLocateBusy = true
            if (!deviceHealth.requestAutoVisionLocate()) {
                root.autoVisionLocateBusy = false
            }
        }
    }

    /* autoVisionDetectDelayTimer 等对焦或静止延时结束后，再复用现有当前帧检测链路。 */
    Timer {
        id: autoVisionDetectDelayTimer
        interval: root.autoVisionDefaultDetectDelayMs
        repeat: false
        running: false

        onTriggered: {
            root.autoVisionDetectFromZFlow = true
            root.workflowState = "模型检测"
            root.storageState = "零件已在检测高度，开始模型检测"
            root.showStorageToast()
            root.handleDetectAction()
        }
    }

    /* autoVisionActuatorSettleTimer 统一承载 Z 轴物理等待、短步微调稳定和 3 秒对焦稳定等异步阶段。 */
    Timer {
        id: autoVisionActuatorSettleTimer
        interval: 450
        repeat: false
        running: false

        onTriggered: {
            if (root.autoVisionActuatorPhase === "z-motion-down-wait") {
                root.autoVisionNeedsZUp = true
                root.autoVisionPendingZMoveSteps = 0
                root.autoVisionPendingZMoveSpeedRpm = 0
                root.autoVisionPendingZMoveDirection = 0
                root.workflowState = "Z轴下降本地超时"
                root.storageState = "Z轴下降达到参数页超时，未收到F4 ACTUATOR_MOVE_DONE，按MP157本地等待策略继续ROI复查"
                root.autoVisionLastText = root.storageState
                root.showStorageToast()
                root.autoVisionRequestFineTuneLocate()
            } else if (root.autoVisionActuatorPhase === "z-motion-up-wait") {
                root.autoVisionNeedsZUp = true
                root.autoVisionZFocusSettled = false
                root.autoVisionDetectFromZFlow = false
                root.autoVisionPendingZMoveSteps = 0
                root.autoVisionPendingZMoveSpeedRpm = 0
                root.autoVisionPendingZMoveDirection = 0
                root.autoVisionActuatorPhase = ""
                root.workflowState = "Z轴回升超时"
                root.storageState = "未收到F4 ACTUATOR_MOVE_DONE，禁止启动机械臂抓取，请手动确认Z轴已离开零件"
                root.autoVisionLastText = root.storageState
                root.showStorageToast()
            } else if (root.autoVisionActuatorPhase === "z-down-skip"
                    || root.autoVisionActuatorPhase.indexOf("fine-tune") === 0) {
                root.autoVisionRequestFineTuneLocate()
            } else if (root.autoVisionActuatorPhase === "focus-settle") {
                root.autoVisionZFocusSettled = true
                root.autoVisionActuatorPhase = ""
                root.autoVisionStartDetectDelay()
            }
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

    /* stepperStartupSyncTimer 在开机后自动把参数页三台电机配置同步到 F4，避免 F4 继续使用旧运行参数。 */
    Timer {
        id: stepperStartupSyncTimer
        interval: 1600
        repeat: false
        running: false

        onTriggered: {
            root.syncStepperSettingsToF4("开机自动下发")
        }
    }

    /*
     * Component.onCompleted 在 QML 根对象加载完成后执行一次。
     * 这里二次隐藏 overlay 视频层，用于覆盖手动重启 Qt 但 overlay 仍在运行的场景。
     */
    Component.onCompleted: {
        root.setBootOverlayVisible(false)
        root.evaluateRuntimeAlarms()
        stepperStartupSyncTimer.restart()
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
            root.evaluateRuntimeAlarms()

            if (!root.usingKmsOverlay || deviceHealth.cameraStatusText !== "在线") {
                return
            }

            if (root.bootOverlayRestoreFinished && !root.splashOverlayVisible && root.activePage === "home") {
                root.setBootOverlayVisible(true)
            }
        }

        /*
         * onNetworkStatusChanged 的作用：
         *   4G 状态变化后复用自动告警评估，首次离线会写入独立告警日志，恢复后解除去重标记。
         */
        onNetworkStatusChanged: {
            root.evaluateRuntimeAlarms()
        }

        /*
         * onF4StatusChanged 的作用：
         *   F4 串口握手状态变化后检查心跳类告警，避免只在顶部状态栏显示待接入。
         */
        onF4StatusChanged: {
            root.evaluateRuntimeAlarms()
        }

        /*
         * onLocationStatusChanged 的作用：
         *   开机单次高德 IP 省份定位完成后刷新设备健康摘要。
         *   定位失败只影响顶部位置显示和日志摘要，不作为产线停机告警。
         */
        onLocationStatusChanged: {
            root.evaluateRuntimeAlarms()
        }

        /*
         * onF4CommandFinished 的作用：
         *   接收 C++ 后台串口命令结果，更新称重标定弹窗和底部提示条。
         *
         * 参数：
         *   ok 表示 F4 回复是否被 C++ 判定为成功。
         *   detail 是 F4 返回文本或 C++ 侧失败原因。
         */
        onF4CommandFinished: {
            root.calibrationSending = false
            if (ok) {
                root.calibrationResultText = "标定命令成功：" + detail
                root.settingsLastActionText = "已下发二进制称重标定命令 " + root.calibrationWeightText + " g"
            } else {
                root.calibrationResultText = "标定命令失败：" + detail
                root.settingsLastActionText = root.calibrationResultText
            }
            root.storageState = root.formatF4ToastText(root.calibrationResultText)
            root.showStorageToast()
            root.evaluateRuntimeAlarms()
        }

        /*
         * onF4ManualCommandFinished 的作用：
         *   接收 C++ 后台传送带手动命令结果，把 F407 回包写回手动页状态和命令日志。
         *
         * 参数：
         *   ok 表示 F4 回复是否被 C++ 判定为成功。
         *   command 是刚下发的 BELT 命令。
         *   detail 是 F4 返回文本或 C++ 侧失败原因。
         */
        onF4ManualCommandFinished: {
            root.manualPendingF4Command = ""
            root.manualBeltCommandText = command

            if (ok) {
                if (command === "BELT_MANUAL_SCAN") {
                    root.manualBeltState = "巡航中"
                } else if (command === "BELT_MANUAL_STOP") {
                    root.manualBeltState = "停止"
                } else if (command === "QUERY_STATUS") {
                    root.manualBeltState = "状态已返回"
                }
                root.manualLastAckText = "F4回执：" + detail
            } else {
                root.manualBeltState = "命令失败"
                root.manualLastAckText = "F4命令失败：" + detail
            }

            root.storageState = root.formatF4ToastText(root.manualLastAckText)
            root.appendManualCommandLog(command, "传送带", root.manualLastAckText)
            root.showStorageToast()
            root.evaluateRuntimeAlarms()
        }

        /*
         * onF4StepperSettingsFinished 的作用：
         *   接收 C++ 后台步进电机参数下发结果，更新参数弹窗和底部提示条。
         *
         * 参数：
         *   ok 表示 F4 是否返回匹配 STEPPER_PARAM_SET 的 ACK。
         *   detail 是 ACK/NACK 解析文本或串口失败原因。
         */
        onF4StepperSettingsFinished: {
            root.stepperSettingsSending = false
            if (ok) {
                root.stepperMotorResultText = "F4已接收步进参数："
                        + root.stepperMotorRoleAddressSummary()
                        + "，上料=" + root.conveyorScanSpeedRpm() + "rpm"
                        + "，对中=" + root.conveyorTrackSpeedRpm() + "rpm；"
                        + detail
                root.settingsLastActionText = "步进电机参数已保存并下发 F4："
                        + root.stepperMotorRoleAddressSummary()
                        + "，上料=" + root.conveyorScanSpeedRpm() + "rpm"
                        + "，对中=" + root.conveyorTrackSpeedRpm() + "rpm"
            } else {
                root.stepperMotorResultText = "F4步进参数下发失败：" + detail
                root.settingsLastActionText = root.stepperMotorResultText
            }
            root.storageState = root.formatF4ToastText(root.settingsLastActionText)
            root.showStorageToast()
            root.evaluateRuntimeAlarms()
        }

        /*
         * onF4AutoControlFinished 的作用：
         *   接收 C++ 二进制自动流程命令结果，首页四按钮只有收到 ACK 后才真正改变本地流程状态。
         *
         * 参数：
         *   ok 表示 F4 是否返回匹配本次命令、SEQ 和 cycle_id 的 ACK。
         *   action 是刚下发的首页动作。
         *   cycleId 是本次自动检测流程号。
         *   detail 是 ACK/NACK 解析文本或串口失败原因。
         */
        onF4AutoControlFinished: {
            root.autoControlBusy = false
            root.autoPendingAction = ""
            root.autoPendingStateText = ""
            root.autoCycleId = cycleId

            if (ok) {
                if (action === "start") {
                    root.autoWorkflowRunning = true
                    root.autoWorkflowPaused = false
                    root.workflowState = "定位预览"
                    root.startAutoVisionLoop()
                } else if (action === "pause") {
                    root.autoWorkflowRunning = true
                    root.autoWorkflowPaused = true
                    root.workflowState = "暂停"
                    root.stopAutoVisionLoop("自动视觉已暂停")
                } else if (action === "resume") {
                    root.autoWorkflowRunning = true
                    root.autoWorkflowPaused = false
                    root.workflowState = "继续检测"
                    root.startAutoVisionLoop()
                } else if (action === "stop") {
                    root.autoWorkflowRunning = false
                    root.autoWorkflowPaused = false
                    root.workflowState = "停止"
                    root.stopAutoVisionLoop("自动视觉已停止")
                }

                if (!root.usingKmsOverlay && !root.usingGstVideo) {
                    if (action === "pause" || action === "stop") {
                        cameraView.running = false
                    } else if (action === "start" || action === "resume") {
                        cameraView.running = true
                    }
                }

                root.autoLastAckText = "自动流程ACK：cycle=" + cycleId + "，" + detail
            } else {
                root.autoLastAckText = "自动流程失败：" + detail
                if (action === "start" || action === "resume") {
                    root.stopAutoVisionLoop("自动视觉未启动：" + detail)
                }
                if (root.autoWorkflowPaused) {
                    root.workflowState = "暂停"
                } else if (root.autoWorkflowRunning) {
                    root.workflowState = "自动检测运行中"
                } else {
                    root.workflowState = "定位预览"
                }
            }

            root.storageState = root.formatF4ToastText(root.autoLastAckText)
            root.showStorageToast()
            root.evaluateRuntimeAlarms()
        }

        /*
         * onAutoVisionLocateFinished 的作用：
         *   接收 C++ overlay LOCATE 定位结果，并推进 MP157->F4 视觉坐标下发。
         *
         * 参数：
         *   ok 表示 overlay 是否成功返回 LOCATE 行。
         *   result 是 C++ 解析后的坐标 map。
         *   detail 是 overlay 原始回复或错误原因。
         */
        onAutoVisionLocateFinished: {
            root.handleAutoVisionLocateFinished(ok, result, detail)
        }

        /*
         * onF4VisionCommandFinished 的作用：
         *   接收 VISION_POS、VISION_LOST 和 BELT_STOP_CENTERED 的 ACK/NACK，释放视觉闭环串口忙标志。
         *
         * 参数：
         *   ok 表示 F4 是否 ACK 本次视觉闭环命令。
         *   action 是视觉命令名称。
         *   cycleId 是当前自动流程号。
         *   detail 是 ACK/NACK 解析结果或串口失败原因。
         */
        onF4VisionCommandFinished: {
            root.autoVisionCommandBusy = false
            root.autoCycleId = cycleId

            if (ok) {
                root.autoVisionLastText = "视觉闭环ACK：" + action + " " + detail
            } else {
                root.autoVisionLastText = "视觉闭环失败：" + action + " " + detail
                if (action === "VISION_POS") {
                    root.autoVisionStableFrames = 0
                }
            }

            if (action === "BELT_STOP_CENTERED") {
                if (ok) {
                    root.autoVisionRunning = false
                    autoVisionTimer.stop()
                    root.workflowState = "准备下探"
                    root.storageState = "F4已确认居中停机，准备让上下电机下降到检测高度"
                    root.autoVisionRequestZDown()
                } else {
                    root.autoVisionCenteredSent = false
                    root.autoVisionStableFrames = 0
                    root.storageState = root.autoVisionLastText
                    if (root.autoWorkflowRunning && !root.autoWorkflowPaused) {
                        root.autoVisionRunning = true
                        autoVisionTimer.restart()
                    }
                }
                root.showStorageToast()
            }

            root.evaluateRuntimeAlarms()
        }

        /*
         * onF4ActuatorCommandFinished 的作用：
         *   接收 ACTUATOR_POS_MOVE 的完成事件结果或其它执行器命令 ACK/NACK，并按阶段推进。
         *
         * 参数：
         *   ok 对 ACTUATOR_POS_MOVE 表示已经收到 F4 ACTUATOR_MOVE_DONE；对其它命令表示 ACK 成功。
         *   action 是执行器命令名称。
         *   cycleId 是当前自动流程号，手动命令通常为 0。
         *   detail 是 ACK、EVENT_REPORT、NACK 或串口失败原因。
         */
        onF4ActuatorCommandFinished: {
            root.autoVisionCommandBusy = false
            root.autoCycleId = cycleId

            if (root.autoVisionActuatorPhase !== "") {
                if (ok) {
                    root.autoVisionLastText = "执行器完成：" + root.autoVisionActuatorPhase + " " + detail
                    if (root.autoVisionActuatorPhase === "z-down") {
                        root.autoVisionNeedsZUp = true
                        root.autoVisionActuatorPhase = "z-motion-down-wait"
                        root.autoVisionHandleActuatorMoveDone(detail)
                    } else if (root.autoVisionActuatorPhase.indexOf("fine-tune") === 0) {
                        if (root.autoVisionActuatorPhase === "fine-tune-lateral") {
                            var lateralOffset = root.commitAutoVisionLateralFineTuneOffset()
                            root.autoVisionLastText = root.autoVisionLastText
                                    + "；左右轴累计回中偏移=" + lateralOffset + " step"
                            root.storageState = root.autoVisionLastText
                            root.showStorageToast()
                        }
                        autoVisionActuatorSettleTimer.interval = root.autoVisionShortSettleMs
                        autoVisionActuatorSettleTimer.restart()
                    } else if (root.autoVisionActuatorPhase === "z-up") {
                        root.autoVisionActuatorPhase = "z-motion-up-wait"
                        root.autoVisionHandleActuatorMoveDone(detail)
                    } else if (root.autoVisionActuatorPhase === "lateral-return") {
                        root.autoVisionLateralReturnOffsetSteps = 0
                        root.clearAutoVisionPendingLateralFineTune()
                        root.autoVisionActuatorPhase = ""
                        root.workflowState = "左右轴已回中"
                        root.storageState = "左右轴已按本轮累计偏移回到皮带基准：" + detail
                        root.autoVisionLastText = root.storageState
                        root.showStorageToast()
                        root.autoVisionStartF4ArmInspectionAfterZUp()
                    }
                } else {
                    root.autoVisionLastText = "执行器失败：" + root.autoVisionActuatorPhase + " " + detail
                    root.storageState = root.autoVisionLastText
                    root.showStorageToast()
                    if (root.autoVisionActuatorPhase === "fine-tune-lateral") {
                        root.clearAutoVisionPendingLateralFineTune()
                    }
                    if (root.autoVisionActuatorPhase === "z-up") {
                        root.autoVisionNeedsZUp = true
                    } else if (root.autoVisionActuatorPhase === "lateral-return") {
                        root.workflowState = "左右轴回中失败"
                        root.storageState = "左右轴回中失败，累计偏移仍为 "
                                + root.autoVisionLateralReturnOffsetSteps
                                + " step，请手动让绿色ROI重新对齐黑色传送带两边后再继续"
                        root.autoVisionLastText = root.storageState
                        root.showStorageToast()
                    }
                    root.autoVisionActuatorPhase = ""
                }

                root.evaluateRuntimeAlarms()
                return
            }

            if (root.stepperHomePendingCommand !== "") {
                var finishedHomeCommand = root.stepperHomePendingCommand
                var homeResult = ok
                        ? ("F4已将当前位置设为零点：" + detail)
                        : ("F4当前位置设零失败：" + detail)
                if (ok && finishedHomeCommand === "ACTUATOR_HOME_2") {
                    root.manualCameraZZeroKnown = true
                    root.manualCameraZOffsetSteps = 0
                    homeResult = homeResult + "；上下轴本地回原位偏移已清零"
                }
                root.stepperHomeSending = false
                root.stepperHomePendingCommand = ""
                root.stepperMotorResultText = homeResult
                root.settingsLastActionText = homeResult
                root.storageState = root.formatF4ToastText(homeResult + " [" + finishedHomeCommand + "]")
                root.showStorageToast()
                root.evaluateRuntimeAlarms()
                return
            }

        if (action === "ACTUATOR_STOP_NOW") {
            var stopNowResult = ok
                    ? ("F4强制停止帧已写入：" + detail)
                    : ("F4强制停止帧写入失败：" + detail)
            root.manualLastAckText = stopNowResult
            root.storageState = root.formatF4ToastText(stopNowResult)
            root.appendManualCommandLog(action, "三轴电机", stopNowResult)
            root.showStorageToast()
            root.evaluateRuntimeAlarms()
            return
        }

        if (root.manualPendingF4Command !== "") {
            var finishedManualCommand = root.manualPendingF4Command
            var manualResult = ok ? ("F4执行器回执：" + detail) : ("F4执行器失败：" + detail)
            if (ok && finishedManualCommand.indexOf("ACTUATOR_VEL_MOVE") === 0) {
                manualResult = "F4执行器回执：" + detail + "；持续运动中，按停止结束"
            } else if (ok && finishedManualCommand.indexOf("ACTUATOR_POS_MOVE_Z") === 0) {
                root.updateManualCameraZOffsetAfterMove(root.manualPendingZDirection,
                                                        root.manualPendingZSteps,
                                                        root.manualPendingZReturnHome)
                manualResult = "F4执行器回执：" + detail + "；上下轴固定步数已完成"
                if (root.manualCameraZZeroKnown) {
                    manualResult = manualResult
                            + "；本地零点偏移="
                            + Math.floor(Number(root.manualCameraZOffsetSteps || 0))
                            + " step"
                }
            }
            root.manualLastAckText = manualResult
            root.manualPendingF4Command = ""
            root.clearManualPendingMotionContext()
            root.storageState = root.formatF4ToastText(manualResult)
            root.appendManualCommandLog(action, "三轴电机", manualResult)
            root.showStorageToast()
        }

            root.evaluateRuntimeAlarms()
        }

        /*
         * onF4ArmInspectionFlowFinished 的作用：
         *   接收 F4 主动回传的称重和电感数据，收齐后启动一次性完整云端上传。
         *
         * 主要流程：
         *   1. 成功时把 WEIGHT_RESULT/LDC_RESULT/F4流程上下文交给 storageController。
         *   2. 失败时停止本轮自动流程并提示人工处理，避免缺少传感器数据仍上传完整记录。
         *   3. 上传动作不在 F4 回调里直接拼 JSON，保证云端字段由 C++ 上传控制器统一生成。
         */
        onF4ArmInspectionFlowFinished: {
            if (ok) {
                root.workflowState = "完整上传"
                root.storageState = "称重和电感数据已收齐，正在把图片、模型和传感器数据一次性上传"
                root.showStorageToast()
                storageController.uploadCompletedInspectionBundle(weightContextJson,
                                                                  ldcContextJson,
                                                                  f4FlowContextJson)
            } else {
                root.workflowState = "待人工处理"
                root.storageState = "F4机械臂称重/电感流程失败：" + detail
                root.raiseRuntimeAlarm("f4-link-failed",
                                       "ALM-F4-001",
                                       "告警",
                                       "F4机械臂检测流程失败",
                                       detail)
                root.showStorageToast()
            }
        }

        /*
         * onF4FinalSortFinished 的作用：
         *   接收 F4 最终分拣和 CYCLE_DONE 结果，本轮自动检测到这里才闭环完成。
         *
         * 说明：
         *   如果前一步完整上传失败，C++ 会把 FINAL_SORT_RESULT 改为 upload_status=2/final_bin=3，
         *   因此这里收到成功也可能表示“已放入待复核盘”，不是云端上传成功。
         */
        onF4FinalSortFinished: {
            if (ok) {
                root.workflowState = "本轮完成"
                root.storageState = "F4已完成最终分拣：" + detail + " " + cycleDoneContextJson
                root.markAlarmRecovered("f4-link-failed")
                root.markAlarmRecovered("cloud-upload-failed")
            } else {
                root.workflowState = "分拣异常"
                root.storageState = "F4最终分拣失败：" + detail
                root.raiseRuntimeAlarm("f4-link-failed",
                                       "ALM-F4-001",
                                       "告警",
                                       "F4最终分拣失败",
                                       detail)
            }
            root.showStorageToast()
            root.evaluateRuntimeAlarms()
        }

        /*
         * onDetailTextChanged 的作用：
         *   当 C++ 后台 F4 心跳或异步故障更新设备详情时，把这条 F4 二进制回包摘要同步到底部提示。
         *
         * 说明：
         *   detailText 也会被 4G、云端、相机和 SD 卡刷新修改，因此这里先按文本前缀过滤，
         *   只处理确实来自 F4 的详情，避免把其它设备健康提示误标成 F4。
         */
        onDetailTextChanged: {
            var detail = deviceHealth.detailText
            if (detail.indexOf("F4") === 0) {
                root.storageState = root.formatF4ToastText(detail)
                root.showStorageToast()
            }
        }

        /*
         * onCloudStatusChanged 的作用：
         *   云端 health 状态变化后检查上传/云端类告警，弱网恢复时允许下一次失败重新记录。
         */
        onCloudStatusChanged: {
            root.evaluateRuntimeAlarms()
        }

        /*
         * onSdcardStatusChanged 的作用：
         *   SD 卡挂载或可写状态变化后检查存储类告警，避免日志误写 rootfs 前没有界面提示。
         */
        onSdcardStatusChanged: {
            root.evaluateRuntimeAlarms()
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
            if (resultText.indexOf("保存失败") === 0 || resultText.indexOf("上传失败") === 0) {
                root.raiseRuntimeAlarm(resultText.indexOf("上传失败") === 0 ? "cloud-upload-failed" : "storage-save-failed",
                                       resultText.indexOf("上传失败") === 0 ? "ALM-UPLOAD-001" : "ALM-SAVE-001",
                                       "预警",
                                       resultText.indexOf("上传失败") === 0 ? "云端上传失败" : "图片保存失败",
                                       resultText)
            } else if (resultText.indexOf("保存成功") === 0 || resultText.indexOf("上传成功") === 0) {
                root.markAlarmRecovered("storage-save-failed")
                root.markAlarmRecovered("cloud-upload-failed")
            }
            showStorageToast()

            if (root.autoVisionDetectFromZFlow) {
                root.autoVisionDetectFromZFlow = false
                root.autoVisionRequestZUp()
            }
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
                storageState = root.settingsUploadEnabled ? "综合判定完成，正在上传..." : "综合判定完成，自动上传已关闭"
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
            if (resultText.indexOf("RESULT ") === 0) {
                root.latestModelResultText = resultText
            }
            storageState = resultText
            if (resultText.indexOf("检测失败") === 0) {
                root.raiseRuntimeAlarm("model-detect-failed",
                                       "ALM-MODEL-001",
                                       "预警",
                                       "模型检测链路失败",
                                       resultText)
            } else {
                root.markAlarmRecovered("model-detect-failed")
                if (isUploadStatusFailure(resultText)) {
                    root.raiseRuntimeAlarm("cloud-upload-failed",
                                           "ALM-UPLOAD-001",
                                           "预警",
                                           "云端上传失败",
                                           resultText)
                } else {
                    root.markAlarmRecovered("cloud-upload-failed")
                }
            }
            showStorageToast()

            if (root.autoVisionDetectFromZFlow) {
                root.autoVisionDetectFromZFlow = false
                root.autoVisionRequestZUp()
            }
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
                root.markAlarmRecovered("cloud-upload-failed")
            } else if (row === root.selectedHistoryIndex) {
                root.selectedHistoryRecord = uploadHistory.entryAt(root.selectedHistoryIndex)
            }

            storageState = resultText
            if (resultText.indexOf("重新发送失败") === 0) {
                root.raiseRuntimeAlarm("cloud-upload-failed",
                                       "ALM-UPLOAD-001",
                                       "预警",
                                       "云端上传失败",
                                       resultText)
            }
            showStorageToast()
        }

        /*
         * onCompletedInspectionBundleUploaded 的作用：
         *   自动流程完整上传结束后，把上传结果交给 F4 做最终分拣决策。
         *
         * 主要流程：
         *   1. 成功上传时按云端 result 分拣到良品盘、不良品盘或待复核盘。
         *   2. 上传失败时本地历史已经保留，仍然通知 F4，但 C++ 会强制 final_bin=3 待复核盘。
         *   3. 只有 F4 后续返回 CYCLE_DONE，才认为这一件零件自动检测流程真正完成。
         */
        onCompletedInspectionBundleUploaded: {
            root.latestCompletedUploadResultText = resultText
            root.latestCompletedCloudResult = cloudResult && cloudResult.length > 0 ? cloudResult : "review"
            root.storageState = resultText

            if (ok) {
                root.workflowState = "等待分拣"
                root.markAlarmRecovered("cloud-upload-failed")
            } else {
                root.workflowState = "上传失败待复核"
                root.raiseRuntimeAlarm("cloud-upload-failed",
                                       "ALM-UPLOAD-001",
                                       "预警",
                                       "完整上传失败，零件将进入待复核盘",
                                       resultText)
            }

            root.showStorageToast()

            if (!deviceHealth.requestF4FinalSortResult(root.latestCompletedCloudResult,
                                                       root.latestCompletedUploadResultText,
                                                       root.settingsF4ArmResultTimeoutMs)) {
                root.storageState = "F4最终分拣命令未启动：" + root.latestCompletedUploadResultText
                root.showStorageToast()
            }
        }
    }

    /* 顶部状态栏：显示系统时间、通信状态、相机状态、F4状态、高德 IP 省份位置和云端状态。 */
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
                    {"name": "位置", "value": deviceHealth.locationShortText, "dot": deviceHealth.locationStatusColor},
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
                    {"text": "告警维护", "page": "alarm"},
                    {"text": "日志查看", "page": "logs"}
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
                 && !root.settingsPageVisible && !root.alarmPageVisible && !root.logPageVisible

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
            text: "dy=" + root.dxPixelsText
            color: root.dxPixelsValid && Math.abs(dxPixels) <= root.autoVisionCenterTolerancePx ? root.accentGreen : root.accentAmber
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
                 && !root.settingsPageVisible && !root.alarmPageVisible && !root.logPageVisible

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
                    {"name": "偏差", "value": root.dxPixelsText},
                    {"name": "类别", "value": root.compactHomeClassText(root.detectClassName)},
                    {"name": "模型", "value": root.compactHomeModelText(root.detectState)},
                    {"name": "耗时", "value": root.detectTimeText}
                ] : [
                    {"name": "当前零件", "value": root.detectPartName},
                    {"name": "流程状态", "value": root.workflowState},
                    {"name": "视觉偏差", "value": root.dxPixelsText},
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
                    property bool actionEnabled: !root.autoControlBusy && !root.autoVisionCommandBusy
                                                 && ((modelData.action === "start" && !root.autoWorkflowRunning && !root.autoWorkflowPaused)
                                                     || (modelData.action === "pause" && root.autoWorkflowRunning && !root.autoWorkflowPaused)
                                                     || (modelData.action === "resume" && root.autoWorkflowPaused)
                                                     || (modelData.action === "stop" && (root.autoWorkflowRunning || root.autoWorkflowPaused)))
                    color: !actionEnabled ? "#171b1e" : (overlayButtonMouse.pressed ? "#2d3338" : "#22272b")
                    border.color: actionEnabled ? modelData.color : "#3a4248"
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: modelData.text
                        color: parent.actionEnabled ? "#ffffff" : "#6d777d"
                        font.pixelSize: 12
                        font.bold: true
                    }

                    MouseArea {
                        id: overlayButtonMouse
                        anchors.fill: parent

                        onClicked: {
                            if (!parent.actionEnabled) {
                                return
                            }
                            root.handleControlAction(modelData.action, modelData.state)
                        }
                    }
                }
            }
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
                boundsBehavior: Flickable.DragOverBounds
                highlightRangeMode: ListView.NoHighlightRange
                interactive: uploadHistory.count > 0
                cacheBuffer: width * root.uiListCachePages
                flickDeceleration: root.uiHorizontalFlickDeceleration
                maximumFlickVelocity: root.uiHorizontalFlickVelocity
                highlightMoveDuration: 120

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
                    boundsBehavior: Flickable.DragOverBounds
                    interactive: root.historyImages().length > 1
                    cacheBuffer: width * root.uiCarouselCachePages
                    flickDeceleration: root.uiHorizontalFlickDeceleration
                    maximumFlickVelocity: root.uiHorizontalFlickVelocity
                    highlightMoveDuration: 120
                    clip: true

                    delegate: Image {
                        width: imageCarousel.width
                        height: imageCarousel.height
                        source: "file://" + modelData.path
                        sourceSize.width: imageCarousel.width
                        sourceSize.height: imageCarousel.height
                        fillMode: Image.PreserveAspectFit
                        smooth: !imageCarousel.moving
                        asynchronous: true
                        cache: true
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
                boundsBehavior: Flickable.DragOverBounds
                maximumFlickVelocity: root.uiVerticalFlickVelocity
                flickDeceleration: root.uiVerticalFlickDeceleration
                interactive: contentHeight > height

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
                    boundsBehavior: Flickable.DragOverBounds
                    cacheBuffer: height * root.uiListCachePages
                    maximumFlickVelocity: root.uiVerticalFlickVelocity
                    flickDeceleration: root.uiVerticalFlickDeceleration

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

        /* manualBeltPanel 负责进入三轴手动控制弹窗，同时保留传送带停止和状态查询的快捷入口。 */
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
                text: "三轴电机"
                color: "#f1f4f5"
                font.pixelSize: 18
                font.bold: true
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 14
                y: 13
                text: root.manualBeltState + " / " + root.manualBeltCommandText
                color: root.manualBeltState === "停止" ? root.accentGreen : root.accentAmber
                font.pixelSize: 12
                font.bold: true
                elide: Text.ElideRight
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
                        {"text": "三轴控制", "action": "open-motor-popup", "color": "#5aa7ff"},
                        {"text": "停止", "action": "stop", "color": root.accentRed},
                        {"text": "查询状态", "action": "belt-info", "color": "#5aa7ff"}
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

            Rectangle {
                x: 14
                y: 128
                width: parent.width - 28
                height: 26
                radius: 6
                color: "#171b1e"
                border.color: "#30363b"
                border.width: 1

                Text {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    verticalAlignment: Text.AlignVCenter
                    text: "协议：ACTUATOR_POS_MOVE/STOP；传送带/左右/上下三页操作"
                    color: "#9aa5ab"
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }
        }

        /* manualAssistPanel 只保留检测辅助和人工标记入口，不再显示现场未接入的硬件控制项。 */
        Rectangle {
            id: manualAssistPanel
            x: 332
            y: 62
            width: 484
            height: 164
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1

            Text {
                x: 14
                y: 10
                text: "检测辅助"
                color: "#f1f4f5"
                font.pixelSize: 18
                font.bold: true
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 14
                y: 13
                text: "本地标记"
                color: root.accentAmber
                font.pixelSize: 12
                font.bold: true
            }

            Grid {
                id: manualAssistButtonGrid
                x: 14
                y: 42
                width: parent.width - 28
                columns: 3
                rowSpacing: 8
                columnSpacing: 8

                Repeater {
                    model: [
                        {"text": "检测当前帧", "action": "detect-frame", "color": root.accentGreen},
                        {"text": "刷新状态", "action": "refresh", "color": "#5aa7ff"},
                        {"text": "标记GOOD", "action": "mark-good", "color": root.accentGreen},
                        {"text": "标记BAD", "action": "mark-bad", "color": root.accentRed},
                        {"text": "待复核", "action": "mark-uncertain", "color": root.accentAmber}
                    ]

                    Rectangle {
                        property bool actionAllowed: root.manualActionAllowed(modelData.action)

                        width: (manualAssistButtonGrid.width - manualAssistButtonGrid.columnSpacing * 2) / 3
                        height: 34
                        radius: 6
                        color: actionAllowed ? (manualAssistMouse.pressed ? "#2d3338" : "#22272b") : "#171b1e"
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
                            id: manualAssistMouse
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

            Flickable {
                id: manualSafetyFlickable
                x: 14
                y: 42
                width: parent.width - 28
                height: 138
                clip: true
                contentHeight: manualSafetyColumn.height
                boundsBehavior: Flickable.DragOverBounds
                maximumFlickVelocity: root.uiVerticalFlickVelocity
                flickDeceleration: root.uiVerticalFlickDeceleration
                interactive: contentHeight > height

                Column {
                    id: manualSafetyColumn
                    width: manualSafetyFlickable.width
                    spacing: 8

                    Repeater {
                        model: [
                            {"name": "F4控制器", "value": deviceHealth.f4StatusText, "color": deviceHealth.f4StatusColor},
                            {"name": "手动模式", "value": root.manualMode ? "允许" : "未进入", "color": root.manualMode ? root.accentAmber : root.accentGreen},
                            {"name": "急停", "value": root.manualEmergencyStop ? "已按下" : "释放", "color": root.manualEmergencyStop ? root.accentRed : root.accentGreen},
                            {"name": "限位", "value": "未触发", "color": root.accentGreen},
                            {"name": "传送带", "value": root.manualBeltState, "color": root.manualBeltState === "停止" ? root.accentGreen : root.accentAmber},
                            {"name": "左右轴", "value": "位置模式待命", "color": root.accentGreen},
                            {"name": "上下轴", "value": root.autoVisionNeedsZUp ? "等待回升" : "位置模式待命", "color": root.autoVisionNeedsZUp ? root.accentAmber : root.accentGreen},
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
                boundsBehavior: Flickable.DragOverBounds
                cacheBuffer: height * root.uiListCachePages
                maximumFlickVelocity: root.uiVerticalFlickVelocity
                flickDeceleration: root.uiVerticalFlickDeceleration

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

    Rectangle {
        id: manualMotorPopupOverlay
        anchors.fill: parent
        z: 892
        visible: root.manualMotorPopup
        color: "#b0000000"

        MouseArea {
            anchors.fill: parent

            onClicked: {
                root.manualMotorPopup = false
            }
        }

        Rectangle {
            id: manualMotorPopupPanel
            width: 610
            height: 430
            anchors.centerIn: parent
            radius: 10
            color: "#20262a"
            border.color: root.manualEmergencyStop ? root.accentRed : root.accentGreen
            border.width: 1
            clip: true

            property var motorConfig: root.manualMotorSetting()
            property int actuatorId: root.manualMotorPageIndex
            property string negativeLabel: root.manualMotorPageIndex === 2 ? "下降" : (root.manualMotorPageIndex === 1 ? "左移" : "后退")
            property string positiveLabel: root.manualMotorPageIndex === 2 ? "上升" : (root.manualMotorPageIndex === 1 ? "右移" : "前进")

            MouseArea {
                anchors.fill: parent
            }

            Text {
                x: 18
                y: 14
                width: parent.width - 126
                text: "三轴手动控制"
                color: "#f1f4f5"
                font.pixelSize: 20
                font.bold: true
                elide: Text.ElideRight
            }

            Rectangle {
                x: parent.width - 90
                y: 12
                width: 72
                height: 30
                radius: 7
                color: closeManualMotorMouse.pressed ? "#3a1b1f" : "#2a2020"
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
                    id: closeManualMotorMouse
                    anchors.fill: parent

                    onClicked: {
                        root.manualMotorPopup = false
                    }
                }
            }

            Row {
                id: manualMotorPageTabs
                x: 18
                y: 58
                width: parent.width - 36
                height: 38
                spacing: 8

                Repeater {
                    model: detectSettings.stepperMotorSettings

                    Rectangle {
                        width: (manualMotorPageTabs.width - 16) / 3
                        height: 38
                        radius: 7
                        color: root.manualMotorPageIndex === index
                               ? "#1f332b"
                               : (manualMotorTabMouse.pressed ? "#26323a" : "#1a2024")
                        border.color: root.manualMotorPageIndex === index ? root.accentGreen : "#344149"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: modelData.name
                            color: root.manualMotorPageIndex === index ? "#eafff2" : "#d7dee2"
                            font.pixelSize: 13
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        MouseArea {
                            id: manualMotorTabMouse
                            anchors.fill: parent

                            onClicked: {
                                root.manualMotorPageIndex = index
                                root.manualLastAckText = modelData.name + " 手动页"
                            }
                        }
                    }
                }
            }

            Rectangle {
                x: 18
                y: 114
                width: parent.width - 36
                height: 92
                radius: 8
                color: "#171b1e"
                border.color: "#344149"
                border.width: 1

                Text {
                    x: 14
                    y: 12
                    width: parent.width - 28
                    text: (manualMotorPopupPanel.motorConfig.name || "--")
                          + "  ID " + (manualMotorPopupPanel.motorConfig.addressHex || "--")
                          + "  " + (manualMotorPopupPanel.motorConfig.serialName || "--")
                    color: "#f1f4f5"
                    font.pixelSize: 16
                    font.bold: true
                    elide: Text.ElideRight
                }

                Text {
                    x: 14
                    y: 44
                    width: parent.width - 28
                    height: 34
                    text: root.manualMotorPageIndex === 2
                          ? ("上下轴：下降 "
                             + Math.floor(Number(manualMotorPopupPanel.motorConfig.zDownFixedSteps || 0))
                             + " step，上升 "
                             + Math.floor(Number(manualMotorPopupPanel.motorConfig.zUpFixedSteps || 0))
                             + " step；回原位按偏移 "
                             + (root.manualCameraZZeroKnown
                                ? (Math.floor(Number(root.manualCameraZOffsetSteps || 0)) + " step")
                                : "未设零")
                             + "。")
                          : ("速度 "
                             + (manualMotorPopupPanel.motorConfig.normalSpeedRpm || 0)
                             + " rpm；点击方向键后持续运动，按停止键结束。")
                    color: "#aeb8be"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
            }

            Row {
                x: 18
                y: 232
                width: parent.width - 36
                height: 72
                spacing: 12

                Repeater {
                    id: manualMotorActionRepeater
                    model: root.manualMotorActionButtons()

                    Rectangle {
                        property bool actionAllowed: root.manualMode && !root.manualEmergencyStop
                        property bool stopButton: modelData.action === "stop"

                        width: (parent.width - parent.spacing * Math.max(0, manualMotorActionRepeater.count - 1))
                               / Math.max(1, manualMotorActionRepeater.count)
                        height: 72
                        radius: 8
                        color: actionAllowed || stopButton
                               ? (manualMotorActionMouse.pressed ? "#2d3338" : "#22272b")
                               : "#171b1e"
                        border.color: actionAllowed || stopButton ? modelData.color : "#3a4147"
                        border.width: 1
                        opacity: actionAllowed || stopButton ? 1.0 : 0.45

                        Text {
                            anchors.centerIn: parent
                            text: modelData.text
                            color: "#ffffff"
                            font.pixelSize: 18
                            font.bold: true
                        }

                        MouseArea {
                            id: manualMotorActionMouse
                            anchors.fill: parent
                            enabled: parent.actionAllowed || parent.stopButton

                            onClicked: {
                                if (parent.stopButton) {
                                    root.sendManualActuatorStop(manualMotorPopupPanel.actuatorId, modelData.text)
                                } else if (modelData.action === "return") {
                                    root.sendManualActuatorZReturnHome(modelData.text)
                                } else {
                                    root.sendManualActuatorMove(manualMotorPopupPanel.actuatorId,
                                                                modelData.direction,
                                                                modelData.text)
                                }
                            }
                        }
                    }
                }
            }

            Text {
                x: 18
                y: 326
                width: parent.width - 36
                height: 54
                text: root.manualEmergencyStop
                      ? "急停已按下：只允许停止和清故障；释放后仍需确认 F4/现场联锁。"
                      : (root.manualMode
                         ? "手动模式已允许：传送带/左右轴按一次持续运动；上下轴按一次走固定步数。"
                         : "请先点击手动页“进入手动”，再执行三轴手动动作。")
                color: root.manualEmergencyStop ? "#ffd6dc" : "#dce3e6"
                font.pixelSize: 13
                font.bold: true
                wrapMode: Text.Wrap
            }
        }
    }

    /* settingsPage 是参数设置界面：参数值直接绑定 C++ DetectSettingsController，并保存到 SD 卡 JSON。 */
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

        /* settingsProcessPanel 负责真实零件类型、模型阈值和复核阈值，直接对应当前检测结果融合策略。 */
        Rectangle {
            id: settingsProcessPanel
            x: 16
            y: 62
            width: 252
            height: 190
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1
            clip: true

            Text {
                x: 14
                y: 10
                text: "零件与模型判定"
                color: "#f1f4f5"
                font.pixelSize: 15
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
                    anchors.right: parent.right
                    anchors.rightMargin: 58
                    anchors.verticalCenter: parent.verticalCenter
                    text: "零件：" + root.settingsPartType
                    color: "#ffffff"
                    font.pixelSize: 13
                    font.bold: true
                    elide: Text.ElideRight
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
                        {"label": "模型阈值", "value": root.settingsThresholdText(root.settingsDecisionThreshold), "key": "decision", "step": 10, "note": "好坏线"},
                        {"label": "复核阈值", "value": root.settingsThresholdText(root.settingsReviewThreshold), "key": "review", "step": 10, "note": "低可信"},
                        {"label": "ROI大小", "value": root.settingsRoiSize + "px", "key": "roi", "step": 20, "note": "中心裁剪"}
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

        /* settingsVisionPanel 展示当前视觉检测链路中真实存在的模型策略，不再伪装成已经下发硬件参数。 */
        Rectangle {
            id: settingsVisionPanel
            x: 284
            y: 62
            width: 252
            height: 190
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1
            clip: true

            Text {
                x: 14
                y: 10
                text: "视觉检测策略"
                color: "#f1f4f5"
                font.pixelSize: 16
                font.bold: true
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 14
                y: 14
                text: "真实检测配置"
                color: "#9fdcff"
                font.pixelSize: 12
                font.bold: true
            }

            Grid {
                id: settingsVisionSummaryColumn
                x: 14
                y: 38
                width: parent.width - 28
                columns: 2
                rowSpacing: 6
                columnSpacing: 8

                Repeater {
                    model: [
                        {"name": "分类阈值", "value": root.settingsThresholdText(root.settingsDecisionThreshold), "color": root.accentGreen},
                        {"name": "复核阈值", "value": root.settingsThresholdText(root.settingsReviewThreshold), "color": root.accentAmber},
                        {"name": "UNet像素", "value": root.settingsSegmentMinPixels + "px", "color": "#9fdcff"},
                        {"name": "叠加透明", "value": root.settingsOverlayAlpha.toFixed(2), "color": "#eef3f4"}
                    ]

                    Rectangle {
                        width: (settingsVisionSummaryColumn.width - settingsVisionSummaryColumn.columnSpacing) / 2
                        height: 24
                        radius: 5
                        color: "#20262a"
                        border.color: "#2f3840"
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
                            y: 3
                            width: parent.width - 30
                            text: modelData.name
                            color: "#9aa5ab"
                            font.pixelSize: 9
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        Text {
                            x: 22
                            y: 12
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
                width: 86
                height: 26
                radius: 7
                anchors.right: parent.right
                anchors.rightMargin: 14
                y: 132
                color: settingsVisionDetailMouse.pressed ? "#30413a" : "#1f332b"
                border.color: root.accentGreen
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "查看详情"
                    color: "#eafff2"
                    font.pixelSize: 12
                    font.bold: true
                }

                MouseArea {
                    id: settingsVisionDetailMouse
                    anchors.fill: parent

                    onClicked: {
                        root.openSettingsDetail("vision")
                    }
                }
            }

            Text {
                x: 14
                y: 132
                width: parent.width - 120
                text: "配置：" + root.settingsConfigPath
                color: "#8f9aa1"
                font.pixelSize: 11
                elide: Text.ElideRight
            }

            Row {
                id: settingsVisionStepperRow
                x: 14
                y: 98
                width: parent.width - 28
                height: 28
                spacing: 6

                Repeater {
                    model: [
                        {"text": "UNet-", "key": "segment", "delta": -10, "color": "#5aa7ff"},
                        {"text": "UNet+", "key": "segment", "delta": 10, "color": "#5aa7ff"},
                        {"text": "透明-", "key": "alpha", "delta": -0.05, "color": root.accentGreen},
                        {"text": "透明+", "key": "alpha", "delta": 0.05, "color": root.accentGreen}
                    ]

                    Rectangle {
                        width: (settingsVisionStepperRow.width - 18) / 4
                        height: 28
                        radius: 6
                        color: settingsVisionStepMouse.pressed ? "#2d3338" : "#22272b"
                        border.color: modelData.color
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: modelData.text
                            color: "#ffffff"
                            font.pixelSize: 11
                            font.bold: true
                        }

                        MouseArea {
                            id: settingsVisionStepMouse
                            anchors.fill: parent

                            onClicked: {
                                root.changeSettingValue(modelData.key, modelData.delta)
                            }
                        }
                    }
                }
            }
        }

        /* settingsMotionPanel 说明 F4 运动控制边界；当前 Qt 参数页不提供会误导用户的硬件运动可调项。 */
        Rectangle {
            id: settingsMotionPanel
            x: 552
            y: 62
            width: 264
            height: 190
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1
            clip: true

            Text {
                x: 14
                y: 10
                text: "F4接入边界"
                color: "#f1f4f5"
                font.pixelSize: 16
                font.bold: true
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 14
                y: 14
                text: "F4管运动"
                color: root.accentAmber
                font.pixelSize: 12
                font.bold: true
            }

            Column {
                id: settingsMotionSummaryColumn
                x: 14
                y: 38
                width: parent.width - 28
                spacing: 4

                Repeater {
                    model: [
                        {"name": "传送带", "value": "BELT命令已接", "color": root.accentGreen},
                        {"name": "称重", "value": "WEIGHT标定已接", "color": root.accentGreen},
                        {"name": "串口边界", "value": "MP157只发高层命令", "color": "#9fdcff"},
                        {"name": "安全联锁", "value": "仍由F407执行", "color": "#eef3f4"}
                    ]

                    Row {
                        width: parent.width
                        height: 18
                        spacing: 8

                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            anchors.verticalCenter: parent.verticalCenter
                            color: modelData.color
                        }

                        Text {
                            width: 70
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name
                            color: "#9aa5ab"
                            font.pixelSize: 11
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width - 94
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

            Rectangle {
                x: 14
                y: 132
                width: 68
                height: 26
                radius: 7
                color: calibrationOpenMouse.pressed ? "#273940" : "#1d3036"
                border.color: "#5aa7ff"
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "称重"
                    color: "#e8f5ff"
                    font.pixelSize: 11
                    font.bold: true
                }

                MouseArea {
                    id: calibrationOpenMouse
                    anchors.fill: parent

                    onClicked: {
                        root.openCalibrationPopup()
                    }
                }
            }

            Rectangle {
                x: 90
                y: 132
                width: 94
                height: 26
                radius: 7
                color: stepperMotorOpenMouse.pressed ? "#30413a" : "#1f332b"
                border.color: root.accentGreen
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "步进参数"
                    color: "#eafff2"
                    font.pixelSize: 11
                    font.bold: true
                }

                MouseArea {
                    id: stepperMotorOpenMouse
                    anchors.fill: parent

                    onClicked: {
                        root.openStepperMotorPopup(0)
                    }
                }
            }

            Rectangle {
                width: 58
                height: 26
                radius: 7
                anchors.right: parent.right
                anchors.rightMargin: 14
                y: 132
                color: settingsF4DetailMouse.pressed ? "#3c3322" : "#33291b"
                border.color: root.accentAmber
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "详情"
                    color: "#fff3d5"
                    font.pixelSize: 11
                    font.bold: true
                }

                MouseArea {
                    id: settingsF4DetailMouse
                    anchors.fill: parent

                    onClicked: {
                        root.openSettingsDetail("f4")
                    }
                }
            }
        }

        /* settingsStoragePanel 汇总相机、SD 卡和 COS 上传策略，贴合当前已打通的数据路径。 */
        Rectangle {
            id: settingsStoragePanel
            x: 16
            y: 266
            width: 398
            height: 226
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1
            clip: true

            Text {
                x: 14
                y: 10
                text: "相机、存储与上传"
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
                        {"name": "SD目录", "value": "/mnt/sdcard/images", "color": "#eef3f4"},
                        {"name": "COS上传", "value": root.settingsUploadEnabled ? "自动上传" : "本地保存", "color": root.settingsUploadEnabled ? root.accentGreen : root.accentAmber}
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
                y: 184
                width: parent.width - 28
                height: 28
                radius: 6
                color: uploadToggleMouse.pressed ? "#2d3338" : "#22272b"
                border.color: root.settingsUploadEnabled ? root.accentGreen : root.accentAmber
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: root.settingsUploadEnabled ? "切换为手动上传" : "切换为自动上传"
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
            y: 266
            width: 386
            height: 226
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
                id: settingsArmTimeoutControl
                x: 14
                y: 60
                width: parent.width - 28
                height: 30
                radius: 6
                color: "#15191c"
                border.color: "#343d43"
                border.width: 1

                Text {
                    x: 10
                    width: 86
                    anchors.verticalCenter: parent.verticalCenter
                    text: "机械臂等待"
                    color: "#dce3e6"
                    font.pixelSize: 12
                    font.bold: true
                    elide: Text.ElideRight
                }

                Rectangle {
                    x: 102
                    width: 76
                    height: 22
                    radius: 5
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#20262a"
                    border.color: root.accentAmber
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: root.settingsF4ArmResultTimeoutText()
                        color: "#fff3d5"
                        font.pixelSize: 12
                        font.bold: true
                    }
                }

                Repeater {
                    model: [
                        {"text": "-10秒", "delta": -10000, "color": "#9aa6ad"},
                        {"text": "+10秒", "delta": 10000, "color": root.accentGreen}
                    ]

                    Rectangle {
                        x: 190 + index * 78
                        width: 70
                        height: 22
                        radius: 5
                        anchors.verticalCenter: parent.verticalCenter
                        color: settingsArmTimeoutMouse.pressed ? "#2d3338" : "#22272b"
                        border.color: modelData.color
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: modelData.text
                            color: "#ffffff"
                            font.pixelSize: 11
                            font.bold: true
                        }

                        MouseArea {
                            id: settingsArmTimeoutMouse
                            anchors.fill: parent

                            onClicked: {
                                root.changeSettingValue("f4-arm-timeout", modelData.delta)
                            }
                        }
                    }
                }
            }

            Rectangle {
                x: 14
                y: 96
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
                y: 138
                width: parent.width - 28
                columns: 2
                rowSpacing: 8
                columnSpacing: 8

                Repeater {
                    model: [
                        {"text": "应用检测", "action": "apply", "color": root.accentGreen},
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

    Rectangle {
        id: settingsDetailOverlay
        anchors.fill: parent
        z: 890
        visible: root.settingsDetailVisible
        color: "#b0000000"

        MouseArea {
            anchors.fill: parent

            onClicked: {
                root.settingsDetailVisible = false
            }
        }

        Rectangle {
            width: 640
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
                width: parent.width - 126
                text: root.settingsDetailTitle
                color: "#f1f4f5"
                font.pixelSize: 18
                font.bold: true
                elide: Text.ElideRight
            }

            Rectangle {
                x: parent.width - 90
                y: 12
                width: 72
                height: 30
                radius: 7
                color: closeSettingsDetailMouse.pressed ? "#3a1b1f" : "#2a2020"
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
                    id: closeSettingsDetailMouse
                    anchors.fill: parent

                    onClicked: {
                        root.settingsDetailVisible = false
                    }
                }
            }

            Flickable {
                id: settingsDetailFlickable
                x: 18
                y: 56
                width: parent.width - 36
                height: parent.height - 74
                contentWidth: width
                contentHeight: fullSettingsDetailText.height
                clip: true
                boundsBehavior: Flickable.DragOverBounds
                maximumFlickVelocity: root.uiVerticalFlickVelocity
                flickDeceleration: root.uiVerticalFlickDeceleration
                interactive: contentHeight > height

                Text {
                    id: fullSettingsDetailText
                    width: settingsDetailFlickable.width
                    text: root.settingsDetailText
                    color: "#d7dee2"
                    font.pixelSize: 15
                    lineHeightMode: Text.ProportionalHeight
                    lineHeight: 1.26
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    Rectangle {
        id: stepperMotorPopup
        anchors.fill: parent
        z: 894
        visible: root.stepperMotorPopupVisible
        color: "#b0000000"

        MouseArea {
            anchors.fill: parent

            onClicked: {
                root.stepperMotorPopupVisible = false
            }
        }

        Rectangle {
            id: stepperMotorPopupPanel
            width: 660
            height: 560
            anchors.centerIn: parent
            radius: 10
            color: "#20262a"
            border.color: root.accentGreen
            border.width: 1
            clip: true

            /* motorConfig 保存当前页电机参数对象，所有字段显示都从 C++ 配置控制器读取。 */
            property var motorConfig: root.stepperMotorSettingsRevision >= 0
                                      ? root.currentStepperMotorSetting()
                                      : ({})

            MouseArea {
                anchors.fill: parent
            }

            Text {
                x: 18
                y: 14
                width: parent.width - 126
                text: "步进电机参数"
                color: "#f1f4f5"
                font.pixelSize: 20
                font.bold: true
                elide: Text.ElideRight
            }

            Rectangle {
                x: parent.width - 90
                y: 12
                width: 72
                height: 30
                radius: 7
                color: closeStepperMotorMouse.pressed ? "#3a1b1f" : "#2a2020"
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
                    id: closeStepperMotorMouse
                    anchors.fill: parent

                    onClicked: {
                        root.stepperMotorPopupVisible = false
                    }
                }
            }

            Text {
                x: 18
                y: 50
                width: parent.width - 36
                height: 34
                text: "保存并下发会写入 MP157 JSON 并通知 F407；真正运动、限幅、急停和联锁仍由 F407 固件执行。"
                color: "#cfd7db"
                font.pixelSize: 13
                font.bold: true
                wrapMode: Text.Wrap
            }

            Row {
                id: stepperMotorPageTabs
                x: 18
                y: 92
                width: parent.width - 36
                height: 36
                spacing: 8

                Repeater {
                    model: detectSettings.stepperMotorSettings

                    Rectangle {
                        width: (stepperMotorPageTabs.width - 16) / 3
                        height: 36
                        radius: 7
                        color: root.stepperMotorPageIndex === index
                               ? "#1f332b"
                               : (stepperTabMouse.pressed ? "#26323a" : "#1a2024")
                        border.color: root.stepperMotorPageIndex === index ? root.accentGreen : "#344149"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: modelData.name
                            color: root.stepperMotorPageIndex === index ? "#eafff2" : "#d7dee2"
                            font.pixelSize: 13
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        MouseArea {
                            id: stepperTabMouse
                            anchors.fill: parent

                            onClicked: {
                                root.stepperMotorPageIndex = index
                                root.stepperSpeedEditorVisible = false
                                root.stepperSpeedEditKey = "normalSpeedRpm"
                                root.stepperStepEditorVisible = false
                                root.stepperStepEditKey = ""
                                root.stepperSpeedInputText = "" + (modelData.normalSpeedRpm || 0)
                                root.stepperMotorResultText = modelData.name + " 参数页"
                            }
                        }
                    }
                }
            }

            Rectangle {
                x: 18
                y: 142
                width: parent.width - 36
                height: 314
                radius: 8
                color: "#171b1e"
                border.color: "#344149"
                border.width: 1

                /*
                 * stepperMotorSettingsFlickable 的作用：
                 *   1. 只负责中间参数区域的垂直滑动，标题、页签、结果栏和底部按钮保持固定。
                 *   2. 摄像头上下电机比其它电机多了下探、回升和超时时间三行，1024x600 屏幕上会超出固定卡片高度。
                 *   3. 通过 contentHeight 让触摸屏可以把被遮住的 Z 轴超时行滑出来，而不是继续依赖 clip 裁剪。
                 *   4. stepperMotorInputTouchGuard 标记下面按钮会禁止 Flickable 抢走点击，避免能滑动但输入按钮打不开。
                 */
                Flickable {
                    id: stepperMotorSettingsFlickable
                    anchors.fill: parent
                    clip: true
                    contentWidth: width
                    contentHeight: stepperMotorSettingsContent.height
                    flickableDirection: Flickable.VerticalFlick
                    boundsBehavior: Flickable.DragOverBounds
                    maximumFlickVelocity: root.uiVerticalFlickVelocity
                    flickDeceleration: root.uiVerticalFlickDeceleration
                    interactive: contentHeight > height

                    Item {
                        id: stepperMotorSettingsContent
                        width: stepperMotorSettingsFlickable.width
                        height: (stepperMotorPopupPanel.motorConfig.role || "") === "camera_z"
                                ? 360
                                : ((stepperMotorPopupPanel.motorConfig.role || "") === "conveyor" ? 280 : 240)
                    }
                }

                Text {
                    parent: stepperMotorSettingsContent
                    x: 14
                    y: 12
                    width: parent.width - 28
                    text: (stepperMotorPopupPanel.motorConfig.name || "--")
                          + "  " + (stepperMotorPopupPanel.motorConfig.serialName || "--")
                    color: "#f1f4f5"
                    font.pixelSize: 16
                    font.bold: true
                    elide: Text.ElideRight
                }

                Text {
                    parent: stepperMotorSettingsContent
                    x: 14
                    y: 38
                    width: parent.width - 28
                    height: 34
                    text: "ID 地址用于区分同一串口上的 Emm42；速度单位为 rpm，最小步长单位为 step。"
                    color: "#8f9aa1"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }

                Row {
                    parent: stepperMotorSettingsContent
                    x: 14
                    y: 72
                    width: parent.width - 28
                    height: 38
                    spacing: 8

                    Text {
                        width: 92
                        anchors.verticalCenter: parent.verticalCenter
                        text: "ID地址"
                        color: "#dce3e6"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    Rectangle {
                        width: 132
                        height: 34
                        radius: 7
                        color: "#20262a"
                        border.color: "#3b454b"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: root.stepperMotorAddressText(stepperMotorPopupPanel.motorConfig)
                            color: "#eef3f4"
                            font.pixelSize: 14
                            font.bold: true
                        }
                    }

                    Rectangle {
                        width: 84
                        height: 34
                        radius: 7
                        color: stepperAddressMinusMouse.pressed ? "#30363b" : "#22272b"
                        border.color: "#5aa7ff"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "地址-"
                            color: "#d9ecff"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: stepperAddressMinusMouse
                            anchors.fill: parent
                            preventStealing: true

                            onClicked: {
                                root.changeStepperMotorValue("address", -1)
                            }
                        }
                    }

                    Rectangle {
                        width: 84
                        height: 34
                        radius: 7
                        color: stepperAddressPlusMouse.pressed ? "#30413a" : "#1f332b"
                        border.color: root.accentGreen
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "地址+"
                            color: "#eafff2"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: stepperAddressPlusMouse
                            anchors.fill: parent
                            preventStealing: true

                            onClicked: {
                                root.changeStepperMotorValue("address", 1)
                            }
                        }
                    }
                }

                Row {
                    parent: stepperMotorSettingsContent
                    x: 14
                    y: 112
                    width: parent.width - 28
                    height: 38
                    spacing: 8

                    Text {
                        width: 92
                        anchors.verticalCenter: parent.verticalCenter
                        text: "最小步长"
                        color: "#dce3e6"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    Rectangle {
                        width: 172
                        height: 34
                        radius: 7
                        color: "#20262a"
                        border.color: "#3b454b"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: (stepperMotorPopupPanel.motorConfig.minStep || 0) + " step"
                            color: "#eef3f4"
                            font.pixelSize: 14
                            font.bold: true
                        }
                    }

                    Rectangle {
                        width: 84
                        height: 34
                        radius: 7
                        color: stepperMinStepMinusMouse.pressed ? "#30363b" : "#22272b"
                        border.color: "#5aa7ff"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "步长-50"
                            color: "#d9ecff"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: stepperMinStepMinusMouse
                            anchors.fill: parent
                            preventStealing: true

                            onClicked: {
                                root.changeStepperMotorValue("minStep", -50)
                            }
                        }
                    }

                    Rectangle {
                        width: 84
                        height: 34
                        radius: 7
                        color: stepperMinStepPlusMouse.pressed ? "#30413a" : "#1f332b"
                        border.color: root.accentGreen
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "步长+50"
                            color: "#eafff2"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: stepperMinStepPlusMouse
                            anchors.fill: parent
                            preventStealing: true

                            onClicked: {
                                root.changeStepperMotorValue("minStep", 50)
                            }
                        }
                    }
                }

                Row {
                    parent: stepperMotorSettingsContent
                    x: 14
                    y: 152
                    width: parent.width - 28
                    height: 38
                    spacing: 8

                    Text {
                        width: 92
                        anchors.verticalCenter: parent.verticalCenter
                        text: (stepperMotorPopupPanel.motorConfig.role || "") === "conveyor" ? "对中速度" : "常规速度"
                        color: "#dce3e6"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    Rectangle {
                        width: 172
                        height: 34
                        radius: 7
                        color: "#20262a"
                        border.color: "#3b454b"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: (stepperMotorPopupPanel.motorConfig.normalSpeedRpm || 0) + " rpm"
                            color: "#eef3f4"
                            font.pixelSize: 14
                            font.bold: true
                        }
                    }

                    Rectangle {
                        width: 72
                        height: 34
                        radius: 7
                        color: stepperSpeedMinusMouse.pressed ? "#30363b" : "#22272b"
                        border.color: "#5aa7ff"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "速度-"
                            color: "#d9ecff"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: stepperSpeedMinusMouse
                            anchors.fill: parent
                            preventStealing: true

                            onClicked: {
                                root.changeStepperMotorValue("normalSpeedRpm", -10)
                            }
                        }
                    }

                    Rectangle {
                        width: 72
                        height: 34
                        radius: 7
                        color: stepperSpeedInputMouse.pressed ? "#3c3322" : "#33291b"
                        border.color: root.accentAmber
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "输入"
                            color: "#fff3d5"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: stepperSpeedInputMouse
                            anchors.fill: parent
                            preventStealing: true

                            onClicked: {
                                root.openStepperSpeedEditor("normalSpeedRpm")
                            }
                        }
                    }

                    Rectangle {
                        width: 72
                        height: 34
                        radius: 7
                        color: stepperSpeedPlusMouse.pressed ? "#30413a" : "#1f332b"
                        border.color: root.accentGreen
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "速度+"
                            color: "#eafff2"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: stepperSpeedPlusMouse
                            anchors.fill: parent
                            preventStealing: true

                            onClicked: {
                                root.changeStepperMotorValue("normalSpeedRpm", 10)
                            }
                        }
                    }
                }

                Row {
                    parent: stepperMotorSettingsContent
                    x: 14
                    y: 192
                    width: parent.width - 28
                    height: 34
                    spacing: 8
                    visible: (stepperMotorPopupPanel.motorConfig.role || "") === "conveyor"

                    Text {
                        width: 92
                        anchors.verticalCenter: parent.verticalCenter
                        text: "上料速度"
                        color: "#dce3e6"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    Rectangle {
                        width: 172
                        height: 34
                        radius: 7
                        color: "#20262a"
                        border.color: "#3b454b"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: (stepperMotorPopupPanel.motorConfig.scanSpeedRpm || 0) + " rpm"
                            color: "#eef3f4"
                            font.pixelSize: 14
                            font.bold: true
                        }
                    }

                    Rectangle {
                        width: 72
                        height: 34
                        radius: 7
                        color: stepperScanSpeedMinusMouse.pressed ? "#30363b" : "#22272b"
                        border.color: "#5aa7ff"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "速度-"
                            color: "#d9ecff"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: stepperScanSpeedMinusMouse
                            anchors.fill: parent
                            preventStealing: true

                            onClicked: {
                                root.changeStepperMotorValue("scanSpeedRpm", -10)
                            }
                        }
                    }

                    Rectangle {
                        width: 72
                        height: 34
                        radius: 7
                        color: stepperScanSpeedInputMouse.pressed ? "#3c3322" : "#33291b"
                        border.color: root.accentAmber
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "输入"
                            color: "#fff3d5"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: stepperScanSpeedInputMouse
                            anchors.fill: parent
                            preventStealing: true

                            onClicked: {
                                root.openStepperSpeedEditor("scanSpeedRpm")
                            }
                        }
                    }

                    Rectangle {
                        width: 72
                        height: 34
                        radius: 7
                        color: stepperScanSpeedPlusMouse.pressed ? "#30413a" : "#1f332b"
                        border.color: root.accentGreen
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "速度+"
                            color: "#eafff2"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: stepperScanSpeedPlusMouse
                            anchors.fill: parent
                            preventStealing: true

                            onClicked: {
                                root.changeStepperMotorValue("scanSpeedRpm", 10)
                            }
                        }
                    }
                }

                Row {
                    parent: stepperMotorSettingsContent
                    x: 14
                    y: (stepperMotorPopupPanel.motorConfig.role || "") === "conveyor" ? 232 : 192
                    width: parent.width - 28
                    height: 34
                    spacing: 8

                    Text {
                        width: 92
                        anchors.verticalCenter: parent.verticalCenter
                        text: "方向"
                        color: "#dce3e6"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    Rectangle {
                        width: 172
                        height: 34
                        radius: 7
                        color: "#20262a"
                        border.color: "#3b454b"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: root.stepperMotorDirectionText(stepperMotorPopupPanel.motorConfig.direction || 1)
                            color: "#eef3f4"
                            font.pixelSize: 14
                            font.bold: true
                        }
                    }

                    Rectangle {
                        width: 84
                        height: 34
                        radius: 7
                        color: stepperReverseMouse.pressed ? "#3a1b1f" : "#2a2020"
                        border.color: root.accentRed
                        border.width: 1
                        opacity: stepperMotorPopupPanel.motorConfig.direction < 0 ? 1.0 : 0.78

                        Text {
                            anchors.centerIn: parent
                            text: "反向"
                            color: "#ffecef"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: stepperReverseMouse
                            anchors.fill: parent
                            preventStealing: true

                            onClicked: {
                                root.changeStepperMotorValue("direction", -1)
                            }
                        }
                    }

                    Rectangle {
                        width: 84
                        height: 34
                        radius: 7
                        color: stepperForwardMouse.pressed ? "#30413a" : "#1f332b"
                        border.color: root.accentGreen
                        border.width: 1
                        opacity: stepperMotorPopupPanel.motorConfig.direction >= 0 ? 1.0 : 0.78

                        Text {
                            anchors.centerIn: parent
                            text: "正向"
                            color: "#eafff2"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: stepperForwardMouse
                            anchors.fill: parent
                            preventStealing: true

                            onClicked: {
                                root.changeStepperMotorValue("direction", 1)
                            }
                        }
                    }
                }

                Row {
                    parent: stepperMotorSettingsContent
                    x: 14
                    y: 232
                    width: parent.width - 28
                    height: 34
                    spacing: 8
                    visible: (stepperMotorPopupPanel.motorConfig.role || "") === "camera_z"

                    Text {
                        width: 92
                        anchors.verticalCenter: parent.verticalCenter
                        text: "下探步数"
                        color: "#dce3e6"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    Rectangle {
                        width: 172
                        height: 34
                        radius: 7
                        color: "#20262a"
                        border.color: "#3b454b"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: Math.floor(Number(stepperMotorPopupPanel.motorConfig.zDownFixedSteps || 0)) + " step"
                            color: "#eef3f4"
                            font.pixelSize: 13
                            font.bold: true
                            elide: Text.ElideRight
                        }
                    }

                    Rectangle {
                        width: 172
                        height: 34
                        radius: 7
                        color: stepperZDownInputMouse.pressed ? "#3c3322" : "#33291b"
                        border.color: root.accentAmber
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "输入0~4294967295"
                            color: "#fff3d5"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: stepperZDownInputMouse
                            anchors.fill: parent
                            preventStealing: true

                            onClicked: {
                                root.openStepperStepEditor("zDownFixedSteps")
                            }
                        }
                    }
                }

                Row {
                    parent: stepperMotorSettingsContent
                    x: 14
                    y: 272
                    width: parent.width - 28
                    height: 34
                    spacing: 8
                    visible: (stepperMotorPopupPanel.motorConfig.role || "") === "camera_z"

                    Text {
                        width: 92
                        anchors.verticalCenter: parent.verticalCenter
                        text: "回升步数"
                        color: "#dce3e6"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    Rectangle {
                        width: 172
                        height: 34
                        radius: 7
                        color: "#20262a"
                        border.color: "#3b454b"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: Math.floor(Number(stepperMotorPopupPanel.motorConfig.zUpFixedSteps || 0)) + " step"
                            color: "#eef3f4"
                            font.pixelSize: 13
                            font.bold: true
                            elide: Text.ElideRight
                        }
                    }

                    Rectangle {
                        width: 172
                        height: 34
                        radius: 7
                        color: stepperZUpInputMouse.pressed ? "#3c3322" : "#33291b"
                        border.color: root.accentAmber
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "输入0~4294967295"
                            color: "#fff3d5"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: stepperZUpInputMouse
                            anchors.fill: parent
                            preventStealing: true

                            onClicked: {
                                root.openStepperStepEditor("zUpFixedSteps")
                            }
                        }
                    }
                }

                Row {
                    parent: stepperMotorSettingsContent
                    x: 14
                    y: 312
                    width: parent.width - 28
                    height: 34
                    spacing: 8
                    visible: (stepperMotorPopupPanel.motorConfig.role || "") === "camera_z"

                    Text {
                        width: 92
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Z轴超时"
                        color: "#dce3e6"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    Rectangle {
                        width: 172
                        height: 34
                        radius: 7
                        color: "#20262a"
                        border.color: "#3b454b"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: (Math.floor(Number(stepperMotorPopupPanel.motorConfig.zMotionTimeoutMs || 10000)) / 1000.0).toFixed(1) + " 秒"
                            color: "#eef3f4"
                            font.pixelSize: 13
                            font.bold: true
                            elide: Text.ElideRight
                        }
                    }

                    Rectangle {
                        width: 172
                        height: 34
                        radius: 7
                        color: stepperZTimeoutInputMouse.pressed ? "#3c3322" : "#33291b"
                        border.color: root.accentAmber
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "输入1~60秒"
                            color: "#fff3d5"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: stepperZTimeoutInputMouse
                            anchors.fill: parent
                            preventStealing: true

                            onClicked: {
                                root.openStepperStepEditor("zMotionTimeoutMs")
                            }
                        }
                    }
                }
            }

            Rectangle {
                x: 18
                y: 466
                width: parent.width - 36
                height: 46
                radius: 7
                color: "#141719"
                border.color: "#344149"
                border.width: 1

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.stepperMotorResultText
                    color: "#dce3e6"
                    font.pixelSize: 12
                    font.bold: true
                    wrapMode: Text.Wrap
                }
            }

            Rectangle {
                x: 18
                y: 522
                width: 132
                height: 34
                radius: 8
                color: stepperNextPageMouse.pressed ? "#26323a" : "#1a2024"
                border.color: "#5aa7ff"
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "下一台电机"
                    color: "#d9ecff"
                    font.pixelSize: 13
                    font.bold: true
                }

                MouseArea {
                    id: stepperNextPageMouse
                    anchors.fill: parent

                    onClicked: {
                        var motors = detectSettings.stepperMotorSettings
                        var count = motors && motors.length > 0 ? motors.length : 1
                        root.stepperMotorPageIndex = (root.stepperMotorPageIndex + 1) % count
                        root.stepperSpeedEditorVisible = false
                        root.stepperSpeedEditKey = "normalSpeedRpm"
                        root.stepperStepEditorVisible = false
                        root.stepperStepEditKey = ""
                        root.stepperSpeedInputText = "" + (root.currentStepperMotorSetting().normalSpeedRpm || 0)
                        root.stepperMotorResultText = root.currentStepperMotorSetting().name + " 参数页"
                    }
                }
            }

            Rectangle {
                x: 166
                y: 522
                width: 180
                height: 34
                radius: 8
                color: stepperHomeMouse.pressed ? "#3c3322" : "#33291b"
                border.color: root.accentAmber
                border.width: 1
                opacity: root.stepperHomeSending ? 0.55 : 1.0

                Text {
                    anchors.centerIn: parent
                    text: root.stepperHomeSending ? "设零中..." : "设当前位置为零点"
                    color: "#fff3d5"
                    font.pixelSize: 12
                    font.bold: true
                    elide: Text.ElideRight
                }

                MouseArea {
                    id: stepperHomeMouse
                    anchors.fill: parent
                    enabled: !root.stepperHomeSending && !root.stepperSettingsSending

                    onClicked: {
                        root.sendStepperActuatorHome()
                    }
                }
            }

            Rectangle {
                x: parent.width - 178
                y: 522
                width: 160
                height: 34
                radius: 8
                color: stepperSaveMouse.pressed ? "#30413a" : "#1f332b"
                border.color: root.accentGreen
                border.width: 1
                opacity: root.stepperSettingsSending ? 0.55 : 1.0

                Text {
                    anchors.centerIn: parent
                    text: root.stepperSettingsSending ? "下发中..." : "保存并下发"
                    color: "#eafff2"
                    font.pixelSize: 13
                    font.bold: true
                }

                MouseArea {
                    id: stepperSaveMouse
                    anchors.fill: parent
                    enabled: !root.stepperSettingsSending

                    onClicked: {
                        root.settingsApplyAction("save")
                    }
                }
            }

            Rectangle {
                id: stepperSpeedEditor
                anchors.fill: parent
                z: 30
                visible: root.stepperSpeedEditorVisible
                color: "#cc000000"

                MouseArea {
                    anchors.fill: parent

                    onClicked: {
                        root.stepperSpeedEditorVisible = false
                    }
                }

                Rectangle {
                    width: 424
                    height: 420
                    anchors.centerIn: parent
                    radius: 10
                    color: "#20262a"
                    border.color: root.accentAmber
                    border.width: 1
                    clip: true

                    MouseArea {
                        anchors.fill: parent
                    }

                    Text {
                        x: 16
                        y: 14
                        width: parent.width - 108
                        text: root.stepperSpeedEditKey === "scanSpeedRpm" ? "上料速度输入" : "常规/对中速度输入"
                        color: "#f1f4f5"
                        font.pixelSize: 18
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Rectangle {
                        x: parent.width - 80
                        y: 12
                        width: 64
                        height: 30
                        radius: 7
                        color: closeSpeedEditorMouse.pressed ? "#3a1b1f" : "#2a2020"
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
                            id: closeSpeedEditorMouse
                            anchors.fill: parent

                            onClicked: {
                                root.stepperSpeedEditorVisible = false
                            }
                        }
                    }

                    Text {
                        x: 16
                        y: 54
                        width: parent.width - 32
                        text: "范围 0~5000 rpm；0 表示保存为停止速度，自动流程会用兜底速度避免误停。"
                        color: "#cfd7db"
                        font.pixelSize: 12
                        font.bold: true
                        wrapMode: Text.Wrap
                    }

                    Rectangle {
                        x: 16
                        y: 88
                        width: parent.width - 32
                        height: 54
                        radius: 8
                        color: "#171b1e"
                        border.color: "#344149"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: root.stepperSpeedInputText + " rpm"
                            color: "#ffffff"
                            font.pixelSize: 24
                            font.bold: true
                        }
                    }

                    Grid {
                        id: stepperSpeedKeypadGrid
                        x: 16
                        y: 158
                        width: parent.width - 32
                        columns: 3
                        rowSpacing: 8
                        columnSpacing: 8

                        Repeater {
                            model: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "清空", "0", "退格"]

                            Rectangle {
                                width: (stepperSpeedKeypadGrid.width - 16) / 3
                                height: 42
                                radius: 7
                                color: stepperSpeedKeyMouse.pressed ? "#26323a" : "#1a2024"
                                border.color: modelData === "清空" || modelData === "退格" ? "#5aa7ff" : "#344149"
                                border.width: 1

                                Text {
                                    anchors.centerIn: parent
                                    text: modelData
                                    color: modelData === "清空" || modelData === "退格" ? "#d9ecff" : "#f1f4f5"
                                    font.pixelSize: 15
                                    font.bold: true
                                }

                                MouseArea {
                                    id: stepperSpeedKeyMouse
                                    anchors.fill: parent

                                    onClicked: {
                                        if (modelData === "清空") {
                                            root.clearStepperSpeedInput()
                                        } else if (modelData === "退格") {
                                            root.backspaceStepperSpeedDigit()
                                        } else {
                                            root.appendStepperSpeedDigit(modelData)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        x: 16
                        y: 370
                        width: 140
                        height: 36
                        radius: 8
                        color: resetSpeedMouse.pressed ? "#30363b" : "#22272b"
                        border.color: "#5aa7ff"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "重置为0"
                            color: "#d9ecff"
                            font.pixelSize: 13
                            font.bold: true
                        }

                        MouseArea {
                            id: resetSpeedMouse
                            anchors.fill: parent

                            onClicked: {
                                root.clearStepperSpeedInput()
                            }
                        }
                    }

                    Rectangle {
                        x: parent.width - 176
                        y: 370
                        width: 160
                        height: 36
                        radius: 8
                        color: applySpeedMouse.pressed ? "#30413a" : "#1f332b"
                        border.color: root.accentGreen
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "应用速度"
                            color: "#eafff2"
                            font.pixelSize: 13
                            font.bold: true
                        }

                        MouseArea {
                            id: applySpeedMouse
                            anchors.fill: parent

                            onClicked: {
                                root.applyStepperSpeedInput()
                            }
                        }
                    }
                }
            }

            Rectangle {
                id: stepperStepEditor
                anchors.fill: parent
                z: 31
                visible: root.stepperStepEditorVisible
                color: "#cc000000"

                MouseArea {
                    anchors.fill: parent

                    onClicked: {
                        root.stepperStepEditorVisible = false
                    }
                }

                Rectangle {
                    width: 470
                    height: 430
                    anchors.centerIn: parent
                    radius: 10
                    color: "#20262a"
                    border.color: root.accentAmber
                    border.width: 1
                    clip: true

                    MouseArea {
                        anchors.fill: parent
                    }

                    Text {
                        x: 16
                        y: 14
                        width: parent.width - 108
                        text: root.stepperStepEditorTitleText()
                        color: "#f1f4f5"
                        font.pixelSize: 18
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Rectangle {
                        x: parent.width - 80
                        y: 12
                        width: 64
                        height: 30
                        radius: 7
                        color: closeStepEditorMouse.pressed ? "#3a1b1f" : "#2a2020"
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
                            id: closeStepEditorMouse
                            anchors.fill: parent

                            onClicked: {
                                root.stepperStepEditorVisible = false
                            }
                        }
                    }

                    Text {
                        x: 16
                        y: 54
                        width: parent.width - 32
                        text: root.stepperStepEditorRangeText()
                        color: "#cfd7db"
                        font.pixelSize: 12
                        font.bold: true
                        wrapMode: Text.Wrap
                    }

                    Rectangle {
                        x: 16
                        y: 92
                        width: parent.width - 32
                        height: 54
                        radius: 8
                        color: "#171b1e"
                        border.color: "#344149"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: root.stepperStepInputText + " " + root.stepperStepEditorUnitText()
                            color: "#ffffff"
                            font.pixelSize: 22
                            font.bold: true
                            elide: Text.ElideRight
                        }
                    }

                    Grid {
                        id: stepperStepKeypadGrid
                        x: 16
                        y: 160
                        width: parent.width - 32
                        columns: 3
                        rowSpacing: 8
                        columnSpacing: 8

                        Repeater {
                            model: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "清空", "0", "退格"]

                            Rectangle {
                                width: (stepperStepKeypadGrid.width - 16) / 3
                                height: 42
                                radius: 7
                                color: stepperStepKeyMouse.pressed ? "#26323a" : "#1a2024"
                                border.color: modelData === "清空" || modelData === "退格" ? "#5aa7ff" : "#344149"
                                border.width: 1

                                Text {
                                    anchors.centerIn: parent
                                    text: modelData
                                    color: modelData === "清空" || modelData === "退格" ? "#d9ecff" : "#f1f4f5"
                                    font.pixelSize: 15
                                    font.bold: true
                                }

                                MouseArea {
                                    id: stepperStepKeyMouse
                                    anchors.fill: parent

                                    onClicked: {
                                        if (modelData === "清空") {
                                            root.clearStepperStepInput()
                                        } else if (modelData === "退格") {
                                            root.backspaceStepperStepDigit()
                                        } else {
                                            root.appendStepperStepDigit(modelData)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        x: 16
                        y: 380
                        width: 140
                        height: 36
                        radius: 8
                        color: resetStepMouse.pressed ? "#30363b" : "#22272b"
                        border.color: "#5aa7ff"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: root.stepperStepEditorIsTimeout() ? "重置为10秒" : "重置为0"
                            color: "#d9ecff"
                            font.pixelSize: 13
                            font.bold: true
                        }

                        MouseArea {
                            id: resetStepMouse
                            anchors.fill: parent

                            onClicked: {
                                root.clearStepperStepInput()
                            }
                        }
                    }

                    Rectangle {
                        x: parent.width - 176
                        y: 380
                        width: 160
                        height: 36
                        radius: 8
                        color: applyStepMouse.pressed ? "#30413a" : "#1f332b"
                        border.color: root.accentGreen
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: root.stepperStepEditorIsTimeout() ? "应用超时" : "应用步数"
                            color: "#eafff2"
                            font.pixelSize: 13
                            font.bold: true
                        }

                        MouseArea {
                            id: applyStepMouse
                            anchors.fill: parent

                            onClicked: {
                                root.applyStepperStepInput()
                            }
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        id: calibrationPopup
        anchors.fill: parent
        z: 895
        visible: root.calibrationPopupVisible
        color: "#b0000000"

        MouseArea {
            anchors.fill: parent

            onClicked: {
                if (!root.calibrationSending) {
                    root.calibrationPopupVisible = false
                }
            }
        }

        Rectangle {
            width: 572
            height: 560
            anchors.centerIn: parent
            radius: 10
            color: "#20262a"
            border.color: "#5aa7ff"
            border.width: 1
            clip: true

            MouseArea {
                anchors.fill: parent
            }

            Text {
                x: 18
                y: 14
                width: parent.width - 122
                text: "称重标定"
                color: "#f1f4f5"
                font.pixelSize: 20
                font.bold: true
                elide: Text.ElideRight
            }

            Rectangle {
                x: parent.width - 90
                y: 12
                width: 72
                height: 30
                radius: 7
                color: closeCalibrationMouse.pressed ? "#3a1b1f" : "#2a2020"
                border.color: root.accentRed
                border.width: 1
                opacity: root.calibrationSending ? 0.45 : 1.0

                Text {
                    anchors.centerIn: parent
                    text: "关闭"
                    color: "#ffecef"
                    font.pixelSize: 12
                    font.bold: true
                }

                MouseArea {
                    id: closeCalibrationMouse
                    anchors.fill: parent
                    enabled: !root.calibrationSending

                    onClicked: {
                        root.calibrationPopupVisible = false
                    }
                }
            }

            Text {
                x: 18
                y: 54
                width: parent.width - 36
                height: 42
                text: "F4状态：" + deviceHealth.f4StatusText + "；串口 /dev/ttySTM2 115200；MP157-F4主链路只发送二进制帧"
                color: "#cfd7db"
                font.pixelSize: 13
                font.bold: true
                wrapMode: Text.Wrap
            }

            Rectangle {
                x: 18
                y: 104
                width: parent.width - 36
                height: 58
                radius: 8
                color: "#171b1e"
                border.color: "#344149"
                border.width: 1

                Text {
                    x: 14
                    y: 9
                    text: "标定克重(g)"
                    color: "#9aa5ab"
                    font.pixelSize: 12
                    font.bold: true
                }

                TextInput {
                    id: calibrationWeightInput
                    x: 128
                    y: 8
                    width: 168
                    height: 42
                    text: root.calibrationWeightText
                    color: "#ffffff"
                    selectionColor: root.accentGreen
                    selectedTextColor: "#101214"
                    font.pixelSize: 24
                    font.bold: true
                    horizontalAlignment: TextInput.AlignHCenter
                    verticalAlignment: TextInput.AlignVCenter
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: IntValidator {
                        bottom: 1
                        top: 5000
                    }

                    onTextChanged: {
                        root.calibrationWeightText = text
                    }
                }

                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    width: 190
                    text: "范围 1~5000g，推荐先空载 TARE，再放砝码标定"
                    color: "#8f9aa1"
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }
            }

            Grid {
                id: calibrationQuickGrid
                x: 18
                y: 178
                width: parent.width - 36
                columns: 4
                rowSpacing: 8
                columnSpacing: 8

                Repeater {
                    model: [100, 500, 1000, 2000]

                    Rectangle {
                        width: (calibrationQuickGrid.width - 24) / 4
                        height: 36
                        radius: 7
                        color: quickWeightMouse.pressed ? "#30413a" : "#1f332b"
                        border.color: root.accentGreen
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: modelData + " g"
                            color: "#eafff2"
                            font.pixelSize: 13
                            font.bold: true
                        }

                        MouseArea {
                            id: quickWeightMouse
                            anchors.fill: parent

                            onClicked: {
                                root.selectCalibrationWeight(modelData)
                            }
                        }
                    }
                }
            }

            Rectangle {
                x: 18
                y: 426
                width: parent.width - 36
                height: 74
                radius: 7
                color: "#141719"
                border.color: root.calibrationResultText.indexOf("失败") >= 0
                              || root.calibrationResultText.indexOf("必须") >= 0
                              ? root.accentRed : "#344149"
                border.width: 1

                Flickable {
                    id: calibrationResultFlickable
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.top: parent.top
                    anchors.topMargin: 8
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 8
                    clip: true
                    contentWidth: width
                    contentHeight: calibrationResultTextItem.height
                    flickableDirection: Flickable.VerticalFlick
                    boundsBehavior: Flickable.DragOverBounds
                    maximumFlickVelocity: root.uiVerticalFlickVelocity
                    flickDeceleration: root.uiVerticalFlickDeceleration
                    interactive: contentHeight > height

                    Text {
                        id: calibrationResultTextItem
                        width: calibrationResultFlickable.width
                        text: root.calibrationResultText
                        color: root.calibrationResultText.indexOf("失败") >= 0
                               || root.calibrationResultText.indexOf("必须") >= 0
                               ? "#ffd6dc" : "#dce3e6"
                        font.pixelSize: 12
                        font.bold: true
                        wrapMode: Text.Wrap
                    }
                }
            }

            Grid {
                id: calibrationKeypadGrid
                x: 18
                y: 226
                width: parent.width - 36
                columns: 3
                rowSpacing: 8
                columnSpacing: 8

                Repeater {
                    model: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "清空", "0", "退格"]

                    Rectangle {
                        width: (calibrationKeypadGrid.width - 16) / 3
                        height: 44
                        radius: 7
                        color: keypadMouse.pressed ? "#26323a" : "#1a2024"
                        border.color: modelData === "清空" || modelData === "退格" ? "#5aa7ff" : "#344149"
                        border.width: 1
                        opacity: root.calibrationSending ? 0.45 : 1.0

                        Text {
                            anchors.centerIn: parent
                            text: modelData
                            color: modelData === "清空" || modelData === "退格" ? "#d9ecff" : "#f1f4f5"
                            font.pixelSize: 15
                            font.bold: true
                        }

                        MouseArea {
                            id: keypadMouse
                            anchors.fill: parent
                            enabled: !root.calibrationSending

                            onClicked: {
                                if (modelData === "清空") {
                                    root.clearCalibrationWeight()
                                } else if (modelData === "退格") {
                                    root.backspaceCalibrationDigit()
                                } else {
                                    root.appendCalibrationDigit(modelData)
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                x: 18
                y: 508
                width: 168
                height: 40
                radius: 8
                color: refreshF4ForCalMouse.pressed ? "#3c3322" : "#33291b"
                border.color: root.accentAmber
                border.width: 1
                opacity: root.calibrationSending ? 0.45 : 1.0

                Text {
                    anchors.centerIn: parent
                    text: "刷新F4状态"
                    color: "#fff3d5"
                    font.pixelSize: 13
                    font.bold: true
                }

                MouseArea {
                    id: refreshF4ForCalMouse
                    anchors.fill: parent
                    enabled: !root.calibrationSending

                    onClicked: {
                        deviceHealth.refreshF4StatusNow()
                        root.calibrationResultText = "已发送 STATUS，等待F4状态刷新"
                    }
                }
            }

            Rectangle {
                x: parent.width - 198
                y: 508
                width: 180
                height: 40
                radius: 8
                color: sendCalibrationMouse.pressed ? "#30413a" : "#1f332b"
                border.color: root.accentGreen
                border.width: 1
                opacity: root.calibrationSending ? 0.55 : 1.0

                Text {
                    anchors.centerIn: parent
                    text: root.calibrationSending ? "发送中..." : "发二进制"
                    color: "#eafff2"
                    font.pixelSize: 14
                    font.bold: true
                }

                MouseArea {
                    id: sendCalibrationMouse
                    anchors.fill: parent
                    enabled: !root.calibrationSending

                    onClicked: {
                        root.sendCalibrationCommand()
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
                        {"name": "定位", "value": deviceHealth.locationStatusText, "color": deviceHealth.locationStatusColor},
                        {"name": "当前告警", "value": root.alarmCleared ? "无未处理" : root.alarmCurrentLevel, "color": root.alarmCleared ? root.accentGreen : root.alarmLevelColor()},
                        {"name": "SD卡", "value": deviceHealth.sdcardStatusText, "color": deviceHealth.sdcardStatusColor},
                        {"name": "云端", "value": deviceHealth.cloudStatusText, "color": deviceHealth.cloudStatusColor},
                        {"name": "KMS视频", "value": root.usingKmsOverlay ? deviceHealth.cameraStatusText : "预览/桥接", "color": root.usingKmsOverlay ? deviceHealth.cameraStatusColor : root.accentAmber},
                        {"name": "4G", "value": deviceHealth.networkStatusText, "color": deviceHealth.networkStatusColor}
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
                boundsBehavior: Flickable.DragOverBounds
                cacheBuffer: height * root.uiListCachePages
                maximumFlickVelocity: root.uiVerticalFlickVelocity
                flickDeceleration: root.uiVerticalFlickDeceleration

                delegate: Rectangle {
                    width: alarmHistoryListView.width
                    height: 28
                    radius: 6
                    color: "#20262a"
                    border.color: root.alarmLevelColorFor(level)
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
                            color: root.alarmLevelColorFor(level)
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
                        {"step": "1", "text": root.alarmSourceAdvice("camera-kms-no-frame")[0]},
                        {"step": "2", "text": root.alarmSourceAdvice("sdcard-not-writable")[0]},
                        {"step": "3", "text": root.alarmSourceAdvice("cloud-upload-failed")[0]},
                        {"step": "4", "text": root.alarmSourceAdvice("f4-heartbeat-lost")[0]}
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
                width: parent.width - 128
                text: "确认不等于解除联锁，最终以 F4 状态为准。"
                color: "#8f9aa1"
                font.pixelSize: 11
                elide: Text.ElideRight
            }

            Rectangle {
                width: 96
                height: 28
                radius: 7
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 10
                color: alarmAdviceDetailMouse.pressed ? "#30413a" : "#1f332b"
                border.color: root.accentGreen
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "查看全部"
                    color: "#eafff2"
                    font.pixelSize: 12
                    font.bold: true
                }

                MouseArea {
                    id: alarmAdviceDetailMouse
                    anchors.fill: parent

                    onClicked: {
                        root.alarmAdviceDetailVisible = true
                    }
                }
            }
        }
    }

    Rectangle {
        id: alarmAdviceDetailOverlay
        anchors.fill: parent
        z: 900
        visible: root.alarmAdviceDetailVisible
        color: "#b0000000"

        MouseArea {
            anchors.fill: parent

            onClicked: {
                root.alarmAdviceDetailVisible = false
            }
        }

        Rectangle {
            width: 640
            height: 438
            anchors.centerIn: parent
            radius: 10
            color: "#20262a"
            border.color: root.accentAmber
            border.width: 1
            clip: true

            MouseArea {
                anchors.fill: parent
            }

            Text {
                x: 18
                y: 14
                text: "处理建议完整说明"
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
                color: closeAlarmAdviceDetailMouse.pressed ? "#3a1b1f" : "#2a2020"
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
                    id: closeAlarmAdviceDetailMouse
                    anchors.fill: parent

                    onClicked: {
                        root.alarmAdviceDetailVisible = false
                    }
                }
            }

            Flickable {
                id: alarmAdviceDetailFlickable
                x: 18
                y: 56
                width: parent.width - 36
                height: parent.height - 74
                contentWidth: width
                contentHeight: fullAlarmAdviceText.height
                clip: true
                boundsBehavior: Flickable.DragOverBounds
                maximumFlickVelocity: root.uiVerticalFlickVelocity
                flickDeceleration: root.uiVerticalFlickDeceleration
                interactive: contentHeight > height

                Text {
                    id: fullAlarmAdviceText
                    width: alarmAdviceDetailFlickable.width
                    text: root.alarmFullAdviceText()
                    color: "#d7dee2"
                    font.pixelSize: 15
                    lineHeightMode: Text.ProportionalHeight
                    lineHeight: 1.26
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    /* logPage 是日志查看界面：读取 /mnt/sdcard/logs 下的 .log/.txt 文件，点击文件名后弹窗查看全文。 */
    Rectangle {
        id: logPage
        x: 176
        y: 72
        width: root.width - 192
        height: root.height - 88
        radius: 8
        color: "#171a1d"
        border.color: root.borderColor
        border.width: 1
        visible: root.logPageVisible
        clip: true

        /* logHeader 显示页面标题、扫描状态、日志数量、刷新入口和返回首页入口。 */
        Rectangle {
            id: logHeader
            x: 16
            y: 12
            width: parent.width - 32
            height: 38
            color: "transparent"

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "日志查看"
                color: "#f1f4f5"
                font.pixelSize: 20
                font.bold: true
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 100
                anchors.right: logHeaderActions.left
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: logFileModel.statusText
                color: logFileModel.count > 0 ? "#aeb9bf" : root.accentAmber
                font.pixelSize: 13
                elide: Text.ElideRight
            }

            Row {
                id: logHeaderActions
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                Rectangle {
                    width: 96
                    height: 28
                    radius: 6
                    color: "#20332a"
                    border.color: root.accentGreen
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "日志 " + logFileModel.count
                        color: "#d9ffe8"
                        font.pixelSize: 13
                        font.bold: true
                    }
                }

                Rectangle {
                    width: 82
                    height: 28
                    radius: 6
                    color: refreshLogMouse.pressed ? "#30413a" : "#1f332b"
                    border.color: root.accentGreen
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "刷新"
                        color: "#eafff2"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    MouseArea {
                        id: refreshLogMouse
                        anchors.fill: parent

                        onClicked: {
                            root.refreshLogFileList()
                        }
                    }
                }

                Rectangle {
                    width: 86
                    height: 28
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
            id: logListPanel
            x: 16
            y: 58
            width: parent.width - 32
            height: parent.height - 74
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1
            clip: true

            Text {
                x: 18
                y: 14
                width: parent.width - 36
                text: "点击日志文件名查看完整内容，长日志可在弹窗内上下滑动"
                color: "#aeb9bf"
                font.pixelSize: 14
                font.bold: true
                elide: Text.ElideRight
            }

            ListView {
                id: logListView
                x: 18
                y: 48
                width: parent.width - 36
                height: parent.height - 66
                model: logFileModel
                spacing: 10
                clip: true
                boundsBehavior: Flickable.DragOverBounds
                interactive: logFileModel.count > 0
                cacheBuffer: height * root.uiListCachePages
                flickDeceleration: root.uiVerticalFlickDeceleration
                maximumFlickVelocity: root.uiVerticalFlickVelocity

                delegate: Rectangle {
                    width: logListView.width
                    height: 78
                    radius: 8
                    color: root.selectedLogIndex === index ? "#20362f" : "#20262a"
                    border.color: root.selectedLogIndex === index ? root.accentGreen : "#343c42"
                    border.width: 1
                    clip: true

                    Rectangle {
                        x: 14
                        y: 14
                        width: 82
                        height: 50
                        radius: 7
                        color: suffix === "log" ? "#3a1f24" : "#1d2d3d"
                        border.color: suffix === "log" ? root.accentRed : "#5aa7ff"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: typeText
                            color: "#ffffff"
                            font.pixelSize: 13
                            font.bold: true
                        }
                    }

                    Text {
                        x: 112
                        y: 12
                        width: parent.width - 250
                        text: fileName
                        color: "#f4f7f8"
                        font.pixelSize: 17
                        font.bold: true
                        elide: Text.ElideMiddle
                    }

                    Text {
                        x: 112
                        y: 40
                        width: parent.width - 250
                        text: filePath
                        color: "#8f9aa1"
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                    }

                    Column {
                        x: parent.width - 126
                        y: 13
                        width: 104
                        spacing: 6

                        Text {
                            width: parent.width
                            text: sizeText
                            color: "#dce3e6"
                            font.pixelSize: 13
                            font.bold: true
                            horizontalAlignment: Text.AlignRight
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width
                            text: modifiedText
                            color: "#aeb9bf"
                            font.pixelSize: 11
                            horizontalAlignment: Text.AlignRight
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                        }
                    }

                    MouseArea {
                        anchors.fill: parent

                        onClicked: {
                            root.openLogDetail(index)
                        }
                    }
                }

                Rectangle {
                    anchors.fill: parent
                    radius: 8
                    color: "#141719"
                    border.color: "#30363b"
                    border.width: 1
                    visible: logFileModel.count <= 0

                    Column {
                        anchors.centerIn: parent
                        width: parent.width - 80
                        spacing: 10

                        Text {
                            width: parent.width
                            text: "暂无可查看日志"
                            color: "#dce3e6"
                            font.pixelSize: 22
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                        }

                        Text {
                            width: parent.width
                            text: logFileModel.statusText
                            color: "#909aa0"
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
        }
    }

    Connections {
        target: detectSettings

        /*
         * onSettingsChanged 的作用：
         *   C++ 参数控制器清洗或恢复默认后，QML 的摘要属性会自动重新绑定；
         *   这里同步最近操作提示，保证底部状态栏能显示真实配置变化。
         */
        onSettingsChanged: {
            root.stepperMotorSettingsRevision += 1
            root.settingsLastActionText = detectSettings.lastStatusText
            root.storageState = root.settingsLastActionText
        }

        /*
         * onLastStatusTextChanged 的作用：
         *   保存 JSON、读取 JSON 或恢复默认后刷新参数页提示文本。
         */
        onLastStatusTextChanged: {
            root.settingsLastActionText = detectSettings.lastStatusText
        }
    }

    Rectangle {
        id: logDetailOverlay
        anchors.fill: parent
        z: 910
        visible: root.logDetailVisible
        color: "#b0000000"

        MouseArea {
            anchors.fill: parent

            onClicked: {
                root.logDetailVisible = false
            }
        }

        Rectangle {
            width: 720
            height: 486
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
                width: parent.width - 126
                text: selectedLogEntry.fileName && selectedLogEntry.fileName.length > 0
                      ? selectedLogEntry.fileName
                      : "日志详情"
                color: "#f1f4f5"
                font.pixelSize: 18
                font.bold: true
                elide: Text.ElideMiddle
            }

            Text {
                x: 18
                y: 40
                width: parent.width - 126
                text: (selectedLogEntry.typeText && selectedLogEntry.typeText.length > 0
                       ? selectedLogEntry.typeText : "日志文件")
                      + " · "
                      + (selectedLogEntry.sizeText && selectedLogEntry.sizeText.length > 0
                         ? selectedLogEntry.sizeText : "--")
                      + " · "
                      + (selectedLogEntry.modifiedText && selectedLogEntry.modifiedText.length > 0
                         ? selectedLogEntry.modifiedText : "--")
                color: "#aeb9bf"
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            Rectangle {
                x: parent.width - 90
                y: 14
                width: 72
                height: 32
                radius: 7
                color: closeLogDetailMouse.pressed ? "#3a1b1f" : "#2a2020"
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
                    id: closeLogDetailMouse
                    anchors.fill: parent

                    onClicked: {
                        root.logDetailVisible = false
                    }
                }
            }

            Rectangle {
                x: 18
                y: 66
                width: parent.width - 36
                height: parent.height - 84
                radius: 8
                color: "#141719"
                border.color: "#30363b"
                border.width: 1
                clip: true

                Flickable {
                    id: logDetailFlickable
                    anchors.fill: parent
                    anchors.margins: 14
                    contentWidth: width
                    contentHeight: logDetailTextItem.height
                    clip: true
                    boundsBehavior: Flickable.DragOverBounds
                    maximumFlickVelocity: root.uiVerticalFlickVelocity
                    flickDeceleration: root.uiVerticalFlickDeceleration
                    interactive: contentHeight > height

                    Text {
                        id: logDetailTextItem
                        width: logDetailFlickable.width
                        text: root.selectedLogContent
                        color: "#d7dee2"
                        font.family: "monospace"
                        font.pixelSize: 13
                        lineHeightMode: Text.ProportionalHeight
                        lineHeight: 1.22
                        wrapMode: Text.Wrap
                    }
                }
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
                 && !root.manualPageVisible && !root.settingsPageVisible
                 && !root.alarmPageVisible && !root.logPageVisible

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
                    property bool actionEnabled: !root.autoControlBusy && !root.autoVisionCommandBusy
                                                 && ((modelData.action === "start" && !root.autoWorkflowRunning && !root.autoWorkflowPaused)
                                                     || (modelData.action === "pause" && root.autoWorkflowRunning && !root.autoWorkflowPaused)
                                                     || (modelData.action === "resume" && root.autoWorkflowPaused)
                                                     || (modelData.action === "stop" && (root.autoWorkflowRunning || root.autoWorkflowPaused)))
                    color: !actionEnabled ? "#171b1e" : (mouseArea.pressed ? "#2d3338" : "#22272b")
                    border.color: actionEnabled ? modelData.color : "#3a4248"
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: modelData.text
                        color: parent.actionEnabled ? "#ffffff" : "#6d777d"
                        font.pixelSize: 16
                        font.bold: true
                    }

                    MouseArea {
                        id: mouseArea
                        anchors.fill: parent

                        onClicked: {
                            if (!parent.actionEnabled) {
                                return
                            }
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

    /* globalStorageToastLayer 是全局操作提示层，放在所有页面之后渲染，避免被历史、统计、设置等页面底部控件盖住。 */
    Item {
        id: globalStorageToastLayer
        anchors.fill: parent
        z: 900
        visible: true

        /* storageToast 是底部横向提示条，用于显示检测、上传、安全卸载和参数保存的最近状态。 */
        Rectangle {
            id: storageToast
            anchors.left: parent.left
            anchors.leftMargin: 188
            anchors.right: parent.right
            anchors.rightMargin: 28
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 14
            height: 32
            radius: 6
            color: isUploadStatusFailure(storageState) || storageState.indexOf("失败") >= 0 ? "#3a1b1f" : "#16291f"
            border.color: isUploadStatusFailure(storageState) || storageState.indexOf("失败") >= 0 ? root.accentRed : root.accentGreen
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
                color: isUploadStatusFailure(storageState) || storageState.indexOf("失败") >= 0 ? "#ffd6dc" : "#d9ffe8"
                font.pixelSize: 14
                font.bold: true
                elide: Text.ElideMiddle
                verticalAlignment: Text.AlignVCenter
            }
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
