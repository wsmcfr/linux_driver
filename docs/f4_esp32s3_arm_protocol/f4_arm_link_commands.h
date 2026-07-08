#ifndef F4_ARM_LINK_COMMANDS_H
#define F4_ARM_LINK_COMMANDS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "arm_link_protocol.h"

#include <stdint.h>

/*
 * 本文件提供 F4 侧更好用的机械臂命令封装。
 * F4 自动流程不需要每次手工填写 stage_id、target_bin、motion_profile 等字段，
 * 只需要按业务阶段调用下面这些函数，再把 out_frame 通过 F4->ESP32S3 的 UART 发出去。
 */

uint16_t F4ArmLink_BuildMoveToWeightCommand(uint16_t sequence,
                                            uint16_t cycle_id,
                                            uint8_t part_type,
                                            uint8_t model_result,
                                            uint32_t timeout_ms,
                                            uint8_t *out_frame,
                                            uint16_t out_frame_size);

uint16_t F4ArmLink_BuildMoveToLdcCommand(uint16_t sequence,
                                         uint16_t cycle_id,
                                         uint8_t part_type,
                                         uint8_t model_result,
                                         uint32_t timeout_ms,
                                         uint8_t *out_frame,
                                         uint16_t out_frame_size);

uint16_t F4ArmLink_BuildSortResultCommand(uint16_t sequence,
                                          uint16_t cycle_id,
                                          uint8_t part_type,
                                          uint8_t model_result,
                                          uint8_t target_bin,
                                          uint32_t timeout_ms,
                                          uint8_t *out_frame,
                                          uint16_t out_frame_size);

uint8_t F4ArmLink_ModelResultToTargetBin(uint8_t model_result);

uint16_t F4ArmLink_BuildSortByModelResultCommand(uint16_t sequence,
                                                 uint16_t cycle_id,
                                                 uint8_t part_type,
                                                 uint8_t model_result,
                                                 uint32_t timeout_ms,
                                                 uint8_t *out_frame,
                                                 uint16_t out_frame_size);

uint16_t F4ArmLink_BuildHomeCommand(uint16_t sequence,
                                    uint16_t cycle_id,
                                    uint32_t timeout_ms,
                                    uint8_t *out_frame,
                                    uint16_t out_frame_size);

uint16_t F4ArmLink_BuildStopCommand(uint16_t sequence,
                                    uint16_t cycle_id,
                                    uint32_t timeout_ms,
                                    uint8_t *out_frame,
                                    uint16_t out_frame_size);

#ifdef __cplusplus
}
#endif

#endif
