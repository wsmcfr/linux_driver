# STM32MP157-F407 自动检测联调路线与缺口清单

| 项目 | 内容 |
|---|---|
| 文档目的 | 把当前自动检测总目标、已完成内容、MP157/F4/ESP32S3/云端还缺什么写清楚，后续按表逐项联调。 |
| 适用链路 | STM32MP157 视觉主控 `<->` STM32F407 执行主控 `<->` ESP32S3 机械臂控制器 `<->` 云端。 |
| 现有主协议 | `docs/stm32mp157-f407-binary-protocol.md`。 |
| 机械臂协议 | `docs/f4_esp32s3_arm_protocol/README.md`。 |
| 云端字段依据 | `D:\yunfuwu\docs\stm32mp157-cloud-upload-data-contract.md`。 |
| 更新时间 | `2026-07-25`。 |

## 1. 总目标

最终要实现的是“首页按一次开始后连续全自动检测”：

| 阶段 | 谁负责 | 目标动作 | 成功标志 |
|---|---|---|---|
| 上料扫描 | F4 | 启动传送带张大头 Emm42，让零件进入摄像头视野。 | MP157 识别到零件并开始发送视觉坐标。 |
| 视觉对中 | MP157 + F4 | MP157 周期发送零件坐标，F4 先控制传送带完成上料和前后/Y 方向对中；Z 轴下降复查后，传送带继续负责前后短步微调，摄像头左右轴负责 X 方向短步微调。 | 零件进入中心 ROI，MP157 发送 `BELT_STOP_CENTERED`。 |
| Z 轴下探、复查和对焦 | MP157 + F4 | F4 停止传送带后，MP157 让摄像头上下轴下降固定步数；收到下降 ACK 后继续等待 F4 `EVENT_REPORT event=0x14 actuator-move-done`；真实到位后重新读取 ROI，按 Y/X 偏差分别微调传送带和摄像头左右轴，确认居中后等待约 3 秒稳定对焦。 | 零件仍在 ROI 中央、Z 轴真实到位且对焦等待完成。 |
| 补光准备 | MP157 + F4 | MP157 下发 `FILL_LIGHT_CONTROL action=1`；F4 用 PB6/TIM4_CH1 输出 50 Hz PWM 2 秒，让 270 度舵机到开灯位置并停止 PWM；MP157 收到 `EVENT_REPORT event=0x16` 后非阻塞等待 5 秒。 | 开灯完成事件的 cycle、related_seq、action 和 270 度结果均匹配，5 秒计时到期后才启动模型。 |
| 模型检测与关灯 | MP157 + F4 | MP157 保存本次原图并运行 MobileNetV3-Small 和 UNet；成功或失败都下发 `FILL_LIGHT_CONTROL action=0`，F4 让舵机回 0 度、停止 PWM并回 `event=0x16`。 | MP157 生成 good/bad/review，并在关灯完成后才回升 Z 轴和进入机械臂阶段。 |
| 机械臂到称重 | F4 + ESP32S3 | F4 通知 ESP32S3 抓取 ROI 中零件并放到称重模块。 | ESP32S3 返回 `ARM_STAGE_DONE stage=PICK_BELT_TO_WEIGHT result=OK`。 |
| 称重 | F4 | F4 读取 HX711，稳定后把重量结果发给 MP157。 | MP157 收到 `WEIGHT_RESULT` 并 ACK。 |
| 机械臂到电感 | F4 + ESP32S3 | F4 通知 ESP32S3 从称重模块夹起零件并放到电磁感应模块。 | ESP32S3 返回 `ARM_STAGE_DONE stage=WEIGHT_TO_LDC result=OK`。 |
| 电感检测 | F4 | F4 读取 LDC1614，生成电磁感应结果并发给 MP157。 | MP157 收到 `LDC_RESULT` 并 ACK。 |
| 上传、分拣和完成 | MP157 + F4 + ESP32S3 | MP157 先把图片、模型、重量、电感、F4/ESP32 流程上下文写入本地历史并一次性上传云端；上传成功后按云端结果下发最终分拣，上传失败则保留历史可重传并强制待复核盘。 | F4 收到 `FINAL_SORT_RESULT` 后控制 ESP32S3 放入目标盘，最终发送 `CYCLE_DONE`；MP157 ACK 后本轮完成。 |

核心原则：

| 原则 | 说明 |
|---|---|
| MP157 不直接控电机和传感器 | MP157 只发业务指令和视觉坐标，实际运动、称重、电感读取由 F4 完成。 |
| F4 不做视觉模型和云端上传 | F4 只回传结构化结果帧，云端 JSON 由 MP157 组装。 |
| F4 不重新判断零件好坏 | F4 只缓存 MP157 的 `MODEL_READY.model_result`；真正最终分拣以 MP157 上传结束后下发的 `FINAL_SORT_RESULT` 为准。上传失败时必须进入待复核盘。 |
| ESP32S3 只做机械臂动作 | ESP32S3 不判断好坏，不读重量和电感，只告诉 F4 动作是否已经完成。 |
| 所有主链路都走二进制帧 | MP157-F4 与 F4-ESP32S3 都使用 `A5 5A ... CRC16 ... 6B`，不再依赖 `[OK]`、`[ERROR]` 文本。 |

## 2. 当前已经完成

| 模块 | 已完成内容 | 证据/现状 |
|---|---|---|
| MP157-F4 帧格式 | 已定义帧头 `A5 5A`、版本、命令、长度、SEQ、payload、CRC16-CCITT-FALSE、帧尾 `6B`。 | `docs/stm32mp157-f407-binary-protocol.md`。 |
| MP157 首页四按钮 | 首页 `开始/暂停/继续/停止` 已调用 C++ 发送二进制 `START_CYCLE/PAUSE_CYCLE/RESUME_CYCLE/STOP_CYCLE`。 | `20_uvc_camera/qt_camera_display/main.cpp`、`qml/Main.qml`。 |
| MP157 overlay 内存定位 | `uvc_kms_overlay` 已新增 `LOCATE` 命令，直接从原始 YUYV 帧中间搜索带和黑色传送带纵向区域计算零件中心、bbox、置信度、ring 结构证据和 `diag/cand_*` 拒绝诊断，不保存图片、不调用模型。 | `20_uvc_camera/qt_camera_display/uvc_kms_overlay.c`；板端可用首页底部状态观察，若工具支持 Unix socket 也可用 `printf 'LOCATE\n' \| nc -U /tmp/uvc-kms-overlay-control.sock` 验证。 |
| MP157 视觉坐标闭环首版 | 首页 `开始` 收到 F4 ACK 后启动 `autoVisionTimer`，每 100ms 请求 `LOCATE`；按“上方来料”使用 `center_y` 对齐 `height/2`，下发 `VISION_POS`；连续 3 帧进入 ±24px 后发送 `BELT_STOP_CENTERED`；本轮已见过目标后短暂漏检不再发送 `VISION_LOST reason=1`，避免 F4 重新扫描把零件送走。注意：这只是闭环流程已落地，不代表视觉识别已经稳定；当前现场问题就是零件在上料、跟踪或 ROI 内任意阶段丢失后重捕获仍需继续验证。 | `20_uvc_camera/qt_camera_display/main.cpp`、`20_uvc_camera/qt_camera_display/qml/Main.qml`；本地静态测试覆盖 `requestAutoVisionLocate/sendF4VisionPosition/autoVisionTimer/autoVisionHasSeenTarget`、默认关闭首次 no-ring 建链、已见目标后 no-ring 保持/重捕获和 `diag/cand_*` 显示等 marker。 |
| MP157 Z 轴同步下探检测 | 收到 `BELT_STOP_CENTERED` ACK 后，QML 下发 `ACTUATOR_POS_MOVE actuator=ACT_CAMERA_Z direction=DOWN`，用参数页 `zDownFixedSteps` 下降固定步数；C++ 先等 ACK，再继续等同一 `related_seq` 的 `EVENT_REPORT actuator-move-done`；QML 的 `z-motion-down-wait/z-motion-up-wait` 只作为本地超时保护，正常推进由 `autoVisionHandleActuatorMoveDone()` 触发。Z 下降 DONE 后复查 ROI，必要时用传送带做前后/Y 短步微调，用 `ACT_CAMERA_LATERAL` 左右轴做 X 短步微调；确认 ROI 在检测中心后才进入 `focus-settle` 等待约 3 秒，再运行模型检测；模型检测完成后再用 `zUpFixedSteps` 自动回升，并在回升 DONE 后才通知 F4/ESP32S3 机械臂。 | `20_uvc_camera/qt_camera_display/main.cpp`、`qml/Main.qml`；静态测试覆盖 `runF4ActuatorPositionMoveAndWaitDone`、`BINARY_PROTOCOL_EVENT_ACTUATOR_MOVE_DONE`、`autoVisionHandleActuatorMoveDone`、`z-motion-down-wait`、`z-motion-up-wait`、`autoVisionFineTuneConveyor`、`autoVisionFineTuneLateral`、`focus-settle` 等 marker。 |
| MP157 模型前补光状态机 | 对焦完成后先下发 `FILL_LIGHT_CONTROL 0x23 action=1`；C++ 严格等待 ACK 和 `EVENT_REPORT 0x16`，QML 再启动独立 5000 ms Timer。模型检测成功、失败和停止路径统一先发 action=0；关灯完成前保留 Z 轴待回升状态并禁止机械臂继续。 | `main.cpp`、`qml/Main.qml`、`test_qt_kms_overlay_assets.sh`；Windows 静态契约已通过，尚未交叉编译和板端联调。 |
| MP157 完整自动检测闭环代码 | 模型检测后先写本地历史 `upload_status=LOCAL_READY`，Z 轴回升后下发 `MODEL_READY 0x32` 和 `ARM_JOB_START 0x31`；收到 `WEIGHT_RESULT/LDC_RESULT` 后把 `weight_context_json/ldc_context_json/f4_flow_context_json/decision_context_json/vision_context_json` 写回同一条历史，再一次性上传；上传成功或失败都会下发 `FINAL_SORT_RESULT 0x33`，失败强制 `final_bin=3`。 | `20_uvc_camera/qt_camera_display/main.cpp`、`qml/Main.qml`、`defect-cos-upload`、`test_qt_kms_overlay_assets.sh`；本地静态测试已通过，板端部署和整机动作还需现场验证。 |
| MP157 UNet 平衡判定和板端参数 | Windows 源码已把旧单像素阈值升级为 8 邻域连通域三级证据；默认 `20/80/300/120 px`，参数页 `视觉检测策略 -> UNet参数` 可调整并保存 schema 2 JSON。分类 `GOOD + WEAK` 进入待复核，`STRONG` 才能由 UNet 独立判坏，F4 协议仍只接收最终 `good/bad/review`。 | `defect_segment_evidence.h`、`defect_segment.cpp`、`main.cpp`、`qml/Main.qml`、`test_defect_segment_evidence.cpp`；主机算法测试、静态契约以及 `defect-segment`/`qt_camera_display` ARM 交叉编译均已通过，尚待板端部署和真实反光样本标定。 |
| MP157 底部提示 | F4 返回内容已在底部提示前统一加 `F4:`。 | `Main.qml::f4ToastText()`。 |
| MP157 手动传送带调试 | 手动页可发送 `BELT_MANUAL_CONTROL` 和 `QUERY_STATUS`。 | F4 ACK、STATUS_REPORT 能被 Qt 解析。 |
| MP157 三轴手动控制 | 手动页已改成三轴弹窗，三页分别控制传送带、摄像头左右轴、摄像头上下轴；传送带/左右轴按一次方向键下发 `ACTUATOR_VEL_MOVE` 并持续运动到停止，上下轴按一次下降/上升只执行 `zDownFixedSteps/zUpFixedSteps` 固定步数；模拟急停下发 `ACTUATOR_STOP actuator=0xFF`，安全状态区域改为可滑动查看。 | `20_uvc_camera/qt_camera_display/qml/Main.qml`。 |
| F4 二进制解析 | F4 已能解析 `HELLO/HEARTBEAT/START/PAUSE/RESUME/STOP/VISION_POS/VISION_LOST/BELT_STOP_CENTERED/WEIGHT_CALIBRATE/MODEL_READY/ARM_JOB_START/FINAL_SORT_RESULT/QUERY_STATUS/BELT_MANUAL_CONTROL/ACTUATOR_POS_MOVE/ACTUATOR_STOP/ACTUATOR_VEL_MOVE/ACTUATOR_HOME`，并提供 `WEIGHT_RESULT/LDC_RESULT/CYCLE_DONE` 主动上报函数；执行器成功 ACK 统一 `status=0`。 | `E:\hal\bisai_f407_project\User\App\binary_protocol_service.c`；Keil 命令行构建此前已验证 `0 Error(s), 0 Warning(s)`，但烧录由用户执行。 |
| F4 补光舵机服务 | 已新增 `FILL_LIGHT_CONTROL 0x23` 和 `FILL_LIGHT_MOVE_DONE 0x16`；PB6/AF2/TIM4_CH1 输出 50 Hz，默认把 500~2500 us 映射到 0~270 度，每次保持 2 秒后停止 PWM。 | `fill_light_service.c/.h`、`freertos.c`、Keil 工程文件和主机测试；代码已改但尚未由用户 Keil 编译、下载和示波器验证。 |
| F4 传送带绑定 | 传送带服务已按当前方案使用 `UART4 PC10/PC11`，Emm42 地址 `0x01`。 | F4 启动日志显示 `UART4=PC10/PC11, addr=1`。 |
| F4 摄像头电机 ID 规划 | 当前现场摄像头左右轴地址 `0x03`，摄像头上下轴地址 `0x02`，共用 `USART6 PC6/PC7`。 | F4 启动日志或 `CAMINFO` 显示 `lateral_addr=3, z_addr=2`。 |
| F4 执行器位置、速度和设零模式 | `binary_protocol_service` 已分发 `ACTUATOR_POS_MOVE/ACTUATOR_STOP/ACTUATOR_VEL_MOVE/ACTUATOR_HOME`；`conveyor_motor_service` 和 `camera_motor_service` 分别负责速度持续运动、相对位置移动、停止和当前位置设零；`emm42_motor` 已接入 Emm42 速度模式、`0xFD` 相对位置模式、到位回包 `[addr FD 9F 6B]` 非阻塞解析和 `[addr 0A 6D 6B]` 当前位置清零命令。`ACTUATOR_POS_MOVE` 成功 ACK 后，F4 还会在真实到位时上报 `EVENT_REPORT event=0x14`，超时上报 `event=0x15`。 | `E:\hal\bisai_f407_project\User\App\binary_protocol_service.c`、`camera_motor_service.c`、`conveyor_motor_service.c`、`E:\hal\bisai_f407_project\User\Driver\emm42_motor.c`；Keil 命令行构建已验证 `0 Error(s), 0 Warning(s)`。 |
| F4 状态查询 | `QUERY_STATUS` 成功返回 `STATUS_REPORT`，包含 F4 状态、传送带模式、方向、速度、误差、故障位。 | 当前串口助手已看到 `belt_desired=SCAN`、`speed_rpm=300` 这类状态。 |
| 云端图片上传 | MP157 已能上传 source 原图和多张 annotated 结果图，并创建云端记录。 | `defect-cos-upload` 支持 `--jpg` 和多次 `--annotated`。 |

## 3. 当前没有完成的关键缺口

### 3.1 MP157 还缺什么

| 缺口 | 当前现状 | 需要补的内容 |
|---|---|---|
| 自动视觉稳定识别与重捕获 | Windows 源码已落地：`LOCATE` -> `VISION_POS`，上方来料使用 `center_y` 和 `height/2`；自动流程速度已改为读取参数页，传送带 SCAN 使用 `scan_speed_rpm`，零件入画后的 TRACK/短步微调使用 `normal_speed_rpm`，不再写死 40rpm；当前策略是首次建链默认必须依赖 `ring=1`，避免黑色传送带反光误触发；本轮已经见过目标后，`ring=0` 高置信候选可用于保持和重捕获。 | 还需要在板端先空传送带验证不会误进跟踪，再用真实三种零件分别测试“入画前段、接近 ROI、ROI 内静止”三种位置，确认丢失后能重新建立目标，并看底部 `diag/cand/cconf/cring/cdens` 判断是否还卡在阈值、搜索带、bbox、density 或 confidence。现场调速优先改 MP157 参数页，而不是重新改 F4 宏。 |
| ROI 居中停止 | Windows 源码已落地：连续 3 帧 `abs(center_y-height/2)<=24px` 后发送 `BELT_STOP_CENTERED hold_ms=2000`，与当前 F4 死区保持同量级。 | 还需要实测传送带惯性、F4 停机响应和相机画面稳定时间；必要时调整死区、稳定帧数或 F4 减速曲线。 |
| Z 轴下降后 ROI 复查 | Windows 源码已落地：F4 居中停机 ACK 后 QML 先让 Z 轴下降；C++ 保持串口连接，先等 ACK，再等同一 `related_seq` 的 `EVENT_REPORT actuator-move-done`；DONE 后才复查 ROI，必要时用传送带做前后/Y 微调、用左右轴做 X 微调；ROI 复查通过后才等待 3 秒聚焦并启动模型检测。 | 还需要同步到板端并确认张大头 Response 设置为 `Reached/Both`、Z 轴下降固定值、传送带前后微调方向、左右轴微调方向、ROI 死区和模型检测触发不会互相抢串口或摄像头资源。 |
| 模型前补光 | Windows MP157 源码已接入严格 ACK/完成事件和 5 秒状态机；暂停/停止会取消计时并在 STOP_CYCLE 清 cycle 前关灯。 | 还需要虚拟机交叉编译、部署板端，并与 F4 新固件联调 cycle/related_seq、5 秒延时、检测失败关灯和关灯失败重试。 |
| UNet 四阈值现场标定 | Windows 源码和主机单测已完成，默认值为小区域过滤 `20px`、待复核 `80px`、坏品总面积 `300px`、强连通域 `120px`。 | 还需要分别采集反光良品、边界样本和明确缺陷，记录 `raw/filtered/largest/component_count/evidence/fused_result`；先调过滤线降低良品误报，再调复核线，最后用真实坏品确认两条 STRONG 条件不会漏判。 |
| 模型结果缓存并通知 F4 | Windows 源码已落地：模型检测完成后先让 Z 轴回升；C++ 等 F4 回升 `EVENT_REPORT actuator-move-done` 后，再下发 `MODEL_READY 0x32`，payload 包含 `cycle_id/model_result/part_type/defect_type/top1_confidence/image_seq/model_ms/option_bits`。 | 还需要 MP157 板端部署、F4 烧录后串口实测 ACK/EVENT/NACK，并确认机械臂抓取前检测头已经离开零件。 |
| 机械臂任务触发 | Windows 源码已落地：`MODEL_READY` ACK 后下发 `ARM_JOB_START 0x31`，等待 F4 主动上报称重和电感结果。 | ESP32S3 机械臂固件不在本仓库实现，联调时必须按协议返回阶段完成帧。 |
| F4 异步结果解析 | Windows 源码已落地：Qt 等待 `WEIGHT_RESULT/LDC_RESULT/CYCLE_DONE`，把解析值转换为上传上下文 JSON。 | 还需要板端串口联调确认主动帧、ACK 和超时重发策略。 |
| 云端完整 JSON 组装 | `defect-cos-upload` 已支持 `CLOUD_WEIGHT_CONTEXT/CLOUD_LDC_CONTEXT/CLOUD_F4_FLOW_CONTEXT/CLOUD_DECISION_CONTEXT/CLOUD_VISION_CONTEXT`，会写入云端 `sensor_context/decision_context/vision_context/device_context`。 | 还需要板端真实上传后在云端详情页确认字段可见。 |
| 历史完整重传持久化 | Windows 源码已落地：上传前先把完整上下文写回同一条 `upload_history_YYYYMMDD.json`；历史重新发送会复用这些 JSON 字段，而不是只重传图片。 | 还需要板端构造一次网络失败记录，再恢复网络从历史页重新发送验证。 |
| 连续下一件流程 | Windows 源码已有 `FINAL_SORT_RESULT` 后等待 `CYCLE_DONE` 的闭环；F4 收到 `CYCLE_DONE` ACK 后可启动下一件扫描。 | 还需要 F4 烧录、ESP32S3 配合和整机验证，确认下一件扫描不会在上传完成前提前启动。 |
| 暂停/停止跨阶段处理 | 当前按钮对传送带阶段有基础语义，机械臂/传感器阶段还没联动。 | 暂停时保存自动流程上下文；停止时作废当前 `cycle_id`，清理未上传临时记录或标记为 aborted。 |

### 3.2 F4 还缺什么

| 缺口 | 当前现状 | 需要补的内容 |
|---|---|---|
| `WEIGHT_CALIBRATE 0x30` | F4 源码已定义并分发，调用 `WeightService_RequestCalibration()`。 | 用户烧录后在参数页称重标定弹窗实测 ACK/NACK。 |
| `MODEL_READY 0x32` | F4 源码已实现独立命令，不能再和 `WEIGHT_CALIBRATE 0x30` 共用。 | 烧录后确认 F4 能缓存当前 `cycle_id` 的模型结果，并拒绝错误 cycle 或非法 payload。 |
| `ARM_JOB_START 0x31` | F4 源码已实现任务入口，并能与机械臂服务衔接。 | 烧录后接 ESP32S3，确认先抓取到称重模块，再进入称重采样。 |
| `FINAL_SORT_RESULT 0x33` | F4 源码已实现最终分拣入口，上传失败件必须落待复核盘。 | 烧录后分别测试上传成功 good/bad/review 和上传失败四种路径。 |
| F4-ESP32S3 协议 | 已有 `robot_arm_service.c/.h` 和文档参考协议，ESP32S3 固件不由本仓库实现。 | ESP32S3 端必须实现 ACK、阶段完成和故障帧，否则 F4 自动流程无法闭环。 |
| 机械臂阶段状态机 | F4 源码已按“放称重、称重、放电感、电感、等待上传结果、最终分拣”组织流程。 | 还需要真实机械臂动作、超时、急停和失败盘策略硬件验证。 |
| 称重结果上报 | F4 源码已提供 `BinaryProtocolService_SendWeightResult()`。 | 接 HX711 后确认稳定重量、sample_count、状态位和 MP157 ACK。 |
| 电感结果上报 | F4 源码已提供 `BinaryProtocolService_SendLdcResult()`。 | 接 LDC1614 后确认通道原始值/差值、overall_decision、状态位和 MP157 ACK。 |
| 结果 ACK 和重发 | F4 源码已保留主动结果帧发送和 ACK 语义。 | 还需要整机串口测试丢 ACK、重复帧去重和超时策略。 |
| `EVENT_REPORT` | F4 源码已保留阶段事件上报。 | 上报 `TARGET_CENTERED/ARM_WEIGHT_PLACED/ARM_LDC_PLACED/NEXT_SCAN_STARTED` 等事件需要现场日志确认。 |
| 补光 PB6/TIM4 实机验证 | F4 源码和硬件资源表已落地，主机角度测试通过。 | 用户需 Keil 编译烧录，并用示波器确认 50 Hz、开灯约 2500 us、关灯约 500 us、各输出 2 秒后停止；再接舵机标定机械行程。 |
| 连续下一件扫描 | F4 源码按 `CYCLE_DONE` ACK 后进入下一件扫描的方向实现。 | 烧录后确认暂停/停止/故障不会误触发下一件扫描。 |
| 摄像头轴实机验证 | F4 位置模式代码已接入，现场地址规划为左右轴 `0x03`、上下轴 `0x02`。 | 编译烧录后确认 `ACTUATOR_POS_MOVE` 能让左右轴、上下轴按方向和步数动作，并确认方向映射、限位、堵转和错误码。 |

### 3.3 ESP32S3 还缺什么

| 缺口 | 说明 |
|---|---|
| 二进制协议解析 | 需要按 `docs/f4_esp32s3_arm_protocol/arm_link_protocol.c/.h` 解析 F4 发来的 `A5 5A ... 6B`。 |
| 动作命令实现 | 至少实现 `ARM_MOVE_TO_WEIGHT`、`ARM_MOVE_TO_LDC`、`ARM_SORT_RESULT`、`ARM_HOME`、`ARM_STOP`。 |
| 即时 ACK | 收到合法命令并接受后必须先回 `ACK`，F4 用它判断串口和命令已被 ESP32S3 接收。 |
| 动作完成回包 | 每个动作真正完成后必须回 `ARM_STAGE_DONE`，F4 只能收到这个帧后开始称重或电感检测。 |
| 故障回包 | 抓取失败、舵机异常、动作超时、急停必须回 `FAULT_REPORT` 或 `ARM_STAGE_DONE result!=OK`。 |

## 4. MP157 云端上传还缺哪些数据

`defect-cos-upload` 当前实际创建记录时只写入这些字段：

```json
{
  "record_no": "MP157-YYYYMMDD-HHMMSS",
  "part_id": 1,
  "device_id": 1,
  "result": "good|bad|review",
  "device_context": {
    "part_code": "washer",
    "class_label": "washer_good"
  },
  "captured_at": "2026-07-03T10:00:00+08:00",
  "detected_at": "2026-07-03T10:00:00+08:00"
}
```

还需要把下面字段补进去：

| 数据来源 | 应放到云端哪里 | 需要放什么 |
|---|---|---|
| 摄像头和 ROI | `vision_context.camera`、`vision_context.roi` | 相机 ID、图像宽高、像素格式、中心 ROI 的 `x/y/w/h`、触发来源 `f4_centered`。 |
| 分类模型 | `vision_context.mobilenetv3_small` | 模型名、版本、输入尺寸、top1 标签、top1 分数、topk 列表。 |
| UNet 分割 | `vision_context.unet` | 已写模型名、`evidence`、原始/过滤后像素、连通域数量、最大连通域面积和四个板端阈值；后续仍需补模型版本、面积比例并在云端真实详情页验证展示。 |
| 缺陷框 | `vision_context.defect_boxes` | 缺陷区域 `x/y/w/h`、标签、置信度、来源。 |
| 综合判定 | `decision_context` | pipeline、模型版本、阈值、分类耗时、分割耗时、总耗时、是否需要复核、判定原因。 |
| F4 串口状态 | `sensor_context.f4_uart` | `/dev/ttySTM*`、波特率、最近 F4 帧序号、CRC 是否正确、最近帧时间、状态 `ok/timeout/fault`。 |
| 传送带状态 | `sensor_context.conveyor_motor` | Emm42 型号、`UART4`、`PC10/PC11`、地址 `1`、方向、目标 rpm、误差 px、是否运行、故障位、最近动作。 |
| 称重结果 | `sensor_context.weighing` | HX711 类型、DOUT/SCK、raw_adc、tare、filtered、gross/net weight、stable、sample_count、decision。 |
| 电感结果 | `sensor_context.ldc1614_eddy_current` | LDC1614、I2C、地址、4 通道 raw、baseline、delta、quality、overall_decision。 |
| F4 控制状态 | `sensor_context.f4_control_state` | 最近命令名、最近命令 SEQ、当前 `cycle_id`、报警码。 |
| 板端软件和缓存 | `device_context` | `device_code`、MP157 软件版本、模型包版本、F4 固件版本、本地缓存目录。 |

建议 MP157 后续不要继续只靠环境变量调用 `defect-cos-upload` 拼固定 JSON，而是生成本次完整 `record.json`：

| 文件 | 作用 |
|---|---|
| `/mnt/sdcard/images/<record_no>/record.json` | 保存云端 `POST /records` 完整请求体，断网补传可原样重放。 |
| `/mnt/sdcard/images/<record_no>/model_result.json` | 保存 MobileNetV3-Small、UNet 原始输出和综合判定。 |
| `/mnt/sdcard/images/<record_no>/f4_context.json` | 保存本次收到的 `STATUS_REPORT/WEIGHT_RESULT/LDC_RESULT/CYCLE_DONE/FAULT_REPORT` 解析结果。 |
| `/mnt/sdcard/images/<record_no>/upload_state.json` | 保存 record_id、COS object_key、source/annotated 登记状态。 |

## 5. MP157-F4 协议还需要扩充和落地的命令

| CMD | 名称 | 方向 | 当前状态 | 下一步要求 |
|---:|---|---|---|---|
| `0x23` | `FILL_LIGHT_CONTROL` | MP157 -> F4 | MP157/F4 源码和主机静态测试已实现，待交叉编译、Keil 编译和实物验证。 | payload 为 `cycle_id:u16, action:u8, flags:u8`；必须先等 ACK，再等 `EVENT_REPORT 0x16`，开灯完成后 MP157 固定等待 5 秒，检测结束后必须关灯。 |
| `0x30` | `WEIGHT_CALIBRATE` | MP157 -> F4 | MP157/F4 源码已实现，待烧录和实物验证。 | payload 写 `cycle_id/known_weight_g/flags`，替代文本 `CAL <克重>`。 |
| `0x31` | `ARM_JOB_START` | MP157 -> F4 | MP157/F4 源码已实现，待烧录和 ESP32S3 联调。 | payload 写 `cycle_id/job_id/job_profile/part_type/final_bin_hint/option_bits`，F4 收到后开始调 ESP32S3。 |
| `0x32` | `MODEL_READY` | MP157 -> F4 | MP157/F4 源码已实现，待烧录和实物验证。 | payload 写 `cycle_id/model_result/part_type/defect_type/top1_confidence/image_seq/model_ms/option_bits`。 |
| `0x33` | `FINAL_SORT_RESULT` | MP157 -> F4 | MP157/F4 源码已实现，待烧录和实物验证。 | 上传成功按 `good/bad/review` 映射托盘；上传失败固定 `upload_status=2/final_result=3/final_bin=3`。 |
| `0x83` | `EVENT_REPORT` | F4 -> MP157 | F4 已用于执行器 `0x14/0x15` 和补光舵机完成 `0x16`；其它机械臂/传感器阶段事件待整机联调。 | `related_seq` 必须等于原始位置或补光命令序号；补光还必须匹配 `step_code=action` 与 `detail_i32=0/270`。 |
| `0x84` | `WEIGHT_RESULT` | F4 -> MP157 | 命令字已保留，未主动上报。 | 上报 `raw_adc/tare_raw/net_weight_mg/stable/sample_count/decision/status`。 |
| `0x85` | `LDC_RESULT` | F4 -> MP157 | 命令字已保留，未主动上报。 | 上报 2 或 4 通道 raw/delta、overall_decision、status。 |
| `0x86` | `CYCLE_DONE` | F4 -> MP157 | 命令字已保留，未主动上报。 | 上报 `weight_status/ldc_status/arm_status/fault_bits`，MP157 ACK 后 F4 连续扫描下一件。 |
| `0x50` | `ACTUATOR_POS_MOVE` | MP157 -> F4 | MP157/F4 代码已实现，待编译烧录和硬件验证。 | 负载为 `cycle_id:u16, actuator:u8, direction:u8, mode:u8, speed_rpm:u16, steps:u32, flags:u8`，用于三轴相对位置模式移动。 |
| `0x51` | `ACTUATOR_STOP` | MP157 -> F4 | MP157/F4 代码已实现，待编译烧录和硬件验证。 | 负载为 `cycle_id:u16, actuator:u8, flags:u8`，`actuator=0xFF` 表示模拟急停停止全部可停止执行器。 |
| `0x52` | `ACTUATOR_VEL_MOVE` | MP157 -> F4 | MP157/F4 代码已实现，待编译烧录和硬件验证。 | 负载为 `cycle_id:u16, actuator:u8, direction:u8, speed_rpm:u16, flags:u8`，当前用于手动传送带和左右轴持续速度运动，直到收到 `ACTUATOR_STOP`。 |
| `0x53` | `ACTUATOR_HOME` | MP157 -> F4 | MP157/F4 代码已实现，待编译烧录和硬件验证。 | 负载为 `cycle_id:u16, actuator:u8, flags:u8`，参数页把当前电机页对应执行器的当前位置设为新的零点；不做主动回零运动，也不支持 `actuator=0xFF`。 |
| `0x60` | `WEIGHT_TARE` | MP157 -> F4 | 未定义。 | 后续如需替代文本 `TARE`，必须另行定义负载和 ACK 语义。 |
| `0x62` | `FAULT_CLEAR` | MP157 -> F4 | 未定义。 | 清除可恢复故障位，重新允许开始。 |

## 6. F4 收到 MP157 停止居中后怎么接机械臂

这里的“停止”指视觉对中停止 `BELT_STOP_CENTERED`，不是首页“停止流程” `STOP_CYCLE`。

| 步骤 | 触发 | F4 动作 | F4 回 MP157 |
|---:|---|---|---|
| 1 | MP157 发 `BELT_STOP_CENTERED` | F4 停传送带，状态置 `CENTERED_HOLD`。 | 立即回 `ACK`，可再发 `EVENT_REPORT event=TARGET_CENTERED`。 |
| 2 | MP157 发 `ACTUATOR_POS_MOVE actuator=ACT_CAMERA_Z direction=DOWN` | F4 控制上下轴按相对位置模式下降固定步数。 | 先回 `ACK` 或 `NACK`；真实到位后回 `EVENT_REPORT event=0x14 related_seq=<下降SEQ>`，超时回 `event=0x15`。 |
| 3 | MP157 收到下降 DONE 后复查 ROI，`errorY` 超死区时发 `ACTUATOR_POS_MOVE actuator=ACT_CONVEYOR direction=0/1`，`errorX` 超死区时发 `ACTUATOR_POS_MOVE actuator=ACT_CAMERA_LATERAL direction=0/1`；ROI 到中心后再进入约 3 秒 `focus-settle`。 | F4 控制上下轴、传送带短步前后微调，或控制左右轴短步横向微调。 | 每次位置运动命令先回 `ACK`；真实到位后回 `EVENT_REPORT event=0x14`。 |
| 4 | MP157 完成模型检测并把图片/模型结果写入 SD 卡后，发 `ACTUATOR_POS_MOVE actuator=ACT_CAMERA_Z direction=UP`。 | F4 控制上下轴回升固定步数。 | 先回 `ACK` 或 `NACK`；真实到位后回 `EVENT_REPORT event=0x14 related_seq=<回升SEQ>`。 |
| 5 | Z 轴回升 DONE 后，MP157 发 `MODEL_READY 0x32`。 | F4 保存模型结果。 | 回 `ACK`。 |
| 6 | MP157 发 `ARM_JOB_START 0x31`。 | F4 状态置 `ARM_PICKING`，向 ESP32S3 发 `ARM_MOVE_TO_WEIGHT`。 | 回 `ACK` 表示已接受机械臂任务。 |
| 7 | ESP32S3 回 `ACK` | F4 知道 ESP32S3 已接收动作。 | 可发 `EVENT_REPORT event=ARM_STAGE_ACCEPTED`。 |
| 8 | ESP32S3 回 `ARM_STAGE_DONE stage=PICK_BELT_TO_WEIGHT result=OK` | F4 状态置 `WEIGHING`，开始 HX711 稳定采样。 | 采样完成后发 `WEIGHT_RESULT`。 |
| 9 | MP157 ACK `WEIGHT_RESULT` | F4 向 ESP32S3 发 `ARM_MOVE_TO_LDC`。 | 可发 `EVENT_REPORT event=WEIGHT_ACKED`。 |
| 10 | ESP32S3 回 `ARM_STAGE_DONE stage=WEIGHT_TO_LDC result=OK` | F4 状态置 `LDC_TESTING`，开始 LDC1614 检测。 | 检测完成后发 `LDC_RESULT`。 |
| 11 | MP157 ACK `LDC_RESULT` 后，把图片、模型、重量、电感、F4/ESP32 流程上下文先写回历史 JSON，再按云端契约一次性上传。 | F4 等待 `FINAL_SORT_RESULT`，不提前最终分拣。 | 上传成功或失败都由 MP157 给出最终命令。 |
| 12 | MP157 发 `FINAL_SORT_RESULT 0x33`；上传成功按 `good/bad/review` 选择托盘，上传失败固定 `final_bin=3`。 | F4 向 ESP32S3 发 `ARM_SORT_RESULT`，上传失败件只放待复核盘。 | 回 `ACK`，可发 `EVENT_REPORT event=SORT_COMMAND_SENT`。 |
| 13 | ESP32S3 回 `ARM_STAGE_DONE stage=LDC_TO_SORT_BIN result=OK`。 | F4 发 `CYCLE_DONE`。 | MP157 ACK 后本轮完成。 |
| 14 | MP157 ACK `CYCLE_DONE`。 | F4 自动启动传送带扫描下一件。 | 可发 `EVENT_REPORT event=NEXT_SCAN_STARTED`。 |

## 7. 首页按钮在完整自动流程里的语义

| 当前状态 | 开始 | 暂停 | 继续 | 停止 |
|---|---|---|---|---|
| 空闲/已停止 | 新建 `cycle_id`，发 `START_CYCLE`。 | 禁用。 | 禁用。 | 禁用。 |
| 扫描/跟踪/居中等待模型 | 禁用；如要重新开始，先停止。 | F4 停传送带和可停执行器，保存当前阶段。 | 禁用。 | 停止并作废当前 `cycle_id`。 |
| 机械臂动作中 | 禁用。 | 如果 ESP32S3 支持安全点暂停则暂停；否则 F4 返回 busy。 | 禁用。 | 发 F4 停止，F4 再发 ESP32S3 `ARM_STOP`。 |
| 称重/电感采样中 | 禁用。 | 当前采样完成后暂停，避免半包数据。 | 禁用。 | 停止流程并标记本次记录 aborted。 |
| 已暂停 | 开始新流程前必须先停止旧流程。 | 禁用。 | 发 `RESUME_CYCLE`，继续同一 `cycle_id`。 | 作废当前 `cycle_id`。 |

## 8. 推荐联调顺序

| 阶段 | 目标 | 验证方法 | 通过标准 |
|---:|---|---|---|
| 0 | 本地静态契约 | Windows 本地运行 `cd 20_uvc_camera/qt_camera_display && ./test_qt_kms_overlay_assets.sh`。 | 输出 `PASS: Qt KMS overlay assets contract`，且脚本能找到 `LOCATE`、`sendF4VisionPosition`、`autoVisionTimer`、`autoVisionHasSeenTarget` 和黑色零件漏检保持逻辑。 |
| 1 | 传送带单模块 | MP157 或串口助手发 `START_CYCLE/BELT_MANUAL_CONTROL/QUERY_STATUS`。 | STATUS 显示扫描，实际电机转动。 |
| 2 | overlay 视觉定位 | 板端执行 `printf 'LOCATE\n' \| nc -U /tmp/uvc-kms-overlay-control.sock`，再把零件从画面上方送入相机中间竖向搜索带。 | 无零件时 `has_target=0`；有零件时 `has_target=1`，`center_y` 随零件从上方向下移动而变化。 |
| 3 | 视觉坐标闭环 | 首页按 `开始`，MP157 识别目标后周期发 `VISION_POS`。 | F4 `latest_error_px = axis-target` 的绝对值变小；进入死区后 MP157 发 `BELT_STOP_CENTERED`。 |
| 3.1 | 零件任意阶段丢失重捕获 | 首页按 `开始`，先空传送带观察是否误触发，再分别让垫圈在入画前段、接近 ROI、ROI 内静止时出现 `ring=0` 或短暂 `has_target=0`。 | 空传送带不应进入跟踪；首次识别应由 `ring=1` 候选建链；本轮已见过目标后，`ring=0` 但 bbox 合理、置信度足够高的候选可用于保持/重捕获，F4 不应收到 `VISION_LOST reason=1` 后重新扫描。 |
| 4 | Z 轴下探物理等待 | ROI 居中并收到 `BELT_STOP_CENTERED` ACK 后，Qt 发送 `ACTUATOR_POS_MOVE actuator=2 direction=DOWN steps=zDownFixedSteps`。 | F4 返回 ACK 后，Qt 进入 `z-motion-down-wait`，按步数/速度等待上下轴物理下降完成，传送带保持停止。 |
| 5 | ROI 复查和前后/左右微调 | Z 轴物理下降等待结束后继续读取 `LOCATE`；若 `errorY` 超出死区，Qt 发送 `ACTUATOR_POS_MOVE actuator=0 direction=0/1 steps=fine_tune_steps`；若 `errorX` 超出死区，Qt 发送 `ACTUATOR_POS_MOVE actuator=1 direction=0/1 steps=fine_tune_steps`。`fine_tune_steps` 至少等于对应电机 `minStep`，每超出约 `4px` 加一档，连续无改善时继续加档，单次最多 `12 * minStep` 且不超过 `10000 step`；`errorX>0` 时左右轴发送 `direction=1`，让相机右移、画面左移回中心。 | 微调次数受限，X/Y 偏差都回到 ±24px 内后进入 3 秒对焦稳定；若 `steps` 放大后误差仍不变，优先检查 F4 是否真的收到 `ACTUATOR_POS_MOVE`、电机地址、方向、使能、共地和张大头 Response。 |
| 6 | 补光、自动模型、关灯、Z 轴回升和左右轴回中 | 3 秒对焦结束后观察 action=1、`event=0x16 angle=270` 和 5 秒等待，再运行双模型；模型完成后观察 action=0、`event=0x16 angle=0`，随后才发送 Z 轴 UP 和必要的左右轴反向回中。 | 开灯完成前和 5 秒内不检测；检测成功/失败都关灯；关灯失败不启动 Z 轴/机械臂；正常关灯后模型融合、Z 回升和必要回中按原流程推进。 |
| 7 | F4-ESP32S3 握手 | F4 发 `HELLO/HEARTBEAT/ARM_HOME`。 | ESP32S3 ACK，机械臂可回安全位。 |
| 8 | 称重阶段 | F4 发 `ARM_MOVE_TO_WEIGHT`，ESP32S3 完成后 F4 采样 HX711。 | MP157 收到 `WEIGHT_RESULT`。 |
| 9 | 电感阶段 | F4 发 `ARM_MOVE_TO_LDC`，ESP32S3 完成后 F4 采样 LDC1614。 | MP157 收到 `LDC_RESULT`。 |
| 10 | 模型结果分拣 | F4 根据本轮缓存的 MP157 模型结果发送 `ARM_SORT_RESULT target_bin=good/bad/review`。 | ESP32S3 把零件放入对应盘子，F4 收到分拣完成回包。 |
| 11 | 云端完整字段 | MP157 组装完整 `record.json` 上传。 | 云端详情能看到图片、模型、重量、电感、分拣目标、F4 状态。 |
| 12 | 连续自动 | CYCLE_DONE ACK 后 F4 自动扫描下一件。 | 不再需要人工重复点开始。 |

## 9. 当前现场调试提醒

| 现象 | 判断 |
|---|---|
| Qt 显示 `F4: ACK ... state=SCANNING` 但传送带不动 | 软件协议层已接受命令，优先查 `UART4 PC10/PC11`、TX/RX 交叉、电机地址 `0x01`、驱动供电、共地、波特率、使能。 |
| F4 上电刷 `LDC init failed` | LDC 未接时应改为二进制 `FAULT_REPORT` 或状态位，不应在 USART1 正式链路刷文本。 |
| 只接传送带时 | `START_CYCLE` 和 `BELT_MANUAL_CONTROL` 应可独立工作，称重、电感、摄像头轴、机械臂未接不应阻止传送带调试。 |
| 停止后按继续 | 不允许；停止已经作废旧流程，只能重新按开始生成新 `cycle_id`。 |
| 暂停后按继续 | 允许；继续同一个 `cycle_id`，不要生成新记录。 |
