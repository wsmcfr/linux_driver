# STM32MP157-F407 摄像头 Z 轴同步下探检测设计

| 项目 | 内容 |
|---|---|
| 目标 | 零件停在摄像头 ROI 中央后，先让摄像头上下轴下降固定步数，再用前后轴微调 ROI，完成模型检测后立即让上下轴回升固定步数。 |
| 方案取舍 | 本轮采用同步简化版，不等待 ESP32S3 机械臂取走完成事件，也不要求 MP157 常驻监听 F4 `EVENT_REPORT`。 |
| 控制边界 | MP157 只发送 F4 二进制业务命令；F4 再通过 `USART6 PC6/PC7` 向张大头 Emm42 发送 `0xFD` 相对位置模式帧。 |
| 生效提醒 | 文档和代码写入 Windows 仓库不等于板端或 F4 已生效；MP157 需要同步到虚拟机、交叉编译、部署，F4 需要重新编译烧录。 |

## 当前基础

| 模块 | 已具备能力 | 本轮复用方式 |
|---|---|---|
| MP157 自动视觉 | 已能 `LOCATE`、发送 `VISION_POS`，连续居中后发送 `BELT_STOP_CENTERED`。 | 把原来居中 ACK 后 2 秒直接检测，改为先执行 Z 轴下降和前后微调。 |
| MP157 参数页 | 已有三台步进电机地址、最小步长、速度、方向配置，并能下发 `STEPPER_PARAM_SET`。 | 给摄像头上下轴增加 `z_down_fixed_steps` 和 `z_up_fixed_steps`，保存 JSON 并下发给 F4。 |
| F4 摄像头电机服务 | 已支持 `RequestForwardJog`、`RequestZJog`、`RequestStopAll` 和运行时配置。 | 新增相对位置模式请求，继续由摄像头电机任务独占 `USART6` 串行发帧。 |
| Emm42 驱动层 | 已支持使能、速度模式和立即停止。 | 新增 `EMM42_MotorMoveRelativePosition()`，封装 `0xFD` 相对位置模式。 |

## 同步自动流程

| 顺序 | MP157 状态 | MP157 下发 | F4 动作 | 进入下一步条件 |
|---:|---|---|---|---|
| 1 | `视觉居中` | `BELT_STOP_CENTERED` | 停止传送带。 | 收到 ACK。 |
| 2 | `Z轴下降` | `ACTUATOR_POS_MOVE axis=CAMERA_Z dir=DOWN steps=z_down_fixed_steps` | 上下轴按相对位置模式下降固定步数。 | 收到 ACK。 |
| 3 | `下降后复查` | `LOCATE` | 无。 | MP157 得到当前零件坐标。 |
| 4 | `前后微调` | `ACTUATOR_POS_MOVE axis=CAMERA_FORWARD dir=FORWARD/BACKWARD steps=minStep` | 前后轴按相对位置模式小步微调。 | 误差进入死区或达到重试上限。 |
| 5 | `模型检测` | 复用现有 `requestDetectCurrentFrame()` | F4 等待后续动作。 | Qt 收到双模型检测完成信号。 |
| 6 | `Z轴回升` | `ACTUATOR_POS_MOVE axis=CAMERA_Z dir=UP steps=z_up_fixed_steps` | 上下轴按相对位置模式回升固定步数。 | 收到 ACK 或 NACK 后给出提示。 |

## 新增协议建议

| CMD | 名称 | 方向 | 负载 | 用途 |
|---:|---|---|---|---|
| `0x50` | `ACTUATOR_POS_MOVE` | MP157 -> F4 | `cycle_id:u16, actuator:u8, direction:u8, mode:u8, speed_rpm:u16, steps:u32, flags:u8` | 手动页和自动流程共用的执行器相对位置移动。 |
| `0x51` | `ACTUATOR_STOP` | MP157 -> F4 | `cycle_id:u16, actuator:u8, flags:u8` | 急停或手动停止时停止指定执行器；`actuator=0xFF` 表示全部可停止执行器。 |

执行器编号：

| 编号 | 名称 | 说明 |
|---:|---|---|
| `0` | `ACT_CONVEYOR` | 传送带电机，当前位置模式不用于自动下探。 |
| `1` | `ACT_CAMERA_FORWARD` | 摄像头前进/后退电机，用于 Z 轴下降后的 ROI 微调。 |
| `2` | `ACT_CAMERA_Z` | 摄像头上下电机，用于下降到检测高度和回升到原高度。 |
| `0xFF` | `ACT_ALL` | 停止命令专用，停止传送带和摄像头运动电机。 |

方向枚举：

| 方向值 | 前后轴含义 | 上下轴含义 |
|---:|---|---|
| `0` | 后退 | 下降 |
| `1` | 前进 | 上升 |

## 参数页变化

| 字段 | 归属 | 范围 | 用途 |
|---|---|---:|---|
| `z_down_fixed_steps` | 摄像头上下电机 | `0~4294967295 step` | ROI 居中停机后下降的相对步数。 |
| `z_up_fixed_steps` | 摄像头上下电机 | `0~4294967295 step` | 模型检测完成后回升的相对步数。 |

说明：

| 项目 | 规则 |
|---|---|
| 范围来源 | Emm42 `0xFD` 位置模式的脉冲数字段是 4 字节无符号数，因此 MP157 保存和协议下发都按 `u32 step` 处理。 |
| 缺省值 | 默认先给 `0`，表示本地配置存在但不会产生实际上下移动，现场确认机构方向后再输入具体步数。 |
| 速度 | 复用摄像头上下电机 `normalSpeedRpm`，F4 仍做 `0~5000 rpm` 限幅。 |

## 手动控制变化

| 区域 | 设计 |
|---|---|
| 三轴弹窗 | 当前传送带控制入口改为三页弹窗：传送带、摄像头前后、摄像头上下。 |
| 传送带页 | 保留巡航、停止、状态查询，继续使用已有 `BELT_MANUAL_CONTROL/QUERY_STATUS`。 |
| 前后轴页 | 提供前进固定步、后退固定步、停止、状态刷新。 |
| 上下轴页 | 提供上升固定步、下降固定步、停止、状态刷新。 |
| 模拟急停 | 按下后真实发送 `ACTUATOR_STOP actuator=ACT_ALL`，并保留 QML 本地禁用运动按钮。 |
| 安全状态 | 右侧状态详情使用 `Flickable`，底部动作按钮固定，避免状态文本被按钮遮挡。 |

## 验证重点

| 测试目标 | 执行位置 | 命令/动作 | 预期输出/现象 | 失败时排查 |
|---|---|---|---|---|
| 静态契约 | Windows 或虚拟机仓库 | `cd 20_uvc_camera/qt_camera_display && sh ./test_qt_kms_overlay_assets.sh` | 输出 `PASS: Qt KMS overlay assets contract`。 | 检查 `ACTUATOR_POS_MOVE`、`sendF4ActuatorPositionMove`、`zDownFixedSteps`、`manualMotorPopup` 等 marker。 |
| 参数保存 | 板端 Qt 参数页 | 输入 Z 轴下降/上升固定值并保存。 | `/mnt/sdcard/config/defect_ui_config.json` 包含 `z_down_fixed_steps/z_up_fixed_steps`。 | 查 SD 卡挂载、JSON 保存结果和 QML 输入范围。 |
| 自动下探 | 板端首页 + F4 日志 | 首页按开始，零件居中后观察 Z 轴。 | `BELT_STOP_CENTERED` ACK 后，上下轴下降固定步数，再进行复查和检测。 | 查 F4 是否实现 `ACTUATOR_POS_MOVE`，查 `USART6`、上下轴地址 `0x02`、方向映射和 Emm42 电源。 |
| 检测回升 | 板端首页 | 等双模型检测结束。 | MP157 发送 Z 轴上升固定步数，界面显示回升 ACK/NACK。 | 查 `detectCurrentFrameFinished` 后是否调用回升，查串口忙状态。 |
| 手动三轴 | 板端手动页 | 打开三轴弹窗，分别点传送带、前后、上下控制。 | 三页动作不会互相遮挡，摄像头轴命令走 F4 二进制协议。 | 查按钮 enabled 条件、F4 ACK/NACK 和摄像头电机任务队列。 |
