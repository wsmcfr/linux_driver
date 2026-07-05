# F4 Actuator Motion Done Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make MP157 advance the automatic detection flow only after F4 reports that the Zhangdatou Emm42 actuator has physically reached the requested position.

**Architecture:** F4 still ACKs `ACTUATOR_POS_MOVE` immediately after accepting the command into the motor service, but the motor service then waits for the Emm42 reached reply `addr FD 9F 6B`. After the real reached reply arrives, F4 sends `EVENT_REPORT event=ACTUATOR_MOVE_DONE`; MP157 waits for that event before ROI re-check, focus settle, model detection, or robot-arm handoff.

**Tech Stack:** STM32F407 C firmware, FreeRTOS queues, STM32 HAL UART, Zhangdatou Emm42 TTL protocol, STM32MP157 Qt 5 C++ serial worker, QML automatic workflow state machine, shell static contract tests.

---

### Task 1: Lock the New Contract With Tests

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`
- Modify: `E:/hal/bisai_f407_project/User/App/binary_protocol_service_host_test.c`

**Steps:**
1. Add static checks for `ACTUATOR_MOVE_DONE`, `autoVisionHandleActuatorMoveDone`, and `runF4ActuatorPositionMoveAndWaitDone`.
2. Add F4 host-test assertions for `BINARY_PROTOCOL_EVENT_ACTUATOR_MOVE_DONE`, `BINARY_PROTOCOL_EVENT_ACTUATOR_MOVE_TIMEOUT`, and the 16-byte `EVENT_REPORT` payload contract.
3. Run the tests once and confirm they fail before implementation.

### Task 2: Parse Emm42 Reached Replies on F4

**Files:**
- Modify: `E:/hal/bisai_f407_project/User/Driver/emm42_motor.h`
- Modify: `E:/hal/bisai_f407_project/User/Driver/emm42_motor.c`

**Steps:**
1. Add a small reached-ACK parser that recognizes `addr FD 9F 6B`.
2. Flush stale RX bytes before position moves.
3. Poll UART non-blockingly from motor tasks so STOP commands can still be processed.
4. Add timeout estimation based on `steps / rpm` plus a safety margin.

### Task 3: Report Motion Completion From F4 Motor Services

**Files:**
- Modify: `E:/hal/bisai_f407_project/User/App/binary_protocol_service.h`
- Modify: `E:/hal/bisai_f407_project/User/App/binary_protocol_service.c`
- Modify: `E:/hal/bisai_f407_project/User/App/conveyor_motor_service.h`
- Modify: `E:/hal/bisai_f407_project/User/App/conveyor_motor_service.c`
- Modify: `E:/hal/bisai_f407_project/User/App/camera_motor_service.h`
- Modify: `E:/hal/bisai_f407_project/User/App/camera_motor_service.c`

**Steps:**
1. Define `EVENT_ACTUATOR_MOVE_DONE=0x14` and `EVENT_ACTUATOR_MOVE_TIMEOUT=0x15`.
2. Carry `cycle_id`, `related_seq`, actuator, and direction from `ACTUATOR_POS_MOVE` into the queued motor command.
3. After `addr FD 9F 6B`, send `EVENT_REPORT` with `related_seq` equal to the original MP157 frame sequence.
4. On timeout or UART parse error, set the motor fault bit, send a fault report, and send the timeout event.

### Task 4: Wait for Motion Done on MP157

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/main.cpp`
- Modify: `20_uvc_camera/qt_camera_display/qml/Main.qml`

**Steps:**
1. For `ACTUATOR_POS_MOVE`, keep the serial worker open after ACK and wait for `EVENT_REPORT event=ACTUATOR_MOVE_DONE`.
2. Fail immediately on `EVENT_ACTUATOR_MOVE_TIMEOUT`.
3. Update QML so Z down/up and fine-tune moves advance through `autoVisionHandleActuatorMoveDone()`, not through a local elapsed timer.
4. Keep the timer only as a UI timeout guard that stops the automatic flow and marks the part for review.

### Task 5: Update Documentation and Verify

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/README.md`
- Modify: `docs/stm32mp157-f407-binary-protocol.md`
- Modify: `docs/stm32mp157-f407-auto-detect-debug-roadmap.md`
- Modify: `E:/hal/bisai_f407_project/User/App/camera_motor_service.md`
- Modify: `E:/hal/bisai_f407_project/User/App/emm42_conveyor_code_guide.md`

**Steps:**
1. Document that Emm42 Response must be configured as `Reached` or `Both`.
2. Document `EVENT_REPORT event=ACTUATOR_MOVE_DONE` and timeout behavior.
3. Run MP157 static tests, upload helper self-tests, F4 host tests, and whitespace checks.
4. If the board and Keil toolchain are available, build/deploy MP157 and compile F4; otherwise report exactly which verification remains hardware-side.
