# ESP32S3 机械臂回包顺序说明

| 项目 | 内容 |
|---|---|
| 文档目的 | 说明 ESP32S3 收到 F4 机械臂命令后，到底应该回什么帧，F4 才会继续称重、电感、上传和最终分拣流程。 |
| 适用链路 | STM32F407 USART3 `<->` ESP32S3 UART，固定 `115200 8N1`，3.3V TTL，共地。 |
| 关键结论 | ESP32S3 不能只回 `ARM_ACK`；每个动作真实放好零件后，必须再回对应 `ARM_STAGE_DONE`。 |
| 当前 F4 代码 | `E:\hal\bisai_f407_project\User\App\robot_arm_service.c` |
| 共享参考代码 | `docs/f4_esp32s3_arm_protocol/arm_link_protocol.h`、`docs/f4_esp32s3_arm_protocol/arm_link_protocol.c` |

## 1. ESP32S3 回包总原则

| 字段 | ESP32S3 应该怎么处理 |
|---|---|
| `cycle_id` | 这是 MP157/F4 当前这一件零件的流程号，ESP32S3 必须原样带回 ACK、DONE、NACK。 |
| `job_id` | 首版 MP157 直接让 `job_id = cycle_id`，F4 内部用它对账；F4 发给 ESP32S3 的动作 payload 里没有单独 `job_id` 字段，ESP32S3 不需要自己生成。 |
| `SEQ` | 这是 UART 帧序号，不是零件序号。ESP32S3 回 ACK 时，ACK 帧自己的 `SEQ` 可以用 ESP32S3 本地递增值，但 ACK payload 里的 `acked_seq` 必须等于 F4 原命令的 `SEQ`。 |
| `acked_cmd` | ACK payload 里的 `acked_cmd` 必须等于 F4 原命令 CMD，例如 `0x20`、`0x21`、`0x22`。 |
| `stage_id` | DONE payload 里的 `stage_id` 必须等于 F4 原命令 payload 的 `stage_id`。F4 就靠它判断当前阶段是否完成。 |
| `result` | DONE payload 里 `result=0` 表示动作成功；非 0 表示失败，F4 会上报故障并停止后续流程。 |
| `timeout_ms` | F4 动作命令 payload 里的 `timeout_ms` 是 `u32`，当前正式固件发送 `100000ms`，ESP32S3 要按 100 秒动作超时处理。 |

## 2. 零件类型和序号

| 名称 | 字段 | 来源 | 取值 |
|---|---|---|---|
| 当前零件序号 | `cycle_id` | MP157 开始一轮自动检测时分配，F4 后续沿用 | `1..65535`，`0` 表示无自动流程或手动命令 |
| 机械臂任务号 | `job_id` | MP157 下发 `ARM_JOB_START` 时分配 | 首版等于 `cycle_id`，只在 MP157-F4 层使用 |
| 串口帧序号 | `SEQ` | F4 和 ESP32S3 各自发送帧时递增 | 只用于 ACK 对账，不代表第几个零件 |
| 零件类型 | `part_type` | MP157 根据模型 `class=` 映射后传给 F4，再由 F4 转给 ESP32S3 | `0=未知`，`1=波形垫圈/gasket/wave_washer`，`2=平垫圈/washer/flat_washer`，`3=弹性垫圈/splitwasher/split_washer` |
| 模型结果 | `model_result` | MP157 根据双模型综合结果传给 F4 | `0=未知`，`1=良品`，`2=不良品`，`3=待复核` |
| 分拣盘 | `target_bin` | MP157 完整上传后通过 F4 最终下发给 ESP32S3 | `0=非分拣动作`，`1=良品盘`，`2=不良品盘`，`3=待复核盘` |

## 3. F4 发给 ESP32S3 的动作 payload

`ARM_MOVE_TO_WEIGHT 0x20`、`ARM_MOVE_TO_LDC 0x21`、`ARM_SORT_RESULT 0x22` 共用下面 14 字节 payload。

| 偏移 | 字段 | 类型 | ESP32S3 用法 |
|---:|---|---|---|
| `0` | `cycle_id` | `u16` | 保存为当前动作所属零件号，ACK/DONE 必须原样带回。 |
| `2` | `stage_id` | `u8` | 决定执行哪个阶段动作。 |
| `3` | `part_type` | `u8` | 可选用于选择夹爪高度、轨迹或动作组；未知为 `0`。 |
| `4` | `model_result` | `u8` | 可选用于选择动作策略；最终分拣以 `target_bin` 为准。 |
| `5` | `target_bin` | `u8` | `0` 表示称重/电感阶段；`1/2/3` 表示最终分拣目标盘。 |
| `6` | `timeout_ms` | `u32` | 小端序动作超时；当前 `100000ms` 对应 `A0 86 01 00`。 |
| `10` | `motion_profile` | `u8` | 首版 `0=默认轨迹`。 |
| `11` | `flags` | `u16` | 首版填 `0`。 |
| `13` | `reserved` | `u8` | 保留，填 `0`。 |

## 4. ACK 应该怎么发

ESP32S3 收到合法命令并已经把动作放入本地队列后，必须立即发 `ARM_ACK 0x80`。ACK 只表示“我收到并接受了”，不表示机械臂已经放好。

| ACK payload 偏移 | 字段 | 类型 | 必须填写 |
|---:|---|---|---|
| `0` | `cycle_id` | `u16` | 原动作 payload 的 `cycle_id`。 |
| `2` | `acked_seq` | `u16` | F4 原命令帧的 `SEQ`。 |
| `4` | `acked_cmd` | `u8` | F4 原命令的 CMD。 |
| `5` | `status` | `u8` | `0=accepted`；重复命令可填 `1=duplicate`。 |
| `6` | `arm_state` | `u8` | 建议 `1=moving`，空闲可填 `0`，故障填 `5`。 |

F4 会检查 `cycle_id`、`acked_seq`、`acked_cmd`。任一不匹配，F4 会打印 `ACK mismatch` 并停止等待后续 DONE。

## 5. DONE 应该怎么发

机械臂真实完成动作以后，ESP32S3 必须发 `ARM_STAGE_DONE 0x30`。只有这个帧能让 F4 进入下一步。

最关键的称重阶段完成条件是：`stage_id=1` 表示零件已经放到称重模块，且 `result=0` 表示成功；F4 只有收到这个组合才会读取 HX711 并发送 `WEIGHT_RESULT`。

| DONE payload 偏移 | 字段 | 类型 | 必须填写 |
|---:|---|---|---|
| `0` | `cycle_id` | `u16` | 原动作 payload 的 `cycle_id`。 |
| `2` | `stage_id` | `u8` | 原动作 payload 的 `stage_id`。 |
| `3` | `result` | `u8` | `0=成功`，非 0 表示失败。 |
| `4` | `detail_code` | `u16` | 成功填 `0`；失败填错误码，例如抓取失败、超时、舵机故障。 |
| `6` | `elapsed_ms` | `u16` | 本阶段耗时。若动作超过 `65535ms`，首版可填 `65535`，真实超时判断以动作命令里的 `timeout_ms:u32` 为准。 |
| `8` | `fault_bits` | `u16` | 成功填 `0`；失败时填故障位。 |

## 6. 三个阶段怎么让 F4 继续走

| 阶段 | F4 发给 ESP32S3 | ESP32S3 立即回 | ESP32S3 动作完成后回 | F4 收到成功 DONE 后 |
|---|---|---|---|---|
| 抓到称重模块 | `CMD=0x20 ARM_MOVE_TO_WEIGHT`，`stage_id=1`，`target_bin=0` | `ARM_ACK acked_cmd=0x20` | `ARM_STAGE_DONE stage_id=1 result=0` | 读取 HX711，给 MP157 发 `WEIGHT_RESULT`，再发 `ARM_MOVE_TO_LDC`。 |
| 从称重到电感 | `CMD=0x21 ARM_MOVE_TO_LDC`，`stage_id=2`，`target_bin=0` | `ARM_ACK acked_cmd=0x21` | `ARM_STAGE_DONE stage_id=2 result=0` | 读取 LDC1614，给 MP157 发 `LDC_RESULT`，等待 MP157 上传完成。 |
| 从电感到盘子 | `CMD=0x22 ARM_SORT_RESULT`，`stage_id=3`，`target_bin=1/2/3` | `ARM_ACK acked_cmd=0x22` | `ARM_STAGE_DONE stage_id=3 result=0` | 给 MP157 发 `CYCLE_DONE`，本轮结束。 |

## 7. 现场最常见卡住点

| 现象 | 说明 | 先查什么 |
|---|---|---|
| 机械臂已放到称重模块，但 F4 没有读重量 | ESP32S3 没发合法 `ARM_STAGE_DONE stage_id=1 result=0`，或 `cycle_id/stage_id` 不匹配。 | F4 USART1 日志有没有 `[ARM] DONE received ... stage=1 result=0`。 |
| F4 只有 ACK 日志，没有 DONE 日志 | ESP32S3 把 ACK 当成动作完成，或动作完成后没有发 DONE。 | ESP32S3 动作完成回调里是否调用了 DONE 组帧发送。 |
| F4 打印 `DONE mismatch` | DONE 里的 `cycle_id` 或 `stage_id` 和当前等待动作不一致。 | 用串口抓包看 DONE payload 偏移 `0..3`。 |
| F4 打印 `DONE timeout` | ESP32S3 没在 `timeout_ms + 10000ms` 内发 DONE。 | 检查 ESP32S3 是否按 `u32 timeout_ms=100000` 解析，而不是按旧 `u16`。 |
| F4 打印 `ACK mismatch` | ACK payload 里的 `acked_seq` 或 `acked_cmd` 填错。 | ACK payload 偏移 `2..4` 是否回了 F4 原命令的 `SEQ/CMD`。 |

## 8. 修改文件清单

| 文件 | 本次说明 |
|---|---|
| `docs/f4_esp32s3_arm_protocol/esp32s3_send_sequence.md` | 新增 ESP32S3 回包顺序和字段填写说明。 |
| `docs/f4_esp32s3_arm_protocol/README.md` | 同步动作 payload 长度、`timeout_ms:u32` 和本文链接。 |
| `docs/f4_esp32s3_arm_protocol/f4_esp32s3_integration_guide.md` | 同步集成手册中的动作 payload 和超时字段。 |
| `docs/f4_esp32s3_arm_protocol/arm_link_protocol.h` | 共享协议结构体把动作 payload 扩展为 14 字节，`timeout_ms` 改为 `uint32_t`。 |
| `docs/f4_esp32s3_arm_protocol/arm_link_protocol.c` | 共享协议编码/解码改为 `timeout_ms:u32` 小端。 |
| `docs/f4_esp32s3_arm_protocol/f4_arm_link_commands.h` | F4 参考命令构建接口的 `timeout_ms` 改为 `uint32_t`。 |
| `docs/f4_esp32s3_arm_protocol/f4_arm_link_commands.c` | F4 参考命令构建实现同步 `uint32_t timeout_ms`。 |
| `E:\hal\bisai_f407_project\User\App\robot_arm_service.c` | F4 实际固件动作 payload 从 12 字节扩展为 14 字节，`timeout_ms=100000ms` 不再被截断。 |

## 9. 验证方式

| 测试目标 | 执行位置 | 命令/方法 | 预期输出/现象 | 失败时排查 |
|---|---|---|---|---|
| 静态契约检查 | Windows 仓库根目录 | `"C:/Program Files/Git/bin/bash.exe" ./20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh` | 检查通过，不再提示 F4 timeout payload 仍是 12 字节。 | 如果失败，按脚本提示检查 `robot_arm_service.c` 和本目录协议文档。 |
| F4 发出 100 秒超时 | 串口分析仪监听 F4 USART3 TX | 自动流程触发 `ARM_MOVE_TO_WEIGHT` 后抓包 | 动作 payload 偏移 `6..9` 为 `A0 86 01 00`。 | 若仍看到旧 2 字节或 `60 EA`，说明 F4 未重新编译、下载或 ESP32S3 仍按旧协议解析。 |
| 称重阶段推进 | F4 USART1 日志 | 让 ESP32S3 在放好称重后发 DONE | 出现 `[ARM] DONE received ... stage=1 result=0`，随后有 `ARM_MOVE_TO_LDC` 入队日志。 | 若无 DONE 日志，检查 ESP32S3 是否只 ACK 没 DONE。 |
| 电感阶段推进 | F4 USART1 日志 | 让 ESP32S3 在放好电感后发 DONE | 出现 `stage=2 result=0`，MP157 收到 `LDC_RESULT` 后开始完整上传。 | 若 MP157 等电感失败，查 F4 是否收到 stage2 DONE。 |
| 最终分拣闭环 | MP157/F4 日志 | 上传完成后 MP157 下发 `FINAL_SORT_RESULT` | ESP32S3 收 `CMD=0x22`，DONE stage3 后 MP157 收 `CYCLE_DONE`。 | 若卡住，查 target_bin、ACK、stage3 DONE。 |

## 10. 修改记录

| 日期 | 修改点 | 说明 |
|---|---|---|
| 2026-07-07 | 明确 ESP32S3 ACK/DONE 回包顺序 | 说明 `cycle_id` 是当前零件序号，`SEQ` 是帧序号，ACK 和 DONE 要分别负责接收确认和真实完成。 |
| 2026-07-07 | 动作超时字段扩展为 `u32` | F4 实际固件当前使用 `100000ms`，旧 `u16 timeout_ms` 会截断成 `34464ms`，因此协议 payload 从 12 字节扩到 14 字节。 |
