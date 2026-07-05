# F4 与 ESP32S3 机械臂对接执行手册

| 项目 | 内容 |
|---|---|
| 文档目的 | 让 ESP32S3 机械臂固件负责人可以直接按本文实现 F4 对接：F4 发什么、ESP32S3 立即回什么、动作完成后再回什么、F4 收到后进入哪一步。 |
| 适用链路 | STM32F407 USART3 `<->` ESP32S3 UART，首版固定 `115200 8N1`。 |
| 推荐协议 | 正式自动流程使用 `A5 5A VER CMD LEN SEQ PAYLOAD CRC 6B` 二进制协议。 |
| 参考实现 | `docs/f4_esp32s3_arm_protocol/arm_link_protocol.h`、`arm_link_protocol.c`、`f4_arm_link_commands.h`、`f4_arm_link_commands.c`。 |
| 当前提醒 | F4 工程里的 `robot_arm_service.c` 仍保留 `55 55` LeArm 动作组转发兼容逻辑；该逻辑没有可靠动作完成回包，不能作为最终自动检测闭环协议。 |

## 1. 总体原则

| 原则 | 说明 |
|---|---|
| F4 是主控 | F4 根据 MP157 的自动流程状态决定什么时候让 ESP32S3 抓取、搬运、分拣。 |
| ESP32S3 是执行器 | ESP32S3 只负责机械臂轨迹、夹爪、舵机、动作组、限位和本地安全判断。 |
| ACK 不等于完成 | ESP32S3 收到合法命令后立即回 `ARM_ACK`，只表示命令已接受。 |
| DONE 才是真完成 | ESP32S3 必须在零件真实放到目标位置后回 `ARM_STAGE_DONE result=OK`。 |
| FAIL 必须上报 | 抓取失败、动作超时、舵机故障、急停、限位触发时，ESP32S3 必须回 `ARM_STAGE_DONE result!=OK` 或 `ARM_FAULT_REPORT`。 |
| F4 收到 DONE 后再测量 | F4 只有收到“放到称重模块完成”才读取 HX711，收到“放到电磁感应模块完成”才读取 LDC1614。 |
| 上传失败走待复核 | MP157 上传失败时会把本轮结果判为待复核，F4 最终通知 ESP32S3 放到待复核盘。 |

## 2. 硬件连接

| 信号 | STM32F407 | ESP32S3 | 要求 |
|---|---|---|---|
| F4 TX | `USART3_TX`，当前工程注释为 `PD8` | ESP32S3 RX | F4 发命令给 ESP32S3。 |
| F4 RX | `USART3_RX`，当前工程注释为 `PD9` | ESP32S3 TX | ESP32S3 回 ACK、DONE、NACK、FAULT。 |
| GND | GND | GND | 必须共地，否则串口会随机乱码或完全不通。 |
| 电平 | 3.3V TTL | 3.3V TTL | 禁止直接接 RS232 电平。 |
| 波特率 | 115200 | 115200 | 数据位 8，停止位 1，无校验。 |

> 如果实物接线不是 `PD8/PD9`，必须同步修改 F4 CubeMX/代码注释、ESP32S3 UART 初始化、本文硬件表和联调记录。

## 3. 正式帧格式

```text
A5 5A VER CMD LEN SEQ_L SEQ_H PAYLOAD... CRC_L CRC_H 6B
```

| 字段 | 字节数 | 取值/规则 |
|---|---:|---|
| `A5 5A` | 2 | 固定帧头。 |
| `VER` | 1 | 当前为 `0x01`。 |
| `CMD` | 1 | 命令字，见第 4 节。 |
| `LEN` | 1 | payload 字节数，当前最大 `48`。 |
| `SEQ_L/SEQ_H` | 2 | 发送方序号，小端序，每发一帧递增。 |
| `PAYLOAD` | N | 业务数据，所有 `u16` 都是小端序。 |
| `CRC_L/CRC_H` | 2 | `CRC16-CCITT-FALSE`，覆盖 `VER` 到 `PAYLOAD`。 |
| `6B` | 1 | 固定帧尾。 |

CRC 参数：

| 参数 | 取值 |
|---|---|
| 算法 | `CRC16-CCITT-FALSE` |
| 初值 | `0xFFFF` |
| 多项式 | `0x1021` |
| 输入反转 | 否 |
| 输出反转 | 否 |
| 结果异或 | `0x0000` |
| 字节序 | 帧内低字节在前，即 `CRC_L CRC_H` |

## 4. 命令总表

| CMD | 名称 | 方向 | ESP32S3 接收后必须做什么 |
|---:|---|---|---|
| `0x01` | `ARM_LINK_HELLO` | 双向 | 回 `ARM_ACK`，payload 可为空或按后续扩展能力返回。 |
| `0x02` | `ARM_LINK_HEARTBEAT` | 双向 | 回 `ARM_ACK`，用于确认链路在线。 |
| `0x20` | `ARM_MOVE_TO_WEIGHT` | F4 -> ESP32S3 | 先回 `ARM_ACK`，再从 ROI/传送带抓取零件并放到称重模块，完成后回 `ARM_STAGE_DONE stage=1`。 |
| `0x21` | `ARM_MOVE_TO_LDC` | F4 -> ESP32S3 | 先回 `ARM_ACK`，再从称重模块抓取零件并放到电磁感应模块，完成后回 `ARM_STAGE_DONE stage=2`。 |
| `0x22` | `ARM_SORT_RESULT` | F4 -> ESP32S3 | 先回 `ARM_ACK`，再从电磁感应模块抓取零件并放到指定盘，完成后回 `ARM_STAGE_DONE stage=3`。 |
| `0x23` | `ARM_HOME` | F4 -> ESP32S3 | 先回 `ARM_ACK`，回安全初始位，完成后回 `ARM_STAGE_DONE stage=4`。 |
| `0x24` | `ARM_STOP` | F4 -> ESP32S3 | 立即优先处理，停止或进入安全位，回 `ARM_STAGE_DONE stage=5` 或 `ARM_FAULT_REPORT`。 |
| `0x30` | `ARM_STAGE_DONE` | ESP32S3 -> F4 | 动作阶段真正完成或失败后发送。 |
| `0x31` | `ARM_STAGE_REPORT` | ESP32S3 -> F4 | 可选进度上报，例如已夹住、移动中、已放下。首版 F4 可不依赖它推进流程。 |
| `0x32` | `ARM_JOB_DONE` | ESP32S3 -> F4 | 可选整套任务完成。首版推荐不用，避免和阶段 DONE 混淆。 |
| `0x80` | `ARM_ACK` | 双向 | 命令合法并已接受。 |
| `0x81` | `ARM_NACK` | 双向 | 命令非法、状态不允许、正忙、payload 错误等。 |
| `0x87` | `ARM_FAULT_REPORT` | ESP32S3 -> F4 | 机械臂主动故障上报。 |

## 5. 阶段和分拣枚举

### 5.1 `stage_id`

| stage_id | 名称 | 机械臂实际动作 |
|---:|---|---|
| `0` | `NONE` | 无动作。 |
| `1` | `PICK_BELT_TO_WEIGHT` | 从 ROI/传送带取件，放到称重模块。 |
| `2` | `WEIGHT_TO_LDC` | 从称重模块取件，放到电磁感应模块。 |
| `3` | `LDC_TO_SORT_BIN` | 从电磁感应模块取件，放到最终盘。 |
| `4` | `HOME` | 回安全初始位。 |
| `5` | `STOP_SAFE` | 停止动作并进入安全状态。 |

### 5.2 `target_bin`

| target_bin | 名称 | 位置 |
|---:|---|---|
| `0` | `NONE` | 非分拣动作填 0。 |
| `1` | `GOOD` | 良品盘。 |
| `2` | `BAD` | 不良品盘。 |
| `3` | `REVIEW` | 待复核盘。 |

### 5.3 `model_result`

| model_result | 含义 | 默认分拣目标 |
|---:|---|---|
| `0` | 未知或未收到模型结果 | 待复核盘。 |
| `1` | 模型判定良品 | 良品盘。 |
| `2` | 模型判定不良品 | 不良品盘。 |
| `3` | 模型不确定、人工复核、上传失败 | 待复核盘。 |

## 6. Payload 定义

### 6.1 F4 发给 ESP32S3 的动作命令 payload

`ARM_MOVE_TO_WEIGHT`、`ARM_MOVE_TO_LDC`、`ARM_SORT_RESULT`、`ARM_HOME`、`ARM_STOP` 共用 12 字节 payload。

| 偏移 | 字段 | 类型 | F4 填法 | ESP32S3 用法 |
|---:|---|---|---|---|
| `0` | `cycle_id` | `u16` | MP157/F4 当前单件流程号。 | 回 ACK、DONE、NACK 时原样带回。 |
| `2` | `stage_id` | `u8` | 对应第 5.1 节。 | 决定执行哪个动作阶段。 |
| `3` | `part_type` | `u8` | 零件类型，未知填 `0`。 | 可选，用于选择不同夹取轨迹。 |
| `4` | `model_result` | `u8` | `0/1/2/3`。 | 分拣阶段可辅助选择动作；最终盘以 `target_bin` 为准。 |
| `5` | `target_bin` | `u8` | 称重/电感填 `0`，分拣填 `1/2/3`。 | `ARM_SORT_RESULT` 必须按该字段放盘。 |
| `6` | `timeout_ms` | `u16` | F4 允许本动作最长时间。 | ESP32S3 本地动作超时阈值，超时要失败回包。 |
| `8` | `motion_profile` | `u8` | 首版填 `0`。 | 可选速度/力度方案，首版按默认动作处理。 |
| `9` | `flags` | `u16` | 首版填 `0`。 | bit0 可扩展为完成后是否松爪；首版按默认动作处理。 |
| `11` | `reserved` | `u8` | 固定填 `0`。 | 保留。 |

### 6.2 ESP32S3 回给 F4 的 `ARM_ACK` payload

ESP32S3 在命令合法、状态允许、已经放入动作队列后立刻回 ACK。

| 偏移 | 字段 | 类型 | 填法 |
|---:|---|---|---|
| `0` | `cycle_id` | `u16` | 被确认命令里的 `cycle_id`。 |
| `2` | `acked_seq` | `u16` | 被确认命令的 `SEQ`。 |
| `4` | `acked_cmd` | `u8` | 被确认命令的 `CMD`。 |
| `5` | `status` | `u8` | `0=accepted`，`1=duplicate`。 |
| `6` | `arm_state` | `u8` | `0=IDLE,1=MOVING,2=HOLDING_PART,3=DONE,4=STOPPED,5=FAULT`。 |

### 6.3 ESP32S3 回给 F4 的 `ARM_STAGE_DONE` payload

ESP32S3 必须在动作真正完成后发送本帧，不能在动作刚开始时发送。

| 偏移 | 字段 | 类型 | 填法 |
|---:|---|---|---|
| `0` | `cycle_id` | `u16` | 本阶段所属流程号。 |
| `2` | `stage_id` | `u8` | 已完成或失败的阶段。 |
| `3` | `result` | `u8` | `0=OK`，非 0 失败原因。 |
| `4` | `detail_code` | `u16` | 失败细节或补充码，成功填 `0`。 |
| `6` | `elapsed_ms` | `u16` | 本动作实际耗时。 |
| `8` | `fault_bits` | `u16` | ESP32S3 本地故障位，成功填 `0`。 |

### 6.4 ESP32S3 回给 F4 的 `ARM_NACK` payload

ESP32S3 在不能接受命令时回 NACK，不能静默丢弃。

| 偏移 | 字段 | 类型 | 填法 |
|---:|---|---|---|
| `0` | `cycle_id` | `u16` | 能解析到业务 payload 时填原 `cycle_id`，否则填 `0`。 |
| `2` | `rejected_seq` | `u16` | 被拒绝帧的 `SEQ`。 |
| `4` | `rejected_cmd` | `u8` | 被拒绝帧的 `CMD`。 |
| `5` | `error_code` | `u8` | 见第 7 节。 |
| `6` | `arm_state` | `u8` | 当前机械臂状态。 |
| `7` | `detail` | `u16` | 补充信息，例如当前正在执行的 stage。 |

## 7. 结果码和故障位

### 7.1 `result/error_code`

| 数值 | 名称 | 什么时候使用 |
|---:|---|---|
| `0` | `OK` | ACK 接受或 DONE 成功。 |
| `1` | `CRC` | CRC 校验失败。CRC 错误时可不回包；如果能定位帧边界，也可回 NACK。 |
| `2` | `FRAME_LENGTH` | 帧长度不合法。 |
| `3` | `CMD_UNKNOWN` | 命令字不支持。 |
| `4` | `PAYLOAD_LENGTH` | payload 长度不符合命令要求。 |
| `5` | `STATE_NOT_ALLOWED` | 当前状态不能执行该动作，例如未持有零件却要求放到电感模块。 |
| `6` | `BUSY` | 正在执行动作，不能接受新动作。 |
| `7` | `CYCLE_MISMATCH` | 收到的 `cycle_id` 与当前动作不一致。 |
| `8` | `MOTION_TIMEOUT` | 动作超过 `timeout_ms` 还未完成。 |
| `9` | `GRIP_FAILED` | 夹取失败、零件掉落、未检测到夹住。 |
| `10` | `SERVO_FAULT` | 舵机、总线、电源或 PWM 输出异常。 |
| `11` | `LIMIT_OR_ESTOP` | 限位、急停或安全区域触发。 |

### 7.2 `fault_bits`

| bit | 含义 |
|---:|---|
| `0` | 夹爪没有夹住零件。 |
| `1` | 运动超时。 |
| `2` | 舵机或总线通信异常。 |
| `3` | 急停触发。 |
| `4` | 机械限位触发。 |
| `5` | 放置后零件状态不确定。 |
| `6` | ESP32S3 本地任务看门狗或动作任务异常。 |
| `7` | 预留给机械臂负责人扩展。 |

## 8. 一次完整自动检测中的 F4-ESP32S3 交互

| 步骤 | F4 动作 | ESP32S3 必须回什么 | F4 收到后做什么 |
|---:|---|---|---|
| 1 | F4 收到 MP157 模型结果，并确认 Z 轴已上升到安全高度。 | 无。 | 准备机械臂流程。 |
| 2 | F4 发 `ARM_MOVE_TO_WEIGHT`。 | 立即回 `ARM_ACK acked_cmd=0x20`。 | 标记机械臂已接受动作，继续等待 DONE。 |
| 3 | ESP32S3 从 ROI 抓取零件并放到称重模块。 | 放好后回 `ARM_STAGE_DONE stage=1 result=0`。 | F4 读取 HX711 重量，并把 `WEIGHT_RESULT` 发给 MP157。 |
| 4 | F4 发 `ARM_MOVE_TO_LDC`。 | 立即回 `ARM_ACK acked_cmd=0x21`。 | 标记机械臂已接受动作，继续等待 DONE。 |
| 5 | ESP32S3 从称重模块抓取零件并放到电磁感应模块。 | 放好后回 `ARM_STAGE_DONE stage=2 result=0`。 | F4 读取 LDC1614 数据，并把 `LDC_RESULT` 发给 MP157。 |
| 6 | MP157 汇总图片、模型、重量、电感并上传云端；上传完成后把最终结果发给 F4。 | 无。 | F4 缓存最终分拣结果。 |
| 7 | F4 发 `ARM_SORT_RESULT target_bin=1/2/3`。 | 立即回 `ARM_ACK acked_cmd=0x22`。 | 标记机械臂已接受分拣动作，继续等待 DONE。 |
| 8 | ESP32S3 从电磁感应模块抓取零件并放入目标盘。 | 放好后回 `ARM_STAGE_DONE stage=3 result=0`。 | F4 给 MP157 发 `CYCLE_DONE`，本轮结束。 |
| 9 | 任意阶段失败。 | 回 `ARM_STAGE_DONE result!=0` 或 `ARM_FAULT_REPORT`。 | F4 给 MP157 发故障/待复核状态，自动流程停止或进入人工复核。 |

## 9. 三个核心动作怎么对接

### 9.1 F4 通知 ESP32S3 抓取零件到称重模块

F4 发送：

| 字段 | 值 |
|---|---|
| `CMD` | `0x20 ARM_MOVE_TO_WEIGHT` |
| `stage_id` | `1 PICK_BELT_TO_WEIGHT` |
| `target_bin` | `0 NONE` |
| `cycle_id` | 当前单件流程号 |
| `timeout_ms` | 建议 `8000` 到 `15000`，按机械臂实际动作时间调 |

ESP32S3 收到后：

| 时机 | ESP32S3 回包 | 说明 |
|---|---|---|
| 解析成功并接受动作 | `ARM_ACK` | `acked_cmd=0x20`，`arm_state=1 MOVING`。 |
| 零件已经真实放在称重模块上 | `ARM_STAGE_DONE` | `stage_id=1`，`result=0`。 |
| 抓取失败或超时 | `ARM_STAGE_DONE` | `stage_id=1`，`result=9 GRIP_FAILED` 或 `8 MOTION_TIMEOUT`。 |

F4 收到 `stage=1 result=0` 后，才能读取称重模块；不能在 ACK 后就读取。

### 9.2 ESP32S3 放到称重模块完成后返回给 F4 什么

成功时返回：

| 字段 | 值 |
|---|---|
| `CMD` | `0x30 ARM_STAGE_DONE` |
| `cycle_id` | 原 `ARM_MOVE_TO_WEIGHT` 的 `cycle_id` |
| `stage_id` | `1 PICK_BELT_TO_WEIGHT` |
| `result` | `0 OK` |
| `detail_code` | `0` |
| `elapsed_ms` | 实际耗时，例如 `3500` |
| `fault_bits` | `0` |

失败时返回：

| 场景 | `result` | `detail_code` | `fault_bits` |
|---|---:|---:|---:|
| 没夹住零件 | `9 GRIP_FAILED` | 可填夹爪检测码 | bit0=1 |
| 动作超时 | `8 MOTION_TIMEOUT` | 可填超时动作段编号 | bit1=1 |
| 舵机故障 | `10 SERVO_FAULT` | 可填舵机 ID | bit2=1 |
| 急停或限位 | `11 LIMIT_OR_ESTOP` | 可填触发来源 | bit3 或 bit4=1 |

### 9.3 F4 通知 ESP32S3 从称重模块搬到电磁感应模块

F4 发送：

| 字段 | 值 |
|---|---|
| `CMD` | `0x21 ARM_MOVE_TO_LDC` |
| `stage_id` | `2 WEIGHT_TO_LDC` |
| `target_bin` | `0 NONE` |

ESP32S3 回包：

| 时机 | 回包 |
|---|---|
| 接受动作 | `ARM_ACK acked_cmd=0x21 arm_state=MOVING` |
| 已放到电磁感应模块 | `ARM_STAGE_DONE stage=2 result=0` |
| 失败 | `ARM_STAGE_DONE stage=2 result!=0` 或 `ARM_FAULT_REPORT` |

F4 收到 `stage=2 result=0` 后，才能读取电磁感应模块数据。

### 9.4 F4 通知 ESP32S3 按最终结果分拣

F4 发送：

| 最终结果 | `CMD` | `stage_id` | `model_result` | `target_bin` | ESP32S3 要放到 |
|---|---:|---:|---:|---:|---|
| 良品 | `0x22` | `3` | `1` | `1` | 良品盘 |
| 不良品 | `0x22` | `3` | `2` | `2` | 不良品盘 |
| 待复核 | `0x22` | `3` | `3` 或 `0` | `3` | 待复核盘 |
| 云端上传失败 | `0x22` | `3` | `3` | `3` | 待复核盘 |

ESP32S3 回包：

| 时机 | 回包 |
|---|---|
| 接受动作 | `ARM_ACK acked_cmd=0x22 arm_state=MOVING` |
| 已放到目标盘 | `ARM_STAGE_DONE stage=3 result=0` |
| 放置失败 | `ARM_STAGE_DONE stage=3 result!=0` 或 `ARM_FAULT_REPORT` |

F4 收到 `stage=3 result=0` 后，给 MP157 发本轮 `CYCLE_DONE`。

## 10. HEX 示例

以下示例固定：

| 字段 | 示例值 |
|---|---|
| `cycle_id` | `0x1234`，payload 中小端为 `34 12` |
| `part_type` | `0x00` |
| `timeout_ms` | `10000 ms`，payload 中小端为 `10 27` |
| `motion_profile` | `0x00` |
| `flags` | `0x0000` |

实际联调时 `SEQ` 必须由发送方递增，不能固定照抄。

### 10.1 F4 发“抓取到称重模块”

```text
A5 5A 01 20 0C 01 00 34 12 01 00 01 00 10 27 00 00 00 00 54 AA 6B
```

解析：

| 字段 | 值 |
|---|---|
| `CMD` | `0x20 ARM_MOVE_TO_WEIGHT` |
| `SEQ` | `0x0001` |
| `cycle_id` | `0x1234` |
| `stage_id` | `1` |
| `model_result` | `1 good` |
| `target_bin` | `0 none` |

### 10.2 ESP32S3 接受称重动作后回 ACK

```text
A5 5A 01 80 07 01 10 34 12 01 00 20 00 01 72 F1 6B
```

解析：

| 字段 | 值 |
|---|---|
| `CMD` | `0x80 ARM_ACK` |
| `SEQ` | `0x1001`，ESP32S3 自己的发送序号 |
| `acked_seq` | `0x0001` |
| `acked_cmd` | `0x20` |
| `status` | `0 accepted` |
| `arm_state` | `1 moving` |

### 10.3 ESP32S3 放到称重模块完成后回 DONE

```text
A5 5A 01 30 0A 02 10 34 12 01 00 00 00 AC 0D 00 00 BD 5C 6B
```

解析：

| 字段 | 值 |
|---|---|
| `CMD` | `0x30 ARM_STAGE_DONE` |
| `SEQ` | `0x1002`，ESP32S3 自己的发送序号 |
| `cycle_id` | `0x1234` |
| `stage_id` | `1 PICK_BELT_TO_WEIGHT` |
| `result` | `0 OK` |
| `elapsed_ms` | `3500 ms`，小端 `AC 0D` |
| `fault_bits` | `0` |

### 10.4 F4 发“称重模块到电磁感应模块”

```text
A5 5A 01 21 0C 02 00 34 12 02 00 01 00 10 27 00 00 00 00 1C 3E 6B
```

### 10.5 F4 发最终分拣

| 目标 | HEX 示例 |
|---|---|
| 良品盘 | `A5 5A 01 22 0C 03 00 34 12 03 00 01 01 10 27 00 00 00 00 14 7A 6B` |
| 不良品盘 | `A5 5A 01 22 0C 04 00 34 12 03 00 02 02 10 27 00 00 00 00 E5 1A 6B` |
| 待复核盘 | `A5 5A 01 22 0C 05 00 34 12 03 00 03 03 10 27 00 00 00 00 36 9E 6B` |

### 10.6 ESP32S3 忙时回 NACK

```text
A5 5A 01 81 09 03 10 34 12 01 00 20 06 01 00 00 FA AC 6B
```

解析：

| 字段 | 值 |
|---|---|
| `CMD` | `0x81 ARM_NACK` |
| `rejected_seq` | `0x0001` |
| `rejected_cmd` | `0x20 ARM_MOVE_TO_WEIGHT` |
| `error_code` | `6 BUSY` |
| `arm_state` | `1 MOVING` |

## 11. ESP32S3 固件必须实现的状态机

| 状态 | 允许接收 | 收到命令后怎么处理 |
|---|---|---|
| `IDLE` | `MOVE_TO_WEIGHT`、`HOME`、`STOP`、`HEARTBEAT` | 接受动作并回 ACK，进入 `MOVING`；心跳保持 `IDLE`。 |
| `MOVING` | `STOP`、`HEARTBEAT` | 普通动作回 `NACK BUSY`；`STOP` 优先执行。 |
| `HOLDING_PART` | 内部状态，可选 | 只有动作流程内部使用，不建议让 F4 直接依赖。 |
| `DONE` | 下一阶段命令 | 已回 DONE 后可以转回 `IDLE`，等待 F4 下发下一阶段。 |
| `STOPPED` | `HOME`、`HEARTBEAT` | 回安全位后转 `IDLE`。 |
| `FAULT` | `STOP`、`HOME`、`HEARTBEAT` | 故障未清除时拒绝普通动作，回 `NACK STATE_NOT_ALLOWED`。 |

ESP32S3 处理一帧动作命令的推荐流程：

1. 从 UART 字节流中寻找 `A5 5A`。
2. 按 `LEN` 等待完整帧。
3. 校验 `VER`、`EOF`、`CRC`。
4. 校验 `CMD` 是否支持。
5. 校验 payload 长度是否符合命令。
6. 如果机械臂正在动作且不是 `STOP`，回 `ARM_NACK error_code=BUSY`。
7. 如果命令合法且状态允许，先回 `ARM_ACK`。
8. 执行动作组、IK 或舵机轨迹。
9. 动作真实完成并确认零件放好后，回 `ARM_STAGE_DONE result=OK`。
10. 动作失败、超时、急停或限位时，回 `ARM_STAGE_DONE result!=OK` 或 `ARM_FAULT_REPORT`。

## 12. F4 收包校验要求

F4 收到 ESP32S3 回包后必须校验：

| 校验项 | 规则 |
|---|---|
| 帧头帧尾 | 必须是 `A5 5A ... 6B`。 |
| CRC | 校验失败直接丢弃并计入链路错误。 |
| `cycle_id` | 必须等于当前自动流程 ID。 |
| `acked_seq` | ACK/NACK 必须匹配 F4 刚发出的 `SEQ`。 |
| `acked_cmd` | 必须匹配 F4 刚发出的 `CMD`。 |
| `stage_id` | DONE 必须匹配当前等待阶段。 |
| `result` | 只有 `0 OK` 才推进到称重、电感或分拣完成。 |
| 超时 | ACK 超时和 DONE 超时要分别记录，不能混在一起。 |

## 13. 与 MP157/F4 自动流程的关系

| 上游阶段 | F4-ESP32S3 阶段 | F4 回 MP157 的数据 |
|---|---|---|
| MP157 模型检测完成，发送模型结果给 F4 | F4 发 `ARM_MOVE_TO_WEIGHT` | 暂不回传重量，等待称重完成。 |
| ESP32S3 回 `stage=1 OK` | F4 读取 HX711 | F4 发 `WEIGHT_RESULT`。 |
| MP157 收到重量并 ACK | F4 发 `ARM_MOVE_TO_LDC` | 暂不回传电感，等待电感完成。 |
| ESP32S3 回 `stage=2 OK` | F4 读取 LDC1614 | F4 发 `LDC_RESULT`。 |
| MP157 完成云端上传并把最终结果发给 F4 | F4 发 `ARM_SORT_RESULT` | 暂不结束本轮，等待分拣完成。 |
| ESP32S3 回 `stage=3 OK` | F4 本轮机械臂流程完成 | F4 发 `CYCLE_DONE`。 |

## 14. 当前 F4 兼容旧 LeArm 动作组的说明

当前 `E:\hal\bisai_f407_project\User\App\robot_arm_service.c` 里保留了旧的 `55 55` 动作组方式：

| 动作 | 旧 LeArm 动作组编号 |
|---|---:|
| 放到称重模块 | `10` |
| 放到电磁感应模块 | `11` |
| 放到良品盘 | `12` |
| 放到不良品盘 | `13` |
| 放到待复核盘 | `14` |

旧方式发送示例：

```text
55 55 05 06 0A 01 00
```

含义是运行动作组 `10` 一次。

旧方式的问题：

| 问题 | 影响 |
|---|---|
| 出厂动作组运行命令通常无回包 | F4 只能知道“已发送”，不能知道“已放好”。 |
| 没有 `cycle_id` | 多轮自动检测时无法确认回包属于哪一个零件。 |
| 没有 ACK/DONE 分离 | F4 不能可靠决定什么时候读 HX711/LDC1614。 |
| 没有失败原因 | 抓取失败、掉件、舵机故障无法按协议回 MP157。 |

因此最终自动检测必须切换为本文的 `A5 5A` 正式协议，或在 ESP32S3 旧动作组固件外层增加 `A5 5A` 协议适配层：F4 发 `A5 5A ARM_MOVE_TO_WEIGHT`，ESP32S3 内部运行动作组 10，动作组真正结束后再回 `A5 5A ARM_STAGE_DONE stage=1`。

## 15. ESP32S3 对接验收清单

| 测试目标 | 执行位置 | 命令/方法 | 预期输出/现象 | 失败时先查 |
|---|---|---|---|---|
| 串口连通 | F4 或串口调试工具 | 发送 `ARM_LINK_HEARTBEAT` 完整帧 | ESP32S3 回 `ARM_ACK`。 | TX/RX 是否交叉、GND、波特率、串口号。 |
| CRC 一致 | F4 和 ESP32S3 工程 | 用本文第 10 节 `MOVE_WEIGHT` 示例解帧 | ESP32S3 解出 `CMD=0x20`、`cycle_id=0x1234`。 | CRC 覆盖范围是否从 `VER` 到 payload。 |
| ACK 不冒充完成 | ESP32S3 串口日志 | 收到 `ARM_MOVE_TO_WEIGHT` 后先回 ACK，再执行动作 | F4 不会在 ACK 后立刻称重。 | ESP 是否把 ACK 和 DONE 写反。 |
| 称重放置完成 | 实物机械臂 | 执行 `ARM_MOVE_TO_WEIGHT` | 零件放到称重模块后才回 `stage=1 result=0`。 | 动作组结束事件、夹爪松开时机、放置检测。 |
| 电感放置完成 | 实物机械臂 | 执行 `ARM_MOVE_TO_LDC` | 零件放到电磁感应模块后才回 `stage=2 result=0`。 | 称重模块取件点、电感模块放置点。 |
| 最终分拣 | 实物机械臂 | 分别发送 `target_bin=1/2/3` | 分别放到良品、不良品、待复核盘，完成后回 `stage=3 result=0`。 | target_bin 映射是否写反。 |
| 忙状态保护 | ESP32S3 | 动作执行中再发普通动作 | 回 `ARM_NACK error_code=BUSY`，不插队执行。 | ESP 状态机是否允许重复入队。 |
| STOP 优先 | ESP32S3 | 动作执行中发 `ARM_STOP` | 优先停止或安全退出，回 `stage=5` 或故障帧。 | STOP 是否被普通 BUSY 拒绝。 |
| 故障上报 | 人为断开舵机或触发急停 | 执行动作 | 回 `result=10/11` 或 `ARM_FAULT_REPORT`。 | 舵机错误检测、急停输入、fault_bits。 |

## 16. 修改文件清单

| 文件 | 本次用途 |
|---|---|
| `docs/f4_esp32s3_arm_protocol/f4_esp32s3_integration_guide.md` | 新增 F4 与 ESP32S3 机械臂对接执行手册，明确正式协议、动作顺序、回包要求和验收方法。 |
| `docs/f4_esp32s3_arm_protocol/README.md` | 增加本执行手册入口，避免只看到底层协议而忽略自动流程对接规则。 |

## 17. 后续落地顺序

| 顺序 | 动作 | 说明 |
|---:|---|---|
| 1 | ESP32S3 先实现本文 `A5 5A` 解帧、ACK、NACK、DONE | 不需要先改机械臂轨迹，先把协议打通。 |
| 2 | ESP32S3 把 `stage=1/2/3` 映射到现有动作组 10/11/12/13/14 | 可以复用现有动作组，只是外层协议换成可靠 DONE。 |
| 3 | F4 `robot_arm_service.c` 从旧 `55 55` 转发切换到 `A5 5A` 正式协议收发 | F4 必须等待 ESP32S3 DONE 后再读称重/电感。 |
| 4 | MP157-F4-ESP32S3 三端联调 | 验证一轮完整自动检测：模型、称重、电感、上传、分拣、CYCLE_DONE。 |

