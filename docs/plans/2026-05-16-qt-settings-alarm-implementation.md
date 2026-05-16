# Qt 参数设置与告警维护界面实施记录

## 实施范围

| 项目 | 内容 |
|---|---|
| 目标模块 | `20_uvc_camera/qt_camera_display/` |
| 修改页面 | `参数设置`、`告警维护` |
| 技术范围 | Qt Quick/QML 本地状态、触摸交互、页面导航、静态契约测试、模块文档 |
| 非目标 | 不新增真实串口通信，不写真实 JSON 配置文件，不生成真实诊断文件，不修改 C++ 控制器 |

## 修改文件

| 路径 | 修改原因 |
|---|---|
| `20_uvc_camera/qt_camera_display/qml/Main.qml` | 新增 `settingsPageVisible`、`alarmPageVisible`，允许左侧导航进入参数和告警页；新增参数状态、告警状态、参数处理函数和告警处理函数；新增两个全屏功能页。 |
| `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh` | 增加参数设置页和告警维护页的静态契约检查，防止后续改动丢失页面入口、关键控件或告警码。 |
| `20_uvc_camera/qt_camera_display/README.md` | 同步模块说明、修改文件清单、使用流程和修改记录，明确第一版只做 QML 状态，不代表真实硬件闭环。 |
| `docs/plans/2026-05-16-qt-settings-alarm-design.md` | 记录页面目标、结构、状态契约和验收方式。 |
| `docs/plans/2026-05-16-qt-settings-alarm-implementation.md` | 记录本次实施范围、文件变更和验证边界。 |

## QML 实施点

| 区域 | 实施内容 |
|---|---|
| 导航 | `switchPage()` 支持 `settings` 和 `alarm`；左侧导航不再只允许首页/历史/统计/手动。 |
| Overlay | 首页视频区、首页结果区和底部统计区增加 `settingsPageVisible/alarmPageVisible` 隐藏条件；KMS overlay 仍只在首页显示。 |
| 参数状态 | 增加零件类型、良坏阈值、复核阈值、精定位阈值、稳定帧数、脉冲标定、低速档、分拣超时、自动上传状态。 |
| 参数交互 | `changeSettingValue()` 负责参数加减和范围限制；`settingsApplyAction()` 负责应用、保存、恢复默认、导出摘要、切换上传、切换零件。 |
| 告警状态 | 增加当前告警码 `0x0007`、告警名称、等级、发生时间、确认状态、清除状态。 |
| 告警交互 | `alarmHistoryModel` 保存维护历史；`handleAlarmAction()` 处理确认、清故障、刷新状态、保存诊断。 |

## 验证记录

| 验证项 | 当前结果 | 说明 |
|---|---|---|
| TDD 红灯 | 已完成等价确认 | 修改测试脚本后，先用 `Select-String` 确认新增标记在实现前不存在；随后实现页面让契约变绿。 |
| 静态标记检查 | 已通过 | `settingsPageVisible`、`alarmPageVisible`、页面 `id`、关键按钮和告警码均已检出。 |
| 脚本执行 | 已通过 | 使用 Git Bash 执行 `./test_qt_kms_overlay_assets.sh`，输出 `PASS: Qt KMS overlay assets contract`。 |
| 板端交互 | 未执行 | 本次只在本地工作区写 QML 和文档，尚未交叉编译部署到 STM32MP157。 |

## 后续验证命令

| 测试目标 | 执行位置 | 命令 | 预期输出/现象 | 失败时排查 |
|---|---|---|---|---|
| 静态契约 | Linux 虚拟机项目目录 | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ./test_qt_kms_overlay_assets.sh` | 输出 `PASS: Qt KMS overlay assets contract` | 检查新增标记、脚本路径和 `defect-cos-upload --self-test-json-parser`。 |
| 本地标记 | Windows 仓库 | `Select-String -Path '20_uvc_camera\qt_camera_display\qml\Main.qml' -Pattern 'settingsPageVisible|alarmPageVisible|settingsApplyAction|handleAlarmAction'` | 输出对应行号 | 若无输出，说明 QML 没有同步。 |
| 交叉编译 | Linux 虚拟机 | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ./build_qt_camera_display.sh` | 生成 `build-mp157/qt_camera_display` | 先确认已加载 ST Qt SDK，QML 资源被重新 rcc。 |
| 板端参数页 | 开发板屏幕 | 点击 `参数设置`，再点 `+/-`、`恢复默认` | 页面显示参数区，摘要和底部提示变化，视频不遮挡 | 查 `switchPage("settings")`、overlay `VISIBLE`。 |
| 板端告警页 | 开发板屏幕 | 点击 `告警维护`，再点 `确认`、`清故障`、`保存诊断` | 告警状态和历史记录更新，视频不遮挡 | 查 `handleAlarmAction()`、`alarmHistoryModel`。 |
