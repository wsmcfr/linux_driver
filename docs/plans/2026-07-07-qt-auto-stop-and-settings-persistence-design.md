# Qt 自动停止与参数持久化设计

**问题背景**

- 首页自动流程当前把“自动循环会话是否还在”和“当前这一件零件的 cycle 是否仍在 F4 运行中”混成了一套状态。
- 一轮检测结束收到 `CYCLE_DONE` 后，QML 侧没有正确进入“下一轮待启动”或“会话已停止”的明确状态，C++ 侧也继续把旧 cycle 视为运行中。
- 参数设置页虽然设计为读写 `/mnt/sdcard/config/defect_ui_config.json`，但当前界面值、保存结果提示和重启后的回显闭环不够明确，现场容易看到默认显示值而不是上次保存值。

**目标**

- `停止` 按钮语义固定为“终止整个自动循环会话，后续必须重新点开始”。
- 单件流程结束收到 `CYCLE_DONE` 后，如果用户没有停止，就自动发起下一轮 `START_CYCLE`，继续找下一件零件。
- 参数页以 `DetectSettingsController` 为唯一真值源，保存后、加载后、重启后都显示同一份真实配置。

**方案**

## 1. 自动流程状态拆分

- 新增“自动循环会话是否启用”和“当前 cycle 是否正在运行”两个本地概念。
- `开始`：开启自动循环会话，并启动第一轮 `START_CYCLE`。
- `暂停` / `继续`：只作用于当前正在运行的 cycle。
- `停止`：
  - 如果当前 cycle 正在运行或暂停中，向 F4 发送 `STOP_CYCLE`，并在 ACK 后终止整个会话。
  - 如果当前 cycle 已经结束、但自动循环会话仍准备进入下一轮，则本地直接结束会话，不再发旧 cycle 的 `STOP_CYCLE`。

## 2. CYCLE_DONE 后的下一轮行为

- `onF4FinalSortFinished(ok=true)` 代表单件流程闭环完成。
- 完成后立刻把“当前 cycle 运行中”状态清掉。
- 如果自动循环会话仍启用，且用户没有停止，则自动重新发送一次 `START_CYCLE`，创建新的 `cycle_id`，进入下一件流程。
- 如果最终分拣失败或机械臂流程失败，则停止自动循环会话，避免故障状态下无限重试。

## 3. C++ F4 本地状态修正

- `handleF4FinalSortFinished()` 收到 `CYCLE_DONE` 后，要把 `m_f4AutoRunning` 和 `m_f4AutoPaused` 清零。
- 这样后续新的 `START_CYCLE` 才能合法创建新 `cycle_id`，也不会再拿旧 cycle 响应“停止”。

## 4. 参数页唯一真值源

- QML 参数显示继续直接绑定 `detectSettings` 暴露的属性，不在 QML 里长期维护第二份默认值副本。
- `detectSettings.settingsChanged` 时，统一刷新参数页摘要、步进弹窗版本号和最近状态文本。
- `saveSettingsToDisk()` 成功后，QML 立即调用一次 `loadSettingsFromDisk()` 重新从 JSON 回读，确保“保存成功后的界面值”和“重启后的界面值”完全同源。

## 5. 文档与验证

- 更新 `20_uvc_camera/qt_camera_display/README.md`，把“停止整个自动循环”和“参数保存后重启回显”写进模块说明与验证表。
- 静态验证继续用 `test_qt_kms_overlay_assets.sh`，并补自动循环状态机 marker。
- 板端验证重点覆盖：
  - 单件完成后自动进入下一轮。
  - 下一轮开始前点 `停止` 不再报旧 cycle 异常。
  - 修改参数、点击保存、重启 Qt/开发板后，参数页仍显示上次值。
