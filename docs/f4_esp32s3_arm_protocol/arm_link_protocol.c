#include "arm_link_protocol.h"

#include <string.h>

/*
 * 本文件只做协议层的纯 C 处理：
 * - 不直接访问 UART；
 * - 不直接控制舵机；
 * - 不直接创建任务或队列；
 * - F4 和 ESP32S3 可以把本文件加入各自工程，共用同一套 CRC 和 payload 编解码。
 */

/*
 * 函数作用：把 16 位无符号整数按小端序写入缓冲区。
 * 主要流程：低字节写在前，高字节写在后。
 * 参数 buffer：至少能写 2 字节的目标缓冲区。
 * 参数 value：要写入的 16 位数。
 * 返回值：无。
 */
static void ArmLinkProtocol_WriteU16Le(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
}

/*
 * 函数作用：把 32 位无符号整数按小端序写入缓冲区。
 * 主要流程：最低有效字节写在前，最高有效字节写在最后。
 * 参数 buffer：至少能写 4 字节的目标缓冲区。
 * 参数 value：要写入的 32 位数。
 * 返回值：无。
 */
static void ArmLinkProtocol_WriteU32Le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFUL);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFUL);
    buffer[2] = (uint8_t)((value >> 16) & 0xFFUL);
    buffer[3] = (uint8_t)((value >> 24) & 0xFFUL);
}

/*
 * 函数作用：从小端序缓冲区读取 16 位无符号整数。
 * 主要流程：先读低字节，再把高字节左移 8 位合并。
 * 参数 buffer：至少包含 2 字节的源缓冲区。
 * 返回值：读取到的 16 位数。
 */
static uint16_t ArmLinkProtocol_ReadU16Le(const uint8_t *buffer)
{
    return (uint16_t)(((uint16_t)buffer[0]) | ((uint16_t)buffer[1] << 8));
}

/*
 * 函数作用：从小端序缓冲区读取 32 位无符号整数。
 * 主要流程：按低字节到高字节合并，得到 F4 写入的毫秒级动作超时。
 * 参数 buffer：至少包含 4 字节的源缓冲区。
 * 返回值：读取到的 32 位数。
 */
static uint32_t ArmLinkProtocol_ReadU32Le(const uint8_t *buffer)
{
    return ((uint32_t)buffer[0]) |
           ((uint32_t)buffer[1] << 8) |
           ((uint32_t)buffer[2] << 16) |
           ((uint32_t)buffer[3] << 24);
}

/*
 * 函数作用：计算 F4-ESP32S3 机械臂协议使用的 CRC16-CCITT-FALSE。
 * 主要流程：使用 0xFFFF 初值，按多项式 0x1021 逐字节、逐 bit 左移计算。
 * 参数 data：指向参与 CRC 的数据，协议中应从 VER 字段开始。
 * 参数 length：参与 CRC 的字节数，协议中应覆盖 VER、CMD、LEN、SEQ 和 PAYLOAD。
 * 返回值：16 位 CRC，组帧时低字节在前。
 */
uint16_t ArmLinkProtocol_Crc16CcittFalse(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFFU;
    uint16_t index = 0U;
    uint8_t bit = 0U;

    if (data == (const uint8_t *)0)
    {
        return 0U;
    }

    for (index = 0U; index < length; index++)
    {
        crc ^= (uint16_t)((uint16_t)data[index] << 8);

        for (bit = 0U; bit < 8U; bit++)
        {
            if ((crc & 0x8000U) != 0U)
            {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            }
            else
            {
                crc = (uint16_t)(crc << 1);
            }
        }
    }

    return crc;
}

/*
 * 函数作用：构建一帧可直接发送到 UART 的机械臂协议帧。
 * 主要流程：
 * 1. 校验 payload 长度和输出缓存容量；
 * 2. 写入帧头、版本、命令、长度、序号和 payload；
 * 3. 对 VER 到 PAYLOAD 计算 CRC；
 * 4. 写入 CRC 和帧尾。
 * 参数 command：命令字。
 * 参数 sequence：发送方帧序号。
 * 参数 payload：payload 指针，payload_length 为 0 时可以为 NULL。
 * 参数 payload_length：payload 字节数，不能超过 ARM_LINK_PROTOCOL_MAX_PAYLOAD_LENGTH。
 * 参数 out_frame：输出完整帧的缓冲区。
 * 参数 out_frame_size：输出缓冲区容量。
 * 返回值：成功返回完整帧长度；失败返回 0。
 */
uint16_t ArmLinkProtocol_BuildFrame(uint8_t command,
                                    uint16_t sequence,
                                    const uint8_t *payload,
                                    uint8_t payload_length,
                                    uint8_t *out_frame,
                                    uint16_t out_frame_size)
{
    uint16_t frame_length = 0U;
    uint16_t crc = 0U;
    uint16_t crc_offset = 0U;

    if (out_frame == (uint8_t *)0)
    {
        return 0U;
    }

    if (payload_length > ARM_LINK_PROTOCOL_MAX_PAYLOAD_LENGTH)
    {
        return 0U;
    }

    if ((payload_length > 0U) && (payload == (const uint8_t *)0))
    {
        return 0U;
    }

    frame_length = (uint16_t)(ARM_LINK_PROTOCOL_MIN_FRAME_LENGTH + payload_length);
    if (out_frame_size < frame_length)
    {
        return 0U;
    }

    out_frame[0] = ARM_LINK_PROTOCOL_SOF0;
    out_frame[1] = ARM_LINK_PROTOCOL_SOF1;
    out_frame[2] = ARM_LINK_PROTOCOL_VERSION;
    out_frame[3] = command;
    out_frame[4] = payload_length;
    ArmLinkProtocol_WriteU16Le(&out_frame[5], sequence);

    if (payload_length > 0U)
    {
        (void)memcpy(&out_frame[7], payload, payload_length);
    }

    crc = ArmLinkProtocol_Crc16CcittFalse(&out_frame[2], (uint16_t)(5U + payload_length));
    crc_offset = (uint16_t)(7U + payload_length);
    ArmLinkProtocol_WriteU16Le(&out_frame[crc_offset], crc);
    out_frame[crc_offset + 2U] = ARM_LINK_PROTOCOL_EOF;

    return frame_length;
}

/*
 * 函数作用：解析一帧机械臂协议。
 * 主要流程：
 * 1. 校验指针和长度；
 * 2. 校验帧头、版本、LEN 和帧尾；
 * 3. 计算 CRC 并与帧内 CRC 比较；
 * 4. 把命令、序号和 payload 复制到 out_frame。
 * 参数 frame_buffer：输入完整帧缓存。
 * 参数 frame_length：输入完整帧长度。
 * 参数 out_frame：解析结果输出结构。
 * 返回值：解析状态，只有 ARM_LINK_PARSE_OK 表示可以分发命令。
 */
ArmLink_ParseStatus_t ArmLinkProtocol_ParseFrame(const uint8_t *frame_buffer,
                                                 uint16_t frame_length,
                                                 ArmLink_Frame_t *out_frame)
{
    uint8_t payload_length = 0U;
    uint16_t expected_length = 0U;
    uint16_t received_crc = 0U;
    uint16_t calculated_crc = 0U;
    uint16_t crc_offset = 0U;

    if ((frame_buffer == (const uint8_t *)0) || (out_frame == (ArmLink_Frame_t *)0))
    {
        return ARM_LINK_PARSE_PARAM_ERROR;
    }

    if (frame_length < ARM_LINK_PROTOCOL_MIN_FRAME_LENGTH)
    {
        return ARM_LINK_PARSE_TOO_SHORT;
    }

    if (frame_length > ARM_LINK_PROTOCOL_MAX_FRAME_LENGTH)
    {
        return ARM_LINK_PARSE_TOO_LONG;
    }

    if ((frame_buffer[0] != ARM_LINK_PROTOCOL_SOF0) || (frame_buffer[1] != ARM_LINK_PROTOCOL_SOF1))
    {
        return ARM_LINK_PARSE_HEADER_ERROR;
    }

    if (frame_buffer[2] != ARM_LINK_PROTOCOL_VERSION)
    {
        return ARM_LINK_PARSE_VERSION_ERROR;
    }

    payload_length = frame_buffer[4];
    if (payload_length > ARM_LINK_PROTOCOL_MAX_PAYLOAD_LENGTH)
    {
        return ARM_LINK_PARSE_LENGTH_ERROR;
    }

    expected_length = (uint16_t)(ARM_LINK_PROTOCOL_MIN_FRAME_LENGTH + payload_length);
    if (frame_length != expected_length)
    {
        return ARM_LINK_PARSE_LENGTH_ERROR;
    }

    if (frame_buffer[frame_length - 1U] != ARM_LINK_PROTOCOL_EOF)
    {
        return ARM_LINK_PARSE_EOF_ERROR;
    }

    crc_offset = (uint16_t)(7U + payload_length);
    received_crc = ArmLinkProtocol_ReadU16Le(&frame_buffer[crc_offset]);
    calculated_crc = ArmLinkProtocol_Crc16CcittFalse(&frame_buffer[2], (uint16_t)(5U + payload_length));
    if (received_crc != calculated_crc)
    {
        return ARM_LINK_PARSE_CRC_ERROR;
    }

    out_frame->version = frame_buffer[2];
    out_frame->command = frame_buffer[3];
    out_frame->payload_length = payload_length;
    out_frame->sequence = ArmLinkProtocol_ReadU16Le(&frame_buffer[5]);
    if (payload_length > 0U)
    {
        (void)memcpy(out_frame->payload, &frame_buffer[7], payload_length);
    }

    return ARM_LINK_PARSE_OK;
}

/*
 * 函数作用：把动作命令结构编码成 14 字节 payload。
 * 参数 payload：动作命令结构，不能为 NULL。
 * 参数 out_payload：输出 payload 缓冲区。
 * 参数 out_payload_size：输出缓冲区容量，至少 14 字节。
 * 返回值：成功返回 14，失败返回 0。
 */
uint8_t ArmLinkProtocol_EncodeStageCommandPayload(const ArmLink_StageCommandPayload_t *payload,
                                                  uint8_t *out_payload,
                                                  uint8_t out_payload_size)
{
    if ((payload == (const ArmLink_StageCommandPayload_t *)0) || (out_payload == (uint8_t *)0))
    {
        return 0U;
    }

    if (out_payload_size < ARM_LINK_STAGE_COMMAND_PAYLOAD_LENGTH)
    {
        return 0U;
    }

    ArmLinkProtocol_WriteU16Le(&out_payload[0], payload->cycle_id);
    out_payload[2] = payload->stage_id;
    out_payload[3] = payload->part_type;
    out_payload[4] = payload->model_result;
    out_payload[5] = payload->target_bin;
    ArmLinkProtocol_WriteU32Le(&out_payload[6], payload->timeout_ms);
    out_payload[10] = payload->motion_profile;
    ArmLinkProtocol_WriteU16Le(&out_payload[11], payload->flags);
    out_payload[13] = 0U;

    return ARM_LINK_STAGE_COMMAND_PAYLOAD_LENGTH;
}

/*
 * 函数作用：把 14 字节动作命令 payload 解码成结构体。
 * 参数 payload_buffer：输入 payload 缓冲区。
 * 参数 payload_length：输入 payload 长度，必须为 14。
 * 参数 out_payload：输出结构体。
 * 返回值：成功返回 1，失败返回 0。
 */
uint8_t ArmLinkProtocol_DecodeStageCommandPayload(const uint8_t *payload_buffer,
                                                  uint8_t payload_length,
                                                  ArmLink_StageCommandPayload_t *out_payload)
{
    if ((payload_buffer == (const uint8_t *)0) || (out_payload == (ArmLink_StageCommandPayload_t *)0))
    {
        return 0U;
    }

    if (payload_length != ARM_LINK_STAGE_COMMAND_PAYLOAD_LENGTH)
    {
        return 0U;
    }

    out_payload->cycle_id = ArmLinkProtocol_ReadU16Le(&payload_buffer[0]);
    out_payload->stage_id = payload_buffer[2];
    out_payload->part_type = payload_buffer[3];
    out_payload->model_result = payload_buffer[4];
    out_payload->target_bin = payload_buffer[5];
    out_payload->timeout_ms = ArmLinkProtocol_ReadU32Le(&payload_buffer[6]);
    out_payload->motion_profile = payload_buffer[10];
    out_payload->flags = ArmLinkProtocol_ReadU16Le(&payload_buffer[11]);

    return 1U;
}

/*
 * 函数作用：把动作完成结构编码成 10 字节 payload。
 * 参数 payload：动作完成结构，不能为 NULL。
 * 参数 out_payload：输出 payload 缓冲区。
 * 参数 out_payload_size：输出缓冲区容量，至少 10 字节。
 * 返回值：成功返回 10，失败返回 0。
 */
uint8_t ArmLinkProtocol_EncodeStageDonePayload(const ArmLink_StageDonePayload_t *payload,
                                               uint8_t *out_payload,
                                               uint8_t out_payload_size)
{
    if ((payload == (const ArmLink_StageDonePayload_t *)0) || (out_payload == (uint8_t *)0))
    {
        return 0U;
    }

    if (out_payload_size < ARM_LINK_STAGE_DONE_PAYLOAD_LENGTH)
    {
        return 0U;
    }

    ArmLinkProtocol_WriteU16Le(&out_payload[0], payload->cycle_id);
    out_payload[2] = payload->stage_id;
    out_payload[3] = payload->result;
    ArmLinkProtocol_WriteU16Le(&out_payload[4], payload->detail_code);
    ArmLinkProtocol_WriteU16Le(&out_payload[6], payload->elapsed_ms);
    ArmLinkProtocol_WriteU16Le(&out_payload[8], payload->fault_bits);

    return ARM_LINK_STAGE_DONE_PAYLOAD_LENGTH;
}

/*
 * 函数作用：把 10 字节动作完成 payload 解码成结构体。
 * 参数 payload_buffer：输入 payload 缓冲区。
 * 参数 payload_length：输入 payload 长度，必须为 10。
 * 参数 out_payload：输出结构体。
 * 返回值：成功返回 1，失败返回 0。
 */
uint8_t ArmLinkProtocol_DecodeStageDonePayload(const uint8_t *payload_buffer,
                                               uint8_t payload_length,
                                               ArmLink_StageDonePayload_t *out_payload)
{
    if ((payload_buffer == (const uint8_t *)0) || (out_payload == (ArmLink_StageDonePayload_t *)0))
    {
        return 0U;
    }

    if (payload_length != ARM_LINK_STAGE_DONE_PAYLOAD_LENGTH)
    {
        return 0U;
    }

    out_payload->cycle_id = ArmLinkProtocol_ReadU16Le(&payload_buffer[0]);
    out_payload->stage_id = payload_buffer[2];
    out_payload->result = payload_buffer[3];
    out_payload->detail_code = ArmLinkProtocol_ReadU16Le(&payload_buffer[4]);
    out_payload->elapsed_ms = ArmLinkProtocol_ReadU16Le(&payload_buffer[6]);
    out_payload->fault_bits = ArmLinkProtocol_ReadU16Le(&payload_buffer[8]);

    return 1U;
}

/*
 * 函数作用：编码 ACK payload。
 * 参数 payload：ACK 结构。
 * 参数 out_payload：输出 payload 缓冲区。
 * 参数 out_payload_size：输出缓冲区容量，至少 7 字节。
 * 返回值：成功返回 7，失败返回 0。
 */
uint8_t ArmLinkProtocol_EncodeAckPayload(const ArmLink_AckPayload_t *payload,
                                         uint8_t *out_payload,
                                         uint8_t out_payload_size)
{
    if ((payload == (const ArmLink_AckPayload_t *)0) || (out_payload == (uint8_t *)0))
    {
        return 0U;
    }

    if (out_payload_size < ARM_LINK_ACK_PAYLOAD_LENGTH)
    {
        return 0U;
    }

    ArmLinkProtocol_WriteU16Le(&out_payload[0], payload->cycle_id);
    ArmLinkProtocol_WriteU16Le(&out_payload[2], payload->acked_sequence);
    out_payload[4] = payload->acked_command;
    out_payload[5] = payload->status;
    out_payload[6] = payload->arm_state;

    return ARM_LINK_ACK_PAYLOAD_LENGTH;
}

/*
 * 函数作用：解码 ACK payload。
 * 参数 payload_buffer：输入 payload。
 * 参数 payload_length：输入长度，必须为 7。
 * 参数 out_payload：输出结构。
 * 返回值：成功返回 1，失败返回 0。
 */
uint8_t ArmLinkProtocol_DecodeAckPayload(const uint8_t *payload_buffer,
                                         uint8_t payload_length,
                                         ArmLink_AckPayload_t *out_payload)
{
    if ((payload_buffer == (const uint8_t *)0) || (out_payload == (ArmLink_AckPayload_t *)0))
    {
        return 0U;
    }

    if (payload_length != ARM_LINK_ACK_PAYLOAD_LENGTH)
    {
        return 0U;
    }

    out_payload->cycle_id = ArmLinkProtocol_ReadU16Le(&payload_buffer[0]);
    out_payload->acked_sequence = ArmLinkProtocol_ReadU16Le(&payload_buffer[2]);
    out_payload->acked_command = payload_buffer[4];
    out_payload->status = payload_buffer[5];
    out_payload->arm_state = payload_buffer[6];

    return 1U;
}

/*
 * 函数作用：编码 NACK payload。
 * 参数 payload：NACK 结构。
 * 参数 out_payload：输出 payload 缓冲区。
 * 参数 out_payload_size：输出缓冲区容量，至少 9 字节。
 * 返回值：成功返回 9，失败返回 0。
 */
uint8_t ArmLinkProtocol_EncodeNackPayload(const ArmLink_NackPayload_t *payload,
                                          uint8_t *out_payload,
                                          uint8_t out_payload_size)
{
    if ((payload == (const ArmLink_NackPayload_t *)0) || (out_payload == (uint8_t *)0))
    {
        return 0U;
    }

    if (out_payload_size < ARM_LINK_NACK_PAYLOAD_LENGTH)
    {
        return 0U;
    }

    ArmLinkProtocol_WriteU16Le(&out_payload[0], payload->cycle_id);
    ArmLinkProtocol_WriteU16Le(&out_payload[2], payload->rejected_sequence);
    out_payload[4] = payload->rejected_command;
    out_payload[5] = payload->error_code;
    out_payload[6] = payload->arm_state;
    ArmLinkProtocol_WriteU16Le(&out_payload[7], payload->detail);

    return ARM_LINK_NACK_PAYLOAD_LENGTH;
}

/*
 * 函数作用：解码 NACK payload。
 * 参数 payload_buffer：输入 payload。
 * 参数 payload_length：输入长度，必须为 9。
 * 参数 out_payload：输出结构。
 * 返回值：成功返回 1，失败返回 0。
 */
uint8_t ArmLinkProtocol_DecodeNackPayload(const uint8_t *payload_buffer,
                                          uint8_t payload_length,
                                          ArmLink_NackPayload_t *out_payload)
{
    if ((payload_buffer == (const uint8_t *)0) || (out_payload == (ArmLink_NackPayload_t *)0))
    {
        return 0U;
    }

    if (payload_length != ARM_LINK_NACK_PAYLOAD_LENGTH)
    {
        return 0U;
    }

    out_payload->cycle_id = ArmLinkProtocol_ReadU16Le(&payload_buffer[0]);
    out_payload->rejected_sequence = ArmLinkProtocol_ReadU16Le(&payload_buffer[2]);
    out_payload->rejected_command = payload_buffer[4];
    out_payload->error_code = payload_buffer[5];
    out_payload->arm_state = payload_buffer[6];
    out_payload->detail = ArmLinkProtocol_ReadU16Le(&payload_buffer[7]);

    return 1U;
}

/*
 * 函数作用：构建动作命令帧的内部公共函数。
 * 参数 command：动作命令字。
 * 参数 sequence：发送方帧序号。
 * 参数 payload：动作命令结构。
 * 参数 out_frame：完整帧输出缓存。
 * 参数 out_frame_size：输出缓存容量。
 * 返回值：成功返回完整帧长度，失败返回 0。
 */
static uint16_t ArmLinkProtocol_BuildStageCommandFrame(uint8_t command,
                                                       uint16_t sequence,
                                                       const ArmLink_StageCommandPayload_t *payload,
                                                       uint8_t *out_frame,
                                                       uint16_t out_frame_size)
{
    uint8_t encoded_payload[ARM_LINK_STAGE_COMMAND_PAYLOAD_LENGTH];
    uint8_t encoded_length = 0U;

    encoded_length = ArmLinkProtocol_EncodeStageCommandPayload(payload,
                                                               encoded_payload,
                                                               (uint8_t)sizeof(encoded_payload));
    if (encoded_length == 0U)
    {
        return 0U;
    }

    return ArmLinkProtocol_BuildFrame(command, sequence, encoded_payload, encoded_length, out_frame, out_frame_size);
}

/*
 * 函数作用：F4 构建“抓取零件并放到称重模块”的完整帧。
 * 返回值：成功返回完整帧长度，失败返回 0。
 */
uint16_t ArmLinkProtocol_BuildMoveToWeightFrame(uint16_t sequence,
                                                const ArmLink_StageCommandPayload_t *payload,
                                                uint8_t *out_frame,
                                                uint16_t out_frame_size)
{
    return ArmLinkProtocol_BuildStageCommandFrame(ARM_LINK_CMD_MOVE_TO_WEIGHT,
                                                  sequence,
                                                  payload,
                                                  out_frame,
                                                  out_frame_size);
}

/*
 * 函数作用：F4 构建“从称重模块移动到电磁感应模块”的完整帧。
 * 返回值：成功返回完整帧长度，失败返回 0。
 */
uint16_t ArmLinkProtocol_BuildMoveToLdcFrame(uint16_t sequence,
                                             const ArmLink_StageCommandPayload_t *payload,
                                             uint8_t *out_frame,
                                             uint16_t out_frame_size)
{
    return ArmLinkProtocol_BuildStageCommandFrame(ARM_LINK_CMD_MOVE_TO_LDC,
                                                  sequence,
                                                  payload,
                                                  out_frame,
                                                  out_frame_size);
}

/*
 * 函数作用：F4 构建“按检测结果分拣零件”的完整帧。
 * 返回值：成功返回完整帧长度，失败返回 0。
 */
uint16_t ArmLinkProtocol_BuildSortResultFrame(uint16_t sequence,
                                              const ArmLink_StageCommandPayload_t *payload,
                                              uint8_t *out_frame,
                                              uint16_t out_frame_size)
{
    return ArmLinkProtocol_BuildStageCommandFrame(ARM_LINK_CMD_SORT_RESULT,
                                                  sequence,
                                                  payload,
                                                  out_frame,
                                                  out_frame_size);
}

/*
 * 函数作用：F4 构建“机械臂回安全位”的完整帧。
 * 返回值：成功返回完整帧长度，失败返回 0。
 */
uint16_t ArmLinkProtocol_BuildHomeFrame(uint16_t sequence,
                                        const ArmLink_StageCommandPayload_t *payload,
                                        uint8_t *out_frame,
                                        uint16_t out_frame_size)
{
    return ArmLinkProtocol_BuildStageCommandFrame(ARM_LINK_CMD_HOME,
                                                  sequence,
                                                  payload,
                                                  out_frame,
                                                  out_frame_size);
}

/*
 * 函数作用：F4 构建“停止当前机械臂动作”的完整帧。
 * 返回值：成功返回完整帧长度，失败返回 0。
 */
uint16_t ArmLinkProtocol_BuildStopFrame(uint16_t sequence,
                                        const ArmLink_StageCommandPayload_t *payload,
                                        uint8_t *out_frame,
                                        uint16_t out_frame_size)
{
    return ArmLinkProtocol_BuildStageCommandFrame(ARM_LINK_CMD_STOP,
                                                  sequence,
                                                  payload,
                                                  out_frame,
                                                  out_frame_size);
}

/*
 * 函数作用：构建 ACK 完整帧。
 * 返回值：成功返回完整帧长度，失败返回 0。
 */
uint16_t ArmLinkProtocol_BuildAckFrame(uint16_t sequence,
                                       const ArmLink_AckPayload_t *payload,
                                       uint8_t *out_frame,
                                       uint16_t out_frame_size)
{
    uint8_t encoded_payload[ARM_LINK_ACK_PAYLOAD_LENGTH];
    uint8_t encoded_length = 0U;

    encoded_length = ArmLinkProtocol_EncodeAckPayload(payload, encoded_payload, (uint8_t)sizeof(encoded_payload));
    if (encoded_length == 0U)
    {
        return 0U;
    }

    return ArmLinkProtocol_BuildFrame(ARM_LINK_CMD_ACK, sequence, encoded_payload, encoded_length, out_frame, out_frame_size);
}

/*
 * 函数作用：构建 NACK 完整帧。
 * 返回值：成功返回完整帧长度，失败返回 0。
 */
uint16_t ArmLinkProtocol_BuildNackFrame(uint16_t sequence,
                                        const ArmLink_NackPayload_t *payload,
                                        uint8_t *out_frame,
                                        uint16_t out_frame_size)
{
    uint8_t encoded_payload[ARM_LINK_NACK_PAYLOAD_LENGTH];
    uint8_t encoded_length = 0U;

    encoded_length = ArmLinkProtocol_EncodeNackPayload(payload, encoded_payload, (uint8_t)sizeof(encoded_payload));
    if (encoded_length == 0U)
    {
        return 0U;
    }

    return ArmLinkProtocol_BuildFrame(ARM_LINK_CMD_NACK, sequence, encoded_payload, encoded_length, out_frame, out_frame_size);
}

/*
 * 函数作用：构建动作完成完整帧。
 * 返回值：成功返回完整帧长度，失败返回 0。
 */
uint16_t ArmLinkProtocol_BuildStageDoneFrame(uint16_t sequence,
                                             const ArmLink_StageDonePayload_t *payload,
                                             uint8_t *out_frame,
                                             uint16_t out_frame_size)
{
    uint8_t encoded_payload[ARM_LINK_STAGE_DONE_PAYLOAD_LENGTH];
    uint8_t encoded_length = 0U;

    encoded_length = ArmLinkProtocol_EncodeStageDonePayload(payload, encoded_payload, (uint8_t)sizeof(encoded_payload));
    if (encoded_length == 0U)
    {
        return 0U;
    }

    return ArmLinkProtocol_BuildFrame(ARM_LINK_CMD_STAGE_DONE, sequence, encoded_payload, encoded_length, out_frame, out_frame_size);
}
