# STM32MP157 与 STM32F407 自动检测二进制通讯协议

| 项目 | 内容 |
|---|---|
| 文档目的 | 记录 STM32MP157 与 STM32F407 之间用于自动检测流程的二进制串口协议，方便后续按命令字和负载表查找实现。 |
| 适用阶段 | 当前调试阶段 MP157-F4 主控制链路已经切到二进制短帧；正确、错误、状态和故障都只按二进制帧判断。 |
| 适用链路 | STM32MP157 视觉主控 `<->` STM32F407 实时执行主控。 |
| 当前版本 | `V1.0` |
| 更新时间 | `2026-07-02` |

## 1. 快速索引

| 要查什么 | 直接看哪一节 | 关键字 |
|---|---|---|
| 帧头、帧尾、CRC | [4. 帧格式](#4-帧格式) | `A5 5A`、`0x6B`、`CRC16-CCITT-FALSE` |
| 命令字总表 | [7. 命令字总表](#7-命令字总表) | `START_CYCLE`、`VISION_POS`、`WEIGHT_RESULT`、`LDC_RESULT` |
| 自动检测顺序 | [11. 自动检测状态机](#11-自动检测状态机) | `SCANNING`、`TRACKING`、`WEIGHING`、`LDC_TESTING` |
| 首页按钮语义 | [12. 首页开始暂停继续停止语义](#12-首页开始暂停继续停止语义) | `开始`、`暂停`、`继续`、`停止` |
| F4 解析方式 | [13. F4 接收状态机](#13-f4-接收状态机) | `WAIT_SOF0`、`READ_PAYLOAD`、`DISPATCH` |
| F4 电机资源 | [14. F4 电机串口和 Emm42 ID 分配](#14-f4-电机串口和-emm42-id-分配) | `UART4`、`USART6`、`addr=0x01/0x02/0x03` |
| 云端字段映射 | [15. 云端 sensor_context 映射](#15-云端-sensor_context-映射) | `f4_uart`、`weighing`、`ldc1614_eddy_current` |
| 文本调试入口边界 | [16. 与现有 ASCII 调试命令的关系](#16-与现有-ascii-调试命令的关系) | 串口助手维护入口，不作为 MP157 成功/失败判断依据 |

## 2. 设计目标

| 目标 | 说明 |
|---|---|
| 抗干扰 | 串口帧必须有帧头、长度、帧尾和 CRC，避免粘包、半包、误码后误执行。 |
| 可恢复 | 每帧带 `SEQ`，每个检测流程带 `cycle_id`，用于识别重复帧、旧流程残留帧和重发帧。 |
| 可调试 | MP157 主链路只发送和解析二进制帧；串口助手文本命令只作为离线维护入口，USART1 文本输出默认静默。 |
| 低负担 | 首版负载不使用浮点数，F4 只解析小端整数和枚举值。 |
| 明确分工 | MP157 负责视觉、模型、图片、云端上传和自动会话主控；F4 负责电机、机械臂桥接、称重、电感检测、安全停机和单件完成后自动启动下一轮传送带扫描。 |
| 连续自动 | 首页按下“开始”后进入连续自动检测会话；只要没有暂停、停止或故障，F4 每完成一件并上报重量/电感/CYCLE_DONE 后，自动重新启动传送带扫描下一件。 |

## 3. 系统角色分工

| 角色 | 职责 | 不建议承担的职责 |
|---|---|---|
| STM32MP157 | 采集摄像头画面、计算零件坐标、判断 ROI 是否居中、等待对焦、运行模型、保存图片和模型结果、汇总 F4 数据并上传云端。 | 不直接控制张大头步进电机，不直接读取 HX711 或 LDC1614。 |
| STM32F407 | 接收 MP157 指令、控制传送带和摄像头运动电机、桥接 ESP32 机械臂、读取 HX711 称重、读取 LDC1614 电感检测、上报检测结果和故障。 | 不运行视觉模型，不负责云端 HTTP/COS 上传。 |
| ESP32 机械臂 | 执行动作组，把零件依次放到称重模块、电感模块和最终位置。 | 不直接与云端通信，不直接做整机流程决策。 |
| 云端 | 接收图片、模型结果、重量、电感、F4 状态和检测记录。 | 不参与毫秒级运动控制。 |

## 4. 帧格式

二进制帧固定使用下面格式：

```text
SOF0 SOF1 VER CMD LEN SEQ_L SEQ_H PAYLOAD... CRC_L CRC_H EOF
```

| 偏移 | 字段 | 字节数 | 首版取值或说明 |
|---:|---|---:|---|
| `0` | `SOF0` | 1 | 固定 `0xA5`，用于快速定位帧头。 |
| `1` | `SOF1` | 1 | 固定 `0x5A`，降低误判帧头概率。 |
| `2` | `VER` | 1 | 协议版本，首版固定 `0x01`。 |
| `3` | `CMD` | 1 | 命令字，见 [7. 命令字总表](#7-命令字总表)。 |
| `4` | `LEN` | 1 | `PAYLOAD` 字节数，首版建议 `0~48`。 |
| `5` | `SEQ_L` | 1 | 发送方本地帧序号低字节。 |
| `6` | `SEQ_H` | 1 | 发送方本地帧序号高字节。 |
| `7` | `PAYLOAD` | N | 命令负载，所有多字节整数采用小端序。 |
| `7+LEN` | `CRC_L` | 1 | CRC16 低字节。 |
| `8+LEN` | `CRC_H` | 1 | CRC16 高字节。 |
| `9+LEN` | `EOF` | 1 | 固定 `0x6B`，与张大头 Emm42 常见帧尾习惯一致，便于人工观察。 |

| 项目 | 规则 |
|---|---|
| 最短帧长度 | `10` 字节，即 `LEN=0`。 |
| 推荐最大帧长度 | `58` 字节，即 `LEN<=48`，便于兼容 F4 现有 64 字节级接收缓存。 |
| CRC 覆盖范围 | 从 `VER` 到 `PAYLOAD`，也就是偏移 `2` 到 `6+LEN`。 |
| CRC 不覆盖 | `SOF0`、`SOF1`、`CRC_L`、`CRC_H`、`EOF`。 |
| CRC 算法 | `CRC16-CCITT-FALSE`，多项式 `0x1021`，初值 `0xFFFF`，无反射，输出不异或。 |

## 5. 字节序、单位和基础类型

| 类型 | 字节数 | 说明 |
|---|---:|---|
| `u8` | 1 | 无符号 8 位整数。 |
| `i8` | 1 | 有符号 8 位整数。 |
| `u16` | 2 | 无符号 16 位整数，小端序。 |
| `i16` | 2 | 有符号 16 位整数，小端序。 |
| `u32` | 4 | 无符号 32 位整数，小端序。 |
| `i32` | 4 | 有符号 32 位整数，小端序。 |

| 数据 | 单位 | 说明 |
|---|---|---|
| 图像坐标 | `px` | 原始相机画面坐标，不使用裁剪 ROI 后的坐标。 |
| 重量 | `mg` | 毫克整数，避免 F4 发送浮点数。 |
| 时间 | `ms` | 毫秒，通常是发送方从上电开始计数。 |
| 置信度 | `0~100` | 百分比整数，`91` 表示 `0.91`。 |
| LDC 原始码 | 原始寄存器值 | F4 按 LDC1614 读取结果上报，MP157 再转换成云端 JSON。 |

## 6. SEQ、auto_session_id 和 cycle_id 规则

| 字段 | 作用 | 规则 |
|---|---|---|
| `SEQ` | 解决串口帧级别的重复、丢失和 ACK 对应问题。 | MP157 和 F4 各自维护自己的发送序号，每发送一帧自增 1，溢出后从 `0` 继续。 |
| `auto_session_id` | 解决首页“开始”后连续自动检测会话归属问题。 | MP157 每按一次“开始”生成一个新的自动会话；首版为了不增加帧长，`START_CYCLE.cycle_id` 同时作为本次自动会话 ID 使用。 |
| `cycle_id` | 解决单个零件检测流程归属问题。 | 自动会话内每检测一个零件就递增一次，`VISION_POS`、`MODEL_READY`、`WEIGHT_RESULT`、`LDC_RESULT`、`CYCLE_DONE` 都带当前单件的 `cycle_id`。 |
| `ACK.acked_seq` | 表示确认了哪一帧。 | ACK 帧自身也有新的 `SEQ`，负载中再写被确认帧的 `acked_seq`。 |
| `NACK.rejected_seq` | 表示拒绝了哪一帧。 | CRC 错误时可能无法可信读取 `CMD/SEQ`，此时可只计数，不强制回 NACK。 |

连续自动检测的关键原则：

| 场景 | 处理 |
|---|---|
| 首页按“开始” | MP157 新建 `auto_session_id`，发送 `START_CYCLE`；F4 进入 `AUTO_RUN` 并开始扫描第一件。 |
| 自动会话内检测下一件 | F4 在上一件 `WEIGHT_RESULT`、`LDC_RESULT`、`CYCLE_DONE` 已发送并得到 ACK 后，自动启动传送带扫描；MP157 发现下一件后用新的 `cycle_id` 继续发 `VISION_POS`。 |
| F4 收到旧 `cycle_id` 的 `VISION_POS` | 丢弃，并回 `NACK error=CYCLE_MISMATCH`；如果是上一件延迟坐标，不能影响当前件。 |
| MP157 收到旧 `cycle_id` 的 `WEIGHT_RESULT` | 不用于当前云端记录，只保存为异常日志。 |
| 按下暂停后继续 | 保留 `auto_session_id` 和当前 `cycle_id`，继续当前件或当前安全阶段。 |
| 按下停止后重新开始 | MP157 必须生成新的 `auto_session_id` 和新的首件 `cycle_id`，不能复用已停止会话。 |

首版兼容说明：

| 项目 | 规则 |
|---|---|
| `START_CYCLE.cycle_id` | 继续保持 6 字节负载不变；它表示“自动会话启动时的首件 cycle_id”，同时被 F4 记录为当前 `auto_session_id` 的兼容值。 |
| 后续单件 `cycle_id` | MP157 在下一件首次进入视野时递增；F4 只接受当前待检测件的 `cycle_id`。如果 F4 主动进入下一轮扫描，可通过 `EVENT_REPORT event=NEXT_SCAN_STARTED` 告诉 MP157 当前期望的下一件编号。 |
| 后续扩展 | 如果后续需要同时显示会话编号和单件编号，可新增 `START_AUTO_SESSION 0x14`，负载显式包含 `auto_session_id` 和 `first_cycle_id`，不破坏首版 `START_CYCLE`。 |

## 7. 命令字总表

| CMD | 名称 | 方向 | 是否必须 ACK | 用途 |
|---:|---|---|---|---|
| `0x01` | `HELLO` | 双向 | 是 | 上电握手，确认协议版本和能力位。 |
| `0x02` | `HEARTBEAT` | MP157 -> F4 | 是 | 二进制心跳，成功返回 `ACK`，用于确认主链路在线。 |
| `0x10` | `START_CYCLE` | MP157 -> F4 | 是 | 首页按“开始”后启动连续自动检测会话，F4 启动传送带扫描第一件。 |
| `0x11` | `PAUSE_CYCLE` | MP157 -> F4 | 是 | 暂停当前自动会话，F4 尽量进入安全静止点。 |
| `0x12` | `RESUME_CYCLE` | MP157 -> F4 | 是 | 从暂停状态继续当前自动会话。 |
| `0x13` | `STOP_CYCLE` | MP157 -> F4 | 是 | 停止并作废当前自动会话。 |
| `0x20` | `VISION_POS` | MP157 -> F4 | 是 | 发送零件视觉坐标，F4 根据坐标控制传送带纠偏，成功返回 `ACK`。 |
| `0x21` | `VISION_LOST` | MP157 -> F4 | 是 | 视觉目标丢失或置信度不足，F4 回扫描或停机后返回 `ACK`。 |
| `0x22` | `BELT_STOP_CENTERED` | MP157 -> F4 | 是 | MP157 判断零件已进入中心 ROI，要求 F4 停传送带。 |
| `0x30` | `MODEL_READY` | MP157 -> F4 | 是 | MP157 模型检测完成，先把结果暂存在 F4/流程上下文里。 |
| `0x31` | `ARM_JOB_START` | MP157 -> F4 | 是 | 要求 F4 通过 ESP32 机械臂执行称重、电感检测和分拣动作组。 |
| `0x40` | `QUERY_STATUS` | MP157 -> F4 | 否 | 查询 F4 协议状态和传送带结构化状态，成功直接返回 `STATUS_REPORT`。 |
| `0x41` | `BELT_MANUAL_CONTROL` | MP157 -> F4 | 是 | 手动调试传送带扫描/停止，成功返回 `ACK`。 |
| `0x50` | `ACTUATOR_POS_MOVE` | MP157 -> F4 | 是 | 执行器相对位置运动，首版用于传送带、摄像头左右轴和摄像头上下轴。 |
| `0x51` | `ACTUATOR_STOP` | MP157 -> F4 | 是 | 停止指定执行器；`actuator=0xFF` 表示停止全部可停止执行器。 |
| `0x52` | `ACTUATOR_VEL_MOVE` | MP157 -> F4 | 是 | 手动调试连续速度运动，当前只允许传送带和摄像头左右轴，直到收到 `ACTUATOR_STOP`。 |
| `0x53` | `ACTUATOR_HOME` | MP157 -> F4 | 是 | 参数页把指定执行器当前位置设为新的零点；不做主动回零运动。 |
| `0x80` | `ACK` | 双向 | 否 | 确认命令已收到并被接受。 |
| `0x81` | `NACK` | 双向 | 否 | 拒绝命令，并说明错误原因。 |
| `0x82` | `STATUS_REPORT` | F4 -> MP157 | 否 | 上报 F4 当前状态、传送带状态、机械臂状态和故障位。 |
| `0x83` | `EVENT_REPORT` | F4 -> MP157 | 否 | 上报关键事件，例如已居中、已放到称重模块、已放到电感模块。 |
| `0x84` | `WEIGHT_RESULT` | F4 -> MP157 | 是 | 上报 HX711 称重结果。 |
| `0x85` | `LDC_RESULT` | F4 -> MP157 | 是 | 上报 LDC1614 电感检测结果。 |
| `0x86` | `CYCLE_DONE` | F4 -> MP157 | 是 | 当前单件 F4 侧动作和传感器检测流程完成；连续模式下 ACK 后 F4 自动扫描下一件。 |
| `0x87` | `FAULT_REPORT` | F4 -> MP157 | 是 | 上报故障，例如电机异常、传感器超时、机械臂失败。 |

说明：

| 规则 | 说明 |
|---|---|
| 当前调试阶段不使用弱 ACK | 为了现场排查“命令成功但电机不动”，已实现的 MP157->F4 控制命令成功都必须有二进制回包。 |
| 关键命令必须 ACK | `HEARTBEAT`、`START_CYCLE`、`PAUSE_CYCLE`、`RESUME_CYCLE`、`STOP_CYCLE`、`VISION_POS`、`VISION_LOST`、`BELT_STOP_CENTERED`、`ACTUATOR_POS_MOVE`、`ACTUATOR_STOP`、`ACTUATOR_VEL_MOVE`、`ACTUATOR_HOME`、`ARM_JOB_START` 必须等 ACK 或 NACK。 |
| 结果帧必须 ACK | `WEIGHT_RESULT`、`LDC_RESULT`、`CYCLE_DONE` 和 `FAULT_REPORT` 必须由 MP157 ACK，避免结果丢失。 |

## 7.1 当前调试阶段正确返回和错误返回

当前 MP157-F4 主链路不再解析 `[OK]`、`[ERROR]`、`READY`、`BELTINFO` 这类文本。所有能让 Qt 显示成功或失败的依据都来自下面二进制帧。

| 场景 | F4 返回帧 | 负载长度 | MP157 处理规则 |
|---|---|---:|---|
| `HELLO/HEARTBEAT` 成功 | `ACK 0x80` | 7 | `acked_seq` 和 `acked_cmd` 匹配，`status=0`，F4 显示接入。 |
| `START_CYCLE/PAUSE_CYCLE/RESUME_CYCLE/STOP_CYCLE/VISION_POS/VISION_LOST/BELT_STOP_CENTERED/BELT_MANUAL_CONTROL/ACTUATOR_POS_MOVE/ACTUATOR_STOP/ACTUATOR_VEL_MOVE/ACTUATOR_HOME` 成功 | `ACK 0x80` | 7 | `cycle_id`、`acked_seq`、`acked_cmd` 匹配，`status=0`，Qt 才推进本地自动流程、手动页或参数页状态；F4 不允许把 `actuator` 填进 ACK `status`，否则左右轴会显示 `status=1`、上下轴会显示 `status=2` 并被 Qt 判定失败。 |
| `QUERY_STATUS` 成功 | `STATUS_REPORT 0x82` | 24 | `cycle_id`、`replied_seq`、`replied_cmd=QUERY_STATUS` 匹配，Qt 显示 F4 状态、传送带模式、速度、误差和故障位。 |
| 命令不支持、CRC 错、长度错、字段越界、状态不允许、cycle 不匹配、硬件队列未就绪 | `NACK 0x81` | 9 | Qt 显示 `error_code/state/detail`，不再查找文本中的 `ERROR`。 |
| LDC 未接、称重异常、传送带不可用、摄像头电机异常、机械臂链路异常等模块故障 | `FAULT_REPORT 0x87` | 16 | Qt 解析 `fault_source/severity/fault_code/detail_i32/fault_bits`，作为故障提示和云端 `sensor_context` 来源。 |

当前称重标定弹窗仍保留 UI 输入，但 C++ 不再写 `CAL <克重>\r\n` 文本；它暂用 `MODEL_READY 0x30` 作为二进制占位命令。F4 首版未实现该命令时，正确表现是返回 `NACK ERR_CMD_UNKNOWN`，这比文本超时更容易定位。

## 8. 枚举定义

### 8.1 零件类型 part_type

| 数值 | 名称 | 中文含义 | 备注 |
|---:|---|---|---|
| `0` | `PART_UNKNOWN` | 未知零件 | 只做定位时使用。 |
| `1` | `PART_FLAT_WASHER` | 平垫圈 | 可映射到 `washer`。 |
| `2` | `PART_SPLIT_WASHER` | 弹性垫圈 | 可映射到 `splitwasher`。 |
| `3` | `PART_WAVE_WASHER` | 波形垫圈 | 可兼容历史训练标签 `gasket`。 |
| `4` | `PART_OTHER` | 其他 | 预留。 |

### 8.2 模型结果 model_result

| 数值 | 名称 | 含义 |
|---:|---|---|
| `0` | `MODEL_UNKNOWN` | 模型未运行或结果无效。 |
| `1` | `MODEL_GOOD` | 模型判定合格。 |
| `2` | `MODEL_BAD` | 模型判定不合格。 |
| `3` | `MODEL_UNCERTAIN` | 模型置信度不足，需要人工或云端复核。 |

### 8.3 传感器判定 decision

| 数值 | 名称 | 含义 |
|---:|---|---|
| `0` | `DECISION_UNKNOWN` | 未判定。 |
| `1` | `DECISION_PASS` | 通过。 |
| `2` | `DECISION_FAIL` | 不通过。 |
| `3` | `DECISION_UNCERTAIN` | 不确定。 |
| `4` | `DECISION_SENSOR_ERROR` | 传感器异常。 |

### 8.4 F4 主状态 state

| 数值 | 名称 | 含义 |
|---:|---|---|
| `0` | `IDLE` | 空闲，等待开始。 |
| `1` | `SCANNING` | 传送带低速扫描，等待零件进入相机视野。 |
| `2` | `TRACKING` | 根据 MP157 视觉坐标控制传送带纠偏。 |
| `3` | `CENTERED_HOLD` | 零件已居中，传送带停止，等待 MP157 对焦和模型检测。 |
| `4` | `WAIT_MODEL` | F4 等待 MP157 下发模型结果或机械臂任务。 |
| `5` | `ARM_PICKING` | F4 已通知 ESP32 机械臂取件。 |
| `6` | `WEIGHING` | 零件已放到称重模块，F4 正在读取 HX711。 |
| `7` | `LDC_TESTING` | 零件已放到电感模块，F4 正在读取 LDC1614。 |
| `8` | `SORTING` | 机械臂根据结果放置零件。 |
| `9` | `DONE` | 本轮 F4 动作完成。 |
| `10` | `PAUSED` | 已暂停，保留当前 `cycle_id` 和阶段上下文。 |
| `11` | `STOPPED` | 已停止，本轮流程作废。 |
| `12` | `FAULT` | 故障，需要人工处理或上位机清故障。 |

### 8.5 NACK 错误码

| 数值 | 名称 | 含义 |
|---:|---|---|
| `1` | `ERR_CRC` | CRC 校验失败。 |
| `2` | `ERR_FRAME_LENGTH` | 长度字段或实际帧长度错误。 |
| `3` | `ERR_CMD_UNKNOWN` | 命令字不支持。 |
| `4` | `ERR_PAYLOAD_LENGTH` | 负载长度和命令定义不一致。 |
| `5` | `ERR_FIELD_RANGE` | 负载字段越界，例如坐标为负或速度超限。 |
| `6` | `ERR_STATE_NOT_ALLOWED` | 当前状态不允许执行该命令。 |
| `7` | `ERR_BUSY` | F4 正忙，例如机械臂动作中暂不接受暂停。 |
| `8` | `ERR_CYCLE_MISMATCH` | `cycle_id` 不匹配。 |
| `9` | `ERR_TIMEOUT` | 等待电机、机械臂或传感器超时。 |
| `10` | `ERR_HARDWARE_FAULT` | 底层硬件故障。 |

## 9. 负载结构详细表

下面表格中的字段顺序就是实际字节顺序。所有 `u16/i16/u32/i32` 都是小端序。

### 9.1 HELLO `0x01`

| 字段 | 类型 | 说明 |
|---|---|---|
| `role` | `u8` | `1=MP157`，`2=F4`。 |
| `proto_min` | `u8` | 支持的最低协议版本。 |
| `proto_max` | `u8` | 支持的最高协议版本。 |
| `feature_bits` | `u32` | 能力位，见下表。 |
| `build_id` | `u16` | 固件或应用构建编号，调试阶段可填 `0`。 |

| bit | 能力 | 说明 |
|---:|---|---|
| `0` | `FEATURE_BINARY_PROTOCOL` | 支持本文档二进制协议。 |
| `1` | `FEATURE_ACK_NACK` | 支持 ACK/NACK。 |
| `2` | `FEATURE_STATUS_REPORT` | 支持状态上报。 |
| `3` | `FEATURE_CONVEYOR` | 支持传送带电机控制。 |
| `4` | `FEATURE_ARM_BRIDGE` | 支持 ESP32 机械臂桥接。 |
| `5` | `FEATURE_WEIGHT` | 支持 HX711 称重。 |
| `6` | `FEATURE_LDC1614` | 支持 LDC1614 电感检测。 |
| `7` | `FEATURE_CAMERA_MOTORS` | 支持摄像头前进和上下电机。 |

### 9.2 HEARTBEAT `0x02`

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 当前流程 ID，没有流程时填 `0`。 |
| `state` | `u8` | 当前主状态。 |
| `fault_bits` | `u16` | 故障位图，没有故障填 `0`。 |
| `uptime_ms` | `u32` | 发送方上电运行毫秒数。 |

### 9.3 START_CYCLE `0x10`

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | MP157 新建的首件流程 ID；在连续自动模式下，它也是本次自动会话的兼容启动 ID。 |
| `mode` | `u8` | `0=连续完整自动检测`，`1=只跑传送带居中单步调试`，`2=只跑机械臂和传感器单步调试`。 |
| `option_bits` | `u16` | 启用项位图，首版 `bit0=启用称重`，`bit1=启用电感`，`bit2=启用分拣`。 |
| `camera_profile` | `u8` | 摄像头位置方案编号，调试阶段填 `0`。 |

F4 接收后的动作：

| 条件 | 动作 |
|---|---|
| 当前为 `IDLE/STOPPED/DONE` 且 `mode=0` | ACK 后进入连续 `AUTO_RUN/SCANNING`，启动传送带低速扫描第一件。 |
| 当前为 `IDLE/STOPPED/DONE` 且 `mode=1/2` | ACK 后只执行指定调试子流程，子流程结束后停在 `DONE`，不自动扫描下一件。 |
| 当前为 `PAUSED` | 返回 `NACK ERR_STATE_NOT_ALLOWED`，因为继续当前流程应使用 `RESUME_CYCLE`。 |
| 当前为运动或检测中 | 返回 `NACK ERR_BUSY`，避免新流程覆盖正在执行的流程。 |

### 9.4 PAUSE_CYCLE `0x11`

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 要暂停的流程 ID。 |
| `pause_reason` | `u8` | `0=用户按下暂停`，`1=视觉不稳定`，`2=上位机调试`。 |
| `pause_mode` | `u8` | `0=安全点暂停`，`1=立即停止可停止的执行器`。 |

F4 接收后的动作：

| 当前状态 | 建议动作 |
|---|---|
| `SCANNING/TRACKING/CENTERED_HOLD/WAIT_MODEL` | 立即停传送带和摄像头运动电机，保存自动会话、当前 `cycle_id` 和阶段，进入 `PAUSED`。 |
| `WEIGHING/LDC_TESTING` | 当前传感器读取完成后进入 `PAUSED`，避免半次采样导致结果无效。 |
| `ARM_PICKING/SORTING` | 如果 ESP32 支持暂停，则进入安全点暂停；如果不支持，返回 `NACK ERR_BUSY`。 |

### 9.5 RESUME_CYCLE `0x12`

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 要继续的流程 ID。 |
| `resume_mode` | `u8` | `0=从暂停点继续`，`1=回到扫描阶段继续`。 |

F4 接收后的动作：

| 条件 | 动作 |
|---|---|
| 当前为 `PAUSED` 且 `cycle_id` 匹配 | ACK 后恢复暂停前阶段；如果暂停前正在扫描，则重新启动传送带扫描；如果暂停前正在等待模型或机械臂，则不重新输送当前件。 |
| 当前不是 `PAUSED` | 返回 `NACK ERR_STATE_NOT_ALLOWED`。 |
| `cycle_id` 不匹配 | 返回 `NACK ERR_CYCLE_MISMATCH`。 |

### 9.6 STOP_CYCLE `0x13`

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 要停止的当前流程 ID；连续自动模式下表示停止整个自动会话。 |
| `stop_reason` | `u8` | `0=用户按下停止`，`1=视觉异常`，`2=云端/上位机取消`，`3=安全故障`。 |
| `stop_level` | `u8` | `0=正常中止`，`1=急停级停止`。 |

F4 接收后的动作：

| 动作 | 说明 |
|---|---|
| 停传送带 | 必须立即执行。 |
| 停摄像头运动电机 | 必须立即执行。 |
| 通知 ESP32 | 如果机械臂支持中止或回安全位，F4 应转发中止动作组。 |
| 清本轮上下文 | 当前 `cycle_id` 和自动会话作废，重量、电感和模型临时结果不再作为有效检测记录。 |
| 上报事件 | 回 ACK 后再发 `EVENT_REPORT event=STOPPED`。 |

### 9.7 VISION_POS `0x20`

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 当前检测流程 ID。 |
| `frame_id` | `u16` | MP157 视觉帧编号。 |
| `flags` | `u8` | `bit0=坐标有效`，`bit1=已进入 ROI`，`bit2=分类有效`，`bit3=多目标`，`bit4=对焦完成`。 |
| `part_type` | `u8` | 零件类型，未分类时填 `PART_UNKNOWN`。 |
| `axis_px` | `i16` | 沿传送带运动方向的当前坐标。 |
| `target_px` | `i16` | 希望对准的目标线坐标。 |
| `center_x_px` | `i16` | 原图中零件中心 X 坐标。 |
| `center_y_px` | `i16` | 原图中零件中心 Y 坐标。 |
| `bbox_x_px` | `i16` | 原图中检测框左上角 X。 |
| `bbox_y_px` | `i16` | 原图中检测框左上角 Y。 |
| `bbox_w_px` | `i16` | 检测框宽度。 |
| `bbox_h_px` | `i16` | 检测框高度。 |
| `confidence` | `u8` | 定位或综合置信度，范围 `0~100`。 |
| `reserved` | `u8` | 保留，首版填 `0`。 |
| `capture_ms` | `u32` | MP157 采集该帧时的毫秒计数。 |

F4 等价控制逻辑：

```text
error_px = axis_px - target_px
如果 abs(error_px) <= 中心死区，则停止传送带并进入居中状态
否则根据 error_px 的正负和绝对值控制张大头 Emm42 速度和方向
```

### 9.8 VISION_LOST `0x21`

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 当前检测流程 ID。 |
| `frame_id` | `u16` | MP157 视觉帧编号。 |
| `reason` | `u8` | `1=未找到目标`，`2=多目标混淆`，`3=置信度过低`，`4=相机离线`。 |
| `confidence` | `u8` | 当前视觉置信度，范围 `0~100`。 |
| `ms_since_seen` | `u16` | 距离上一次看到目标的时间。 |

F4 建议动作：

| reason | F4 行为 |
|---:|---|
| `1` | 如果仍在扫描阶段，继续低速扫描；如果在跟踪阶段超过超时，停止或回扫描。 |
| `2` | 停止传送带，等待 MP157 重新确认目标，避免跟错零件。 |
| `3` | 降低动作幅度或回扫描，等待下一帧。 |
| `4` | 立即停止并上报 `FAULT_REPORT`。 |

### 9.9 BELT_STOP_CENTERED `0x22`

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 当前检测流程 ID。 |
| `frame_id` | `u16` | 触发停止的视觉帧编号。 |
| `reason` | `u8` | `0=进入中心 ROI`，`1=MP157 主动要求停机拍照`。 |
| `hold_ms` | `u16` | 建议保持静止等待时间，首版建议 `2000`。 |
| `reserved` | `u8` | 保留，首版填 `0`。 |

F4 接收后必须停传送带，并上报：

| 上报 | 说明 |
|---|---|
| `ACK` | 表示停止命令已接受。 |
| `EVENT_REPORT event=TARGET_CENTERED` | 表示目标已居中。 |
| `EVENT_REPORT event=BELT_STOPPED` | 表示传送带已下发停止命令。 |

### 9.10 MODEL_READY `0x30`

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 当前检测流程 ID。 |
| `model_result` | `u8` | 模型判定结果，见 [8.2 模型结果 model_result](#82-模型结果-model_result)。 |
| `part_type` | `u8` | 模型确认后的零件类型。 |
| `defect_type` | `u8` | 缺陷类型编号，首版可先用 `0=未知`，具体字符串由 MP157 保存。 |
| `top1_confidence` | `u8` | top1 置信度，范围 `0~100`。 |
| `image_seq` | `u16` | MP157 本地图片或检测记录序号。 |
| `model_ms` | `u16` | 模型推理和后处理耗时。 |
| `option_bits` | `u16` | `bit0=需要称重`，`bit1=需要电感`，`bit2=需要分拣`。 |

说明：

| 规则 | 说明 |
|---|---|
| 模型结果先不上传 | MP157 在本地保存图片和模型结果，等 F4 回传重量、电感后一起上传云端。 |
| F4 不解析缺陷字符串 | F4 只保存枚举和置信度，用于状态上报和机械臂动作选择。 |
| 低置信度处理 | 如果 `model_result=MODEL_UNCERTAIN`，F4 仍可执行称重和电感检测，最终云端记录标记为待复核。 |

### 9.11 ARM_JOB_START `0x31`

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 当前检测流程 ID。 |
| `job_id` | `u16` | MP157 分配的机械臂任务 ID。 |
| `job_profile` | `u8` | `0=完整检测动作组`，`1=只称重`，`2=只电感`，`3=只分拣`。 |
| `part_type` | `u8` | 零件类型，用于 F4 选择动作组。 |
| `final_bin_hint` | `u8` | `0=未知`，`1=合格区`，`2=不合格区`，`3=复核区`。 |
| `option_bits` | `u16` | `bit0=称重`，`bit1=电感`，`bit2=分拣后回零`。 |

F4 接收后的动作：

| 步骤 | 动作 |
|---:|---|
| 1 | ACK `ARM_JOB_START`。 |
| 2 | 通过 F4 与 ESP32 的机械臂串口发送取件动作组。 |
| 3 | ESP32 返回“已放到称重模块”后，F4 上报 `EVENT_REPORT event=ARM_WEIGHT_PLACED`。 |
| 4 | F4 自动读取 HX711，稳定后上报 `WEIGHT_RESULT`。 |
| 5 | F4 通知 ESP32 把零件放到 LDC1614 电感模块。 |
| 6 | ESP32 返回“已放到电感模块”后，F4 上报 `EVENT_REPORT event=ARM_LDC_PLACED`。 |
| 7 | F4 自动读取 LDC1614，完成后上报 `LDC_RESULT`。 |
| 8 | F4 通知 ESP32 放到最终位置，完成后上报 `CYCLE_DONE`。 |

### 9.12 ACTUATOR_POS_MOVE `0x50`

该命令用于 MP157 让 F4 按“相对位置模式”移动指定执行器。首版只定义 `mode=0`，也就是相对位置移动；F4 收到后把命令转换成张大头 Emm42 `0xFD` 位置模式帧。

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 当前自动流程 ID；手动调试时可填 `0`。 |
| `actuator` | `u8` | `0=传送带`，`1=摄像头左右轴`，`2=摄像头上下轴`。 |
| `direction` | `u8` | 传送带 `0=后退/1=前进`，左右轴 `0=左移/1=右移`，上下轴 `0=下降/1=上升`；具体正反方向由 F4 运行时映射到电机方向。 |
| `mode` | `u8` | 首版只支持 `0=相对位置模式`；其它值 F4 必须回 `NACK ERR_PARAM_RANGE`。 |
| `speed_rpm` | `u16` | 本次位置运动速度，单位 rpm；F4 需要按电机服务限幅。 |
| `steps` | `u32` | 本次相对移动步数，范围 `0~4294967295 step`；`0` 表示不移动，应回 `NACK` 或忽略并明确回包。 |
| `flags` | `u8` | 保留位，首版填 `0`。 |

使用边界：

| 场景 | 处理 |
|---|---|
| 首页自动流程 Z 轴下探 | `actuator=2`，`direction=0`，`steps=z_down_fixed_steps`，收到 ACK 后 MP157 复查 ROI。 |
| 首页自动流程前后/Y 方向微调 | `actuator=0`，按 ROI 的 `errorY` 选择 `direction=0/1`，`steps=min_step`，由传送带做短步前后补偿，每次动作后重新读取 `LOCATE`。 |
| 首页自动流程左右/X 方向微调 | `actuator=1`，按 ROI 的 `errorX` 选择 `direction=0/1`，`steps=min_step`，由摄像头左右轴做短步补偿，每次动作后重新读取 `LOCATE`。 |
| 首页模型检测后 Z 轴回升 | `actuator=2`，`direction=1`，`steps=z_up_fixed_steps`，由 MP157 在模型检测完成回调中自动发送。 |
| 手动上下轴弹窗 | `actuator=2`，下降按钮使用 `zDownFixedSteps`，上升按钮使用 `zUpFixedSteps`；每次点击只发一次高层位置命令，不在 MP157 直接拼 Emm42 帧。 |

### 9.13 ACTUATOR_STOP `0x51`

该命令用于停止指定执行器。它和首页停止流程不同，`STOP_CYCLE` 会作废自动流程，`ACTUATOR_STOP` 只处理运动执行器停机。

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 当前自动流程 ID；手动调试和模拟急停时可填 `0`。 |
| `actuator` | `u8` | `0=传送带`，`1=摄像头左右轴`，`2=摄像头上下轴`，`0xFF=全部可停止执行器`。 |
| `flags` | `u8` | 保留位，首版填 `0`。 |

使用边界：

| 场景 | 处理 |
|---|---|
| 手动页停止某一轴 | MP157 填对应 `actuator`，F4 只停该执行器。 |
| 模拟急停 | MP157 填 `actuator=0xFF`，F4 停传送带、摄像头左右轴和摄像头上下轴。 |
| 自动流程停止 | 仍优先使用 `STOP_CYCLE` 作废流程；`ACTUATOR_STOP` 只作为运动层补充制动。 |

### 9.14 ACTUATOR_VEL_MOVE `0x52`

该命令用于 MP157 手动调试时让执行器按速度模式持续运行。它不带步数，F4 收到后必须保持速度运动，直到 MP157 再发送 `ACTUATOR_STOP`。

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 当前自动流程 ID；手动调试时可填 `0`。 |
| `actuator` | `u8` | 当前只允许 `0=传送带`、`1=摄像头左右轴`；`2=摄像头上下轴` 必须回 `NACK ERR_PARAM_RANGE`。 |
| `direction` | `u8` | 传送带 `0=后退/1=前进`，左右轴 `0=左移/1=右移`；具体正反方向由 F4 运行时映射到电机方向。 |
| `speed_rpm` | `u16` | 持续运动速度，单位 rpm，范围 `1~5000`。 |
| `flags` | `u8` | 保留位，首版填 `0`。 |

使用边界：

| 场景 | 处理 |
|---|---|
| 手动传送带 | 正反按钮发送 `ACTUATOR_VEL_MOVE actuator=0 direction=0/1`，停止按钮发送 `ACTUATOR_STOP actuator=0`。 |
| 手动左右轴 | 左移/右移按钮发送 `ACTUATOR_VEL_MOVE actuator=1 direction=0/1`，停止按钮发送 `ACTUATOR_STOP actuator=1`。 |
| 手动上下轴 | 不使用速度连续运动；下降/上升按钮仍使用 `ACTUATOR_POS_MOVE actuator=2` 和对应固定步数。 |

### 9.15 ACTUATOR_HOME `0x53`

该命令用于参数设置页把指定执行器当前位置设为新的零点。F4 收到后先停止目标轴，再发送 Emm42 当前位置清零命令；它不会让电机主动运动，也不会寻找限位。

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 当前自动流程 ID；参数页标定时可填 `0`。 |
| `actuator` | `u8` | `0=传送带`，`1=摄像头左右轴`，`2=摄像头上下轴`；不支持 `0xFF` 全部设零。 |
| `flags` | `u8` | 保留位，首版填 `0`。 |

使用边界：

| 场景 | 处理 |
|---|---|
| 参数页设零 | MP157 在当前电机页发送 `ACTUATOR_HOME actuator=<当前页>`，F4 停止该轴并把当前位置设为零点。 |
| 自动流程 | 不使用该命令；自动流程仍通过固定相对步数下降、回升和微调。 |
| 限位回零 | 尚未实现；后续如加限位开关，应新增专门的回零状态机，不能把当前位置设零误当成找限位回零。 |

### 9.16 ACK `0x80`

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 被确认命令所属流程，没有流程时填 `0`。 |
| `acked_seq` | `u16` | 被确认帧的 `SEQ`。 |
| `acked_cmd` | `u8` | 被确认帧的 `CMD`。 |
| `status` | `u8` | `0=已接受并执行`，`1=重复帧已忽略但状态正常`。MP157 首页只有 `status=0` 才能显示为新的自动流程成功；`status=1` 必须提示重复帧，不能当成传送带已经重新启动。 |
| `state` | `u8` | 发送 ACK 一方的当前主状态。 |

### 9.17 NACK `0x81`

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 被拒绝命令所属流程，没有流程时填 `0`。 |
| `rejected_seq` | `u16` | 被拒绝帧的 `SEQ`。 |
| `rejected_cmd` | `u8` | 被拒绝帧的 `CMD`。 |
| `error_code` | `u8` | 错误码，见 [8.5 NACK 错误码](#85-nack-错误码)。 |
| `state` | `u8` | 发送 NACK 一方的当前主状态。 |
| `detail` | `u16` | 额外错误信息，首版没有时填 `0`。 |

### 9.18 STATUS_REPORT `0x82`

当前 F407 首轮实现的 `STATUS_REPORT` 负载固定为 24 字节，字段如下：

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 查询命令携带的流程 ID，整体查询可填 `0`。 |
| `replied_seq` | `u16` | 被回复的 `QUERY_STATUS` 帧序号。 |
| `replied_cmd` | `u8` | 固定回显 `0x40`，表示回复的是 `QUERY_STATUS`。 |
| `f4_state` | `u8` | F4 当前主状态，见 [8.4 F4 主状态 state](#84-f4-主状态-state)。 |
| `active_cycle_id` | `u16` | F4 当前正在处理的流程 ID，没有流程时为 `0`。 |
| `paused_state` | `u8` | 暂停前主状态；未暂停时为 `0`。 |
| `belt_desired` | `u8` | 传送带期望模式。 |
| `belt_applied` | `u8` | 传送带实际已应用模式。 |
| `belt_dir` | `u8` | `0=CW`，`1=CCW`。 |
| `belt_centered` | `u8` | `1=已进入中心死区`。 |
| `belt_stable` | `u8` | 连续进入死区的稳定计数。 |
| `belt_speed_rpm` | `u16` | 当前下发给传送带 Emm42 的目标速度。 |
| `belt_error_px` | `i32` | 最近一次视觉误差，单位像素。 |
| `feature_bits` | `u16` | F4 当前协议能力位，首版 bit0 表示基础二进制协议可用。 |
| `fault_bits` | `u16` | 当前模块故障位图，例如 LDC 未接、传送带状态不可读。 |

首轮实现没有把机械臂、称重、电感子状态塞进 `STATUS_REPORT`。这些模块的即时故障统一用 `FAULT_REPORT`，最终结果后续用 `WEIGHT_RESULT/LDC_RESULT/CYCLE_DONE`。

### 9.19 EVENT_REPORT `0x83`

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 当前流程 ID。 |
| `event_code` | `u8` | 事件编号，见下表。 |
| `state` | `u8` | 事件发生后的 F4 主状态。 |
| `source` | `u8` | `1=传送带`，`2=机械臂`，`3=称重`，`4=电感`，`5=用户控制`。 |
| `value_i32` | `i32` | 事件附带有符号值，例如误差或重量，未使用填 `0`。 |
| `value_u16` | `u16` | 事件附带无符号值，例如速度或耗时，未使用填 `0`。 |
| `related_seq` | `u16` | 触发该事件的命令序号，没有则填 `0`。 |
| `sender_ms` | `u32` | F4 发送事件时的毫秒计数。 |

| event_code | 名称 | 含义 |
|---:|---|---|
| `0x01` | `EVENT_F4_READY` | F4 已就绪。 |
| `0x10` | `EVENT_SCAN_STARTED` | 传送带已开始扫描。 |
| `0x11` | `EVENT_TARGET_TRACKING` | F4 已进入视觉跟踪。 |
| `0x12` | `EVENT_TARGET_CENTERED` | 零件已进入中心 ROI。 |
| `0x13` | `EVENT_BELT_STOPPED` | 传送带已停止。 |
| `0x20` | `EVENT_ARM_PICK_DONE` | 机械臂已取到零件。 |
| `0x21` | `EVENT_ARM_WEIGHT_PLACED` | 机械臂已把零件放到称重模块。 |
| `0x22` | `EVENT_ARM_LDC_PLACED` | 机械臂已把零件放到电感模块。 |
| `0x23` | `EVENT_ARM_SORT_DONE` | 机械臂已完成分拣或放置。 |
| `0x30` | `EVENT_WEIGHT_DONE` | 称重完成。 |
| `0x31` | `EVENT_LDC_DONE` | 电感检测完成。 |
| `0x40` | `EVENT_PAUSED` | 流程已暂停。 |
| `0x41` | `EVENT_RESUMED` | 流程已继续。 |
| `0x42` | `EVENT_STOPPED` | 流程已停止。 |
| `0xF0` | `EVENT_WARNING` | 非致命告警。 |
| `0xF1` | `EVENT_FAULT` | 故障事件。 |

### 9.20 WEIGHT_RESULT `0x84`

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 当前流程 ID。 |
| `sample_id` | `u16` | 本次称重采样编号。 |
| `stable` | `u8` | `1=已稳定`，`0=未稳定或超时取值`。 |
| `decision` | `u8` | 判定结果，见 [8.3 传感器判定 decision](#83-传感器判定-decision)。 |
| `gross_weight_mg` | `i32` | 毛重，单位 mg。 |
| `net_weight_mg` | `i32` | 净重，单位 mg。 |
| `raw_adc` | `i32` | HX711 原始 ADC 值。 |
| `sample_count` | `u16` | 参与稳定判断的采样数量。 |
| `stable_window_mg` | `u16` | 稳定窗口，单位 mg。 |
| `duration_ms` | `u16` | 本次称重耗时。 |
| `sender_ms` | `u32` | F4 发送结果时的毫秒计数。 |

### 9.21 LDC_RESULT `0x85`

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 当前流程 ID。 |
| `sample_id` | `u16` | 本次电感采样编号。 |
| `ch_count` | `u8` | 通道数量，首版建议填 `4`。 |
| `decision` | `u8` | 综合判定结果。 |
| `status_bits` | `u16` | LDC1614 状态位或错误位。 |
| `ch0_raw` | `u32` | 通道 0 原始码。 |
| `ch0_delta` | `i32` | 通道 0 相对基线差值。 |
| `ch1_raw` | `u32` | 通道 1 原始码。 |
| `ch1_delta` | `i32` | 通道 1 相对基线差值。 |
| `ch2_raw` | `u32` | 通道 2 原始码。 |
| `ch2_delta` | `i32` | 通道 2 相对基线差值。 |
| `ch3_raw` | `u32` | 通道 3 原始码。 |
| `ch3_delta` | `i32` | 通道 3 相对基线差值。 |
| `sender_ms` | `u32` | F4 发送结果时的毫秒计数。 |

说明：

| 规则 | 说明 |
|---|---|
| 不在 F4 帧内放 JSON | F4 只发紧凑整数，MP157 收到后再组装云端 JSON。 |
| 基线由 F4 管理 | `delta` 是相对当前 F4 基线的差值，方便云端判断稳定性。 |
| 后续扩展 | 如果需要频率、等效电感、电阻等详细字段，可新增 `LDC_DETAIL_RESULT`，不要把首版帧撑得过长。 |

### 9.22 CYCLE_DONE `0x86`

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 当前流程 ID。 |
| `result_flags` | `u16` | 完成位图，`bit0=称重完成`，`bit1=电感完成`，`bit2=机械臂完成`，`bit3=分拣完成`。 |
| `final_state` | `u8` | F4 最终状态，通常为 `DONE`。 |
| `model_result` | `u8` | MP157 之前下发的模型结果回显。 |
| `weight_decision` | `u8` | 称重判定结果。 |
| `ldc_decision` | `u8` | 电感判定结果。 |
| `total_ms` | `u32` | 当前单件从首次进入扫描/跟踪到 F4 动作完成的耗时，不包含云端上传时间。 |
| `f4_fault_bits` | `u16` | 完成时的故障位图。 |

连续自动模式下的完成规则：

| 条件 | F4 后续动作 |
|---|---|
| `WEIGHT_RESULT` 已发送且收到 MP157 `ACK` | F4 标记当前件称重数据已被 MP157 接收。 |
| `LDC_RESULT` 已发送且收到 MP157 `ACK` | F4 标记当前件电感数据已被 MP157 接收。 |
| `CYCLE_DONE` 已发送且收到 MP157 `ACK`，并且 `START_CYCLE.mode=0` | F4 不等待云端上传完成，自动调用传送带扫描，进入下一件 `SCANNING`。 |
| `CYCLE_DONE` 已发送但 ACK 超时 | F4 按结果帧重发策略重发；超过最大次数后停在 `FAULT` 或 `DONE_HOLD`，不能直接丢结果进入下一件。 |
| 当前自动会话已暂停或停止 | F4 不启动下一件扫描。 |

### 9.23 FAULT_REPORT `0x87`

当前 F407 首轮实现的 `FAULT_REPORT` 负载固定为 16 字节，字段如下：

| 字段 | 类型 | 说明 |
|---|---|---|
| `cycle_id` | `u16` | 当前流程 ID，没有流程时填 `0`。 |
| `fault_code` | `u16` | 故障码。 |
| `fault_source` | `u8` | `1=串口`，`2=传送带`，`3=摄像头运动电机`，`4=机械臂`，`5=称重`，`6=电感`。 |
| `severity` | `u8` | `1=提示`，`2=告警`，`3=需要停机`。 |
| `state` | `u8` | 故障发生时的 F4 主状态。 |
| `reserved` | `u8` | 保留字段，首版填 `0`。 |
| `detail_i32` | `i32` | 故障附加值，例如错误码、超时时间或传感器原始值。 |
| `related_seq` | `u16` | 触发故障的命令序号，没有则填 `0`。 |
| `fault_bits` | `u16` | 故障位图快照，例如 `LDC_NOT_READY`。 |

## 10. ACK、重发和超时策略

| 项目 | 建议值 | 说明 |
|---|---:|---|
| 视觉坐标发送周期 | `50~100 ms` | 必须短于 F4 视觉跟踪超时时间。 |
| F4 视觉跟踪超时 | `200~300 ms` | 超过该时间没有新坐标时，F4 停止跟踪或回扫描。 |
| 关键命令 ACK 超时 | `50 ms` | MP157 等不到 ACK 时可以重发。 |
| 关键命令最大重发 | `2` 次 | 超过后进入通信故障处理，避免命令堆积。 |
| 结果帧 ACK 超时 | `100 ms` | F4 等不到 MP157 ACK 时重发结果帧。 |
| 结果帧最大重发 | `3` 次 | 仍失败则 F4 保留结果并上报通信故障。 |

| 场景 | 处理策略 |
|---|---|
| `VISION_POS` 丢失 | 允许丢帧，下一帧新坐标会覆盖旧坐标。 |
| `VISION_POS` 重复 | F4 按 `SEQ` 识别重复帧，重复帧不重复更新控制输出。 |
| `START_CYCLE` 重复 | 如果是同一个 `cycle_id` 且 F4 不在 `PAUSED`，当前 F4 固件重新投递一次传送带 `SCAN` 并回 `ACK status=0`；如果 MP157 收到 `ACK status=1`，应提示重复帧未重新执行，不能显示为新的启动成功。 |
| `BELT_STOP_CENTERED` 丢失 | MP157 必须重发，直到收到 ACK 或 F4 状态已是 `CENTERED_HOLD`。 |
| `WEIGHT_RESULT` 丢失 | F4 重发，MP157 用 `sample_id` 去重。 |
| `LDC_RESULT` 丢失 | F4 重发，MP157 用 `sample_id` 去重。 |

## 11. 自动检测状态机

推荐首版完整流程如下。这里的“完整自动检测”不是只跑一件，而是首页按“开始”后进入连续自动会话，直到用户按“暂停/停止”或发生故障。

| 步骤 | MP157 动作 | F4 动作 | 关键帧 |
|---:|---|---|---|
| 1 | 用户按首页“开始”，MP157 创建新的自动会话和首件 `cycle_id`。 | F4 准备进入连续自动扫描。 | `START_CYCLE mode=0` -> `ACK` |
| 2 | MP157 采集摄像头画面，等待零件进入视野。 | F4 启动传送带低速扫描。 | `EVENT_SCAN_STARTED` |
| 3 | MP157 发现零件，周期发送坐标。 | F4 根据坐标控制传送带调速。 | `VISION_POS` |
| 4 | MP157 发现零件进入中心 ROI，发送居中停机。 | F4 停止传送带并保持。 | `BELT_STOP_CENTERED` -> `EVENT_TARGET_CENTERED` |
| 5 | MP157 下发摄像头上下轴下降固定步数，收到下降 ACK 后等待约 `3s` 让摄像头对焦稳定，再重新读取 ROI；若 `errorY` 仍偏离中心，则下发传送带短步前后微调；若 `errorX` 仍偏离中心，则下发摄像头左右轴短步微调。 | F4 用 Emm42 位置模式移动 `addr=0x02` 上下轴、`addr=0x03` 左右轴和 `addr=0x01` 传送带，每条命令都返回 ACK/NACK。 | `ACTUATOR_POS_MOVE` -> `ACK` |
| 6 | ROI 复查通过后，MP157 运行模型检测；检测完成后自动下发上下轴回升固定步数。 | F4 保持传送带静止，执行 Z 轴回升位置命令，等待模型结果或机械臂任务。 | `MODEL_READY` / `ACTUATOR_POS_MOVE` |
| 7 | MP157 下发机械臂检测任务。 | F4 通过 ESP32 控制机械臂取件。 | `ARM_JOB_START` |
| 8 | MP157 等待重量。 | F4 收到 ESP32 “已放到称重模块”后自动读取 HX711。 | `EVENT_ARM_WEIGHT_PLACED` -> `WEIGHT_RESULT` |
| 9 | MP157 等待电感。 | F4 收到 ESP32 “已放到电感模块”后自动读取 LDC1614。 | `EVENT_ARM_LDC_PLACED` -> `LDC_RESULT` |
| 10 | MP157 等待 F4 完成动作。 | F4 让机械臂放到最终位置并结束流程。 | `CYCLE_DONE` |
| 11 | MP157 把图片、模型、重量、电感和 F4 状态写入本地记录，随后异步上传云端。 | F4 在 `WEIGHT_RESULT/LDC_RESULT/CYCLE_DONE` 都被 ACK 后，自动启动传送带扫描下一件。 | `ACK CYCLE_DONE` -> `EVENT_NEXT_SCAN_STARTED` |

状态流转建议：

```text
IDLE
  -> START_CYCLE(mode=0)
AUTO_RUN / SCANNING
  -> VISION_POS
TRACKING
  -> BELT_STOP_CENTERED
CENTERED_HOLD
  -> MODEL_READY
WAIT_MODEL
  -> ARM_JOB_START
ARM_PICKING
  -> ARM_WEIGHT_PLACED
WEIGHING
  -> WEIGHT_RESULT
ARM_PICKING
  -> ARM_LDC_PLACED
LDC_TESTING
  -> LDC_RESULT
SORTING
  -> CYCLE_DONE
DONE_ONE_PART
  -> 如果 mode=0 且结果已 ACK，则自动回到 SCANNING
  -> 如果 mode=1/2 或收到 STOP，则进入 DONE/STOPPED
```

连续自动循环要点：

| 规则 | 说明 |
|---|---|
| F4 自动启动下一轮 | 单件完成后由 F4 自己启动传送带扫描，不要求 MP157 再发一次 `START_CYCLE`。 |
| MP157 仍负责识别下一件 | F4 只负责把下一件送入视野；下一件的坐标、ROI 是否居中、是否停止传送带仍由 MP157 视觉结果决定。 |
| 云端上传不能阻塞传送带 | MP157 收到 F4 结果后先落本地缓存；网络上传可以异步进行，不能让 F4 等 HTTP/COS 完成。 |
| 暂停优先级高于自动下一轮 | 如果用户在 `CYCLE_DONE` 前后按暂停，F4 不能自动启动下一轮，必须停在安全状态。 |
| 停止优先级最高 | 任意阶段收到 `STOP_CYCLE` 后立即停传送带和摄像头运动电机，当前自动会话作废。 |

## 12. 首页开始暂停继续停止语义

首页四个按钮必须和 F4 状态机严格对应。按下“开始”后进入连续自动检测，不需要用户为每一个零件再次点击开始。

| 当前 UI 状态 | 按“开始” | 按“暂停” | 按“继续” | 按“停止” |
|---|---|---|---|---|
| `空闲` | 新建自动会话和首件 `cycle_id`，发送 `START_CYCLE mode=0`。 | 禁用或无动作。 | 禁用或无动作。 | 禁用或无动作。 |
| `运行中-扫描/跟踪` | 禁用；如果强制点击则提示正在连续检测。 | 发送 `PAUSE_CYCLE`，F4 停传送带和摄像头运动电机并保存阶段。 | 禁用或无动作。 | 发送 `STOP_CYCLE`，作废整个自动会话。 |
| `运行中-对焦/模型` | 禁用。 | MP157 暂停后续动作；如果模型已开始，允许模型跑完但不下发 `ARM_JOB_START`。 | 禁用。 | 发送 `STOP_CYCLE`，本地图像和模型结果标记为 aborted。 |
| `运行中-机械臂/传感器` | 禁用。 | 首版建议只允许“请求暂停”，F4 在安全点暂停；如果机械臂不支持暂停则返回 `NACK ERR_BUSY`。 | 禁用。 | 发送 `STOP_CYCLE`，F4 停可停止执行器并通知 ESP32 中止或回安全位。 |
| `运行中-单件完成准备下一件` | 禁用。 | 发送 `PAUSE_CYCLE`，F4 不启动下一轮扫描。 | 禁用。 | 发送 `STOP_CYCLE`，阻止下一件进入视野。 |
| `已暂停` | 重新开始：先发 `STOP_CYCLE` 作废旧自动会话，再新建自动会话发 `START_CYCLE mode=0`。 | 禁用或无动作。 | 发送 `RESUME_CYCLE`，继续同一个自动会话和当前 `cycle_id`。 | 发送 `STOP_CYCLE`，作废暂停中的自动会话。 |
| `已停止` | 新建自动会话和首件 `cycle_id`，发送 `START_CYCLE mode=0`。 | 禁用或无动作。 | 禁用；停止后的流程不能继续。 | 禁用或无动作。 |
| `已完成` | 连续自动模式通常不会停在已完成；若单步调试完成，可新建 `cycle_id` 发送 `START_CYCLE`。 | 禁用或无动作。 | 禁用或无动作。 | 禁用或无动作。 |
| `故障` | 如果故障未清除则禁用；故障清除后可开始新流程。 | 禁用。 | 禁用。 | 可发送 `STOP_CYCLE` 确保全部停机。 |

关键结论：

| 操作 | 规则 |
|---|---|
| 暂停后按继续 | 继续同一个自动会话和当前 `cycle_id`，不丢弃当前图像、坐标和阶段上下文。 |
| 暂停后按开始 | 先作废旧自动会话，再重新开始新的连续检测。 |
| 停止后按继续 | 不允许，因为停止已经清除了自动会话和当前 `cycle_id`。 |
| 停止后按开始 | 开始一轮全新的连续自动检测。 |
| 运行中重复按开始 | UI 应禁用；协议层收到则返回 `NACK ERR_BUSY`。 |

## 13. F4 接收状态机

F4 的 USART1 接收入口必须先识别二进制帧。MP157 主链路只发送 `A5 5A ... 6B` 二进制帧；ASCII 文本命令只允许作为断开 MP157 后的串口助手维护入口。

| 状态 | 动作 | 异常处理 |
|---|---|---|
| `WAIT_SOF0` | 等待 `0xA5`。 | MP157 主链路收到其他字节直接丢弃或计数；维护模式才允许交给 ASCII 命令。 |
| `WAIT_SOF1` | 等待 `0x5A`。 | 如果不是 `0x5A`，回到 `WAIT_SOF0`。 |
| `READ_HEADER` | 读取 `VER/CMD/LEN/SEQ_L/SEQ_H`。 | `LEN>48` 直接丢弃并计数。 |
| `READ_PAYLOAD` | 按 `LEN` 读取负载。 | 超时则丢弃半帧。 |
| `READ_CRC` | 读取 `CRC_L/CRC_H`。 | 字节不足则等待，超时丢弃。 |
| `READ_EOF` | 检查 `0x6B`。 | 帧尾错误时丢弃，并从当前字节重新寻找帧头。 |
| `VERIFY` | 校验版本、长度、CRC 和字段范围。 | 失败则发 NACK 或只计数。 |
| `DISPATCH` | 根据 `CMD` 分发给自动流程、传送带、机械臂或传感器服务。 | 状态不允许时发 `NACK ERR_STATE_NOT_ALLOWED`。 |

解析顺序建议：

| 优先级 | 输入特征 | 处理 |
|---:|---|---|
| 1 | 以 `A5 5A` 开头 | 按本文档二进制协议解析。 |
| 2 | 以 `55 55` 开头 | 如果仍需要机械臂调试透传，可按现有 LeArm/ESP32 帧处理。 |
| 3 | 以 ASCII 字母开头 | 只在维护模式按文本命令解析；MP157 自动流程不得使用该入口。 |
| 4 | 其他 | 丢弃并记录简短错误计数。 |

## 14. F4 电机串口和 Emm42 ID 分配

F4 当前有 3 个张大头 Emm42 步进电机，其中摄像头两个电机共用一条串口，必须用不同地址区分，不能都保持默认地址 `0x01`。

| 执行器编号 | Emm42 地址 | F4 串口 | F4 引脚 | 设备 | 控制原则 |
|---:|---:|---|---|---|---|
| `0` | `0x01` | `UART4` | `PC10(TX) / PC11(RX)` | 传送带电机 | 负责把零件送入相机视野，并根据 `VISION_POS.axis_px-target_px` 做主要对中。 |
| `1` | `0x03` | `USART6` | `PC6(TX) / PC7(RX)` | 摄像头左右电机 | 用于 Z 轴下降后 ROI 的 X 方向左右微调，不能替代传送带前后输送。 |
| `2` | `0x02` | `USART6` | `PC6(TX) / PC7(RX)` | 摄像头上下电机 | 用于高度或焦距标定，首版自动流程按固定下探/回升步数动作。 |

硬件和代码必须同步满足下面规则：

| 规则 | 说明 |
|---|---|
| 传送带必须绑定 `huart4` | 现有 F4 代码若仍把传送带绑定 `huart6`，需要改成 `EMM42_MotorLoadDefaultConfig(&motor, &huart4)`，否则会和摄像头电机串口冲突。 |
| 摄像头两个电机必须不同地址 | 当前现场左右电机设为 `0x03`，上下电机设为 `0x02`；设置地址时建议只接一个电机上电，设置完成后贴标签，避免两个电机同时响应同一命令。 |
| `USART6` 上禁止广播运动命令 | 摄像头两个电机共线，广播速度/停止/回零可能导致两个轴一起动作；自动流程只允许按地址单独控制。 |
| F4 驱动层保持通用 | `emm42_motor.c/.h` 只保存 `UART_HandleTypeDef *huart + address`，不要写死“传送带=USART6”。 |
| 应拆分服务模块 | 传送带继续放在 `conveyor_motor_service.c/.h`；摄像头左右/上下建议放在 `camera_motor_service.c/.h`，内部维护现场地址 `0x03` 和 `0x02` 两个句柄。 |

视觉对中时的执行器分工：

| 偏差情况 | 优先动作 | 说明 |
|---|---|---|
| 零件还没有进入相机视野 | F4 只运行传送带扫描。 | MP157 不发送有效 `VISION_POS`，或发送 `VISION_LOST reason=1`。 |
| 零件已进入视野但离 ROI 中心较远 | F4 主要控制传送带。 | 使用 `axis_px-target_px` 决定传送带方向和速度。 |
| 零件已经接近 ROI，但 Y 方向仍有偏差 | F4 可短步点动传送带。 | 传送带承担前后方向微调，每次点动后等待 MP157 下一帧坐标确认，不能连续盲动。 |
| 零件已经接近 ROI，但 X 方向仍有偏差 | F4 可短步点动摄像头左右轴。 | 左右轴只承担横向微调，每次点动后等待 MP157 下一帧坐标确认，不能连续盲动。 |
| 摄像头左右点动后画面 X 坐标不再改善 | F4 或 MP157 标记该轴达到有效行程边界。 | 停止继续尝试同方向左右对齐，改用传送带 Y 方向微调或接受当前偏差进入复核。 |
| ROI 居中后需要降低零件高度进行模型检测 | MP157 用 `ACTUATOR_POS_MOVE actuator=ACT_CAMERA_Z direction=DOWN` 让上下轴下降固定步数。 | 下降 ACK 后必须等待约 3 秒让摄像头对焦稳定，再重新读取 ROI；检测完成后再用 `direction=UP` 回升固定步数。 |

摄像头左右轴“到边界”的判断建议：

| 判断输入 | 边界判定 |
|---|---|
| F4 已向 `addr=0x03` 下发左移/右移短步命令。 | 记录动作方向、动作时间、动作前 X 方向视觉误差。 |
| MP157 后续 `N` 帧坐标中 `abs(errorX_px)` 没有减小。 | 认为这次左右轴微调无效，累计一次无效计数。 |
| 连续 `2~3` 次同方向微调无效，或电机驱动返回异常/超时。 | 标记该方向已到有效边界，本轮不再继续该方向微调。 |
| 后续反方向微调恢复有效。 | 可清除该方向边界标记，但必须限制最大尝试次数，避免来回振荡。 |

协议中可使用的执行器编号：

| 设备编号 | 设备 | 建议用途 |
|---:|---|---|
| `0` | `ACT_CONVEYOR` | 传送带张大头步进电机。 |
| `1` | `ACT_CAMERA_LATERAL` | 摄像头左右电机；协议编号保持 `1`，旧代码里的 `ACT_CAMERA_FORWARD` 只作为兼容别名。 |
| `2` | `ACT_CAMERA_Z` | 摄像头上/下电机。 |
| `0xFF` | `ACT_ALL` | 仅用于 `ACTUATOR_STOP`，表示全部可停止执行器。 |

当前执行器扩展命令：

| CMD | 名称 | 方向 | 用途 |
|---:|---|---|---|
| `0x50` | `ACTUATOR_POS_MOVE` | MP157 -> F4 | 按相对位置模式移动传送带、摄像头左右轴或摄像头上下轴。 |
| `0x51` | `ACTUATOR_STOP` | MP157 -> F4 | 停止某个执行器，或用 `actuator=0xFF` 停止全部可停止执行器。 |
| `0x52` | `ACTUATOR_VEL_MOVE` | MP157 -> F4 | 手动速度模式持续运动，当前只用于传送带和摄像头左右轴。 |
| `0x53` | `ACTUATOR_HOME` | MP157 -> F4 | 参数页把当前位置设为新的零点，不主动运动。 |

首版自动流程已经使用 `ACTUATOR_POS_MOVE` 完成“居中停机 -> 上下轴下降 -> 等待约 3 秒对焦稳定 -> ROI 复查 -> 传送带前后微调或左右轴微调 -> 模型检测 -> 上下轴回升”。手动调试中，传送带和左右轴用 `ACTUATOR_VEL_MOVE` 持续运动；上下轴用 `ACTUATOR_POS_MOVE` 的固定下探/回升步数，避免无限升降。

## 15. 云端 sensor_context 映射

MP157 收到 F4 的二进制结果后，再组装云端 JSON。F4 不直接拼 JSON。

| 云端字段 | 来源帧 | 映射方式 |
|---|---|---|
| `sensor_context.f4_uart.last_frame_seq` | 任意 F4 帧头 `SEQ` | MP157 保存最近一次收到的 F4 `SEQ`。 |
| `sensor_context.f4_uart.last_frame_crc_ok` | MP157 CRC 校验结果 | 最近一帧 CRC 正确填 `true`。 |
| `sensor_context.f4_uart.status` | `HEARTBEAT/STATUS_REPORT/FAULT_REPORT` | 正常填 `ok`，连续超时填 `timeout`，故障填 `fault`。 |
| `sensor_context.weighing.raw_adc` | `WEIGHT_RESULT.raw_adc` | 直接写入。 |
| `sensor_context.weighing.gross_weight_g` | `WEIGHT_RESULT.gross_weight_mg` | 除以 `1000.0`。 |
| `sensor_context.weighing.net_weight_g` | `WEIGHT_RESULT.net_weight_mg` | 除以 `1000.0`。 |
| `sensor_context.weighing.stable` | `WEIGHT_RESULT.stable` | `1` 转成 `true`。 |
| `sensor_context.weighing.decision` | `WEIGHT_RESULT.decision` | 映射为 `pass/fail/uncertain/sensor_error`。 |
| `sensor_context.ldc1614_eddy_current.channels[].raw_code` | `LDC_RESULT.chN_raw` | 按通道写入。 |
| `sensor_context.ldc1614_eddy_current.channels[].delta_raw_code` | `LDC_RESULT.chN_delta` | 按通道写入。 |
| `sensor_context.ldc1614_eddy_current.overall_decision` | `LDC_RESULT.decision` | 映射为云端枚举或字符串。 |
| `sensor_context.conveyor_motor.running` | `STATUS_REPORT.conveyor_mode/speed_rpm` | `speed_rpm>0` 且模式非停止时为 `true`。 |
| `sensor_context.conveyor_motor.direction` | `STATUS_REPORT.direction` | `0=CW`，`1=CCW`，MP157 可再映射为 `forward/reverse`。 |
| `sensor_context.conveyor_motor.target_rpm` | `STATUS_REPORT.speed_rpm` | 当前 F4 下发目标转速。 |
| `sensor_context.conveyor_motor.uart_device` | F4 硬件资源表或本协议固定配置 | 当前传送带填写 `UART4`。 |
| `sensor_context.conveyor_motor.uart_tx_gpio` | F4 硬件资源表或本协议固定配置 | 当前传送带填写 `PC10`。 |
| `sensor_context.conveyor_motor.uart_rx_gpio` | F4 硬件资源表或本协议固定配置 | 当前传送带填写 `PC11`。 |
| `sensor_context.conveyor_motor.slave_address` | F4 Emm42 地址配置 | 当前传送带填写 `1`。 |
| `sensor_context.camera_motion.lateral_motor_address` | F4 Emm42 地址配置 | 摄像头左右电机填写 `3`，串口为 `USART6 PC6/PC7`；旧字段 `forward_motor_address` 只可作为兼容字段。 |
| `sensor_context.camera_motion.z_motor_address` | F4 Emm42 地址配置 | 摄像头上下电机填写 `2`，串口为 `USART6 PC6/PC7`。 |
| `sensor_context.f4_control_state.last_command` | MP157 最近发送命令 | 例如 `START_CYCLE`、`BELT_STOP_CENTERED`、`ARM_JOB_START`。 |
| `sensor_context.f4_control_state.last_command_seq` | MP157 最近发送命令 `SEQ` | 用于云端排查串口时序。 |
| `sensor_context.f4_control_state.alarm_code` | `FAULT_REPORT.fault_code` | 无故障填 `null`。 |

## 16. 与现有 ASCII 调试命令的关系

| 现有命令 | 是否允许 MP157 自动流程使用 | 二进制替代 |
|---|---|---|
| `STATUS` | 不允许 | `HEARTBEAT 0x02` 成功回 `ACK`；状态详情用 `QUERY_STATUS 0x40` 成功回 `STATUS_REPORT`。 |
| `BELTSCAN` | 不允许 | `BELT_MANUAL_CONTROL action=1` 或自动流程 `START_CYCLE`。 |
| `BELTSTOP` | 不允许 | `BELT_MANUAL_CONTROL action=0` 或自动流程 `STOP_CYCLE/BELT_STOP_CENTERED`。 |
| `BELTTRACK <error>` | 不允许 | `VISION_POS`，F4 根据 `axis_px-target_px` 计算误差。 |
| `BELTINFO` | 不允许 | `QUERY_STATUS 0x40`，F4 返回 `STATUS_REPORT 0x82`。 |
| `GET` | 不允许 | 正式流程由 `WEIGHT_RESULT` 主动上报。 |
| `CAL <克重>` | 不允许 | 当前 Qt 只发二进制占位命令，后续需要新增称重标定二进制命令；正式链路不能发送 `CAL ...\r\n`。 |
| `LDCSTOP/LDCCAL` | 不允许 | 自动流程只上报检测结果和 `FAULT_REPORT`；标定后续需单独定义二进制维护命令。 |

调试阶段特别注意：

| 项目 | 要求 |
|---|---|
| 上电自动扫描 | 如果希望只有 MP157 首页按“开始”后传送带才动，F4 `CONVEYOR_MOTOR_STARTUP_SCAN_ENABLE` 应改为 `0`。 |
| 传送带串口 | `START_CYCLE/VISION_POS/BELT_MANUAL_CONTROL/ACTUATOR_POS_MOVE actuator=0` 必须走 F4 `UART4 PC10/PC11` 控制传送带 Emm42 地址 `0x01`，不能继续走 `USART6`。 |
| 摄像头电机串口 | `USART6 PC6/PC7` 同时挂现场左右轴 `addr=0x03`、上下轴 `addr=0x02`，调试时先单独用 `ACTUATOR_POS_MOVE` 小步确认不会两个轴同时动。 |
| 自动流程 | ASCII 命令只用于单模块调试；正式连续自动流程以二进制 `START_CYCLE/VISION_POS/BELT_STOP_CENTERED/ACTUATOR_POS_MOVE/CYCLE_DONE` 为准。 |

## 17. 调试和验证建议

| 测试目标 | 执行位置 | 命令或方法 | 预期现象 | 失败时排查 |
|---|---|---|---|---|
| 查找本文档 | Windows 仓库根目录 | `Select-String -Path docs/stm32mp157-f407-binary-protocol.md -Pattern "VISION_POS"` | 能定位到视觉坐标帧定义。 | 检查文件路径是否正确。 |
| 验证帧头识别 | MP157 串口工具 | 发送以 `A5 5A` 开头的合法帧。 | F4 进入二进制解析并 ACK。 | 查波特率、线序、GND、DMA 接收和 CRC。 |
| 验证 CRC 错误处理 | MP157 串口工具 | 故意改错 CRC。 | F4 不执行动作，并记录 CRC 错误或回 NACK。 | 检查 CRC 覆盖范围是否一致。 |
| 验证重复开始 | Qt 首页或 MP157 串口工具 | 在 F4 未清旧流程时再次发送同一 `cycle_id` 的 `START_CYCLE`。 | 新固件日志应出现 `START_CYCLE repeat rescan`，Qt 只有收到 `ACK status=0 state=SCANNING` 才显示成功。 | 如果 Qt 显示 `ACK重复帧`，说明 F4 仍按旧语义只回 `status=1`；如果 status=0 但电机不动，查 UART4 接线、电机地址和使能。 |
| 验证传送带方向 | MP157 或串口助手 | 发送 `VISION_POS axis_px > target_px` 和 `axis_px < target_px`。 | 传送带方向相反，误差进入死区后停止。 | 如果越调越远，反转 F4 误差到方向的映射。 |
| 验证传送带串口 | Qt 手动页或 MP157 二进制串口工具 | 发送 `BELT_MANUAL_CONTROL action=1` 或 `START_CYCLE`。 | `UART4 PC10/PC11` 上的传送带电机动作，`USART6` 摄像头电机不动，F4 返回 `ACK status=0`。 | 检查 `conveyor_motor_service.c` 是否仍绑定 `huart6`，再查 PC10/PC11 接线、共地、电机地址 `0x01`。 |
| 验证摄像头双 ID | 串口助手或 F4 调试命令 | 分别向 `USART6` 地址 `0x02` 和 `0x03` 发送点动。 | 上下轴和左右轴分别动作，互不影响。 | 检查两个电机是否都还是默认地址 `0x01`，或是否误用了广播命令。 |
| 验证手动连续运动 | Qt 手动三轴弹窗或 MP157 二进制串口工具 | 分别发送 `ACTUATOR_VEL_MOVE actuator=0/1 direction=0/1 speed_rpm>0`，再发送对应 `ACTUATOR_STOP`。 | 传送带和左右轴按方向持续运动，直到停止命令；F4 返回 `ACK status=0`，不能返回 `status=actuator`。 | 查 `binary_protocol_service.c` 分发、`conveyor_motor_service.c` JOG 模式、`camera_motor_service.c` JOG 命令和 Emm42 `0xF6` 速度模式帧。 |
| 验证上下轴固定步数 | Qt 手动三轴弹窗或 MP157 二进制串口工具 | 分别发送 `ACTUATOR_POS_MOVE actuator=2 direction=DOWN/UP steps=zDownFixedSteps/zUpFixedSteps`。 | 上下轴每点一次只移动对应固定步数，F4 返回 `ACK status=0`。 | 查 `zDownFixedSteps/zUpFixedSteps` 是否为 0、上下轴地址 `0x02`、方向映射和 Emm42 `0xFD` 位置模式帧。 |
| 验证当前位置设零 | Qt 参数设置页步进电机弹窗 | 切到任一电机页，点击 `设当前位置为零点`。 | MP157 发送 `ACTUATOR_HOME actuator=<当前页>`，F4 返回 `ACK status=0`，对应电机不运动但当前位置被设为零点。 | 查 `emm42_motor.c` 是否发送 `[addr 0A 6D 6B]`，并确认不是 `ACTUATOR_STOP actuator=0xFF` 或运动回零流程。 |
| 验证模拟急停 | Qt 手动页点击模拟急停 | MP157 发送 `ACTUATOR_STOP actuator=0xFF`。 | 传送带、左右轴、上下轴都收到停止动作，F4 返回 ACK 或明确 NACK。 | 查 `ACT_ALL` 分支、各服务停止函数和故障位。 |
| 验证 Z 轴下探/回升 | Qt 首页自动流程或参数页调小步数后实测 | 居中 ACK 后观察 `ACTUATOR_POS_MOVE actuator=2 direction=DOWN`，下降 ACK 后观察界面提示等待约 3 秒对焦稳定，模型检测完成后观察 `direction=UP`。 | 上下轴先下降固定步数，等待对焦稳定后才 ROI 复查和模型检测，检测完成后回升固定步数。 | 查 `zDownFixedSteps/zUpFixedSteps` 是否为 0、方向是否反、上下轴地址是否为 `0x02`，再查 QML `autoVisionZFocusSettleMs` 是否仍为 `3000`。 |
| 验证摄像头左右微调极限 | MP157 视觉闭环 | 在 ROI 附近让 F4 点动摄像头左右轴，再观察连续 X 坐标。 | X 坐标改善时继续小步；连续无改善后停止该方向微调。 | 查点动方向、相机坐标轴定义、机械行程和电机地址。 |
| 验证居中停止 | MP157 | 发送 `BELT_STOP_CENTERED hold_ms=2000`。 | F4 停传送带并上报 `EVENT_TARGET_CENTERED`。 | 查 ACK、F4 状态和 Emm42 停止命令。 |
| 验证称重结果 | F4 自动流程 | ESP32 放到称重模块后，F4 上报 `WEIGHT_RESULT`。 | MP157 得到 `net_weight_mg` 和 `stable=1`。 | 查 HX711 接线、去皮、稳定窗口和采样超时。 |
| 验证电感结果 | F4 自动流程 | ESP32 放到电感模块后，F4 上报 `LDC_RESULT`。 | MP157 得到 4 通道原始码和差值。 | 查 I2C、LDC1614 地址、基线和模块供电。 |
| 验证连续自动下一件 | Qt 首页和 F4 日志 | 首页按“开始”，完成一件后不再手动点击。 | MP157 ACK `CYCLE_DONE` 后，F4 自动启动传送带扫描下一件。 | 检查 F4 是否等待结果 ACK、是否误停在 `DONE`、是否被 UI 状态当成单次流程。 |
| 验证停止后不能继续 | Qt 首页 | 点击停止后再点击继续。 | UI 禁用继续或 F4 返回 `NACK ERR_STATE_NOT_ALLOWED`。 | 检查 UI 状态机是否错误保留旧 `cycle_id`。 |

## 18. CRC 参考实现说明

CRC 输入范围必须是：

```text
VER CMD LEN SEQ_L SEQ_H PAYLOAD...
```

C 语言参考逻辑如下，后续放到 F4 或 MP157 工程时需要按项目代码风格补充头文件和函数声明：

```c
/*
 * 函数作用：计算本文档协议使用的 CRC16-CCITT-FALSE。
 * 主要流程：先使用 0xFFFF 初始化 CRC，再逐字节把输入数据移入高 8 位，
 *          每一位根据最高位是否为 1 决定是否异或多项式 0x1021。
 * 参数 data：指向需要参与 CRC 的首字节，不能为 NULL。
 * 参数 len：需要参与 CRC 的字节数，协议中应覆盖 VER 到 PAYLOAD。
 * 返回值：16 位 CRC 结果，发送时低字节在前，高字节在后。
 */
uint16_t Protocol_Crc16CcittFalse(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFU;          /* 保存当前 CRC 累计值，初始值按 CCITT-FALSE 固定为 0xFFFF。 */
    uint16_t i = 0U;                 /* 外层循环下标，用于遍历每一个输入字节。 */
    uint8_t bit = 0U;                /* 内层循环下标，用于遍历当前字节的 8 个 bit。 */

    for (i = 0U; i < len; i++)       /* 逐字节处理 VER、CMD、LEN、SEQ 和 PAYLOAD。 */
    {
        crc ^= ((uint16_t)data[i] << 8);  /* 把当前字节放入 CRC 高 8 位，符合 CCITT 的计算方向。 */

        for (bit = 0U; bit < 8U; bit++)   /* 每个字节固定处理 8 次移位。 */
        {
            if ((crc & 0x8000U) != 0U)    /* 最高位为 1 表示本轮移位后需要异或多项式。 */
            {
                crc = (uint16_t)((crc << 1) ^ 0x1021U); /* 左移一位并异或 CCITT 多项式。 */
            }
            else                          /* 最高位为 0 表示只需要左移，不需要异或多项式。 */
            {
                crc = (uint16_t)(crc << 1);             /* 左移一位继续累计。 */
            }
        }
    }

    return crc;                       /* 返回最终 CRC，组帧时按低字节、高字节顺序发送。 */
}
```

## 19. 示例帧

假设 MP157 要发送 `START_CYCLE`：

| 字段 | 值 |
|---|---|
| `VER` | `0x01` |
| `CMD` | `0x10` |
| `LEN` | `0x06` |
| `SEQ` | `0x0034` |
| `cycle_id` | `0x0012` |
| `mode` | `0x00`，连续完整自动检测 |
| `option_bits` | `0x0007`，称重、电感、分拣都启用 |
| `camera_profile` | `0x00` |

不含 CRC 的帧主体为：

```text
A5 5A 01 10 06 34 00 12 00 00 07 00 00 CRC_L CRC_H 6B
```

说明：

| 字节段 | 含义 |
|---|---|
| `A5 5A` | 帧头。 |
| `01 10 06 34 00` | `VER=1`，`CMD=START_CYCLE`，`LEN=6`，`SEQ=0x0034`。 |
| `12 00 00 07 00 00` | `cycle_id=0x0012`，`mode=0` 表示连续完整自动检测，`option_bits=0x0007`，`camera_profile=0`。 |
| `CRC_L CRC_H` | 根据 `01 10 06 34 00 12 00 00 07 00 00` 计算。 |
| `6B` | 帧尾。 |

## 20. 后续实现文件建议

| 端 | 建议文件 | 职责 |
|---|---|---|
| F4 | `User/App/binary_protocol_service.c/.h` | 帧解析、CRC、ACK/NACK 组帧和命令分发。 |
| F4 | `User/App/auto_inspection_service.c/.h` | 保存 `cycle_id`、主状态机、暂停继续停止语义和自动检测流程。 |
| F4 | `User/App/conveyor_motor_service.c/.h` | 继续负责传送带 Emm42 控制；必须绑定 `huart4`，使用 Emm42 地址 `0x01`，新增从 `VISION_POS` 更新误差的入口。 |
| F4 | `User/App/camera_motor_service.c/.h` | 新增摄像头运动电机服务；绑定 `huart6`，当前现场左右轴地址 `0x03`，上下轴地址 `0x02`，提供点动、停止、边界状态接口。 |
| F4 | `User/App/robot_arm_service.c/.h` | 继续负责 F4 到 ESP32 机械臂桥接，新增动作组完成事件回调。 |
| F4 | `User/App/weight_service.c/.h` | 读取 HX711 后生成 `WEIGHT_RESULT`。 |
| F4 | `User/App/ldc1614_service.c/.h` | 读取 LDC1614 后生成 `LDC_RESULT`。 |
| MP157 | `20_uvc_camera/qt_camera_display` 内新增协议类 | 组帧、解帧、CRC、ACK 等待、重发和串口日志。 |
| MP157 | Qt 首页控制器 | 把“开始/暂停/继续/停止”映射到本文档的 `START_CYCLE/PAUSE_CYCLE/RESUME_CYCLE/STOP_CYCLE`。 |

首版实现建议：

| 优先级 | 内容 | 原因 |
|---:|---|---|
| 1 | 先修正 F4 硬件映射：传送带 `huart4/addr=0x01`，摄像头电机 `huart6` 上现场左右轴 `addr=0x03`、上下轴 `addr=0x02`。 | 先避免两个摄像头电机和传送带串口混用。 |
| 2 | 实现 `START_CYCLE`、`VISION_POS`、`BELT_STOP_CENTERED`、`STATUS_REPORT`。 | 先打通传送带居中闭环。 |
| 3 | 加入传送带前后短步微调、摄像头左右轴微调和极限判断。 | 解决 Z 轴下降后 ROI 附近的 X/Y 微小偏移，但避免到机械边界后继续硬顶。 |
| 4 | 实现 `MODEL_READY`、`ARM_JOB_START`、`WEIGHT_RESULT`、`LDC_RESULT`、`CYCLE_DONE`。 | 接机械臂和传感器链路，并让 F4 在结果 ACK 后自动启动下一件扫描。 |
| 5 | 完善 `PAUSE_CYCLE`、`RESUME_CYCLE`、`STOP_CYCLE` 在机械臂和传感器阶段的安全点边界。 | F4 首版已接入传送带阶段的暂停、继续和停止，后续还要结合 ESP32 机械臂安全点细调。 |
| 6 | 保留 ASCII 调试命令。 | 现场排查时串口助手能直接验证单模块。 |
