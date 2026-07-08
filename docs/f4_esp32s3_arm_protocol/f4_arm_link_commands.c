#include "f4_arm_link_commands.h"

/*
 * 函数作用：填充 F4 发给 ESP32S3 的动作命令公共字段。
 * 主要流程：
 * 1. 清空业务不需要手填的字段；
 * 2. 写入 cycle_id、stage_id、零件类型、模型结果、目标区域和超时；
 * 3. 使用 motion_profile=0 和 flags=0，表示使用 ESP32S3 默认动作方案。
 * 参数 payload：输出动作命令结构，不能为 NULL。
 * 参数 cycle_id：当前单件流程 ID。
 * 参数 stage_id：动作阶段编号。
 * 参数 part_type：零件类型，未知填 0。
 * 参数 model_result：模型结果，未知填 0。
 * 参数 target_bin：分拣目标，非分拣阶段填 ARM_LINK_BIN_NONE。
 * 参数 timeout_ms：本动作允许的最长时间。
 * 返回值：成功返回 1，参数错误返回 0。
 */
static uint8_t F4ArmLink_FillStageCommand(ArmLink_StageCommandPayload_t *payload,
                                          uint16_t cycle_id,
                                          uint8_t stage_id,
                                          uint8_t part_type,
                                          uint8_t model_result,
                                          uint8_t target_bin,
                                          uint32_t timeout_ms)
{
    if (payload == (ArmLink_StageCommandPayload_t *)0)
    {
        return 0U;
    }

    payload->cycle_id = cycle_id;
    payload->stage_id = stage_id;
    payload->part_type = part_type;
    payload->model_result = model_result;
    payload->target_bin = target_bin;
    payload->timeout_ms = timeout_ms;
    payload->motion_profile = 0U;
    payload->flags = 0U;

    return 1U;
}

/*
 * 函数作用：构建“从传送带/ROI 抓取零件并放到称重模块”的完整 UART 帧。
 * 主要流程：填充 stage=PICK_BELT_TO_WEIGHT、target_bin=NONE，再调用共享协议组帧函数。
 * 参数 sequence：F4 发送序号，每发一帧自增。
 * 参数 cycle_id：当前单件流程 ID。
 * 参数 part_type：MP157 模型识别出的零件类型，未知填 0。
 * 参数 model_result：MP157 模型综合结果，未知填 0。
 * 参数 timeout_ms：允许机械臂完成本动作的最长时间。
 * 参数 out_frame：输出完整帧，F4 可直接用 UART 发送。
 * 参数 out_frame_size：输出缓存容量。
 * 返回值：成功返回完整帧长度，失败返回 0。
 */
uint16_t F4ArmLink_BuildMoveToWeightCommand(uint16_t sequence,
                                            uint16_t cycle_id,
                                            uint8_t part_type,
                                            uint8_t model_result,
                                            uint32_t timeout_ms,
                                            uint8_t *out_frame,
                                            uint16_t out_frame_size)
{
    ArmLink_StageCommandPayload_t payload;

    if (F4ArmLink_FillStageCommand(&payload,
                                   cycle_id,
                                   ARM_LINK_STAGE_PICK_BELT_TO_WEIGHT,
                                   part_type,
                                   model_result,
                                   ARM_LINK_BIN_NONE,
                                   timeout_ms) == 0U)
    {
        return 0U;
    }

    return ArmLinkProtocol_BuildMoveToWeightFrame(sequence, &payload, out_frame, out_frame_size);
}

/*
 * 函数作用：构建“从称重模块夹起零件并放到电磁感应模块”的完整 UART 帧。
 * 主要流程：填充 stage=WEIGHT_TO_LDC、target_bin=NONE，再调用共享协议组帧函数。
 * 返回值：成功返回完整帧长度，失败返回 0。
 */
uint16_t F4ArmLink_BuildMoveToLdcCommand(uint16_t sequence,
                                         uint16_t cycle_id,
                                         uint8_t part_type,
                                         uint8_t model_result,
                                         uint32_t timeout_ms,
                                         uint8_t *out_frame,
                                         uint16_t out_frame_size)
{
    ArmLink_StageCommandPayload_t payload;

    if (F4ArmLink_FillStageCommand(&payload,
                                   cycle_id,
                                   ARM_LINK_STAGE_WEIGHT_TO_LDC,
                                   part_type,
                                   model_result,
                                   ARM_LINK_BIN_NONE,
                                   timeout_ms) == 0U)
    {
        return 0U;
    }

    return ArmLinkProtocol_BuildMoveToLdcFrame(sequence, &payload, out_frame, out_frame_size);
}

/*
 * 函数作用：构建“从电磁感应模块夹起零件并放到分拣区域”的完整 UART 帧。
 * 主要流程：填充 stage=LDC_TO_SORT_BIN，并把 target_bin 写成 good/bad/review。
 * 参数 target_bin：分拣目标，只允许 ARM_LINK_BIN_GOOD、ARM_LINK_BIN_BAD、ARM_LINK_BIN_REVIEW。
 * 返回值：成功返回完整帧长度，target_bin 非法或组帧失败返回 0。
 */
uint16_t F4ArmLink_BuildSortResultCommand(uint16_t sequence,
                                          uint16_t cycle_id,
                                          uint8_t part_type,
                                          uint8_t model_result,
                                          uint8_t target_bin,
                                          uint32_t timeout_ms,
                                          uint8_t *out_frame,
                                          uint16_t out_frame_size)
{
    ArmLink_StageCommandPayload_t payload;

    if ((target_bin != ARM_LINK_BIN_GOOD) &&
        (target_bin != ARM_LINK_BIN_BAD) &&
        (target_bin != ARM_LINK_BIN_REVIEW))
    {
        return 0U;
    }

    if (F4ArmLink_FillStageCommand(&payload,
                                   cycle_id,
                                   ARM_LINK_STAGE_LDC_TO_SORT_BIN,
                                   part_type,
                                   model_result,
                                   target_bin,
                                   timeout_ms) == 0U)
    {
        return 0U;
    }

    return ArmLinkProtocol_BuildSortResultFrame(sequence, &payload, out_frame, out_frame_size);
}

/*
 * 函数作用：把 MP157 下发到 F4 的模型综合结果转换成机械臂分拣盘编号。
 * 主要流程：
 * 1. model_result=1 表示模型判定良品，映射到良品盘；
 * 2. model_result=2 表示模型判定不良品，映射到不良品盘；
 * 3. model_result=3 表示模型不确定，映射到待复核盘；
 * 4. model_result=0 或其它非法值都保守映射到待复核盘，避免把未知件误放到良品盘。
 * 参数 model_result：MP157 通过 MODEL_READY 发给 F4 的模型综合结果。
 * 返回值：ARM_LINK_BIN_GOOD、ARM_LINK_BIN_BAD 或 ARM_LINK_BIN_REVIEW。
 */
uint8_t F4ArmLink_ModelResultToTargetBin(uint8_t model_result)
{
    if (model_result == 1U)
    {
        return ARM_LINK_BIN_GOOD;
    }

    if (model_result == 2U)
    {
        return ARM_LINK_BIN_BAD;
    }

    return ARM_LINK_BIN_REVIEW;
}

/*
 * 函数作用：按 MP157 模型结果直接构建“分拣到对应盘子”的完整 UART 帧。
 * 主要流程：
 * 1. 调用 F4ArmLink_ModelResultToTargetBin() 得到目标盘；
 * 2. 使用 ARM_SORT_RESULT 命令让 ESP32S3 从电磁感应模块夹起零件并放入目标盘；
 * 3. 称重和电感结果首版不参与分拣盘选择，只随 F4 结果帧上传到 MP157。
 * 参数 sequence：F4 发送序号，每发一帧自增。
 * 参数 cycle_id：当前单件流程 ID。
 * 参数 part_type：MP157 模型识别出的零件类型，未知填 0。
 * 参数 model_result：MP157 模型综合结果，1 良品，2 不良品，3 待复核，其他值保守待复核。
 * 参数 timeout_ms：允许机械臂完成分拣动作的最长时间。
 * 参数 out_frame：输出完整帧，F4 可直接用 UART 发送。
 * 参数 out_frame_size：输出缓存容量。
 * 返回值：成功返回完整帧长度，失败返回 0。
 */
uint16_t F4ArmLink_BuildSortByModelResultCommand(uint16_t sequence,
                                                 uint16_t cycle_id,
                                                 uint8_t part_type,
                                                 uint8_t model_result,
                                                 uint32_t timeout_ms,
                                                 uint8_t *out_frame,
                                                 uint16_t out_frame_size)
{
    const uint8_t target_bin = F4ArmLink_ModelResultToTargetBin(model_result);

    return F4ArmLink_BuildSortResultCommand(sequence,
                                            cycle_id,
                                            part_type,
                                            model_result,
                                            target_bin,
                                            timeout_ms,
                                            out_frame,
                                            out_frame_size);
}

/*
 * 函数作用：构建“机械臂回安全初始位”的完整 UART 帧。
 * 主要流程：填充 stage=HOME，零件类型和模型结果都置 0，再调用共享协议组帧函数。
 * 返回值：成功返回完整帧长度，失败返回 0。
 */
uint16_t F4ArmLink_BuildHomeCommand(uint16_t sequence,
                                    uint16_t cycle_id,
                                    uint32_t timeout_ms,
                                    uint8_t *out_frame,
                                    uint16_t out_frame_size)
{
    ArmLink_StageCommandPayload_t payload;

    if (F4ArmLink_FillStageCommand(&payload,
                                   cycle_id,
                                   ARM_LINK_STAGE_HOME,
                                   0U,
                                   0U,
                                   ARM_LINK_BIN_NONE,
                                   timeout_ms) == 0U)
    {
        return 0U;
    }

    return ArmLinkProtocol_BuildHomeFrame(sequence, &payload, out_frame, out_frame_size);
}

/*
 * 函数作用：构建“停止当前机械臂动作并进入安全状态”的完整 UART 帧。
 * 主要流程：填充 stage=STOP_SAFE，零件类型和模型结果都置 0，再调用共享协议组帧函数。
 * 返回值：成功返回完整帧长度，失败返回 0。
 */
uint16_t F4ArmLink_BuildStopCommand(uint16_t sequence,
                                    uint16_t cycle_id,
                                    uint32_t timeout_ms,
                                    uint8_t *out_frame,
                                    uint16_t out_frame_size)
{
    ArmLink_StageCommandPayload_t payload;

    if (F4ArmLink_FillStageCommand(&payload,
                                   cycle_id,
                                   ARM_LINK_STAGE_STOP_SAFE,
                                   0U,
                                   0U,
                                   ARM_LINK_BIN_NONE,
                                   timeout_ms) == 0U)
    {
        return 0U;
    }

    return ArmLinkProtocol_BuildStopFrame(sequence, &payload, out_frame, out_frame_size);
}
