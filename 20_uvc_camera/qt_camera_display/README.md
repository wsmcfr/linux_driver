# STM32MP157 Qt GPU 摄像头界面

这个目录提供一个最小可落地的 Qt Quick 工业检测主界面：左侧为功能导航，中间显示 UVC 摄像头实时画面，右侧显示检测结果，底部显示统计和控制按钮。

| 项目 | 当前实现 |
|---|---|
| UI 框架 | Qt Quick / QML |
| 摄像头输入 | 自定义 `V4L2VideoItem`，默认 `/dev/video0` |
| 视频显示 | 安全预览使用 V4L2 YUYV 帧上传到 OpenGL ES 纹理；正式路线使用 KMS overlay plane 显示视频 |
| GPU 路径 | `galcore` + OpenGL ES + `eglfs` 或 `wayland-egl` |
| 目标分辨率 | 1024x600 |
| 当前检测逻辑 | 只显示界面和实时画面，检测结果为演示占位 |
| SD 卡按钮 | 右侧面板提供 `保存图片` 测试按钮和 `安全卸载` 正式按钮；保存图片会后台生成 JPG/PNG 并触发 COS 上传，保存期间只禁止重复保存和安全卸载，不阻塞其它页面触摸。 |
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
| 上传历史路线 | 每次点击 `保存图片` 后，Qt 控制器在后台执行本地保存和 COS 上传，完成后回到 Qt 主线程追加本次 JPG/PNG、本地上传时间、云端记录号和上传状态到 `/mnt/sdcard/images/upload_history.json`；历史记录页横向滑动显示；历史卡片的 `删除` 会同步删除该条 JSON 记录和对应 JPG/PNG 图片文件。 |
| 统计分析路线 | 左侧 `统计分析` 页面直接读取本地 `uploadHistory` 模型，汇总总记录、良品/待复核、上传成功率、图片数量、文件大小、最近保存趋势和云端状态；最近记录卡片内支持竖向滑动查看更多记录；当前统计基于保存/上传历史，不代表真实模型缺陷类型已经接入。 |
| 手动控制路线 | 左侧 `手动控制` 页面提供传送带、机械臂、光源、安全状态、人工复核和命令日志；第一版只做 UI 模拟状态、安全置灰和保存当前帧复用，不绕过 STM32F4 直接控制硬件。 |
| 参数设置路线 | 左侧 `参数设置` 页面提供工艺判定、视觉居中、输送分拣、相机光源、SD/COS 策略和参数摘要；第一版只维护 QML 本地状态与提示，真实 JSON 持久化和 `CMD_PARAM_SYNC` 下发 F4 后续接 C++ 控制器。 |
| 告警维护路线 | 左侧 `告警维护` 页面提供当前告警、设备健康、告警历史、处理建议、确认/清故障/刷新/保存诊断入口；告警本身第一版仍使用 QML 模拟状态，`保存诊断` 已通过 C++ 控制器真实写入 `/mnt/sdcard/logs/qt_alarm_snapshot.txt` 并执行 `fsync`，真实 `ALARM/STS_SENSOR/HEARTBEAT` 后续由串口控制器覆盖。 |
| 开机启动动画 | `fb_boot_splash` 先在 Qt/GPU 启动前直接写 `/dev/fb0` 显示静态首帧；QML 顶层 `splashOverlay` 随后显示工业检测自检动画，包含相机扫描窗口、ROI 框、阶段状态和进度条；`uvc_kms_overlay` 启动时先隐藏视频层，Qt splash 完全淡出后再恢复摄像头画面，避免摄像头先于 Qt 界面出现。 |
| 保底视频路线 | 自定义 `V4L2VideoItem` 安全预览和 `VIDEO_BACKEND=qt-gst` mmap 桥接可演示，但不是最终零拷贝目标。 |
| 禁用结论 | QtMultimedia `Camera + VideoOutput` 和 `qmlglsink + UVC DMABUF` 不能作为稳定路线，前者触发内核 Oops，后者在 Vivante `libGAL.so` 中崩溃。 |

## 修改文件清单

| 路径 | 修改原因 |
|---|---|
| `20_uvc_camera/qt_camera_display/main.cpp` | 注册 `V4L2VideoItem`，向 QML 注入摄像头节点、采集尺寸、视频后端、GStreamer 状态等上下文参数；SD 卡动作控制器负责请求 overlay 保存当前帧、调用 `defect-cos-upload` 上传 JPG/PNG、调用 `sdcard-safe-remove`，并通过 `saveAlarmSnapshotToSdCard()` 把告警诊断快照写入 `/mnt/sdcard/logs/qt_alarm_snapshot.txt`；新增 `requestSaveCurrentFrameToSdCard()`、`saveInProgress` 和 `saveCurrentFrameFinished`，让 QML 保存图片走后台线程，避免本地保存和 COS 上传阻塞主界面触摸；提供 `--storage-self-test` 和 `--alarm-snapshot-self-test` 两个 SSH 自检入口；`UploadHistoryModel` 把每次上传结果追加到 `/mnt/sdcard/images/upload_history.json` 并暴露给 QML 历史页，同时提供 `removeRecord()` 删除历史记录和对应 JPG/PNG 图片文件。 |
| `20_uvc_camera/qt_camera_display/qml/Main.qml` | 实现 1024x600 工业检测界面、状态栏、结果面板、统计区、`开始/暂停/继续/停止` 触摸按钮，以及 `保存图片`、`安全卸载` 两个 SD 卡按钮；`保存图片` 调用异步 `requestSaveCurrentFrameToSdCard()`，保存期间显示 `保存中...`，只禁用存储按钮，左侧导航、历史、统计、手动、参数和告警页面仍可触摸；新增 `splashOverlayVisible`、`splashStageModel`、`advanceSplashStage()` 和 `finishSplashAnimation()`，启动时显示工业检测自检动画，使用相机扫描窗口、ROI 框、阶段状态和进度条替代启动空窗；`历史记录` 页面拆成第一层上传列表和第二层记录详情，第一层横向显示每次上传时间卡片并提供 `删除`、`查看` 按钮，滑动参数已调软以减少卡顿跳页感；点选卡片只改变高亮，不绑定 `ListView.currentIndex`，避免列表自动从开头滚到选中记录；详情页左侧横向滑动查看 JPG/PNG，右侧显示检测结果、云端摘要、文件大小、检测信息和本地路径；`统计分析` 页面复用上传历史模型，显示 KPI、最近保存趋势、结果分布、云端与文件状态、最近记录表，最近记录表在卡片内竖向滑动查看更多记录，并支持从最近记录跳转历史详情；`手动控制` 页面新增 `manualPageVisible`、`manualCommandLog` 和 `handleManualAction()`，提供手动模式、安全联锁、模拟 ACK、命令日志和保存当前帧入口，后续可接 C++ `MotionController`；`参数设置` 页面新增 `settingsPageVisible`、工艺/视觉/运动/存储参数和 `settingsApplyAction()`，第一版只做本地 UI 调整、摘要和提示；`告警维护` 页面新增 `alarmPageVisible`、`alarmHistoryModel`、`alarmSnapshotText()` 和 `handleAlarmAction()`，显示当前告警、设备健康、维护日志和处理建议，并把 `保存诊断` 的真实落盘结果显示到提示条。 |
| `20_uvc_camera/qt_camera_display/qml/GstVideoSurface.qml` | 为 `qmlglsink` 桥接路线提供 QML 视频承载面。 |
| `20_uvc_camera/qt_camera_display/v4l2_video_item.h` | 定义 Qt Quick 自定义视频 Item、采集线程状态和 QML 可配置属性。 |
| `20_uvc_camera/qt_camera_display/v4l2_video_item.cpp` | 实现 V4L2 mmap 采集、YUYV 帧上传、OpenGL ES shader 转换、状态回传和安全预览渲染。 |
| `20_uvc_camera/qt_camera_display/qt_camera_display.pro` | 维护 Qt 工程、QML 资源、C++ 源文件和链接依赖。 |
| `20_uvc_camera/qt_camera_display/qml.qrc` | 把 QML 资源打进 Qt 程序，部署时不依赖散落的 QML 文件。 |
| `20_uvc_camera/qt_camera_display/build_qt_camera_display.sh` | 在虚拟机中自动加载 ST Qt/Wayland SDK 环境并交叉编译 Qt 程序。 |
| `20_uvc_camera/qt_camera_display/deploy_qt_camera_display.sh` | 把 Qt 二进制、脚本、QML/资源、overlay 工具、`defect-cos-upload` 上传脚本、`S05display-quiet` 和 `S90uvc-camera` 部署到 NFS rootfs；部署环境提供 `CLOUD_ACCOUNT/CLOUD_PASSWORD` 时，会在 rootfs 中生成本地私有的 `cos-upload.env`。 |
| `20_uvc_camera/qt_camera_display/install_qt_runtime_from_sdk.sh` | 从 ST SDK sysroot 复制 Qt5、QML、eglfs/wayland 插件和 Vivante EGL/GLES 运行库到 rootfs。 |
| `20_uvc_camera/qt_camera_display/run_qt_camera_display.sh` | 板端 Qt 主运行脚本，处理 eglfs/wayland、Goodix 触摸输入、旧预览停止、后端选择和日志。 |
| `20_uvc_camera/qt_camera_display/probe_zero_copy_video_path.sh` | 板端零拷贝候选路线探测脚本，收集 `/dev/video*`、DRM、GStreamer 和 CPU/Oops 证据。 |
| `20_uvc_camera/qt_camera_display/uvc_kms_overlay.c` | 正式化 KMS overlay 视频进程，使用 V4L2 mmap 采集、NEON `YUYV -> ARGB8888` 转换和 `drmModeSetPlane` 上屏；Unix socket 控制端点支持 `SAVE_DUAL`，把同一帧保存为 SD 卡 JPG/PNG 图片并 `fsync`；新增 `VISIBLE 0/1` 命令和 `-V 0|1` 初始可见性参数，开机时可先隐藏实时视频 plane，避免盖住启动动画和历史图片。 |
| `20_uvc_camera/qt_camera_display/build_uvc_kms_overlay.sh` | 交叉编译 `uvc_kms_overlay`，链接 libdrm、libjpeg、libpng、zlib 并生成板端可执行文件；脚本使用 `OVERLAY_CC` 作为专用覆盖变量，避免已经 `source` ST Qt SDK 后的 `CC="编译器 参数"` 污染 overlay 构建。 |
| `20_uvc_camera/qt_camera_display/fb_boot_splash.c` | 新增早期静态启动首帧绘制器，直接 mmap `/dev/fb0` 绘制深色背景、检测窗口、ROI 框、扫描线、英文标题和 18% 初始进度；不依赖 Qt、OpenGL、DRM、图片解码库或摄像头。 |
| `20_uvc_camera/qt_camera_display/build_fb_boot_splash.sh` | 交叉编译 `fb_boot_splash` 并生成 `build-mp157/fb_boot_splash`；脚本使用 `SPLASH_CC` 作为专用编译器变量，避免 ST Qt SDK 的 `CC` 污染非 Qt 辅助程序构建。 |
| `20_uvc_camera/qt_camera_display/defect-cos-upload` | 板端 COS 上传脚本，先读取 `/root/qt_camera_display/cos-upload.env` 作为默认上传账号，JPG 按 `file_kind=source` 上传，PNG 按 `file_kind=annotated` 上传，并把两个文件登记到同一条检测记录。 |
| `20_uvc_camera/qt_camera_display/run_qt_kms_overlay_display.sh` | 板端 Qt + KMS overlay 生命周期脚本，支持 `start/stop/restart/status/restore-fallback`；启动 Qt 前调用 `fb_boot_splash` 先显示静态首帧，再以隐藏状态启动 Qt UI 和 overlay 视频，等 QML 启动动画完成后由 Qt 发送 `VISIBLE 1` 恢复摄像头画面。 |
| `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh` | 静态检查 overlay 源码、构建脚本、运行脚本、触摸按钮、SD 卡按钮、历史记录模型/页面、统计分析页面、手动控制页面、参数设置页面、告警维护页面、告警诊断快照落盘接口、早期静态首帧绘制器、启动动画覆盖层和默认启动契约。 |
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
| `.trellis/spec/frontend/component-guidelines.md` | 沉淀 Qt/Wayland SDK 编译约定、Goodix 触摸输入、KMS overlay UI 按钮验收规则，以及 JPG/PNG 保存和 COS 上传的跨层契约。 |
| `docs/plans/2026-05-03-qt-sdcard-camera-actions.md` | 记录本次 SD 卡保存图片和安全卸载按钮的实施方案、数据路径和验证步骤。 |
| `docs/plans/2026-05-07-qt-cos-jpg-png-save-design.md` | 记录保存按钮双格式落盘和 COS 上传设计。 |
| `docs/plans/2026-05-07-qt-cos-jpg-png-save-implementation.md` | 记录 JPG/PNG 保存和 COS 上传实施计划、测试步骤和验证边界。 |

## 使用流程

| 阶段 | 命令 | 预期结果 |
|---|---|---|
| 加载 Qt SDK 并构建 | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ./build_qt_camera_display.sh` | 生成 `build-mp157/qt_camera_display`。 |
| 构建 KMS overlay 工具 | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ./build_uvc_kms_overlay.sh` | 生成 `build-mp157/uvc_kms_overlay`；即使当前 shell 已经加载 ST Qt SDK，也不能继承 SDK 的 `CC`，需要覆盖编译器时使用 `OVERLAY_CC=/path/to/gcc ./build_uvc_kms_overlay.sh`。 |
| 构建早期静态首帧工具 | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ./build_fb_boot_splash.sh` | 生成 `build-mp157/fb_boot_splash`；即使当前 shell 已经加载 ST Qt SDK，也不能继承 SDK 的 `CC`，需要覆盖编译器时使用 `SPLASH_CC=/path/to/gcc ./build_fb_boot_splash.sh`。 |
| 静态契约检查 | `./test_qt_kms_overlay_assets.sh` | 输出 `PASS: Qt KMS overlay assets contract`。 |
| 部署到 NFS rootfs | `sudo ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` | 板端 `/root/qt_camera_display/` 获得 Qt 程序、overlay 工具、`fb_boot_splash` 和运行脚本，`/etc/init.d/` 获得 `S05display-quiet` 和 `S90uvc-camera`。 |
| 部署默认上传账号 | `CLOUD_ACCOUNT='<账号>' CLOUD_PASSWORD='<密码>' sudo -E ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` | 额外生成 `/home/cfr/linux/nfs/rootfs/root/qt_camera_display/cos-upload.env`，权限为 `600`；保存按钮后续可不再手工传账号密码。 |
| 安装 Qt runtime | `./install_qt_runtime_from_sdk.sh /home/cfr/linux/nfs/rootfs` | rootfs 获得 Qt5 库、QML 模块、eglfs/wayland 插件和 Vivante 库。 |
| 板端启动正式路线 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart` | Qt UI 与 overlay 视频同时运行。 |
| 板端查看状态 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh status` | 显示 Qt PID、overlay PID、fallback PID、plane、控制 socket 和日志路径。 |
| 早期静态首帧观察 | 开发板 SSH | `/root/qt_camera_display/fb_boot_splash -f /dev/fb0; echo $?` | 命令返回 `0`，LCD 立即显示深色工业检测静态首帧，包含英文标题、检测窗口、ROI 框、扫描线和 18% 初始进度；不依赖 Qt、GPU、摄像头或 overlay。 |
| 开机动画观察 | 开发板重启或执行 `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart` 后观察 LCD | 先看到 `fb_boot_splash` 静态首帧，然后切换为 QML `工业缺陷检测系统`、`STM32MP157 Vision Inspection Terminal`、相机扫描窗口、`加载相机/初始化检测模型/连接运动控制/挂载存储/进入检测界面` 和进度条，随后淡出进入首页并恢复摄像头画面；摄像头画面不应抢在 Qt 启动画面前出现，画面中不出现 Ubuntu 企鹅图标。 |
| SSH 保存自检 | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 '/root/qt_camera_display/qt_camera_display --storage-self-test'` | 不启动 QML，直接复用 Qt 保存控制器请求 overlay 保存 JPG/PNG，并尝试调用 `defect-cos-upload`。 |
| SSH 告警诊断自检 | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 '/root/qt_camera_display/qt_camera_display --alarm-snapshot-self-test && test -s /mnt/sdcard/logs/qt_alarm_snapshot.txt && tail -n 30 /mnt/sdcard/logs/qt_alarm_snapshot.txt'` | 不启动 QML，直接复用 Qt 告警诊断控制器写 `/mnt/sdcard/logs/qt_alarm_snapshot.txt`；输出 `诊断已保存：...` 且 `tail` 能看到 `alarm_code=0x0007`。 |
| 按钮保存当前帧 | 开发板屏幕点击 `保存图片` | 按钮立即显示 `保存中...`，底部提示条先显示 `正在保存图片...`；后台完成后显示 `保存成功：JPG /mnt/sdcard/images/uvc_*.jpg PNG /mnt/sdcard/images/uvc_*.png；上传成功/上传失败...`，图片来自 KMS overlay 当前显示帧。 |
| 保存期间界面响应 | 开发板屏幕 | 点击 `保存图片` 后，在仍显示 `保存中...` 时立即点击左侧 `历史记录`、`统计分析`、`手动控制`、`参数设置` 或 `告警维护` | 页面应立即切换或响应触摸；只有 `保存图片` 和 `安全卸载` 两个存储按钮暂时不可点。 | 若页面切换仍卡住，检查 QML 是否又直接调用 `storageController.saveCurrentFrameToSdCard()`；若能重复保存，检查 `saveInProgress`、`saveImageBusy` 和 `requestSaveCurrentFrameToSdCard()`。 |
| 查看上传历史 | 开发板屏幕点击左侧 `历史记录`，横向滑动上传时间卡片并点击 `查看` | 实时视频 plane 被临时隐藏；第一层只显示上传记录列表，滑动应连续跟手，不应轻触后突然跳过多条；点击 `查看` 后进入详情页；详情页左侧可横向滑动 JPG/PNG，右侧显示检测结果、云端摘要、文件大小和检测信息。 |
| 查看统计分析 | 开发板屏幕点击左侧 `统计分析` | 实时视频 plane 被临时隐藏；页面显示总记录、良品/待复核、上传成功率、图片总量、最近保存趋势、结果分布、云端与文件状态、最近记录表；最近记录卡片可上下滑动查看更多记录，点击任一记录行可进入对应历史详情页。 |
| 打开手动控制 | 开发板屏幕点击左侧 `手动控制` | 实时视频 plane 被临时隐藏；页面显示传送带、机械臂、光源、安全状态、人工复核和命令日志；未进入手动模式时运动按钮置灰或提示先进入手动模式。 |
| 手动页保存当前帧 | 开发板屏幕点击 `手动控制`，再点击 `保存当前帧` | 复用现有 `保存图片` 链路，生成 `/mnt/sdcard/images/uvc_*.jpg` 和 `.png`，并在命令日志记录本次辅助动作。 |
| 打开参数设置 | 开发板屏幕点击左侧 `参数设置` | 实时视频 plane 被临时隐藏；页面显示工艺与判定、视觉定位、输送与分拣、相机光源与存储、参数摘要和操作按钮。 |
| 调整参数设置 | 开发板屏幕点击 `参数设置` 页中的 `+/-`、`切换`、`应用参数`、`保存配置`、`恢复默认` | 界面参数值、参数摘要和底部提示条同步变化；第一版不写真实配置文件，不向 F4 下发参数。 |
| 打开告警维护 | 开发板屏幕点击左侧 `告警维护` | 实时视频 plane 被临时隐藏；页面显示当前告警 `0x0007`、设备健康矩阵、告警历史和处理建议。 |
| 告警维护操作 | 开发板屏幕点击 `确认`、`清故障`、`刷新状态`、`保存诊断` | 当前告警状态或维护日志更新；点击 `保存诊断` 时底部提示条显示 `诊断已保存：/mnt/sdcard/logs/qt_alarm_snapshot.txt` 或明确失败原因；第一版不代表真实 F4 告警已经清除。 |
| SSH 查看告警诊断快照 | 开发板屏幕点击 `保存诊断` 后，或执行 `--alarm-snapshot-self-test` 后，在 SSH 执行 `test -s /mnt/sdcard/logs/qt_alarm_snapshot.txt; wc -c /mnt/sdcard/logs/qt_alarm_snapshot.txt; tail -n 30 /mnt/sdcard/logs/qt_alarm_snapshot.txt` | 文件存在且大小大于 0，内容包含 `STM32MP157 Qt Alarm Snapshot`、`alarm_code=0x0007`、`camera_status=`、`storage_state=` 和 `[recent_alarm_history]`。 |
| 删除上传历史 | 开发板屏幕点击左侧 `历史记录`，在某条卡片点击 `删除` | 该条记录从 `/mnt/sdcard/images/upload_history.json` 中移除，对应 JPG/PNG 文件同步删除；如果删除最后一条，历史页显示 `暂无上传记录`。 |
| 返回实时首页 | 历史页/统计页/手动页/参数页/告警页右上点击 `返回首页` 或左侧点击 `首页` | KMS overlay 实时视频 plane 恢复显示，回到实时检测布局。 |
| 按钮安全卸载 | 开发板屏幕点击 `安全卸载` | 底部提示条短暂显示卸载结果；脚本同步并卸载 SD 卡，提示后才能拔卡。 |
| 板端恢复兜底线 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restore-fallback` | 停止正式路线，恢复 `640x480@10fps -> drop6 -> BGRA -> kmssink` 可见线。 |
| 安全预览运行 | `/root/qt_camera_display/run_qt_camera_display.sh` | 使用默认 `320x240@10fps` V4L2 安全预览。 |
| 提高画质测试 | `CAMERA_WIDTH=640 CAMERA_HEIGHT=480 CAMERA_FPS=15 /root/qt_camera_display/run_qt_camera_display.sh` | 可观察较高画质，但 CPU 明显升高。 |

## 修改记录

| 时间 | 修改点 | 结果 |
|---|---|---|
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
| 2026-05-16 | 新增手动控制界面 | 左侧 `手动控制` 导航正式可点击；页面提供传送带点动、机械臂回零/夹取/放置、背光常亮状态、补光亮度、人工复核、安全状态和命令日志；第一版只更新 QML 模拟状态和安全提示，真实运动仍等待后续 `MotionController` 接入 STM32F4。 |
| 2026-05-16 | 修正手动页硬件显示 | 背光源按实际硬件改为常亮只读状态，不再显示可控开/关按钮；机械臂末端执行器统一显示为夹爪，不再出现旧的错误末端执行器文案。 |
| 2026-05-16 | 新增参数设置界面 | 左侧 `参数设置` 导航正式可点击；页面提供零件类型、良坏阈值、复核阈值、精定位阈值、稳定帧数、脉冲标定、低速档、分拣超时、相机/光源/SD/COS 状态和参数摘要；第一版只做 QML 本地状态和提示，不写真实 JSON、不向 F4 下发。 |
| 2026-05-16 | 新增告警维护界面 | 左侧 `告警维护` 导航正式可点击；页面提供当前告警 `0x0007`、设备健康矩阵、告警历史、处理建议和确认/清故障/刷新/保存诊断入口；第一版使用模拟告警和维护日志，真实告警帧后续由串口控制器接入。 |
| 2026-05-16 | 压缩参数页和告警页布局 | 针对 1024x600 板端截图中文字超出问题，缩短顶部摘要、参数说明、设备健康和处理建议文案；给设置/告警卡片增加 `clip`，修正告警历史行固定列宽，按钮网格改为按父容器计算宽度，避免中文字体差异导致右侧越界。 |
| 2026-05-16 | 调整当前告警操作区 | 根据板端拍屏反馈，当前告警卡片底部三个按钮挤占发生时间和处理状态区域；已把按钮移到卡片右侧纵向操作列，左侧只保留告警码、标题和状态字段，右侧设备健康卡片同步缩窄。 |
| 2026-05-16 | 新增开机启动动画 | `Main.qml` 顶层新增 `splashOverlay`，启动时显示工业检测自检动画：相机扫描窗口、ROI 框、缺陷框、五段启动状态和进度条；约 3.5 秒后淡出进入原主界面，用于承接内核企鹅 logo 隐藏后的空白阶段。 |
| 2026-05-16 | 修复摄像头画面抢在 Qt 启动画面前出现 | `uvc_kms_overlay` 新增 `-V 0|1` 初始可见性参数；`run_qt_kms_overlay_display.sh` 改为先隐藏启动 overlay，再启动 Qt；`Main.qml` 在 splash 完全关闭后发送 `VISIBLE 1`，保证启动动画先于实时视频层出现。 |
| 2026-05-16 | 增加早期显示静默兜底 | 新增 `S05display-quiet`，部署脚本同步安装到 rootfs `/etc/init.d/`，用于尽早关闭 fbcon 光标和清理 `/dev/tty0` 残留；彻底消除最早期 `_` 仍建议在内核 bootargs 增加 `vt.global_cursor_default=0`。 |
| 2026-05-16 | 增加早期静态首帧 | 新增 `fb_boot_splash.c` 和 `build_fb_boot_splash.sh`，部署到 `/root/qt_camera_display/fb_boot_splash`；`S05display-quiet`、`S90uvc-camera` 和 `run_qt_kms_overlay_display.sh` 会在 Qt 启动前调用它，先显示启动动画第一帧风格的静态图。 |
| 2026-05-16 | 修复告警保存诊断只显示路径但不生成文件 | 根因为 `handleAlarmAction("snapshot")` 只把 `快照目标：/mnt/sdcard/logs/qt_alarm_snapshot.txt` 写到界面提示，没有调用任何文件写入逻辑；现在 QML 通过 `alarmSnapshotText()` 组装诊断文本，C++ `saveAlarmSnapshotToSdCard()` 检查 `/mnt/sdcard` 挂载、创建 `/mnt/sdcard/logs`、覆盖写入 `qt_alarm_snapshot.txt` 并 `fsync`。 |
| 2026-05-16 | 修复保存图片期间界面无法点击其它页面 | 根因为 QML 直接同步调用 `saveCurrentFrameToSdCard()`，本地保存、COS 上传和历史记录处理会占住 Qt 主线程；现在 QML 调用 `requestSaveCurrentFrameToSdCard()` 后立即返回，C++ 后台线程完成保存和上传，结束后通过 `saveCurrentFrameFinished` 回填提示并在主线程追加历史记录。 |

## 硬件资源

| 资源 | 用途 | 注意事项 |
|---|---|---|
| RGB LCD 1024x600 | Qt Quick 主界面显示 | QML 根对象固定按 1024x600 设计。 |
| UVC 摄像头 `/dev/video0` | 摄像头画面输入 | KMS overlay 正式路线由 `uvc_kms_overlay` 独占该节点，Qt UI 只显示状态。 |
| `/dev/galcore` | Qt eglfs/OpenGL ES GPU 渲染 | 启动前必须 `modprobe galcore` 成功。 |
| DRM overlay plane 36 | 正式视频显示平面 | 默认矩形为 `177,73,640,480`，由运行脚本环境变量可调。 |
| Goodix 触摸 `/dev/input/eventX` | Qt 按钮触摸输入 | 运行脚本会自动选择触摸节点并配置 `evdevtouch`。 |
| SD 卡 `/mnt/sdcard` | 保存测试图片和安全卸载 | 保存按钮只允许写入 `/mnt/sdcard/images`；安全卸载必须走 `sdcard-safe-remove`。 |
| Unix socket `/tmp/uvc-kms-overlay-control.sock` | Qt UI 请求 overlay 保存当前帧 | 由 `uvc_kms_overlay` 创建，`run_qt_kms_overlay_display.sh status` 会显示路径。 |
| 上传历史文件 `/mnt/sdcard/images/upload_history.json` | Qt 历史记录页数据源 | 每次保存/上传完成后由 `UploadHistoryModel` 追加写入；记录包含 `upload_time`、`jpg_path`、`png_path`、`record_id`、`record_no`、`upload_status` 和文件大小。 |
| 统计分析页面 | 汇总本地上传历史 | 数据来自 `uploadHistory` 模型，不新增单独数据库；统计页打开时通过 `setOverlayVisible(false)` 隐藏 KMS 视频 plane，返回首页时恢复。 |
| 手动控制页面 | 调试级人工操作界面 | 数据来自 QML 模拟状态和 `manualCommandLog`；手动页打开时通过 `setOverlayVisible(false)` 隐藏 KMS 视频 plane；传送带 PWM、机械臂舵机、急停、限位和联锁仍归 STM32F4，MP157 第一版不直接控制硬件。 |
| 启动动画覆盖层 | 替代启动空窗和展示系统自检 | 数据来自 QML 本地 `splashStageModel`，不是硬件真实自检结果；动画使用 Qt Quick 基础图元和 transform/opacity/y 动画，不加载外部图片或视频资源，避免拖慢板端启动。 |
| 后续 `MotionController` | MP157 到 STM32F4 的高层命令桥 | 当前未实现；后续应把 `handleManualAction()` 中的模拟 ACK 替换为串口命令发送、ACK 等待、超时和错误码映射。 |
| 云端后端 `http://119.91.65.122` | 检测记录创建、COS 预签名上传和文件登记 | `defect-cos-upload` 默认使用该地址，可通过 `CLOUD_BASE_URL` 覆盖。 |
| COS 上传脚本 `/root/qt_camera_display/defect-cos-upload` | 上传 JPG/PNG 到 COS | 需要 curl、后端 Cookie，或本地 `/root/qt_camera_display/cos-upload.env` 中的 `CLOUD_ACCOUNT/CLOUD_PASSWORD`；`CLOUD_DEVICE_ID`、`CLOUD_PART_ID` 可显式覆盖，留空时自动取当前账号第一条可用设备/零件；`captured_at/uploaded_at` 显式使用 `TZ=CST-8` 的北京时间墙钟并追加 `+08:00`，不得在前后端额外手工加减 8 小时。 |
| 默认上传账号配置 `/root/qt_camera_display/cos-upload.env` | 保存按钮自动登录云端 | 只保存在板端/rootfs 本地，不提交 Git；权限必须为 `600`；可通过 `CLOUD_UPLOAD_ENV_FILE` 指定其他板端绝对路径，运行时环境变量优先于该文件。 |
| Qt 进程时区 `TZ=CST-8` | 顶部状态栏显示北京时间 | `main.cpp` 在 Qt 应用创建前设置默认 `TZ` 并调用 `tzset()`；启动脚本也导出 `TZ=CST-8`。如果上传时间正确但界面时间仍是 UTC，优先检查 `/proc/$(pidof qt_camera_display)/environ` 中是否有 `TZ=CST-8`，不要去改 `defect-cos-upload`。 |

## 手动控制联调阶段待办

下面功能先记为后续实机联调任务，当前第一版界面不继续扩大实现范围；等 STM32F4 串口协议、夹爪到位信号、限位/急停输入和动作点位一起调试时再接入。

| 优先级 | 待加功能 | 联调触发条件 | 实现要点 |
|---|---|---|---|
| 高 | F4 在线、超时和 ACK 状态 | MP157 与 STM32F4 串口协议开始实测时 | `MotionController` 负责发送命令、等待 ACK、记录超时；QML 只显示在线/离线、最近 ACK 和错误码。 |
| 高 | 夹爪开合到位反馈 | 夹爪开/关传感器或 F4 状态帧可用时 | 手动页不能只靠按钮点击改状态，应以 F4 回传的“夹爪打开到位/闭合到位/超时”覆盖 `manualActuatorState`。 |
| 高 | 回零前置引导 | 机械臂回零开关、限位或动作组开始联调时 | 未回零时只开放回零、停止、急停、刷新状态；抓取、放良品、放坏品保持禁用并提示原因。 |
| 中 | 复位到安全状态 | F4 已支持停止传送带、机械臂待机、夹爪关闭的组合命令时 | 一键下发安全复位动作组，完成后刷新 F4 状态；失败时保留错误码和禁止继续动作。 |
| 中 | 手动动作二次确认 | 清故障、急停释放、放坏品、模式切换等高风险动作接入真实硬件前 | 用弹窗或确认状态阻止误触；确认后才调用真实命令，取消时只写命令日志。 |
| 中 | 当前帧冻结、保存和人工复核联动 | 检测记录、人工复核结果和图片保存链路需要合并时 | 手动页的 `GOOD/BAD/UNCERTAIN` 后续写入检测记录，保存图片时同时带上复核标记和本次动作上下文。 |
| 低 | 参数设置页 | 速度档位、动作组、补光亮度等参数需要现场可调时 | 参数页只保存目标值；最终限幅、互锁和危险动作拒绝仍由 F4 执行。 |

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

## 待修问题记录

| 日期 | 问题 | 当前证据 | 预期目标 | 下一步优先排查 |
|---|---|---|---|---|
| 2026-05-07 | 单次保存上传的云端记录隔离失败 | 板端 `--storage-self-test` 创建了新记录 `record_id=7/8`，但后续日志显示文件登记仍打到 `/api/v1/records/3/files`，且 COS `object_key` 仍包含旧记录名 `SIM-DIANPIAN-20260420185013`。根因是脚本用通用 JSON `id` 解析函数从创建记录响应里抓到了后面的嵌套 `part.id/device.id=3`。 | 按一次 `保存图片` 后，云端应只关联本次两张图片：JPG 原图 `source` + PNG 标注图 `annotated`；prepare、register、detail 回查都必须使用创建记录返回的新 `record_id`。 | 已在脚本侧修复并新增 `--self-test-json-parser` 回归自检；明天板端验证时重点看 stderr 中 `record_no=...`、`created_record_id=...`、`prepare_object_key=...`、`register_url=/api/v1/records/<新ID>/files`，并确认新记录详情只含 2 张图，旧 `record_id=3` 不再增加。 |

## 新版测试与验证矩阵

| Test goal | Run location | Command | Expected result | Failure triage |
|---|---|---|---|---|
| 静态契约检查 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `./test_qt_kms_overlay_assets.sh` | 输出 `PASS: Qt KMS overlay assets contract`。 | 若失败，按脚本提示检查 QML 按钮、overlay 源码、运行脚本和默认参数。 |
| 上传 JSON 解析回归 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `sh defect-cos-upload --self-test-json-parser` | 构造顶层 `record_id=8`、嵌套 `part.id/device.id=3` 的响应，解析结果仍为 `8`。 | 若失败，说明创建记录响应解析又可能把旧嵌套 ID 当成 record_id。 |
| 上传时间格式回归 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `sh defect-cos-upload --self-test-json-parser` | 同时构造固定墙钟 `2026-05-08T13:07:02`，输出应保持 `2026-05-08T13:07:02+08:00`，不能变成 `21:07:02+08:00`。 | 若失败，说明脚本又对板端墙钟做了时区二次换算。 |
| Qt 程序编译 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `./build_qt_camera_display.sh` | 生成 `build-mp157/qt_camera_display`。 | 若 qmake 缺失，确认脚本已 source ST Qt/Wayland SDK 环境。 |
| overlay 工具编译 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `./build_uvc_kms_overlay.sh` | 生成 `build-mp157/uvc_kms_overlay`，并且在已 `source` ST Qt SDK 的 shell 中也能继续使用 Buildroot 的 overlay 编译器。 | 若 libdrm、libjpeg 或 libpng 缺失，查 Buildroot defconfig 和 sysroot/runtime 部署；若需要自定义编译器，只设置 `OVERLAY_CC`，不要依赖外部 `CC`。 |
| 部署 Qt 文件 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `sudo ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` | `/home/cfr/linux/nfs/rootfs/root/qt_camera_display/` 更新。 | 若权限失败，确认用 `sudo`，并检查 NFS rootfs 路径。 |
| 部署默认上传账号 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `CLOUD_ACCOUNT='<账号>' CLOUD_PASSWORD='<密码>' sudo -E ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs; sudo ls -l /home/cfr/linux/nfs/rootfs/root/qt_camera_display/cos-upload.env` | `ls -l` 输出 `-rw------- root root`。 | 若文件不存在，确认 `sudo -E` 保留了环境变量；若权限不是 `600`，重新部署或手工 `chmod 600`。 |
| GPU 运行前检查 | 开发板 | `modprobe galcore; ls -l /dev/galcore` | `/dev/galcore` 存在。 | 若不存在，查 galcore 模块、内核版本和 `/lib/modules`。 |
| 早期静态首帧 | 开发板 SSH | `/root/qt_camera_display/fb_boot_splash -f /dev/fb0; echo $?; ls -l /root/qt_camera_display/fb_boot_splash /dev/fb0` | `echo $?` 输出 `0`，LCD 显示静态启动图；程序和 `/dev/fb0` 都存在。 | 若命令失败，查二进制是否部署且可执行、`/dev/fb0` 是否存在、framebuffer 位深是否为 16/24/32 bpp；若命令成功但随后黑屏，查后续启动脚本或 Qt 是否覆盖了 framebuffer。 |
| 正式路线启动 | 开发板 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart` | Qt UI 和 overlay 视频同时显示。 | 若黑屏，先运行 `status`，再查 `/tmp/qt_camera_display.log` 和 `/tmp/uvc_kms_overlay.log`。 |
| 启动动画验证 | 开发板屏幕 | 重启开发板或执行 `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart` | 先显示 `fb_boot_splash` 静态首帧，再显示 QML `工业缺陷检测系统` 启动画面、绿色扫描线、ROI 框、五段自检文字和进度条，随后淡出到首页并恢复摄像头画面；摄像头画面不应先于 Qt splash 出现；不再显示 Ubuntu 企鹅图标。 | 若没有静态首帧，先手动运行 `/root/qt_camera_display/fb_boot_splash -f /dev/fb0`；若没有 QML 动画，确认部署的是新 Qt 二进制并检查 `strings /root/qt_camera_display/qt_camera_display | grep splashOverlayVisible`；若摄像头仍抢先出现，确认 `/tmp/uvc-kms-overlay.log` 包含 `initial-visible=0`，`run_qt_kms_overlay_display.sh` 含 `-V 0`；若仍有企鹅，确认内核 `CONFIG_LOGO` 已关闭并重新加载了新的 `uImage`。 |
| 早期光标兜底 | 开发板 SSH | `cat /proc/cmdline; cat /sys/class/graphics/fbcon/cursor_blink; ls -l /etc/init.d/S05display-quiet` | `cursor_blink` 输出 `0`，`S05display-quiet` 存在且可执行；当前 bootargs 若仍无 `vt.global_cursor_default=0`，最早期内核阶段仍可能短暂出现 `_`。 | 若 `cursor_blink` 不是 `0`，手动执行 `/etc/init.d/S05display-quiet start` 并查脚本权限；若要彻底消除最早期 `_`，需要改内核/U-Boot bootargs 增加 `vt.global_cursor_default=0 quiet loglevel=3` 并重新部署 `uImage`。 |
| 正式路线状态 | 开发板 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh status` | 显示 Qt PID、overlay PID、fallback PID、plane 和日志路径。 | 如果只有 Qt 或只有 overlay，说明某一半链路退出，按对应日志排查。 |
| Qt 顶部时钟时区 | 开发板 SSH | `qtpid=$(pidof qt_camera_display); tr '\0' '\n' < /proc/$qtpid/environ | grep '^TZ='; TZ=CST-8 date '+%H:%M:%S %Z'; date -u '+%H:%M:%S UTC'` | 进程环境显示 `TZ=CST-8`，屏幕顶部时间应与 `TZ=CST-8 date` 同小时，且比 `date -u` 快 8 小时。 | 若进程环境没有 `TZ=CST-8`，重新部署 `qt_camera_display` 和两个启动脚本；若环境正确但屏幕仍是 UTC，确认是否运行的是旧二进制或旧 QML 资源。 |
| 安全预览兜底 | 开发板 | `/root/qt_camera_display/run_qt_camera_display.sh` | 以默认 `320x240@10fps` 显示安全预览。 | 若启动失败，查 `/dev/video0`、`/dev/galcore`、Qt runtime 和 QML 模块。 |
| 高画质压力测试 | 开发板 | `CAMERA_WIDTH=640 CAMERA_HEIGHT=480 CAMERA_FPS=15 /root/qt_camera_display/run_qt_camera_display.sh` | 能显示更高画质，同时 CPU 升高。 | 若 CPU 过高，这是安全路径预期瓶颈，不要误判为内核崩溃。 |
| 触摸按钮验证 | 开发板屏幕 | 依次点击 `开始`、`暂停`、`继续`、`停止` | 状态栏文字随动作变化，视频 plane 不被按钮遮挡。 | 若触摸无效，查 Goodix event 节点、`QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS` 和 Qt 输入日志。 |
| 保存按钮前置检查 | 开发板 | `mount | grep ' /mnt/sdcard '; df -h /mnt/sdcard; /root/qt_camera_display/run_qt_kms_overlay_display.sh status` | `/mnt/sdcard` 已挂载，overlay PID 存在，状态输出包含 `/tmp/uvc-kms-overlay-control.sock`。 | 若未挂载，执行 `/etc/init.d/S85sdcard-mount status`；若 socket 不存在，查 `/tmp/uvc-kms-overlay.log`。 |
| Qt 保存控制器自检 | 开发板 SSH | `/root/qt_camera_display/qt_camera_display --storage-self-test` | 输出 `保存成功：JPG /mnt/sdcard/images/uvc_*.jpg PNG /mnt/sdcard/images/uvc_*.png`，后面跟随 `上传成功` 或明确上传失败原因。 | 若本地保存失败，查 C++ 控制器、`/proc/mounts`、目录创建或 overlay socket；若上传失败，查 `defect-cos-upload`、Cookie、网络和 COS prepare。 |
| 保存当前显示帧 | 开发板屏幕和 SSH | 屏幕点击 `保存图片`，然后执行 `ls -lt /mnt/sdcard/images/uvc_*.jpg /mnt/sdcard/images/uvc_*.png | head -n 4` | 最新 JPG 和 PNG 同时存在，底部提示条显示双路径和上传状态。 | 若显示保存失败，按界面状态先查 `/mnt/sdcard`、socket、`storage action save-image` Qt 日志和 overlay 日志。 |
| 图片大小稳定 | 开发板 | `jpg=$(ls -t /mnt/sdcard/images/uvc_*.jpg | head -n 1); png=$(ls -t /mnt/sdcard/images/uvc_*.png | head -n 1); s1=$(wc -c < "$jpg"); p1=$(wc -c < "$png"); sleep 1; s2=$(wc -c < "$jpg"); p2=$(wc -c < "$png"); [ "$s1" = "$s2" ] && [ "$p1" = "$p2" ] && [ "$s1" -gt 0 ] && [ "$p1" -gt 0 ]` | JPG/PNG 文件大小连续两次一致且大于 0。 | 若大小为 0 或还在变化，查 overlay 是否正在写文件、SD 卡是否只读。 |
| 图片内容检查 | 开发板 | `jpg=$(ls -t /mnt/sdcard/images/uvc_*.jpg | head -n 1); png=$(ls -t /mnt/sdcard/images/uvc_*.png | head -n 1); head -c 2 "$jpg"; echo; head -c 8 "$png" | hexdump -C` | JPG 文件头为 JPEG SOI，PNG 文件头为 `89 50 4e 47 0d 0a 1a 0a`。 | 若文件头不正确，说明编码失败或取错文件。 |
| 上传历史文件检查 | 开发板 SSH | `test -s /mnt/sdcard/images/upload_history.json; tail -n 40 /mnt/sdcard/images/upload_history.json` | JSON 中出现最新 `upload_time`、`jpg_path`、`png_path`、`upload_status`，上传成功时还有 `record_id` 和 `record_no`。 | 若文件不存在，先确认 `保存图片` 是否走到 Qt 主进程而不是只手工运行上传脚本；再查 `/tmp/qt_camera_display.log` 中 `upload history save failed`。 |
| 历史页触摸验证 | 开发板屏幕 | 点击左侧 `历史记录`，横向滑动时间卡片，点击某条记录空白区域选中，再继续左右滑动，点击某条记录的 `查看`，再在左侧图片区域左右滑动 | 页面第一层只显示上传记录列表；滑动时间卡片和 JPG/PNG 轮播时应连续跟手，不应轻触后突然跳过多页；点选某条历史卡片只改变该卡片高亮，不应把列表自动从开头或当前位置滑到选中项；点击 `查看` 后进入详情页，详情页左侧能在 JPG/PNG 之间切换，右侧显示检测结果、云端摘要、文件大小和检测信息；点击 `返回列表` 回到第一层。 | 若历史页空白，先查 `/mnt/sdcard/images/upload_history.json`；若点选后列表自动滚动，检查 `Main.qml` 是否又出现 `currentIndex: root.selectedHistoryIndex` 或 `highlightRangeMode: ListView.ApplyRange`；若图片不显示，确认文件路径存在且图片头正确；若无法触摸，查 Goodix 输入配置。 |
| 统计页触摸验证 | 开发板屏幕 | 点击左侧 `统计分析`，观察 KPI、最近保存趋势、分布概览、云端与文件状态、最近记录表；在最近记录卡片内上下滑动，再点击任一记录行 | 统计页打开后实时视频 plane 隐藏；总记录应等于 `upload_history.json` 记录数；良品/待复核、上传成功率、图片数量和文件大小有值；最近记录表能在卡片内竖向滑动查看更多记录；点击最近记录行进入对应历史详情页。 | 若统计页无数据，先查 `/mnt/sdcard/images/upload_history.json` 是否存在且非空；若不能滑动，检查 `statsRecentListView` 是否仍是 `ListView` 且 `statsRecentRows()` 是否只返回 5 条；若点击无反应，检查 `openHistoryDetailFromStats` 和 `showHistoryDetail`；若视频仍覆盖统计页，查 `setOverlayVisible(pageName === "home")` 和 overlay `VISIBLE` 命令。 |
| 参数页布局验证 | 开发板屏幕 | 点击左侧 `参数设置`，依次观察顶部摘要、三张参数卡、下方存储卡和参数摘要卡；点击 `+/-`、`切换`、`应用参数`、`保存配置`、`恢复默认` | 所有文字都在卡片边框内，长路径和摘要只省略不换行溢出；参数按钮不挤出卡片，底部提示条不遮挡其它区域。 | 若仍有文字越界，先查 `Main.qml` 中对应卡片是否缺少 `clip: true`、`elide: Text.ElideRight/ElideMiddle`，再按板端实际字体继续缩短文案或减少同屏字段。 |
| 告警页布局验证 | 开发板屏幕 | 点击左侧 `告警维护`，观察当前告警、设备健康矩阵、告警历史、处理建议；点击 `确认`、`清故障`、`刷新状态`、`保存诊断` | 当前告警按钮在告警卡片右侧纵向排列，不挤占发生时间和处理状态；设备健康 3x3 网格、告警历史每一行和处理建议都不越过卡片边界；长诊断路径和状态文字只省略显示。 | 若当前告警按钮仍压住文字，检查 `alarmCurrentPanel` 是否为左信息右按钮布局；若历史行仍超出，优先检查 `alarmHistoryListView` delegate 的列宽总和与 `spacing`；若设备健康覆盖刷新按钮，检查 `alarmHealthGrid` 单元高度、行距和刷新按钮 `y`。 |
| 告警诊断快照落盘 | 开发板屏幕和 SSH | 屏幕点击 `告警维护` 的 `保存诊断`，或执行 `/root/qt_camera_display/qt_camera_display --alarm-snapshot-self-test`，再执行 `test -s /mnt/sdcard/logs/qt_alarm_snapshot.txt; wc -c /mnt/sdcard/logs/qt_alarm_snapshot.txt; tail -n 30 /mnt/sdcard/logs/qt_alarm_snapshot.txt` | 文件存在且非空，`tail` 能看到 `snapshot_time=`、`alarm_code=0x0007`、`alarm_status=`、`camera_status=` 和 `[recent_alarm_history]`；按钮提示为真实 C++ 返回值。 | 若目录为空，先查屏幕提示或自检输出是否为 `诊断保存失败`；再查 `/tmp/qt_camera_display.log` 中 `storage action alarm-snapshot`、`mount | grep ' /mnt/sdcard '`、`df -h /mnt/sdcard` 和 SD 卡是否只读。 |
| 手动页入口验证 | 开发板屏幕 | 点击左侧 `手动控制` | 手动页显示传送带、机械臂、光源与辅助、安全状态、人工复核和命令日志；实时 KMS 视频 plane 不覆盖页面。 | 若无法进入，检查 `switchPage()` 是否允许 `manual`，左侧导航点击分支是否包含 `manual`；若视频覆盖页面，查 `setOverlayVisible(pageName === "home")` 和 overlay `VISIBLE` 命令。 |
| 手动页安全置灰 | 开发板屏幕 | 未点击 `进入手动` 时观察并点击 `正向点动`、`抓取测试`、`放良品` | 运动按钮应置灰；若触摸事件仍到达保护分支，应提示 `请先进入手动模式` 并写入命令日志。 | 若未进手动仍能执行运动状态，检查 `manualActionAllowed()` 和 `handleManualAction()` 的 `manualMode` 保护分支。 |
| 手动模式切换 | 开发板屏幕 | 点击 `进入手动`，再点击传送带 `正向点动`、`停止` | 顶部模式显示 `手动`；传送带状态显示 `正向点动` 后回到 `停止`；命令日志最新在上。 | 若模式不变，查 `manualMode` 绑定；若日志不更新，查 `manualCommandLog.insert(0, ...)`。 |
| 机械臂回零联锁 | 开发板屏幕 | 进入手动后先点击 `抓取测试`，再点击 `回零`，再点击 `抓取测试` | 未回零时抓取类按钮置灰或提示 `机械臂未回零`；回零后抓取测试可更新为 `抓取测试/吸取`。 | 若未回零也能抓取，检查 `manualArmHomeOk` 和抓取/放置动作的联锁条件。 |
| 手动页保存当前帧 | 开发板屏幕和 SSH | 手动页点击 `保存当前帧`，再执行 `ls -lt /mnt/sdcard/images/uvc_*.jpg /mnt/sdcard/images/uvc_*.png | head -n 4` | JPG/PNG 最新文件生成，底部提示沿用保存链路，命令日志出现 `已请求保存当前帧`。 | 若图片未生成，按保存按钮链路排查 `/mnt/sdcard`、overlay socket、`saveCurrentFrameToSdCard()` 和 `/tmp/qt_camera_display.log`。 |
| 历史页删除验证 | 开发板屏幕和 SSH | 删除前记录 `jpg=$(tail -n 40 /mnt/sdcard/images/upload_history.json | sed -n 's/.*\"jpg_path\": \"\\([^\"]*\\)\".*/\\1/p' | tail -n 1); png=$(tail -n 40 /mnt/sdcard/images/upload_history.json | sed -n 's/.*\"png_path\": \"\\([^\"]*\\)\".*/\\1/p' | tail -n 1); ls -lh \"$jpg\" \"$png"`，屏幕点击对应历史卡片 `删除`，再执行 `test ! -e \"$jpg\" && test ! -e \"$png\"; grep -F \"$jpg\" /mnt/sdcard/images/upload_history.json || true` | 被删记录对应 JPG/PNG 文件不存在，历史 JSON 不再包含该 JPG 路径；界面自动选中相邻记录或显示 `暂无上传记录`。 | 若 JSON 仍包含路径，查 `/tmp/qt_camera_display.log` 中 `upload history remove save failed`；若 JSON 删除但图片还在，查日志里的 `upload history image remove failed` 或路径是否不在 `/mnt/sdcard/images`。 |
| 历史页 overlay 遮挡检查 | 开发板屏幕和 SSH | 进入历史页后观察实时视频是否消失；必要时执行 `printf 'VISIBLE 0\n' | nc -U /tmp/uvc-kms-overlay-control.sock` 和 `printf 'VISIBLE 1\n' | nc -U /tmp/uvc-kms-overlay-control.sock` | 历史页打开时实时 KMS 视频 plane 不遮挡历史图片，返回首页后实时视频恢复。 | 若 `nc -U` 不存在，可只用屏幕行为验收；若历史页仍被视频覆盖，查 `uvc_kms_overlay` 是否为新版并支持 `VISIBLE` 命令。 |
| 默认账号配置检查 | 开发板 SSH | `ls -l /root/qt_camera_display/cos-upload.env; grep ^CLOUD_ /root/qt_camera_display/cos-upload.env | cut -d= -f1` | `ls -l` 输出 `-rw------- root root`，并只列出 `CLOUD_ACCOUNT`、`CLOUD_PASSWORD` 两个变量名。 | 若文件不存在或不可读，重新部署默认账号配置；不要把账号密码写进源码或日志。 |
| COS 上传脚本自检 | 开发板 SSH | `/root/qt_camera_display/defect-cos-upload --jpg "$jpg" --png "$png"` | stdout 输出 `上传成功：record_id=... record_no=... jpg_kind=source png_kind=annotated`；stderr 打印 `record_no`、`created_record_id`、`prepare_object_key`、`register_url`，且两次 `register_url` 都是新记录 ID；详情页时间应接近 `TZ=CST-8 date` 显示的北京时间。 | 若登录失败，查 `cos-upload.env`、Cookie；若自动查询失败，手动设置 `CLOUD_PART_ID/CLOUD_DEVICE_ID`；若提示 `object_key 未包含当前 record_no`，说明 prepare 仍落到旧记录路径，停止继续登记；若详情时间偏 8 小时，查 `board-time-sync status` 和脚本是否导出 `TZ=CST-8`。 |
| COS 记录回查 | 开发板 SSH | `record_id=<上一步ID>; curl -fsS -b /tmp/cloud_cookie.txt "http://119.91.65.122/api/v1/records/$record_id"` | 返回记录详情，`files` 中只包含本次 `source` 和 `annotated` 两类文件，并有 `preview_url`；旧 `record_id=3` 不再新增文件；`captured_at/uploaded_at` 与 `TZ=CST-8 date` 同小时。 | 若 `files` 为空或数量不是 2，脚本会返回上传失败；优先查脚本输出的 `created_record_id`、`prepare_object_key` 和后端 `/records/<id>/files` 日志；若时间偏 8 小时，先看 `board-time-sync status`、板端 `/tmp/defect-cos-record.json` 和 `/tmp/defect-cos-register-*.json` 原始值。 |
| 安全卸载按钮 | 开发板屏幕和 SSH | 屏幕点击 `安全卸载`，再执行 `/etc/init.d/S85sdcard-mount status` | 界面显示卸载完成，状态显示未挂载或等待拔卡。 | 若卸载失败，查是否有进程占用 `/mnt/sdcard`，不要直接拔卡。 |
| 零拷贝候选探测 | 开发板 | `VIDEO_DEV=/dev/video0 CAMERA_WIDTH=640 CAMERA_HEIGHT=480 CAMERA_FPS=15 /root/qt_camera_display/probe_zero_copy_video_path.sh` | 在 `logs/zero-copy-probe-*.log` 生成探测日志。 | 若某路线失败，保留日志，不要重新启用已知崩溃的 QtMultimedia `Camera + VideoOutput`。 |

## 读写/数据路径验证

| 数据路径 | 读验证 | 写验证 | 通过标准 |
|---|---|---|---|
| Qt 运行资源 | `ls /root/qt_camera_display/qt_camera_display /usr/lib/libQt5Core.so.5` | `deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` | 板端程序和 Qt runtime 同时存在。 |
| 早期静态首帧 | `ls -l /root/qt_camera_display/fb_boot_splash /dev/fb0` | `/root/qt_camera_display/fb_boot_splash -f /dev/fb0` | LCD 显示静态启动首帧，命令返回 `0`；该路径不读取摄像头、不依赖 `/dev/galcore`，只验证 framebuffer 输出。 |
| UVC 输入 | `v4l2-ctl -d /dev/video0 --list-formats-ext` | KMS overlay 或安全预览通过 V4L2 ioctl 配置采集 | 能取帧并显示，日志无 `VIDIOC_*` 致命错误。 |
| KMS overlay 输出 | `run_qt_kms_overlay_display.sh status` | `uvc_kms_overlay` 调用 DRM plane 上屏 | overlay PID 存在，视频显示在预期矩形。 |
| KMS overlay 当前帧保存 | `test -S /tmp/uvc-kms-overlay-control.sock; ls -lh /mnt/sdcard/images/uvc_*.jpg /mnt/sdcard/images/uvc_*.png` | Qt 按钮通过后台任务向 socket 发送 `SAVE_DUAL /mnt/sdcard/images` | JPG/PNG 文件来自同一帧 overlay 当前 ARGB8888 显示帧，写入后 overlay 已执行 `fsync`；Qt 主界面在保存期间仍可响应页面切换。 |
| 上传历史记录 | `cat /mnt/sdcard/images/upload_history.json` | Qt 保存后台任务完成后，主线程 `UploadHistoryModel` 追加 JSON 记录 | 历史记录包含本次图片路径、上传时间、上传状态、文件大小和云端记录信息；历史页读取该文件后可横向查看。 |
| 统计分析汇总 | `test -s /mnt/sdcard/images/upload_history.json; grep -c '"upload_time"' /mnt/sdcard/images/upload_history.json` | 统计页通过 `uploadHistory` 模型读取同一份 JSON，不额外写统计数据库 | 统计页 `总记录` 与 JSON 中 `upload_time` 条目数量一致，最近记录表可跳转历史详情；没有历史记录时显示空状态。 |
| 手动控制命令日志 | 屏幕查看 `手动控制` 页命令日志；源码静态检查 `rg -n "manualCommandLog|appendManualCommandLog|handleManualAction" qml/Main.qml` | QML 按钮点击经 `handleManualAction()` 写入 `manualCommandLog` | 日志显示时间、命令、目标和结果；第一版不写串口、不改真实 GPIO/PWM，后续接入 F4 时再由 `MotionController` 写硬件命令。 |
| 参数与告警 UI 状态 | 屏幕查看 `参数设置`、`告警维护`；源码静态检查 `rg -n "settingsSummaryText|alarmSnapshotText|alarmHistoryListView|alarmAdvicePanel" qml/Main.qml` | QML 按钮点击更新本地属性、提示条和 `alarmHistoryModel`；保存诊断调用 C++ 控制器写 `/mnt/sdcard/logs/qt_alarm_snapshot.txt` | 第一版不写真实配置、不清真实 F4 告警；告警诊断快照必须是可在 SSH 中读取的真实文本文件。 |
| 告警诊断快照文件 | `test -s /mnt/sdcard/logs/qt_alarm_snapshot.txt; tail -n 30 /mnt/sdcard/logs/qt_alarm_snapshot.txt` | `saveAlarmSnapshotToSdCard()` 创建目录、覆盖写入快照、执行 `fsync`；可由屏幕按钮或 `--alarm-snapshot-self-test` 触发 | 文件非空，包含当前告警码、状态、相机/视频/存储摘要和最近告警历史；点击后不需要固定 `sleep`。 |
| COS 上传 | `ls -l /root/qt_camera_display/cos-upload.env; tail -n 20 /tmp/defect-cos-record-detail.json; cat /tmp/source_upload_headers.txt /tmp/annotated_upload_headers.txt` | `defect-cos-upload` 读取默认账号配置后调用后端 API 和 COS PUT | 配置文件权限为 `600`，HTTP 200、响应头含 ETag，云端新记录详情只出现本次 `source` 和 `annotated` 文件对象。 |
| 触摸输入 | `grep -A8 -B2 "Goodix" /proc/bus/input/devices` | Qt 按钮状态由触摸事件驱动更新 | 点击按钮后状态栏文字变化。 |
| 检测图片/日志 | `wc -c <file>`、`tail <log>` | 后续检测程序写入 `/mnt/sdcard/images` 或日志目录 | 文件大小稳定、日志进程退出、`sync` 或应用内 `fsync` 完成。 |

## 图片保存和检测结果落盘等待规则

当前界面主要显示实时画面，检测结果仍是演示占位。`保存图片` 测试按钮必须以屏幕真实点击结果为准；只用 socket 或 `--storage-self-test` 只能证明下层链路，不能替代 UI 验收。后续接入“正式拍照”“缺陷截图”“检测日志导出”并写入 SD 卡时，也必须使用下面的完成条件，不能只在按钮回调后 `sleep`：

保存按钮当前生成 JPG/PNG 两种图片。按钮点击后 QML 会立即返回并显示 `保存中...`，后台完成时才显示最终成功或失败结果。最终提示返回本地保存成功时，overlay 进程已经关闭文件并执行 `fsync`；人工验收仍建议用下面命令确认文件存在且大小稳定：

```sh
mount | grep ' /mnt/sdcard '
df -h /mnt/sdcard
# 在屏幕上点击“保存图片”后执行：
jpg=$(ls -t /mnt/sdcard/images/uvc_*.jpg | head -n 1)
png=$(ls -t /mnt/sdcard/images/uvc_*.png | head -n 1)
head -c 2 "$jpg"; echo
head -c 8 "$png" | hexdump -C
s1=$(wc -c < "$jpg")
p1=$(wc -c < "$png")
sleep 1
s2=$(wc -c < "$jpg")
p2=$(wc -c < "$png")
[ "$s1" = "$s2" ] && [ "$p1" = "$p2" ] && [ "$s1" -gt 0 ] && [ "$p1" -gt 0 ]
# 确认后再点击“安全卸载”，或在 SSH 中执行：
sdcard-safe-remove
```

| 保存对象 | 完成条件 | 额外说明 |
|---|---|---|
| JPG 原图 | `保存图片` 返回本地保存成功，文件头为 JPEG SOI，文件存在且大小稳定，overlay 已 `fsync` | JPG 文件路径形如 `/mnt/sdcard/images/uvc_YYYYMMDD_HHMMSS_000001.jpg`，上传时登记为 `source`。 |
| PNG 结果图 | `保存图片` 返回本地保存成功，文件头为 PNG 签名，文件存在且大小稳定，overlay 已 `fsync` | PNG 文件路径形如 `/mnt/sdcard/images/uvc_YYYYMMDD_HHMMSS_000001.png`，上传时登记为 `annotated`。 |
| COS 文件对象 | `defect-cos-upload` 返回成功，COS PUT 为 HTTP 200，响应头含 ETag，记录详情 `files` 只包含本次 `source` 和 `annotated` 两张图 | 上传成功不等于本地文件可以立即拔卡；仍要确认本地文件写完并安全卸载。 |
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
