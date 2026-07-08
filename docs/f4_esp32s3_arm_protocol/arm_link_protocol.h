#ifndef ARM_LINK_PROTOCOL_H
#define ARM_LINK_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*
 * 本文件定义 F4 与 ESP32S3 机械臂控制器之间的共享二进制协议。
 * 设计目标：
 * 1. F4 和 ESP32S3 使用同一套帧头、帧尾、CRC 和命令号，避免两端各写一套格式；
 * 2. 本文件不依赖 STM32 HAL、ESP-IDF 或 Arduino API，只负责组帧、解帧和 payload 编解码；
 * 3. F4 侧拿到 out_frame 后用 HAL_UART_Transmit() 发送，ESP32S3 侧拿到 out_frame 后用 Serial.write() 或 uart_write_bytes() 发送。
 */

/* 固定帧头第 1 字节，用于从串口字节流中快速寻找一帧开始。 */
#define ARM_LINK_PROTOCOL_SOF0                  (0xA5U)

/* 固定帧头第 2 字节，用于降低误把普通数据当成协议帧的概率。 */
#define ARM_LINK_PROTOCOL_SOF1                  (0x5AU)

/* 首版协议版本，F4 和 ESP32S3 不一致时必须拒绝执行动作。 */
#define ARM_LINK_PROTOCOL_VERSION               (0x01U)

/* 固定帧尾，和 MP157-F4 主协议保持一致。 */
#define ARM_LINK_PROTOCOL_EOF                   (0x6BU)

/* 首版最大 payload 长度，保持短帧，便于 MCU 使用小缓存接收。 */
#define ARM_LINK_PROTOCOL_MAX_PAYLOAD_LENGTH    (48U)

/* 协议最短帧长度：A5 5A VER CMD LEN SEQ_L SEQ_H CRC_L CRC_H 6B。 */
#define ARM_LINK_PROTOCOL_MIN_FRAME_LENGTH      (10U)

/* 协议最大帧长度，调用者的发送和接收缓存至少要能容纳这个长度。 */
#define ARM_LINK_PROTOCOL_MAX_FRAME_LENGTH      (ARM_LINK_PROTOCOL_MIN_FRAME_LENGTH + ARM_LINK_PROTOCOL_MAX_PAYLOAD_LENGTH)

/* 动作命令 payload 固定长度，供 ARM_MOVE_TO_WEIGHT / ARM_MOVE_TO_LDC / ARM_SORT_RESULT / ARM_HOME / ARM_STOP 共用。 */
#define ARM_LINK_STAGE_COMMAND_PAYLOAD_LENGTH   (14U)

/* 动作完成 payload 固定长度，由 ESP32S3 在动作真正完成后发给 F4。 */
#define ARM_LINK_STAGE_DONE_PAYLOAD_LENGTH      (10U)

/* ACK payload 固定长度，用于确认命令已被接收和接受。 */
#define ARM_LINK_ACK_PAYLOAD_LENGTH             (7U)

/* NACK payload 固定长度，用于拒绝命令并说明错误原因。 */
#define ARM_LINK_NACK_PAYLOAD_LENGTH            (9U)

/* 机械臂链路命令字。 */
typedef enum
{
    ARM_LINK_CMD_HELLO = 0x01U,              /* 双向握手命令，不驱动机械臂动作。 */
    ARM_LINK_CMD_HEARTBEAT = 0x02U,          /* 双向心跳命令，用于确认链路在线。 */
    ARM_LINK_CMD_MOVE_TO_WEIGHT = 0x20U,     /* F4 要求 ESP32S3 从传送带/ROI 取件并放到称重模块。 */
    ARM_LINK_CMD_MOVE_TO_LDC = 0x21U,        /* F4 要求 ESP32S3 从称重模块取件并放到电磁感应模块。 */
    ARM_LINK_CMD_SORT_RESULT = 0x22U,        /* F4 要求 ESP32S3 从电磁感应模块取件并按结果分拣。 */
    ARM_LINK_CMD_HOME = 0x23U,               /* F4 要求 ESP32S3 回机械臂安全初始位。 */
    ARM_LINK_CMD_STOP = 0x24U,               /* F4 要求 ESP32S3 停止当前动作并进入安全状态。 */
    ARM_LINK_CMD_STAGE_DONE = 0x30U,         /* ESP32S3 通知 F4 某个动作阶段已经完成。 */
    ARM_LINK_CMD_STAGE_REPORT = 0x31U,       /* ESP32S3 可选上报动作进度。 */
    ARM_LINK_CMD_JOB_DONE = 0x32U,           /* ESP32S3 可选上报整套机械臂任务完成。 */
    ARM_LINK_CMD_ACK = 0x80U,                /* 命令接受回包。 */
    ARM_LINK_CMD_NACK = 0x81U,               /* 命令拒绝回包。 */
    ARM_LINK_CMD_FAULT_REPORT = 0x87U        /* 机械臂故障上报。 */
} ArmLink_Command_t;

/* 机械臂动作阶段编号。 */
typedef enum
{
    ARM_LINK_STAGE_NONE = 0U,                /* 无动作阶段。 */
    ARM_LINK_STAGE_PICK_BELT_TO_WEIGHT = 1U, /* 从传送带/ROI 抓取零件并放到称重模块。 */
    ARM_LINK_STAGE_WEIGHT_TO_LDC = 2U,       /* 从称重模块抓取零件并放到电磁感应模块。 */
    ARM_LINK_STAGE_LDC_TO_SORT_BIN = 3U,     /* 从电磁感应模块抓取零件并放到分拣区域。 */
    ARM_LINK_STAGE_HOME = 4U,                /* 回安全初始位。 */
    ARM_LINK_STAGE_STOP_SAFE = 5U            /* 停止当前动作并进入安全位。 */
} ArmLink_Stage_t;

/* 分拣目标编号。 */
typedef enum
{
    ARM_LINK_BIN_NONE = 0U,                  /* 不需要分拣目标，例如称重或电感阶段。 */
    ARM_LINK_BIN_GOOD = 1U,                  /* 良品区域。 */
    ARM_LINK_BIN_BAD = 2U,                   /* 不良品区域。 */
    ARM_LINK_BIN_REVIEW = 3U                 /* 待复核区域。 */
} ArmLink_TargetBin_t;

/* 机械臂状态编号，用于 ACK/NACK 中辅助 F4 判断 ESP32S3 当前状态。 */
typedef enum
{
    ARM_LINK_STATE_IDLE = 0U,                /* 空闲，等待命令。 */
    ARM_LINK_STATE_MOVING = 1U,              /* 正在执行动作。 */
    ARM_LINK_STATE_HOLDING_PART = 2U,        /* 已夹住零件。 */
    ARM_LINK_STATE_DONE = 3U,                /* 最近动作已完成。 */
    ARM_LINK_STATE_STOPPED = 4U,             /* 已停止。 */
    ARM_LINK_STATE_FAULT = 5U                /* 故障状态。 */
} ArmLink_State_t;

/* 错误码和动作结果码，0 表示成功，非 0 表示失败原因。 */
typedef enum
{
    ARM_LINK_RESULT_OK = 0U,                 /* 成功。 */
    ARM_LINK_ERROR_CRC = 1U,                 /* CRC 校验失败。 */
    ARM_LINK_ERROR_FRAME_LENGTH = 2U,        /* 帧长度错误。 */
    ARM_LINK_ERROR_CMD_UNKNOWN = 3U,         /* 命令字未知。 */
    ARM_LINK_ERROR_PAYLOAD_LENGTH = 4U,      /* payload 长度错误。 */
    ARM_LINK_ERROR_STATE_NOT_ALLOWED = 5U,   /* 当前状态不允许执行该命令。 */
    ARM_LINK_ERROR_BUSY = 6U,                /* 机械臂正忙。 */
    ARM_LINK_ERROR_CYCLE_MISMATCH = 7U,      /* cycle_id 不匹配。 */
    ARM_LINK_ERROR_MOTION_TIMEOUT = 8U,      /* 动作超时。 */
    ARM_LINK_ERROR_GRIP_FAILED = 9U,         /* 抓取失败或零件掉落。 */
    ARM_LINK_ERROR_SERVO_FAULT = 10U,        /* 舵机或舵机总线故障。 */
    ARM_LINK_ERROR_LIMIT_OR_ESTOP = 11U      /* 限位或急停触发。 */
} ArmLink_Result_t;

/* 解帧函数返回状态。 */
typedef enum
{
    ARM_LINK_PARSE_OK = 0,                   /* 帧格式、版本、长度、CRC 和帧尾都正确。 */
    ARM_LINK_PARSE_PARAM_ERROR,              /* 函数参数为空或输出缓存非法。 */
    ARM_LINK_PARSE_TOO_SHORT,                /* 输入长度小于最短帧。 */
    ARM_LINK_PARSE_TOO_LONG,                 /* 输入长度超过最大帧。 */
    ARM_LINK_PARSE_HEADER_ERROR,             /* 帧头不是 A5 5A。 */
    ARM_LINK_PARSE_VERSION_ERROR,            /* 版本号不匹配。 */
    ARM_LINK_PARSE_LENGTH_ERROR,             /* LEN 字段和实际帧长不一致。 */
    ARM_LINK_PARSE_EOF_ERROR,                /* 帧尾不是 6B。 */
    ARM_LINK_PARSE_CRC_ERROR                 /* CRC 校验失败。 */
} ArmLink_ParseStatus_t;

/* 解帧后的通用帧结构。 */
typedef struct
{
    uint8_t version;                         /* 协议版本。 */
    uint8_t command;                         /* 命令字。 */
    uint8_t payload_length;                  /* payload 有效长度。 */
    uint16_t sequence;                       /* 发送方帧序号。 */
    uint8_t payload[ARM_LINK_PROTOCOL_MAX_PAYLOAD_LENGTH]; /* payload 数据副本。 */
} ArmLink_Frame_t;

/* F4 发给 ESP32S3 的动作命令负载。 */
typedef struct
{
    uint16_t cycle_id;                       /* 当前单件流程 ID。 */
    uint8_t stage_id;                        /* 动作阶段编号。 */
    uint8_t part_type;                       /* 零件类型，未知填 0。 */
    uint8_t model_result;                    /* 模型结果，0 unknown，1 good，2 bad，3 review。 */
    uint8_t target_bin;                      /* 分拣目标，称重和电感阶段填 0。 */
    uint32_t timeout_ms;                     /* F4 允许动作运行的最长时间，单位 ms，按 u32 小端传输。 */
    uint8_t motion_profile;                  /* 运动方案编号，首版 0 表示默认动作。 */
    uint16_t flags;                          /* 动作标志位，首版填 0。 */
} ArmLink_StageCommandPayload_t;

/* ESP32S3 发给 F4 的动作完成负载。 */
typedef struct
{
    uint16_t cycle_id;                       /* 完成动作对应的流程 ID。 */
    uint8_t stage_id;                        /* 完成的动作阶段。 */
    uint8_t result;                          /* 动作结果，0 成功，非 0 失败。 */
    uint16_t detail_code;                    /* 失败或补充原因。 */
    uint16_t elapsed_ms;                     /* 动作耗时。 */
    uint16_t fault_bits;                     /* ESP32S3 侧故障位。 */
} ArmLink_StageDonePayload_t;

/* ACK 负载。 */
typedef struct
{
    uint16_t cycle_id;                       /* 对应流程 ID。 */
    uint16_t acked_sequence;                 /* 被确认帧的 SEQ。 */
    uint8_t acked_command;                   /* 被确认帧的 CMD。 */
    uint8_t status;                          /* 0 accepted，1 duplicate。 */
    uint8_t arm_state;                       /* 当前机械臂状态。 */
} ArmLink_AckPayload_t;

/* NACK 负载。 */
typedef struct
{
    uint16_t cycle_id;                       /* 对应流程 ID。 */
    uint16_t rejected_sequence;              /* 被拒绝帧的 SEQ。 */
    uint8_t rejected_command;                /* 被拒绝帧的 CMD。 */
    uint8_t error_code;                      /* 拒绝原因。 */
    uint8_t arm_state;                       /* 当前机械臂状态。 */
    uint16_t detail;                         /* 补充信息。 */
} ArmLink_NackPayload_t;

uint16_t ArmLinkProtocol_Crc16CcittFalse(const uint8_t *data, uint16_t length);

uint16_t ArmLinkProtocol_BuildFrame(uint8_t command,
                                    uint16_t sequence,
                                    const uint8_t *payload,
                                    uint8_t payload_length,
                                    uint8_t *out_frame,
                                    uint16_t out_frame_size);

ArmLink_ParseStatus_t ArmLinkProtocol_ParseFrame(const uint8_t *frame_buffer,
                                                 uint16_t frame_length,
                                                 ArmLink_Frame_t *out_frame);

uint8_t ArmLinkProtocol_EncodeStageCommandPayload(const ArmLink_StageCommandPayload_t *payload,
                                                  uint8_t *out_payload,
                                                  uint8_t out_payload_size);

uint8_t ArmLinkProtocol_DecodeStageCommandPayload(const uint8_t *payload_buffer,
                                                  uint8_t payload_length,
                                                  ArmLink_StageCommandPayload_t *out_payload);

uint8_t ArmLinkProtocol_EncodeStageDonePayload(const ArmLink_StageDonePayload_t *payload,
                                               uint8_t *out_payload,
                                               uint8_t out_payload_size);

uint8_t ArmLinkProtocol_DecodeStageDonePayload(const uint8_t *payload_buffer,
                                               uint8_t payload_length,
                                               ArmLink_StageDonePayload_t *out_payload);

uint8_t ArmLinkProtocol_EncodeAckPayload(const ArmLink_AckPayload_t *payload,
                                         uint8_t *out_payload,
                                         uint8_t out_payload_size);

uint8_t ArmLinkProtocol_DecodeAckPayload(const uint8_t *payload_buffer,
                                         uint8_t payload_length,
                                         ArmLink_AckPayload_t *out_payload);

uint8_t ArmLinkProtocol_EncodeNackPayload(const ArmLink_NackPayload_t *payload,
                                          uint8_t *out_payload,
                                          uint8_t out_payload_size);

uint8_t ArmLinkProtocol_DecodeNackPayload(const uint8_t *payload_buffer,
                                          uint8_t payload_length,
                                          ArmLink_NackPayload_t *out_payload);

uint16_t ArmLinkProtocol_BuildMoveToWeightFrame(uint16_t sequence,
                                                const ArmLink_StageCommandPayload_t *payload,
                                                uint8_t *out_frame,
                                                uint16_t out_frame_size);

uint16_t ArmLinkProtocol_BuildMoveToLdcFrame(uint16_t sequence,
                                             const ArmLink_StageCommandPayload_t *payload,
                                             uint8_t *out_frame,
                                             uint16_t out_frame_size);

uint16_t ArmLinkProtocol_BuildSortResultFrame(uint16_t sequence,
                                              const ArmLink_StageCommandPayload_t *payload,
                                              uint8_t *out_frame,
                                              uint16_t out_frame_size);

uint16_t ArmLinkProtocol_BuildHomeFrame(uint16_t sequence,
                                        const ArmLink_StageCommandPayload_t *payload,
                                        uint8_t *out_frame,
                                        uint16_t out_frame_size);

uint16_t ArmLinkProtocol_BuildStopFrame(uint16_t sequence,
                                        const ArmLink_StageCommandPayload_t *payload,
                                        uint8_t *out_frame,
                                        uint16_t out_frame_size);

uint16_t ArmLinkProtocol_BuildAckFrame(uint16_t sequence,
                                       const ArmLink_AckPayload_t *payload,
                                       uint8_t *out_frame,
                                       uint16_t out_frame_size);

uint16_t ArmLinkProtocol_BuildNackFrame(uint16_t sequence,
                                        const ArmLink_NackPayload_t *payload,
                                        uint8_t *out_frame,
                                        uint16_t out_frame_size);

uint16_t ArmLinkProtocol_BuildStageDoneFrame(uint16_t sequence,
                                             const ArmLink_StageDonePayload_t *payload,
                                             uint8_t *out_frame,
                                             uint16_t out_frame_size);

#ifdef __cplusplus
}
#endif

#endif
