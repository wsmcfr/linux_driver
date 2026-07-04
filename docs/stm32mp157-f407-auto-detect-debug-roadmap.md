# STM32MP157-F407 自动检测联调路线与缺口清单

| 项目 | 内容 |
|---|---|
| 文档目的 | 把当前自动检测总目标、已完成内容、MP157/F4/ESP32S3/云端还缺什么写清楚，后续按表逐项联调。 |
| 适用链路 | STM32MP157 视觉主控 `<->` STM32F407 执行主控 `<->` ESP32S3 机械臂控制器 `<->` 云端。 |
| 现有主协议 | `docs/stm32mp157-f407-binary-protocol.md`。 |
| 机械臂协议 | `docs/f4_esp32s3_arm_protocol/README.md`。 |
| 云端字段依据 | `D:\yunfuwu\docs\stm32mp157-cloud-upload-data-contract.md`。 |
| 更新时间 | `2026-07-04`。 |

## 1. 总目标

最终要实现的是“首页按一次开始后连续全自动检测”：

| 阶段 | 谁负责 | 目标动作 | 成功标志 |
|---|---|---|---|
| 上料扫描 | F4 | 启动传送带张大头 Emm42，让零件进入摄像头视野。 | MP157 识别到零件并开始发送视觉坐标。 |
| 视觉对中 | MP157 + F4 | MP157 周期发送零件坐标，F4 先控制传送带完成上料和前后/Y 方向对中；Z 轴下降复查后，传送带继续负责前后短步微调，摄像头左右轴负责 X 方向短步微调。 | 零件进入中心 ROI，MP157 发送 `BELT_STOP_CENTERED`。 |
| Z 轴下探、对焦和复查 | MP157 + F4 | F4 停止传送带后，MP157 让摄像头上下轴下降固定步数，收到下降 ACK 后等待约 3 秒让摄像头对焦稳定，再重新读取 ROI；若仍有轻微偏移，`errorY` 交给传送带前后短步微调，`errorX` 交给摄像头左右轴短步微调。 | 零件仍在 ROI 中央，MP157 开始模型检测。 |
| 模型检测 | MP157 | 保存本次原图，运行 MobileNetV3-Small 和 UNet，先缓存结果不立即上传。 | MP157 生成本次模型结果、零件类型、综合 good/bad/review。 |
| 机械臂到称重 | F4 + ESP32S3 | F4 通知 ESP32S3 抓取 ROI 中零件并放到称重模块。 | ESP32S3 返回 `ARM_STAGE_DONE stage=PICK_BELT_TO_WEIGHT result=OK`。 |
| 称重 | F4 | F4 读取 HX711，稳定后把重量结果发给 MP157。 | MP157 收到 `WEIGHT_RESULT` 并 ACK。 |
| 机械臂到电感 | F4 + ESP32S3 | F4 通知 ESP32S3 从称重模块夹起零件并放到电磁感应模块。 | ESP32S3 返回 `ARM_STAGE_DONE stage=WEIGHT_TO_LDC result=OK`。 |
| 电感检测 | F4 | F4 读取 LDC1614，生成电磁感应结果并发给 MP157。 | MP157 收到 `LDC_RESULT` 并 ACK。 |
| 分拣和完成 | F4 + ESP32S3 + MP157 | F4 使用 MP157 在 `MODEL_READY` 中下发的模型综合好坏结果选择放置盘，要求 ESP32S3 从电感模块夹起零件并放到对应位置，再发送 `CYCLE_DONE`。 | MP157 合并图片、模型、重量、电感、F4 状态上传云端；F4 启动下一件扫描。 |

核心原则：

| 原则 | 说明 |
|---|---|
| MP157 不直接控电机和传感器 | MP157 只发业务指令和视觉坐标，实际运动、称重、电感读取由 F4 完成。 |
| F4 不做视觉模型和云端上传 | F4 只回传结构化结果帧，云端 JSON 由 MP157 组装。 |
| F4 不重新判断零件好坏 | F4 只缓存 MP157 的 `MODEL_READY.model_result`；称重和电感结果用于上传和追溯，首版不改变分拣盘选择。 |
| ESP32S3 只做机械臂动作 | ESP32S3 不判断好坏，不读重量和电感，只告诉 F4 动作是否已经完成。 |
| 所有主链路都走二进制帧 | MP157-F4 与 F4-ESP32S3 都使用 `A5 5A ... CRC16 ... 6B`，不再依赖 `[OK]`、`[ERROR]` 文本。 |

## 2. 当前已经完成

| 模块 | 已完成内容 | 证据/现状 |
|---|---|---|
| MP157-F4 帧格式 | 已定义帧头 `A5 5A`、版本、命令、长度、SEQ、payload、CRC16-CCITT-FALSE、帧尾 `6B`。 | `docs/stm32mp157-f407-binary-protocol.md`。 |
| MP157 首页四按钮 | 首页 `开始/暂停/继续/停止` 已调用 C++ 发送二进制 `START_CYCLE/PAUSE_CYCLE/RESUME_CYCLE/STOP_CYCLE`。 | `20_uvc_camera/qt_camera_display/main.cpp`、`qml/Main.qml`。 |
| MP157 overlay 内存定位 | `uvc_kms_overlay` 已新增 `LOCATE` 命令，直接从原始 YUYV 帧中间 300px 宽全高搜索带计算零件中心、bbox 和置信度，不保存图片、不调用模型。 | `20_uvc_camera/qt_camera_display/uvc_kms_overlay.c`；板端可用 `printf 'LOCATE\n' \| nc -U /tmp/uvc-kms-overlay-control.sock` 验证。 |
| MP157 视觉坐标闭环首版 | 首页 `开始` 收到 F4 ACK 后启动 `autoVisionTimer`，每 100ms 请求 `LOCATE`；已按“上方来料”使用 `center_y` 对齐 `height/2`，下发 `VISION_POS`；连续 3 帧进入 ±24px 后发送 `BELT_STOP_CENTERED`；目标出现过后短暂漏检不再发送 `VISION_LOST reason=1`，避免 F4 重新扫描把零件送走。 | `20_uvc_camera/qt_camera_display/main.cpp`、`20_uvc_camera/qt_camera_display/qml/Main.qml`；本地静态测试已覆盖 `requestAutoVisionLocate/sendF4VisionPosition/autoVisionTimer/autoVisionHasSeenTarget` 等 marker。 |
| MP157 Z 轴同步下探检测 | 收到 `BELT_STOP_CENTERED` ACK 后，QML 下发 `ACTUATOR_POS_MOVE actuator=ACT_CAMERA_Z direction=DOWN`，用参数页 `zDownFixedSteps` 下降固定步数；下降 ACK 后等待约 3 秒让摄像头对焦稳定，再复查 ROI，必要时用传送带做前后/Y 短步微调，用 `ACT_CAMERA_LATERAL` 左右轴做 X 短步微调；模型检测完成后再用 `zUpFixedSteps` 自动回升。 | `20_uvc_camera/qt_camera_display/main.cpp`、`qml/Main.qml`；静态测试覆盖 `ACTUATOR_POS_MOVE`、`zDownFixedSteps`、`zUpFixedSteps`、`autoVisionFineTuneConveyor`、`autoVisionFineTuneLateral`、`autoVisionZFocusSettleMs`、`manualMotorPopup` 等 marker。 |
| MP157 底部提示 | F4 返回内容已在底部提示前统一加 `F4:`。 | `Main.qml::f4ToastText()`。 |
| MP157 手动传送带调试 | 手动页可发送 `BELT_MANUAL_CONTROL` 和 `QUERY_STATUS`。 | F4 ACK、STATUS_REPORT 能被 Qt 解析。 |
| MP157 三轴手动控制 | 手动页已改成三轴弹窗，三页分别控制传送带、摄像头左右轴、摄像头上下轴；传送带/左右轴按一次方向键下发 `ACTUATOR_VEL_MOVE` 并持续运动到停止，上下轴按一次下降/上升只执行 `zDownFixedSteps/zUpFixedSteps` 固定步数；模拟急停下发 `ACTUATOR_STOP actuator=0xFF`，安全状态区域改为可滑动查看。 | `20_uvc_camera/qt_camera_display/qml/Main.qml`。 |
| F4 二进制解析 | F4 已能解析 `HELLO/HEARTBEAT/START/PAUSE/RESUME/STOP/VISION_POS/VISION_LOST/BELT_STOP_CENTERED/QUERY_STATUS/BELT_MANUAL_CONTROL/ACTUATOR_POS_MOVE/ACTUATOR_STOP/ACTUATOR_VEL_MOVE/ACTUATOR_HOME`，并修正执行器成功 ACK 必须统一 `status=0`。 | `E:\hal\bisai_f407_project\User\App\binary_protocol_service.c`。 |
| F4 传送带绑定 | 传送带服务已按当前方案使用 `UART4 PC10/PC11`，Emm42 地址 `0x01`。 | F4 启动日志显示 `UART4=PC10/PC11, addr=1`。 |
| F4 摄像头电机 ID 规划 | 当前现场摄像头左右轴地址 `0x03`，摄像头上下轴地址 `0x02`，共用 `USART6 PC6/PC7`。 | F4 启动日志或 `CAMINFO` 显示 `lateral_addr=3, z_addr=2`。 |
| F4 执行器位置、速度和设零模式 | `binary_protocol_service` 已分发 `ACTUATOR_POS_MOVE/ACTUATOR_STOP/ACTUATOR_VEL_MOVE/ACTUATOR_HOME`；`conveyor_motor_service` 和 `camera_motor_service` 分别负责速度持续运动、相对位置移动、停止和当前位置设零；`emm42_motor` 已接入 Emm42 速度模式、`0xFD` 相对位置模式和 `[addr 0A 6D 6B]` 当前位置清零命令。 | `E:\hal\bisai_f407_project\User\App\binary_protocol_service.c`、`camera_motor_service.c`、`conveyor_motor_service.c`、`E:\hal\bisai_f407_project\User\Driver\emm42_motor.c`。 |
| F4 状态查询 | `QUERY_STATUS` 成功返回 `STATUS_REPORT`，包含 F4 状态、传送带模式、方向、速度、误差、故障位。 | 当前串口助手已看到 `belt_desired=SCAN`、`speed_rpm=300` 这类状态。 |
| 云端图片上传 | MP157 已能上传 source 原图和多张 annotated 结果图，并创建云端记录。 | `defect-cos-upload` 支持 `--jpg` 和多次 `--annotated`。 |

## 3. 当前没有完成的关键缺口

### 3.1 MP157 还缺什么

| 缺口 | 当前现状 | 需要补的内容 |
|---|---|---|
| 自动视觉坐标下发 | 首版代码已落地：`LOCATE` -> `VISION_POS`，上方来料使用 `center_y` 和 `height/2`。 | 还需要同步到虚拟机、交叉编译、部署到板端，并用真实三种零件调阈值、看 F4 `latest_error_px` 是否随零件靠近中心而减小。 |
| ROI 居中停止 | 首版代码已落地：连续 3 帧 `abs(center_y-height/2)<=24px` 后发送 `BELT_STOP_CENTERED hold_ms=2000`，与当前 F4 死区保持同量级。 | 还需要实测传送带惯性、F4 停机响应和相机画面稳定时间；必要时调整死区、稳定帧数或 F4 减速曲线。 |
| Z 轴下降后 ROI 复查 | 首版代码已落地：F4 居中停机 ACK 后 QML 先让 Z 轴下降，再复查 ROI，必要时用传送带做前后/Y 微调、用左右轴做 X 微调，最后调用现有 `handleDetectAction()`。 | 还需要板端确认 Z 轴下降固定值、传送带前后微调方向、左右轴微调方向、ROI 死区和模型检测触发不会互相抢串口或摄像头资源。 |
| 模型结果缓存并通知 F4 | 当前上传脚本能拿模型结果，但自动流程没有把模型结果下发 F4。 | 定义并实现 `MODEL_READY` payload，把 `part_type/model_result/confidence/defect_type/model_time_ms` 发给 F4。 |
| 机械臂任务触发 | MP157 还没有向 F4 下发正式 `ARM_JOB_START`。 | 模型完成后发送 `ARM_JOB_START`，告诉 F4 本件可进入机械臂、称重、电感流程。 |
| F4 异步结果解析 | Qt 目前主要等 ACK/NACK/STATUS_REPORT/FAULT_REPORT，未完整消费 `EVENT_REPORT/WEIGHT_RESULT/LDC_RESULT/CYCLE_DONE`。 | 串口接收线程要持续读取 F4 主动上报帧，并按 `cycle_id` 存入当前检测上下文。 |
| 云端完整 JSON 组装 | `defect-cos-upload` 当前只传基础记录字段和图片文件。 | 生成 `record.json`，填入 `vision_context`、`sensor_context`、`decision_context`、`device_context` 后再 POST。 |
| 连续下一件流程 | 完成一件后 MP157 还没有完整状态机等待 F4 下一轮扫描。 | ACK `CYCLE_DONE` 后清理当前件缓存，递增 `cycle_id`，继续监听下一件视觉坐标。 |
| 暂停/停止跨阶段处理 | 当前按钮对传送带阶段有基础语义，机械臂/传感器阶段还没联动。 | 暂停时保存自动流程上下文；停止时作废当前 `cycle_id`，清理未上传临时记录或标记为 aborted。 |

### 3.2 F4 还缺什么

| 缺口 | 当前现状 | 需要补的内容 |
|---|---|---|
| `MODEL_READY 0x30` | 命令字已在头文件保留，但 `HandleFrame()` 没有分发，当前会 NACK unknown。 | 解码模型结果，保存到当前 `cycle_id` 上下文，状态进入 `WAIT_MODEL` 或 `WAIT_ARM_JOB`。 |
| `ARM_JOB_START 0x31` | 命令字已保留，未处理。 | 收到后启动 F4-ESP32S3 机械臂协议，先发 `ARM_MOVE_TO_WEIGHT`。 |
| F4-ESP32S3 协议 | 当前 F4 机械臂服务仍偏向 LeArm `55 55` 透传和启动探测。 | 新增 `arm_link_protocol.c/.h`，改用 `A5 5A ... 6B` 二进制协议发动作阶段命令。 |
| 机械臂阶段状态机 | 当前没有“放称重、称重、放电感、电感、分拣”的自动推进。 | 按 ESP32S3 `ARM_STAGE_DONE` 推进 `WEIGHING/LDC_TESTING/SORTING/DONE`。 |
| 称重结果上报 | HX711 服务存在，但还没有把自动流程稳定重量打包成 `WEIGHT_RESULT` 主动发给 MP157。 | 在 ESP32S3 报“已放到称重模块”后采样 HX711，发送 `WEIGHT_RESULT`。 |
| 电感结果上报 | LDC1614 服务存在，但模块未接时会文本刷错；自动结果帧未完成。 | 接入后读取通道原始值/差值，发送 `LDC_RESULT`；未接时只发 `FAULT_REPORT`，不刷文本。 |
| 结果 ACK 和重发 | `WEIGHT_RESULT/LDC_RESULT/CYCLE_DONE` 需要 MP157 ACK，但 F4 还没实现主动结果帧和等待 ACK。 | 结果帧发送后保留最近结果，超时可重发或进入故障。 |
| `EVENT_REPORT` | 命令字保留，未用于阶段提示。 | 上报 `TARGET_CENTERED/ARM_WEIGHT_PLACED/ARM_LDC_PLACED/NEXT_SCAN_STARTED` 等事件。 |
| 连续下一件扫描 | 文档定义了 CYCLE_DONE 后继续扫描，但 F4 自动状态机还未完整实现。 | 收到 MP157 对 `CYCLE_DONE` 的 ACK 后，若未停止/暂停/故障，自动 `ConveyorMotorService_RequestScan()`。 |
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
| UNet 分割 | `vision_context.unet` | 模型名、版本、阈值、缺陷像素面积、面积比例、连通域数量、最大连通域面积。 |
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
| `0x30` | `MODEL_READY` | MP157 -> F4 | 命令字已保留，F4 未处理。 | payload 写 `cycle_id/part_type/model_result/confidence/defect_type/model_time_ms/model_flags`。 |
| `0x31` | `ARM_JOB_START` | MP157 -> F4 | 命令字已保留，F4 未处理。 | payload 写 `cycle_id/job_flags/part_type/model_result/target_bin/timeout_ms`，F4 收到后开始调 ESP32S3。 |
| `0x83` | `EVENT_REPORT` | F4 -> MP157 | 命令字已保留，未主动上报。 | 上报居中、机械臂放到称重、放到电感、分拣完成、下一轮扫描启动。 |
| `0x84` | `WEIGHT_RESULT` | F4 -> MP157 | 命令字已保留，未主动上报。 | 上报 `raw_adc/tare_raw/net_weight_mg/stable/sample_count/decision/status`。 |
| `0x85` | `LDC_RESULT` | F4 -> MP157 | 命令字已保留，未主动上报。 | 上报 2 或 4 通道 raw/delta、overall_decision、status。 |
| `0x86` | `CYCLE_DONE` | F4 -> MP157 | 命令字已保留，未主动上报。 | 上报 `weight_status/ldc_status/arm_status/fault_bits`，MP157 ACK 后 F4 连续扫描下一件。 |
| `0x50` | `ACTUATOR_POS_MOVE` | MP157 -> F4 | MP157/F4 代码已实现，待编译烧录和硬件验证。 | 负载为 `cycle_id:u16, actuator:u8, direction:u8, mode:u8, speed_rpm:u16, steps:u32, flags:u8`，用于三轴相对位置模式移动。 |
| `0x51` | `ACTUATOR_STOP` | MP157 -> F4 | MP157/F4 代码已实现，待编译烧录和硬件验证。 | 负载为 `cycle_id:u16, actuator:u8, flags:u8`，`actuator=0xFF` 表示模拟急停停止全部可停止执行器。 |
| `0x52` | `ACTUATOR_VEL_MOVE` | MP157 -> F4 | MP157/F4 代码已实现，待编译烧录和硬件验证。 | 负载为 `cycle_id:u16, actuator:u8, direction:u8, speed_rpm:u16, flags:u8`，当前用于手动传送带和左右轴持续速度运动，直到收到 `ACTUATOR_STOP`。 |
| `0x53` | `ACTUATOR_HOME` | MP157 -> F4 | MP157/F4 代码已实现，待编译烧录和硬件验证。 | 负载为 `cycle_id:u16, actuator:u8, flags:u8`，参数页把当前电机页对应执行器的当前位置设为新的零点；不做主动回零运动，也不支持 `actuator=0xFF`。 |
| `0x60` | `WEIGHT_TARE` | MP157 -> F4 | 未定义。 | 后续替代文本 `TARE`。 |
| `0x61` | `WEIGHT_CALIBRATE` | MP157 -> F4 | 未定义。 | 后续替代文本 `CAL <克重>`。 |
| `0x62` | `FAULT_CLEAR` | MP157 -> F4 | 未定义。 | 清除可恢复故障位，重新允许开始。 |

## 6. F4 收到 MP157 停止居中后怎么接机械臂

这里的“停止”指视觉对中停止 `BELT_STOP_CENTERED`，不是首页“停止流程” `STOP_CYCLE`。

| 步骤 | 触发 | F4 动作 | F4 回 MP157 |
|---:|---|---|---|
| 1 | MP157 发 `BELT_STOP_CENTERED` | F4 停传送带，状态置 `CENTERED_HOLD`。 | 立即回 `ACK`，可再发 `EVENT_REPORT event=TARGET_CENTERED`。 |
| 2 | MP157 发 `ACTUATOR_POS_MOVE actuator=ACT_CAMERA_Z direction=DOWN` | F4 控制上下轴按相对位置模式下降固定步数。 | 回 `ACK` 或 `NACK`。 |
| 3 | MP157 收到下降 ACK 后等待约 3 秒，再复查 ROI；`errorY` 超死区时发 `ACTUATOR_POS_MOVE actuator=ACT_CONVEYOR direction=0/1`，`errorX` 超死区时发 `ACTUATOR_POS_MOVE actuator=ACT_CAMERA_LATERAL direction=0/1` | F4 控制传送带短步前后微调，或控制左右轴短步横向微调。 | 每次回 `ACK` 或 `NACK`，MP157 再读下一帧坐标。 |
| 4 | MP157 完成模型检测后发 `ACTUATOR_POS_MOVE actuator=ACT_CAMERA_Z direction=UP` | F4 控制上下轴回升固定步数。 | 回 `ACK` 或 `NACK`，MP157 同步提示。 |
| 5 | MP157 发 `MODEL_READY` | F4 保存模型结果。 | 回 `ACK`。 |
| 6 | MP157 发 `ARM_JOB_START` | F4 状态置 `ARM_PICKING`，向 ESP32S3 发 `ARM_MOVE_TO_WEIGHT`。 | 回 `ACK` 表示已接受机械臂任务。 |
| 7 | ESP32S3 回 `ACK` | F4 知道 ESP32S3 已接收动作。 | 可发 `EVENT_REPORT event=ARM_STAGE_ACCEPTED`。 |
| 8 | ESP32S3 回 `ARM_STAGE_DONE stage=PICK_BELT_TO_WEIGHT result=OK` | F4 状态置 `WEIGHING`，开始 HX711 稳定采样。 | 采样完成后发 `WEIGHT_RESULT`。 |
| 9 | MP157 ACK `WEIGHT_RESULT` | F4 向 ESP32S3 发 `ARM_MOVE_TO_LDC`。 | 可发 `EVENT_REPORT event=WEIGHT_ACKED`。 |
| 10 | ESP32S3 回 `ARM_STAGE_DONE stage=WEIGHT_TO_LDC result=OK` | F4 状态置 `LDC_TESTING`，开始 LDC1614 检测。 | 检测完成后发 `LDC_RESULT`。 |
| 11 | MP157 ACK `LDC_RESULT`，且 F4 已收到 ESP32S3 的电感放置完成回包 | F4 读取本轮缓存的 `MODEL_READY.model_result`，把 `GOOD` 映射到良品盘、`BAD` 映射到不良品盘、`UNCERTAIN/UNKNOWN` 映射到待复核盘，然后向 ESP32S3 发 `ARM_SORT_RESULT`。 | 可发 `EVENT_REPORT event=SORT_COMMAND_SENT`，事件中带 `target_bin`。 |
| 12 | ESP32S3 回 `ARM_STAGE_DONE stage=LDC_TO_SORT_BIN result=OK` | F4 发 `CYCLE_DONE`。 | MP157 合并图片、模型、重量、电感、分拣目标和 F4 状态上传云端。 |
| 13 | MP157 ACK `CYCLE_DONE` | F4 自动启动传送带扫描下一件。 | 可发 `EVENT_REPORT event=NEXT_SCAN_STARTED`。 |

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
| 3.1 | 黑色波形零件漏检保持 | 首页按 `开始`，让黑色波形零件已经被识别一次后，遮挡/反光导致 `LOCATE` 偶发返回 `has_target=0`。 | Qt 显示 `目标短暂丢失` 或 `连续丢失但保持停机`，右侧偏差显示 `-- px`；F4 不应收到 `VISION_LOST reason=1` 后重新扫描，传送带应保持或超时停住等待重新识别。 |
| 4 | Z 轴下探和对焦等待 | ROI 居中并收到 `BELT_STOP_CENTERED` ACK 后，Qt 发送 `ACTUATOR_POS_MOVE actuator=2 direction=DOWN steps=zDownFixedSteps`。 | F4 返回 ACK，摄像头上下轴下降固定高度，Qt 显示等待约 3 秒对焦稳定，传送带保持停止。 |
| 5 | 对焦稳定后 ROI 复查和前后/左右微调 | 3 秒等待结束后继续读取 `LOCATE`；若 `errorY` 超出死区，Qt 发送 `ACTUATOR_POS_MOVE actuator=0 direction=0/1 steps=minStep`；若 `errorX` 超出死区，Qt 发送 `ACTUATOR_POS_MOVE actuator=1 direction=0/1 steps=minStep`。 | 微调次数受限，X/Y 偏差都回到 ±24px 内后进入检测；超限时界面显示需要人工复核。 |
| 6 | 自动模型触发和 Z 轴回升 | ROI 复查通过后运行双模型；模型检测完成回调发送 `ACTUATOR_POS_MOVE actuator=2 direction=UP steps=zUpFixedSteps`。 | Qt 生成 source、annotated、模型结果缓存，Z 轴回到原高度。 |
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
