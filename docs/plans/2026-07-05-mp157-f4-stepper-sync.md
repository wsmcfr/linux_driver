# MP157 F4 Stepper Sync Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 修复 MP157 自动流程速度、Z 轴完成等待和 F4 步进参数同步，使参数页设置对手动和自动流程同时生效。

**Architecture:** MP157 继续作为参数源，启动和保存时通过 `STEPPER_PARAM_SET 0x42` 下发三台电机运行参数。F4 扩展同一命令的单台记录，接收传送带上料扫描速度和对中/微调速度，并在自动流程不同阶段使用对应速度。

**Tech Stack:** Qt/QML、C++、STM32F407 C、MP157-F4 二进制协议、项目静态 shell 契约测试。

---

### Task 1: 扩展 MP157 静态契约测试

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`

**Steps:**
- 增加 `scan_speed_rpm`、`conveyorScanSpeedRpm`、`conveyorTrackSpeedRpm`、开机同步和 31 字节负载检查。
- 运行 `bash -lc './test_qt_kms_overlay_assets.sh'`，预期先失败在缺少新字段。

### Task 2: 修改 MP157 C++ 参数模型和协议负载

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/main.cpp`

**Steps:**
- 给 `StepperMotorSettings` 增加传送带专用 `scanSpeedRpm`。
- JSON 读写、QML map、日志、自检摘要同步增加 `scan_speed_rpm`。
- `STEPPER_PARAM_SET` 负载改为 `cycle_id + motor_count + flags + 3 * 9`，每台记录包含 `role/address/min_step/normal_speed_rpm/scan_speed_rpm/direction`。

### Task 3: 修改 MP157 QML 自动流程和参数同步

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/qml/Main.qml`

**Steps:**
- 去掉自动流程固定 `40rpm` 限速，改为读取参数页速度。
- 传送带自动扫描使用 `scanSpeedRpm`，居中后短步微调用 `normalSpeedRpm`。
- `ACTUATOR_POS_MOVE` 的成功回调直接按 C++ 已等到的 `actuator-move-done` 推进，不再叠加本地估算等待。
- `Component.onCompleted` 启动短延时同步，参数页保存也统一下发 F4。

### Task 4: 修改 F4 参数解析和传送带运行参数

**Files:**
- Modify: `E:\hal\bisai_f407_project\User\App\binary_protocol_service.c`
- Modify: `E:\hal\bisai_f407_project\User\App\binary_protocol_service.h`
- Modify: `E:\hal\bisai_f407_project\User\App\conveyor_motor_service.c`
- Modify: `E:\hal\bisai_f407_project\User\App\conveyor_motor_service.h`
- Modify as needed: `E:\hal\bisai_f407_project\User\App\camera_motor_service.c/.h`

**Steps:**
- 让 F4 `STEPPER_PARAM_SET` 接收 9 字节单电机记录。
- 传送带保存 `scan_speed_rpm` 和 `normal_speed_rpm` 两档速度。
- `START_CYCLE` 扫描阶段使用 `scan_speed_rpm`，`VISION_POS` 跟踪和 `ACTUATOR_POS_MOVE actuator=0` 微调用 `normal_speed_rpm`。

### Task 5: 同步文档和验证

**Files:**
- Modify: `docs/stm32mp157-f407-binary-protocol.md`
- Modify: `docs/stm32mp157-f407-auto-detect-debug-roadmap.md`
- Modify: `20_uvc_camera/qt_camera_display/README.md`

**Steps:**
- 写明 `STEPPER_PARAM_SET` 新负载长度、双速度含义、开机同步行为和部署/烧录注意。
- 运行 MP157 静态测试、`git diff --check`。
- 对 F4 至少运行可用的源码静态搜索/构建命令；如果 Keil 命令行不可用，明确说明未构建。
