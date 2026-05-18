# STM32MP157 Qt GPU 摄像头界面

这个目录提供一个最小可落地的 Qt Quick 工业检测主界面：左侧为功能导航，中间显示 UVC 摄像头实时画面，右侧显示检测结果，底部显示统计和控制按钮。

| 项目 | 当前实现 |
|---|---|
| UI 框架 | Qt Quick / QML |
| 摄像头输入 | 自定义 `V4L2VideoItem`，默认 `/dev/video0` |
| 视频显示 | 安全预览使用 V4L2 YUYV 帧上传到 OpenGL ES 纹理；正式路线使用 KMS overlay plane 显示视频 |
| GPU 路径 | `galcore` + OpenGL ES + `eglfs` 或 `wayland-egl` |
| 目标分辨率 | 1024x600 |
| 当前检测逻辑 | 点击 `检测` 后先运行 MobileNetV3-Small 分类，再运行 UNet 分割；分类完成后首页立即显示零件类型、类别、GOOD/BAD 和置信度，UNet 完成后立即显示双模型总耗时，COS 上传完成后再把本次原始图片、UNet raw/overlay/mask 结果图、两个模型输出和云端上传状态合并成一条历史记录。 |
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
| 检测历史路线 | 每次点击 `检测` 后，Qt 控制器后台依次执行当前帧 JPG 保存、MobileNetV3-Small 分类、UNet 分割、COS 上传；分类完成信号先刷新首页零件类型和类别，双模型完成信号再刷新 `total_time_ms`，最终上传完成后追加 `source_path`、`annotated_images`、`classification_result`、`segmentation_result`、云端记录号和上传状态到 `/mnt/sdcard/images/upload_history.json`；云端 `records.result` 由模型 `GOOD/BAD` 显式映射为 `good/bad`。 |
| 统计分析路线 | 左侧 `统计分析` 页面直接读取本地 `uploadHistory` 模型，汇总总记录、良品/待复核、上传成功率、图片数量、文件大小、最近检测趋势和云端状态；最近记录卡片内支持竖向滑动查看更多记录；当前统计基于双模型检测历史。 |
| 手动控制路线 | 左侧 `手动控制` 页面提供传送带、机械臂、光源、安全状态、人工复核和命令日志；第一版只做 UI 模拟状态、安全置灰和检测当前帧复用，不绕过 STM32F4 直接控制硬件。 |
| 参数设置路线 | 左侧 `参数设置` 页面提供工艺判定、视觉居中、输送分拣、相机光源、SD/COS 策略和参数摘要；第一版只维护 QML 本地状态与提示，真实 JSON 持久化和 `CMD_PARAM_SYNC` 下发 F4 后续接 C++ 控制器。 |
| 告警维护路线 | 左侧 `告警维护` 页面提供当前告警、设备健康、告警历史、处理建议、确认/清故障/刷新/保存诊断入口；告警本身第一版仍使用 QML 模拟状态，`保存诊断` 已通过 C++ 控制器真实写入 `/mnt/sdcard/logs/qt_alarm_snapshot.txt` 并执行 `fsync`；设备健康状态由 `DeviceHealthController` 真实探测 4G、相机、F4、云端和 SD 卡，不再显示固定在线文案。 |
| 真实健康状态 | `DeviceHealthController` 通过 `QTimer` 每 8 秒周期调度，4G/云端用异步 `QProcess`，相机 `STATUS` 和 F4 串口握手用后台线程；4G/云端进程启动失败由 `errorOccurred` 信号回写状态，不在刷新路径调用 `waitForStarted()`；周期刷新保留上一轮稳定状态，不再每轮把网络/云端显示成“检测中”；QML 只绑定状态属性，不执行 socket、串口或 shell 阻塞调用。 |
| 开机启动动画 | `fb_boot_splash` 先在 Qt/GPU 启动前直接写 `/dev/fb0` 显示静态首帧；QML 顶层 `splashOverlay` 随后显示工业检测自检动画，包含相机扫描窗口、ROI 框、阶段状态和进度条；`uvc_kms_overlay` 可以提前后台初始化和采集，但启动时必须先隐藏视频层，只有 Qt splash 完全淡出并置位 `bootOverlayRestoreFinished` 后才允许恢复摄像头画面，避免摄像头先于 Qt 界面出现。 |
| 保底视频路线 | 自定义 `V4L2VideoItem` 安全预览和 `VIDEO_BACKEND=qt-gst` mmap 桥接可演示，但不是最终零拷贝目标。 |
| 禁用结论 | QtMultimedia `Camera + VideoOutput` 和 `qmlglsink + UVC DMABUF` 不能作为稳定路线，前者触发内核 Oops，后者在 Vivante `libGAL.so` 中崩溃。 |

## 修改文件清单

| 路径 | 修改原因 |
|---|---|
| `20_uvc_camera/qt_camera_display/main.cpp` | 注册 `V4L2VideoItem`，向 QML 注入摄像头节点、采集尺寸、视频后端、GStreamer 状态等上下文参数；`CameraStorageController` 负责请求 overlay 保存当前帧、串行调用 `defect-classify` 和 `defect-segment`、通过 `detectClassificationReady` 在第一个模型结束后立即通知 QML 刷新零件类型和类别、通过 `detectModelsReady` 在两个模型结束后立即通知 QML 刷新 `total_time_ms`、按分类 `GOOD/BAD` 设置 `CLOUD_RESULT=good/bad` 后调用 `defect-cos-upload --jpg <source> --annotated <raw> --annotated <overlay> --annotated <mask>` 上传本次检测图片、调用 `sdcard-safe-remove`，并通过 `saveAlarmSnapshotToSdCard()` 把告警诊断快照写入 `/mnt/sdcard/logs/qt_alarm_snapshot.txt`；新增 `DeviceHealthController`，每 8 秒周期异步探测 4G、相机、F4、云端和 SD 卡真实状态，4G/云端进程启动失败通过异步错误信号回写，不在健康检测刷新路径等待启动，周期刷新保留网络/云端上一轮稳定状态，摄像头连续离线时只调用 `restart-overlay` 重启 overlay 视频进程；提供 `--detect-self-test`、`--storage-self-test` 和 `--alarm-snapshot-self-test` SSH 自检入口；`UploadHistoryModel` 把每次检测结果追加到 `/mnt/sdcard/images/upload_history.json` 并暴露给 QML 历史页，同时提供 `removeRecord()` 删除历史记录和对应 source/annotated 图片文件。 |
| `20_uvc_camera/qt_camera_display/qml/Main.qml` | 实现 1024x600 工业检测界面、状态栏、结果面板、统计区、`开始/暂停/继续/停止` 触摸按钮，以及 `检测`、`安全卸载` 两个真实操作按钮；顶部网络、相机、F4、云端和告警页设备健康矩阵绑定 `deviceHealth`，4G 测试不通过不显示在线，F4 未串口握手成功不显示接入，KMS 相机未出帧或帧序号停滞不显示在线；相机恢复在线时，`onCameraStatusChanged` 必须同时满足 `bootOverlayRestoreFinished`、`!splashOverlayVisible` 和当前在首页，才会发送 `VISIBLE 1` 恢复 overlay 画面，避免健康检测先于 Qt 启动画面把摄像头层打开；返回首页的 `switchPage()` 路径也使用同一启动完成门控，不在首页则保持隐藏；`检测` 调用异步 `requestDetectCurrentFrame()`，检测期间显示 `检测中...`，收到 `detectClassificationReady` 后立即显示模型识别出的零件类型、类别、GOOD/BAD 和置信度，收到 `detectModelsReady` 后立即显示双模型总耗时，最终 `detectCurrentFrameFinished` 只恢复忙状态并显示上传/历史记录结果；检测期间只禁用重复检测和安全卸载，左侧导航、历史、统计、手动、参数和告警页面仍可触摸；新增 `splashOverlayVisible`、`splashStageModel`、`advanceSplashStage()` 和 `finishSplashAnimation()`，启动时显示工业检测自检动画，使用相机扫描窗口、ROI 框、阶段状态和进度条替代启动空窗；`历史记录` 页面拆成第一层检测列表和第二层记录详情，第一层横向显示每次检测时间卡片并提供 `删除`、`查看` 按钮，滑动参数已调软以减少卡顿跳页感；点选卡片只改变高亮，不绑定 `ListView.currentIndex`，避免列表自动从开头滚到选中记录；详情页左侧横向滑动查看原始图片和 UNet raw/overlay/mask，右侧只保留上传时间、记录ID、图片数量和云端编号，并把模型原始 RESULT 转换为检测结论、可信度、缺陷提示等普通操作员可读文字；`统计分析` 页面复用检测历史模型，显示 KPI、最近检测趋势、结果分布、云端与文件状态、最近记录表，最近记录表在卡片内竖向滑动查看更多记录，并支持从最近记录跳转历史详情；`手动控制` 页面新增 `manualPageVisible`、`manualCommandLog` 和 `handleManualAction()`，提供手动模式、安全联锁、模拟 ACK、命令日志和检测当前帧入口，后续可接 C++ `MotionController`；`参数设置` 页面新增 `settingsPageVisible`、工艺/视觉/运动/存储参数和 `settingsApplyAction()`，第一版只做本地 UI 调整、摘要和提示；`告警维护` 页面新增 `alarmPageVisible`、`alarmHistoryModel`、`alarmSnapshotText()` 和 `handleAlarmAction()`，显示当前告警、真实设备健康、维护日志和处理建议，并把 `保存诊断` 的真实落盘结果显示到提示条。 |
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
| `20_uvc_camera/qt_camera_display/uvc_kms_overlay.c` | 正式化 KMS overlay 视频进程，使用 V4L2 mmap 采集、NEON `YUYV -> ARGB8888` 转换和 `drmModeSetPlane` 上屏；Unix socket 控制端点支持 `SAVE_DETECT <dir>`，把当前原始摄像头帧保存为检测 source JPG 并 `fsync`；保留 `SAVE_DUAL` 兼容旧保存自检；新增 `VISIBLE 0/1` 命令和 `-V 0|1` 初始可见性参数，开机时可先隐藏实时视频 plane，避免盖住启动动画和历史图片；新增 `STATUS` 命令返回 `has_frame/serial/visible/width/height`，Qt 用它判断相机是否真实出帧。 |
| `20_uvc_camera/qt_camera_display/build_uvc_kms_overlay.sh` | 交叉编译 `uvc_kms_overlay`，链接 libdrm、libjpeg、libpng、zlib 并生成板端可执行文件；脚本使用 `OVERLAY_CC` 作为专用覆盖变量，避免已经 `source` ST Qt SDK 后的 `CC="编译器 参数"` 污染 overlay 构建。 |
| `20_uvc_camera/qt_camera_display/fb_boot_splash.c` | 新增早期静态启动首帧绘制器，直接 mmap `/dev/fb0` 绘制深色背景、检测窗口、ROI 框、扫描线、英文标题和 18% 初始进度；不依赖 Qt、OpenGL、DRM、图片解码库或摄像头。 |
| `20_uvc_camera/qt_camera_display/build_fb_boot_splash.sh` | 交叉编译 `fb_boot_splash` 并生成 `build-mp157/fb_boot_splash`；脚本使用 `SPLASH_CC` 作为专用编译器变量，避免 ST Qt SDK 的 `CC` 污染非 Qt 辅助程序构建。 |
| `20_uvc_camera/qt_camera_display/defect-cos-upload` | 板端 COS 上传脚本，先读取 `/root/qt_camera_display/cos-upload.env` 作为默认上传账号；`--jpg` 原图按 `file_kind=source` 上传，重复传入的 `--annotated <jpg/png>` 检测结果图按 `file_kind=annotated` 上传，并按扩展名自动使用 `image/jpeg` 或 `image/png`；`CLOUD_RESULT` 只接受 `good/bad/review`，检测链路必须显式传入模型结果，避免坏品记录被固定写成良品。旧 `--png` 参数仍兼容，内部等同于一张 `--annotated` 结果图。 |
| `20_uvc_camera/qt_camera_display/defect_segment.cpp` | 新增独立 ONNX Runtime + libjpeg/libpng UNet 分割程序，读取分类 source JPG，裁剪中心 ROI，执行 INT8 UNet，输出 `RESULT_SEG`，并生成 raw JPG、overlay JPG、mask PNG 三张 annotated 结果图。 |
| `20_uvc_camera/qt_camera_display/build_defect_segment.sh` | 新增 `defect-segment` 交叉编译脚本，依赖 ARMv7 ONNX Runtime SDK、libjpeg 和 libpng。 |
| `20_uvc_camera/qt_camera_display/run_qt_kms_overlay_display.sh` | 板端 Qt + KMS overlay 生命周期脚本，支持 `start/stop/restart/restart-overlay/status/restore-fallback`；启动 Qt 前调用 `fb_boot_splash` 先显示静态首帧，再以隐藏状态启动 Qt UI 和 overlay 视频，等 QML 启动动画完成后由 Qt 发送 `VISIBLE 1` 恢复摄像头画面；`restart-overlay` 只重启 `uvc_kms_overlay` 且保持视频层隐藏，用于 USB 摄像头热拔插恢复，不杀 Qt 界面，避免当前在历史/统计/参数等页面时被视频层覆盖。 |
| `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh` | 静态检查 overlay 源码、构建脚本、运行脚本、触摸按钮、SD 卡按钮、历史记录模型/页面、统计分析页面、手动控制页面、参数设置页面、告警维护页面、真实设备健康状态、健康检测非阻塞契约、告警诊断快照落盘接口、早期静态首帧绘制器、启动动画覆盖层和默认启动契约。 |
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
| 构建 MobileNet 分类程序 | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ORT_ROOT=/home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display/onnxruntime-arm ./build_defect_classify.sh` | 生成 `build-mp157/defect-classify`。 |
| 构建 UNet 分割程序 | `cd /home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display && ORT_ROOT=/home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display/onnxruntime-arm ./build_defect_segment.sh` | 生成 `build-mp157/defect-segment`。 |
| 静态契约检查 | `./test_qt_kms_overlay_assets.sh` | 输出 `PASS: Qt KMS overlay assets contract`。 |
| 部署到 NFS rootfs | `sudo DEFECT_MODEL_SRC=/home/cfr/linux/model_picture/checkpoints_classify/defect_classifier_static_mixed_int8.onnx DEFECT_LABELS_SRC=/home/cfr/linux/model_picture/checkpoints_classify/defect_classifier_static_mixed_int8_labels.json DEFECT_UNET_MODEL_SRC=/tmp/defect_unet_test_decoder_head_int8.onnx ORT_ROOT=/home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display/onnxruntime-arm ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` | 板端 `/root/qt_camera_display/` 获得 Qt 程序、overlay 工具、`fb_boot_splash`、`defect-classify`、`defect-segment`、分类模型、UNet 模型、labels、ONNX Runtime 库和运行脚本，`/etc/init.d/` 获得 `S05display-quiet` 和 `S90uvc-camera`。 |
| 部署默认上传账号 | `CLOUD_ACCOUNT='<账号>' CLOUD_PASSWORD='<密码>' sudo -E ./deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` | 额外生成 `/home/cfr/linux/nfs/rootfs/root/qt_camera_display/cos-upload.env`，权限为 `600`；检测按钮后续可不再手工传账号密码。 |
| 安装 Qt runtime | `./install_qt_runtime_from_sdk.sh /home/cfr/linux/nfs/rootfs` | rootfs 获得 Qt5 库、QML 模块、eglfs/wayland 插件和 Vivante 库。 |
| 板端启动正式路线 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart` | Qt UI 与 overlay 视频同时运行。 |
| 只重启 overlay | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart-overlay` | 只重启 `uvc_kms_overlay` 并保持隐藏，Qt 主界面不退出；用于 USB 摄像头拔插后的手动恢复或健康检测自动恢复，画面显示恢复由 QML 在首页时发送 `VISIBLE 1`。 |
| 板端查看状态 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh status` | 显示 Qt PID、overlay PID、fallback PID、plane、控制 socket 和日志路径。 |
| 早期静态首帧观察 | 开发板 SSH | `/root/qt_camera_display/fb_boot_splash -f /dev/fb0; echo $?` | 命令返回 `0`，LCD 立即显示深色工业检测静态首帧，包含英文标题、检测窗口、ROI 框、扫描线和 18% 初始进度；不依赖 Qt、GPU、摄像头或 overlay。 |
| 开机动画观察 | 开发板重启或执行 `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart` 后观察 LCD | 先看到 `fb_boot_splash` 静态首帧，然后切换为 QML `工业缺陷检测系统`、`STM32MP157 Vision Inspection Terminal`、相机扫描窗口、`加载相机/初始化检测模型/连接运动控制/挂载存储/进入检测界面` 和进度条，随后淡出进入首页并恢复摄像头画面；摄像头画面不应抢在 Qt 启动画面前出现，画面中不出现 Ubuntu 企鹅图标。 |
| SSH 双模型检测自检 | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 'cd /root/qt_camera_display && ./qt_camera_display --detect-self-test'` | 不启动 QML，直接复用点击 `检测` 的链路：保存 source JPG、运行 MobileNetV3-Small、运行 UNet、上传 source 和全部 annotated 图、追加历史记录；成功输出以 `RESULT ` 开头。 |
| SSH 旧保存链路自检 | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 '/root/qt_camera_display/qt_camera_display --storage-self-test'` | 不启动 QML，直接复用旧 Qt 保存控制器请求 overlay 保存 JPG/PNG；该入口只保留作底层兼容诊断，正式界面不再暴露独立保存图片按钮。 |
| SSH 告警诊断自检 | `ssh -i /home/cfr/.ssh/id_ed25519_github -o IdentitiesOnly=yes root@192.168.1.250 '/root/qt_camera_display/qt_camera_display --alarm-snapshot-self-test && test -s /mnt/sdcard/logs/qt_alarm_snapshot.txt && tail -n 30 /mnt/sdcard/logs/qt_alarm_snapshot.txt'` | 不启动 QML，直接复用 Qt 告警诊断控制器写 `/mnt/sdcard/logs/qt_alarm_snapshot.txt`；输出 `诊断已保存：...` 且 `tail` 能看到 `alarm_code=0x0007`。 |
| 按钮双模型检测 | 开发板屏幕点击 `检测` | 按钮立即显示 `检测中...`；第一个分类模型完成后右侧结果面板立即显示模型识别出的零件名、GOOD/BAD、类别和百分制置信度；UNet 完成后立即显示双模型总耗时；COS 上传完成后底部提示条显示 `RESULT ... total_time_ms=... upload_status=OK/FAIL`，`/mnt/sdcard/images/upload_history.json` 新增一条含 source 和 3 张 UNet annotated 图片的历史记录。 |
| 检测期间界面响应 | 开发板屏幕 | 点击 `检测` 后，在仍显示 `检测中...` 时立即点击左侧 `历史记录`、`统计分析`、`手动控制`、`参数设置` 或 `告警维护` | 页面应立即切换或响应触摸；只有重复 `检测` 和 `安全卸载` 暂时不可点。若页面切换卡住，检查 QML 是否又直接调用同步检测函数；若能重复检测，检查 `detectInProgress`、`detectImageBusy` 和 `requestDetectCurrentFrame()`。 |
| 查看检测历史 | 开发板屏幕点击左侧 `历史记录`，横向滑动检测时间卡片并点击 `查看` | 实时视频 plane 被临时隐藏；第一层只显示检测记录列表，滑动应连续跟手；点击 `查看` 后进入详情页；详情页左侧可横向滑动原始图片和 UNet raw/overlay/mask，右侧显示上传时间、记录ID、图片数量、云端编号、云端状态、本地图片位置，以及“检测结论/可信度/缺陷提示”三条普通中文说明。 |
| 查看统计分析 | 开发板屏幕点击左侧 `统计分析` | 实时视频 plane 被临时隐藏；页面显示总记录、良品/待复核、上传成功率、图片总量、最近检测趋势、结果分布、云端与文件状态、最近记录表；最近记录卡片可上下滑动查看更多记录，点击任一记录行可进入对应历史详情页。 |
| 打开手动控制 | 开发板屏幕点击左侧 `手动控制` | 实时视频 plane 被临时隐藏；页面显示传送带、机械臂、光源、安全状态、人工复核和命令日志；未进入手动模式时运动按钮置灰或提示先进入手动模式。 |
| 手动页检测当前帧 | 开发板屏幕点击 `手动控制`，再点击 `检测当前帧` | 复用首页 `检测` 链路，生成分类 source 图、UNet annotated 图、云端记录和本地历史，并在命令日志记录本次辅助动作。 |
| 打开参数设置 | 开发板屏幕点击左侧 `参数设置` | 实时视频 plane 被临时隐藏；页面显示工艺与判定、视觉定位、输送与分拣、相机光源与存储、参数摘要和操作按钮。 |
| 调整参数设置 | 开发板屏幕点击 `参数设置` 页中的 `+/-`、`切换`、`应用参数`、`保存配置`、`恢复默认` | 界面参数值、参数摘要和底部提示条同步变化；第一版不写真实配置文件，不向 F4 下发参数。 |
| 打开告警维护 | 开发板屏幕点击左侧 `告警维护` | 实时视频 plane 被临时隐藏；页面显示当前告警 `0x0007`、设备健康矩阵、告警历史和处理建议。 |
| 告警维护操作 | 开发板屏幕点击 `确认`、`清故障`、`刷新状态`、`保存诊断` | 当前告警状态或维护日志更新；点击 `保存诊断` 时底部提示条显示 `诊断已保存：/mnt/sdcard/logs/qt_alarm_snapshot.txt` 或明确失败原因；第一版不代表真实 F4 告警已经清除。 |
| SSH 查看告警诊断快照 | 开发板屏幕点击 `保存诊断` 后，或执行 `--alarm-snapshot-self-test` 后，在 SSH 执行 `test -s /mnt/sdcard/logs/qt_alarm_snapshot.txt; wc -c /mnt/sdcard/logs/qt_alarm_snapshot.txt; tail -n 30 /mnt/sdcard/logs/qt_alarm_snapshot.txt` | 文件存在且大小大于 0，内容包含 `STM32MP157 Qt Alarm Snapshot`、`alarm_code=0x0007`、`camera_status=`、`storage_state=` 和 `[recent_alarm_history]`。 |
| 删除检测历史 | 开发板屏幕点击左侧 `历史记录`，在某条卡片点击 `删除` | 该条记录从 `/mnt/sdcard/images/upload_history.json` 中移除，对应 source/annotated 文件同步删除；如果删除最后一条，历史页显示 `暂无检测记录`。 |
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
| 2026-05-18 | 收紧相机在线回调的启动显示门控 | `Main.qml` 新增 `bootOverlayRestoreFinished`，只有 Qt splash 完全淡出并进入首页后，`onCameraStatusChanged` 和 `switchPage("home")` 才能发送 `VISIBLE 1`；相机可以提前后台初始化，但不能早于 Qt 启动画面显示。 |
| 2026-05-16 | 增加早期显示静默兜底 | 新增 `S05display-quiet`，部署脚本同步安装到 rootfs `/etc/init.d/`，用于尽早关闭 fbcon 光标和清理 `/dev/tty0` 残留；彻底消除最早期 `_` 仍建议在内核 bootargs 增加 `vt.global_cursor_default=0`。 |
| 2026-05-16 | 增加早期静态首帧 | 新增 `fb_boot_splash.c` 和 `build_fb_boot_splash.sh`，部署到 `/root/qt_camera_display/fb_boot_splash`；`S05display-quiet`、`S90uvc-camera` 和 `run_qt_kms_overlay_display.sh` 会在 Qt 启动前调用它，先显示启动动画第一帧风格的静态图。 |
| 2026-05-16 | 修复告警保存诊断只显示路径但不生成文件 | 根因为 `handleAlarmAction("snapshot")` 只把 `快照目标：/mnt/sdcard/logs/qt_alarm_snapshot.txt` 写到界面提示，没有调用任何文件写入逻辑；现在 QML 通过 `alarmSnapshotText()` 组装诊断文本，C++ `saveAlarmSnapshotToSdCard()` 检查 `/mnt/sdcard` 挂载、创建 `/mnt/sdcard/logs`、覆盖写入 `qt_alarm_snapshot.txt` 并 `fsync`。 |
| 2026-05-17 | 首页检测按钮接入 MobileNetV3-Small INT8 | 新增独立 `defect-classify` 推理程序，Qt 点击“检测”后通过 `SAVE_DETECT` 保存当前帧 JPG，再调用 ONNX Runtime 推理并把 GOOD/BAD、类别、置信度和耗时显示到首页结果面板。 |
| 2026-05-17 | 增加屏幕中心 ROI 观察框 | `uvc_kms_overlay` 在 KMS overlay 显示 framebuffer 上绘制绿色中心 `300x300` ROI 框，方便测试时把零件摆到模型实际检测区域；保存和检测图片从当前原始 YUYV 缓冲生成，不把绿色框送入模型。 |
| 2026-05-17 | 修复 ROI 框闪烁和预览卡顿 | 根因为上一版每帧先额外缓存整张 RGB24，再在整帧转换后补画 ROI 框，增加内存带宽且单 framebuffer 扫描时会看到框线被覆盖/重画；现在改为每行 YUYV 转换完成后立即覆盖该行 ROI 像素，常态预览不再每帧生成 `clean_rgb24`。 |
| 2026-05-18 | 扩展云端检测结果图上传参数 | `defect-cos-upload` 新增可重复 `--annotated <jpg/png>`，同一条记录中所有检测结果图统一登记为 `file_kind=annotated`，并按扩展名自动选择 `image/jpeg` 或 `image/png`；旧 `--png` 仍兼容，便于旧保存自检复用原调用。 |
| 2026-05-16 | 修复保存图片期间界面无法点击其它页面 | 根因为 QML 直接同步调用 `saveCurrentFrameToSdCard()`，本地保存、COS 上传和历史记录处理会占住 Qt 主线程；现在 QML 调用 `requestSaveCurrentFrameToSdCard()` 后立即返回，C++ 后台线程完成保存和上传，结束后通过 `saveCurrentFrameFinished` 回填提示并在主线程追加历史记录。 |
| 2026-05-18 | 接入 UNet INT8 分割模型并替代独立保存按钮 | 新增 `defect-segment` 和 `--detect-self-test`；点击 `检测` 时先保存 source JPG 并运行 MobileNetV3-Small 分类，再运行 UNet 分割输出 raw/overlay/mask 三张 annotated 图，最后把 source、annotated、分类 RESULT、UNet RESULT_SEG 和云端记录号合并为一条历史记录；首页不再暴露 `保存图片` 按钮。 |
| 2026-05-18 | 修正检测结果和首页显示契约 | Qt 检测链路根据分类 `GOOD/BAD` 显式传 `CLOUD_RESULT=good/bad`，上传脚本默认改为保守 `review` 并校验结果值；首页零件栏改为模型类别前缀，置信度改为百分制，耗时改为分类+UNet 都完成后的 `total_time_ms`；KMS 右侧按钮重新排布，避免 `开始/暂停/继续/停止` 与 `检测/安全卸载` 重叠。 |
| 2026-05-18 | 拆分检测显示时机 | `main.cpp` 新增 `detectClassificationReady` 和 `detectModelsReady` 阶段信号；QML 在第一个分类模型完成后立即显示零件类型、类别、GOOD/BAD 和置信度，在两个模型完成后立即显示 `total_time_ms`，不再等待 COS 上传完成才刷新首页检测结果。 |
| 2026-05-18 | 精简历史详情字段 | 历史详情右侧指标只保留上传时间、记录ID、图片数量和云端编号；删除分类图大小、检测图总量和流程状态展示；检测信息区不再直接显示 `classification_result`/`segmentation_result` 原始行，而是显示检测结论、可信度和缺陷提示，便于普通操作员复核。 |
| 2026-05-18 | 历史图片标签去技术化 | 历史详情左侧第一张图的标签由“分类原图”改为“原始图片”，右侧路径标题改为“本地图片位置”，避免操作员误以为需要关注分类图文件大小或模型内部阶段。 |
| 2026-05-18 | 顶部和健康矩阵改为真实设备状态 | 新增 `DeviceHealthController`，4G 必须 `4g-ppp test` 返回 0 才显示在线；KMS 相机必须 overlay `STATUS` 返回 `has_frame=1`、`serial>0` 且 `serial` 相对上一轮有变化才显示在线；F4 必须 `/dev/ttySTM2` 发送 `STATUS\r\n` 并收到 `ACK/OK/F4/READY` 才显示接入；云端必须 `curl -fsS --max-time 2 http://119.91.65.122/health` 成功才显示已连接；4G/云端启动失败通过 Qt 异步错误信号处理，不在刷新路径等待进程启动；健康刷新周期放慢到 8 秒，并保留上一轮稳定状态，避免网络/云端在“检测中”和“在线/已连接”之间循环闪烁；所有耗时检测都用异步进程或后台线程，QML 主线程不等待。 |
| 2026-05-18 | 支持 USB 摄像头热拔插恢复 | `uvc_kms_overlay` 新增 `STATUS` 命令，Qt 连续检测相机离线后后台调用 `run_qt_kms_overlay_display.sh restart-overlay`，只重启 overlay 视频进程，不重启 Qt 界面；摄像头重新插入并恢复出帧后界面状态自动回到在线。 |

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
| F4 串口 `/dev/ttySTM2` | F4 控制器真实接入判断 | Qt 后台线程按 115200 8N1 raw 模式发送 `STATUS\r\n`；回复中包含 `ACK`、`OK`、`F4` 或 `READY` 才显示“接入”，否则显示“待接入”。 |
| 云端 health `http://119.91.65.122/health` | 云端连接状态 | Qt 异步执行 `curl -fsS --max-time 2`；请求成功才显示“已连接”，失败或超时不显示已连接。 |
| 检测历史文件 `/mnt/sdcard/images/upload_history.json` | Qt 历史记录页数据源 | 每次检测/上传完成后由 `UploadHistoryModel` 追加写入；记录包含 `source_path`、`annotated_images`、`classification_result`、`segmentation_result`、`record_id`、`record_no`、`upload_status` 和文件大小，同时保留旧 `jpg_path/png_path` 兼容字段。 |
| 统计分析页面 | 汇总本地检测历史 | 数据来自 `uploadHistory` 模型，不新增单独数据库；统计页打开时通过 `setOverlayVisible(false)` 隐藏 KMS 视频 plane，返回首页时恢复。 |
| 手动控制页面 | 调试级人工操作界面 | 数据来自 QML 模拟状态和 `manualCommandLog`；手动页打开时通过 `setOverlayVisible(false)` 隐藏 KMS 视频 plane；传送带 PWM、机械臂舵机、急停、限位和联锁仍归 STM32F4，MP157 第一版不直接控制硬件。 |
| 启动动画覆盖层 | 替代启动空窗和展示系统自检 | 数据来自 QML 本地 `splashStageModel`，不是硬件真实自检结果；动画使用 Qt Quick 基础图元和 transform/opacity/y 动画，不加载外部图片或视频资源，避免拖慢板端启动。 |
| 后续 `MotionController` | MP157 到 STM32F4 的高层命令桥 | 当前未实现；后续应把 `handleManualAction()` 中的模拟 ACK 替换为串口命令发送、ACK 等待、超时和错误码映射。 |
| 云端后端 `http://119.91.65.122` | 检测记录创建、COS 预签名上传和文件登记 | `defect-cos-upload` 默认使用该地址，可通过 `CLOUD_BASE_URL` 覆盖。 |
| COS 上传脚本 `/root/qt_camera_display/defect-cos-upload` | 上传 source/annotated 检测图到 COS | 需要 curl、后端 Cookie，或本地 `/root/qt_camera_display/cos-upload.env` 中的 `CLOUD_ACCOUNT/CLOUD_PASSWORD`；`--jpg` 登记为 `source`，重复 `--annotated <jpg/png>` 登记为 `annotated`，脚本按扩展名设置 `image/jpeg` 或 `image/png`；`CLOUD_DEVICE_ID`、`CLOUD_PART_ID` 可显式覆盖，留空时自动取当前账号第一条可用设备/零件；`captured_at/uploaded_at` 显式使用 `TZ=CST-8` 的北京时间墙钟并追加 `+08:00`，不得在前后端额外手工加减 8 小时。 |
| 默认上传账号配置 `/root/qt_camera_display/cos-upload.env` | 检测按钮自动登录云端 | 只保存在板端/rootfs 本地，不提交 Git；权限必须为 `600`；可通过 `CLOUD_UPLOAD_ENV_FILE` 指定其他板端绝对路径，运行时环境变量优先于该文件。 |
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
| 中 | 当前帧冻结、双模型检测和人工复核联动 | 检测记录、人工复核结果和图片保存链路需要合并时 | 手动页的 `GOOD/BAD/UNCERTAIN` 后续写入检测记录，检测图片上传时同时带上复核标记和本次动作上下文。 |
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
| 双模型检测 + COS 上传 | 2026-05-18 板端 `./qt_camera_display --detect-self-test` 验证通过。输出 `RESULT status=GOOD class=gasket_good ... segment_status=OK defect_pixels=0 ... upload_status=OK`；本地历史新增 `record_id=48 / MP157-20260518-145002`，包含 `source_path`、3 张 `annotated_images`、`classification_result` 和 `segmentation_result`；云端详情同一条记录包含 1 张 `source` 和 3 张 `annotated` 文件，其中 JPEG 为 `image/jpeg`，mask PNG 为 `image/png`。后续坏品验证必须确认 `RESULT status=BAD` 时云端记录 `result` 为 `bad`，不能再显示成良品。 |

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
| 上传参数解析回归 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `sh defect-cos-upload --self-test-args` | `--annotated` 能保留多张结果图，`.jpg/.jpeg` 识别为 `image/jpeg`，`.png` 识别为 `image/png`，多张 annotated 的响应头路径互不覆盖。 | 若失败，说明检测结果图上传参数或动态 content-type 逻辑回退。 |
| 上传结果字段回归 | 虚拟机 `20_uvc_camera/qt_camera_display/` | `grep -n "CLOUD_RESULT" main.cpp defect-cos-upload; grep -n "validate_cloud_result" defect-cos-upload` | Qt 检测上传前设置 `CLOUD_RESULT`，脚本含 `validate_cloud_result`，默认结果为 `review` 而不是固定 `good`。 | 若缺少这些标记，坏品检测可能再次被云端登记成良品。 |
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
| 真实 4G 状态 | 开发板 SSH | `4g-ppp test; echo "exit=$?"` | 命令退出码为 `0` 时，Qt 顶部网络显示 `在线`；退出码非 0、命令不存在或超过 2.5 秒时显示离线/未安装/超时。 | 若界面仍显示在线，检查是否运行新 Qt 二进制，并用 `strings /root/qt_camera_display/qt_camera_display | grep DeviceHealthController` 确认已部署。 |
| overlay STATUS | 开发板 SSH | `printf 'STATUS\n' | nc -U /tmp/uvc-kms-overlay-control.sock; sleep 3; printf 'STATUS\n' | nc -U /tmp/uvc-kms-overlay-control.sock` | 输出类似 `OK STATUS has_frame=1 serial=123 visible=1 width=640 height=480`，第二次 `serial` 应大于第一次，且首页相机状态显示 `在线`。 | 若 `nc` 不支持 `-U`，只用屏幕状态和 `/tmp/uvc_kms_overlay.log` 验证；若 `has_frame=0`、`serial=0` 或 `serial` 不变化，先查 USB 摄像头、`/dev/video0` 和 overlay 日志。 |
| USB 摄像头拔出检测 | 开发板屏幕和 SSH | 保持 Qt 首页运行，拔掉 USB 摄像头，再观察 6 秒；同时执行 `tail -n 80 /tmp/uvc_kms_overlay.log` | 顶部/健康矩阵相机状态变为离线类状态，Qt 主界面仍可切换页面和点击按钮，不应整屏卡死。 | 若界面卡住，检查 `main.cpp` 是否仍在主线程执行 socket/串口等待；若状态不变，确认部署了支持 `STATUS` 的新版 `uvc_kms_overlay`。 |
| USB 摄像头重新插入恢复 | 开发板屏幕和 SSH | 插回 USB 摄像头，必要时执行 `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart-overlay; printf 'STATUS\n' | nc -U /tmp/uvc-kms-overlay-control.sock` | overlay 只重启视频进程，Qt PID 不变；`STATUS` 恢复 `has_frame=1 serial>0` 且后续 `serial` 持续变化后，相机状态回到 `在线`；如果当前页面是首页，QML 再发送 `VISIBLE 1` 让画面重新显示。 | 若 Qt PID 变化，说明误用了 `restart` 而不是 `restart-overlay`；若 overlay 起不来，查 USB 枚举、`/dev/video0`、DRM plane 占用和 `/tmp/uvc-kms-overlay.log`。 |
| F4 串口握手状态 | 开发板 SSH | `test -e /dev/ttySTM2 && stty -F /dev/ttySTM2 115200 raw -echo -crtscts; printf 'STATUS\r\n' > /dev/ttySTM2; timeout 1 cat /dev/ttySTM2 | head -c 80` | 读到包含 `ACK`、`OK`、`F4` 或 `READY` 的回复时，Qt 显示 `接入`；没有回复、串口不存在或回复不匹配时显示 `待接入`。 | 若命令占用串口影响 Qt，先停止手工 `cat`；若波特率或协议不同，先统一 F4 固件的 STATUS 应答格式。 |
| 云端 health 状态 | 开发板 SSH | `curl -fsS --max-time 2 http://119.91.65.122/health >/tmp/cloud-health.txt; echo "exit=$?"; cat /tmp/cloud-health.txt` | curl 退出码为 `0` 时，云端显示 `已连接`；失败或超时时显示离线/超时，不再固定显示已连接。 | 若 curl 不存在，先补 rootfs 工具；若后端 health 路径改变，需要同步修改 `DEFAULT_CLOUD_HEALTH_URL` 并更新文档。 |
| SD 卡健康状态 | 开发板 SSH | `mount | grep ' /mnt/sdcard '; df -h /mnt/sdcard` | `/mnt/sdcard` 挂载时健康矩阵显示 `已挂载`，卸载后显示 `未挂载`。 | 若界面状态和命令不一致，检查 `/proc/mounts` 解析和是否运行旧二进制。 |
| Qt 顶部时钟时区 | 开发板 SSH | `qtpid=$(pidof qt_camera_display); tr '\0' '\n' < /proc/$qtpid/environ | grep '^TZ='; TZ=CST-8 date '+%H:%M:%S %Z'; date -u '+%H:%M:%S UTC'` | 进程环境显示 `TZ=CST-8`，屏幕顶部时间应与 `TZ=CST-8 date` 同小时，且比 `date -u` 快 8 小时。 | 若进程环境没有 `TZ=CST-8`，重新部署 `qt_camera_display` 和两个启动脚本；若环境正确但屏幕仍是 UTC，确认是否运行的是旧二进制或旧 QML 资源。 |
| 安全预览兜底 | 开发板 | `/root/qt_camera_display/run_qt_camera_display.sh` | 以默认 `320x240@10fps` 显示安全预览。 | 若启动失败，查 `/dev/video0`、`/dev/galcore`、Qt runtime 和 QML 模块。 |
| 高画质压力测试 | 开发板 | `CAMERA_WIDTH=640 CAMERA_HEIGHT=480 CAMERA_FPS=15 /root/qt_camera_display/run_qt_camera_display.sh` | 能显示更高画质，同时 CPU 升高。 | 若 CPU 过高，这是安全路径预期瓶颈，不要误判为内核崩溃。 |
| 触摸按钮验证 | 开发板屏幕 | 依次点击 `开始`、`暂停`、`继续`、`停止` | 状态栏文字随动作变化，视频 plane 不被按钮遮挡。 | 若触摸无效，查 Goodix event 节点、`QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS` 和 Qt 输入日志。 |
| 检测前置检查 | 开发板 | `mount | grep ' /mnt/sdcard '; df -h /mnt/sdcard; /root/qt_camera_display/run_qt_kms_overlay_display.sh status; ls -lh /root/qt_camera_display/defect-classify /root/qt_camera_display/defect-segment /root/qt_camera_display/models/defect_unet_test_decoder_head_int8.onnx` | `/mnt/sdcard` 已挂载，overlay PID 存在，状态输出包含 `/tmp/uvc-kms-overlay-control.sock`，两个推理程序和 UNet 模型存在。 | 若未挂载，执行 `/etc/init.d/S85sdcard-mount status`；若 socket 不存在，查 `/tmp/uvc-kms-overlay.log`；若模型缺失，重新部署 `DEFECT_UNET_MODEL_SRC`。 |
| 双模型 SSH 自检 | 开发板 SSH | `cd /root/qt_camera_display && ./qt_camera_display --detect-self-test` | 输出以 `RESULT ` 开头，包含 `status=GOOD|BAD`、`total_time_ms=<ms>`、`segment_status=OK|NG`、`source_path=/mnt/sdcard/images/uvc_*.jpg` 和 `upload_status=OK|FAIL`；当 `status=BAD` 时云端记录应为 `bad`。 | 若保存失败，查 `/mnt/sdcard`、overlay socket 和 `SAVE_DETECT`；若分类失败，查 `defect-classify`、分类模型和 labels；若 UNet 失败，查 `defect-segment`、UNet 模型和 ONNX Runtime；若上传结果变成良品，查 Qt 是否传入 `CLOUD_RESULT`、脚本是否部署新版。 |
| 检测图片内容检查 | 开发板 SSH | `src=$(ls -t /mnt/sdcard/images/uvc_*.jpg | head -n 1); raw=$(ls -t /mnt/sdcard/images/segment_*_raw.jpg | head -n 1); overlay=$(ls -t /mnt/sdcard/images/segment_*_overlay.jpg | head -n 1); mask=$(ls -t /mnt/sdcard/images/segment_*_mask.png | head -n 1); ls -lh "$src" "$raw" "$overlay" "$mask"; head -c 2 "$src" | hexdump -C; head -c 2 "$raw" | hexdump -C; head -c 2 "$overlay" | hexdump -C; head -c 8 "$mask" | hexdump -C` | source/raw/overlay 是 JPEG SOI `ff d8`，mask 是 PNG 签名 `89 50 4e 47 0d 0a 1a 0a`，四个文件都非空。 | 若文件头不正确，说明编码失败或取错文件；若文件不存在，先查 `RESULT_SEG` 中的 `raw_path/overlay_path/mask_path`。 |
| 检测历史文件检查 | 开发板 SSH | `test -s /mnt/sdcard/images/upload_history.json; tail -n 120 /mnt/sdcard/images/upload_history.json` | JSON 最新记录包含 `source_path`、3 项 `annotated_images`、`classification_result`、`segmentation_result`、`upload_status`、`record_id` 和 `record_no`。 | 若文件不存在，先确认点击 `检测` 或 `--detect-self-test` 是否走到 Qt 主进程；再查 `/tmp/qt-kms-overlay-shell.log` 和 `upload history save failed`。 |
| 历史页触摸验证 | 开发板屏幕 | 点击左侧 `历史记录`，横向滑动检测时间卡片，点击某条记录空白区域选中，再点击 `查看`，再在左侧图片区域左右滑动 | 页面第一层只显示检测记录列表；滑动时间卡片和图片轮播应连续跟手；详情页左侧能在原始图片、UNet raw、UNet overlay、UNet mask 之间切换，右侧只显示上传时间、记录ID、图片数量、云端编号、云端状态、本地图片位置，以及“检测结论/可信度/缺陷提示”三条普通中文说明。 | 若历史页空白，先查 `/mnt/sdcard/images/upload_history.json`；若点选后列表自动滚动，检查 `Main.qml` 是否又出现 `currentIndex: root.selectedHistoryIndex` 或 `highlightRangeMode: ListView.ApplyRange`；若图片不显示，确认文件路径存在且图片头正确；若无法触摸，查 Goodix 输入配置。 |
| 统计页触摸验证 | 开发板屏幕 | 点击左侧 `统计分析`，观察 KPI、最近检测趋势、分布概览、云端与文件状态、最近记录表；在最近记录卡片内上下滑动，再点击任一记录行 | 统计页打开后实时视频 plane 隐藏；总记录应等于 `upload_history.json` 记录数；良品/待复核、上传成功率、图片数量和文件大小有值；最近记录表能在卡片内竖向滑动查看更多记录；点击最近记录行进入对应历史详情页。 | 若统计页无数据，先查 `/mnt/sdcard/images/upload_history.json` 是否存在且非空；若不能滑动，检查 `statsRecentListView` 是否仍是 `ListView`；若点击无反应，检查 `openHistoryDetailFromStats` 和 `showHistoryDetail`；若视频仍覆盖统计页，查 `setOverlayVisible(pageName === "home")` 和 overlay `VISIBLE` 命令。 |
| 参数页布局验证 | 开发板屏幕 | 点击左侧 `参数设置`，依次观察顶部摘要、三张参数卡、下方存储卡和参数摘要卡；点击 `+/-`、`切换`、`应用参数`、`保存配置`、`恢复默认` | 所有文字都在卡片边框内，长路径和摘要只省略不换行溢出；参数按钮不挤出卡片，底部提示条不遮挡其它区域。 | 若仍有文字越界，先查 `Main.qml` 中对应卡片是否缺少 `clip: true`、`elide: Text.ElideRight/ElideMiddle`，再按板端实际字体继续缩短文案或减少同屏字段。 |
| 告警页布局验证 | 开发板屏幕 | 点击左侧 `告警维护`，观察当前告警、设备健康矩阵、告警历史、处理建议；点击 `确认`、`清故障`、`刷新状态`、`保存诊断` | 当前告警按钮在告警卡片右侧纵向排列，不挤占发生时间和处理状态；设备健康 3x3 网格、告警历史每一行和处理建议都不越过卡片边界；长诊断路径和状态文字只省略显示。 | 若当前告警按钮仍压住文字，检查 `alarmCurrentPanel` 是否为左信息右按钮布局；若历史行仍超出，优先检查 `alarmHistoryListView` delegate 的列宽总和与 `spacing`；若设备健康覆盖刷新按钮，检查 `alarmHealthGrid` 单元高度、行距和刷新按钮 `y`。 |
| 告警诊断快照落盘 | 开发板屏幕和 SSH | 屏幕点击 `告警维护` 的 `保存诊断`，或执行 `/root/qt_camera_display/qt_camera_display --alarm-snapshot-self-test`，再执行 `test -s /mnt/sdcard/logs/qt_alarm_snapshot.txt; wc -c /mnt/sdcard/logs/qt_alarm_snapshot.txt; tail -n 30 /mnt/sdcard/logs/qt_alarm_snapshot.txt` | 文件存在且非空，`tail` 能看到 `snapshot_time=`、`alarm_code=0x0007`、`alarm_status=`、`camera_status=` 和 `[recent_alarm_history]`；按钮提示为真实 C++ 返回值。 | 若目录为空，先查屏幕提示或自检输出是否为 `诊断保存失败`；再查 `/tmp/qt_camera_display.log` 中 `storage action alarm-snapshot`、`mount | grep ' /mnt/sdcard '`、`df -h /mnt/sdcard` 和 SD 卡是否只读。 |
| 手动页入口验证 | 开发板屏幕 | 点击左侧 `手动控制` | 手动页显示传送带、机械臂、光源与辅助、安全状态、人工复核和命令日志；实时 KMS 视频 plane 不覆盖页面。 | 若无法进入，检查 `switchPage()` 是否允许 `manual`，左侧导航点击分支是否包含 `manual`；若视频覆盖页面，查 `setOverlayVisible(pageName === "home")` 和 overlay `VISIBLE` 命令。 |
| 手动页安全置灰 | 开发板屏幕 | 未点击 `进入手动` 时观察并点击 `正向点动`、`抓取测试`、`放良品` | 运动按钮应置灰；若触摸事件仍到达保护分支，应提示 `请先进入手动模式` 并写入命令日志。 | 若未进手动仍能执行运动状态，检查 `manualActionAllowed()` 和 `handleManualAction()` 的 `manualMode` 保护分支。 |
| 手动模式切换 | 开发板屏幕 | 点击 `进入手动`，再点击传送带 `正向点动`、`停止` | 顶部模式显示 `手动`；传送带状态显示 `正向点动` 后回到 `停止`；命令日志最新在上。 | 若模式不变，查 `manualMode` 绑定；若日志不更新，查 `manualCommandLog.insert(0, ...)`。 |
| 机械臂回零联锁 | 开发板屏幕 | 进入手动后先点击 `抓取测试`，再点击 `回零`，再点击 `抓取测试` | 未回零时抓取类按钮置灰或提示 `机械臂未回零`；回零后抓取测试可更新为 `抓取测试/吸取`。 | 若未回零也能抓取，检查 `manualArmHomeOk` 和抓取/放置动作的联锁条件。 |
| 手动页检测当前帧 | 开发板屏幕和 SSH | 手动页点击 `检测当前帧`，再执行 `ls -lt /mnt/sdcard/images/uvc_*.jpg /mnt/sdcard/images/segment_* | head -n 8` | source JPG 和 UNet raw/overlay/mask 最新文件生成，底部提示沿用检测链路，命令日志出现 `已请求双模型检测当前帧`。 | 若图片未生成，按检测链路排查 `/mnt/sdcard`、overlay socket、`requestDetectCurrentFrame()` 和 `/tmp/qt-kms-overlay-shell.log`。 |
| 历史页删除验证 | 开发板屏幕和 SSH | 删除前从最新 JSON 记录确认 `source_path` 和 `annotated_images[].path`，屏幕点击对应历史卡片 `删除`，再确认这些路径均不存在且 JSON 不再包含该 `source_path`。 | 被删记录对应 source/annotated 文件不存在，历史 JSON 不再包含该 source 路径；界面自动选中相邻记录或显示 `暂无检测记录`。 | 若 JSON 仍包含路径，查 `/tmp/qt-kms-overlay-shell.log` 中 `upload history remove save failed`；若 JSON 删除但图片还在，查日志里的 `upload history image remove failed` 或路径是否不在 `/mnt/sdcard/images`。 |
| 历史页 overlay 遮挡检查 | 开发板屏幕和 SSH | 进入历史页后观察实时视频是否消失；必要时执行 `printf 'VISIBLE 0\n' | nc -U /tmp/uvc-kms-overlay-control.sock` 和 `printf 'VISIBLE 1\n' | nc -U /tmp/uvc-kms-overlay-control.sock` | 历史页打开时实时 KMS 视频 plane 不遮挡历史图片，返回首页后实时视频恢复。 | 若 `nc -U` 不存在，可只用屏幕行为验收；若历史页仍被视频覆盖，查 `uvc_kms_overlay` 是否为新版并支持 `VISIBLE` 命令。 |
| 默认账号配置检查 | 开发板 SSH | `ls -l /root/qt_camera_display/cos-upload.env; grep ^CLOUD_ /root/qt_camera_display/cos-upload.env | cut -d= -f1` | `ls -l` 输出 `-rw------- root root`，并只列出 `CLOUD_ACCOUNT`、`CLOUD_PASSWORD` 两个变量名。 | 若文件不存在或不可读，重新部署默认账号配置；不要把账号密码写进源码或日志。 |
| COS 上传脚本自检 | 开发板 SSH | `/root/qt_camera_display/defect-cos-upload --jpg "$raw_jpg" --annotated "$overlay_jpg" --annotated "$mask_png"` | stdout 输出 `上传成功：record_id=... record_no=... source_kind=source annotated_count=2`；stderr 打印 `record_no`、`created_record_id`、`prepare_local_tag`、`prepare_object_key`、`register_url`，且所有 `register_url` 都是新记录 ID；详情页时间应接近 `TZ=CST-8 date` 显示的北京时间。 | 若登录失败，查 `cos-upload.env`、Cookie；若自动查询失败，手动设置 `CLOUD_PART_ID/CLOUD_DEVICE_ID`；若提示 `object_key 未包含当前 record_no`，说明 prepare 仍落到旧记录路径，停止继续登记；若详情时间偏 8 小时，查 `board-time-sync status` 和脚本是否导出 `TZ=CST-8`。 |
| COS 记录回查 | 开发板 SSH | `record_id=<上一步ID>; curl -fsS -b /tmp/cloud_cookie.txt "http://119.91.65.122/api/v1/records/$record_id"` | 返回记录详情，`files` 中包含 1 张 `source` 原图和传入数量一致的 `annotated` 检测结果图，并有 `preview_url`；旧 `record_id=3` 不再新增文件；`captured_at/uploaded_at` 与 `TZ=CST-8 date` 同小时。 | 若 `files` 为空或数量不是 `1 + annotated_count`，脚本会返回上传失败；优先查脚本输出的 `created_record_id`、`prepare_object_key` 和后端 `/records/<id>/files` 日志；若时间偏 8 小时，先看 `board-time-sync status`、板端 `/tmp/defect-cos-record.json` 和 `/tmp/defect-cos-register-*.json` 原始值。 |
| 安全卸载按钮 | 开发板屏幕和 SSH | 屏幕点击 `安全卸载`，再执行 `/etc/init.d/S85sdcard-mount status` | 界面显示卸载完成，状态显示未挂载或等待拔卡。 | 若卸载失败，查是否有进程占用 `/mnt/sdcard`，不要直接拔卡。 |
| 零拷贝候选探测 | 开发板 | `VIDEO_DEV=/dev/video0 CAMERA_WIDTH=640 CAMERA_HEIGHT=480 CAMERA_FPS=15 /root/qt_camera_display/probe_zero_copy_video_path.sh` | 在 `logs/zero-copy-probe-*.log` 生成探测日志。 | 若某路线失败，保留日志，不要重新启用已知崩溃的 QtMultimedia `Camera + VideoOutput`。 |

## 读写/数据路径验证

| 数据路径 | 读验证 | 写验证 | 通过标准 |
|---|---|---|---|
| Qt 运行资源 | `ls /root/qt_camera_display/qt_camera_display /usr/lib/libQt5Core.so.5` | `deploy_qt_camera_display.sh /home/cfr/linux/nfs/rootfs` | 板端程序和 Qt runtime 同时存在。 |
| 早期静态首帧 | `ls -l /root/qt_camera_display/fb_boot_splash /dev/fb0` | `/root/qt_camera_display/fb_boot_splash -f /dev/fb0` | LCD 显示静态启动首帧，命令返回 `0`；该路径不读取摄像头、不依赖 `/dev/galcore`，只验证 framebuffer 输出。 |
| UVC 输入 | `v4l2-ctl -d /dev/video0 --list-formats-ext` | KMS overlay 或安全预览通过 V4L2 ioctl 配置采集 | 能取帧并显示，日志无 `VIDIOC_*` 致命错误。 |
| KMS overlay 输出 | `run_qt_kms_overlay_display.sh status; printf 'STATUS\n' | nc -U /tmp/uvc-kms-overlay-control.sock` | `uvc_kms_overlay` 调用 DRM plane 上屏，Qt 健康检测后台发送 `STATUS` | overlay PID 存在，`STATUS` 返回 `has_frame=1 serial>0` 且连续两轮 `serial` 增长才代表真实摄像头在线。 |
| 4G 状态探测 | `command -v 4g-ppp; 4g-ppp test; echo $?` | Qt `DeviceHealthController` 异步执行 `4g-ppp test` | 只有测试退出码为 0 时顶部网络显示在线；失败不能显示在线。 |
| F4 状态探测 | `ls -l /dev/ttySTM2`，必要时用串口工具读取 F4 回复 | Qt 后台线程向 `/dev/ttySTM2` 写入 `STATUS\r\n` 并等待短回复 | 只有回复包含 `ACK/OK/F4/READY` 才显示接入；未通信成功时显示待接入。 |
| 云端状态探测 | `curl -fsS --max-time 2 http://119.91.65.122/health` | Qt 异步执行 curl health | curl 成功才显示已连接；失败、弱网或超时不显示已连接。 |
| SD 卡健康探测 | `mount | grep ' /mnt/sdcard '` | Qt 读取 `/proc/mounts` | 挂载点存在显示已挂载；不存在显示未挂载。 |
| KMS overlay 当前帧保存 | `test -S /tmp/uvc-kms-overlay-control.sock; ls -lh /mnt/sdcard/images/uvc_*.jpg` | Qt 检测后台任务向 socket 发送 `SAVE_DETECT /mnt/sdcard/images` | source JPG 来自当前原始 YUYV 缓冲，写入后 overlay 已执行 `fsync`；绿色 ROI 观察框不会进入模型输入图。 |
| 检测历史记录 | `cat /mnt/sdcard/images/upload_history.json` | Qt 检测后台任务完成后，主线程 `UploadHistoryModel` 追加 JSON 记录 | 历史记录包含本次 source、annotated 图数组、两个模型输出、上传状态、文件大小和云端记录信息；历史页读取该文件后可横向查看。 |
| 统计分析汇总 | `test -s /mnt/sdcard/images/upload_history.json; grep -c '"upload_time"' /mnt/sdcard/images/upload_history.json` | 统计页通过 `uploadHistory` 模型读取同一份 JSON，不额外写统计数据库 | 统计页 `总记录` 与 JSON 中 `upload_time` 条目数量一致，最近记录表可跳转历史详情；没有历史记录时显示空状态。 |
| 手动控制命令日志 | 屏幕查看 `手动控制` 页命令日志；源码静态检查 `rg -n "manualCommandLog|appendManualCommandLog|handleManualAction" qml/Main.qml` | QML 按钮点击经 `handleManualAction()` 写入 `manualCommandLog` | 日志显示时间、命令、目标和结果；第一版不写串口、不改真实 GPIO/PWM，后续接入 F4 时再由 `MotionController` 写硬件命令。 |
| 参数与告警 UI 状态 | 屏幕查看 `参数设置`、`告警维护`；源码静态检查 `rg -n "settingsSummaryText|alarmSnapshotText|alarmHistoryListView|alarmAdvicePanel" qml/Main.qml` | QML 按钮点击更新本地属性、提示条和 `alarmHistoryModel`；保存诊断调用 C++ 控制器写 `/mnt/sdcard/logs/qt_alarm_snapshot.txt` | 第一版不写真实配置、不清真实 F4 告警；告警诊断快照必须是可在 SSH 中读取的真实文本文件。 |
| 告警诊断快照文件 | `test -s /mnt/sdcard/logs/qt_alarm_snapshot.txt; tail -n 30 /mnt/sdcard/logs/qt_alarm_snapshot.txt` | `saveAlarmSnapshotToSdCard()` 创建目录、覆盖写入快照、执行 `fsync`；可由屏幕按钮或 `--alarm-snapshot-self-test` 触发 | 文件非空，包含当前告警码、状态、相机/视频/存储摘要和最近告警历史；点击后不需要固定 `sleep`。 |
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
ls -lh "$src" "$raw" "$overlay" "$mask" /mnt/sdcard/images/upload_history.json
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
| `main.cpp` | 增加 `requestDetectCurrentFrame()` 和 `--detect-self-test` 双模型入口，负责保存当前帧、调用 `defect-classify`、调用 `defect-segment`、上传 source/annotated 图片并追加历史记录 |
| `qml/Main.qml` | 首页右侧结果面板保留“检测”按钮，显示模型零件名、GOOD/BAD、类别、百分制置信度、good/bad 总概率和双模型总耗时；历史页显示原始图片和 UNet raw/overlay/mask |
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
| 检测结果 | QML 结果面板和历史页 | 首页分阶段显示：分类模型完成后显示模型零件名、GOOD/BAD、类别名、百分制置信度和 good/bad 总概率；UNet 完成后显示 `total_time_ms` 双模型耗时；上传完成后历史详情保留云端状态，并把分类/分割模型原始行转换为普通中文检测结论、可信度和缺陷提示。 |

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
| SSH 双模型检测 | 开发板 SSH | `cd /root/qt_camera_display && ./qt_camera_display --detect-self-test` | 输出 `RESULT status=GOOD|BAD ... segment_status=OK|NG ... upload_status=OK|FAIL`，并追加 `/mnt/sdcard/images/upload_history.json` | 查 `/tmp/uvc-kms-overlay-control.sock`、overlay 是否启动、两个模型运行库是否缺失、COS 账号和网络 |
| 首页按钮检测 | 触摸屏/开发板 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh start` 后点击“检测” | 分类模型完成后右侧面板先显示模型零件名、良品/坏品、类别和百分制置信度；UNet 完成后再显示双模型总耗时；上传完成后历史页新增一条包含 4 张图片的检测记录。 | 查 `/tmp/uvc-kms-overlay-control.sock`、overlay 是否启动、模型运行库是否缺失；若结果仍等上传后才显示，确认 Qt 二进制包含 `detectClassificationReady` 和 `detectModelsReady`。 |
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
