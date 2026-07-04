# STM32MP157 Qt GPU 摄像头界面

这个目录提供一个最小可落地的 Qt Quick 工业检测主界面：左侧为功能导航，中间显示 UVC 摄像头实时画面，右侧显示检测结果，底部显示统计和控制按钮。

| 项目 | 当前实现 |
|---|---|
| UI 框架 | Qt Quick / QML |
| 摄像头输入 | 自定义 `V4L2VideoItem`，默认 `/dev/video0` |
| 视频显示 | 安全预览使用 V4L2 YUYV 帧上传到 OpenGL ES 纹理；正式路线使用 KMS overlay plane 显示视频 |
| GPU 路径 | `galcore` + OpenGL ES + `eglfs` 或 `wayland-egl` |
| 目标分辨率 | 1024x600 |
| 当前检测逻辑 | 点击 `检测` 后先运行 MobileNetV3-Small 分类，再运行 UNet 分割；分类完成后首页立即显示零件类型、短类别、分类初判和置信度，但主状态保持“等待综合判定”；UNet 完成后按两个模型综合生成最终判定并显示双模型总耗时；首页窄栏只显示“分类良/分类坏/综合良品/综合坏品”等短结果，完整模型依据保留在历史详情；只要分类模型判坏或 UNet 检出缺陷像素，最终就不能判为良品；COS 上传完成后再把本次原始图片、UNet raw/overlay/mask 结果图、两个模型输出、综合判定和云端上传状态合并成一条历史记录。 |
| SD 卡按钮 | 右侧面板只保留 `检测` 和 `安全卸载`；独立 `保存图片` 按钮已取消，图片保存由双模型检测流程自动完成。 |
| 板端 SSH | 从虚拟机执行 `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250` |

> 记录：当前版本已经打通 Qt 界面和 UVC 摄像头显示，但它是“安全预览路径”，不是最终零拷贝视频路径。
> 默认采集参数为 `320x240@10fps`，板端 5 秒平均 CPU 实测约 `5.9%`，画质明显不足。
> `640x480@15fps` 安全路径实测约 `42.0%`，接近旧 CPU framebuffer 预览，所以后续必须继续做稳定的零拷贝/硬件视频显示链路。

## 模块文档总览

| 项目 | 内容 |
|---|---|
| 模块目的 | 提供 STM32MP157 工业缺陷检测 Qt Quick 主界面，并把 UVC 摄像头画面以安全 V4L2、GStreamer 桥接或 KMS overlay 的方式显示到 1024x600 RGB 屏。 |
| 模块目录 | `20_uvc_camera/qt_camera_display/` |
| 默认 UI 技术 | Qt Quick/QML + eglfs + Vivante galcore OpenGL ES。 |
| 当前正式视频路线 | Qt UI 运行在 primary surface，`uvc_kms_overlay` 运行在 KMS overlay plane 36，默认 `640x480@10fps`。 |
| 自动视觉居中路线 | 首页按 `开始` 且 F4 `START_CYCLE` ACK 后，QML 启动 `autoVisionTimer`，每 100ms 通过 overlay socket 发送 `LOCATE`；`uvc_kms_overlay` 读取原始 YUYV 中水平居中的 300px 宽全高搜索带，使用亮度连通域提前发现从画面上方进入的零件，并返回零件中心和 bbox；MP157 使用上方来料规则，把 `center_y` 作为 `VISION_POS.axis_px`、`height/2` 作为 `target_px` 下发 F4；连续 3 帧满足 `abs(center_y-height/2)<=24px` 后发送 `BELT_STOP_CENTERED`；收到 ACK 后不再只等待 2 秒，而是下发 `ACTUATOR_POS_MOVE actuator=ACT_CAMERA_Z direction=DOWN` 让上下轴下降固定步数，收到下降 ACK 后先等待约 3 秒让摄像头对焦稳定，再复查 ROI，必要时用 `ACT_CAMERA_FORWARD` 前后轴短步微调，确认仍在 ROI 中央后复用现有 `检测` 链路运行双模型；模型检测完成后自动下发 `ACTUATOR_POS_MOVE actuator=ACT_CAMERA_Z direction=UP` 回升固定步数；如果黑色波形零件已经识别过，后续短暂漏检只保持停机等待重新识别，不再发送会让 F4 重新扫描的 `VISION_LOST reason=1`。 |
| 检测历史路线 | 每次点击 `检测` 后，Qt 控制器后台依次执行当前帧 JPG 保存、MobileNetV3-Small 分类、UNet 分割、COS 上传；分类完成信号先刷新首页零件类型和类别，双模型完成信号再刷新 `fused_status/fused_result/fused_reason` 与 `total_time_ms`，最终上传完成后追加 `source_path`、`annotated_images`、`classification_result`、`segmentation_result`、云端记录号和上传状态到当天 `/mnt/sdcard/images/upload_history_YYYYMMDD.json`；启动时兼容读取旧 `/mnt/sdcard/images/upload_history.json` 和多天 `upload_history_*.json`，历史页/统计页仍汇总显示多天记录；上传成功判定统一以 `upload_status=OK` 或 `上传成功` 为准，上传完成后的详情回查异常只写 `verify_status=warning` 诊断，不再把云端已存在的记录显示成上传失败；云端 `records.result` 由分类和 UNet 综合结果映射为 `good/bad/review`，不再只看第一个模型；云端 `records.part_id` 由分类 `class=` 去掉 `_good/_bad` 后得到的零件类型映射，良品和坏品不能拆成两个零件类型。 |
| 统计分析路线 | 左侧 `统计分析` 页面直接读取本地 `uploadHistory` 模型，汇总总记录、良品/待复核、上传成功率、图片数量、文件大小、最近检测趋势和云端状态；`分布概览` 用两列显示，左列是良品/坏品/待复核，右列是上传成功/上传失败，避免五条统计在 160px 面板中纵向越界；最近记录卡片内支持竖向滑动查看更多记录；当前统计基于双模型检测历史。 |
| 手动控制路线 | 左侧 `手动控制` 页面提供传送带、检测辅助、安全状态、人工复核和命令日志；手动电机控制入口打开 `manualMotorPopup` 三页弹窗，第一页控制传送带，第二页控制摄像头前后轴，第三页控制摄像头上下轴；传送带和前后轴方向按钮通过 `/dev/ttySTM2` 向 F407 下发 `ACTUATOR_VEL_MOVE`，点击一次持续运动，停止按钮下发 `ACTUATOR_STOP` 后才结束；如果普通运动 ACK 尚未返回，停止按钮会排队等待该 ACK/NACK 后立即补发，避免停止点击被忙状态吞掉；上下轴方向按钮仍下发 `ACTUATOR_POS_MOVE`，下降取 `zDownFixedSteps`，上升和 `回原位` 取 `zUpFixedSteps`，每点一次只走对应固定步数；模拟急停下发 `ACTUATOR_STOP actuator=0xFF`；传送带仍保留 `BELT_MANUAL_CONTROL/QUERY_STATUS` 巡航、停止和状态查询语义；安全状态区域使用 `manualSafetyFlickable` 可滑动查看，避免底部按钮遮挡长文本。 |
| 参数设置路线 | 左侧 `参数设置` 页面已收敛为波形垫圈、平垫圈、弹性垫圈三种真实零件；`DetectSettingsController` 启动时读取 `/mnt/sdcard/config/defect_ui_config.json`，页面可真实调整模型阈值、复核阈值、ROI 大小、UNet 最小缺陷像素、overlay 透明度、COS 自动上传开关和三台步进电机参数；当前现场默认电机地址为传送带 `0x01`、摄像头前后 `0x03`、摄像头上下 `0x02`；保存配置用 `QSaveFile` 原子写 JSON，下一次检测会把模型阈值传给 `defect-classify --bad-threshold`，把 ROI、overlay 透明度和最小缺陷像素传给 `defect-segment --roi/--alpha/--min-defect-pixels`，步进电机参数保存到 `stepper_motors` 数组；摄像头上下电机额外保存 `z_down_fixed_steps` 和 `z_up_fixed_steps`，QML 输入范围按 32 位位置步数处理，允许 `0~4294967295 step`；步进弹窗的 `保存并下发` 会在保存 JSON 后通过 `/dev/ttySTM2` 发送二进制 `STEPPER_PARAM_SET 0x42`，界面会显示本次下发 ID，例如 `传送带=1，前后=3，上下=2`，负载为 25 字节，包含 `cycle_id=0`、`motor_count=3`、`flags=0` 和三条 7 字节电机记录；步进弹窗的 `设当前位置为零点` 会发送 `ACTUATOR_HOME 0x53`，让 F4 停止当前页电机并把当前位置设为新的零点；`保存配置` 和 `导出摘要` 都会追加 `/mnt/sdcard/logs/qt_settings_YYYYMMDD.log`，日志查看页刷新后可点开看到 `config_path`、阈值、ROI、UNet 像素阈值、overlay、上传策略、三台电机地址/步长/速度/方向、Z 轴下探/回升固定步数和模型命令参数；关闭自动上传时仍保存 source/annotated 和本地历史，最终返回 `upload_status=SKIP`；F4 返回 `ACK acked_cmd=0x42 status=0` 只表示 F4 协议层接收参数，现场仍要用 `CAMINFO` 或 F4 串口 `Runtime config applied` 日志确认运行时地址已应用，不代表 F4 已重新烧录，也不会写 Emm42 EEPROM；F4 返回 `ACK acked_cmd=0x53 status=0` 只表示清零命令帧已被 F4 接收并投递，不等于已做限位回零；称重标定 UI 发送二进制 `WEIGHT_CALIBRATE 0x30`，负载为 5 字节，包含 `cycle_id=0`、`known_weight_g=1~5000` 和 `flags=0`，F4 成功返回 `ACK`，未去皮、克重越界或采样异常返回结构化 `NACK`。 |
| 告警维护路线 | 左侧 `告警维护` 页面提供当前告警、设备健康、告警历史、处理建议、确认/清故障/刷新/保存诊断入口；处理建议面板只显示短摘要，点击 `查看全部` 后进入 `alarmAdviceDetailOverlay`，由 `alarmAdviceDetailFlickable` 滚动查看相机、SD 卡、4G、云端、F4、保存/上传和模型链路完整排查说明；设备健康矩阵中的 `4G` 格直接绑定 `DeviceHealthController::networkStatusText`，不再显示参数配置占位；QML 监听 `DeviceHealthController` 和检测/上传返回值，运行中识别相机/KMS 无帧、SD 卡未挂载或不可写、4G/云端异常、F4 心跳异常、保存/上传失败、模型检测失败等真实问题；每个新问题调用 C++ 追加到当天 `/mnt/sdcard/logs/qt_alarm_YYYYMMDD.log`，每次 `保存诊断` 追加到当天 `/mnt/sdcard/logs/qt_alarm_snapshot_YYYYMMDD.txt`，两类文件都执行 `flush` 和 `fsync`，跨天自动新开日期文件。 |
| 日志查看路线 | 左侧 `日志查看` 页面通过 C++ `LogFileModel` 只读扫描 `/mnt/sdcard/logs` 下的 `.log` 和 `.txt` 文件，列表显示日志名称、类型、大小、修改时间和完整路径；点击日志行会打开 `logDetailOverlay`，由 `logDetailFlickable` 滚动查看完整文件内容；页面可直接查看告警日志、诊断快照和参数日志，其中参数日志文件名为 `qt_settings_YYYYMMDD.log`；页面提供刷新和返回首页按钮，不提供删除、编辑或截断操作，避免误破坏现场诊断证据。 |
| 真实健康状态 | `DeviceHealthController` 通过 `QTimer` 每 8 秒周期调度，4G/云端用异步 `QProcess`，相机 overlay `STATUS` 和 F4 二进制 `HEARTBEAT` 握手用后台线程；4G/云端进程启动失败由 `errorOccurred` 信号回写状态，不在刷新路径调用 `waitForStarted()`；F4 `HEARTBEAT` 后台心跳按 120 秒节流发送，人工刷新和打开标定弹窗时可立即触发一次；周期刷新保留上一轮稳定状态，不再每轮把网络/云端显示成“检测中”；QML 只绑定状态属性，不执行 socket、串口或 shell 阻塞调用。 |
| 开机启动动画 | `fb_boot_splash` 先在 Qt/GPU 启动前直接写 `/dev/fb0` 显示 AI 竞赛品牌静态首帧；静态首帧由 `ai_boot_splash_preview.html` 离线渲染成 `boot_splash.rgb565`，因此中文标题、光晕、透明层和字体效果与 HTML 预览保持一致；QML 顶层 `splashOverlay` 随后显示工业检测自检动画，包含相机扫描窗口、ROI 框、阶段状态和进度条；`uvc_kms_overlay` 可以提前后台初始化和采集，但启动时必须先隐藏视频层，启动脚本只等待 Qt 进程稳定，不依赖 QML `console.log` 调试文本；只有 Qt splash 完全淡出并置位 `bootOverlayRestoreFinished` 后才允许恢复摄像头画面，避免摄像头先于 Qt 界面出现。 |
| AI 品牌启动图预览 | `ai_boot_splash_preview.html` 是电脑浏览器里的 1024x600 静态视觉源，用纯 HTML/CSS 重绘 `第九届嵌入式芯片与系统设计竞赛`、`9th AI赋能设计，设计点亮AI! / AI for Design & Design for AI!` 标识和芯片、电路、技术铭牌元素；`generate_boot_splash_asset.py` 使用 Edge/Chromium 无头截图生成 `boot_splash.png` 和 `boot_splash.rgb565`，板端 `fb_boot_splash` 只负责把 RGB565 资源 blit 到 framebuffer。 |
| 保底视频路线 | 自定义 `V4L2VideoItem` 安全预览和 `VIDEO_BACKEND=qt-gst` mmap 桥接可演示，但不是最终零拷贝目标。 |
| 禁用结论 | QtMultimedia `Camera + VideoOutput` 和 `qmlglsink + UVC DMABUF` 不能作为稳定路线，前者触发内核 Oops，后者在 Vivante `libGAL.so` 中崩溃。 |

## 2026-07-03 本次经验总结

| 主题 | 做了什么 | 怎么测试 |
|---|---|---|
| 板端自动视觉定时器作用域修复 | 板端部署后日志出现 `qrc:/qml/Main.qml:4435: TypeError`，根因是 `Timer { id: autoVisionTimer }` 和 `Timer { id: autoVisionDetectDelayTimer }` 是 QML id，不是 `root` 属性；`BELT_STOP_CENTERED` ACK 分支里写成 `root.autoVisionTimer.stop()` 会找不到对象，可能打断居中后的检测延时。已把 ACK 分支改为直接调用 `autoVisionTimer.stop()/restart()` 和 `autoVisionDetectDelayTimer.restart()`，并在 `test_qt_kms_overlay_assets.sh` 中增加禁止 `root.autoVisionTimer/root.autoVisionDetectDelayTimer` 的静态回归检查。修改文件：`20_uvc_camera/qt_camera_display/qml/Main.qml`、`20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`、`20_uvc_camera/qt_camera_display/README.md`。 | 虚拟机执行 `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && sh ./test_qt_kms_overlay_assets.sh && ./build_qt_camera_display.sh`；板端替换 `/root/qt_camera_display/qt_camera_display` 后执行 `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart`，再用 `tail -n 80 /tmp/qt-kms-overlay-shell.log | grep 'TypeError'` 确认新启动后不再出现该错误；现场点击首页 `开始`，零件居中后应进入当前版本的 Z 轴下降、3 秒对焦等待、ROI 复查、模型检测和 Z 轴回升流程。 |
| 上方来料自动视觉居中 | `uvc_kms_overlay.c` 新增 `LOCATE` 控制命令和 `locate_part_in_yuyv_frame()`，直接从最新 YUYV 原始帧中间 300px 宽全高搜索带统计亮度阈值并筛选连通域，提前发现上方进入的零件，返回 `has_target/frame_id/width/height/center_x/center_y/bbox/confidence`；黑色波形零件现场漏检后，基础亮度差阈值已从 `18` 降到 `12`；`main.cpp` 新增 `requestAutoVisionLocate()`、`sendF4VisionPosition()`、`sendF4VisionLost()`、`sendF4BeltStopCentered()`，按协议编码 `VISION_POS 0x20`、`VISION_LOST 0x21`、`BELT_STOP_CENTERED 0x22`；`Main.qml` 新增 `autoVisionTimer`、`handleAutoVisionLocateFinished()`、`autoVisionHasSeenTarget` 和 `autoVisionLostFrames`，开始 ACK 后每 100ms 定位，上方来料使用 `center_y` 对齐 `height/2`，连续 3 帧进入 ±24px 后停传送带；目标出现过后短暂漏检不再发 `VISION_LOST reason=1`，而是保持停机等待重新识别，避免 F4 重新扫描把零件送走。修改文件：`20_uvc_camera/qt_camera_display/uvc_kms_overlay.c`、`20_uvc_camera/qt_camera_display/main.cpp`、`20_uvc_camera/qt_camera_display/qml/Main.qml`、`20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`、`20_uvc_camera/qt_camera_display/README.md`。 | 本地执行 `"C:/Program Files/Git/bin/bash.exe" -lc 'cd /c/Users/caofengrui/Desktop/linux/20_uvc_camera/qt_camera_display && ./test_qt_kms_overlay_assets.sh'`；板端部署后执行 `printf 'LOCATE\n' \| nc -U /tmp/uvc-kms-overlay-control.sock`，预期返回 `OK LOCATE has_target=0/1 ... center_y=... confidence=...`；再按首页 `开始`，F4 ACK 后底部应显示自动视觉坐标，零件进入中心后显示 `BELT_STOP_CENTERED` ACK，并进入当前 README 上方记录的 Z 轴下降、ROI 复查、模型检测、Z 轴回升流程。代码已写不等于板端或 F4 已生效，必须同步到虚拟机、交叉编译、替换板端 `qt_camera_display` 和 `uvc_kms_overlay`，并确认 F4 已烧录支持 `VISION_POS/BELT_STOP_CENTERED/ACTUATOR_POS_MOVE` 的固件。 |

## 2026-07-01 本次经验总结

| 主题 | 做了什么 | 怎么测试 |
|---|---|---|
| 首页自动流程按钮接入 F4 二进制协议 | `main.cpp` 在 `DeviceHealthController` 中新增 MP157->F407 二进制短帧组帧、`CRC16-CCITT-FALSE` 校验、ACK/NACK 解帧和 `sendF4AutoControlCommand()`；`qml/Main.qml` 把首页 `开始/暂停/继续/停止` 改为先下发 `START_CYCLE/PAUSE_CYCLE/RESUME_CYCLE/STOP_CYCLE`，收到 F4 ACK 后才确认界面状态；停止后不能继续，只能重新开始生成新 `cycle_id`；暂停后继续复用同一个 `cycle_id`；`ACK status=1` 表示重复帧，Qt 不再把它当作新的启动成功，避免“绿色成功但传送带不动”的误判。修改文件：`20_uvc_camera/qt_camera_display/main.cpp`、`20_uvc_camera/qt_camera_display/qml/Main.qml`、`20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`、`20_uvc_camera/qt_camera_display/README.md`。 | 本地执行 `"C:/Program Files/Git/bin/bash.exe" -lc 'cd /c/Users/caofengrui/Desktop/linux/20_uvc_camera/qt_camera_display && ./test_qt_kms_overlay_assets.sh'`，确认静态契约包含 `BINARY_PROTOCOL_CMD_START_CYCLE`、`readF4BinaryReply`、`sendF4AutoControlCommand`、`ACK重复帧` 和 `onF4AutoControlFinished`；虚拟机交叉编译后执行 `strings build-mp157/qt_camera_display \| grep -E 'START_CYCLE\|sendF4AutoControlCommand\|ACK重复帧\|自动流程ACK'`；板端运行 Qt 后点击首页四按钮，预期底部提示显示 `自动流程ACK：cycle=... status=0 state=SCANNING`，若显示 `自动流程失败：ACK重复帧`、`NACK ERR_STATE_NOT_ALLOWED/ERR_BUSY`，先查 F4 当前状态机和串口日志。 |
| 日志查看页面 | 新增左侧 `日志查看` 导航项；`main.cpp` 新增只读 `LogFileModel`，扫描 `/mnt/sdcard/logs` 中的 `.log` 和 `.txt`；`Main.qml` 新增日志列表、刷新按钮、空状态、`logDetailOverlay` 和 `logDetailFlickable`，点击文件名后可滚动查看完整日志内容。 | 本地执行 `"C:/Program Files/Git/bin/bash.exe" -lc 'cd /c/Users/caofengrui/Desktop/linux/20_uvc_camera/qt_camera_display && ./test_qt_kms_overlay_assets.sh'`；虚拟机交叉编译后执行 `strings build-mp157/qt_camera_display \| grep -E '日志查看\|logDetailFlickable\|LogFileModel'`；板端打开 `日志查看`，点击任意日志并向下滑动到末尾。 |
| 日志数据边界 | 日志页只读展示 `/mnt/sdcard/logs/qt_alarm_YYYYMMDD.log` 和 `/mnt/sdcard/logs/qt_alarm_snapshot_YYYYMMDD.txt` 等文件，不提供删除、编辑或导出，避免操作员误删现场证据。 | 板端执行 `date +%Y%m%d; ls -lh /mnt/sdcard/logs; tail -n 20 /mnt/sdcard/logs/qt_alarm_$(date +%Y%m%d).log 2>/dev/null || true`，再在页面中确认同名文件可见且弹窗内容与 `tail/cat` 读取结果一致。 |
| 页面遮挡处理 | `logPageVisible` 接入首页视频、结果面板和底部统计面板的显隐条件；进入日志页时 `switchPage("logs")` 会刷新日志列表并在 KMS overlay 模式下隐藏视频层，避免摄像头 plane 盖住日志列表。 | 板端运行 `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart` 后点击 `日志查看`，确认实时画面隐藏、日志列表完整显示，点击 `返回首页` 后摄像头画面恢复。 |
| 上传状态误判修复 | `defect-cos-upload` 在 source/annotated 都已 PUT 到 COS 并登记文件后，如果最终详情回查失败，只输出 `verify_status=warning` 并保持 `上传成功：upload_status=OK record_id=... record_no=...`；`main.cpp` 和 `Main.qml` 统一用 `upload_status=OK/FAIL/SKIP` 判断历史、统计、重发和告警状态。 | 本地执行 `"C:/Program Files/Git/bin/bash.exe" ./test_qt_kms_overlay_assets.sh`、`./defect-cos-upload --self-test-json-parser` 和 `./defect-cos-upload --self-test-args`；板端点击 `检测` 后查看历史详情，云端能看到记录时历史页应显示上传成功而不是上传失败。 |
| 全局底部提示浮层 | `storageToast` 从页面中部移动到 `globalStorageToastLayer`，`z: 900`，放在历史、统计、手动、参数、告警、日志等普通页面之后渲染，并保留在启动遮罩下方；`globalStorageToastLayer` 外层保持 `visible: true` 常驻，只由内部 `storageToast.opacity/visible` 控制淡入淡出，避免父层反向绑定子提示条可见性后导致绿色提示完全不显示。 | 本地执行 `"C:/Program Files/Git/bin/bash.exe" ./test_qt_kms_overlay_assets.sh`；板端分别在 `历史记录`、`统计分析`、`参数设置`、`告警维护` 和 `日志查看` 页面触发检测、保存配置或刷新动作，确认底部绿色/红色提示完整可见。 |

## 2026-05-21 本次经验总结

| 主题 | 做了什么 | 怎么测试 |
|---|---|---|
| 云服务器迁移 | 板端默认云服务器切到 `http://139.9.35.72`：`DEFAULT_CLOUD_HEALTH_URL` 用新 `/health`，`defect-cos-upload` 默认 `CLOUD_BASE_URL` 用新后端，`board-review-tunnel.sh` 默认 `CLOUD_HOST` 用新公网 IP；账号、Cookie、`cos-upload.env`、API 路径和云端本机 `127.0.0.1:18081` 回写端口不变。 | 本地执行 `"C:/Program Files/Git/bin/bash.exe" ./test_qt_kms_overlay_assets.sh`；板端部署后执行 `curl -fsS --max-time 2 http://139.9.35.72/health`、`/root/qt_camera_display/defect-cos-upload --self-test-json-parser` 和 `/etc/init.d/S91board-review-tunnel status`。 |
| 告警处理建议完整显示 | 告警维护页右下角处理建议仍保留短列表，新增 `查看全部` 按钮；点击后打开 `alarmAdviceDetailOverlay`，完整文本由 `alarmFullAdviceText()` 复用 `alarmSourceAdvice()` 生成，并交给 `alarmAdviceDetailFlickable` 滚动显示。 | 本地执行 `"C:/Program Files/Git/bin/bash.exe" ./test_qt_kms_overlay_assets.sh`；板端点击 `告警维护` -> `查看全部`，确认能滚动读到相机、SD 卡、4G、云端、F4、保存/上传和模型检测排查项。 |
| 告警设备健康 4G 状态 | 设备健康矩阵最后一格从参数 `配置` 改为 `4G`，直接显示 `deviceHealth.networkStatusText` 和 `deviceHealth.networkStatusColor`，与顶部网络状态同源，只有 `4g-ppp test` 通过才显示在线。 | 板端执行 `command -v 4g-ppp; 4g-ppp test; echo $?`，再点击告警页 `刷新状态`，确认 `4G` 格与命令退出码一致。 |
| 参数详情完整显示 | 参数设置页的 `视觉检测策略` 和 `F4接入边界` 卡片新增 `查看详情`；视觉详情按云端上报契约说明 `record_no`、零件归一、双模型判定、`source/annotated` 图片、COS 登记和断网补传；F4 详情说明 `/dev/ttySTM1`、`115200`、心跳/CRC/帧序号、LDC1614、HX711、Emm42_V5.0、急停限位和 Qt 不接管运动控制的边界。 | 本地执行 `"C:/Program Files/Git/bin/bash.exe" ./test_qt_kms_overlay_assets.sh`；板端点击 `参数设置` -> `视觉检测策略/查看详情` 和 `F4接入边界/查看详情`，确认弹层可滚动读完；确认存储卡片标题为 `相机、存储与上传`。 |
| 参数详情按钮防重叠 | 板端拍屏发现 `查看详情` 按钮贴在卡片右下角会遮住最后一行状态；已把两个卡片摘要列表压为 4 行，并把底部状态文字放左侧、`查看详情` 按钮固定在右侧预留区域，避免按钮覆盖文本。 | 本地执行 `"C:/Program Files/Git/bin/bash.exe" ./test_qt_kms_overlay_assets.sh`；板端打开 `参数设置`，确认 `视觉检测策略` 和 `F4接入边界` 两张卡片底部状态文本与 `查看详情` 按钮左右分栏，没有重叠。 |
| 参数详情滚动归零 | 两个参数详情共用 `settingsDetailFlickable`，板端发现从一个详情滑到中间后再打开另一个详情会继承滚动位置；已在 `openSettingsDetail()` 每次打开详情后通过 `Qt.callLater()` 把 `settingsDetailFlickable.contentY` 归零。 | 本地执行 `"C:/Program Files/Git/bin/bash.exe" ./test_qt_kms_overlay_assets.sh`；板端打开 `视觉检测策略/查看详情` 滑到中间并关闭，再打开 `F4接入边界/查看详情`，确认正文从顶部开始显示。 |
| F4 二进制心跳、状态和故障回包 | F4 健康心跳从每 8 秒尝试发送改为 120 秒节流，减少 RS485 占用；参数设置页新增 `称重标定` 弹窗，快捷克重 100/500/1000/2000g，也支持通过内置数字键盘输入任意 1~5000g 整数；Qt 不再写 `CAL` 文本，当前发送二进制 `WEIGHT_CALIBRATE 0x30` 并按 `ACK/NACK/FAULT_REPORT` 显示结果。 | 本地执行 `"C:/Program Files/Git/bin/bash.exe" ./test_qt_kms_overlay_assets.sh`；板端点击 `参数设置` -> `称重标定`，用 `清空`、数字键和 `退格` 输入非快捷值如 `750` 后点击 `发二进制`；F4 已去皮且最近一次 HX711 采样有效时，预期弹窗显示 `ACK WEIGHT_CALIBRATE ...`；若未去皮、克重越界或 HX711 异常，预期显示对应 `NACK ERR_STATE_NOT_ALLOWED/ERR_FIELD_RANGE/ERR_HARDWARE_FAULT`。 |
| 步进电机参数弹窗 | `F4接入边界` 卡片底部新增小按钮 `步进参数`，点击后打开三页弹窗，分别设置传送带电机、摄像头前后电机、摄像头上下电机的 ID 地址、最小步长、常规速度和方向；常规速度可点 `输入` 打开内置数字键盘，写入 `0~5000 rpm` 任意整数；配置写入 `defect_ui_config.json` 的 `stepper_motors` 数组，并写入 `qt_settings_YYYYMMDD.log`；点击弹窗底部 `保存并下发` 后调用 `sendF4StepperSettings()` 发送 `STEPPER_PARAM_SET 0x42`，QML 通过 `f4StepperSettingsFinished` 显示 ACK/NACK 结果。代码已写不等于板端或 F4 已生效；F4 必须重新编译烧录后才会识别 `0x42`。 | 本地执行 `"C:/Program Files/Git/bin/bash.exe" ./test_qt_kms_overlay_assets.sh`；板端部署新二进制后点击 `参数设置` -> `步进参数`，三页分别修改地址/步长/速度/方向，其中速度分别试 `0`、`137`、`5000`，再点 `保存并下发`；SSH 执行 `grep -n "stepper_motors" /mnt/sdcard/config/defect_ui_config.json` 和 `tail -n 60 /mnt/sdcard/logs/qt_settings_$(date +%Y%m%d).log` 确认 JSON 与日志落盘；底部提示应显示 `F4已接收步进参数：ACK acked_cmd=0x42...`，若显示 `NACK` 先按错误码查字段范围、队列是否创建和 F4 是否已烧录新固件。 |

## 2026-05-20 本次经验总结

| 主题 | 做了什么 | 怎么测试 |
|---|---|---|
| 云端长文本完整显示 | 历史详情右侧固定面板改为短摘要，完整云端修正、UNet 提示、图片留档和模型原始输出放进 `historyAnalysisDetailOverlay`，由 `analysisDetailFlickable` 滚动承载。 | 虚拟机执行 `sh ./test_qt_kms_overlay_assets.sh`；板端历史详情点击 `查看完整说明`，确认长复核原因能滚到最后一行。 |
| 首页/历史短文案预算 | 首页窄结果栏使用 `compactHomeClassText()` 和 `compactHomeModelText()`，避免完整类别名和综合原因在 182px 面板中显示不全。 | 板端点击 `检测`，确认分类完成后能看到短零件类型和短类别；历史详情固定面板不出现文字越界。 |
| 统计分布防溢出 | 统计页 `分布概览` 从单列五条改为左右两列，左列显示检测结果分布，右列显示上传链路分布，防止“上传失败”条超出面板边框。 | 虚拟机执行 `"C:/Program Files/Git/bin/bash.exe" ./test_qt_kms_overlay_assets.sh`；板端点击 `统计分析`，确认五项分布都完整显示在面板内。 |
| 波形垫圈命名修正 | 旧训练标签 `gasket/gasket_good/gasket_bad` 在板端显示和上传时都按“波形垫圈 / 垫圈类”处理；`washer` 是“平垫圈”，不是同一个零件。 | 运行 `sh defect-cos-upload --self-test-json-parser`；云端零件页确认 `gasket` 不显示成“垫片”，`washer` 不被合并到 `gasket`。 |
| 参数设置页真实配置 | 参数页 `零件与模型判定` 只在波形垫圈、平垫圈、弹性垫圈之间循环；模型阈值、复核阈值、ROI、UNet 像素阈值、overlay 透明度和自动上传开关通过 `detectSettings` 绑定到 C++，保存到 `/mnt/sdcard/config/defect_ui_config.json`；`保存配置` 和 `导出摘要` 追加当天 `qt_settings_YYYYMMDD.log`。 | 虚拟机或本地执行 `sh ./test_qt_kms_overlay_assets.sh`；板端打开 `参数设置`，确认界面显示 `真实检测配置` 和 JSON 路径，点击 `保存配置` 后 SSH 查看 JSON，下一次检测命令包含 `--bad-threshold` 和 `--min-defect-pixels`；进入 `日志查看` 刷新并点开 `qt_settings_YYYYMMDD.log`，确认能看到 `config_path`、阈值、ROI、`segment_args` 和上传策略。 |
| 每日记录文件 | 检测历史、自动告警日志和告警诊断快照统一按自然日归档；检测历史写入 `/mnt/sdcard/images/upload_history_YYYYMMDD.json`，告警日志追加到 `/mnt/sdcard/logs/qt_alarm_YYYYMMDD.log`，诊断快照追加到 `/mnt/sdcard/logs/qt_alarm_snapshot_YYYYMMDD.txt`；第二天自动新建当天文件。 | 虚拟机执行 `./test_qt_kms_overlay_assets.sh`；板端执行 `--detect-self-test`、`--alarm-snapshot-self-test`、`--alarm-log-self-test` 后用 `date +%Y%m%d` 组装路径，再执行 `test -s`、`tail`、`grep` 验证当天文件非空并含检测/告警字段。 |
| 正式反向隧道 | 新增板端 `board-review-tunnel.sh` 和 `S91board-review-tunnel`，部署后由板端主动连接云服务器并守护 `ssh -R`；新增云端检查脚本和 timer。 | 板端执行 `/etc/init.d/S91board-review-tunnel status`；云端执行 `systemctl status yunduan-board-review-tunnel-check.timer`、`ss -ltnp | grep 127.0.0.1:18081`、`curl -i --max-time 5 http://127.0.0.1:18081/api/v1/review-result`。 |
| 最终运行边界 | 最终现场只需要板端和云服务器常驻；Windows/虚拟机只用于开发部署，不参与正式隧道。 | 杀掉临时 Windows/VM `ssh -R` 后，确认板端 monitor 仍能维持云端 `127.0.0.1:18081`。 |

## 修改文件清单

| 路径 | 修改原因 |
|---|---|
| `20_uvc_camera/qt_camera_display/main.cpp` | 注册 `V4L2VideoItem`，向 QML 注入摄像头节点、采集尺寸、视频后端、GStreamer 状态等上下文参数；`CameraStorageController` 负责请求 overlay 保存当前帧、串行调用 `defect-classify` 和 `defect-segment`、通过 `detectClassificationReady` 在第一个模型结束后立即通知 QML 刷新零件类型和类别、通过 `detectModelsReady` 在两个模型结束后立即通知 QML 刷新 `fused_status/fused_result/fused_reason` 与 `total_time_ms`、按分类和 UNet 综合结果设置 `CLOUD_RESULT=good/bad/review`，并把分类 `class=gasket_good/gasket_bad` 统一剥离成 `CLOUD_PART_CODE=gasket` 后调用 `defect-cos-upload --jpg <source> --annotated <raw> <overlay> <mask>` 上传本次检测图片、调用 `sdcard-safe-remove`；新增 `isUploadStatusSuccess()` 和 `isUploadStatusFailure()` 统一识别 `upload_status=OK/FAIL/SKIP`、`上传成功`、`上传失败`，让检测历史追加、失败重发排序、工作流文案和告警状态不再各自按字符串前缀判断；`UploadHistoryModel` 启动时读取旧 `/mnt/sdcard/images/upload_history.json` 和多天 `/mnt/sdcard/images/upload_history_*.json`，写入时按 `upload_time` 分配到 `upload_history_YYYYMMDD.json`，删除、云端复核和失败重发都会同步写回对应日期文件；`saveAlarmSnapshotToSdCard()` 追加到当天 `qt_alarm_snapshot_YYYYMMDD.txt`，`recordAlarmIssueToSdCard()` 追加到当天 `qt_alarm_YYYYMMDD.log`，`recordSettingsSummaryToSdCard()` 追加到当天 `qt_settings_YYYYMMDD.log`，三者共用目录创建、挂载/可写检查、UTF-8 写入、`flush` 和 `fsync` 逻辑；新增 `LogFileModel` 只读扫描 `/mnt/sdcard/logs` 下 `.log/.txt`，向 QML 暴露日志文件名、路径、类型、大小、修改时间和全文读取接口；`DeviceHealthController` 每 8 秒周期异步探测 4G、相机、F4、云端和 SD 卡真实状态，4G/云端进程启动失败通过异步错误信号回写，不在健康检测刷新路径等待启动，周期刷新保留网络/云端上一轮稳定状态，F4 串口主链路只发送二进制 `HEARTBEAT/START_CYCLE/QUERY_STATUS/BELT_MANUAL_CONTROL` 等帧，正确只认 `ACK/STATUS_REPORT`，错误只认 `NACK/FAULT_REPORT`；摄像头连续离线时只调用 `restart-overlay` 重启 overlay 视频进程，SD 卡状态会区分未挂载和不可写；提供 `--detect-self-test`、`--storage-self-test`、`--alarm-snapshot-self-test`、`--alarm-log-self-test` 和 `--settings-log-self-test` SSH 自检入口。 |
| `20_uvc_camera/qt_camera_display/qml/Main.qml` | 实现 1024x600 工业检测界面、状态栏、结果面板、统计区、`开始/暂停/继续/停止` 触摸按钮，以及 `检测`、`安全卸载` 两个真实操作按钮；顶部网络、相机、F4、位置、云端和告警页设备健康矩阵绑定真实状态，位置来自 `deviceHealth.locationShortText`，Qt 开机只执行一次高德 IP 省份定位，成功后显示 `河南省` 这类省份文本，4G 测试不通过不显示在线，F4 未串口握手成功不显示接入，KMS 相机未出帧或帧序号停滞不显示在线；新增 `uploadStatusTokenValue()`、`isUploadStatusSuccess()`、`isUploadStatusFailure()`，历史详情、统计上传成功率、重新发送按钮和云端告警统一按 `upload_status=OK/FAIL/SKIP` 判断；`storageToast` 移入 `globalStorageToastLayer` 并设置 `z: 900`，使检测、参数保存、告警诊断等绿色/红色底部提示在历史、统计、手动、参数、告警和日志页面都能显示在最上层；新增 `日志查看` 页面，进入时调用 `refreshLogFileList()` 刷新 `/mnt/sdcard/logs` 文件列表，点击日志行后通过 `openLogDetail()` 打开 `logDetailOverlay`，完整日志由 `logDetailFlickable` 滚动承载；告警页设备健康矩阵的 `4G` 格直接绑定 `deviceHealth.networkStatusText`，`定位` 格直接绑定 `deviceHealth.locationStatusText`，处理建议面板通过 `查看全部` 打开 `alarmAdviceDetailOverlay`，完整建议由 `alarmFullAdviceText()` 生成并通过 `alarmAdviceDetailFlickable` 滚动阅读；相机恢复在线时，`onCameraStatusChanged` 必须同时满足 `bootOverlayRestoreFinished`、`!splashOverlayVisible` 和当前在首页，才会发送 `VISIBLE 1` 恢复 overlay 画面，避免健康检测先于 Qt 启动画面把摄像头层打开；返回首页的 `switchPage()` 路径也使用同一启动完成门控，不在首页则保持隐藏；`检测` 调用异步 `requestDetectCurrentFrame()`，检测期间显示 `检测中...`，收到 `detectClassificationReady` 后立即显示模型识别出的零件类型、短类别、分类初判和置信度，但主结果显示“等待综合判定”，收到 `detectModelsReady` 后用综合判定覆盖首页主状态并显示双模型总耗时，最终 `detectCurrentFrameFinished` 只恢复忙状态并显示上传/历史记录结果；首页窄结果栏通过 `compactHomeClassText()` 和 `compactHomeModelText()` 显示短类别和短模型结论，`gasket/gasket_good` 显示为 `波形垫圈`，`washer` 显示为 `平垫圈`，`splitwasher` 显示为 `弹性垫圈`；参数设置页通过 `detectSettings` 读取和写入真实检测配置，`零件与模型判定` 可调模型阈值、复核阈值和 ROI，`视觉检测策略` 可调 UNet 最小像素阈值和 overlay 透明度，`相机、存储与上传` 可切换自动上传，`保存配置` 调用 C++ 写 `/mnt/sdcard/config/defect_ui_config.json` 并追加参数日志，`导出摘要` 直接追加参数日志；`F4接入边界` 明确传送带 BELT 已接、称重 `WEIGHT_CALIBRATE` 已接、相机运动轴不在 Qt 页面显示，避免把未实现运动轴显示成可操作状态；两个参数详情按钮使用底部右侧预留区域，摘要列表压缩为 4 行，底部状态文字在左侧显示，避免按钮遮挡文本；每次打开参数详情都会把 `settingsDetailFlickable.contentY` 重置为 0；告警维护页通过 `evaluateRuntimeAlarms()` 监听真实问题并写入每日日志。 |
| `20_uvc_camera/qt_camera_display/README.md` | 同步记录告警处理建议 `查看全部` 弹层、设备健康 `4G` 状态格、修改原因、板端验证命令和失败排查方式，避免后续只看文档时仍以为右下角建议只能显示短文本或最后一格仍是参数配置。 |
| `22_4g_ppp/4g-location` | 新增高德 IP 省份定位脚本；Qt 只在开机第一轮健康检测调用一次 `4g-location once`，脚本读取 `AMAP_WEB_KEY` 或 `/etc/4g-location/amap-web-key`，访问 `https://restapi.amap.com/v3/ip`，成功后写 `/var/run/4g-location.state` 并输出 `state=ip_ok/display=河南省/short_display=河南省`；脚本不访问 GPS、AT 串口或 `/dev/ttyUSB*`，`status` 仅供 SSH 人工排查缓存。 |
| `20_uvc_camera/qt_camera_display/qml/GstVideoSurface.qml` | 为 `qmlglsink` 桥接路线提供 QML 视频承载面。 |
| `20_uvc_camera/qt_camera_display/v4l2_video_item.h` | 定义 Qt Quick 自定义视频 Item、采集线程状态和 QML 可配置属性。 |
| `20_uvc_camera/qt_camera_display/v4l2_video_item.cpp` | 实现 V4L2 mmap 采集、YUYV 帧上传、OpenGL ES shader 转换、状态回传和安全预览渲染。 |
| `20_uvc_camera/qt_camera_display/qt_camera_display.pro` | 维护 Qt 工程、QML 资源、C++ 源文件和链接依赖。 |
| `20_uvc_camera/qt_camera_display/qml.qrc` | 把 QML 资源打进 Qt 程序，部署时不依赖散落的 QML 文件；QML 文件使用 `compress="0"` 关闭资源压缩，便于交叉构建后用 `strings` 直接确认板端二进制包含本次界面 marker。 |
| `20_uvc_camera/qt_camera_display/build_qt_camera_display.sh` | 在虚拟机中自动加载 ST Qt/Wayland SDK 环境并交叉编译 Qt 程序。 |
| `20_uvc_camera/qt_camera_display/deploy_qt_camera_display.sh` | 把 Qt 二进制、脚本、QML/资源、overlay 工具、`defect-cos-upload` 上传脚本、`board-review-tunnel.sh`、`S05display-quiet`、`S90uvc-camera` 和 `S91board-review-tunnel` 部署到 NFS rootfs；部署环境提供 `CLOUD_ACCOUNT/CLOUD_PASSWORD` 时，会在 rootfs 中生成本地私有的 `cos-upload.env`。 |
| `20_uvc_camera/qt_camera_display/cloud_review_result_api.md` | 说明云端“修正板端结果”按钮如何调用板端 `POST /api/v1/review-result`，检测记录上传时 `part_id/part_code` 的零件类型映射规则，以及正式反向隧道链路：板端独立守护 `ssh -R`，云端只周期检查 `127.0.0.1:18081`。 |
| `20_uvc_camera/qt_camera_display/install_qt_runtime_from_sdk.sh` | 从 ST SDK sysroot 复制 Qt5、QML、eglfs/wayland 插件和 Vivante EGL/GLES 运行库到 rootfs。 |
| `20_uvc_camera/qt_camera_display/run_qt_camera_display.sh` | 板端 Qt 主运行脚本，处理 eglfs/wayland、Goodix 触摸输入、旧预览停止、后端选择和日志。 |
| `20_uvc_camera/qt_camera_display/probe_zero_copy_video_path.sh` | 板端零拷贝候选路线探测脚本，收集 `/dev/video*`、DRM、GStreamer 和 CPU/Oops 证据。 |
| `20_uvc_camera/qt_camera_display/uvc_kms_overlay.c` | 正式化 KMS overlay 视频进程，使用 V4L2 mmap 采集、NEON `YUYV -> ARGB8888` 转换和 `drmModeSetPlane` 上屏；Unix socket 控制端点支持 `SAVE_DETECT <dir>`，把当前原始摄像头帧保存为检测 source JPG 并 `fsync`；保留 `SAVE_DUAL` 兼容旧保存自检；新增 `VISIBLE 0/1` 命令和 `-V 0|1` 初始可见性参数，开机时可先隐藏实时视频 plane，避免盖住启动动画和历史图片；新增 `STATUS` 命令返回 `has_frame/serial/visible/width/height`，Qt 用它判断相机是否真实出帧。 |
| `20_uvc_camera/qt_camera_display/build_uvc_kms_overlay.sh` | 交叉编译 `uvc_kms_overlay`，链接 libdrm、libjpeg、libpng、zlib 并生成板端可执行文件；脚本使用 `OVERLAY_CC` 作为专用覆盖变量，避免已经 `source` ST Qt SDK 后的 `CC="编译器 参数"` 污染 overlay 构建。 |
| `20_uvc_camera/qt_camera_display/fb_boot_splash.c` | 早期静态启动首帧绘制器，直接 mmap `/dev/fb0`，优先读取 `/root/qt_camera_display/boot_splash.rgb565` 并整张 blit 到 framebuffer；资源缺失时才回退到低保真 C 几何绘制；不依赖 Qt、OpenGL、DRM、PNG 解码库或摄像头。 |
| `20_uvc_camera/qt_camera_display/ai_boot_splash_preview.html` | AI 品牌启动图视觉源，用纯 HTML/CSS 重绘用户提供的芯片 Logo、中英文标语和赛事名称，并额外加入电路背景、机器视觉 ROI 和底部技术铭牌；后续改视觉优先改这个文件。 |
| `20_uvc_camera/qt_camera_display/generate_boot_splash_asset.py` | 新增启动图资源生成脚本，调用 Edge/Chromium 无头模式把 HTML 截成 `1024x600` PNG，再用 Pillow 转成 `RGB565 little-endian` raw 文件。 |
| `20_uvc_camera/qt_camera_display/boot_splash.png` | 由 HTML 渲染出的 1024x600 预览图，可在电脑上直接确认最终静态首帧像素效果。 |
| `20_uvc_camera/qt_camera_display/boot_splash.rgb565` | 板端实际读取的启动图资源，大小固定为 `1024*600*2=1228800` 字节，部署到 `/root/qt_camera_display/boot_splash.rgb565`。 |
| `20_uvc_camera/qt_camera_display/build_fb_boot_splash.sh` | 交叉编译 `fb_boot_splash` 并生成 `build-mp157/fb_boot_splash`；脚本使用 `SPLASH_CC` 作为专用编译器变量，避免 ST Qt SDK 的 `CC` 污染非 Qt 辅助程序构建。 |
| `20_uvc_camera/qt_camera_display/defect-cos-upload` | 板端 COS 上传脚本，先读取 `/root/qt_camera_display/cos-upload.env` 作为默认上传账号；`--jpg` 原图按 `file_kind=source` 上传，重复传入的 `--annotated <jpg/png>` 检测结果图按 `file_kind=annotated` 上传，并按扩展名自动使用 `image/jpeg` 或 `image/png`；`CLOUD_RESULT` 只接受 `good/bad/review`，检测链路必须显式传入模型结果，避免坏品记录被固定写成良品；source 和 annotated 均完成 COS PUT 与 `/records/<id>/files` 登记后，详情回查失败只在 stderr 写 `verify_status=warning`，stdout 仍输出 `上传成功：upload_status=OK record_id=... record_no=... verify_status=warning`，避免云端已经有记录但本地历史显示上传失败；`CLOUD_PART_CODE` 表示零件类型，脚本先按 `/api/v1/parts?limit=100` 查询云端 `part_id`，查不到时不再失败，而是在创建记录请求中发送 `part_code/part_name/part_category/auto_create_part=true` 让云端自动创建零件；`gasket/wave_washer` 自动写入 `part_name=波形垫圈`、`part_category=垫圈类`，`washer` 写入 `平垫圈 / 垫圈类`，`splitwasher` 写入 `弹性垫圈 / 垫圈类`，可用 `CLOUD_PART_NAME/CLOUD_PART_CATEGORY` 覆盖。旧 `--png` 参数仍兼容，内部等同于一张 `--annotated` 结果图。 |
| `20_uvc_camera/qt_camera_display/board-review-tunnel.sh` | 板端云端复核回写反向隧道守护脚本，使用 `/root/.ssh/id_ed25519_yunfuwu_tunnel` 主动连接云服务器，建立 `ssh -R 127.0.0.1:18081:127.0.0.1:18080`；脚本保存 monitor PID 和 ssh PID，周期检查 ssh 进程，网络断开、云端重启或端口释放后自动重连，日志写 `/tmp/board-review-tunnel.log`。 |
| `20_uvc_camera/S91board-review-tunnel` | Buildroot SysV init 入口，在 `S90uvc-camera` 之后启动板端反向隧道守护，支持 `start/stop/restart/status`。 |
| `20_uvc_camera/qt_camera_display/cloud_check_board_review_tunnel.sh` | 云端检测脚本，只检查云端本机 `127.0.0.1:18081` 是否监听并能转发到板端 HTTP 服务；它不负责重连，真正重连由板端守护完成。 |
| `20_uvc_camera/qt_camera_display/yunduan-board-review-tunnel-check.service` | 云端 systemd oneshot 服务，执行 `/opt/yunduan/scripts/check_board_review_tunnel.sh`。 |
| `20_uvc_camera/qt_camera_display/yunduan-board-review-tunnel-check.timer` | 云端 systemd timer，每 60 秒运行一次隧道检查。 |
| `20_uvc_camera/qt_camera_display/defect_segment.cpp` | 新增独立 ONNX Runtime + libjpeg/libpng UNet 分割程序，读取分类 source JPG，裁剪中心 ROI，执行 INT8 UNet，输出 `RESULT_SEG`，并生成 raw JPG、overlay JPG、mask PNG 三张 annotated 结果图。 |
| `20_uvc_camera/qt_camera_display/build_defect_segment.sh` | 新增 `defect-segment` 交叉编译脚本，依赖 ARMv7 ONNX Runtime SDK、libjpeg 和 libpng。 |
| `20_uvc_camera/qt_camera_display/run_qt_kms_overlay_display.sh` | 板端 Qt + KMS overlay 生命周期脚本，支持 `start/stop/restart/restart-overlay/status/restore-fallback`；启动 Qt 前调用 `fb_boot_splash` 先显示静态首帧，再以隐藏状态启动 Qt UI 和 overlay 视频，等 QML 启动动画完成后由 Qt 发送 `VISIBLE 1` 恢复摄像头画面；`restart-overlay` 只重启 `uvc_kms_overlay` 且保持视频层隐藏，用于 USB 摄像头热拔插恢复，不杀 Qt 界面，避免当前在历史/统计/参数等页面时被视频层覆盖。 |
| `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh` | 静态检查 overlay 源码、构建脚本、运行脚本、触摸按钮、SD 卡按钮、历史记录模型/页面、日志查看模型/页面/详情弹窗、历史检测信息紧凑显示、首页结果短字段、统一上传状态判定、COS 回查 warning 降级、全局底部提示浮层、失败历史重新发送、统计分析页面、统计分布两列布局、手动控制页面、参数设置页面、参数详情弹层、未接硬件项清理、告警维护页面、处理建议完整弹层、4G 健康状态格、真实设备健康状态、健康检测非阻塞契约、每日检测历史/告警日志/诊断快照/参数日志落盘接口、早期静态首帧绘制器、启动动画覆盖层、QML 资源不压缩打包和默认启动契约。 |
| `docs/plans/2026-05-21-qt-alarm-advice-detail-implementation.md` | 记录告警处理建议完整说明和 4G 健康状态格的实施计划、测试先行步骤、QML 改动、文档更新和验证命令。 |
| `docs/plans/2026-05-21-qt-settings-detail-implementation.md` | 记录参数设置页视觉检测策略详情、F4 接入边界详情、未接硬件项清理、静态测试先行、QML 改动、文档更新和验证命令。 |
| `docs/plans/2026-07-04-mp157-f4-camera-z-sync-flow-design.md` | 记录 ROI 居中后 Z 轴下降、前后轴微调、模型检测后 Z 轴回升的同步简化版设计、协议边界、参数范围和验证矩阵。 |
| `docs/plans/2026-07-04-mp157-f4-camera-z-sync-flow.md` | 记录同步简化版的测试先行、MP157 参数和协议、QML 自动流程、三轴手动弹窗、F4 Emm42 位置模式、协议分发和文档验证步骤。 |
| `20_uvc_camera/S05display-quiet` | Buildroot 早期 init 脚本，尽早关闭 framebuffer console 光标、清理 `/dev/tty0` 显示残留，并在 `/dev/fb0` 已可用时调用 `fb_boot_splash` 绘制静态首帧，用于缩短内核 logo 隐藏后到 Qt 启动前的黑屏窗口。 |
| `20_uvc_camera/S90uvc-camera` | 等待 `/dev/fb0` 可用后再次调用 `fb_boot_splash`，确保等待摄像头、GPU 和 Qt 进程期间 LCD 保持静态启动图；随后默认启动 Qt + KMS overlay 正式路线。 |
| `docs/plans/2026-05-16-qt-manual-control-design.md` | 记录手动控制页的目标、控制边界、安全交互、状态契约和验收方式。 |
| `docs/plans/2026-05-16-qt-manual-control-implementation.md` | 记录手动控制页的测试先行、QML 状态 helper、页面布局、文档同步和验证命令。 |
| `docs/plans/2026-05-16-qt-statistics-analysis-design.md` | 记录统计分析页的用户目标、布局结构、数据来源、交互边界和验收方式。 |
| `docs/plans/2026-05-16-qt-statistics-analysis-implementation.md` | 记录统计分析页的实施步骤、TDD 静态契约、QML 页面改动、文档更新和验证命令。 |
| `20_uvc_camera/qt_camera_display/zero_copy_hardware_video_plan.md` | 记录 Wayland/KMS/custom GL 等硬件视频路线计划、结果和下一步。 |
| `20_uvc_camera/qt_camera_display/zero_copy_next_chat_plan.md` | 记录下一轮优化上下文，避免重复尝试已失败的 qmlglsink+dmabuf 路线。 |
| `20_uvc_camera/S90uvc-camera` | 默认后端切到 `qt-kms-overlay`，开机时进入 Qt 工业界面而不是旧 framebuffer 预览。 |
| `.trellis/spec/backend/embedded-linux-workflow.md` | 沉淀 Qt UVC fallback、KMS overlay、zero-copy 候选、后台运行和验证矩阵。 |
| `.trellis/spec/frontend/component-guidelines.md` | 沉淀 Qt/Wayland SDK 编译约定、Goodix 触摸输入、KMS overlay UI 按钮验收规则，以及检测图片保存和 COS 上传的跨层契约。 |
| `docs/plans/2026-05-03-qt-sdcard-camera-actions.md` | 记录本次 SD 卡保存图片和安全卸载按钮的实施方案、数据路径和验证步骤。 |
| `docs/plans/2026-05-07-qt-cos-jpg-png-save-design.md` | 记录保存按钮双格式落盘和 COS 上传设计。 |
| `docs/plans/2026-05-07-qt-cos-jpg-png-save-implementation.md` | 记录 JPG/PNG 保存和 COS 上传实施计划、测试步骤和验证边界。 |

## 使用流程

| 阶段 | 命令 | 预期结果 |
|---|---|---|
| 加载 Qt SDK 并构建 | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ./build_qt_camera_display.sh` | 生成 `build-mp157/qt_camera_display`。 |
| 构建 KMS overlay 工具 | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ./build_uvc_kms_overlay.sh` | 生成 `build-mp157/uvc_kms_overlay`；即使当前 shell 已经加载 ST Qt SDK，也不能继承 SDK 的 `CC`，需要覆盖编译器时使用 `OVERLAY_CC=/path/to/gcc ./build_uvc_kms_overlay.sh`。 |
| 构建早期静态首帧工具 | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ./build_fb_boot_splash.sh` | 生成 `build-mp157/fb_boot_splash`；即使当前 shell 已经加载 ST Qt SDK，也不能继承 SDK 的 `CC`，需要覆盖编译器时使用 `SPLASH_CC=/path/to/gcc ./build_fb_boot_splash.sh`。 |
| 生成早期静态首帧资源 | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && python3 generate_boot_splash_asset.py` | 生成 `boot_splash.png` 和 `boot_splash.rgb565`；`boot_splash.rgb565` 文件大小必须是 `1228800` 字节。 |
| 本地预览 AI 品牌启动图 | 在 Windows 浏览器打开 `C:\Users\caofengrui\Desktop\linux\20_uvc_camera\qt_camera_display\ai_boot_splash_preview.html` | 显示 1024x600 静态启动图预览；赛事名、芯片图标、`9th`、中文标题、英文副标题、电路背景、ROI 线稿和底部技术铭牌均由 HTML/CSS 绘制，不依赖外部图片或网络资源。 |
| 构建 MobileNet 分类程序 | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ORT_ROOT=/home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display/onnxruntime-arm ./build_defect_classify.sh` | 生成 `build-mp157/defect-classify`。 |
| 构建 UNet 分割程序 | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ORT_ROOT=/home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display/onnxruntime-arm ./build_defect_segment.sh` | 生成 `build-mp157/defect-segment`。 |
| 静态契约检查 | `./test_qt_kms_overlay_assets.sh` | 输出 `PASS: Qt KMS overlay assets contract`。 |
| 部署到 NFS rootfs | `sudo DEFECT_MODEL_SRC=/home/cfr/linux/model_picture/checkpoints_classify/defect_classifier_static_mixed_int8.onnx DEFECT_LABELS_SRC=/home/cfr/linux/model_picture/checkpoints_classify/defect_classifier_static_mixed_int8_labels.json DEFECT_UNET_MODEL_SRC=/tmp/defect_unet_test_decoder_head_int8.onnx ORT_ROOT=/home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display/onnxruntime-arm ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` | 板端 `/root/qt_camera_display/` 获得 Qt 程序、overlay 工具、`fb_boot_splash`、`boot_splash.rgb565`、`defect-classify`、`defect-segment`、分类模型、UNet 模型、labels、ONNX Runtime 库和运行脚本，`/etc/init.d/` 获得 `S05display-quiet` 和 `S90uvc-camera`。 |
| 部署默认上传账号 | `CLOUD_ACCOUNT='<账号>' CLOUD_PASSWORD='<密码>' sudo -E ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` | 额外生成 `/home/cfr/linux/nfs/rootfs/root/qt_camera_display/cos-upload.env`，权限为 `600`；检测按钮后续可不再手工传账号密码。 |
| 安装 Qt runtime | `./install_qt_runtime_from_sdk.sh /home/cfr/linux/nfs/rootfs` | rootfs 获得 Qt5 库、QML 模块、eglfs/wayland 插件和 Vivante 库。 |
| 板端启动正式路线 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart` | Qt UI 与 overlay 视频同时运行。 |
| 只重启 overlay | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart-overlay` | 只重启 `uvc_kms_overlay` 并保持隐藏，Qt 主界面不退出；用于 USB 摄像头拔插后的手动恢复或健康检测自动恢复，画面显示恢复由 QML 在首页时发送 `VISIBLE 1`。 |
| 板端查看状态 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh status` | 显示 Qt PID、overlay PID、fallback PID、plane、控制 socket 和日志路径。 |
| overlay 内存定位自检 | `printf 'LOCATE\n' \| nc -U /tmp/uvc-kms-overlay-control.sock` | overlay 不保存图片、不跑模型，直接返回 `OK LOCATE has_target=0/1 frame_id=... width=640 height=480 center_x=... center_y=... bbox_x=... confidence=...`；用于确认自动视觉坐标来源真实可用。 |
| 首页自动居中和 Z 轴下探流程 | 开发板屏幕点击首页 `开始` | F4 ACK `START_CYCLE` 后传送带扫描，MP157 每 100ms 请求 `LOCATE`，识别到目标后发送 `VISION_POS`；零件从画面上方进入后，`center_y` 向 `height/2` 靠近；连续 3 帧进入 ±24px 后发送 `BELT_STOP_CENTERED`；ACK 后发送 `ACTUATOR_POS_MOVE actuator=2 direction=DOWN steps=zDownFixedSteps`，下降 ACK 后等待约 3 秒让摄像头对焦稳定，再复查 ROI，必要时发送 `ACTUATOR_POS_MOVE actuator=1 direction=0/1 steps=minStep` 微调；ROI 复查通过后自动进入模型检测，检测完成后发送 `ACTUATOR_POS_MOVE actuator=2 direction=UP steps=zUpFixedSteps` 回升；目标已经出现后若短暂返回 `has_target=0`，界面显示 `目标短暂丢失/连续丢失但保持停机`，不会再发 `VISION_LOST reason=1` 让 F4 继续扫描。 |
| 早期静态首帧观察 | 开发板 SSH | `/root/qt_camera_display/fb_boot_splash -f /dev/fb0; echo $?` | 命令返回 `0`，LCD 立即显示由 HTML 渲染资源生成的 AI 竞赛品牌静态首帧，中文标题、白色 Logo 面板、光晕和底部技术铭牌应与 `boot_splash.png` 基本一致；不依赖 Qt、GPU、摄像头或 overlay。 |
| 开机动画观察 | 开发板重启或执行 `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart` 后观察 LCD | 先看到 `fb_boot_splash` 的 AI 竞赛品牌静态首帧，然后切换为 QML `工业缺陷检测系统`、`STM32MP157 Vision Inspection Terminal`、相机扫描窗口、`加载相机/初始化检测模型/连接运动控制/挂载存储/进入检测界面` 和进度条，随后淡出进入首页并恢复摄像头画面；摄像头画面不应抢在 Qt 启动画面前出现，画面中不出现 Ubuntu 企鹅图标。 |
| SSH 双模型检测自检 | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 'cd /root/qt_camera_display && ./qt_camera_display --detect-self-test'` | 不启动 QML，直接复用点击 `检测` 的链路：保存 source JPG、运行 MobileNetV3-Small、运行 UNet、上传 source 和全部 annotated 图、追加历史记录；成功输出以 `RESULT ` 开头。 |
| SSH 旧保存链路自检 | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 '/root/qt_camera_display/qt_camera_display --storage-self-test'` | 不启动 QML，直接复用旧 Qt 保存控制器请求 overlay 保存 JPG/PNG；该入口只保留作底层兼容诊断，正式界面不再暴露独立保存图片按钮。 |
| SSH 告警诊断自检 | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 'out=$(/root/qt_camera_display/qt_camera_display --alarm-snapshot-self-test); echo "$out"; file=${out#诊断已保存：}; day=$(date +%Y%m%d); test "$file" = "/mnt/sdcard/logs/qt_alarm_snapshot_${day}.txt"; test -s "$file"; wc -c "$file"; tail -n 30 "$file"'` | 不启动 QML，直接复用 Qt 告警诊断控制器追加当天 `/mnt/sdcard/logs/qt_alarm_snapshot_YYYYMMDD.txt`；输出 `诊断已保存：...`，文件非空，`tail` 能看到 `alarm_code=`、`camera_status=` 和 `[recent_alarm_history]`。 |
| SSH 自动告警日志自检 | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 'out=$(/root/qt_camera_display/qt_camera_display --alarm-log-self-test); echo "$out"; file=${out#告警日志已保存：}; day=$(date +%Y%m%d); test "$file" = "/mnt/sdcard/logs/qt_alarm_${day}.log"; test -s "$file"; wc -c "$file"; tail -n 40 "$file"'` | 不启动 QML，直接复用自动告警落盘控制器追加当天 `/mnt/sdcard/logs/qt_alarm_YYYYMMDD.log`；内容包含 `source=model-detect-failed`、`alarm_code=ALM-MODEL-001` 和 `[troubleshooting]`。 |
| 按钮双模型检测 | 开发板屏幕点击 `检测` | 按钮立即显示 `检测中...`；第一个分类模型完成后右侧结果面板立即显示模型识别出的零件名、分类初判、类别和百分制置信度，同时主结果显示 `等待综合判定`；UNet 完成后立即显示综合判定和双模型总耗时，若 `fused_status=BAD` 则不能继续显示良品；COS 上传完成后底部提示条显示 `RESULT ... fused_result=good/bad/review total_time_ms=... upload_status=OK/FAIL`，当天 `/mnt/sdcard/images/upload_history_YYYYMMDD.json` 新增一条含 source 和 3 张 UNet annotated 图片的历史记录。 |
| 检测期间界面响应 | 开发板屏幕 | 点击 `检测` 后，在仍显示 `检测中...` 时立即点击左侧 `历史记录`、`统计分析`、`手动控制`、`参数设置` 或 `告警维护` | 页面应立即切换或响应触摸；只有重复 `检测` 和 `安全卸载` 暂时不可点。若页面切换卡住，检查 QML 是否又直接调用同步检测函数；若能重复检测，检查 `detectInProgress`、`detectImageBusy` 和 `requestDetectCurrentFrame()`。 |
| 查看检测历史 | 开发板屏幕点击左侧 `历史记录`，横向滑动检测时间卡片并点击 `查看` | 实时视频 plane 被临时隐藏；进入历史页默认停留在检测历史列表，并自动选中、滚动到最新卡片；点击 `查看` 后进入详情页；详情页左侧可横向滑动原始图片和 UNet raw/overlay/mask，右侧显示上传时间、记录ID、图片数量、云端编号、云端状态，以及“检测结论/可信度/缺陷提示/图片留档”普通中文说明，不单独显示本地图片路径。 |
| 重新发送失败历史 | 开发板屏幕点击左侧 `历史记录`，打开一条云端状态为上传失败的详情，再点击 `重新发送` | 按钮显示 `发送中` 并置灰，Qt 主界面仍可响应；成功后同一条历史记录的本地上传时间改为本次重发时间，记录移动到历史列表最后一项，云端状态、记录ID、云端编号更新；失败时保留原时间、原位置和失败摘要，并允许再次点击。 |
| 查看统计分析 | 开发板屏幕点击左侧 `统计分析` | 实时视频 plane 被临时隐藏；页面显示总记录、良品/待复核、上传成功率、图片总量、最近检测趋势、结果分布、云端与文件状态、最近记录表；最近记录卡片可上下滑动查看更多记录，点击任一记录行可进入对应历史详情页。 |
| 打开手动控制 | 开发板屏幕点击左侧 `手动控制` | 实时视频 plane 被临时隐藏；页面显示传送带、检测辅助、安全状态、人工复核和命令日志；点击手动电机控制入口会打开三页弹窗，分别操作传送带、摄像头前后轴、摄像头上下轴；未进入手动模式时运动按钮置灰或提示先进入手动模式。 |
| 手动三轴运动和模拟急停 | 开发板屏幕点击左侧 `手动控制`，进入手动模式后打开三轴弹窗 | 传送带和前后轴正反按钮记录命令日志并下发 `ACTUATOR_VEL_MOVE`，按一次持续运动直到点击停止；上下轴下降/上升按钮下发 `ACTUATOR_POS_MOVE actuator=2`，分别使用 `zDownFixedSteps/zUpFixedSteps` 固定步数；停止按钮下发 `ACTUATOR_STOP actuator=<当前轴>`；模拟急停下发 `ACTUATOR_STOP actuator=0xFF`，底部安全状态文字可上下滑动查看完整内容。 |
| 手动页检测当前帧 | 开发板屏幕点击 `手动控制`，再点击 `检测当前帧` | 复用首页 `检测` 链路，生成分类 source 图、UNet annotated 图、云端记录和本地历史，并在命令日志记录本次辅助动作。 |
| 打开参数设置 | 开发板屏幕点击左侧 `参数设置` | 实时视频 plane 被临时隐藏；页面显示 `零件与模型判定`、`视觉检测策略`、`F4接入边界`、`相机、存储与上传`、参数摘要和操作按钮；页面显示 `真实检测配置`、JSON 路径 `/mnt/sdcard/config/defect_ui_config.json` 和当前 ROI/UNet/上传状态。 |
| 调整参数设置 | 开发板屏幕点击 `参数设置` 页中的 `+/-`、`切换`、`应用检测`、`保存配置`、`恢复默认`、`导出摘要` | 可调模型阈值、复核阈值、ROI、UNet 最小像素、overlay 透明度和 COS 自动/本地策略；零件只在波形垫圈、平垫圈、弹性垫圈之间循环；摄像头上下电机弹窗可输入 `zDownFixedSteps` 和 `zUpFixedSteps`，范围为 `0~4294967295 step`；检测线程立即使用内存参数；`保存配置` 真实写入 `/mnt/sdcard/config/defect_ui_config.json` 并追加参数日志；`导出摘要` 不改 JSON，只把当前参数摘要追加到当天 `qt_settings_YYYYMMDD.log`；视觉参数不下发 F4，步进参数按现有 `STEPPER_PARAM_SET` 下发，Z 轴下探/回升固定步数用于 MP157 自动流程发 `ACTUATOR_POS_MOVE`；`设当前位置为零点` 发送 `ACTUATOR_HOME actuator=<当前电机页>`，用于现场标定当前位置基准。 |
| 打开告警维护 | 开发板屏幕点击左侧 `告警维护` | 实时视频 plane 被临时隐藏；页面显示真实当前告警或 `ALM-OK/ALM-INIT` 占位、设备健康矩阵、告警历史和处理建议。 |
| 告警维护操作 | 开发板屏幕点击 `确认`、`清故障`、`刷新状态`、`保存诊断` | 当前告警状态或维护日志更新；点击 `保存诊断` 时底部提示条显示 `诊断已保存：/mnt/sdcard/logs/qt_alarm_snapshot_YYYYMMDD.txt` 或明确失败原因；清故障只改变维护状态，不代表真实 F4 联锁已经解除。 |
| SSH 查看告警日志和诊断快照 | 开发板屏幕点击 `保存诊断` 后，或执行 `--alarm-snapshot-self-test`/`--alarm-log-self-test` 后，在 SSH 执行 `day=$(date +%Y%m%d); snap=/mnt/sdcard/logs/qt_alarm_snapshot_${day}.txt; log=/mnt/sdcard/logs/qt_alarm_${day}.log; ls -lh "$snap" "$log"; test -s "$snap"; test -s "$log"; tail -n 30 "$snap"; tail -n 40 "$log"` | 当天快照和告警日志存在且大小大于 0，内容包含 `STM32MP157 Qt Alarm Snapshot`、`alarm_code=`、`device_health=`、`storage_state=`、`[recent_alarm_history]`、`source=`、问题描述和 `[troubleshooting]`。 |
| 删除检测历史 | 开发板屏幕点击左侧 `历史记录`，在某条卡片点击 `删除` | 该条记录从所属日期的 `/mnt/sdcard/images/upload_history_YYYYMMDD.json` 中移除，对应 source/annotated 文件同步删除；如果删除最后一条，历史页显示 `暂无检测记录`。 |
| 返回实时首页 | 历史页/统计页/手动页/参数页/告警页右上点击 `返回首页` 或左侧点击 `首页` | KMS overlay 实时视频 plane 恢复显示，回到实时检测布局。 |
| 按钮安全卸载 | 开发板屏幕点击 `安全卸载` | 底部提示条短暂显示卸载结果；脚本同步并卸载 SD 卡，提示后才能拔卡。 |
| 板端恢复兜底线 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restore-fallback` | 停止正式路线，恢复 `640x480@10fps -> drop6 -> BGRA -> kmssink` 可见线。 |
| 安全预览运行 | `/root/qt_camera_display/run_qt_camera_display.sh` | 使用默认 `320x240@10fps` V4L2 安全预览。 |
| 提高画质测试 | `CAMERA_WIDTH=640 CAMERA_HEIGHT=480 CAMERA_FPS=15 /root/qt_camera_display/run_qt_camera_display.sh` | 可观察较高画质，但 CPU 明显升高。 |

## 修改记录

| 时间 | 修改点 | 结果 |
|---|---|---|
| 2026-07-04 | 新增 Z 轴同步下探检测和三轴手动控制 | ROI 居中停传送带后，MP157 通过 `ACTUATOR_POS_MOVE` 让摄像头上下轴下降固定步数，收到下降 ACK 后等待约 3 秒让摄像头对焦稳定，再复查 ROI 并按需用前后轴微调，双模型检测完成后自动回升；手动控制弹窗改为传送带、前后轴、上下轴三页；传送带和前后轴按一次方向键下发 `ACTUATOR_VEL_MOVE` 并持续运动到停止，上下轴按一次下降/上升只执行 `zDownFixedSteps/zUpFixedSteps` 固定步数；模拟急停下发 `ACTUATOR_STOP actuator=0xFF`；安全状态区域改为可滑动；参数页新增 Z 轴下探/回升固定步数输入和 `ACTUATOR_HOME` 当前位置设零按钮。 |
| 2026-07-04 | 修正黑色波形零件短暂漏检 | 首页偏差改为 LOCATE 真实 `errorY`，等待/漏检时显示 `-- px`；MP157 居中死区与 F4 对齐到 ±24px；目标识别过后短暂漏检不再发会触发 F4 扫描的 `VISION_LOST reason=1`，而是保持停机等待重新识别；overlay 亮度差阈值降到 `12`，提升黑色零件连续识别概率。 |
| 2026-07-03 | 新增上方来料自动视觉居中 | overlay 支持 `LOCATE` 内存定位；Qt C++ 支持 `VISION_POS/VISION_LOST/BELT_STOP_CENTERED`；QML 在 `START_CYCLE` ACK 后启动 100ms 自动视觉循环，使用 `center_y` 对齐 `height/2`，连续 3 帧进入 ±10px 后停传送带；后续 2026-07-04 已改为 ACK 后进入 Z 轴下降、约 3 秒对焦等待、ROI 复查和自动检测流程。 |
| 2026-07-01 | 记录类文件改为每日归档 | `UploadHistoryModel` 新增每日历史文件规则：当天检测写 `/mnt/sdcard/images/upload_history_YYYYMMDD.json`，启动时汇总多天 `upload_history_*.json` 并兼容旧 `upload_history.json`；告警日志改为追加 `/mnt/sdcard/logs/qt_alarm_YYYYMMDD.log`，诊断快照改为追加 `/mnt/sdcard/logs/qt_alarm_snapshot_YYYYMMDD.txt`，跨天自动新建当天文件；静态检查脚本同步检查 `dailyHistoryFilePath`、`dailyLogFilePath` 和追加写入逻辑。 |
| 2026-04-30 | 新增 Qt Quick 工业检测界面和自定义 `V4L2VideoItem` 安全预览 | Qt UI、eglfs、galcore 和 UVC 显示打通。 |
| 2026-04-30 | 验证并记录 QtMultimedia `Camera + VideoOutput` 问题 | 该路线触发 `galcore _UserMemoryAttach/dma_map_sg` 内核 Oops，不作为稳定路线。 |
| 2026-04-30 | 增加 `probe_zero_copy_video_path.sh` 和 zero-copy 文档 | 明确 `v4l2src io-mode=dmabuf ! glupload ! glimagesink` 低 CPU但非 Qt 内嵌，`qmlglsink + dmabuf` 会崩溃。 |
| 2026-05-01 | 正式化 `uvc_kms_overlay.c` 和 `run_qt_kms_overlay_display.sh` | Qt UI 与 KMS overlay 视频共存，默认 `640x480@10fps`。 |
| 2026-05-01 | 增加 QML 触摸按钮和 Goodix 输入配置 | 用户实测 `开始/暂停/继续/停止` 按钮可改变状态栏文字。 |
| 2026-05-01 | `S90uvc-camera` 默认切到 `qt-kms-overlay` | 开机默认进入 Qt + KMS overlay 工业检测界面。 |
| 2026-05-03 | 增加 `保存图片` 和 `安全卸载` 两个 SD 卡按钮 | 保存按钮通过 Qt 控制器请求 `uvc_kms_overlay` 把当前显示帧写入 `/mnt/sdcard/images/*.ppm` 并 `fsync`；安全卸载按钮复用 `sdcard-safe-remove`。 |
| 2026-05-04 | 修正 SD 卡提示布局和按钮诊断 | 操作结果移到底部空白区并自动隐藏；Qt 保存控制器增加日志和 `--storage-self-test`，方便定位屏幕点击失败。 |
| 2026-05-04 | 修复 Qt 保存控制器误判 SD 卡未挂载 | 根因为 Qt 侧用 `QTextStream::atEnd()` 读取 `/proc/mounts` 时在 procfs 上提前判定文件结束；已改成 POSIX `fopen/fscanf/fclose` 逐字段解析，和 overlay 保存端保持一致。 |
| 2026-05-07 | 保存按钮改为 JPG/PNG 双格式并触发 COS 上传 | `uvc_kms_overlay` 新增 `SAVE_DUAL`，同一帧写出 `.jpg` 和 `.png`；Qt 控制器调用 `defect-cos-upload`，JPG 登记为 `source`，PNG 登记为 `annotated`。 |
| 2026-05-07 | 增加板端默认上传账号配置 | `defect-cos-upload` 启动时读取 `/root/qt_camera_display/cos-upload.env`，部署脚本可从部署环境变量生成权限 `600` 的本地配置文件；源码不保存真实账号密码。 |
| 2026-05-07 | 记录未完成问题：云端记录隔离失败 | 板端 `--storage-self-test` 能保存本次 JPG/PNG 并上传成功，但云端详情页复用 `record_id=3`，把历史图片和本次两张图累计显示到同一记录里；明天需要修成每次保存只关联本次 JPG+PNG 两张图片。 |
| 2026-05-07 | 修复上传脚本复用旧 `record_id=3` 的根因 | `defect-cos-upload` 改为专门解析创建记录响应的顶层 `id/record_no`，并把该 `record_id` 同时传给 COS prepare 和文件登记；prepare 返回的 `object_key` 必须包含当前 `record_no`，否则停止上传。 |
| 2026-05-08 | 修复保存上传时间多 8 小时 | 根因为板端 `date` 墙钟数值当时已经是本地时间但时区标签显示 UTC，旧脚本又用 `TZ=CST-8` 二次换算，导致 `captured_at/uploaded_at` 写成 21 点；现在先由 `board-time-sync` 校准系统 UTC，上传脚本再显式导出 `TZ=CST-8` 读取北京时间墙钟并追加 `+08:00`。 |
| 2026-05-08 | 记录时间排障顺序 | 遇到详情页时间偏移时，先对比 `board-time-sync status`、`TZ=CST-8 date`、`date -u`、服务器 nginx access log、板端 `/tmp/defect-cos-record*.json` 原始请求/响应和详情接口，不要先在前端或后端响应层反向加减 8 小时。 |
| 2026-05-08 | 修复 Qt 顶部状态栏时间仍显示 UTC | 根因为顶部时间来自 QML `new Date()`，它读取的是 `qt_camera_display` 进程本地时区；上传脚本导出 `TZ=CST-8` 不会影响已经启动的 Qt 进程。现在 `main.cpp` 在 `QGuiApplication` 创建前默认设置 `TZ=CST-8` 并调用 `tzset()`，`run_qt_camera_display.sh` 和 `run_qt_kms_overlay_display.sh` 也显式导出 `TZ=CST-8`。 |
| 2026-05-08 | 新增并整理 Qt 上传历史记录界面 | `保存图片` 完成后追加 `/mnt/sdcard/images/upload_history.json`；左侧导航 `历史记录` 页面按上传时间横向叠加卡片，点击 `查看` 后进入独立详情页，左侧滑动浏览 JPG/PNG，右侧显示检测结果、云端摘要、文件大小、本地路径和检测信息；历史页打开时通过 overlay `VISIBLE 0` 隐藏实时视频 plane，返回首页时用 `VISIBLE 1` 恢复。 |
| 2026-05-08 | 优化历史页滑动手感并增加删除按钮 | 历史列表和详情图片轮播增加缓存与滑动速度/减速度约束，减少左右滑动时突然跳页的感觉；历史卡片在 `查看` 上方新增 `删除`，点击后通过 `UploadHistoryModel::removeRecord()` 同步删除 JSON 记录和对应 JPG/PNG 图片文件。 |
| 2026-05-08 | 修复历史卡片点选后列表自动回滚 | 根因为 `historyListView.currentIndex` 绑定 `selectedHistoryIndex` 且使用 `ApplyRange`，Qt 会在点选时自动把当前项拉入高亮范围，视觉上像从列表开头滑到选中项；现在改为 `NoHighlightRange` 并只用 `selectedHistoryIndex` 控制卡片颜色。 |
| 2026-05-16 | 新增统计分析界面 | 左侧 `统计分析` 导航正式可点击；统计页读取 `/mnt/sdcard/images/upload_history.json` 对应的 `uploadHistory` 模型，展示 KPI、最近保存趋势、结果/上传分布、云端与文件状态、最近记录表；点击最近记录行复用历史详情页查看图片和云端摘要。 |
| 2026-05-16 | 统计页最近记录改为卡片内竖向滑动 | `statsRecentRows()` 保留全部历史记录并按最新在前排序；`statsRecentPanel` 内部使用 `statsRecentListView` 竖向滑动，记录多时无需跳转即可查看更多。 |
| 2026-05-20 | 修复统计分布概览越界 | `statsDistributionBars()` 保留良品、坏品、待复核、上传成功、上传失败五项；页面改用 `statsDistributionLeftBars()` 和 `statsDistributionRightBars()` 分成左右两列显示，避免“上传失败”在 1024x600 LCD 的分布面板底部超出边框。 |
| 2026-05-16 | 新增手动控制界面 | 左侧 `手动控制` 导航正式可点击；页面提供传送带、运动机构预留、人工复核、安全状态和命令日志；初版只更新 QML 状态和安全提示。 |
| 2026-07-01 | 传送带接入 F407 真实 BELT 命令 | `DeviceHealthController` 新增 `sendF4BeltCommand()` 入口，手动页 `巡航启动/停止/查询状态` 后续已在 2026-07-02 改为二进制 `BELT_MANUAL_CONTROL/QUERY_STATUS`；MP157 不直接拼张大头 Emm42 帧，相机上下轴和前后轴在 F407 未定义固定二进制服务前不在界面显示。 |
| 2026-07-02 | F4 主链路统一二进制返回 | MP157 到 F407 的 `/dev/ttySTM2` 主链路不再发送 `STATUS/CAL/BELT...` 文本；心跳走 `HEARTBEAT`，手动传送带走 `BELT_MANUAL_CONTROL/QUERY_STATUS`，正确返回 `ACK/STATUS_REPORT`，错误返回 `NACK/FAULT_REPORT`。 |
| 2026-07-02 | 恢复顶部位置并删除未接运动轴显示 | 顶部状态栏保留 `位置`，但内容改为开机单次高德 IP 省份定位结果，例如 `河南省`；Qt 后续健康刷新不再周期调用定位脚本；手动页、参数页和告警健康矩阵删除相机运动轴待接提示，不再显示 `待F4协议/等待CAM协议` 这类占位状态；后续接入前只在文档待办中记录 F407 协议要求。 |
| 2026-05-16 | 新增参数设置界面 | 左侧 `参数设置` 导航正式可点击；当时版本用于整理页面入口和参数摘要，后续已在 2026-07-01 改为真实检测配置链路；当前状态以本表 2026-07-01 的 `参数设置页接入真实检测配置` 记录为准，运动参数仍不由 Qt 直接下发。 |
| 2026-05-20 | 参数设置页真实零件收敛 | `settingsSupportedPartTypes` 只保留波形垫圈、平垫圈、弹性垫圈；参数页删除旧演示零件名和容易误导的运动/分拣可调项，保留模型阈值、复核阈值、相机、SD 卡和 COS 策略。 |
| 2026-07-01 | 参数设置页接入真实检测配置 | 新增 `DetectSettingsController` 读取/保存 `/mnt/sdcard/config/defect_ui_config.json`；参数页通过 `detectSettings` 调整模型阈值、复核阈值、ROI、UNet 最小缺陷像素、overlay 透明度和自动上传；分类命令新增 `--bad-threshold`，分割命令新增 `--min-defect-pixels`；自动上传关闭时检测结果返回 `upload_status=SKIP` 并保留本地历史。 |
| 2026-07-02 | 参数设置日志与未接硬件项清理 | `保存配置` 和 `导出摘要` 调用 `CameraStorageController::recordSettingsSummaryToSdCard()` 追加 `/mnt/sdcard/logs/qt_settings_YYYYMMDD.log`；日志查看页刷新后可打开全文；参数页和手动页删除现场未接入的硬件控制入口，只保留相机、存储、上传、检测辅助和 F4 边界说明。 |
| 2026-07-03 | 参数页新增步进电机参数弹窗 | `F4接入边界` 卡片新增 `步进参数` 小入口；弹窗按三页分别配置传送带电机、摄像头前后电机、摄像头上下电机的 ID 地址、最小步长、常规速度和方向；常规速度通过内置数字键盘支持 `0~5000 rpm` 任意整数。C++ 将配置保存到 `stepper_motors` JSON 数组，参数日志也记录每台电机字段；`保存并下发` 会通过 `sendF4StepperSettings()` 发送 `STEPPER_PARAM_SET 0x42`，并用 `f4StepperSettingsFinished` 显示 `ACK/NACK`。F4 回 ACK 只代表运行内存接收，不代表 F4 已烧录或 Emm42 EEPROM 已保存。 |
| 2026-05-16 | 新增告警维护界面 | 左侧 `告警维护` 导航正式可点击；页面提供当前告警 `0x0007`、设备健康矩阵、告警历史、处理建议和确认/清故障/刷新/保存诊断入口；第一版使用模拟告警和维护日志，真实告警帧后续由串口控制器接入。 |
| 2026-05-16 | 压缩参数页和告警页布局 | 针对 1024x600 板端截图中文字超出问题，缩短顶部摘要、参数说明、设备健康和处理建议文案；给设置/告警卡片增加 `clip`，修正告警历史行固定列宽，按钮网格改为按父容器计算宽度，避免中文字体差异导致右侧越界。 |
| 2026-05-16 | 调整当前告警操作区 | 根据板端拍屏反馈，当前告警卡片底部三个按钮挤占发生时间和处理状态区域；已把按钮移到卡片右侧纵向操作列，左侧只保留告警码、标题和状态字段，右侧设备健康卡片同步缩窄。 |
| 2026-05-16 | 新增开机启动动画 | `Main.qml` 顶层新增 `splashOverlay`，启动时显示工业检测自检动画：相机扫描窗口、ROI 框、缺陷框、五段启动状态和进度条；约 3.5 秒后淡出进入原主界面，用于承接内核企鹅 logo 隐藏后的空白阶段。 |
| 2026-05-16 | 修复摄像头画面抢在 Qt 启动画面前出现 | `uvc_kms_overlay` 新增 `-V 0|1` 初始可见性参数；`run_qt_kms_overlay_display.sh` 改为先隐藏启动 overlay，再启动 Qt；`Main.qml` 在 splash 完全关闭后发送 `VISIBLE 1`，保证启动动画先于实时视频层出现。 |
| 2026-05-18 | 收紧相机在线回调的启动显示门控 | `Main.qml` 新增 `bootOverlayRestoreFinished`，只有 Qt splash 完全淡出并进入首页后，`onCameraStatusChanged` 和 `switchPage("home")` 才能发送 `VISIBLE 1`；相机可以提前后台初始化，但不能早于 Qt 启动画面显示。 |
| 2026-05-20 | 移除启动脚本对 QML 调试日志的依赖 | `run_qt_kms_overlay_display.sh` 的 `wait_for_qt_boot_surface()` 改为等待 Qt 进程在缓冲期内持续存活，不再查找 `console.log` 文本；`test_qt_kms_overlay_assets.sh` 增加检查，避免正式版本为了启动判断保留 QML 控制台调试输出。 |
| 2026-05-16 | 增加早期显示静默兜底 | 新增 `S05display-quiet`，部署脚本同步安装到 rootfs `/etc/init.d/`，用于尽早关闭 fbcon 光标和清理 `/dev/tty0` 残留；彻底消除最早期 `_` 仍建议在内核 bootargs 增加 `vt.global_cursor_default=0`。 |
| 2026-05-16 | 增加早期静态首帧 | 新增 `fb_boot_splash.c` 和 `build_fb_boot_splash.sh`，部署到 `/root/qt_camera_display/fb_boot_splash`；`S05display-quiet`、`S90uvc-camera` 和 `run_qt_kms_overlay_display.sh` 会在 Qt 启动前调用它，先显示启动动画第一帧风格的静态图。 |
| 2026-05-16 | 修复告警保存诊断只显示路径但不生成文件 | 根因为 `handleAlarmAction("snapshot")` 只把 `快照目标：/mnt/sdcard/logs/qt_alarm_snapshot.txt` 写到界面提示，没有调用任何文件写入逻辑；现在 QML 通过 `alarmSnapshotText()` 组装诊断文本，C++ `saveAlarmSnapshotToSdCard()` 检查 `/mnt/sdcard` 挂载、创建 `/mnt/sdcard/logs`、覆盖写入 `qt_alarm_snapshot.txt` 并 `fsync`。 |
| 2026-05-20 | 告警维护接入真实状态和时间戳日志 | QML 监听相机/KMS、SD 卡、4G、云端、F4、保存/上传和模型检测状态，首次出现新问题时更新当前告警、插入告警历史，并调用 `recordAlarmIssueToSdCard()` 创建独立 `qt_alarm_YYYYMMDD_HHMMSS_zzz_<source>.log`；每次点击 `保存诊断` 或执行 `--alarm-snapshot-self-test` 都创建新的 `qt_alarm_snapshot_YYYYMMDD_HHMMSS_zzz.txt`，不再覆盖固定 `qt_alarm_snapshot.txt`。C++ 落盘路径保留 `/proc/mounts` 挂载检查、目录创建、可写检查、UTF-8 写入、`flush` 和 `fsync`，失败时返回明确中文原因。 |
| 2026-05-17 | 首页检测按钮接入 MobileNetV3-Small INT8 | 新增独立 `defect-classify` 推理程序，Qt 点击“检测”后通过 `SAVE_DETECT` 保存当前帧 JPG，再调用 ONNX Runtime 推理并把 GOOD/BAD、类别、置信度和耗时显示到首页结果面板。 |
| 2026-05-17 | 增加屏幕中心 ROI 观察框 | `uvc_kms_overlay` 在 KMS overlay 显示 framebuffer 上绘制绿色中心 `300x300` ROI 框，方便测试时把零件摆到模型实际检测区域；保存和检测图片从当前原始 YUYV 缓冲生成，不把绿色框送入模型。 |
| 2026-05-17 | 修复 ROI 框闪烁和预览卡顿 | 根因为上一版每帧先额外缓存整张 RGB24，再在整帧转换后补画 ROI 框，增加内存带宽且单 framebuffer 扫描时会看到框线被覆盖/重画；现在改为每行 YUYV 转换完成后立即覆盖该行 ROI 像素，常态预览不再每帧生成 `clean_rgb24`。 |
| 2026-05-18 | 扩展云端检测结果图上传参数 | `defect-cos-upload` 新增可重复 `--annotated <jpg/png>`，同一条记录中所有检测结果图统一登记为 `file_kind=annotated`，并按扩展名自动选择 `image/jpeg` 或 `image/png`；旧 `--png` 仍兼容，便于旧保存自检复用原调用。 |
| 2026-05-16 | 修复保存图片期间界面无法点击其它页面 | 根因为 QML 直接同步调用 `saveCurrentFrameToSdCard()`，本地保存、COS 上传和历史记录处理会占住 Qt 主线程；现在 QML 调用 `requestSaveCurrentFrameToSdCard()` 后立即返回，C++ 后台线程完成保存和上传，结束后通过 `saveCurrentFrameFinished` 回填提示并在主线程追加历史记录。 |
| 2026-05-18 | 接入 UNet INT8 分割模型并替代独立保存按钮 | 新增 `defect-segment` 和 `--detect-self-test`；点击 `检测` 时先保存 source JPG 并运行 MobileNetV3-Small 分类，再运行 UNet 分割输出 raw/overlay/mask 三张 annotated 图，最后把 source、annotated、分类 RESULT、UNet RESULT_SEG 和云端记录号合并为一条历史记录；首页不再暴露 `保存图片` 按钮。 |
| 2026-05-19 | 双模型综合判定 | 新增 `fused_status/fused_result/fused_reason` 综合字段；分类模型或 UNet 任一发现缺陷时最终云端结果写 `bad`，本地历史写 `待复核`，避免分类 `GOOD` 但 UNet 检出划痕时误判为良品；历史失败重发也复用 `classification_result + segmentation_result` 恢复综合云端结果。 |
| 2026-05-19 | 收紧首页和历史详情显示 | 首页结果栏新增 `compactHomeClassText()` 和 `compactHomeModelText()`，把完整类别名和综合判定长句压缩成短类别、短模型结果，避免窄面板显示不全；历史详情检测信息新增 `compactHistoryInfoLine()`，去掉空行和多余空白，并用紧凑行距显示检测结论、可信度、缺陷提示和图片留档四条说明。 |
| 2026-05-20 | 修正垫圈类零件显示 | 首页和上传脚本统一把历史训练编码 `gasket` 显示为 `波形垫圈`，不再显示成 `垫片`；`washer` 显示为 `平垫圈`，`splitwasher` 显示为 `弹性垫圈`，三者大类统一为 `垫圈类`。 |
| 2026-05-20 | 历史详情长文改为可读弹层 | 历史详情右侧固定面板只显示短摘要和 `查看完整说明` 按钮；完整云端说明、云端修正、UNet 提示和模型原始输出放入可滚动弹层，避免云端长文本在 1024x600 面板内被截断。 |
| 2026-05-20 | 关闭 QML 资源压缩用于部署验收 | `qml.qrc` 对 `Main.qml` 和 `GstVideoSurface.qml` 增加 `compress="0"`，让 `historyAnalysisDetailOverlay`、`波形垫圈`、`查看完整说明` 等界面 marker 能在 `build-mp157/qt_camera_display` 和板端 `/root/qt_camera_display/qt_camera_display` 中通过 `strings` 直接检出，避免误把旧 QML 二进制部署到板端。 |
| 2026-05-18 | 修正检测结果和首页显示契约 | Qt 检测链路根据分类 `GOOD/BAD` 显式传 `CLOUD_RESULT=good/bad`，上传脚本默认改为保守 `review` 并校验结果值；首页零件栏改为模型类别前缀，置信度改为百分制，耗时改为分类+UNet 都完成后的 `total_time_ms`；KMS 右侧按钮重新排布，避免 `开始/暂停/继续/停止` 与 `检测/安全卸载` 重叠。 |
| 2026-05-18 | 拆分检测显示时机 | `main.cpp` 新增 `detectClassificationReady` 和 `detectModelsReady` 阶段信号；QML 在第一个分类模型完成后立即显示零件类型、类别、分类初判和置信度，在两个模型完成后立即显示综合判定和 `total_time_ms`，不再等待 COS 上传完成才刷新首页检测结果。 |
| 2026-05-18 | 精简历史详情字段 | 历史详情右侧指标只保留上传时间、记录ID、图片数量和云端编号；删除分类图大小、检测图总量和流程状态展示；检测信息区不再直接显示 `classification_result`/`segmentation_result` 原始行，而是显示检测结论、可信度和缺陷提示，便于普通操作员复核。 |
| 2026-05-18 | 历史图片标签去技术化 | 历史详情左侧第一张图的标签由“分类原图”改为“原始图片”，当时右侧路径标题改为“本地图片位置”，后续在 2026-05-19 已取消单独路径展示并合并为图片留档说明。 |
| 2026-05-19 | 优化历史列表入口和上传失败重发 | 点击左侧 `历史记录` 保持在检测历史列表，并自动选中、滚动到最新卡片；上传失败记录详情显示 `重新发送`，后台复用本地 source/annotated 图片重新调用 `defect-cos-upload`；成功后同一条本地历史的 `upload_time` 改为重发完成时间，记录移动到数组末尾作为最新记录，并更新云端状态；失败时保留原时间和原位置；右侧不再单独展示本地图片路径，图片留档说明合并到检测信息区。 |
| 2026-05-19 | 上传记录携带零件类型 | `main.cpp` 从分类 `class=` 提取零件类型并剥离 `_good/_bad` 后缀，通过 `CLOUD_PART_CODE` 传给 `defect-cos-upload`；脚本按云端 `/api/v1/parts?limit=100` 将零件类型映射成 `part_id`，并把 `device_context.part_code/class_label` 写入检测记录。良品和坏品只影响 `CLOUD_RESULT`，不影响零件类型；只有 gasket、washer、splitwasher 这类不同零件才应在云端创建不同零件类型。 |
| 2026-05-20 | 支持云端自动创建零件 | `defect-cos-upload` 保留旧的 `CLOUD_PART_ID` 直传路径；当云端零件列表没有匹配 `CLOUD_PART_CODE` 时，不再提示必须先手工建零件，而是在 `POST /api/v1/records` 中发送 `part_code/part_name/part_category/auto_create_part=true`，由云端按编码创建或复用零件。`gasket/wave_washer` 固定映射为 `波形垫圈 / 垫圈类`，避免把波形垫圈误写成“电平”或“垫片”。 |
| 2026-05-20 | 正式化云端复核回写反向隧道 | 新增板端 `board-review-tunnel.sh` 和 `S91board-review-tunnel`，由板端开机后主动建立并守护 `ssh -R 127.0.0.1:18081:127.0.0.1:18080`；新增云端 `check_board_review_tunnel.sh`、systemd service 和 timer，每 60 秒检查隧道是否可用并写日志。最终现场运行不依赖 Windows 或虚拟机，断线重连由板端守护负责，云端只负责后端常驻和状态检测。 |
| 2026-05-18 | 顶部和健康矩阵改为真实设备状态 | 新增 `DeviceHealthController`，4G 必须 `4g-ppp test` 返回 0 才显示在线；KMS 相机必须 overlay `STATUS` 返回 `has_frame=1`、`serial>0` 且 `serial` 相对上一轮有变化才显示在线；F4 健康状态后续已在 2026-07-02 改为发送二进制 `HEARTBEAT` 并等待匹配 `ACK`；云端必须 `curl -fsS --max-time 2 http://139.9.35.72/health` 成功才显示已连接；4G/云端启动失败通过 Qt 异步错误信号处理，不在刷新路径等待进程启动；健康刷新周期放慢到 8 秒，并保留上一轮稳定状态，避免网络/云端在“检测中”和“在线/已连接”之间循环闪烁；所有耗时检测都用异步进程或后台线程，QML 主线程不等待。 |
| 2026-06-08 | 迁移云服务器默认地址 | 默认云端从 `http://119.91.65.122` 改为 `http://139.9.35.72`，同步 Qt health、COS 上传、反向隧道和 4G HTTP Date 兜底；账号和 API 契约不变。 |
| 2026-05-18 | 支持 USB 摄像头热拔插恢复 | `uvc_kms_overlay` 新增 `STATUS` 命令，Qt 连续检测相机离线后后台调用 `run_qt_kms_overlay_display.sh restart-overlay`，只重启 overlay 视频进程，不重启 Qt 界面；摄像头重新插入并恢复出帧后界面状态自动回到在线。 |
| 2026-05-19 | 新增 AI 品牌启动图 HTML 预览稿 | `ai_boot_splash_preview.html` 以 1024x600 画布重绘 `第九届嵌入式芯片与系统设计竞赛`、`9th AI赋能设计，设计点亮AI! / AI for Design & Design for AI!` 标识，并加入电路、芯片轮廓、机器视觉 ROI 和底部技术铭牌，作为迁移到早期 framebuffer 静态图的视觉基准。 |
| 2026-05-19 | 迁移 AI 品牌启动图到真实早期静态首帧 | `fb_boot_splash.c` 按 HTML 预览稿重绘深蓝科技背景、赛事条、白色 Logo 面板、`9TH` 芯片、AI 设计标语、弱化 ROI 线稿和三枚技术铭牌；去掉旧版 `LOADING CAMERA`、`18%` 和 `BOOT SELF CHECK` 进度条语义；`test_qt_kms_overlay_assets.sh` 同步检查新主题文本并禁止旧进度条文案。 |
| 2026-05-19 | 改为 HTML 渲染资源直写 framebuffer | 新增 `generate_boot_splash_asset.py`、`boot_splash.png` 和 `boot_splash.rgb565`；`fb_boot_splash.c` 优先读取 RGB565 raw 并整张写入 `/dev/fb0`，让板端首帧和 HTML 预览一致；C 几何绘制仅作为资源缺失时的 fallback；部署脚本同步安装 `boot_splash.rgb565`。 |

## 硬件资源

| 资源 | 用途 | 注意事项 |
|---|---|---|
| RGB LCD 1024x600 | Qt Quick 主界面显示 | QML 根对象固定按 1024x600 设计。 |
| UVC 摄像头 `/dev/video0` | 摄像头画面输入 | KMS overlay 正式路线由 `uvc_kms_overlay` 独占该节点，Qt UI 只显示状态。 |
| `/dev/galcore` | Qt eglfs/OpenGL ES GPU 渲染 | 启动前必须 `modprobe galcore` 成功。 |
| DRM overlay plane 36 | 正式视频显示平面 | 默认矩形为 `177,73,640,480`，由运行脚本环境变量可调。 |
| Goodix 触摸 `/dev/input/eventX` | Qt 按钮触摸输入 | 运行脚本会自动选择触摸节点并配置 `evdevtouch`。 |
| 4G PPP 管理命令 `4g-ppp` | 顶部网络真实状态 | Qt 周期调用 `4g-ppp test`；只有退出码为 0 才显示“在线”，失败、超时或命令不存在都不能显示在线。 |
| SD 卡 `/mnt/sdcard` | 保存检测图片和安全卸载 | 检测流程只允许写入 `/mnt/sdcard/images`；安全卸载必须走 `sdcard-safe-remove`。 |
| Unix socket `/tmp/uvc-kms-overlay-control.sock` | Qt UI 请求 overlay 保存当前检测帧 | 由 `uvc_kms_overlay` 创建，`run_qt_kms_overlay_display.sh status` 会显示路径；检测流程发送 `SAVE_DETECT /mnt/sdcard/images`。 |
| F4 串口 `/dev/ttySTM2` | F4 控制器真实接入判断、称重标定、传送带和摄像头运动轴高层控制 | Qt 后台线程按 115200 8N1 raw 模式发送 `A5 5A ... 6B` 二进制帧；周期心跳发送 `HEARTBEAT`，人工刷新不节流；参数设置页称重标定弹窗发送二进制 `WEIGHT_CALIBRATE 0x30`，手动页传送带按钮发送 `BELT_MANUAL_CONTROL` 或 `QUERY_STATUS`，三轴手动弹窗中传送带/前后轴发送 `ACTUATOR_VEL_MOVE/ACTUATOR_STOP`，上下轴和自动 Z 轴流程发送 `ACTUATOR_POS_MOVE/ACTUATOR_STOP`；F407 再把传送带映射到 `UART4 PC10/PC11 addr=0x01`，把摄像头前后轴映射到 `USART6 PC6/PC7 addr=0x03`，把摄像头上下轴映射到 `USART6 PC6/PC7 addr=0x02`；正确只认 `ACK/STATUS_REPORT`，错误只认 `NACK/FAULT_REPORT`。 |
| 云端 health `http://139.9.35.72/health` | 云端连接状态 | Qt 异步执行 `curl -fsS --max-time 2`；请求成功才显示“已连接”，失败或超时不显示已连接。 |
| 云端复核反向隧道 `127.0.0.1:18081 -> 127.0.0.1:18080` | 云端按钮回写板端本地历史 | 最终运行时由板端 `/etc/init.d/S91board-review-tunnel` 开机启动并守护，不依赖 Windows 或虚拟机；板端使用 `/root/.ssh/id_ed25519_yunfuwu_tunnel` 主动连接云端，云端只通过 systemd timer 周期检查 `127.0.0.1:18081` 是否可达。 |
| 检测历史文件 `/mnt/sdcard/images/upload_history_YYYYMMDD.json` | Qt 历史记录页数据源 | 每次检测/上传完成后由 `UploadHistoryModel` 追加写入当天文件；启动时汇总读取多天 `upload_history_*.json`，并兼容旧 `/mnt/sdcard/images/upload_history.json`；记录包含 `source_path`、`annotated_images`、`classification_result`、`segmentation_result`、`record_id`、`record_no`、`upload_status` 和文件大小，同时保留旧 `jpg_path/png_path` 兼容字段。 |
| 统计分析页面 | 汇总本地检测历史 | 数据来自 `uploadHistory` 模型，不新增单独数据库；统计页打开时通过 `setOverlayVisible(false)` 隐藏 KMS 视频 plane，返回首页时恢复。 |
| 手动控制页面 | 调试级人工操作界面 | 传送带状态来自 F407 二进制 `ACK/STATUS_REPORT` 和 `manualCommandLog`；手动页打开时通过 `setOverlayVisible(false)` 隐藏 KMS 视频 plane；三轴弹窗通过 `ACTUATOR_VEL_MOVE/ACTUATOR_POS_MOVE/ACTUATOR_STOP` 高层命令控制三台电机，不在 MP157 直接发送 Emm42 帧；相机上下轴、前后轴、限位和联锁仍由 STM32F4 实际执行和返回错误码。 |
| 启动动画覆盖层 | 替代启动空窗和展示系统自检 | 数据来自 QML 本地 `splashStageModel`，不是硬件真实自检结果；动画使用 Qt Quick 基础图元和 transform/opacity/y 动画，不加载外部图片或视频资源，避免拖慢板端启动。 |
| F4 执行器命令桥 | MP157 到 STM32F4 的高层运动命令桥 | `DeviceHealthController::sendF4BeltCommand()` 继续负责 `BELT_MANUAL_CONTROL/QUERY_STATUS`；`sendF4ActuatorVelocityMove()` 负责手动传送带/前后轴连续速度运动，底层发送 `ACTUATOR_VEL_MOVE 0x52`；`sendF4ActuatorPositionMove()` 负责自动流程和手动上下轴固定步数位置运动，底层发送 `ACTUATOR_POS_MOVE 0x50`；`sendF4ActuatorStop()` 负责单轴或全部停止，底层发送 `ACTUATOR_STOP 0x51`；`sendF4ActuatorHome()` 负责参数页把当前电机页当前位置设为零点，底层发送 `ACTUATOR_HOME 0x53`。F407 再转成 Emm42 速度模式、位置模式、停止或当前位置清零命令。 |
| 云端后端 `http://139.9.35.72` | 检测记录创建、COS 预签名上传和文件登记 | `defect-cos-upload` 默认使用该地址，可通过 `CLOUD_BASE_URL` 覆盖。 |
| COS 上传脚本 `/root/qt_camera_display/defect-cos-upload` | 上传 source/annotated 检测图到 COS | 需要 curl、后端 Cookie，或本地 `/root/qt_camera_display/cos-upload.env` 中的 `CLOUD_ACCOUNT/CLOUD_PASSWORD`；`--jpg` 登记为 `source`，重复 `--annotated <jpg/png>` 登记为 `annotated`，脚本按扩展名设置 `image/jpeg` 或 `image/png`；`CLOUD_DEVICE_ID`、`CLOUD_PART_ID` 可显式覆盖；未显式设置 `CLOUD_PART_ID` 时优先用 `CLOUD_PART_CODE` 或 `CLOUD_CLASS_LABEL` 去掉 `_good/_bad` 后缀后的零件类型查询云端 `part_id`，若云端没有该零件，则创建记录时发送 `part_code/part_name/part_category/auto_create_part=true` 自动创建；没有传零件类型时才使用第一条零件兜底；`captured_at/uploaded_at` 显式使用 `TZ=CST-8` 的北京时间墙钟并追加 `+08:00`，不得在前后端额外手工加减 8 小时。 |
| 默认上传账号配置 `/root/qt_camera_display/cos-upload.env` | 检测按钮自动登录云端 | 只保存在板端/rootfs 本地，不提交 Git；权限必须为 `600`；可通过 `CLOUD_UPLOAD_ENV_FILE` 指定其他板端绝对路径，运行时环境变量优先于该文件。 |
| Qt 进程时区 `TZ=CST-8` | 顶部状态栏显示北京时间 | `main.cpp` 在 Qt 应用创建前设置默认 `TZ` 并调用 `tzset()`；启动脚本也导出 `TZ=CST-8`。如果上传时间正确但界面时间仍是 UTC，优先检查 `/proc/$(pidof qt_camera_display)/environ` 中是否有 `TZ=CST-8`，不要去改 `defect-cos-upload`。 |

## 手动控制和 Z 轴流程联调阶段待办

当前 MP157 Qt 和 STM32F407 工程代码已经接入 `ACTUATOR_POS_MOVE/ACTUATOR_STOP/ACTUATOR_VEL_MOVE/ACTUATOR_HOME`，界面也已经开放传送带、摄像头前后轴、摄像头上下轴三页手动控制和参数页“设当前位置为零点”。代码已写不等于板端或 F4 已生效，仍需要同步到虚拟机交叉编译部署 Qt，并编译烧录 F407 固件后做实机验证。

| 优先级 | 待加功能 | 联调触发条件 | 实现要点 |
|---|---|---|---|
| 高 | 手动速度模式和上下轴固定步数确认 | F407 烧录包含 `ACTUATOR_POS_MOVE/ACTUATOR_STOP/ACTUATOR_VEL_MOVE` 的固件后 | 验证传送带和前后轴 `ACTUATOR_VEL_MOVE` 按一次持续运动、停止键结束；验证上下轴下降/上升分别执行 `zDownFixedSteps/zUpFixedSteps` 固定步数；确认所有成功 ACK 都是 `status=0`，`actuator=0xFF` 能停止全部可停止执行器。 |
| 高 | 参数页当前位置设零确认 | F407 烧录包含 `ACTUATOR_HOME` 和 Emm42 `[addr 0A 6D 6B]` 清零命令的固件后 | 在参数页切换传送带、前后轴、上下轴，分别点击 `设当前位置为零点`；F4 应返回 `ACK acked_cmd=0x53 status=0`，目标电机不主动运动，只把当前位置作为新的零点。 |
| 高 | Z 轴下探/回升固定值标定 | 上下轴实际带载并能稳定移动后 | 在参数页输入 `zDownFixedSteps/zUpFixedSteps`，先用小步数验证方向，再逐步增加到模型检测所需高度；记录不会撞限位的安全范围。 |
| 高 | 相机轴限位和回零状态 | 上下/前后轴安装限位或零位传感器后 | F407 负责限位去抖、回零过程、越界拒绝和错误码；QML 只显示 F4 返回的状态，不用本地变量假定已回零。 |
| 中 | 复位到安全状态 | F4 已支持停止传送带、停止相机运动轴和清联锁状态时 | 一键下发 F4 二进制高层安全复位命令，完成后通过 `QUERY_STATUS/STATUS_REPORT` 刷新传送带和相机轴状态；失败时保留 `NACK/FAULT_REPORT` 错误码并禁止继续动作。 |
| 中 | 手动动作二次确认 | 清故障、急停释放、放坏品、模式切换等高风险动作接入真实硬件前 | 用弹窗或确认状态阻止误触；确认后才调用真实命令，取消时只写命令日志。 |
| 中 | 当前帧冻结、双模型检测和人工复核联动 | 检测记录、人工复核结果和图片保存链路需要合并时 | 手动页的 `GOOD/BAD/UNCERTAIN` 后续写入检测记录，检测图片上传时同时带上复核标记和本次动作上下文。 |
| 低 | 运动参数持久化到 F4 | 需要 F4 上电后脱离 MP157 仍保留步进参数时 | 当前参数页保存 MP157 JSON，并可通过 `STEPPER_PARAM_SET` 下发 F4 运行内存；若要写 F4 Flash 或 Emm42 EEPROM，需要另行定义维护命令和写入保护。 |

## 验证证据

| 验证项 | 已记录结果 |
|---|---|
| Qt 安全预览低 CPU | `320x240@10fps` 约 `5.9% CPU`，画质不足。 |
| Qt 安全预览高画质 | `640x480@15fps` 约 `42.0% CPU`，接近旧 framebuffer。 |
| qmlglsink mmap 桥接 | `VIDEO_BACKEND=qt-gst` 可显示 Qt UI + 视频，`640x480@15fps` 约 `36.2% CPU`。 |
| qmlglsink DMABUF | `GST_IO_MODE=dmabuf VIDEO_BACKEND=qt-gst` 用户态 rc=`139`，core 指向 Vivante `libGAL.so:gcoTEXTURE_GetMipMap()`。 |
| standalone GL 路线 | `v4l2src io-mode=dmabuf ! glupload ! glimagesink` 长测约 `2.0% CPU`，但不显示 Qt UI。 |
| KMS overlay 正式路线 | `640x480@10fps` 两次 10 秒样本 overlay `13.5%/13.6%`，Qt `2.3%/2.5%`，用户确认画面融合正常。 |
| 触摸按钮 | 用户在开发板屏幕上点击 `开始/暂停/继续/停止`，确认状态栏文字变化。 |
| SD 卡按钮 | 2026-05-03 人工点击 `保存图片` 未通过验收；2026-05-04 定位到 Qt 侧 procfs 挂载判断误判 `/mnt/sdcard 未挂载`，已改为 POSIX 解析 `/proc/mounts`；板端 `--storage-self-test` 已生成 `/mnt/sdcard/images/uvc_20260504_113814_000263.ppm`，屏幕按钮触发 Qt 主进程日志并生成 `uvc_20260504_113846_000592.ppm`、`uvc_20260504_113946_001192.ppm`，文件头 `P6` 且大小稳定。 |
| JPG/PNG + COS 上传 | 2026-05-08 板端验证通过。`record_id=9` 证明只上传本次 `source/annotated` 两张图；随后修复时间二次换算后，`record_id=10` 返回 `captured_at=2026-05-08T13:12:51`、`uploaded_at=2026-05-08T13:13:16`，详情文件数仍为 2。 |
| 双模型检测 + COS 上传 | 2026-05-18 板端 `./qt_camera_display --detect-self-test` 验证通过。输出 `RESULT status=GOOD class=gasket_good ... segment_status=OK defect_pixels=0 ... upload_status=OK`；本地历史新增 `record_id=48 / MP157-20260518-145002`，包含 `source_path`、3 张 `annotated_images`、`classification_result` 和 `segmentation_result`；云端详情同一条记录包含 1 张 `source` 和 3 张 `annotated` 文件，其中 JPEG 为 `image/jpeg`，mask PNG 为 `image/png`。后续坏品验证必须确认 `RESULT status=BAD` 或 `segment_status=NG/defect_pixels>0` 时输出含 `fused_result=bad`，云端记录 `result` 也为 `bad`，不能再显示成良品。 |

## 待修问题记录

| 日期 | 问题 | 当前证据 | 预期目标 | 下一步优先排查 |
|---|---|---|---|---|
| 2026-05-07 | 单次保存上传的云端记录隔离失败 | 板端 `--storage-self-test` 创建了新记录 `record_id=7/8`，但后续日志显示文件登记仍打到 `/api/v1/records/3/files`，且 COS `object_key` 仍包含旧记录名 `SIM-DIANPIAN-20260420185013`。根因是脚本用通用 JSON `id` 解析函数从创建记录响应里抓到了后面的嵌套 `part.id/device.id=3`。 | 按一次 `保存图片` 后，云端应只关联本次两张图片：JPG 原图 `source` + PNG 标注图 `annotated`；prepare、register、detail 回查都必须使用创建记录返回的新 `record_id`。 | 已在脚本侧修复并新增 `--self-test-json-parser` 回归自检；明天板端验证时重点看 stderr 中 `record_no=...`、`created_record_id=...`、`prepare_object_key=...`、`register_url=/api/v1/records/<新ID>/files`，并确认新记录详情只含 2 张图，旧 `record_id=3` 不再增加。 |

## 新版测试与验证矩阵

| Test goal | Run location | Command | Expected result | Failure triage |
|---|---|---|---|---|
| 静态契约检查 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `./test_qt_kms_overlay_assets.sh` | 输出 `PASS: Qt KMS overlay assets contract`；脚本同时检查历史页默认聚焦最新列表卡片、失败记录重发入口、重发成功后本地时间刷新并移动到末尾，以及不再单独显示本地图片路径。 | 若失败，按脚本提示检查 QML 按钮、overlay 源码、运行脚本和默认参数。 |
| 上传 JSON 解析回归 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `sh defect-cos-upload --self-test-json-parser` | 构造顶层 `record_id=8`、嵌套 `part.id/device.id=3` 的响应，解析结果仍为 `8`。 | 若失败，说明创建记录响应解析又可能把旧嵌套 ID 当成 record_id。 |
| 上传时间格式回归 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `sh defect-cos-upload --self-test-json-parser` | 同时构造固定墙钟 `2026-05-08T13:07:02`，输出应保持 `2026-05-08T13:07:02+08:00`，不能变成 `21:07:02+08:00`。 | 若失败，说明脚本又对板端墙钟做了时区二次换算。 |
| 上传零件类型回归 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `sh defect-cos-upload --self-test-json-parser` | 同时构造云端零件列表，`gasket_good` 和 `gasket_bad` 必须映射到同一个 `gasket` 的 `part_id`，`washer_good` 才映射到另一个零件类型；当 `wave_washer` 未匹配到 `part_id` 时，请求字段必须包含 `part_code=wave_washer`、`part_name=波形垫圈`、`part_category=垫圈类`、`auto_create_part=true`。 | 若失败，说明脚本把好坏结果误当成零件类型，或未知零件仍没有走云端自动创建路径。 |
| 上传参数解析回归 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `sh defect-cos-upload --self-test-args` | `--annotated` 能保留多张结果图，`.jpg/.jpeg` 识别为 `image/jpeg`，`.png` 识别为 `image/png`，多张 annotated 的响应头路径互不覆盖。 | 若失败，说明检测结果图上传参数或动态 content-type 逻辑回退。 |
| 上传结果和零件字段回归 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `grep -n "CLOUD_RESULT\\|CLOUD_PART_CODE" main.cpp defect-cos-upload; grep -n "validate_cloud_result\\|class_label_to_part_code" defect-cos-upload` | Qt 检测上传前设置 `CLOUD_RESULT` 和 `CLOUD_PART_CODE`；脚本校验结果枚举，并把 `gasket_good/gasket_bad` 归一成同一个零件类型。 | 若缺少这些标记，坏品检测可能再次被云端登记成良品，或不同好坏样本被云端当成不同零件类型。 |
| Qt 程序编译 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `./build_qt_camera_display.sh` | 生成 `build-mp157/qt_camera_display`。 | 若 qmake 缺失，确认脚本已 source ST Qt/Wayland SDK 环境。 |
| QML 二进制 marker 验证 | 虚拟机和开发板 SSH | 虚拟机执行 `strings build-mp157/qt_camera_display \| grep -E 'historyAnalysisDetailOverlay|historyPartNameText|波形垫圈|查看完整说明'`；部署后开发板执行 `strings /root/qt_camera_display/qt_camera_display \| grep -E 'historyAnalysisDetailOverlay|historyPartNameText|波形垫圈|查看完整说明'` | 两端都能检出完整说明弹层、历史零件名函数和垫圈类中文名，证明本次 QML 已打进二进制并实际部署到板端。 | 若虚拟机检不出，先检查 `qml.qrc` 是否保留 `compress="0"`、`rcc -name qml` 是否重新运行；若板端检不出，先停止 Qt 进程后重新部署，避免旧二进制仍在运行。 |
| overlay 工具编译 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `./build_uvc_kms_overlay.sh` | 生成 `build-mp157/uvc_kms_overlay`，并且在已 `source` ST Qt SDK 的 shell 中也能继续使用 Buildroot 的 overlay 编译器。 | 若 libdrm、libjpeg 或 libpng 缺失，查 Buildroot defconfig 和 sysroot/runtime 部署；若需要自定义编译器，只设置 `OVERLAY_CC`，不要依赖外部 `CC`。 |
| 部署 Qt 文件 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `sudo ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` | `/home/cfr/linux/nfs/rootfs/root/qt_camera_display/` 更新。 | 若权限失败，确认用 `sudo`，并检查 NFS rootfs 路径。 |
| 部署默认上传账号 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `CLOUD_ACCOUNT='<账号>' CLOUD_PASSWORD='<密码>' sudo -E ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs; sudo ls -l /home/cfr/linux/nfs/rootfs/root/qt_camera_display/cos-upload.env` | `ls -l` 输出 `-rw------- root root`。 | 若文件不存在，确认 `sudo -E` 保留了环境变量；若权限不是 `600`，重新部署或手工 `chmod 600`。 |
| 板端反向隧道守护 | 开发板 SSH | `/etc/init.d/S91board-review-tunnel restart; /etc/init.d/S91board-review-tunnel status; tail -n 80 /tmp/board-review-tunnel.log` | 输出 `monitor: running`、`ssh_tunnel: running` 和 `remote_forward: 127.0.0.1:18081:127.0.0.1:18080`。 | 若 monitor 未运行，查脚本权限；若 ssh 未运行，查 4G 网络、`/root/.ssh/id_ed25519_yunfuwu_tunnel` 权限、云端 authorized_keys 和 18081 是否被旧隧道占用。 |
| 云端反向隧道检查 | 云服务器 | `systemctl status yunduan-board-review-tunnel-check.timer; ss -ltnp | grep 127.0.0.1:18081; curl -i --max-time 5 http://127.0.0.1:18081/api/v1/review-result; tail -n 80 /var/log/yunduan-board-review-tunnel-check.log` | timer 为 active，`ss` 显示 `sshd` 监听，curl 返回板端 HTTP 404 JSON，日志出现 `OK 反向隧道可达`。 | 若云端未监听，等待板端守护重连；若 curl 超时，查板端 4G/SSH；若返回连接拒绝，查板端 Qt 回写服务是否监听 18080。 |
| GPU 运行前检查 | 开发板 | `modprobe galcore; ls -l /dev/galcore` | `/dev/galcore` 存在。 | 若不存在，查 galcore 模块、内核版本和 `/lib/modules`。 |
| 早期静态首帧 | 开发板 SSH | `/root/qt_camera_display/fb_boot_splash -f /dev/fb0; echo $?; ls -l /root/qt_camera_display/fb_boot_splash /root/qt_camera_display/boot_splash.rgb565 /dev/fb0` | `echo $?` 输出 `0`，LCD 显示与 HTML 预览一致的 AI 竞赛品牌静态启动图；`boot_splash.rgb565` 大小为 `1228800` 字节，程序和 `/dev/fb0` 都存在。 | 若命令失败，查二进制是否部署且可执行、`boot_splash.rgb565` 是否存在且大小正确、`/dev/fb0` 是否存在、framebuffer 位深是否为 16/24/32 bpp；若命令成功但随后黑屏，查后续启动脚本或 Qt 是否覆盖了 framebuffer。 |
| 正式路线启动 | 开发板 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart` | Qt UI 和 overlay 视频同时显示。 | 若黑屏，先运行 `status`，再查 `/tmp/qt_camera_display.log` 和 `/tmp/uvc_kms_overlay.log`。 |
| 启动动画验证 | 开发板屏幕 | 重启开发板或执行 `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart` | 先显示 `fb_boot_splash` AI 竞赛品牌静态首帧，再显示 QML `工业缺陷检测系统` 启动画面、绿色扫描线、ROI 框、五段自检文字和进度条，随后淡出到首页并恢复摄像头画面；摄像头画面不应先于 Qt splash 出现；不再显示 Ubuntu 企鹅图标。 | 若没有静态首帧，先手动运行 `/root/qt_camera_display/fb_boot_splash -f /dev/fb0`；若没有 QML 动画，确认部署的是新 Qt 二进制并检查 `strings /root/qt_camera_display/qt_camera_display | grep splashOverlayVisible`；若摄像头仍抢先出现，确认 `/tmp/uvc-kms-overlay.log` 包含 `initial-visible=0`，`run_qt_kms_overlay_display.sh` 含 `-V 0`；若仍有企鹅，确认内核 `CONFIG_LOGO` 已关闭并重新加载了新的 `uImage`。 |
| 早期光标兜底 | 开发板 SSH | `cat /proc/cmdline; cat /sys/class/graphics/fbcon/cursor_blink; ls -l /etc/init.d/S05display-quiet` | `cursor_blink` 输出 `0`，`S05display-quiet` 存在且可执行；当前 bootargs 若仍无 `vt.global_cursor_default=0`，最早期内核阶段仍可能短暂出现 `_`。 | 若 `cursor_blink` 不是 `0`，手动执行 `/etc/init.d/S05display-quiet start` 并查脚本权限；若要彻底消除最早期 `_`，需要改内核/U-Boot bootargs 增加 `vt.global_cursor_default=0 quiet loglevel=3` 并重新部署 `uImage`。 |
| 正式路线状态 | 开发板 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh status` | 显示 Qt PID、overlay PID、fallback PID、plane 和日志路径。 | 如果只有 Qt 或只有 overlay，说明某一半链路退出，按对应日志排查。 |
| 真实 4G 状态 | 开发板 SSH | `4g-ppp test; echo "exit=$?"` | 命令退出码为 `0` 时，Qt 顶部网络显示 `在线`；退出码非 0、命令不存在或超过 2.5 秒时显示离线/未安装/超时。 | 若界面仍显示在线，检查是否运行新 Qt 二进制，并用 `strings /root/qt_camera_display/qt_camera_display | grep DeviceHealthController` 确认已部署。 |
| overlay STATUS | 开发板 SSH | `printf 'STATUS\n' | nc -U /tmp/uvc-kms-overlay-control.sock; sleep 3; printf 'STATUS\n' | nc -U /tmp/uvc-kms-overlay-control.sock` | 输出类似 `OK STATUS has_frame=1 serial=123 visible=1 width=640 height=480`，第二次 `serial` 应大于第一次，且首页相机状态显示 `在线`。 | 若 `nc` 不支持 `-U`，只用屏幕状态和 `/tmp/uvc_kms_overlay.log` 验证；若 `has_frame=0`、`serial=0` 或 `serial` 不变化，先查 USB 摄像头、`/dev/video0` 和 overlay 日志。 |
| USB 摄像头拔出检测 | 开发板屏幕和 SSH | 保持 Qt 首页运行，拔掉 USB 摄像头，再观察 6 秒；同时执行 `tail -n 80 /tmp/uvc_kms_overlay.log` | 顶部/健康矩阵相机状态变为离线类状态，Qt 主界面仍可切换页面和点击按钮，不应整屏卡死。 | 若界面卡住，检查 `main.cpp` 是否仍在主线程执行 socket/串口等待；若状态不变，确认部署了支持 `STATUS` 的新版 `uvc_kms_overlay`。 |
| USB 摄像头重新插入恢复 | 开发板屏幕和 SSH | 插回 USB 摄像头，必要时执行 `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart-overlay; printf 'STATUS\n' | nc -U /tmp/uvc-kms-overlay-control.sock` | overlay 只重启视频进程，Qt PID 不变；`STATUS` 恢复 `has_frame=1 serial>0` 且后续 `serial` 持续变化后，相机状态回到 `在线`；如果当前页面是首页，QML 再发送 `VISIBLE 1` 让画面重新显示。 | 若 Qt PID 变化，说明误用了 `restart` 而不是 `restart-overlay`；若 overlay 起不来，查 USB 枚举、`/dev/video0`、DRM plane 占用和 `/tmp/uvc-kms-overlay.log`。 |
| F4 串口握手状态 | 开发板屏幕和日志 | Qt 启动后等待健康检测，或在页面触发一次刷新状态；必要时查看 `/tmp/qt-kms-overlay-shell.log` 中 F4 心跳摘要 | Qt 通过 `/dev/ttySTM2` 发送二进制 `HEARTBEAT 0x02`，收到匹配 `ACK 0x80` 后顶部 F4 显示 `接入`，底部提示显示 `F4: F4 串口握手成功：ACK ...`；收到 `NACK/FAULT_REPORT`、串口失败或超时时显示 `F4: F4 待接入：...`。 | 若没有 `F4:` 提示或仍出现旧文本判断，确认板端运行的是新 Qt 二进制；若一直待接入，查 F407 是否烧录二进制协议固件、MP157 `/dev/ttySTM2` 与 F407 USART1 TX/RX 是否交叉、共地和 115200 波特率。 |
| F4 称重标定弹窗 | 开发板屏幕 | 点击左侧 `参数设置`，在 `F4接入边界` 卡片点击 `称重标定`，选择 `1000 g` 或用弹窗数字键盘按 `清空`、`7`、`5`、`0` 输入 `750`，再点击 `发二进制` | Qt 不发送 `CAL` 文本，只发送二进制 `WEIGHT_CALIBRATE 0x30`；F4 已去皮且最近一次 HX711 采样有效时，弹窗和底部提示应显示 `F4: 标定命令成功：ACK WEIGHT_CALIBRATE...`；未去皮、克重越界或 HX711 异常时显示结构化 `NACK`。 | 若弹窗仍显示 `正在发送 CAL` 或 `[OK][WEIGHT]`，说明板端还是旧 QML/旧二进制；若无回包，先按 F4 串口握手状态排查；若输入被拒绝，确认克重为 1~5000 的整数；若返回 `ERR_STATE_NOT_ALLOWED`，先用串口助手或 F4 初始化流程完成空载 TARE；若按输入框不弹键盘，直接使用弹窗内置数字键盘。 |
| F4 称重标定 SSH 对照 | 开发板 SSH | `strings /root/qt_camera_display/qt_camera_display | grep -E 'WEIGHT_CALIBRATE|标定克重必须是1~5000g整数|formatF4ToastText|发二进制'` | 板端二进制包含称重标定命令和 `F4:` 提示格式相关标记，证明 QML/C++ 已经重新编进程序。 | 该对照只证明部署版本正确，不直接向 F4 写 ASCII；若需要裸串口验证，应使用能发送二进制帧的专用工具，不能再用 `CAL 1000` 文本判断主链路。 |
| F4 传送带手动控制 | 开发板屏幕 | 点击左侧 `手动控制`，点击 `进入手动`，再点 `巡航启动`、`查询状态`、`停止` | 命令日志依次记录 `BELT_MANUAL_SCAN`、`QUERY_STATUS`、`BELT_MANUAL_STOP` 语义名；底层向 F407 下发二进制 `BELT_MANUAL_CONTROL 0x41` 或 `QUERY_STATUS 0x40`；底部提示统一显示 `F4: F4回执：ACK ...`、`F4: F4回执：STATUS_REPORT ...` 或 `F4: F4命令失败：NACK/FAULT_REPORT...`。 | 若 UI 显示 F4 命令失败，先看底部 `F4:` 后面的 NACK 错误码；再确认 F407 已创建 `ConveyorMotorService_Task()`，并检查 `UART4 PC10/PC11` 到传送带 Emm42 地址 `0x01` 的接线、共地和电源。 |
| F4 传送带 SSH 对照 | 开发板 SSH | `strings /root/qt_camera_display/qt_camera_display | grep -E 'BELT_MANUAL_SCAN|BELT_MANUAL_STOP|QUERY_STATUS|BINARY_PROTOCOL_CMD_QUERY_STATUS'` | 板端 Qt 二进制只暴露二进制协议语义名，不再包含旧 `BELTSCAN/BELTSTOP/BELTINFO` 发送路径；实际 Emm42 帧仍由 F407 `UART4 PC10/PC11` 发送到地址 `0x01` 的传送带电机。 | 若 `strings` 仍能看到旧 ASCII 发送路径，重新同步、交叉编译并替换 `/root/qt_camera_display/qt_camera_display`；若 UI 有 ACK 但电机不动，转查 F407 传送带服务、电机 ID、电源、使能、闭环/开环参数和 UART4 接线。 |
| 云端 health 状态 | 开发板 SSH | `curl -fsS --max-time 2 http://139.9.35.72/health >/tmp/cloud-health.txt; echo "exit=$?"; cat /tmp/cloud-health.txt` | curl 退出码为 `0` 时，云端显示 `已连接`；失败或超时时显示离线/超时，不再固定显示已连接。 | 若 curl 不存在，先补 rootfs 工具；若后端 health 路径改变，需要同步修改 `DEFAULT_CLOUD_HEALTH_URL` 并更新文档。 |
| SD 卡健康状态 | 开发板 SSH | `mount | grep ' /mnt/sdcard '; df -h /mnt/sdcard` | `/mnt/sdcard` 挂载时健康矩阵显示 `已挂载`，卸载后显示 `未挂载`。 | 若界面状态和命令不一致，检查 `/proc/mounts` 解析和是否运行旧二进制。 |
| Qt 顶部时钟时区 | 开发板 SSH | `qtpid=$(pidof qt_camera_display); tr '\0' '\n' < /proc/$qtpid/environ | grep '^TZ='; TZ=CST-8 date '+%H:%M:%S %Z'; date -u '+%H:%M:%S UTC'` | 进程环境显示 `TZ=CST-8`，屏幕顶部时间应与 `TZ=CST-8 date` 同小时，且比 `date -u` 快 8 小时。 | 若进程环境没有 `TZ=CST-8`，重新部署 `qt_camera_display` 和两个启动脚本；若环境正确但屏幕仍是 UTC，确认是否运行的是旧二进制或旧 QML 资源。 |
| 安全预览兜底 | 开发板 | `/root/qt_camera_display/run_qt_camera_display.sh` | 以默认 `320x240@10fps` 显示安全预览。 | 若启动失败，查 `/dev/video0`、`/dev/galcore`、Qt runtime 和 QML 模块。 |
| 高画质压力测试 | 开发板 | `CAMERA_WIDTH=640 CAMERA_HEIGHT=480 CAMERA_FPS=15 /root/qt_camera_display/run_qt_camera_display.sh` | 能显示更高画质，同时 CPU 升高。 | 若 CPU 过高，这是安全路径预期瓶颈，不要误判为内核崩溃。 |
| 触摸按钮验证 | 开发板屏幕 | 依次点击 `开始`、`暂停`、`继续`、`停止` | 状态栏文字随动作变化，视频 plane 不被按钮遮挡。 | 若触摸无效，查 Goodix event 节点、`QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS` 和 Qt 输入日志。 |
| 检测前置检查 | 开发板 | `mount | grep ' /mnt/sdcard '; df -h /mnt/sdcard; /root/qt_camera_display/run_qt_kms_overlay_display.sh status; ls -lh /root/qt_camera_display/defect-classify /root/qt_camera_display/defect-segment /root/qt_camera_display/models/defect_unet_test_decoder_head_int8.onnx` | `/mnt/sdcard` 已挂载，overlay PID 存在，状态输出包含 `/tmp/uvc-kms-overlay-control.sock`，两个推理程序和 UNet 模型存在。 | 若未挂载，执行 `/etc/init.d/S85sdcard-mount status`；若 socket 不存在，查 `/tmp/uvc-kms-overlay.log`；若模型缺失，重新部署 `DEFECT_UNET_MODEL_SRC`。 |
| 双模型 SSH 自检 | 开发板 SSH | `cd /root/qt_camera_display && ./qt_camera_display --detect-self-test` | 输出以 `RESULT ` 开头，包含 `status=GOOD|BAD`、`fused_status=GOOD|BAD|REVIEW`、`fused_result=good|bad|review`、`total_time_ms=<ms>`、`segment_status=OK|NG`、`source_path=/mnt/sdcard/images/uvc_*.jpg` 和 `upload_status=OK|FAIL|SKIP`；当自动上传关闭时仍生成本地历史但不调用 COS；当 `status=BAD` 或 `segment_status=NG/defect_pixels>=min_defect_pixels` 时云端记录应为 `bad`。 | 若保存失败，查 `/mnt/sdcard`、overlay socket 和 `SAVE_DETECT`；若分类失败，查 `defect-classify --bad-threshold`、分类模型和 labels；若 UNet 失败，查 `defect-segment --min-defect-pixels`、UNet 模型和 ONNX Runtime；若上传结果变成良品，查 Qt 是否传入综合后的 `CLOUD_RESULT`、脚本是否部署新版。 |
| 检测图片内容检查 | 开发板 SSH | `src=$(ls -t /mnt/sdcard/images/uvc_*.jpg | head -n 1); raw=$(ls -t /mnt/sdcard/images/segment_*_raw.jpg | head -n 1); overlay=$(ls -t /mnt/sdcard/images/segment_*_overlay.jpg | head -n 1); mask=$(ls -t /mnt/sdcard/images/segment_*_mask.png | head -n 1); ls -lh "$src" "$raw" "$overlay" "$mask"; head -c 2 "$src" | hexdump -C; head -c 2 "$raw" | hexdump -C; head -c 2 "$overlay" | hexdump -C; head -c 8 "$mask" | hexdump -C` | source/raw/overlay 是 JPEG SOI `ff d8`，mask 是 PNG 签名 `89 50 4e 47 0d 0a 1a 0a`，四个文件都非空。 | 若文件头不正确，说明编码失败或取错文件；若文件不存在，先查 `RESULT_SEG` 中的 `raw_path/overlay_path/mask_path`。 |
| 检测历史文件检查 | 开发板 SSH | `day=$(date +%Y%m%d); hist=/mnt/sdcard/images/upload_history_${day}.json; test -s "$hist"; tail -n 120 "$hist"` | 当天 JSON 最新记录包含 `source_path`、3 项 `annotated_images`、`classification_result`、`segmentation_result`、`upload_status`、`record_id` 和 `record_no`。 | 若文件不存在，先确认点击 `检测` 或 `--detect-self-test` 是否走到 Qt 主进程；再查 `/tmp/qt-kms-overlay-shell.log`、`upload history save failed` 和 SD 卡日期是否正确。 |
| 上传成功状态回归 | 开发板屏幕、SSH 和云端页面 | 屏幕点击 `检测`；SSH 执行 `day=$(date +%Y%m%d); tail -n 160 /mnt/sdcard/images/upload_history_${day}.json; grep -n 'verify_status\\|upload_status=OK\\|record_id' /tmp/qt-kms-overlay-shell.log /tmp/defect-cos-record-detail.json 2>/dev/null || true`；云端打开最新检测记录 | 如果云端已经出现本次记录和图片，本地历史详情应显示 `上传成功`，JSON 中 `upload_status` 不应是 `上传失败`；脚本允许 stdout 出现 `verify_status=warning`，但仍必须带 `upload_status=OK`、`record_id` 和 `record_no`。 | 若云端有记录但本地仍显示失败，确认板端二进制和 `/root/qt_camera_display/defect-cos-upload` 是否都是新版；再查 `isUploadStatusSuccess()`、`cloudStatusSummary()` 和脚本 stdout 是否被旧启动包覆盖。 |
| 历史页触摸验证 | 开发板屏幕 | 点击左侧 `历史记录`，确认停留在检测历史列表且最新卡片可见并高亮；横向滑动检测时间卡片，点击某条记录空白区域选中，再点击 `查看`，再在左侧图片区域左右滑动 | 进入历史页默认显示列表层，最新记录卡片可见；点击 `查看` 后进入详情页；详情页左侧能在原始图片、UNet raw、UNet overlay、UNet mask 之间切换，右侧只显示上传时间、记录ID、图片数量、云端编号、云端状态，以及“检测结论/可信度/缺陷提示/图片留档”四条普通中文说明，不单独显示本地路径。 | 若历史页空白，先查 `/mnt/sdcard/images/upload_history_*.json` 和旧 `/mnt/sdcard/images/upload_history.json`；若点选后列表自动滚动，检查 `Main.qml` 是否又出现 `currentIndex: root.selectedHistoryIndex` 或 `highlightRangeMode: ListView.ApplyRange`；若图片不显示，确认文件路径存在且图片头正确；若无法触摸，查 Goodix 输入配置。 |
| 全局底部提示层 | 开发板屏幕 | 依次进入 `历史记录`、`统计分析`、`参数设置`、`告警维护`、`日志查看`，分别触发检测完成、`保存配置`、`导出摘要`、`保存诊断` 或刷新动作 | 底部绿色或红色提示条显示在当前页面最上层，不被页面底部按钮、表格、卡片或日志弹窗外层内容遮挡；启动动画显示时提示层仍在 `splashOverlay` 下方。 | 若提示完全不显示，先查 `globalStorageToastLayer` 是否是 `visible: true` 常驻，而不是 `visible: storageToast.visible`；若只有首页能看到提示，其它页面看不到，再查它是否在 `Main.qml` 末尾且 `z: 900`，并确认运行的是新 QML 资源编译出的二进制。 |
| 历史失败重发验证 | 开发板屏幕和 SSH | 准备一条 `upload_status` 含 `上传失败` 的历史记录，屏幕进入该记录详情点击 `重新发送`，再执行 `day=$(date +%Y%m%d); tail -n 120 /mnt/sdcard/images/upload_history_${day}.json` | 点击后按钮短暂显示 `发送中`；成功时同一条 JSON 记录移动到数组最后，`upload_time` 变为本次重发完成时间，并写入本次重发日期文件，出现新的 `record_id`、`record_no` 和 `上传成功` 状态，界面详情同步刷新到这条最新记录；失败时该条记录保留原时间、原位置和失败状态并允许再次重发。 | 若按钮不显示，确认每日 JSON 中 `upload_status` 含 `失败`；若成功但界面不刷新，查 `retryUploadFinished` 和 `updateRecordUploadResult`；若时间仍是旧值，查 `refreshedUploadTime` 和 `m_entries.move(row, lastRow)`；若上传失败，查 4G、云端 health、账号配置、`/tmp/defect-cos-record*.json` 和脚本 stderr。 |
| 统计页触摸验证 | 开发板屏幕 | 点击左侧 `统计分析`，观察 KPI、最近检测趋势、分布概览、云端与文件状态、最近记录表；在最近记录卡片内上下滑动，再点击任一记录行 | 统计页打开后实时视频 plane 隐藏；总记录应等于所有 `upload_history_*.json` 汇总记录数；良品/待复核、上传成功率、图片数量和文件大小有值；`分布概览` 左列显示良品/坏品/待复核，右列显示上传成功/上传失败，五条都在面板边框内；最近记录表能在卡片内竖向滑动查看更多记录；点击最近记录行进入对应历史详情页。 | 若统计页无数据，先查 `/mnt/sdcard/images/upload_history_*.json` 是否存在且非空；若分布概览仍越界，检查 `statsDistributionLeftColumn`、`statsDistributionRightColumn` 和 `statsDistributionBarDelegate`；若不能滑动，检查 `statsRecentListView` 是否仍是 `ListView`；若点击无反应，检查 `openHistoryDetailFromStats` 和 `showHistoryDetail`；若视频仍覆盖统计页，查 `setOverlayVisible(pageName === "home")` 和 overlay `VISIBLE` 命令。 |
| 参数页真实零件验证 | 开发板屏幕 | 点击左侧 `参数设置`，连续点击零件 `切换` 至少 4 次 | 零件显示只在波形垫圈、平垫圈、弹性垫圈之间循环，不再出现旧演示名或泛化垫片名。 | 若出现其它名称，先查 `Main.qml` 的 `settingsSupportedPartTypes`、`settingsNextPartType()` 和 `settingsApplyAction("part-next")`。 |
| 参数页真实配置验证 | 开发板屏幕和 SSH | 屏幕点击 `参数设置`，调整模型阈值、ROI、UNet 像素阈值和自动上传，点击 `保存配置`；SSH 执行 `cat /mnt/sdcard/config/defect_ui_config.json` | JSON 包含 `model_threshold`、`review_threshold`、`roi_size`、`segment_min_pixels`、`overlay_alpha`、`auto_upload_enabled`；下一次检测使用这些值，分类命令包含 `--bad-threshold`，分割命令包含 `--min-defect-pixels`；自动上传关闭时结果为 `upload_status=SKIP`。 | 若 JSON 不存在，先查 `/mnt/sdcard` 是否挂载和可写；若界面值与 JSON 不一致，查 `DetectSettingsController::saveSettingsToDisk()` 和 QML `detectSettings` 绑定；若检测仍用旧值，查 `CameraStorageController::setDetectSettingsController()` 和后台线程快照复制。 |
| 步进电机速度任意值 | 开发板屏幕和 SSH | 屏幕点击 `参数设置` -> `步进参数`，选择任一电机，点击常规速度行的 `输入`，分别输入 `0`、`137`、`5000` 并点 `应用速度`、`保存并下发`；SSH 执行 `cat /mnt/sdcard/config/defect_ui_config.json | grep -A8 'normal_speed_rpm'` | JSON 的 `stepper_motors[].normal_speed_rpm` 能保存 `0~5000 rpm` 任意整数，参数日志也出现对应 `normal_speed_rpm` 行；F4 若已烧录新固件，应返回 `ACK acked_cmd=0x42 status=0`，表示运行内存参数已接收。 | 若输入 5001 被接受，说明 QML 或 C++ 限幅失效；若 0 被拒绝，检查 `applyStepperSpeedInput()` 和 `clampedInt(source.normalSpeedRpm, 0, 5000)`；若 JSON 没变，确认点了弹窗的 `保存并下发`，并检查 `/mnt/sdcard` 挂载；若 JSON 已变但 F4 无 ACK，确认 `/dev/ttySTM2`、F4 固件和二进制协议 `0x42` 是否已生效。 |
| 参数日志查看验证 | 开发板屏幕和 SSH | 屏幕点击 `参数设置` -> `保存配置`，再点击 `导出摘要`，进入 `日志查看` 点击 `刷新` 并打开 `qt_settings_YYYYMMDD.log`；SSH 执行 `/root/qt_camera_display/qt_camera_display --settings-log-self-test; day=$(date +%Y%m%d); log=/mnt/sdcard/logs/qt_settings_${day}.log; test -s "$log"; tail -n 80 "$log"` | 屏幕和 SSH 都能看到参数日志；内容包含 `action=保存配置`、`action=导出摘要`、`config_path=/mnt/sdcard/config/defect_ui_config.json`、`model_threshold`、`review_threshold`、`roi_size`、`segment_min_pixels`、`overlay_alpha`、`auto_upload_enabled`、`classify_args` 和 `segment_args`；自检输出还应包含 `log_model_contains=qt_settings_YYYYMMDD.log`，证明日志查看模型能扫描到该文件。 | 若日志查看页没有文件，先确认点击后底部提示或自检输出是否为 `参数日志已保存`，再查 `/mnt/sdcard/logs` 是否存在、`recordSettingsSummaryToSdCard()` 返回值、`refreshLogFileList()` 是否执行；若 SSH 有文件但界面没有，查 `LogFileModel::refresh()` 扫描 `.log` 文件和是否运行旧二进制。 |
| 参数页布局验证 | 开发板屏幕 | 点击左侧 `参数设置`，依次观察顶部摘要、三张参数卡、下方存储卡和参数摘要卡；点击 `+/-`、`切换`、`应用检测`、`保存配置`、`恢复默认` | 所有文字都在卡片边框内，长路径和摘要只省略不换行溢出；参数按钮不挤出卡片，底部提示条不遮挡其它区域。 | 若仍有文字越界，先查 `Main.qml` 中对应卡片是否缺少 `clip: true`、`elide: Text.ElideRight/ElideMiddle`，再按板端实际字体继续缩短文案或减少同屏字段。 |
| 告警页布局验证 | 开发板屏幕 | 点击左侧 `告警维护`，观察当前告警、设备健康矩阵、告警历史、处理建议；点击 `确认`、`清故障`、`刷新状态`、`保存诊断` | 当前告警按钮在告警卡片右侧纵向排列，不挤占发生时间和处理状态；设备健康 3x3 网格包含 `4G` 状态格，告警历史每一行和处理建议都不越过卡片边界；长诊断路径和状态文字只省略显示。 | 若当前告警按钮仍压住文字，检查 `alarmCurrentPanel` 是否为左信息右按钮布局；若历史行仍超出，优先检查 `alarmHistoryListView` delegate 的列宽总和与 `spacing`；若设备健康覆盖刷新按钮，检查 `alarmHealthGrid` 单元高度、行距和刷新按钮 `y`；若仍显示 `配置`，说明板端 QML 资源或二进制未更新。 |
| 告警建议查看全部 | 开发板屏幕 | 点击左侧 `告警维护`，再点击处理建议面板右下角 `查看全部`；在弹层内上下滑动，最后点击 `关闭` 或遮罩区域 | 弹层标题为 `处理建议完整说明`，内容包含当前告警码、设备健康摘要，以及相机/KMS、SD 卡、4G 网络、云端上传、云端 health、F4 串口、保存链路和模型检测排查步骤；长文本能滚动到末尾，关闭后回到告警维护页。 | 若按钮不出现，检查 `alarmAdviceDetailVisible`、`alarmAdviceDetailOverlay` 和 `查看全部` 是否打进 QML；若不能滑动，检查 `alarmAdviceDetailFlickable` 的 `contentHeight` 和 `clip`；若内容缺项，检查 `alarmFullAdviceText()` 是否仍复用 `alarmSourceAdvice()`。 |
| 告警 4G 健康格 | 开发板屏幕和 SSH | 开发板执行 `command -v 4g-ppp; 4g-ppp test; echo $?`，屏幕点击 `告警维护` -> `刷新状态` | `4g-ppp test` 返回 `0` 时，设备健康矩阵 `4G` 格应显示 `在线` 和绿色；返回非 0、超时或命令缺失时，`4G` 格应显示 `离线/超时/未安装` 等非在线状态，颜色不为绿色。 | 若 SSH 命令失败但屏幕仍在线，查 `DeviceHealthController::handleNetworkProcessFinished()`、`networkStatusText` 绑定和是否运行旧二进制；若状态在检测中和在线之间闪烁，检查 `startNetworkProbe()` 是否又每轮设置了 `检测中`。 |
| 告警诊断快照落盘 | 开发板屏幕和 SSH | 屏幕点击 `告警维护` 的 `保存诊断`，或执行 `/root/qt_camera_display/qt_camera_display --alarm-snapshot-self-test`，再执行 `day=$(date +%Y%m%d); file=/mnt/sdcard/logs/qt_alarm_snapshot_${day}.txt; test -s "$file"; wc -c "$file"; tail -n 30 "$file"` | 每次点击或自检都追加当天诊断快照文件；文件存在且非空，`tail` 能看到 `snapshot_time=`、`alarm_code=`、`alarm_status=`、`camera_status=`、`device_health=` 和 `[recent_alarm_history]`；按钮提示为真实 C++ 返回值。 | 若目录为空，先查屏幕提示或自检输出是否为 `诊断保存失败`；再查 `/tmp/qt_camera_display.log` 中 `storage action alarm-snapshot`、`mount | grep ' /mnt/sdcard '`、`df -h /mnt/sdcard`、板端 `date` 和 SD 卡是否只读。 |
| 自动告警日志落盘 | 开发板屏幕和 SSH | 触发相机/KMS 无帧、SD 卡不可写、4G/云端异常、F4 待接入、保存/上传失败或模型检测失败；也可执行 `/root/qt_camera_display/qt_camera_display --alarm-log-self-test`，再执行 `day=$(date +%Y%m%d); log=/mnt/sdcard/logs/qt_alarm_${day}.log; test -s "$log"; wc -c "$log"; tail -n 40 "$log"` | 每个新问题追加当天 `qt_alarm_YYYYMMDD.log`，内容包含 `occurrence_time=`、`source=`、`alarm_code=`、`device_health=` 和 `[troubleshooting]`；同一问题未恢复前不会因 8 秒健康刷新重复刷日志。 | 若没有新日志，先确认问题是否已经在 `activeAlarmKeys` 中处于未恢复状态；再查屏幕提示、`/tmp/qt_camera_display.log` 中 `storage action alarm-log`、SD 卡挂载/可写状态、板端 `date` 和 C++ 返回原因。 |
| 手动页入口验证 | 开发板屏幕 | 点击左侧 `手动控制` | 手动页显示传送带、检测辅助、安全状态、人工复核和命令日志；实时 KMS 视频 plane 不覆盖页面；当前页面不显示未接入的相机运动轴按钮或占位状态。 | 若无法进入，检查 `switchPage()` 是否允许 `manual`，左侧导航点击分支是否包含 `manual`；若视频覆盖页面，查 `setOverlayVisible(pageName === "home")` 和 overlay `VISIBLE` 命令；若又出现未接运动轴按钮，查 `manualAssistPanel` 附近是否回退。 |
| 手动页安全置灰 | 开发板屏幕 | 未点击 `进入手动` 时观察并点击 `巡航启动` | `巡航启动` 应置灰；若触摸事件仍到达保护分支，应提示 `请先进入手动模式` 并写入命令日志。 | 若未进手动仍能执行巡航，检查 `manualActionAllowed()` 和 `handleManualAction()` 的 `manualMode` 保护分支。 |
| 手动模式切换 | 开发板屏幕 | 点击 `进入手动`，再点击传送带 `巡航启动`、`查询状态`、`停止` | 顶部模式显示 `手动`；传送带状态根据 F4 回执显示 `巡航中/状态已返回/停止`；命令日志最新在上。 | 若模式不变，查 `manualMode` 绑定；若日志不更新，查 `manualCommandLog.insert(0, ...)`；若传送带状态不变，查 `onF4ManualCommandFinished` 是否收到 C++ 信号。 |
| 高德 IP 位置显示检查 | 开发板屏幕和 SSH | 开机后进入首页观察顶部 `位置`；SSH 执行 `cat /var/run/4g-location.state; tail -n 80 /var/log/4g-location.log` | 顶部 `位置` 显示高德 IP 省份定位结果，例如 `河南省`；告警维护页设备健康矩阵显示 `定位:IP定位/缺少Key/定位失败`；Qt 本次进程只在开机第一轮调用一次 `4g-location once`，后续健康刷新不再访问高德接口。 | 若仍显示 `检测位`，说明 QML 资源或板端二进制未更新；若显示 `缺少Key`，检查 `AMAP_WEB_KEY` 或 `/etc/4g-location/amap-web-key`；若显示 `定位失败`，查 4G 网络、`restapi.amap.com/v3/ip` 访问和 `/var/log/4g-location.log`。 |
| 未接运动轴显示检查 | 开发板屏幕 | 进入 `手动控制`、`参数设置` 和 `告警维护` 页面逐项观察 | 当前界面不显示相机运动轴按钮、`待F4协议`、`等待CAM协议` 或其它未接占位；顶部状态栏仍保留 `位置`，其值来自高德 IP 省份定位。 | 若出现未接占位，先查 `Main.qml` 的 `statusRow`、手动页、`settingsMotionPanel` 和 `alarmHealthGrid` 是否运行旧二进制或被回退。 |
| 手动页检测当前帧 | 开发板屏幕和 SSH | 手动页点击 `检测当前帧`，再执行 `ls -lt /mnt/sdcard/images/uvc_*.jpg /mnt/sdcard/images/segment_* | head -n 8` | source JPG 和 UNet raw/overlay/mask 最新文件生成，底部提示沿用检测链路，命令日志出现 `已请求双模型检测当前帧`。 | 若图片未生成，按检测链路排查 `/mnt/sdcard`、overlay socket、`requestDetectCurrentFrame()` 和 `/tmp/qt-kms-overlay-shell.log`。 |
| 历史页删除验证 | 开发板屏幕和 SSH | 删除前从最新 JSON 记录确认 `source_path` 和 `annotated_images[].path`，屏幕点击对应历史卡片 `删除`，再确认这些路径均不存在且 JSON 不再包含该 `source_path`。 | 被删记录对应 source/annotated 文件不存在，历史 JSON 不再包含该 source 路径；界面自动选中相邻记录或显示 `暂无检测记录`。 | 若 JSON 仍包含路径，查 `/tmp/qt-kms-overlay-shell.log` 中 `upload history remove save failed`；若 JSON 删除但图片还在，查日志里的 `upload history image remove failed` 或路径是否不在 `/mnt/sdcard/images`。 |
| 历史页 overlay 遮挡检查 | 开发板屏幕和 SSH | 进入历史页后观察实时视频是否消失；必要时执行 `printf 'VISIBLE 0\n' | nc -U /tmp/uvc-kms-overlay-control.sock` 和 `printf 'VISIBLE 1\n' | nc -U /tmp/uvc-kms-overlay-control.sock` | 历史页打开时实时 KMS 视频 plane 不遮挡历史图片，返回首页后实时视频恢复。 | 若 `nc -U` 不存在，可只用屏幕行为验收；若历史页仍被视频覆盖，查 `uvc_kms_overlay` 是否为新版并支持 `VISIBLE` 命令。 |
| 默认账号配置检查 | 开发板 SSH | `ls -l /root/qt_camera_display/cos-upload.env; grep ^CLOUD_ /root/qt_camera_display/cos-upload.env | cut -d= -f1` | `ls -l` 输出 `-rw------- root root`，并只列出 `CLOUD_ACCOUNT`、`CLOUD_PASSWORD` 两个变量名。 | 若文件不存在或不可读，重新部署默认账号配置；不要把账号密码写进源码或日志。 |
| COS 上传脚本自检 | 开发板 SSH | `/root/qt_camera_display/defect-cos-upload --jpg "$raw_jpg" --annotated "$overlay_jpg" --annotated "$mask_png"` | stdout 输出 `上传成功：record_id=... record_no=... source_kind=source annotated_count=2`；stderr 打印 `record_no`、`created_record_id`、`prepare_local_tag`、`prepare_object_key`、`register_url`，且所有 `register_url` 都是新记录 ID；详情页时间应接近 `TZ=CST-8 date` 显示的北京时间；如果 `CLOUD_PART_CODE=wave_washer` 且云端没有该零件，云端应自动出现 `wave_washer / 波形垫圈 / 垫圈类` 零件。 | 若登录失败，查 `cos-upload.env`、Cookie；若自动查询设备失败，手动设置 `CLOUD_DEVICE_ID`；若自动创建后名称不对，查 `CLOUD_PART_NAME/CLOUD_PART_CATEGORY` 覆盖值和云端记录请求体；若提示 `object_key 未包含当前 record_no`，说明 prepare 仍落到旧记录路径，停止继续登记；若详情时间偏 8 小时，查 `board-time-sync status` 和脚本是否导出 `TZ=CST-8`。 |
| COS 记录回查 | 开发板 SSH | `record_id=<上一步ID>; curl -fsS -b /tmp/cloud_cookie.txt "http://139.9.35.72/api/v1/records/$record_id"` | 返回记录详情，`files` 中包含 1 张 `source` 原图和传入数量一致的 `annotated` 检测结果图，并有 `preview_url`；旧 `record_id=3` 不再新增文件；`captured_at/uploaded_at` 与 `TZ=CST-8 date` 同小时。 | 若 `files` 为空或数量不是 `1 + annotated_count`，脚本会返回上传失败；优先查脚本输出的 `created_record_id`、`prepare_object_key` 和后端 `/records/<id>/files` 日志；若时间偏 8 小时，先看 `board-time-sync status`、板端 `/tmp/defect-cos-record.json` 和 `/tmp/defect-cos-register-*.json` 原始值。 |
| 安全卸载按钮 | 开发板屏幕和 SSH | 屏幕点击 `安全卸载`，再执行 `/etc/init.d/S85sdcard-mount status` | 界面显示卸载完成，状态显示未挂载或等待拔卡。 | 若卸载失败，查是否有进程占用 `/mnt/sdcard`，不要直接拔卡。 |
| 零拷贝候选探测 | 开发板 | `VIDEO_DEV=/dev/video0 CAMERA_WIDTH=640 CAMERA_HEIGHT=480 CAMERA_FPS=15 /root/qt_camera_display/probe_zero_copy_video_path.sh` | 在 `logs/zero-copy-probe-*.log` 生成探测日志。 | 若某路线失败，保留日志，不要重新启用已知崩溃的 QtMultimedia `Camera + VideoOutput`。 |

## 读写/数据路径验证

| 数据路径 | 读验证 | 写验证 | 通过标准 |
|---|---|---|---|
| Qt 运行资源 | `ls /root/qt_camera_display/qt_camera_display /usr/lib/libQt5Core.so.5` | `deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` | 板端程序和 Qt runtime 同时存在。 |
| 早期静态首帧 | `ls -l /root/qt_camera_display/fb_boot_splash /root/qt_camera_display/boot_splash.rgb565 /dev/fb0` | `/root/qt_camera_display/fb_boot_splash -f /dev/fb0` | LCD 显示 HTML 渲染资源对应的 AI 竞赛品牌静态启动首帧，命令返回 `0`；该路径不读取摄像头、不依赖 `/dev/galcore`，只验证 framebuffer 输出。 |
| UVC 输入 | `v4l2-ctl -d /dev/video0 --list-formats-ext` | KMS overlay 或安全预览通过 V4L2 ioctl 配置采集 | 能取帧并显示，日志无 `VIDIOC_*` 致命错误。 |
| KMS overlay 输出 | `run_qt_kms_overlay_display.sh status; printf 'STATUS\n' | nc -U /tmp/uvc-kms-overlay-control.sock` | `uvc_kms_overlay` 调用 DRM plane 上屏，Qt 健康检测后台发送 `STATUS` | overlay PID 存在，`STATUS` 返回 `has_frame=1 serial>0` 且连续两轮 `serial` 增长才代表真实摄像头在线。 |
| 4G 状态探测 | `command -v 4g-ppp; 4g-ppp test; echo $?` | Qt `DeviceHealthController` 异步执行 `4g-ppp test` | 只有测试退出码为 0 时顶部网络显示在线；失败不能显示在线。 |
| F4 状态探测 | `ls -l /dev/ttySTM2`，屏幕观察顶部 F4 状态和底部 `F4:` 提示 | Qt 后台线程向 `/dev/ttySTM2` 写入二进制 `HEARTBEAT 0x02`；常规心跳 120 秒节流，人工刷新立即发送；查询状态按钮写入二进制 `QUERY_STATUS 0x40` | 收到匹配 `ACK/STATUS_REPORT` 时显示接入或状态详情；收到 `NACK/FAULT_REPORT`、无回复或串口失败时底部显示 `F4: ...` 错误详情。 |
| F4 称重标定 | 屏幕弹窗观察；源码静态检查 `rg -n "WEIGHT_CALIBRATE|sendF4Command|formatF4ToastText" main.cpp qml/Main.qml` | QML 保留克重输入 UI，C++ 发送二进制 `WEIGHT_CALIBRATE 0x30` 5 字节负载，不再向 `/dev/ttySTM2` 写 `CAL` 文本；F4 回包经 `f4CommandFinished` 进入弹窗和底部提示 | 成功应显示 `F4: 标定命令成功：ACK WEIGHT_CALIBRATE...`；失败以二进制 `NACK/FAULT_REPORT` 显示，QML 主线程不阻塞。 |
| 云端状态探测 | `curl -fsS --max-time 2 http://139.9.35.72/health` | Qt 异步执行 curl health | curl 成功才显示已连接；失败、弱网或超时不显示已连接。 |
| 云端复核回写反向隧道 | 板端读 `/etc/init.d/S91board-review-tunnel status`；云端读 `ss -ltnp | grep 127.0.0.1:18081` 和 `/var/log/yunduan-board-review-tunnel-check.log` | 板端 `board-review-tunnel.sh` 写 ssh -R 隧道；云端 timer 只做周期检查和日志记录 | 板端守护和 ssh 隧道都运行，云端 GET `127.0.0.1:18081/api/v1/review-result` 能收到板端 404 JSON；断线重连由板端完成，不依赖 Windows 或虚拟机。 |
| SD 卡健康探测 | `mount | grep ' /mnt/sdcard '; test -w /mnt/sdcard && echo writable` | Qt 读取 `/proc/mounts` 并检查挂载目录是否可写 | 挂载点存在且可写时显示 `可写`；未挂载显示 `未挂载`；挂载但不可写显示 `不可写` 并触发 SD 卡告警。 |
| KMS overlay 当前帧保存 | `test -S /tmp/uvc-kms-overlay-control.sock; ls -lh /mnt/sdcard/images/uvc_*.jpg` | Qt 检测后台任务向 socket 发送 `SAVE_DETECT /mnt/sdcard/images` | source JPG 来自当前原始 YUYV 缓冲，写入后 overlay 已执行 `fsync`；绿色 ROI 观察框不会进入模型输入图。 |
| 检测历史记录 | `day=$(date +%Y%m%d); cat /mnt/sdcard/images/upload_history_${day}.json` | Qt 检测后台任务完成后，主线程 `UploadHistoryModel` 追加当天 JSON 记录 | 历史记录包含本次 source、annotated 图数组、两个模型输出、上传状态、文件大小和云端记录信息；历史页读取多天每日文件后可横向查看。 |
| 历史失败重发 | `day=$(date +%Y%m%d); tail -n 120 /mnt/sdcard/images/upload_history_${day}.json` | 历史详情页 `重新发送` 调用 `CameraStorageController::retryUploadRecord()`，后台复用本地 source/annotated 图片上传，主线程调用 `UploadHistoryModel::updateRecordUploadResult()` 更新对应每日 JSON | 重发不会新增本地图片文件；成功时同一条记录的 `upload_time` 改为重发完成时间，记录移动到数组末尾，`record_id/record_no/upload_status` 改为新云端结果，并按本次重发日期写入每日文件；失败时保留原时间、原位置和失败摘要，便于继续重试。 |
| 统计分析汇总 | `grep -h '"upload_time"' /mnt/sdcard/images/upload_history_*.json 2>/dev/null | wc -l` | 统计页通过 `uploadHistory` 模型读取多天每日 JSON，不额外写统计数据库 | 统计页 `总记录` 与每日 JSON 汇总条目数量一致，分布概览五项分两列完整显示，最近记录表可跳转历史详情；没有历史记录时显示空状态。 |
| 手动控制命令日志 | 屏幕查看 `手动控制` 页命令日志；源码静态检查 `rg -n "manualCommandLog|appendManualCommandLog|sendManualBeltCommand|handleManualAction" qml/Main.qml` | 传送带按钮经 `sendManualBeltCommand()` 写入 F4 二进制协议帧并把回包追加到 `manualCommandLog`；检测当前帧、人工标记、急停模拟和清故障只写界面日志，不伪装成 F4 运动轴命令。 | 日志显示时间、命令、目标和结果；传送带记录包含 `BELT_MANUAL_SCAN/BELT_MANUAL_STOP/QUERY_STATUS` 和 F4 二进制回执；当前界面不应出现未接运动轴成功记录。 |
| 参数与告警 UI 状态 | 屏幕查看 `参数设置`、`告警维护`；源码静态检查 `rg -n "settingsSummaryText|stepper_motors|sendF4StepperSettings|f4StepperSettingsFinished|applyStepperSpeedInput|alarmSnapshotText|evaluateRuntimeAlarms|recordAlarmIssueToSdCard|alarmHistoryListView|alarmAdvicePanel|alarmAdviceDetailOverlay|alarmAdviceDetailFlickable" qml/Main.qml main.cpp` | QML 按钮点击更新真实检测配置、三台步进电机参数、提示条和 `alarmHistoryModel`；保存诊断调用 C++ 控制器追加当天 `qt_alarm_snapshot_YYYYMMDD.txt`；新问题调用 C++ 控制器追加当天 `qt_alarm_YYYYMMDD.log`；处理建议短面板和完整弹层共用同一组 `alarmSourceAdvice()` 排查内容。 | 参数页已把视觉检测配置和 `stepper_motors` 写入 `/mnt/sdcard/config/defect_ui_config.json`；视觉参数影响下一次检测，步进电机参数通过 `STEPPER_PARAM_SET 0x42` 下发给 F4 运行内存；告警页会记录真实健康/检测链路问题，但 `清故障` 不等于真实 F4 联锁解除。 |
| 告警日志/快照文件 | `day=$(date +%Y%m%d); snap=/mnt/sdcard/logs/qt_alarm_snapshot_${day}.txt; log=/mnt/sdcard/logs/qt_alarm_${day}.log; ls -lh "$snap" "$log"; test -s "$snap"; tail -n 30 "$snap"; test -s "$log"; tail -n 40 "$log"` | `saveAlarmSnapshotToSdCard()` 和 `recordAlarmIssueToSdCard()` 都先确认 `/mnt/sdcard` 挂载、创建 `/mnt/sdcard/logs`、检查可写，再追加写入当天文件并执行 `fsync`；可由屏幕按钮、自动告警或自检触发 | 文件非空，包含当前告警码、状态、相机/视频/存储摘要、最近告警历史和排查建议；点击后不需要固定 `sleep`，如需拔卡仍要执行 `sync` 或 `sdcard-safe-remove`。 |
| COS 上传 | `ls -l /root/qt_camera_display/cos-upload.env; tail -n 20 /tmp/defect-cos-record-detail.json; cat /tmp/source_upload_headers.txt /tmp/annotated_upload_headers-*.txt` | `defect-cos-upload` 读取默认账号配置后调用后端 API 和 COS PUT | 配置文件权限为 `600`，HTTP 200、响应头含 ETag，云端新记录详情出现本次 1 张 `source` 和全部 `annotated` 文件对象。 |
| 触摸输入 | `grep -A8 -B2 "Goodix" /proc/bus/input/devices` | Qt 按钮状态由触摸事件驱动更新 | 点击按钮后状态栏文字变化。 |
| 检测图片/日志 | `wc -c <file>`、`tail <log>` | 后续检测程序写入 `/mnt/sdcard/images` 或日志目录 | 文件大小稳定、日志进程退出、`sync` 或应用内 `fsync` 完成。 |

## 检测结果落盘等待规则

当前正式入口是 `检测` 按钮和 `--detect-self-test`。一次检测会生成 1 张 source JPG、3 张 UNet annotated 图片，并把两个模型 RESULT 写入同一条历史记录。不要用固定 `sleep` 冒充完成判断；必须等待按钮状态恢复或命令退出，再检查文件头、文件大小和历史 JSON。

```sh
mount | grep ' /mnt/sdcard '
df -h /mnt/sdcard
# 在屏幕上点击“检测”，或执行 ./qt_camera_display --detect-self-test 后执行：
src=$(ls -t /mnt/sdcard/images/uvc_*.jpg | head -n 1)
raw=$(ls -t /mnt/sdcard/images/segment_*_raw.jpg | head -n 1)
overlay=$(ls -t /mnt/sdcard/images/segment_*_overlay.jpg | head -n 1)
mask=$(ls -t /mnt/sdcard/images/segment_*_mask.png | head -n 1)
day=$(date +%Y%m%d)
hist=/mnt/sdcard/images/upload_history_${day}.json
ls -lh "$src" "$raw" "$overlay" "$mask" "$hist"
head -c 2 "$src" | hexdump -C
head -c 2 "$raw" | hexdump -C
head -c 2 "$overlay" | hexdump -C
head -c 8 "$mask" | hexdump -C
s1=$(wc -c < "$src")
r1=$(wc -c < "$raw")
o1=$(wc -c < "$overlay")
m1=$(wc -c < "$mask")
sleep 1
s2=$(wc -c < "$src")
r2=$(wc -c < "$raw")
o2=$(wc -c < "$overlay")
m2=$(wc -c < "$mask")
[ "$s1" = "$s2" ] && [ "$r1" = "$r2" ] && [ "$o1" = "$o2" ] && [ "$m1" = "$m2" ] && \
  [ "$s1" -gt 0 ] && [ "$r1" -gt 0 ] && [ "$o1" -gt 0 ] && [ "$m1" -gt 0 ]
# 确认后再点击“安全卸载”，或在 SSH 中执行：
sdcard-safe-remove
```

| 保存对象 | 完成条件 | 额外说明 |
|---|---|---|
| source JPG | `检测` 返回或 `--detect-self-test` 退出，文件头为 JPEG SOI，文件存在且大小稳定，overlay 已 `fsync` | 路径形如 `/mnt/sdcard/images/uvc_YYYYMMDD_HHMMSS_*.jpg`，上传时登记为 `source`。 |
| UNet raw/overlay JPG | `RESULT_SEG` 中含 `raw_path/overlay_path`，文件头为 JPEG SOI，文件存在且大小稳定 | 上传时登记为两张 `annotated`。 |
| UNet mask PNG | `RESULT_SEG` 中含 `mask_path`，文件头为 PNG 签名，文件存在且大小稳定 | 上传时登记为一张 `annotated`，content type 必须是 `image/png`。 |
| COS 文件对象 | `defect-cos-upload` 返回成功，COS PUT 为 HTTP 200，响应头含 ETag，记录详情 `files` 包含本次 1 张 `source` 和全部 `annotated` 图 | 上传成功不等于本地文件可以立即拔卡；仍要确认本地文件写完并安全卸载。 |
| 检测 CSV/日志 | 日志进程退出或关闭文件，大小稳定，`sync` 完成 | 持续追加日志不能在进程仍运行时拔卡。 |
| 视频文件 | 录像进程正常退出，文件大小稳定，`sync` 完成 | 视频容器索引常在退出时写入，不能只看文件非 0。 |
| SD 卡移除 | `sdcard-safe-remove` 提示可以安全拔卡 | 突然断电只能靠下次挂载前 `fsck.fat` 修复，不能替代正常安全卸载。 |

## 为什么不用原来的 `uvc_fb_preview`

`uvc_fb_preview` 会在用户态执行 `YUYV -> RGB565/XRGB8888 -> /dev/fb0`，每帧都由 CPU 做像素转换和显存写入，所以你看到它接近 50% CPU 是正常现象。

这个 Qt 版本把界面合成放到 Qt Quick Scene Graph，由 OpenGL ES 交给 `galcore` GPU 执行。当前摄像头预览使用自定义 V4L2 采集线程，避开 QtMultimedia 的 Vivante 视频节点崩溃问题。

## 当前打通记录

| 日期 | 项目 | 结果 |
|---|---|---|
| 2026-04-29 | 旧 CPU framebuffer 预览 | `/root/uvc_fb_preview` 约 `44%~49% CPU` |
| 2026-04-29 | QtMultimedia `Camera + VideoOutput` | 依赖补齐后能加载 `libgstcamerabin.so`，但会触发 `galcore _UserMemoryAttach/dma_map_sg` 内核 Oops |
| 2026-04-29 | 自定义 V4L2 + OpenGL ES 安全预览，`640x480@15fps` | 能显示摄像头，平均 CPU 约 `42.0%`，画质可用但 CPU 仍高 |
| 2026-04-29 | 自定义 V4L2 + OpenGL ES 安全预览，`320x240@10fps` | 能显示摄像头，平均 CPU 约 `5.9%`，但画质不满足最终项目要求 |
| 2026-04-29 | GStreamer GL 硬件视频路线，`640x480@15fps` | `v4l2src io-mode=dmabuf ! glupload ! glimagesink` 10 分钟长测 CPU 约 `2.0%`，无 `galcore` Oops |
| 2026-04-29 | `qmlglsink` mmap 内嵌桥接 | `VIDEO_BACKEND=qt-gst` 已能显示 Qt UI + 摄像头画面，`640x480@15fps` CPU 约 `36.2%`，属于稳定可演示桥接 |
| 2026-04-29 | `qmlglsink` DMABUF 内嵌 | `GST_IO_MODE=dmabuf VIDEO_BACKEND=qt-gst` 会 rc=`139`，core/gdb 指向 Vivante `libGAL.so:gcoTEXTURE_GetMipMap()` |

当前结论：

| 结论 | 说明 |
|---|---|
| Qt Quick/eglfs/GPU UI 已打通 | 字体、QML、eglfs、`galcore` 主界面渲染链路可用 |
| UVC 摄像头显示已打通 | 通过自定义 V4L2 路径可以稳定显示，不再依赖 QtMultimedia |
| 当前低 CPU 来自降分辨率/降帧率 | `320x240@10fps` 数据量低，所以 CPU 低，但不是最终高清方案 |
| 已打通独立硬件视频路线 | 可用 `VIDEO_BACKEND=gst-gl /root/qt_camera_display/run_qt_camera_display.sh` 直接观察效果 |
| 已打通 Qt 内嵌桥接 | `VIDEO_BACKEND=qt-gst` 默认 mmap 可把视频嵌回工业界面，但 CPU 仍高，只能作为保底演示路线 |
| 下一步主线 | 避开 `qmlglsink + UVC DMABUF` 崩溃路径，优先验证 Wayland surface 合成，其次 KMS plane 合成，最后再考虑自研 Qt GL Item 可控导入 DMABUF |

详细下一步路线见：

```bash
20_uvc_camera/qt_camera_display/zero_copy_hardware_video_plan.md
```

## 零拷贝/硬件视频探测脚本

当前仓库新增了板端探测脚本：

```bash
/root/qt_camera_display/probe_zero_copy_video_path.sh
```

这个脚本不启用 QtMultimedia，也不恢复 `Camera + VideoOutput`。它会先收集 `/dev/video*`、`/dev/dri/card0`、`/dev/fb0`、`/dev/galcore`、`v4l2-ctl --list-formats-ext` 和 `gst-inspect-1.0` 证据，然后优先短时验证：

```bash
gst-launch-1.0 -v v4l2src device=/dev/video0 io-mode=dmabuf ! \
  video/x-raw,format=YUY2,width=640,height=480,framerate=15/1 ! \
  kmssink sync=false
```

脚本会把每轮输出保存到：

```bash
/root/qt_camera_display/logs/zero-copy-probe-*.log
```

如果需要指定摄像头或测试参数，可以这样运行：

```bash
VIDEO_DEV=/dev/video1 CAMERA_WIDTH=640 CAMERA_HEIGHT=480 CAMERA_FPS=30 \
  /root/qt_camera_display/probe_zero_copy_video_path.sh
```

当前板端探测结果：

| 路线 | 结果 |
|---|---|
| `v4l2src io-mode=dmabuf ! fakesink` | `640x480@15fps` 可运行，CPU 约 `0.1%~0.3%` |
| `v4l2src io-mode=dmabuf ! kmssink driver-name=stm` | KMS 可打开，但因 STM32MP157 plane 不支持 YUYV/NV12，报 `not-negotiated` |
| `v4l2src io-mode=dmabuf ! videoconvert ! BGRA ! kmssink driver-name=stm` | 可显示，CPU 约 `21.5%`，属于非零拷贝基线 |
| `v4l2src io-mode=dmabuf ! glupload ! glimagesink` | 可显示，10 分钟长测 CPU 约 `2.0%`，未出现 `galcore` Oops |

因此当前结论不是继续把 `qmlglsink + UVC DMABUF` 当主线。`qmlglsink` 已能用 mmap 桥接嵌回 Qt Quick，但 DMABUF 路径会在 Vivante 用户态崩溃；后续低 CPU 主线应转向 Wayland surface 合成、KMS plane 合成，或自研 Qt GL Item 在 Qt GL 上下文中可控导入 DMABUF。

## 当前启动参数

默认参数：

```bash
/root/qt_camera_display/run_qt_camera_display.sh
```

等价于：

```bash
VIDEO_DEV=/dev/video0 CAMERA_WIDTH=320 CAMERA_HEIGHT=240 CAMERA_FPS=10 /root/qt_camera_display/run_qt_camera_display.sh
```

如果要临时提高画质测试：

```bash
CAMERA_WIDTH=640 CAMERA_HEIGHT=480 CAMERA_FPS=15 /root/qt_camera_display/run_qt_camera_display.sh
```

注意：提高采集尺寸和帧率会显著增加 CPU 占用，因为当前安全路径仍有 V4L2 帧复制和 `glTexImage2D` 纹理上传。

如果要直接查看已经打通的低 CPU 硬件视频路线效果：

```bash
VIDEO_BACKEND=gst-gl /root/qt_camera_display/run_qt_camera_display.sh
```

这条命令会停掉旧 Qt/Framebuffer/GStreamer 预览，然后运行：

```bash
gst-launch-1.0 -v v4l2src device=/dev/video0 io-mode=dmabuf ! \
  video/x-raw,format=YUY2,width=640,height=480,framerate=15/1 ! \
  glupload ! glimagesink sync=false
```

`gst-gl` 模式用于先观察画质、流畅度和 CPU，不显示工业检测 UI；Qt 内嵌可用 `VIDEO_BACKEND=qt-gst` mmap 桥接查看效果，但最终低 CPU 路线需要继续验证 Wayland/KMS/自研 GL Item。

## 检测按钮与双模型部署

当前首页 `检测` 按钮不在 Qt 进程内直接链接 ONNX Runtime，而是串行调用两个独立推理程序。
点击后 Qt 会通过 KMS overlay 控制 socket 保存当前帧 source JPG 到 `/mnt/sdcard/images`，
先调用 `/root/qt_camera_display/defect-classify` 做 MobileNetV3-Small INT8 分类，再调用
`/root/qt_camera_display/defect-segment` 做 UNet INT8 分割，最后把 source JPG、UNet raw/overlay/mask
三张 annotated 图、两个模型输出和云端上传结果合并到一次历史记录。

本次修改文件清单：

| 文件 | 修改原因 |
|---|---|
| `main.cpp` | 增加 `requestDetectCurrentFrame()` 和 `--detect-self-test` 双模型入口，负责保存当前帧、调用 `defect-classify`、调用 `defect-segment`、综合两个模型结果、上传 source/annotated 图片并追加历史记录 |
| `qml/Main.qml` | 首页右侧结果面板保留“检测”按钮，显示模型零件名、GOOD/BAD、类别、百分制置信度、good/bad 总概率、综合判定和双模型总耗时；历史页显示原始图片和 UNet raw/overlay/mask |
| `uvc_kms_overlay.c` | 支持 `SAVE_DETECT /mnt/sdcard/images` 控制命令，保存单张检测 source JPG；同时在 YUYV 转换行内绘制中心 `300x300` ROI 观察框，避免整帧后补画导致闪烁 |
| `defect_classify.cpp` | 新增独立 ONNX Runtime + libjpeg 推理程序，执行 300x300 中心 ROI、224x224 resize、ImageNet 标准化和 6 类分类 |
| `build_defect_classify.sh` | 新增 `defect-classify` 交叉编译脚本，依赖 ARMv7 ONNX Runtime SDK |
| `defect_segment.cpp` | 新增独立 ONNX Runtime + libjpeg/libpng 分割程序，输出 `RESULT_SEG`，并生成 raw JPG、overlay JPG、mask PNG |
| `build_defect_segment.sh` | 新增 `defect-segment` 交叉编译脚本，依赖 ARMv7 ONNX Runtime SDK、libjpeg 和 libpng |
| `deploy_qt_camera_display.sh` | 部署 `defect-classify`、`defect-segment`、分类 ONNX、UNet ONNX、labels JSON 和 `libonnxruntime.so*` 到 NFS rootfs |
| `test_qt_kms_overlay_assets.sh` | 增加检测按钮、双模型入口、模型部署、`SAVE_DETECT`、annotated 上传和 ROI 观察框静态契约检查 |
| `.gitignore` | 排除 `build-mp157/`、本地模型目录和 `onnxruntime-arm/` SDK，避免提交大文件或第三方二进制 |

| 项目 | 路径/行为 | 说明 |
|---|---|---|
| Qt 入口 | `main.cpp::CameraStorageController::requestDetectCurrentFrame()` | 后台线程触发当前帧保存、分类、分割、上传和历史记录写入，不阻塞触摸事件循环 |
| overlay 命令 | `SAVE_DETECT /mnt/sdcard/images` | 保存 source JPG，要求 SD 卡挂载，后续写入历史记录并触发 COS 上传 |
| 分类推理程序 | `/root/qt_camera_display/defect-classify` | 独立 C++ 程序，依赖 ONNX Runtime ARM 动态库和 libjpeg |
| 分割推理程序 | `/root/qt_camera_display/defect-segment` | 独立 C++ 程序，依赖 ONNX Runtime ARM 动态库、libjpeg 和 libpng |
| 分类模型 | `/root/qt_camera_display/models/defect_classifier_static_mixed_int8.onnx` | 来自 `D:\model_picture\checkpoints_classify\defect_classifier_static_mixed_int8.onnx` |
| UNet 模型 | `/root/qt_camera_display/models/defect_unet_test_decoder_head_int8.onnx` | 来自 `D:\model_picture\checkpoints_unet_test\defect_unet_test_decoder_head_int8.onnx` |
| 默认标签 | `/root/qt_camera_display/models/defect_classifier_static_mixed_int8_labels.json` | 类别顺序必须与 ONNX 输出一致 |
| 默认 ROI | 中心 `300x300` | 与当前训练/PC 摄像头测试命令 `--roi_size 300` 保持一致 |
| 屏幕 ROI 观察框 | KMS overlay 绿色框 | 640x480 采集画面下坐标约为 `x=170,y=90,w=300,h=300`；仅用于观察零件是否进中心 ROI |
| 检测输入图 | `/mnt/sdcard/images/uvc_*.jpg` | 点击检测时由 `uvc_kms_overlay` 从当前原始 YUYV 缓冲转换生成，避免绿色框进入模型输入 |
| UNet 结果图 | `/mnt/sdcard/images/segment_*_raw.jpg`、`*_overlay.jpg`、`*_mask.png` | 全部作为 `--annotated` 上传并写入同一条历史记录 |
| 检测结果 | QML 结果面板和历史页 | 首页分阶段显示：分类模型完成后显示模型零件名、分类初判、类别名、百分制置信度和 good/bad 总概率，并保持“等待综合判定”；UNet 完成后显示 `fused_status/fused_reason` 综合判定和 `total_time_ms` 双模型耗时；上传完成后历史详情保留云端状态，并把分类/分割模型原始行转换为普通中文检测结论、可信度和缺陷提示。 |

测试阶段的触发时机：

| 场景 | 建议操作 |
|---|---|
| 只是验证模型链路 | 把零件放到画面中心，等画面稳定后手动点“检测” |
| 测试 ROI 是否对齐 | 零件完整落在中心区域后点“检测”，观察类别和置信度是否稳定 |
| 正式自动产线 | 由定位/编码器/中心到位信号触发，不建议连续每帧推理 |
| 零件仍在移动 | 暂不触发检测，容易因为拖影或位置偏移导致误判 |

### 编译 defect-classify 和 defect-segment

`defect-classify` 和 `defect-segment` 都需要 ARMv7 ONNX Runtime SDK。当前脚本默认从 `onnxruntime-arm/` 读取头文件和动态库；
如果你把 SDK 放到别处，使用 `ORT_ROOT` 指定。

当前测试环境使用过 `VOICEVOX/onnxruntime-builder` 的 `onnxruntime-linux-armhf-1.17.3.tgz` 验证链路。
它能在当前板端运行，但会输出 `/lib/libstdc++.so.6: no version information available` 警告。
正式交付建议用当前 Buildroot/STM32MP157 rootfs 自己交叉编译固定版本 ONNX Runtime，避免第三方包 ABI 警告。

```bash
cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display
ORT_ROOT=/home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display/onnxruntime-arm ./build_defect_classify.sh
ORT_ROOT=/home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display/onnxruntime-arm ./build_defect_segment.sh
```

成功后生成：

```bash
20_uvc_camera/qt_camera_display/build-mp157/defect-classify
20_uvc_camera/qt_camera_display/build-mp157/defect-segment
```

如果提示找不到：

| 报错 | 先检查 |
|---|---|
| `onnxruntime_cxx_api.h` 缺失 | `ORT_ROOT/include/onnxruntime_cxx_api.h` 是否存在 |
| `libonnxruntime.so` 缺失 | `ORT_ROOT/lib/libonnxruntime.so` 是否存在 |
| `jpeglib.h` 缺失 | Buildroot sysroot 是否带 libjpeg 开发头 |
| `png.h` 缺失 | Buildroot sysroot 是否带 libpng 开发头 |

### 部署模型和运行库

部署脚本会同时复制 Qt 程序、KMS overlay、`defect-classify`、`defect-segment`、分类 ONNX 模型、UNet ONNX 模型、labels JSON 和可选 ONNX Runtime 动态库：

```bash
cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display
DEFECT_MODEL_SRC=/mnt/d/model_picture/checkpoints_classify/defect_classifier_static_mixed_int8.onnx \
DEFECT_LABELS_SRC=/mnt/d/model_picture/checkpoints_classify/defect_classifier_static_mixed_int8_labels.json \
DEFECT_UNET_MODEL_SRC=/tmp/defect_unet_test_decoder_head_int8.onnx \
ORT_ROOT=/home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display/onnxruntime-arm \
./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs
```

如果虚拟机里不能直接访问 Windows D 盘，先把分类模型、labels 和 UNet 模型复制到虚拟机任意目录，再把
`DEFECT_MODEL_SRC`、`DEFECT_LABELS_SRC`、`DEFECT_UNET_MODEL_SRC` 改成虚拟机里的实际路径。

### 板端验证命令

| 测试目标 | 执行位置 | 命令 | 预期输出/现象 | 失败时排查 |
|---|---|---|---|---|
| 确认模型文件已部署 | 开发板 SSH | `ls -lh /root/qt_camera_display/models/defect_classifier_static_mixed_int8.onnx /root/qt_camera_display/models/defect_classifier_static_mixed_int8_labels.json /root/qt_camera_display/models/defect_unet_test_decoder_head_int8.onnx` | 分类 ONNX 约 2 MB，UNet ONNX 约 6 MB，labels JSON 存在 | 检查 `DEFECT_MODEL_SRC`、`DEFECT_LABELS_SRC`、`DEFECT_UNET_MODEL_SRC` 和部署脚本输出 |
| 确认推理程序存在 | 开发板 SSH | `ls -lh /root/qt_camera_display/defect-classify /root/qt_camera_display/defect-segment` | 两个文件存在且可执行 | 先运行两个 build 脚本再部署 |
| 确认 ONNX Runtime 动态库 | 开发板 SSH | `ls -lh /root/qt_camera_display/lib/libonnxruntime.so` | 文件存在 | 检查 `ORT_ROOT/lib/libonnxruntime.so` |
| 单图手动推理 | 开发板 SSH | `LD_LIBRARY_PATH=/root/qt_camera_display/lib:$LD_LIBRARY_PATH /root/qt_camera_display/defect-classify --image /tmp/test.jpg` | 输出 `RESULT status=GOOD|BAD ...` | 确认 `/tmp/test.jpg` 是 JPG，模型和 labels 路径存在 |
| 单图 UNet 推理 | 开发板 SSH | `LD_LIBRARY_PATH=/root/qt_camera_display/lib:$LD_LIBRARY_PATH /root/qt_camera_display/defect-segment --image /tmp/test.jpg --output-dir /mnt/sdcard/images` | 输出 `RESULT_SEG status=OK|NG ... raw_path=... overlay_path=... mask_path=...` | 确认 `/tmp/test.jpg` 是 JPG，UNet 模型和输出目录存在 |
| SSH 双模型检测 | 开发板 SSH | `cd /root/qt_camera_display && ./qt_camera_display --detect-self-test` | 输出 `RESULT status=GOOD|BAD ... fused_status=GOOD|BAD|REVIEW fused_result=good/bad/review segment_status=OK|NG ... upload_status=OK|FAIL`，并追加当天 `/mnt/sdcard/images/upload_history_YYYYMMDD.json` | 查 `/tmp/uvc-kms-overlay-control.sock`、overlay 是否启动、两个模型运行库是否缺失、COS 账号和网络；若分类 `GOOD` 但 UNet `NG` 仍上传 `good`，优先确认 Qt 二进制是否包含 `fusedResultFromModelResults`。 |
| 首页按钮检测 | 触摸屏/开发板 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh start` 后点击“检测” | 分类模型完成后右侧面板先显示模型零件名、分类初判、类别和百分制置信度，并显示 `等待综合判定`；UNet 完成后再显示综合判定和双模型总耗时；上传完成后历史页新增一条包含 4 张图片的检测记录。 | 查 `/tmp/uvc-kms-overlay-control.sock`、overlay 是否启动、模型运行库是否缺失；若结果仍等上传后才显示，确认 Qt 二进制包含 `detectClassificationReady`、`detectModelsReady` 和 `fused_status`。 |
| overlay `LOCATE` 当前帧定位 | 开发板 SSH | `test -S /tmp/uvc-kms-overlay-control.sock && printf 'LOCATE\n' \| nc -U /tmp/uvc-kms-overlay-control.sock` | 返回 `OK LOCATE has_target=0/1 frame_id=... width=640 height=480 center_x=... center_y=... bbox_w=... confidence=...`；放入零件后 `has_target` 应变为 `1`，`center_y` 随零件从上方进入而增大。 | 若返回 `ERR` 或无回复，先查 `/root/qt_camera_display/run_qt_kms_overlay_display.sh status`、`/tmp/uvc-kms-overlay.log`、`/dev/video0` 是否被占用；若一直 `has_target=0`，保存现场原图后调整亮度阈值或水平搜索带宽度。 |
| 板端二进制功能标记 | 开发板 SSH | `strings /root/qt_camera_display/qt_camera_display \| grep -E 'sendF4VisionPosition|autoVisionTimer|BELT_STOP_CENTERED|requestAutoVisionLocate|autoVisionZFocusSettleMs|等待约3秒让摄像头对焦稳定'` | 能看到自动视觉、F4 视觉命令和 Z 轴对焦等待 marker，证明 QML/C++ 已重新编进当前板端 Qt 二进制。 | 若 marker 不存在，说明只改了源码或只拷了 QML，未重新交叉编译并替换 `/root/qt_camera_display/qt_camera_display`。 |
| 首页自动视觉居中和 Z 轴对焦等待 | 开发板屏幕 + F4 串口日志 | 点击首页 `开始`，把零件从画面上方放上传送带进入 ROI | 底部提示先出现 `自动流程ACK`，随后出现 `自动视觉：x=... y=... target=... error=... stable=...`，右侧偏差显示真实 `errorY`；F4 收到 `VISION_POS` 后误差逐步变小；连续居中后 Qt 发送 `BELT_STOP_CENTERED`，传送带停止；随后 Qt 发送 `ACTUATOR_POS_MOVE actuator=2 direction=DOWN`，收到 ACK 后显示 `等待约3秒让摄像头对焦稳定`，3 秒后才 ROI 复查并触发双模型检测；目标出现后如黑色波形零件漏检，界面应显示 `目标短暂丢失` 或 `连续丢失但保持停机`，F4 不应收到新的 `VISION_LOST reason=1` 扫描命令。 | 若 F4 ACK 但电机不动，查传送带 UART4、地址 `0x01`、电源和 F4 传送带任务；若 Z 轴不动，查 `ACTUATOR_POS_MOVE` 分发、USART6、上下轴地址 `0x02`、方向和供电；若 Qt 一直未定位，查 `LOCATE` 和 `AUTO_LOCATE_MIN_LUMA_DELTA`；若越调越远，反转 F4 误差到方向的映射，不在 MP157 偷偷反号。 |
| 检测结果图生成 | 开发板 SSH | `ls -lh /mnt/sdcard/images/uvc_*.jpg /mnt/sdcard/images/segment_* | tail` | 点击检测后出现 source JPG 和 UNet raw/overlay/mask | 若无文件，查 `SAVE_DETECT` socket 命令、`RESULT_SEG` 输出和 overlay 日志 |
| ROI 框观察 | 开发板屏幕 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart` | 实时视频中心出现绿色 `300x300` ROI 框，零件进入框内后再点“检测”；框线不应肉眼可见闪烁 | 若没有框或仍明显闪烁，确认已部署新的 `/root/qt_camera_display/uvc_kms_overlay` 并查看 `/tmp/uvc-kms-overlay.log` |
| 检测图不带框 | 开发板 SSH | `ls -t /mnt/sdcard/images/uvc_*.jpg | head -n 1` 后拉取图片观察 | source JPG 是当前摄像头画面，不包含绿色 ROI 边框 | 若 JPG 带框，说明 overlay 没有使用原始 YUYV 生成图片或运行的仍是旧二进制 |

## 编译

在虚拟机中执行：

```bash
cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display
./build_qt_camera_display.sh
```

脚本会自动执行：

```bash
source /opt/st/stm32mp1/3.1-snapshot/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi
qmake
make
```

编译后程序位于：

```bash
20_uvc_camera/qt_camera_display/build-mp157/qt_camera_display
```

## 部署到 NFS rootfs

```bash
cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display
./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs
```

部署后开发板路径为：

```bash
/root/qt_camera_display/qt_camera_display
/root/qt_camera_display/run_qt_camera_display.sh
/root/qt_camera_display/probe_zero_copy_video_path.sh
```

## 安装 Qt runtime 到当前 rootfs

当前精简 Buildroot rootfs 默认没有 Qt5 runtime。可以先从 ST Qt/Wayland SDK 的目标 sysroot 安装运行库：

```bash
cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display
./install_qt_runtime_from_sdk.sh /home/cfr/linux/nfs/rootfs
```

脚本会安装：

| 路径 | 内容 |
|---|---|
| `/usr/lib/libQt5*.so*` | Qt5 Core/Gui/Qml/Quick 等动态库 |
| `/usr/lib/plugins` | `eglfs`、`wayland-egl`、Vivante EGLFS 集成等插件 |
| `/usr/lib/qml` | `QtQuick` 等 QML 模块 |
| `/usr/lib/gstreamer-1.0` | GStreamer 插件 |
| `/vendor/lib` | Vivante/Nano GPU 的 EGL/GLES 真实库 |

如果 rootfs 里还没有 `gst-launch-1.0`、`gst-inspect-1.0`、`v4l2-ctl`，先把 `output-uvc/target/usr/bin`、`output-uvc/target/usr/lib` 和 `output-uvc/target/usr/libexec` 同步到 NFS rootfs。

## 开发板运行

默认走 `eglfs` 直连 GPU：

```bash
/root/qt_camera_display/run_qt_camera_display.sh
```

如果当前系统已经启动 Weston，可改用 Wayland：

```bash
QT_QPA_PLATFORM=wayland /root/qt_camera_display/run_qt_camera_display.sh
```

指定摄像头节点：

```bash
VIDEO_DEV=/dev/video1 /root/qt_camera_display/run_qt_camera_display.sh
```

## GPU 验证

运行前检查：

```bash
modprobe galcore
ls -l /dev/galcore
```

运行时检查旧 CPU 预览是否已停止：

```bash
ps | grep uvc_fb_preview
```

运行 Qt 后检查 CPU 占用：

```bash
top
```

如果 `/dev/galcore` 不存在，`run_qt_camera_display.sh` 会直接退出，避免 Qt 悄悄退回软件渲染。

## rootfs 运行库要求

目标 rootfs 需要包含以下 Qt/GStreamer 运行组件：

| 组件 | 用途 |
|---|---|
| `libQt5Core.so.5`、`libQt5Gui.so.5`、`libQt5Qml.so.5`、`libQt5Quick.so.5` | Qt Quick 界面 |
| `plugins/platforms/libqeglfs.so` 或 `libqwayland-egl.so` | GPU 显示平台 |
| `plugins/egldeviceintegrations/libqeglfs-viv-integration.so` | Vivante/galcore EGLFS 集成 |
| `/dev/video0`、V4L2 头文件对应内核接口 | 自定义 V4L2 摄像头输入 |

如果你当前还是精简 Buildroot rootfs，只装了 `/root/uvc-rootfs` 里的 GStreamer 工具，那么还需要换成带 Qt/Wayland 的 rootfs，或把 Qt5 runtime 正确打进 Buildroot。
