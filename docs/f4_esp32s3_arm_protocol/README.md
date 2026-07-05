# F4-ESP32S3 机械臂二进制协议

| 项目 | 内容 |
|---|---|
| 文档目的 | 定义 F4 与 ESP32S3 机械臂控制器之间的二进制协议，供 F4 固件和 ESP32S3 机械臂固件共同实现。 |
| 适用链路 | STM32F407 USART3 `<->` ESP32S3 UART。 |
| 协议风格 | 与 MP157-F4 主协议保持一致：`A5 5A ... CRC16 ... 6B`。 |
| 参考代码 | `arm_link_protocol.h`、`arm_link_protocol.c`。 |
| 设计边界 | 本协议只规定 F4 发什么命令、ESP32S3 回什么状态；具体舵机轨迹、动作组、IK 或抓取算法由机械臂负责人实现。 |

> 自动流程对接时优先看 [`f4_esp32s3_integration_guide.md`](./f4_esp32s3_integration_guide.md)。本文件偏底层帧格式和共享代码说明；对接手册会逐步写清楚“F4 发称重命令后 ESP32S3 先 ACK、放好后 DONE、F4 再读 HX711”等完整流程。

> 当前 F4 工程里的 `robot_arm_service.c` 仍保留 `55 55` LeArm 动作组兼容转发逻辑，用于早期验证动作组编号。最终自动检测闭环必须切换到 `A5 5A` 正式协议，或者让 ESP32S3 在旧动作组外层增加本文定义的 ACK/DONE 回包。

## 1. 硬件连接建议

| 信号 | F4 | ESP32S3 | 说明 |
|---|---|---|---|
| F4 TX | `USART3_TX`，当前工程日志提示 `PD8` | ESP32S3 RX | F4 发命令给 ESP32S3。 |
| F4 RX | `USART3_RX`，当前工程日志提示 `PD9` | ESP32S3 TX | ESP32S3 回 ACK、DONE、FAULT。 |
| GND | GND | GND | 必须共地。 |
| 波特率 | 115200 | 115200 | 首版固定 8N1。 |

> 如果后续实际引脚与 `PD8/PD9` 不一致，以 CubeMX 和硬件接线为准，但文档、F4 代码注释和 ESP32S3 串口初始化必须同步改。

## 2. 帧格式

```text
A5 5A VER CMD LEN SEQ_L SEQ_H PAYLOAD... CRC_L CRC_H 6B
```

| 字段 | 字节数 | 说明 |
|---|---:|---|
| `A5 5A` | 2 | 固定帧头。 |
| `VER` | 1 | 协议版本，首版 `0x01`。 |
| `CMD` | 1 | 命令字。 |
| `LEN` | 1 | payload 长度，首版最大 48 字节。 |
| `SEQ_L/SEQ_H` | 2 | 发送方帧序号，小端序。 |
| `PAYLOAD` | N | 负载，所有多字节整数小端序。 |
| `CRC_L/CRC_H` | 2 | CRC16-CCITT-FALSE，覆盖 `VER` 到 `PAYLOAD`。 |
| `6B` | 1 | 固定帧尾。 |

## 3. 命令总表

| CMD | 名称 | 方向 | 是否需要 ACK | 用途 |
|---:|---|---|---|---|
| `0x01` | `ARM_LINK_HELLO` | 双向 | 是 | 上电握手，确认协议版本和能力。 |
| `0x02` | `ARM_LINK_HEARTBEAT` | 双向 | 是 | 心跳，确认机械臂链路在线。 |
| `0x20` | `ARM_MOVE_TO_WEIGHT` | F4 -> ESP32S3 | 是 | 从传送带/ROI 抓取零件，放到称重模块。 |
| `0x21` | `ARM_MOVE_TO_LDC` | F4 -> ESP32S3 | 是 | 从称重模块夹起零件，放到电磁感应模块。 |
| `0x22` | `ARM_SORT_RESULT` | F4 -> ESP32S3 | 是 | 从电磁感应模块夹起零件，按结果放到对应区域。 |
| `0x23` | `ARM_HOME` | F4 -> ESP32S3 | 是 | 回安全初始位。 |
| `0x24` | `ARM_STOP` | F4 -> ESP32S3 | 是 | 停止当前动作，进入安全状态。 |
| `0x30` | `ARM_STAGE_DONE` | ESP32S3 -> F4 | 是 | 某个动作阶段真正完成。 |
| `0x31` | `ARM_STAGE_REPORT` | ESP32S3 -> F4 | 否 | 可选进度上报，例如已抓住、移动中、已放下。 |
| `0x32` | `ARM_JOB_DONE` | ESP32S3 -> F4 | 是 | 可选整套机械臂任务完成。 |
| `0x80` | `ARM_ACK` | 双向 | 否 | 命令已接收并接受。 |
| `0x81` | `ARM_NACK` | 双向 | 否 | 命令被拒绝，并返回错误码。 |
| `0x87` | `ARM_FAULT_REPORT` | ESP32S3 -> F4 | 是 | 机械臂故障，例如抓取失败、舵机故障、急停。 |

## 4. 阶段枚举

| stage_id | 名称 | 含义 |
|---:|---|---|
| `1` | `PICK_BELT_TO_WEIGHT` | 从传送带/ROI 取件并放到称重模块。 |
| `2` | `WEIGHT_TO_LDC` | 从称重模块取件并放到电磁感应模块。 |
| `3` | `LDC_TO_SORT_BIN` | 从电磁感应模块取件并放到分拣区域。 |
| `4` | `HOME` | 回到机械臂安全位。 |
| `5` | `STOP_SAFE` | 停止动作并进入安全状态。 |

## 5. 关键负载

### 5.1 动作命令负载

`ARM_MOVE_TO_WEIGHT`、`ARM_MOVE_TO_LDC`、`ARM_SORT_RESULT`、`ARM_HOME`、`ARM_STOP` 使用同一种 12 字节负载。

| 偏移 | 字段 | 类型 | 说明 |
|---:|---|---|---|
| `0` | `cycle_id` | `u16` | MP157/F4 当前单件流程 ID。 |
| `2` | `stage_id` | `u8` | 阶段编号，见第 4 节。 |
| `3` | `part_type` | `u8` | 零件类型，和 MP157-F4 协议一致；未知填 `0`。 |
| `4` | `model_result` | `u8` | 模型结果，`0=unknown,1=good,2=bad,3=review/uncertain`。 |
| `5` | `target_bin` | `u8` | 分拣目标，`0=none,1=good,2=bad,3=review`。称重/电感阶段填 `0`。 |
| `6` | `timeout_ms` | `u16` | F4 允许本动作运行的最长时间。 |
| `8` | `motion_profile` | `u8` | 动作速度/力度方案，首版 `0=默认`。 |
| `9` | `flags` | `u16` | 位图，首版可填 `0`；bit0 可表示动作完成后是否松爪。 |
| `11` | `reserved` | `u8` | 预留，填 `0`。 |

F4 发送示例：

| 业务 | CMD | stage_id | target_bin |
|---|---:|---:|---:|
| 放到称重模块 | `0x20 ARM_MOVE_TO_WEIGHT` | `1` | `0` |
| 放到电磁感应模块 | `0x21 ARM_MOVE_TO_LDC` | `2` | `0` |
| 放到良品区 | `0x22 ARM_SORT_RESULT` | `3` | `1` |
| 放到不良品区 | `0x22 ARM_SORT_RESULT` | `3` | `2` |
| 放到待复核区 | `0x22 ARM_SORT_RESULT` | `3` | `3` |

首版分拣依据固定如下：

| MP157 模型结果 | F4 缓存字段 | F4 发送给 ESP32S3 的 `target_bin` | 说明 |
|---|---:|---:|---|
| `MODEL_GOOD` | `model_result=1` | `ARM_LINK_BIN_GOOD=1` | 零件放入良品盘。 |
| `MODEL_BAD` | `model_result=2` | `ARM_LINK_BIN_BAD=2` | 零件放入不良品盘。 |
| `MODEL_UNCERTAIN` | `model_result=3` | `ARM_LINK_BIN_REVIEW=3` | 零件放入待复核盘。 |
| `MODEL_UNKNOWN` 或未收到模型结果 | `model_result=0` | `ARM_LINK_BIN_REVIEW=3` | 为避免误放，默认进入待复核盘。 |

称重和电感检测结果首版只用于上传云端和追溯，不改变分拣盘选择。后续如果比赛规则要求“模型、重量、电感共同判定分拣”，应先更新 MP157-F4 的判定策略文档，再修改 F4 的映射函数。

### 5.2 `ARM_STAGE_DONE` 负载

| 偏移 | 字段 | 类型 | 说明 |
|---:|---|---|---|
| `0` | `cycle_id` | `u16` | 本动作对应的流程 ID。 |
| `2` | `stage_id` | `u8` | 已完成的阶段。 |
| `3` | `result` | `u8` | `0=OK`，非 0 表示失败。 |
| `4` | `detail_code` | `u16` | 失败或补充原因，例如抓取失败、动作超时、舵机报警。 |
| `6` | `elapsed_ms` | `u16` | 本阶段耗时。 |
| `8` | `fault_bits` | `u16` | ESP32S3 侧机械臂故障位。 |

F4 收到成功回包后的动作：

| 回包 | F4 下一步 |
|---|---|
| `stage=PICK_BELT_TO_WEIGHT result=OK` | 开始 HX711 称重，完成后给 MP157 发 `WEIGHT_RESULT`。 |
| `stage=WEIGHT_TO_LDC result=OK` | 开始 LDC1614 电感检测，完成后给 MP157 发 `LDC_RESULT`；MP157 ACK 后，F4 根据本轮缓存的 MP157 模型结果发送 `ARM_SORT_RESULT`。 |
| `stage=LDC_TO_SORT_BIN result=OK` | 给 MP157 发 `CYCLE_DONE`，等待 ACK 后进入下一件扫描。 |
| `result!=OK` | 给 MP157 发 `FAULT_REPORT fault_source=ARM`，停止当前自动流程或等待人工处理。 |

### 5.3 ACK 负载

| 偏移 | 字段 | 类型 | 说明 |
|---:|---|---|---|
| `0` | `cycle_id` | `u16` | 对应业务流程 ID，没有流程时填 `0`。 |
| `2` | `acked_seq` | `u16` | 被确认帧的 SEQ。 |
| `4` | `acked_cmd` | `u8` | 被确认帧的 CMD。 |
| `5` | `status` | `u8` | `0=accepted`，`1=duplicate`。 |
| `6` | `arm_state` | `u8` | ESP32S3 或 F4 当前机械臂状态。 |

### 5.4 NACK 负载

| 偏移 | 字段 | 类型 | 说明 |
|---:|---|---|---|
| `0` | `cycle_id` | `u16` | 对应流程 ID。 |
| `2` | `rejected_seq` | `u16` | 被拒绝帧的 SEQ。 |
| `4` | `rejected_cmd` | `u8` | 被拒绝帧的 CMD。 |
| `5` | `error_code` | `u8` | 错误码。 |
| `6` | `arm_state` | `u8` | 当前机械臂状态。 |
| `7` | `detail` | `u16` | 补充信息。 |

## 6. 错误码与结果码

| 数值 | 名称 | 含义 |
|---:|---|---|
| `0` | `OK` | 成功。 |
| `1` | `CRC` | CRC 错误。 |
| `2` | `FRAME_LENGTH` | 帧长度错误。 |
| `3` | `CMD_UNKNOWN` | 不支持的命令。 |
| `4` | `PAYLOAD_LENGTH` | 负载长度错误。 |
| `5` | `STATE_NOT_ALLOWED` | 当前状态不能执行该命令。 |
| `6` | `BUSY` | 机械臂正忙。 |
| `7` | `CYCLE_MISMATCH` | `cycle_id` 不匹配。 |
| `8` | `MOTION_TIMEOUT` | 动作超时。 |
| `9` | `GRIP_FAILED` | 抓取失败或零件掉落。 |
| `10` | `SERVO_FAULT` | 舵机或总线异常。 |
| `11` | `LIMIT_OR_ESTOP` | 限位或急停触发。 |

## 7. F4 侧状态推进

| F4 阶段 | F4 发给 ESP32S3 | ESP32S3 必须返回 | F4 收到后做什么 |
|---|---|---|---|
| 机械臂抓取到称重 | `ARM_MOVE_TO_WEIGHT` | 先 `ACK`，完成后 `ARM_STAGE_DONE stage=1 result=OK` | 读取 HX711，给 MP157 发 `WEIGHT_RESULT`。 |
| 机械臂转移到电感 | `ARM_MOVE_TO_LDC` | 先 `ACK`，完成后 `ARM_STAGE_DONE stage=2 result=OK` | 读取 LDC1614，给 MP157 发 `LDC_RESULT`。 |
| 机械臂分拣 | `ARM_SORT_RESULT target_bin=good/bad/review`，其中 `target_bin` 来自 MP157 的模型综合结果 | 先 `ACK`，完成后 `ARM_STAGE_DONE stage=3 result=OK` | 给 MP157 发 `CYCLE_DONE`。 |
| 停止 | `ARM_STOP` | `ACK`，完成后 `ARM_STAGE_DONE stage=5` 或 `FAULT_REPORT` | 停止 F4 自动流程，通知 MP157。 |

## 8. ESP32S3 侧最小实现要求

| 功能 | 要求 |
|---|---|
| 解帧 | 用 `ArmLinkProtocol_ParseFrame()` 找到合法帧；CRC 错误不能执行动作。 |
| ACK | 命令合法且已进入动作队列后立刻回 `ARM_ACK`。 |
| NACK | 状态不允许、payload 错、忙、未知命令时回 `ARM_NACK`。 |
| 动作完成 | 动作真正放好零件后回 `ARM_STAGE_DONE`，不能只在开始动作时回完成。 |
| 超时 | ESP32S3 自己检测到动作超时也要回失败，F4 也会用 `timeout_ms` 做外层超时。 |
| 安全 | 收到 `ARM_STOP` 必须优先处理，停止当前动作或进入安全位。 |

## 9. 修改文件清单

| 文件 | 用途 |
|---|---|
| `README.md` | 协议说明、命令、负载、状态推进和联调要求。 |
| `arm_link_protocol.h` | 共享常量、枚举、负载结构、组帧/解帧函数声明。 |
| `arm_link_protocol.c` | 共享 CRC、组帧、解帧和常用 payload 编解码实现。 |
| `f4_arm_link_commands.h` | F4 侧直接调用的“放称重、放电感、分拣、回零、停止”命令构建函数。 |
| `f4_arm_link_commands.c` | F4 侧命令 payload 默认填充和完整帧构建实现。 |
| `f4_esp32s3_integration_guide.md` | F4 与 ESP32S3 机械臂对接执行手册，明确自动流程命令、回包、失败处理和验收步骤。 |

## 10. 验证方式

| 测试目标 | 执行位置 | 命令/方法 | 预期输出/现象 | 失败时排查 |
|---|---|---|---|---|
| 查找协议命令 | Windows 仓库根目录 | `Select-String -Path docs\f4_esp32s3_arm_protocol\*.h,docs\f4_esp32s3_arm_protocol\*.c -Pattern "ARM_MOVE_TO_WEIGHT"` | 能定位到命令定义和构建函数。 | 检查是否使用了正确文件夹。 |
| CRC 一致 | F4 与 ESP32S3 工程 | 用同一 payload 调 `ArmLinkProtocol_BuildFrame()`。 | 两边输出相同 CRC。 | 检查 CRC 覆盖范围是否从 `VER` 到 `PAYLOAD`。 |
| ESP32S3 ACK | F4 串口发送 `ARM_HOME` | ESP32S3 回 `ARM_ACK`。 | F4 标记机械臂链路在线。 | 查 TX/RX、GND、波特率、ESP32S3 串口号。 |
| 称重阶段完成 | F4 发送 `ARM_MOVE_TO_WEIGHT` | ESP32S3 动作完成后回 `ARM_STAGE_DONE stage=1 result=0`。 | F4 开始 HX711 称重。 | 如果只 ACK 不 DONE，F4 会等待到超时。 |
| 电感阶段完成 | F4 发送 `ARM_MOVE_TO_LDC` | ESP32S3 动作完成后回 `ARM_STAGE_DONE stage=2 result=0`。 | F4 开始 LDC1614 检测。 | 检查动作组是否真的执行到放置完成。 |
