/*
 * main.cpp
 *
 * 作用：
 *   STM32MP157 工业缺陷检测 Qt 主界面的程序入口。
 *   这个入口负责设置 Qt Quick 的 OpenGL ES 渲染环境、解析摄像头设备参数、
 *   加载 QML 主界面，并以全屏方式显示到 7 寸 1024x600 屏幕。
 *
 * 主要流程：
 *   1. 在 QGuiApplication 创建前设置 OpenGL ES 属性，避免退回软件渲染。
 *   2. 初始化 GStreamer，必要时创建 qmlglsink 视频管线。
 *   3. 通过命令行解析 --camera、--video-backend 和 --windowed，便于板端调试。
 *   4. 把摄像头设备节点和视频后端传给 QML，让界面选择安全 V4L2 预览或 GL 视频预览。
 *   5. 加载 qrc:/qml/Main.qml，并默认全屏显示工业检测主界面。
 *
 * 参数：
 *   --camera /dev/video0  指定 UVC 摄像头设备，默认 /dev/video0。
 *   --width 320           指定 V4L2 采集宽度，默认 320。
 *   --height 240          指定 V4L2 采集高度，默认 240。
 *   --fps 10              指定 V4L2 采集帧率，默认 10。
 *   --video-backend qt-safe  使用自定义 V4L2VideoItem 安全预览，默认值。
 *   --video-backend gst-qml  使用 GStreamer qmlglsink 嵌入 Qt Quick。
 *   --video-backend kms-overlay  使用外部 DRM/KMS overlay plane 显示视频，Qt 只绘制界面壳。
 *   --gst-io-mode mmap    指定 gst-qml 后端的 v4l2src io-mode，默认 mmap。
 *   --storage-self-test   不启动 QML，只走 Qt 保存控制器保存一张 SD 卡图片，便于 SSH 验证按钮同路径逻辑。
 *   --detect-self-test    不启动 QML，只走双模型检测链路，便于 SSH 验证分类、UNet、上传和历史记录。
 *   --alarm-snapshot-self-test  不启动 QML，只向当天告警诊断快照文件追加一段内容，便于 SSH 验证日志落盘逻辑。
 *   --alarm-log-self-test  不启动 QML，只向当天自动告警日志文件追加一段内容，便于 SSH 验证按日归档逻辑。
 *   --settings-log-self-test  不启动 QML，只写参数日志并验证日志查看模型能扫描到该文件。
 *   --windowed            使用 1024x600 窗口模式，便于桌面或远程调试。
 *
 * 返回值：
 *   QML 加载失败返回 EXIT_FAILURE；正常进入 Qt 事件循环后返回 app.exec()。
 */

#include "v4l2_video_item.h"          /* V4L2VideoItem 提供不依赖 QtMultimedia 的 UVC 预览控件。 */
#include "defect_segment_evidence.h" /* 共用 UNet 证据默认阈值，保证 Qt 配置与推理程序一致。 */

#include <QAbstractListModel>   /* QAbstractListModel 用于把上传历史记录以模型形式暴露给 QML ListView。 */
#include <QByteArray>           /* QByteArray 用于保存串口二进制协议帧、F4 文本回包和上传脚本输出。 */
#include <QDate>                /* QDate 用于把日志、告警和历史记录归档到当天日期文件。 */
#include <QCommandLineOption>   /* QCommandLineOption 用于定义 --camera 等命令行选项。 */
#include <QCommandLineParser>   /* QCommandLineParser 负责解析用户传入的调试参数。 */
#include <QCoreApplication>     /* QCoreApplication 提供 qputenv 和应用元信息接口。 */
#include <QDateTime>            /* QDateTime 用于记录每次上传完成时的本地时间。 */
#include <QDebug>               /* QDebug/qWarning 用于输出 GStreamer 初始化失败原因。 */
#include <QDir>                 /* QDir 用于创建 SD 卡图片保存目录和历史记录目录。 */
#include <QElapsedTimer>        /* QElapsedTimer 用于统计双模型串行检测总耗时，也用于限制 F4 心跳发送周期。 */
#include <QFile>                /* QFile 用于读写上传历史 JSON 文件。 */
#include <QFileInfo>            /* QFileInfo 用于判断 COS 上传脚本、图片文件和历史文件状态。 */
#include <QGuiApplication>      /* QGuiApplication 是 Qt Quick 图形程序的应用对象。 */
#include <QHash>                /* QHash 用于声明 QML 模型角色名映射。 */
#include <QMap>                 /* QMap 用于保存 HTTP 请求头小写键值映射。 */
#include <QTimer>               /* QTimer 用于周期性异步刷新设备真实健康状态。 */
#include <QJsonArray>           /* QJsonArray 用于把历史记录数组保存到 JSON。 */
#include <QJsonDocument>        /* QJsonDocument 用于解析和生成上传历史 JSON 文档。 */
#include <QJsonObject>          /* QJsonObject 用于保存单条上传历史记录字段。 */
#include <QJsonParseError>      /* QJsonParseError 用于把参数 JSON 解析错误转换成界面可读文本。 */
#include <QJsonValue>           /* QJsonValue 用于读取历史 JSON 中的字符串或数字字段。 */
#include <QHostAddress>         /* QHostAddress 用于指定云端复核回写 HTTP 服务监听地址。 */
#include <QProcess>             /* QProcess 用于调用现有 sdcard-safe-remove 命令。 */
#include <QProcessEnvironment>  /* QProcessEnvironment 用于给 sdcard-safe-remove 传入短等待环境变量。 */
#include <QRegExp>              /* QRegExp 用于按换行解析 4g-location 的 key=value 输出。 */
#include <QSaveFile>            /* QSaveFile 用于原子写入检测参数 JSON，避免断电留下半截配置。 */
#include <QSet>                 /* QSet 用于检测多天历史 JSON 中的重复记录，避免旧文件兼容读取时重复显示。 */
#include <QMetaObject>          /* QMetaObject 用于把后台线程的检测阶段进度安全投递回 Qt 主线程。 */
#include <QMutex>               /* QMutex 用于约束 F4 普通运动帧和强制停止帧的串口写入顺序。 */
#include <QPointer>             /* QPointer 用于后台线程投递进度前判断控制器对象是否仍然存在。 */
#include <QQmlEngine>           /* qmlRegisterType 需要 Qt QML 类型系统声明。 */
#include <QQmlContext>          /* QQmlContext 用于把 C++ 变量暴露给 QML。 */
#include <QQuickItem>           /* QQuickItem 用于在 QML 树中查找 GstGLVideoItem。 */
#include <QQuickView>           /* QQuickView 用于加载并显示 QML 根界面。 */
#include <QQuickWindow>         /* QQuickWindow 提供 scheduleRenderJob，用于在渲染线程安全启动管线。 */
#include <QRunnable>            /* QRunnable 用于把 GStreamer 状态切换安排到 Qt Quick 渲染阶段。 */
#include <QSharedPointer>       /* QSharedPointer 用于在线程完成信号中安全保存后台任务结果。 */
#include <QStringList>          /* QStringList 用于保存一次检测中的多张 annotated 结果图路径。 */
#include <QSurfaceFormat>       /* QSurfaceFormat 用于声明 OpenGL ES 渲染格式。 */
#include <QTcpServer>           /* QTcpServer 用于接收云端回写板端复核结果的 HTTP 请求。 */
#include <QTcpSocket>           /* QTcpSocket 用于读取 HTTP 请求并写回 JSON 响应。 */
#include <QTextStream>          /* QTextStream 用于自检入口输出保存结果，也用于写告警诊断文本。 */
#include <QThread>              /* QThread 用于把图片保存和 COS 上传放到后台线程，避免阻塞 Qt 触摸事件循环。 */
#include <QUrl>                 /* QUrl 用于表达 qrc 资源中的 QML 路径。 */
#include <QVariantList>         /* QVariantList 用于把多张历史图片作为数组返回给 QML。 */
#include <QVariantMap>          /* QVariantMap 用于向 QML 返回当前选中历史记录详情。 */
#include <QVector>              /* QVector 用于保存内存中的上传历史记录列表。 */
#include <algorithm>            /* std::stable_sort 用于把跨日期历史记录按上传时间重新排成时间顺序。 */
#include <atomic>               /* std::atomic 用于跨后台线程记录强制 STOP 代际，取消未写出的旧运动命令。 */
#include <cmath>                /* std::isfinite 用于校验 QML/JSON 传入的 32 位位置步数是否为有效数字。 */
#include <cstdlib>              /* EXIT_SUCCESS/EXIT_FAILURE 是 main 返回值语义。 */
#include <ctime>                /* tzset 用于让运行时立刻重新读取 TZ 时区变量。 */
#include <functional>           /* std::function 用于给检测同步流程注入“分类完成/双模型完成”进度回调。 */

#include <gst/gst.h>            /* GStreamer C API 用于创建 v4l2src->glupload->qmlglsink 管线。 */

#include <cerrno>               /* errno 保存 Unix socket 调用失败原因。 */
#include <cstdio>               /* fopen/fscanf/fclose 用于可靠读取 procfs；stdout 用于自检入口输出保存结果。 */
#include <cstring>              /* strerror 用于把 errno 转成人可读文本。 */
#include <fcntl.h>              /* open/O_NOCTTY 用于后台 F4 串口握手检测。 */
#include <termios.h>            /* termios 用于配置 F4 串口 57600 8N1 原始模式。 */

#include <sys/socket.h>         /* socket/connect 负责与 overlay 控制端点通信。 */
#include <sys/un.h>             /* sockaddr_un 描述 Unix domain socket 地址。 */
#include <sys/select.h>         /* select 用于给 F4 串口握手设置短超时，避免线程长时间阻塞。 */
#include <unistd.h>             /* close/read/write 处理 socket 文件描述符。 */

/* 默认摄像头设备节点：当前 UVC 摄像头已经验证通常枚举为 /dev/video0。 */
static const char *DEFAULT_CAMERA_DEVICE = "/dev/video0";

/* 默认采集宽度：先用 320，减少 USB/V4L2 拷贝和 OpenGL 纹理上传压力。 */
static const int DEFAULT_CAPTURE_WIDTH = 320;

/* 默认采集高度：先用 240，和 320 宽度组成 4:3 预览画面。 */
static const int DEFAULT_CAPTURE_HEIGHT = 240;

/* 默认采集帧率：10fps 足够调试工业检测界面，同时明显降低 CPU 占用。 */
static const int DEFAULT_CAPTURE_FPS = 10;

/* Qt 安全预览后端名称：使用自定义 V4L2VideoItem，保留稳定兜底路线。 */
static const char *BACKEND_QT_SAFE = "qt-safe";

/* GStreamer QML GL 后端名称：使用 qmlglsink 把 GL 视频嵌入 Qt Quick。 */
static const char *BACKEND_GST_QML = "gst-qml";

/* KMS overlay 后端名称：摄像头由外部 DRM plane 进程显示，Qt 只负责 UI 与状态。 */
static const char *BACKEND_KMS_OVERLAY = "kms-overlay";

/* gst-qml 默认采集模式：mmap 会让 glupload 创建普通 GL 纹理，避开 Vivante 绘制 DMABUF 纹理时的用户态段错误。 */
static const char *DEFAULT_GST_IO_MODE = "mmap";

/* overlay 控制 socket 默认路径，需要与 uvc_kms_overlay.c 保持一致。 */
static const char *DEFAULT_OVERLAY_CONTROL_SOCKET = "/tmp/uvc-kms-overlay-control.sock";

/* overlay 控制 socket 连接超时，单位 ms；连接阶段只判断进程是否接收连接，保持短等待避免健康刷新卡住后台线程。 */
static const int OVERLAY_CONTROL_CONNECT_TIMEOUT_MS = 150;

/* overlay STATUS 回复超时，单位 ms；STATUS 只读短状态行，继续保持短超时用于快速判定相机在线状态。 */
static const int OVERLAY_CONTROL_STATUS_REPLY_TIMEOUT_MS = 150;

/* overlay LOCATE 回复超时，单位 ms；自动检测启动后 LOCATE 接近 10fps 帧周期，必须允许跨过一到数帧的采集/转换/定位抖动。 */
static const int OVERLAY_CONTROL_LOCATE_REPLY_TIMEOUT_MS = 600;

/* overlay 单次 read 缓冲大小；真实完整性由换行判断，这里只控制每次从 socket 取多少字节。 */
static const int OVERLAY_CONTROL_REPLY_CHUNK_SIZE = 512;

/* overlay 回复最大缓存，单位字节；覆盖 SAVE 路径和 LOCATE 诊断长行，防止异常端点无限输出撑爆内存。 */
static const int OVERLAY_CONTROL_MAX_REPLY_BYTES = 8192;

/* 默认 overlay 启动控制脚本；相机热拔插恢复时只后台重启 overlay 进程，不重启 Qt 界面。 */
static const char *DEFAULT_OVERLAY_RESTART_SCRIPT = "/root/qt_camera_display/run_qt_kms_overlay_display.sh";

/* SD 卡默认挂载点，保存按钮只允许写入这个挂载点下的 images 目录。 */
static const char *DEFAULT_SDCARD_MOUNT_POINT = "/mnt/sdcard";

/* SD 卡图片保存目录，overlay 收到 SAVE 请求后会在这里生成 PPM 图片。 */
static const char *DEFAULT_SDCARD_IMAGE_DIR = "/mnt/sdcard/images";

/* SD 卡诊断日志目录，告警维护页保存诊断和自动告警日志时会把文本写到这里。 */
static const char *DEFAULT_SDCARD_LOG_DIR = "/mnt/sdcard/logs";

/* 检测参数默认保存文件；参数页保存后，下一次启动会从这里恢复真实检测配置。 */
static const char *DEFAULT_DETECT_SETTINGS_FILE = "/mnt/sdcard/config/defect_ui_config.json";

/* ALARM_SNAPSHOT_PREFIX 是诊断快照文件名前缀，实际文件名会追加当天日期，同一天追加到同一个文件。 */
static const char *ALARM_SNAPSHOT_PREFIX = "qt_alarm_snapshot";

/* ALARM_LOG_PREFIX 是自动告警日志文件名前缀，实际文件名会追加当天日期，同一天所有告警追加到同一个文件。 */
static const char *ALARM_LOG_PREFIX = "qt_alarm";

/* SETTINGS_LOG_PREFIX 是参数设置日志文件名前缀，保存配置和导出摘要都会追加到同一天日志。 */
static const char *SETTINGS_LOG_PREFIX = "qt_settings";

/* 板端 COS 上传脚本默认部署路径，保存按钮会在本地 JPG/PNG 落盘后调用它。 */
static const char *DEFAULT_COS_UPLOAD_SCRIPT = "/root/qt_camera_display/defect-cos-upload";

/* 默认 4G PPP 管理脚本；健康检测只调用 test，不在界面线程里执行 start/restart。 */
static const char *DEFAULT_4G_PPP_SCRIPT = "4g-ppp";

/* 默认 4G IP 省份定位脚本；脚本内部只调用高德 IP 定位接口，不访问 GPS 或 AT 串口。 */
static const char *DEFAULT_4G_LOCATION_SCRIPT = "4g-location";

/* 高德 IP 定位短超时，单位毫秒；首次开机定位允许 HTTPS 请求和 4G 弱网有更长等待。 */
static const int LOCATION_PROBE_TIMEOUT_MS = 40000;

/* 默认云端健康地址；与 defect-cos-upload 的默认后端保持一致。 */
static const char *DEFAULT_CLOUD_HEALTH_URL = "http://139.9.35.72/health";

/* 默认 F4 串口节点；真实接入时只有握手成功才显示接入。 */
static const char *DEFAULT_F4_SERIAL_DEVICE = "/dev/ttySTM2";

/* 默认 F4 串口波特率；当前 MP157-F4 主链路约定使用 57600 8N1。 */
static const int DEFAULT_F4_SERIAL_BAUD = 57600;

/* F4 心跳发送间隔，单位毫秒；120000ms 等于 2 分钟，避免 Qt 每 8 秒健康刷新都占用 RS485 串口。 */
static const int F4_HEARTBEAT_INTERVAL_MS = 120000;

/* 自动视觉居中死区，单位像素；需要与 QML 和 F4 当前中心死区保持同量级。 */
static const int AUTO_VISION_CENTER_TOLERANCE_PX = 24;

/* 自动视觉居中后建议 F4 保持静止时间，单位毫秒；给相机对焦和模型检测留出稳定窗口。 */
static const int AUTO_VISION_CENTER_HOLD_MS = 2000;

/* 二进制协议帧头第 1 字节，固定 0xA5，用于从串口字节流中快速寻找帧起点。 */
static const quint8 BINARY_PROTOCOL_SOF0 = 0xA5U;

/* 二进制协议帧头第 2 字节，固定 0x5A，用于降低噪声误判成帧头的概率。 */
static const quint8 BINARY_PROTOCOL_SOF1 = 0x5AU;

/* 二进制协议版本号，当前 MP157 和 F407 约定首版为 0x01。 */
static const quint8 BINARY_PROTOCOL_VERSION = 0x01U;

/* 二进制协议帧尾，固定 0x6B，便于和 F407 侧协议解析状态机对齐。 */
static const quint8 BINARY_PROTOCOL_EOF = 0x6BU;

/* 二进制协议首版最大负载长度，和 F407 侧 64 字节级接收缓存保持余量。 */
static const int BINARY_PROTOCOL_MAX_PAYLOAD = 48;

/* 二进制协议最短固定帧长度，LEN=0 时仍包含帧头、版本、命令、长度、SEQ、CRC 和帧尾。 */
static const int BINARY_PROTOCOL_MIN_FRAME_SIZE = 10;

/* 自动流程开始命令：F4 收到后启动传送带扫描，等待零件进入相机视野。 */
static const quint8 BINARY_PROTOCOL_CMD_START_CYCLE = 0x10U;

/* 自动流程暂停命令：F4 收到后尽量进入安全静止点，并保留当前 cycle_id。 */
static const quint8 BINARY_PROTOCOL_CMD_PAUSE_CYCLE = 0x11U;

/* 自动流程继续命令：F4 收到后从暂停状态恢复同一个 cycle_id。 */
static const quint8 BINARY_PROTOCOL_CMD_RESUME_CYCLE = 0x12U;

/* 自动流程停止命令：F4 收到后停止传送带和可停止执行器，并作废本轮 cycle_id。 */
static const quint8 BINARY_PROTOCOL_CMD_STOP_CYCLE = 0x13U;

/* 视觉坐标命令：MP157 周期发送零件在原始帧中的位置，F4 据此调节传送带速度和方向。 */
static const quint8 BINARY_PROTOCOL_CMD_VISION_POS = 0x20U;

/* 视觉丢失命令：MP157 暂时没看到零件或置信度不足时通知 F4 回扫描或停机保护。 */
static const quint8 BINARY_PROTOCOL_CMD_VISION_LOST = 0x21U;

/* 居中停止命令：MP157 判断零件已稳定进入中心 ROI 后要求 F4 停传送带并保持。 */
static const quint8 BINARY_PROTOCOL_CMD_BELT_STOP_CENTERED = 0x22U;

/* 二进制心跳命令：MP157 周期确认 F4 在线，成功只看 ACK，不再解析 STATUS 文本。 */
static const quint8 BINARY_PROTOCOL_CMD_HEARTBEAT = 0x02U;

/* 二进制称重标定命令：MP157 发送已知砝码克重，F407 调用 HX711 标定入口并返回 ACK/NACK。 */
static const quint8 BINARY_PROTOCOL_CMD_WEIGHT_CALIBRATE = 0x30U;

/* 机械臂任务命令：Z 轴回升完成后，MP157 请求 F4 通知 ESP32S3 把零件依次放到称重和电感模块。 */
static const quint8 BINARY_PROTOCOL_CMD_ARM_JOB_START = 0x31U;

/* 模型完成命令：MP157 模型检测和 SD 卡保存完成后下发，F4 只缓存模型结果，不触发最终分拣。 */
static const quint8 BINARY_PROTOCOL_CMD_MODEL_READY = 0x32U;

/* 最终分拣命令：MP157 完整上传图片、模型、重量和电感数据后下发，F4 才通知 ESP32S3 放入对应盘。 */
static const quint8 BINARY_PROTOCOL_CMD_FINAL_SORT_RESULT = 0x33U;

/* 二进制状态查询命令：F4 成功时返回 STATUS_REPORT，失败时返回 NACK。 */
static const quint8 BINARY_PROTOCOL_CMD_QUERY_STATUS = 0x40U;

/* 二进制手动传送带命令：手动页扫描/停止只走 ACK/NACK，不再发送 BELTSCAN/BELTSTOP 文本。 */
static const quint8 BINARY_PROTOCOL_CMD_BELT_MANUAL_CONTROL = 0x41U;

/* 二进制步进电机参数命令：参数页保存后把三台 Emm42 的地址、步长、速度和方向下发给 F407。 */
static const quint8 BINARY_PROTOCOL_CMD_STEPPER_PARAM_SET = 0x42U;

/* 二进制执行器位置运动命令：用于让 F4 以 Emm42 位置模式控制传送带、左右轴或上下轴移动固定步数。 */
static const quint8 BINARY_PROTOCOL_CMD_ACTUATOR_POS_MOVE = 0x50U;

/* 执行器位置运动命令负载长度：cycle_id2 + actuator1 + direction1 + mode1 + speed2 + steps4 + flags1。 */
static const int BINARY_PROTOCOL_ACTUATOR_POS_MOVE_PAYLOAD_SIZE = 12;

/* 二进制执行器停止命令：用于手动急停或停止指定执行器，actuator=0xFF 表示全部可停止执行器。 */
static const quint8 BINARY_PROTOCOL_CMD_ACTUATOR_STOP = 0x51U;

/* 二进制执行器速度运动命令：用于手动调试时让传送带或摄像头左右轴持续运动，直到 STOP。 */
static const quint8 BINARY_PROTOCOL_CMD_ACTUATOR_VEL_MOVE = 0x52U;

/* 二进制执行器设零命令：用于参数设置页把当前电机位置设为新的零点，不主动运动。 */
static const quint8 BINARY_PROTOCOL_CMD_ACTUATOR_HOME = 0x53U;

/* F4_ACTUATOR_STOP_NOW_REPEAT_COUNT 是手动停止键重复写入 STOP 帧的次数；STOP 幂等，重复写入能覆盖串口竞争窗口。 */
static const int F4_ACTUATOR_STOP_NOW_REPEAT_COUNT = 3;

/* F4_ACTUATOR_STOP_NOW_REPEAT_DELAY_US 是重复 STOP 帧之间的短间隔，单位 us，给 F4 USART1 任务留出取帧时间。 */
static const int F4_ACTUATOR_STOP_NOW_REPEAT_DELAY_US = 20000;

/* 二进制协议 ACK 命令：F4 用它确认关键命令已被接收并接受。 */
static const quint8 BINARY_PROTOCOL_CMD_ACK = 0x80U;

/* 二进制协议 NACK 命令：F4 用它拒绝命令并返回错误码、状态和细节。 */
static const quint8 BINARY_PROTOCOL_CMD_NACK = 0x81U;

/* 二进制状态回包命令：F4 用固定 24 字节负载返回协议状态和传送带状态。 */
static const quint8 BINARY_PROTOCOL_CMD_STATUS_REPORT = 0x82U;

/* 二进制事件上报命令：F4 主动告诉 MP157 当前机械臂、称重或电感流程阶段。 */
static const quint8 BINARY_PROTOCOL_CMD_EVENT_REPORT = 0x83U;

/* 执行器位置运动完成事件：F4 收到张大头主动到位回包或估算运动完成后发送，status 字段区分来源。 */
static const quint8 BINARY_PROTOCOL_EVENT_ACTUATOR_MOVE_DONE = 0x14U;

/* 执行器位置运动超时事件：Response 未配置、RX 接线异常、地址错误或堵转时由 F4 发送。 */
static const quint8 BINARY_PROTOCOL_EVENT_ACTUATOR_MOVE_TIMEOUT = 0x15U;

/* 执行器位置运动完成状态：0 表示 F4 收到张大头 Emm42 主动到位回包 `[addr FD 9F 6B]`。 */
static const quint16 F4_ACTUATOR_MOVE_STATUS_REACHED_ACK = 0x0000U;

/* 执行器位置运动完成状态：5 表示 F4 未等到主动回包，但按速度、步数和安全余量估算运动已经结束。 */
static const quint16 F4_ACTUATOR_MOVE_STATUS_ESTIMATED_DONE = 0x0005U;

/* MP157 本地估算每圈步数：张大头 Emm42 默认细分折算为 200 step/rev，用于 F4 事件丢失时兜底。 */
static const int MP157_ACTUATOR_FALLBACK_STEPS_PER_REV = 200;

/* MP157 本地估算安全余量：覆盖 F4 转发、驱动器加减速和机构惯性，单位 ms。 */
static const int MP157_ACTUATOR_FALLBACK_SAFETY_MS = 900;

/* MP157 本地估算短稳定时间：运动结束后给画面和机构一个最小稳定窗口，单位 ms。 */
static const int MP157_ACTUATOR_FALLBACK_SETTLE_MS = 450;

/* MP157 本地估算最短等待：小步数也不能马上推进 ROI 复查，单位 ms。 */
static const int MP157_ACTUATOR_FALLBACK_MIN_MS = 1200;

/* MP157 本地估算默认最长等待：保留比 C++ 70 秒串口等待略短的上限，避免后台线程永久占用。 */
static const int MP157_ACTUATOR_FALLBACK_MAX_MS = 65000;

/* MP157 Z 轴参数页可配置超时默认值：旧现场流程使用 10 秒，缺少 JSON 字段时继续沿用这个安全值。 */
static const int MP157_CAMERA_Z_TIMEOUT_DEFAULT_MS = 10000;

/* MP157 Z 轴参数页可配置超时下限：小于 1 秒的等待容易让相机还没稳定就进入检测。 */
static const int MP157_CAMERA_Z_TIMEOUT_MIN_MS = 1000;

/* MP157 Z 轴参数页可配置超时上限：保持低于 C++ ACTUATOR_POS_MOVE 硬等待窗口，避免串口线程长时间占用。 */
static const int MP157_CAMERA_Z_TIMEOUT_MAX_MS = 60000;

/* 摄像头左右轴 ROI 位置微调默认步数：缺少 JSON 字段时每次走 20 step，避免继续依赖速度模式持续转动。 */
static const quint32 MP157_CAMERA_LATERAL_FINE_TUNE_DEFAULT_STEPS = 20U;

/* 摄像头左右轴 ROI 位置微调最小步数：F4 位置模式不接受 0 step，因此参数页必须至少为 1 step。 */
static const quint32 MP157_CAMERA_LATERAL_FINE_TUNE_MIN_STEPS = 1U;

/* 摄像头左右轴 ROI 位置微调最大步数：限制到 10000 step，避免误输入造成一次横移过大。 */
static const quint32 MP157_CAMERA_LATERAL_FINE_TUNE_MAX_STEPS = 10000U;

/* 二进制称重结果命令：F4 读取 HX711 稳定结果后主动上报给 MP157。 */
static const quint8 BINARY_PROTOCOL_CMD_WEIGHT_RESULT = 0x84U;

/* 二进制电感结果命令：F4 读取 LDC1614 电磁感应结果后主动上报给 MP157。 */
static const quint8 BINARY_PROTOCOL_CMD_LDC_RESULT = 0x85U;

/* 二进制整轮完成命令：F4 最终分拣完成后主动上报给 MP157，并等待 ACK。 */
static const quint8 BINARY_PROTOCOL_CMD_CYCLE_DONE = 0x86U;

/* 二进制故障上报命令：F4 用固定 16 字节负载返回 LDC、称重、电机等结构化错误。 */
static const quint8 BINARY_PROTOCOL_CMD_FAULT_REPORT = 0x87U;

/* F4 机械臂主动结果等待默认窗口，单位毫秒。
 * 这个值只是参数页和 JSON 缺省值；真实等待时间必须从 DetectSettingsSnapshot 传入。
 * 默认 75000ms 大于 F4 当前 60000ms 动作超时和 5000ms DONE 串口余量，避免 MP157 提前超时。
 */
static const int F4_ARM_ACTIVE_FRAME_TIMEOUT_DEFAULT_MS = 75000;

/* F4 机械臂主动结果等待最小窗口，单位毫秒；低于 10 秒容易把正常抓取误判为超时。 */
static const int F4_ARM_ACTIVE_FRAME_TIMEOUT_MIN_MS = 10000;

/* F4 机械臂主动结果等待最大窗口，单位毫秒；限制到 300 秒，给 ESP32S3 机械臂慢动作和传感器稳定留足余量。 */
static const int F4_ARM_ACTIVE_FRAME_TIMEOUT_MAX_MS = 300000;

/* 板端缺陷分类推理程序默认路径，首页“检测”按钮会通过 QProcess 调用它。 */
static const char *DEFAULT_DEFECT_CLASSIFY_BIN = "/root/qt_camera_display/defect-classify";

/* 板端 INT8 ONNX 模型默认路径，部署脚本会从 Windows/VM 模型目录复制到这里。 */
static const char *DEFAULT_DEFECT_CLASSIFY_MODEL =
    "/root/qt_camera_display/models/defect_classifier_static_mixed_int8.onnx";

/* 板端标签映射默认路径，类别顺序必须和 ONNX 输出完全一致。 */
static const char *DEFAULT_DEFECT_CLASSIFY_LABELS =
    "/root/qt_camera_display/models/defect_classifier_static_mixed_int8_labels.json";

/* 板端 UNet 分割推理程序默认路径，首页“检测”按钮会在分类结束后通过 QProcess 调用它。 */
static const char *DEFAULT_DEFECT_SEGMENT_BIN = "/root/qt_camera_display/defect-segment";

/* 板端 UNet INT8 ONNX 模型默认路径，必须和部署脚本复制位置一致。 */
static const char *DEFAULT_DEFECT_SEGMENT_MODEL =
    "/root/qt_camera_display/models/defect_unet_test_decoder_head_int8.onnx";

/* 上传历史默认文件：保留旧版固定文件路径作为兼容读取入口，新写入使用 upload_history_YYYYMMDD.json。 */
static const char *DEFAULT_UPLOAD_HISTORY_FILE = "/mnt/sdcard/images/upload_history.json";

/* 上传历史每日文件名前缀：检测历史按自然日归档为 upload_history_YYYYMMDD.json。 */
static const char *UPLOAD_HISTORY_DAILY_PREFIX = "upload_history";

/* 上传历史每日文件扩展名：和旧版 upload_history.json 一样仍然保存 JSON 数组。 */
static const char *UPLOAD_HISTORY_DAILY_SUFFIX = ".json";

/* 云端复核回写默认监听地址；0.0.0.0 便于云端通过板端 IP 访问，现场可用 BOARD_REVIEW_LISTEN 收紧。 */
static const char *DEFAULT_BOARD_REVIEW_LISTEN = "0.0.0.0";

/* 云端复核回写默认端口；云端按钮调用 http://<board-ip>:18080/api/v1/review-result。 */
static const quint16 DEFAULT_BOARD_REVIEW_PORT = 18080;

/* 云端复核回写默认来源标记；写入历史 JSON 后可区分人工云端复核和板端本地模型输出。 */
static const char *DEFAULT_BOARD_REVIEW_SOURCE = "cloud";

/* Qt 界面默认业务时区：POSIX TZ 中 CST-8 表示 UTC+8，也就是北京时间。 */
static const char *DEFAULT_BOARD_TIME_ZONE = "CST-8";

/*
 * StepperMotorSettings 的作用：
 *   保存参数页中一台张大头 Emm42 步进电机的现场可调参数。
 *
 * 字段说明：
 *   name 是界面显示名称，例如传送带电机或摄像头左右电机。
 *   role 是稳定英文角色名，用于 JSON 和后续 F4 参数下发协议识别电机。
 *   serialName 是 F4 侧串口归属，帮助现场确认 UART4 或 USART6 接线。
 *   address 是 Emm42 从机地址，也就是用户口语里的电机 ID 地址。
 *   minStep 是单次点动或闭环微调的最小步长，单位为 step。
 *   normalSpeedRpm 是常规/对中运动速度，单位为 rpm，允许 0~5000 的现场任意整数配置。
 *   scanSpeedRpm 是传送带专用上料扫描速度，单位为 rpm；零件尚未入画时 F4 用它驱动传送带。
 *   direction 是方向映射，1 表示正向，-1 表示反向，用于现场坐标越调越远时快速反转。
 *   lateralFineTuneFixedSteps 是摄像头左右轴 ROI 位置模式每次微调的固定步数，单位 step。
 *   zDownFixedSteps 是上下电机自动检测前下探的固定相对位置步数，单位为 step。
 *   zUpFixedSteps 是上下电机模型检测后回升的固定相对位置步数，单位为 step。
 *   zMotionTimeoutMs 是 MP157 等待上下电机下降/回升 DONE 或本地估算完成的最大时间，单位 ms。
 */
struct StepperMotorSettings
{
    QString name;
    QString role;
    QString serialName;
    int address = 1;
    int minStep = 10;
    int normalSpeedRpm = 40;
    int scanSpeedRpm = 40;
    int direction = 1;
    quint32 lateralFineTuneFixedSteps = 0U;
    quint32 zDownFixedSteps = 0U;
    quint32 zUpFixedSteps = 0U;
    int zMotionTimeoutMs = MP157_CAMERA_Z_TIMEOUT_DEFAULT_MS;
};

/*
 * defaultStepperMotorSettings 的作用：
 *   返回三台步进电机的比赛现场推荐默认参数。
 *
 * 主要流程：
 *   1. 传送带电机固定使用 F4 UART4、地址 0x01。
 *   2. 摄像头左右和上下电机共用 F4 USART6，现场实物当前为左右轴 0x03、上下轴 0x02，避免两个轴同时响应。
 *   3. 速度和步长先给保守值，真实下发前仍必须由 F4 固件做限幅和联锁保护。
 *
 * 返回值：
 *   返回包含三台电机配置的 QVector，顺序与弹窗三页一致。
 */
static QVector<StepperMotorSettings> defaultStepperMotorSettings()
{
    QVector<StepperMotorSettings> motors;

    StepperMotorSettings beltMotor;
    beltMotor.name = QStringLiteral("传送带电机");
    beltMotor.role = QStringLiteral("conveyor");
    beltMotor.serialName = QStringLiteral("UART4 PC10/PC11");
    beltMotor.address = 1;
    beltMotor.minStep = 20;
    beltMotor.normalSpeedRpm = 40;
    beltMotor.scanSpeedRpm = 40;
    beltMotor.direction = 1;
    motors.append(beltMotor);

    StepperMotorSettings cameraLateralMotor;
    cameraLateralMotor.name = QStringLiteral("摄像头左右电机");
    cameraLateralMotor.role = QStringLiteral("camera_lateral");
    cameraLateralMotor.serialName = QStringLiteral("USART6 PC6/PC7");
    cameraLateralMotor.address = 3;
    cameraLateralMotor.minStep = 5;
    cameraLateralMotor.normalSpeedRpm = 120;
    cameraLateralMotor.scanSpeedRpm = 0;
    cameraLateralMotor.direction = 1;
    cameraLateralMotor.lateralFineTuneFixedSteps = MP157_CAMERA_LATERAL_FINE_TUNE_DEFAULT_STEPS;
    motors.append(cameraLateralMotor);

    StepperMotorSettings cameraZMotor;
    cameraZMotor.name = QStringLiteral("摄像头上下电机");
    cameraZMotor.role = QStringLiteral("camera_z");
    cameraZMotor.serialName = QStringLiteral("USART6 PC6/PC7");
    cameraZMotor.address = 2;
    cameraZMotor.minStep = 5;
    cameraZMotor.normalSpeedRpm = 80;
    cameraZMotor.scanSpeedRpm = 0;
    cameraZMotor.direction = 1;
    cameraZMotor.zDownFixedSteps = 800U;
    cameraZMotor.zUpFixedSteps = 800U;
    cameraZMotor.zMotionTimeoutMs = MP157_CAMERA_Z_TIMEOUT_DEFAULT_MS;
    motors.append(cameraZMotor);

    return motors;
}

/*
 * stepperDirectionText 的作用：
 *   把方向映射值转换成界面可读中文。
 *
 * 参数：
 *   direction 是方向映射，非负数显示正向，负数显示反向。
 *
 * 返回值：
 *   返回“正向”或“反向”。
 */
static QString stepperDirectionText(int direction)
{
    return direction >= 0 ? QStringLiteral("正向") : QStringLiteral("反向");
}

/*
 * stepperMotorToVariantMap 的作用：
 *   把单台步进电机参数转换成 QML 可以直接读取的 QVariantMap。
 *
 * 参数：
 *   motor 是 C++ 内部保存的一台电机参数。
 *   index 是电机在三页弹窗中的页序号。
 *
 * 返回值：
 *   返回包含名称、角色、串口、地址、速度、步长和方向文本的 map。
 */
static QVariantMap stepperMotorToVariantMap(const StepperMotorSettings &motor, int index)
{
    QVariantMap map;

    map.insert(QStringLiteral("index"), index);
    map.insert(QStringLiteral("name"), motor.name);
    map.insert(QStringLiteral("role"), motor.role);
    map.insert(QStringLiteral("serialName"), motor.serialName);
    map.insert(QStringLiteral("address"), motor.address);
    map.insert(QStringLiteral("addressHex"), QStringLiteral("0x%1")
        .arg(motor.address, 2, 16, QLatin1Char('0')).toUpper());
    map.insert(QStringLiteral("minStep"), motor.minStep);
    map.insert(QStringLiteral("normalSpeedRpm"), motor.normalSpeedRpm);
    map.insert(QStringLiteral("scanSpeedRpm"), motor.scanSpeedRpm);
    map.insert(QStringLiteral("direction"), motor.direction);
    map.insert(QStringLiteral("directionText"), stepperDirectionText(motor.direction));
    map.insert(QStringLiteral("lateralFineTuneFixedSteps"), static_cast<double>(motor.lateralFineTuneFixedSteps));
    map.insert(QStringLiteral("zDownFixedSteps"), static_cast<double>(motor.zDownFixedSteps));
    map.insert(QStringLiteral("zUpFixedSteps"), static_cast<double>(motor.zUpFixedSteps));
    map.insert(QStringLiteral("zMotionTimeoutMs"), motor.zMotionTimeoutMs);
    return map;
}

/*
 * stepperMotorSettingsToVariantList 的作用：
 *   把三台步进电机配置转换成 QML Repeater 可消费的数组。
 *
 * 参数：
 *   motors 是当前已清洗的电机参数列表。
 *
 * 返回值：
 *   返回 QVariantList，每个元素都是 stepperMotorToVariantMap() 生成的 QVariantMap。
 */
static QVariantList stepperMotorSettingsToVariantList(const QVector<StepperMotorSettings> &motors)
{
    QVariantList list;

    for (int index = 0; index < motors.size(); ++index) {
        list.append(stepperMotorToVariantMap(motors.at(index), index));
    }

    return list;
}

/*
 * DetectSettingsSnapshot 的作用：
 *   保存参数设置页真正参与检测链路的配置快照。
 *
 * 字段说明：
 *   partType 是界面选择的真实零件中文名，当前用于摘要和历史诊断，不直接改模型类别顺序。
 *   modelThreshold 是分类模型 bad_total 判坏阈值，传给 defect-classify 的 --bad-threshold。
 *   reviewThreshold 是综合判定的复核阈值，分类置信度低于该值时进入 REVIEW。
 *   roiSize 是分类和 UNet 使用的中心 ROI 边长，传给两个模型程序的 --roi。
 *   segmentMinComponentPixels 是 UNet 过滤孤立小连通域的面积阈值。
 *   segmentReviewPixels 是 UNet 从 CLEAR 进入 WEAK 待复核区的过滤后总像素阈值。
 *   segmentBadPixels 是 UNet 进入 STRONG 的过滤后总像素阈值。
 *   segmentStrongComponentPixels 是 UNet 进入 STRONG 的最大连通域面积阈值。
 *   overlayAlpha 是 UNet 叠加图透明度，传给 defect-segment 的 --alpha。
 *   autoUploadEnabled 为 false 时检测仍写本地历史，但跳过 COS 上传并返回 upload_status=SKIP。
 *   f4ArmResultTimeoutMs 是 MP157 等待 F4 主动 WEIGHT_RESULT/LDC_RESULT/CYCLE_DONE 的最大窗口，单位 ms。
 *   stepperMotors 保存三台步进电机的地址、最小步长、常规速度、方向和上下轴固定位置步数配置。
 */
struct DetectSettingsSnapshot
{
    QString partType = QStringLiteral("波形垫圈");
    double modelThreshold = 0.85;
    double reviewThreshold = 0.65;
    int roiSize = 300;
    int segmentMinComponentPixels = defect_segment_evidence::kDefaultMinComponentPixels;
    int segmentReviewPixels = defect_segment_evidence::kDefaultReviewPixels;
    int segmentBadPixels = defect_segment_evidence::kDefaultBadPixels;
    int segmentStrongComponentPixels = defect_segment_evidence::kDefaultStrongComponentPixels;
    double overlayAlpha = 0.45;
    bool autoUploadEnabled = true;
    int f4ArmResultTimeoutMs = F4_ARM_ACTIVE_FRAME_TIMEOUT_DEFAULT_MS;
    QVector<StepperMotorSettings> stepperMotors = defaultStepperMotorSettings();
};

/*
 * clampedDouble 的作用：
 *   把 JSON 或 QML 传入的小数限制在指定闭区间，避免坏配置进入模型命令行。
 *
 * 参数：
 *   value 是待限制的输入值。
 *   low/high 是允许的最小值和最大值。
 *
 * 返回值：
 *   返回已经限制到 [low, high] 的 double。
 */
static double clampedDouble(double value, double low, double high)
{
    return std::max(low, std::min(high, value));
}

/*
 * clampedInt 的作用：
 *   把 JSON 或 QML 传入的整数限制在指定闭区间，避免 ROI 或像素阈值越界。
 *
 * 参数：
 *   value 是待限制的输入值。
 *   low/high 是允许的最小值和最大值。
 *
 * 返回值：
 *   返回已经限制到 [low, high] 的 int。
 */
static int clampedInt(int value, int low, int high)
{
    return std::max(low, std::min(high, value));
}

/*
 * clampedUInt32FromDouble 的作用：
 *   把 QML Number 或 JSON number 表示的位置模式步数限制到 Emm42 4 字节脉冲数字段范围。
 *
 * 主要流程：
 *   1. 小于 0、NaN 或无穷大都按 0 处理，避免非法 UI 文本越过 C++ 进入协议层。
 *   2. 大于 0xFFFFFFFF 的值按最大 32 位无符号数处理，和张大头 42 步进位置模式字段宽度一致。
 *   3. 合法数值向下取整，保证最终发给 F4 的 step 是整数。
 *
 * 参数：
 *   value 是 QML 或 JSON 传来的数值。
 *
 * 返回值：
 *   返回 0~4294967295 范围内的 quint32 步数。
 */
static quint32 clampedUInt32FromDouble(double value)
{
    if (!std::isfinite(value) || value <= 0.0) {
        return 0U;
    }

    if (value >= 4294967295.0) {
        return 0xFFFFFFFFU;
    }

    return static_cast<quint32>(value);
}

/*
 * clampedUInt32FromText 的作用：
 *   把 QML 数字键盘输入的十进制文本转换成 Emm42 位置模式允许的 32 位步数。
 *
 * 参数：
 *   text 是用户输入的十进制文本。
 *   ok 非空时用于返回解析是否成功；空字符串、非数字或越界文本会返回 false。
 *
 * 返回值：
 *   返回解析并限幅后的 0~4294967295 step。
 */
static quint32 clampedUInt32FromText(const QString &text, bool *ok)
{
    const QString trimmed = text.trimmed();
    bool localOk = false;
    const qulonglong rawValue = trimmed.toULongLong(&localOk, 10);

    if (!localOk || rawValue > 0xFFFFFFFFULL) {
        if (ok) {
            *ok = false;
        }
        return 0U;
    }

    if (ok) {
        *ok = true;
    }
    return static_cast<quint32>(rawValue);
}

/*
 * detectSettingsToVariantMap 的作用：
 *   把检测配置快照转换成 QML 可直接读取的 QVariantMap。
 *
 * 参数：
 *   settings 是要转换的检测配置快照。
 *
 * 返回值：
 *   返回包含分类、ROI、UNet 四阈值、透明度、上传和机械臂等待配置的 map。
 */
static QVariantMap detectSettingsToVariantMap(const DetectSettingsSnapshot &settings)
{
    QVariantMap map;

    map.insert(QStringLiteral("partType"), settings.partType);
    map.insert(QStringLiteral("modelThreshold"), settings.modelThreshold);
    map.insert(QStringLiteral("reviewThreshold"), settings.reviewThreshold);
    map.insert(QStringLiteral("roiSize"), settings.roiSize);
    map.insert(QStringLiteral("segmentMinComponentPixels"), settings.segmentMinComponentPixels);
    map.insert(QStringLiteral("segmentReviewPixels"), settings.segmentReviewPixels);
    map.insert(QStringLiteral("segmentBadPixels"), settings.segmentBadPixels);
    map.insert(QStringLiteral("segmentStrongComponentPixels"), settings.segmentStrongComponentPixels);
    map.insert(QStringLiteral("overlayAlpha"), settings.overlayAlpha);
    map.insert(QStringLiteral("autoUploadEnabled"), settings.autoUploadEnabled);
    map.insert(QStringLiteral("f4ArmResultTimeoutMs"), settings.f4ArmResultTimeoutMs);
    map.insert(QStringLiteral("stepperMotors"), stepperMotorSettingsToVariantList(settings.stepperMotors));
    return map;
}

/*
 * dateStampString 的作用：
 *   把 QDate 转成文件名中使用的 YYYYMMDD 日期段。
 *
 * 主要流程：
 *   1. 优先使用调用者传入的有效日期。
 *   2. 如果日期无效，回退到当前板端本地日期，避免生成空文件名。
 *
 * 参数：
 *   date 是要写入文件名的自然日。
 *
 * 返回值：
 *   返回 8 位日期字符串，例如 20260701。
 */
static QString dateStampString(const QDate &date)
{
    /* validDate 保存最终参与格式化的日期，无效输入说明调用方没有明确日期。 */
    const QDate validDate = date.isValid() ? date : QDate::currentDate();

    return validDate.toString(QStringLiteral("yyyyMMdd"));
}

/*
 * currentDateStampString 的作用：
 *   返回当前板端本地日期的 YYYYMMDD 字符串，统一日志和历史的每日文件命名。
 *
 * 返回值：
 *   返回 8 位日期字符串，例如 20260701。
 */
static QString currentDateStampString()
{
    return dateStampString(QDate::currentDate());
}

/*
 * dailyFilePathFromLegacyPath 的作用：
 *   根据旧版固定文件路径生成每日归档文件路径。
 *
 * 主要流程：
 *   1. 取出旧路径所在目录，例如 /mnt/sdcard/images。
 *   2. 用传入前缀、日期和后缀拼出 `<prefix>_YYYYMMDD<suffix>`。
 *   3. 返回位于原目录下的新每日文件路径。
 *
 * 参数：
 *   legacyPath 是旧版固定文件路径，例如 /mnt/sdcard/images/upload_history.json。
 *   prefix 是每日文件名前缀，例如 upload_history。
 *   dateStamp 是 YYYYMMDD 日期字符串。
 *   suffix 是扩展名，例如 .json。
 *
 * 返回值：
 *   返回每日文件完整路径。
 */
static QString dailyFilePathFromLegacyPath(const QString &legacyPath,
                                           const QString &prefix,
                                           const QString &dateStamp,
                                           const QString &suffix)
{
    /* baseInfo 只用来取得旧文件所在目录，不直接复用旧文件名。 */
    const QFileInfo baseInfo(legacyPath);

    /* fileName 保存按日期归档后的文件名。 */
    const QString fileName = prefix
        + QLatin1Char('_')
        + dateStamp
        + suffix;

    return QDir(baseInfo.absolutePath()).filePath(fileName);
}

/*
 * uploadHistoryDateStampFromText 的作用：
 *   从历史记录的 upload_time 文本推导该记录应该归档到哪一天。
 *
 * 主要流程：
 *   1. 解析当前程序写入的 `yyyy-MM-dd HH:mm:ss` 时间格式。
 *   2. 兼容只包含日期的 `yyyy-MM-dd` 文本。
 *   3. 解析失败时回退到今天，保证新记录不会因为异常时间写到空路径。
 *
 * 参数：
 *   uploadTime 是历史记录里的上传时间。
 *
 * 返回值：
 *   返回 YYYYMMDD 日期字符串。
 */
static QString uploadHistoryDateStampFromText(const QString &uploadTime)
{
    /* normalizedTime 保存去掉首尾空格后的时间文本，避免 JSON 中偶发空格影响解析。 */
    const QString normalizedTime = uploadTime.trimmed();

    /* dateTime 优先按完整日期时间解析，这是 Qt 写入历史记录的默认格式。 */
    const QDateTime dateTime = QDateTime::fromString(normalizedTime, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (dateTime.isValid()) {
        return dateStampString(dateTime.date());
    }

    /* date 兼容只有日期的旧记录或人工修正记录。 */
    const QDate date = QDate::fromString(normalizedTime.left(10), QStringLiteral("yyyy-MM-dd"));
    if (date.isValid()) {
        return dateStampString(date);
    }

    return currentDateStampString();
}

/*
 * uploadStatusTokenValue 的作用：
 *   从上传状态文本中提取 `key=value` 字段，供历史模型和检测控制器共用。
 *
 * 主要流程：
 *   1. 查找指定 key 对应的 `key=` 起点。
 *   2. 向后读取到空白、分号或中文/英文逗号为止。
 *   3. 返回去掉首尾空格后的字段值。
 *
 * 参数：
 *   text 是 defect-cos-upload、检测 RESULT 或历史 JSON 中保存的状态文本。
 *   key 是 upload_status、record_id 或 record_no 这类字段名。
 *
 * 返回值：
 *   找到字段时返回字段值；找不到时返回空字符串。
 */
static QString uploadStatusTokenValue(const QString &text, const QString &key)
{
    const QString marker = key + QLatin1Char('=');
    const int markerIndex = text.indexOf(marker);

    if (markerIndex < 0) {
        return QString();
    }

    const int valueStart = markerIndex + marker.length();
    int valueEnd = valueStart;

    while (valueEnd < text.length()
           && !text.at(valueEnd).isSpace()
           && text.at(valueEnd) != QLatin1Char(';')
           && text.at(valueEnd) != QLatin1Char(',')
           && text.at(valueEnd) != QChar(0xFF0C)) {
        valueEnd++;
    }

    return text.mid(valueStart, valueEnd - valueStart).trimmed();
}

/*
 * isUploadStatusSuccess 的作用：
 *   统一判断一次上传是否已经完成云端归档，避免各处只按中文前缀各自判断。
 *
 * 主要流程：
 *   1. `upload_status=OK` 是检测链路最终 RESULT 的明确成功信号，优先返回成功。
 *   2. `upload_status=FAIL/SKIP` 是明确失败或跳过信号，不能被 record_id 等历史字段误判为成功。
 *   3. 脚本原始输出和压缩历史状态中的“上传成功”也视为成功。
 *
 * 参数：
 *   uploadStatus 是脚本输出、历史 upload_status 字段或压缩后的历史状态。
 *
 * 返回值：
 *   已完成云端上传返回 true；失败、跳过、本地保存或未知状态返回 false。
 */
static bool isUploadStatusSuccess(const QString &uploadStatus)
{
    const QString normalizedStatus = uploadStatus.trimmed();
    const QString statusToken = uploadStatusTokenValue(normalizedStatus, QStringLiteral("upload_status")).toUpper();

    if (statusToken == QStringLiteral("OK")) {
        return true;
    }
    if (statusToken == QStringLiteral("FAIL") || statusToken == QStringLiteral("SKIP")) {
        return false;
    }

    return normalizedStatus.startsWith(QStringLiteral("上传成功"))
        || normalizedStatus.contains(QStringLiteral("上传成功"));
}

/*
 * isUploadStatusFailure 的作用：
 *   统一判断上传是否明确失败，供告警和历史重发逻辑避免误伤已成功归档的记录。
 *
 * 参数：
 *   uploadStatus 是脚本输出、检测 RESULT 或历史状态文本。
 *
 * 返回值：
 *   明确失败返回 true；成功、跳过或未知状态返回 false。
 */
static bool isUploadStatusFailure(const QString &uploadStatus)
{
    const QString normalizedStatus = uploadStatus.trimmed();
    const QString statusToken = uploadStatusTokenValue(normalizedStatus, QStringLiteral("upload_status")).toUpper();

    if (statusToken == QStringLiteral("FAIL")) {
        return true;
    }
    if (statusToken == QStringLiteral("OK") || statusToken == QStringLiteral("SKIP")) {
        return false;
    }

    return normalizedStatus.startsWith(QStringLiteral("上传失败"))
        || normalizedStatus.contains(QStringLiteral("上传失败"));
}

/*
 * modelResultCodeFromCloudResult 的作用：
 *   把云端 result 字符串映射为 MP157-F4 二进制协议里的最终结果枚举。
 *
 * 参数：
 *   cloudResult 是云端记录 result，允许 good、bad、review、uncertain。
 *
 * 返回值：
 *   1 表示 good，2 表示 bad，3 表示 review/uncertain，0 表示无法识别。
 */
static quint8 modelResultCodeFromCloudResult(const QString &cloudResult)
{
    const QString normalized = cloudResult.trimmed().toLower();

    if (normalized == QStringLiteral("good")) {
        return 1U;
    }
    if (normalized == QStringLiteral("bad")) {
        return 2U;
    }
    if (normalized == QStringLiteral("review") || normalized == QStringLiteral("uncertain")) {
        return 3U;
    }
    return 0U;
}

/*
 * modelResultCodeFromText 的作用：
 *   从模型 RESULT 行中解析 F4 需要缓存的模型结果枚举。
 *
 * 主要流程：
 *   1. 优先读取 fused_result，因为它已经综合 MobileNetV3-Small 和 UNet。
 *   2. 旧 RESULT 缺少 fused_result 时，回退读取 fused_status/status。
 *   3. 无法识别时返回 0，让调用方拒绝下发 MODEL_READY。
 */
static quint8 modelResultCodeFromText(const QString &modelResultText)
{
    const QString fusedResult = uploadStatusTokenValue(modelResultText, QStringLiteral("fused_result"));
    const quint8 cloudCode = modelResultCodeFromCloudResult(fusedResult);

    if (cloudCode != 0U) {
        return cloudCode;
    }

    const QString fusedStatus = uploadStatusTokenValue(modelResultText, QStringLiteral("fused_status")).toUpper();
    const QString status = fusedStatus.isEmpty()
        ? uploadStatusTokenValue(modelResultText, QStringLiteral("status")).toUpper()
        : fusedStatus;

    if (status == QStringLiteral("GOOD")) {
        return 1U;
    }
    if (status == QStringLiteral("BAD")) {
        return 2U;
    }
    if (status == QStringLiteral("REVIEW") || status == QStringLiteral("WAIT")) {
        return 3U;
    }
    return 0U;
}

/*
 * finalBinFromModelResult 的作用：
 *   把模型/云端最终结果映射成机械臂分拣盘编号。
 *
 * 返回值：
 *   1=良品盘，2=不良品盘，3=待复核盘，0=未知。
 */
static quint8 finalBinFromModelResult(quint8 modelResult)
{
    if (modelResult == 1U) {
        return 1U;
    }
    if (modelResult == 2U) {
        return 2U;
    }
    if (modelResult == 3U) {
        return 3U;
    }
    return 0U;
}

/*
 * confidencePercentFromModelText 的作用：
 *   从 RESULT 行提取分类置信度并转换成 0~100 的整数百分比。
 *
 * 说明：
 *   defect-classify 当前输出通常是 0~1 小数；兼容旧输出 0~100。
 */
static quint8 confidencePercentFromModelText(const QString &modelResultText)
{
    const QString confidenceText = uploadStatusTokenValue(modelResultText, QStringLiteral("confidence"));
    bool ok = false;
    double confidence = confidenceText.toDouble(&ok);

    if (!ok) {
        return 50U;
    }

    if (confidence <= 1.0) {
        confidence *= 100.0;
    }

    return static_cast<quint8>(clampedInt(qRound(confidence), 0, 100));
}

/*
 * partTypeCodeFromModelText 的作用：
 *   把分类标签归一到 F4/ESP32S3 可使用的轻量零件类型编号。
 *
 * 返回值：
 *   1=波形垫圈/历史 gasket，2=平垫圈 washer，3=弹性垫圈 splitwasher，0=未知。
 */
static quint8 partTypeCodeFromModelText(const QString &modelResultText)
{
    QString classLabel = uploadStatusTokenValue(modelResultText, QStringLiteral("class")).toLower();

    classLabel.remove(QStringLiteral("_good"));
    classLabel.remove(QStringLiteral("_bad"));
    classLabel.remove(QStringLiteral("-good"));
    classLabel.remove(QStringLiteral("-bad"));

    if (classLabel == QStringLiteral("gasket") || classLabel == QStringLiteral("wave_washer")) {
        return 1U;
    }
    if (classLabel == QStringLiteral("washer") || classLabel == QStringLiteral("flat_washer")) {
        return 2U;
    }
    if (classLabel == QStringLiteral("splitwasher") || classLabel == QStringLiteral("split_washer")) {
        return 3U;
    }
    return 0U;
}

/*
 * defectTypeCodeFromModelText 的作用：
 *   把模型类别中的缺陷关键词映射成 F4 侧保留缺陷类型编号。
 *
 * 返回值：
 *   0=未知或无缺陷，1=分类/UNet 判坏，2=划痕，3=凹坑，4=污渍。
 */
static quint8 defectTypeCodeFromModelText(const QString &modelResultText)
{
    const QString classLabel = uploadStatusTokenValue(modelResultText, QStringLiteral("class")).toLower();
    const quint8 modelResult = modelResultCodeFromText(modelResultText);

    if (classLabel.contains(QStringLiteral("scratch"))) {
        return 2U;
    }
    if (classLabel.contains(QStringLiteral("dent"))) {
        return 3U;
    }
    if (classLabel.contains(QStringLiteral("stain"))) {
        return 4U;
    }
    return modelResult == 2U ? 1U : 0U;
}

/*
 * compactJsonString 的作用：
 *   把一个 JSON 对象压缩成单行 UTF-8 文本，便于作为环境变量传给 defect-cos-upload。
 *
 * 参数：
 *   object 是要传递给上传脚本的结构化上下文。
 *
 * 返回值：
 *   返回无换行、无缩进的 JSON 字符串；空对象返回 "{}"。
 */
static QString compactJsonString(const QJsonObject &object)
{
    const QJsonDocument document(object);
    return QString::fromUtf8(document.toJson(QJsonDocument::Compact));
}

/*
 * SetGstPipelineStateJob 的作用：
 *   把 GStreamer 管线状态切换放到 Qt Quick 渲染同步阶段执行。
 *
 * 主要流程：
 *   1. 构造时引用 pipeline，防止渲染任务执行前对象被释放。
 *   2. run() 中调用 gst_element_set_state 切换到目标状态。
 *   3. 析构时释放引用，避免 GStreamer 对象泄漏。
 *
 * 关键说明：
 *   qmlglsink 官方示例要求在 QQuickWindow::BeforeSynchronizingStage 切到 PLAYING，
 *   这样 sink 能在 Qt Quick/OpenGL 上下文准备好之后再绑定视频纹理。
 */
class SetGstPipelineStateJob : public QRunnable
{
public:
    /* pipeline 是要切换状态的 GStreamer 管线；state 是目标状态，例如 GST_STATE_PLAYING。 */
    SetGstPipelineStateJob(GstElement *pipeline, GstState state)
        : m_pipeline(pipeline ? GST_ELEMENT(gst_object_ref(pipeline)) : nullptr),
          m_state(state)
    {
    }

    /* 析构函数释放构造时保存的 GStreamer 对象引用。 */
    ~SetGstPipelineStateJob() override
    {
        if (m_pipeline) {
            gst_object_unref(m_pipeline);
        }
    }

    /* run 在 Qt Quick 渲染线程的同步阶段执行，负责真正切换 pipeline 状态。 */
    void run() override
    {
        if (m_pipeline) {
            gst_element_set_state(m_pipeline, m_state);
        }
    }

private:
    /* m_pipeline 保存被引用的 GStreamer 管线对象，确保异步任务执行时对象仍有效。 */
    GstElement *m_pipeline;

    /* m_state 保存本次任务要切换到的 GStreamer 状态。 */
    GstState m_state;
};

/*
 * UploadHistoryEntry 的作用：
 *   保存一次“双模型串行检测 + 本地结果图落盘 + 云端上传尝试”的历史记录。
 *
 * 字段说明：
 *   uploadTime 是用户第一层历史卡片看到的具体检测时间。
 *   resultText 是 MobileNetV3-Small 的 GOOD/BAD 汇总，界面显示为“良品/待复核”。
 *   workflowText 是本次串行检测和上传的流程状态，保留在 JSON 中供日志排查和统计兼容使用。
 *   sourcePath/sourceSizeBytes 是分类模型使用的当前帧 JPG，云端登记为 source。
 *   annotatedPaths/annotatedLabels/annotatedSizeBytes 保存 UNet raw/overlay/mask 等结果图，云端统一登记为 annotated。
 *   classificationResult 保存 defect-classify 的 RESULT 行，便于历史页回看分类概率。
 *   segmentationResult 保存 defect-segment 的 RESULT_SEG 行，便于历史页回看缺陷像素和结果图路径。
 *   jpgPath/pngPath/jpgSizeBytes/pngSizeBytes 是旧历史 JSON 兼容字段，新记录会同步写入 source/首张 annotated。
 *   uploadStatus 保存上传脚本返回的一行结果，成功和失败都要保留，便于追查云端问题。
 *   recordId/recordNo 是云端检测记录身份，用于和后台详情页、日志、COS 对象对账。
 *   cloudResult/partCode/classLabel 保存云端重传所需的检测结果和零件身份，避免历史重发时重新猜测。
 *   weightContextJson/ldcContextJson/f4FlowContextJson 保存 F4 传感器和机械臂流程上下文，上传失败后仍能完整重放本次记录。
 *   decisionContextJson/visionContextJson 保存 MP157 判定和视觉模型上下文，历史重传时继续按原始检测证据上报。
 *   boardResultText 保存板端原始双模型结论，云端修正后仍用于追溯误判来源。
 *   cloudReviewResult/cloudReviewText/cloudReviewTime/cloudReviewOperator/cloudReviewSource 保存云端按钮回写的复核信息。
 */
struct UploadHistoryEntry
{
    QString uploadTime;
    QString resultText;
    QString workflowText;
    QString sourcePath;
    QStringList annotatedPaths;
    QStringList annotatedLabels;
    QVector<qint64> annotatedSizeBytes;
    QString classificationResult;
    QString segmentationResult;
    QString jpgPath;
    QString pngPath;
    QString uploadStatus;
    QString recordId;
    QString recordNo;
    QString cloudResult;
    QString partCode;
    QString classLabel;
    QString weightContextJson;
    QString ldcContextJson;
    QString f4FlowContextJson;
    QString decisionContextJson;
    QString visionContextJson;
    QString boardResultText;
    QString cloudReviewResult;
    QString cloudReviewText;
    QString cloudReviewTime;
    QString cloudReviewOperator;
    QString cloudReviewSource;
    qint64 sourceSizeBytes = 0;
    qint64 jpgSizeBytes = 0;
    qint64 pngSizeBytes = 0;
};

/*
 * UploadHistoryModel 的作用：
 *   把上传历史记录提供给 QML 的 ListView、Repeater 和详情页。
 *
 * 主要流程：
 *   1. 启动时从 `/mnt/sdcard/images/upload_history_YYYYMMDD.json` 多日文件读取历史记录。
 *   2. 每次保存/上传完成后追加一条新记录，并立即写回当天 JSON 文件。
 *   3. QML 通过角色名读取时间、图片路径、检测结果、云端记录号等字段。
 *   4. 用户删除某条历史记录时，模型同步删除 JSON 记录和该记录指向的 JPG/PNG 图片文件。
 *
 * 关键说明：
 *   这个模型只负责本地历史展示，不直接访问云端；云端上传仍由 `defect-cos-upload`
 *   负责，避免 UI 模型和网络脚本的职责混在一起。
 */
class UploadHistoryModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    /*
     * HistoryRole 枚举定义 QML 能读取的字段名。
     * Qt::UserRole 之后的数字只在模型内部使用，QML 通过 roleNames() 暴露的中文业务字段访问。
     */
    enum HistoryRole {
        UploadTimeRole = Qt::UserRole + 1,
        ResultTextRole,
        WorkflowTextRole,
        JpgPathRole,
        PngPathRole,
        SourcePathRole,
        UploadStatusRole,
        RecordIdRole,
        RecordNoRole,
        JpgSizeBytesRole,
        PngSizeBytesRole,
        SourceSizeBytesRole,
        TotalSizeBytesRole,
        ImageCountRole,
        BoardResultTextRole,
        CloudReviewResultRole,
        CloudReviewTextRole,
        CloudReviewTimeRole,
        CloudReviewOperatorRole,
        CloudReviewSourceRole
    };

    /*
     * 构造函数的作用：
     *   保存历史文件路径，并在对象创建时加载已有历史记录。
     *
     * 参数：
     *   historyFilePath 是旧版固定 JSON 历史文件路径，用于推导每日文件目录并兼容读取旧数据。
     *   parent 是 Qt 对象树父对象。
     */
    explicit UploadHistoryModel(const QString &historyFilePath, QObject *parent = nullptr)
        : QAbstractListModel(parent),
          m_historyFilePath(historyFilePath)
    {
        loadFromDisk();
    }

    /*
     * rowCount 的作用：
     *   返回当前历史记录数量，供 QML ListView 决定需要创建多少张横向卡片。
     *
     * 参数：
     *   parent 是 Qt 模型树父索引，列表模型不使用它。
     *
     * 返回值：
     *   顶层返回记录数量；非根索引返回 0。
     */
    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid()) {
            return 0;
        }

        return m_entries.size();
    }

    /*
     * data 的作用：
     *   按 QML 请求的角色返回单条历史记录中的具体字段。
     *
     * 参数：
     *   index 是记录行号。
     *   role 是 QML 请求的字段角色。
     *
     * 返回值：
     *   返回 QVariant 封装的字符串、数字或图片数量；索引非法时返回空 QVariant。
     */
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) {
            return QVariant();
        }

        const UploadHistoryEntry &entry = m_entries.at(index.row());

        switch (role) {
        case UploadTimeRole:
            return entry.uploadTime;
        case ResultTextRole:
            return entry.resultText;
        case WorkflowTextRole:
            return entry.workflowText;
        case JpgPathRole:
            return entry.jpgPath;
        case PngPathRole:
            return entry.pngPath;
        case SourcePathRole:
            return normalizedSourcePath(entry);
        case UploadStatusRole:
            return entry.uploadStatus;
        case RecordIdRole:
            return entry.recordId;
        case RecordNoRole:
            return entry.recordNo;
        case JpgSizeBytesRole:
            return entry.jpgSizeBytes;
        case PngSizeBytesRole:
            return entry.pngSizeBytes;
        case SourceSizeBytesRole:
            return normalizedSourceSizeBytes(entry);
        case TotalSizeBytesRole:
            return totalSizeBytesForEntry(entry);
        case ImageCountRole:
            return imageCountForEntry(entry);
        case BoardResultTextRole:
            return normalizedBoardResultText(entry);
        case CloudReviewResultRole:
            return entry.cloudReviewResult;
        case CloudReviewTextRole:
            return entry.cloudReviewText;
        case CloudReviewTimeRole:
            return entry.cloudReviewTime;
        case CloudReviewOperatorRole:
            return entry.cloudReviewOperator;
        case CloudReviewSourceRole:
            return entry.cloudReviewSource;
        default:
            return QVariant();
        }
    }

    /*
     * roleNames 的作用：
     *   把 C++ 角色枚举映射成 QML 中可读的字段名。
     *
     * 返回值：
     *   返回 role -> name 的映射，例如 QML 中可写 `model.uploadTime`。
     */
    QHash<int, QByteArray> roleNames() const override
    {
        QHash<int, QByteArray> roles;

        roles.insert(UploadTimeRole, "uploadTime");
        roles.insert(ResultTextRole, "resultText");
        roles.insert(WorkflowTextRole, "workflowText");
        roles.insert(JpgPathRole, "jpgPath");
        roles.insert(PngPathRole, "pngPath");
        roles.insert(SourcePathRole, "sourcePath");
        roles.insert(UploadStatusRole, "uploadStatus");
        roles.insert(RecordIdRole, "recordId");
        roles.insert(RecordNoRole, "recordNo");
        roles.insert(JpgSizeBytesRole, "jpgSizeBytes");
        roles.insert(PngSizeBytesRole, "pngSizeBytes");
        roles.insert(SourceSizeBytesRole, "sourceSizeBytes");
        roles.insert(TotalSizeBytesRole, "totalSizeBytes");
        roles.insert(ImageCountRole, "imageCount");
        roles.insert(BoardResultTextRole, "boardResultText");
        roles.insert(CloudReviewResultRole, "cloudReviewResult");
        roles.insert(CloudReviewTextRole, "cloudReviewText");
        roles.insert(CloudReviewTimeRole, "cloudReviewTime");
        roles.insert(CloudReviewOperatorRole, "cloudReviewOperator");
        roles.insert(CloudReviewSourceRole, "cloudReviewSource");

        return roles;
    }

    /*
     * count 的作用：
     *   给 QML 提供无需调用 rowCount() 的记录数量属性。
     *
     * 返回值：
     *   返回当前历史记录数量。
     */
    Q_INVOKABLE int count() const
    {
        return m_entries.size();
    }

    /*
     * entryAt 的作用：
     *   按索引返回一条历史记录的完整字段，方便详情页一次性绑定。
     *
     * 参数：
     *   row 是历史记录索引。
     *
     * 返回值：
     *   返回 QVariantMap；索引非法时返回默认空记录。
     */
    Q_INVOKABLE QVariantMap entryAt(int row) const
    {
        if (row < 0 || row >= m_entries.size()) {
            return entryToVariantMap(UploadHistoryEntry());
        }

        return entryToVariantMap(m_entries.at(row));
    }

    /*
     * latestEntry 的作用：
     *   返回最近一次上传记录，便于首页“最近记录”摘要显示。
     *
     * 返回值：
     *   有记录时返回最后一条；无记录时返回空记录。
     */
    Q_INVOKABLE QVariantMap latestEntry() const
    {
        if (m_entries.isEmpty()) {
            return entryToVariantMap(UploadHistoryEntry());
        }

        return entryToVariantMap(m_entries.constLast());
    }

    /*
     * appendRecord 的作用：
     *   把一次新的保存/上传结果追加到历史记录模型，并写回磁盘。
     *
     * 参数：
     *   entry 是已经填好本地路径、云端信息和状态的历史记录。
     *
     * 返回值：
     *   无返回值；磁盘写入失败会输出日志，但不阻断界面展示。
     */
    void appendRecord(const UploadHistoryEntry &entry)
    {
        /*
         * 如果程序启动时 SD 卡尚未挂载，loadFromDisk() 会得到空列表。
         * 第一次保存前若历史文件已经随着 SD 卡出现，这里重新加载一次，避免覆盖旧历史。
         */
        if (m_entries.isEmpty() && anyHistoryFileExists()) {
            loadFromDisk();
        }

        const int insertRow = m_entries.size();

        beginInsertRows(QModelIndex(), insertRow, insertRow);
        m_entries.append(entry);
        endInsertRows();

        emit countChanged();

        if (!saveToDisk()) {
            qWarning() << "upload history save failed" << m_historyFilePath;
        }
    }

    /*
     * appendRecordAndReturnRow 的作用：
     *   追加一条历史记录，并把追加后的行号返回给调用方。
     *
     * 主要流程：
     *   1. 复用 appendRecord() 的插入、countChanged 和磁盘保存逻辑，避免维护两套写历史路径。
     *   2. 追加完成后返回 count()-1，供自动检测完成后原地更新同一条记录的上传状态。
     *
     * 参数：
     *   entry 是已经填好的本地检测历史记录。
     *
     * 返回值：
     *   返回新记录所在行号；如果追加后模型为空，返回 -1。
     */
    int appendRecordAndReturnRow(const UploadHistoryEntry &entry)
    {
        appendRecord(entry);
        return m_entries.isEmpty() ? -1 : (m_entries.size() - 1);
    }

    /*
     * removeRecord 的作用：
     *   删除 QML 指定的一条上传历史记录，并同步删除该记录对应的 JPG/PNG 图片文件。
     *
     * 主要流程：
     *   1. 先校验 row，避免 QML 传入过期索引导致越界访问。
     *   2. 从模型内存中移除记录并写回所属日期的 upload_history_YYYYMMDD.json。
     *   3. 只有历史 JSON 写回成功后，才删除记录里保存的 JPG/PNG 实体文件，避免历史仍在但图片先丢失。
     *   4. 如果 JSON 写回失败，把记录插回原位置，让界面和磁盘状态尽量保持一致。
     *
     * 参数：
     *   row 是 QML 中要删除的历史记录索引。
     *
     * 返回值：
     *   返回中文结果文本；以“删除失败”开头表示记录没有被成功删除。
     */
    Q_INVOKABLE QString removeRecord(int row)
    {
        if (row < 0 || row >= m_entries.size()) {
            qWarning() << "upload history remove invalid row" << row << "count" << m_entries.size();
            return QStringLiteral("删除失败：记录不存在");
        }

        const UploadHistoryEntry removedEntry = m_entries.at(row);

        beginRemoveRows(QModelIndex(), row, row);
        m_entries.removeAt(row);
        endRemoveRows();
        emit countChanged();

        if (!saveToDisk()) {
            qWarning() << "upload history remove save failed, rollback row" << row << m_historyFilePath;

            beginInsertRows(QModelIndex(), row, row);
            m_entries.insert(row, removedEntry);
            endInsertRows();
            emit countChanged();

            return QStringLiteral("删除失败：历史文件写入失败");
        }

        return removeHistoryImageFiles(removedEntry);
    }

    /*
     * retryPayloadAt 的作用：
     *   给“重新发送”按钮读取一条历史记录的本地图片上传载荷。
     *
     * 主要流程：
     *   1. 校验 row，避免 QML 传入已经删除的历史索引。
     *   2. 归一化 source 原图路径，兼容旧 JSON 里的 jpg_path。
     *   3. 归一化 annotated 结果图路径，兼容旧 JSON 里的 png_path。
     *   4. 返回 classification_result 和 segmentation_result，上传时据此恢复综合 good/bad/review 判定。
     *
     * 参数：
     *   row 是历史记录索引。
     *   sourcePath 用于返回云端 file_kind=source 的原始图路径。
     *   annotatedPaths 用于返回云端 file_kind=annotated 的结果图路径列表。
     *   classificationResult 用于返回分类模型 RESULT 行，可为空。
     *   segmentationResult 用于返回 UNet 分割模型 RESULT_SEG 行，可为空。
     *   cloudResult 用于返回创建云端记录时使用的 good/bad/review，旧记录为空时由调用方重新综合。
     *   partCode/classLabel 用于返回零件身份和模型原始标签，避免重传时按当前配置重新猜测。
     *   weightContextJson/ldcContextJson/f4FlowContextJson/decisionContextJson/visionContextJson
     *   用于返回第一次自动检测时已经保存的完整上下文，保证断网重传仍是同一条检测证据。
     *   errorText 用于返回中文失败原因。
     *
     * 返回值：
     *   载荷完整返回 true；记录不存在或图片路径不足返回 false。
     */
    bool retryPayloadAt(int row,
                        QString *sourcePath,
                        QStringList *annotatedPaths,
                        QString *classificationResult,
                        QString *segmentationResult,
                        QString *cloudResult,
                        QString *partCode,
                        QString *classLabel,
                        QString *weightContextJson,
                        QString *ldcContextJson,
                        QString *f4FlowContextJson,
                        QString *decisionContextJson,
                        QString *visionContextJson,
                        QString *errorText) const
    {
        if (row < 0 || row >= m_entries.size()) {
            if (errorText) {
                *errorText = QStringLiteral("记录不存在");
            }
            return false;
        }

        const UploadHistoryEntry &entry = m_entries.at(row);
        const QString normalizedSource = normalizedSourcePath(entry);
        const QStringList normalizedAnnotated = normalizedAnnotatedPaths(entry);

        if (normalizedSource.isEmpty()) {
            if (errorText) {
                *errorText = QStringLiteral("缺少原始图片路径");
            }
            return false;
        }

        if (normalizedAnnotated.isEmpty()) {
            if (errorText) {
                *errorText = QStringLiteral("缺少检测结果图片路径");
            }
            return false;
        }

        if (sourcePath) {
            *sourcePath = normalizedSource;
        }
        if (annotatedPaths) {
            *annotatedPaths = normalizedAnnotated;
        }
        if (classificationResult) {
            *classificationResult = entry.classificationResult;
        }
        if (segmentationResult) {
            *segmentationResult = entry.segmentationResult;
        }
        if (cloudResult) {
            *cloudResult = entry.cloudResult;
        }
        if (partCode) {
            *partCode = entry.partCode;
        }
        if (classLabel) {
            *classLabel = entry.classLabel;
        }
        if (weightContextJson) {
            *weightContextJson = entry.weightContextJson;
        }
        if (ldcContextJson) {
            *ldcContextJson = entry.ldcContextJson;
        }
        if (f4FlowContextJson) {
            *f4FlowContextJson = entry.f4FlowContextJson;
        }
        if (decisionContextJson) {
            *decisionContextJson = entry.decisionContextJson;
        }
        if (visionContextJson) {
            *visionContextJson = entry.visionContextJson;
        }

        return true;
    }

    /*
     * updateRecordInspectionContexts 的作用：
     *   在自动流程收齐称重、电感和 F4/ESP32S3 流程上下文后，先把这些证据写回同一条历史记录。
     *
     * 主要流程：
     *   1. 校验 row，避免上传线程还没启动时历史记录已经被删除。
     *   2. 写入云端结果、零件身份、称重、电感、F4 流程、视觉和决策上下文。
     *   3. 立即保存到每日 `upload_history_YYYYMMDD.json`，保证后续网络失败也能从历史页完整重传。
     *   4. 保存失败时回滚旧记录，避免内存和 SD 卡 JSON 状态不一致。
     *
     * 参数：
     *   row 是当前自动检测对应的历史记录行号。
     *   后续字符串参数都是本次云端上传需要的上下文字段，允许为空，空值表示旧记录或该阶段未产生。
     *   errorText 用于返回中文失败原因。
     *
     * 返回值：
     *   写入并 fsync 成功返回 true；索引非法或写盘失败返回 false。
     */
    bool updateRecordInspectionContexts(int row,
                                        const QString &cloudResult,
                                        const QString &partCode,
                                        const QString &classLabel,
                                        const QString &weightContextJson,
                                        const QString &ldcContextJson,
                                        const QString &f4FlowContextJson,
                                        const QString &decisionContextJson,
                                        const QString &visionContextJson,
                                        QString *errorText)
    {
        if (row < 0 || row >= m_entries.size()) {
            if (errorText) {
                *errorText = QStringLiteral("记录不存在");
            }
            return false;
        }

        const UploadHistoryEntry oldEntry = m_entries.at(row);

        m_entries[row].cloudResult = cloudResult;
        m_entries[row].partCode = partCode;
        m_entries[row].classLabel = classLabel;
        m_entries[row].weightContextJson = weightContextJson;
        m_entries[row].ldcContextJson = ldcContextJson;
        m_entries[row].f4FlowContextJson = f4FlowContextJson;
        m_entries[row].decisionContextJson = decisionContextJson;
        m_entries[row].visionContextJson = visionContextJson;

        if (!saveToDisk()) {
            m_entries[row] = oldEntry;
            emit dataChanged(index(row, 0), index(row, 0));
            if (errorText) {
                *errorText = QStringLiteral("历史完整上下文写入失败");
            }
            return false;
        }

        emit dataChanged(index(row, 0), index(row, 0));
        return true;
    }

    /*
     * updateRecordUploadResult 的作用：
     *   在“重新发送”结束后原地更新同一条历史记录的云端状态。
     *
     * 主要流程：
     *   1. 校验 row，避免后台上传期间用户删除记录后越界写入。
     *   2. 备份旧记录，先更新内存中的 upload_status、record_id、record_no 和流程状态。
     *   3. 如果重发上传成功，把本地 upload_time 改成本次重发完成时间，并移动到历史末尾。
     *   4. 写回所属日期的 upload_history_YYYYMMDD.json；若写入失败，回滚旧记录并通知 QML 刷新。
     *   5. 写入成功后发送 dataChanged，让列表卡片和详情页都能读到最新云端状态。
     *
     * 参数：
     *   row 是需要更新的历史记录索引。
     *   uploadStatus 是压缩后的上传状态文本。
     *   recordId/recordNo 是上传脚本返回的新云端记录身份，失败时可为空；为空时保留旧值。
     *   errorText 用于返回中文失败原因。
     *
     * 返回值：
     *   历史文件成功保存返回 true；索引非法或写盘失败返回 false。
     */
    bool updateRecordUploadResult(int row,
                                  const QString &uploadStatus,
                                  const QString &recordId,
                                  const QString &recordNo,
                                  QString *errorText)
    {
        if (row < 0 || row >= m_entries.size()) {
            if (errorText) {
                *errorText = QStringLiteral("记录不存在");
            }
            return false;
        }

        const UploadHistoryEntry oldEntry = m_entries.at(row);
        const QVector<UploadHistoryEntry> oldEntries = m_entries;
        const bool uploadSucceeded = isUploadStatusSuccess(uploadStatus);
        const QString refreshedUploadTime = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));

        m_entries[row].uploadStatus = uploadStatus;
        if (!recordId.isEmpty()) {
            m_entries[row].recordId = recordId;
        }
        if (!recordNo.isEmpty()) {
            m_entries[row].recordNo = recordNo;
        }
        m_entries[row].workflowText = uploadSucceeded
            ? QStringLiteral("云端已归档")
            : QStringLiteral("本地已保存，等待重新发送");

        if (uploadSucceeded) {
            const int lastRow = m_entries.size() - 1;

            /*
             * 重发成功代表云端重新创建了一条“当前时间”的记录。
             * 本地历史仍复用同一组 source/annotated 图片，但时间和排序要跟本次云端记录对齐。
             * 先把记录移动到数组末尾，再对末尾记录刷新 upload_time，可以让“当前显示的最新记录”
             * 和写入 JSON 的最后一条记录严格对应，避免界面继续显示旧失败时间。
             */
            if (row != lastRow) {
                beginMoveRows(QModelIndex(), row, row, QModelIndex(), m_entries.size());
                m_entries.move(row, lastRow);
                endMoveRows();
            }
            m_entries[lastRow].uploadTime = refreshedUploadTime;
        }

        if (!saveToDisk()) {
            if (uploadSucceeded && row != m_entries.size() - 1) {
                beginResetModel();
                m_entries = oldEntries;
                endResetModel();
            } else {
                m_entries[row] = oldEntry;
                emit dataChanged(index(row, 0), index(row, 0));
            }
            if (errorText) {
                *errorText = QStringLiteral("历史文件写入失败");
            }
            return false;
        }

        const int changedRow = uploadSucceeded ? m_entries.size() - 1 : row;
        emit dataChanged(index(changedRow, 0), index(changedRow, 0),
                         QVector<int>() << UploadTimeRole
                                        << WorkflowTextRole
                                        << UploadStatusRole
                                        << RecordIdRole
                                        << RecordNoRole);
        return true;
    }

    /*
     * applyCloudReviewResult 的作用：
     *   接收云端“修正板端结果”按钮下发的复核结论，并更新同一条本地检测历史记录。
     *
     * 主要流程：
     *   1. 优先按 record_id 查找本地历史；没有 record_id 时按 record_no 查找。
     *   2. 校验 cloudResult 只能是 good/bad/review，避免云端字段漂移污染本地 JSON。
     *   3. 首次回写时把板端原始 resultText 保存到 boardResultText，后续重复回写不覆盖原始依据。
     *   4. 把 resultText 改成云端最终中文结果，让历史列表和统计优先展示修正后的业务结论。
     *   5. 写回所属日期的 upload_history_YYYYMMDD.json，成功后通知 QML 刷新对应历史卡片和详情页。
     *
     * 参数：
     *   recordId 是云端记录 ID，优先用于匹配。
     *   recordNo 是云端记录编号，recordId 为空时作为兜底匹配。
     *   cloudResult 是云端最终复核结果，只允许 good、bad 或 review。
     *   reviewText 是云端弹窗填写的修正原因，板端要求非空。
     *   reviewOperator 是云端操作人显示名或账号。
     *   reviewTime 是云端复核时间；为空时板端使用当前本地时间。
     *   reviewSource 是来源标记，默认 cloud。
     *   updatedRecord 用于返回更新后的记录字段，HTTP 响应会复用。
     *   errorText 用于返回中文失败原因。
     *
     * 返回值：
     *   更新并写盘成功返回 true；参数非法、找不到记录或写盘失败返回 false。
     */
    bool applyCloudReviewResult(const QString &recordId,
                                const QString &recordNo,
                                const QString &cloudResult,
                                const QString &reviewText,
                                const QString &reviewOperator,
                                const QString &reviewTime,
                                const QString &reviewSource,
                                QVariantMap *updatedRecord,
                                QString *errorText)
    {
        const QString normalizedResult = cloudResult.trimmed().toLower();
        const QString normalizedReason = reviewText.trimmed();
        const int row = findRecordRow(recordId.trimmed(), recordNo.trimmed());

        if (recordId.trimmed().isEmpty() && recordNo.trimmed().isEmpty()) {
            if (errorText) {
                *errorText = QStringLiteral("缺少 record_id 或 record_no");
            }
            return false;
        }

        if (row < 0) {
            if (errorText) {
                *errorText = QStringLiteral("记录不存在");
            }
            return false;
        }

        if (!isValidCloudReviewResult(normalizedResult)) {
            if (errorText) {
                *errorText = QStringLiteral("云端复核结果非法");
            }
            return false;
        }

        if (normalizedReason.isEmpty()) {
            if (errorText) {
                *errorText = QStringLiteral("云端修正原因不能为空");
            }
            return false;
        }

        const UploadHistoryEntry oldEntry = m_entries.at(row);
        UploadHistoryEntry updatedEntry = oldEntry;

        if (updatedEntry.boardResultText.isEmpty()) {
            updatedEntry.boardResultText = normalizedBoardResultText(updatedEntry);
        }

        updatedEntry.cloudReviewResult = normalizedResult;
        updatedEntry.cloudReviewText = normalizedReason.left(240);
        updatedEntry.cloudReviewOperator = reviewOperator.trimmed().left(80);
        updatedEntry.cloudReviewTime = reviewTime.trimmed().isEmpty()
            ? QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : reviewTime.trimmed().left(40);
        updatedEntry.cloudReviewSource = reviewSource.trimmed().isEmpty()
            ? QString::fromLatin1(DEFAULT_BOARD_REVIEW_SOURCE)
            : reviewSource.trimmed().left(40);
        updatedEntry.resultText = resultTextFromCloudReviewResult(normalizedResult);
        updatedEntry.workflowText = QStringLiteral("云端修正为%1；板端原始结论%2；原因：%3")
            .arg(updatedEntry.resultText,
                 updatedEntry.boardResultText,
                 updatedEntry.cloudReviewText);

        m_entries[row] = updatedEntry;

        if (!saveToDisk()) {
            m_entries[row] = oldEntry;
            if (errorText) {
                *errorText = QStringLiteral("历史文件写入失败");
            }
            return false;
        }

        emit dataChanged(index(row, 0), index(row, 0),
                         QVector<int>() << ResultTextRole
                                        << WorkflowTextRole
                                        << BoardResultTextRole
                                        << CloudReviewResultRole
                                        << CloudReviewTextRole
                                        << CloudReviewTimeRole
                                        << CloudReviewOperatorRole
                                        << CloudReviewSourceRole);

        if (updatedRecord) {
            *updatedRecord = entryToVariantMap(updatedEntry);
        }

        return true;
    }

signals:
    /* countChanged 在历史记录数量变化时发出，QML 可用它刷新空状态和统计卡。 */
    void countChanged();

private:
    /*
     * normalizedSourcePath 的作用：
     *   返回历史记录的原始检测图路径，并兼容旧 JSON 中只有 jpgPath 的记录。
     *
     * 参数：
     *   entry 是待读取的历史记录。
     *
     * 返回值：
     *   sourcePath 非空时返回 sourcePath；否则返回旧字段 jpgPath。
     */
    QString normalizedSourcePath(const UploadHistoryEntry &entry) const
    {
        return !entry.sourcePath.isEmpty() ? entry.sourcePath : entry.jpgPath;
    }

    /*
     * normalizedSourceSizeBytes 的作用：
     *   返回原始检测图文件大小，并兼容旧 JSON 中只有 jpgSizeBytes 的记录。
     *
     * 参数：
     *   entry 是待读取的历史记录。
     *
     * 返回值：
     *   sourceSizeBytes 大于 0 时返回它；否则返回 jpgSizeBytes。
     */
    qint64 normalizedSourceSizeBytes(const UploadHistoryEntry &entry) const
    {
        return entry.sourceSizeBytes > 0 ? entry.sourceSizeBytes : entry.jpgSizeBytes;
    }

    /*
     * normalizedBoardResultText 的作用：
     *   返回板端原始检测结论，并兼容旧记录没有 board_result_text 的情况。
     *
     * 参数：
     *   entry 是待读取的历史记录。
     *
     * 返回值：
     *   boardResultText 非空时返回它；否则返回当前 resultText，仍为空时返回“待复核”。
     */
    QString normalizedBoardResultText(const UploadHistoryEntry &entry) const
    {
        if (!entry.boardResultText.isEmpty()) {
            return entry.boardResultText;
        }

        if (!entry.resultText.isEmpty()) {
            return entry.resultText;
        }

        return QStringLiteral("待复核");
    }

    /*
     * isValidCloudReviewResult 的作用：
     *   校验云端回写结果枚举，防止云端字段名变化后写入本地历史。
     *
     * 参数：
     *   result 是云端传入的小写结果。
     *
     * 返回值：
     *   good、bad、review 返回 true；其它值返回 false。
     */
    bool isValidCloudReviewResult(const QString &result) const
    {
        return result == QStringLiteral("good")
            || result == QStringLiteral("bad")
            || result == QStringLiteral("review");
    }

    /*
     * resultTextFromCloudReviewResult 的作用：
     *   把云端 good/bad/review 转换为板端历史页展示的中文业务结论。
     *
     * 参数：
     *   result 是云端传入的小写结果。
     *
     * 返回值：
     *   good 返回“良品”，bad 返回“坏品”，review 或未知返回“待复核”。
     */
    QString resultTextFromCloudReviewResult(const QString &result) const
    {
        if (result == QStringLiteral("good")) {
            return QStringLiteral("良品");
        }

        if (result == QStringLiteral("bad")) {
            return QStringLiteral("坏品");
        }

        return QStringLiteral("待复核");
    }

    /*
     * findRecordRow 的作用：
     *   按云端身份查找本地历史记录，优先 record_id，兜底 record_no。
     *
     * 参数：
     *   recordId 是云端记录 ID。
     *   recordNo 是云端记录编号。
     *
     * 返回值：
     *   找到返回行号；找不到返回 -1。
     */
    int findRecordRow(const QString &recordId, const QString &recordNo) const
    {
        if (!recordId.isEmpty()) {
            for (int i = 0; i < m_entries.size(); i++) {
                if (m_entries.at(i).recordId == recordId) {
                    return i;
                }
            }
        }

        if (!recordNo.isEmpty()) {
            for (int i = 0; i < m_entries.size(); i++) {
                if (m_entries.at(i).recordNo == recordNo) {
                    return i;
                }
            }
        }

        return -1;
    }

    /*
     * normalizedAnnotatedPaths 的作用：
     *   返回历史记录中所有检测结果图路径，并兼容旧 JSON 中只有 pngPath 的记录。
     *
     * 参数：
     *   entry 是待读取的历史记录。
     *
     * 返回值：
     *   annotatedPaths 非空时返回它；否则用旧字段 pngPath 生成一项列表。
     */
    QStringList normalizedAnnotatedPaths(const UploadHistoryEntry &entry) const
    {
        if (!entry.annotatedPaths.isEmpty()) {
            return entry.annotatedPaths;
        }

        QStringList paths;
        if (!entry.pngPath.isEmpty()) {
            paths.append(entry.pngPath);
        }
        return paths;
    }

    /*
     * normalizedAnnotatedLabels 的作用：
     *   返回检测结果图标签列表，缺失时根据位置生成默认标签。
     *
     * 参数：
     *   entry 是待读取的历史记录。
     *   paths 是已经归一化后的结果图路径列表。
     *
     * 返回值：
     *   返回和 paths 等长的标签列表。
     */
    QStringList normalizedAnnotatedLabels(const UploadHistoryEntry &entry, const QStringList &paths) const
    {
        QStringList labels = entry.annotatedLabels;

        if (labels.isEmpty() && !entry.pngPath.isEmpty() && paths.size() == 1) {
            labels.append(QStringLiteral("PNG结果图"));
        }

        while (labels.size() < paths.size()) {
            labels.append(QStringLiteral("检测结果%1").arg(labels.size() + 1));
        }

        return labels;
    }

    /*
     * normalizedAnnotatedSizeBytes 的作用：
     *   返回检测结果图大小列表，缺失时兼容旧 pngSizeBytes 或现场读取 QFileInfo。
     *
     * 参数：
     *   entry 是待读取的历史记录。
     *   paths 是已经归一化后的结果图路径列表。
     *
     * 返回值：
     *   返回和 paths 等长的大小列表。
     */
    QVector<qint64> normalizedAnnotatedSizeBytes(const UploadHistoryEntry &entry, const QStringList &paths) const
    {
        QVector<qint64> sizes = entry.annotatedSizeBytes;

        if (sizes.isEmpty() && !entry.pngPath.isEmpty() && paths.size() == 1) {
            sizes.append(entry.pngSizeBytes);
        }

        while (sizes.size() < paths.size()) {
            const QFileInfo info(paths.at(sizes.size()));
            sizes.append(info.exists() ? info.size() : 0);
        }

        return sizes;
    }

    /*
     * imageCountForEntry 的作用：
     *   统计一条历史记录中实际有几张可展示图片。
     *
     * 参数：
     *   entry 是待统计的历史记录。
     *
     * 返回值：
     *   原始检测图非空加 1，annotated 结果图每张加 1。
     */
    int imageCountForEntry(const UploadHistoryEntry &entry) const
    {
        int count = 0;

        if (!normalizedSourcePath(entry).isEmpty()) {
            count++;
        }

        count += normalizedAnnotatedPaths(entry).size();
        return count;
    }

    /*
     * totalSizeBytesForEntry 的作用：
     *   汇总一条历史记录所有本地图片的文件大小。
     *
     * 参数：
     *   entry 是待统计的历史记录。
     *
     * 返回值：
     *   返回 source 和所有 annotated 图片大小之和。
     */
    qint64 totalSizeBytesForEntry(const UploadHistoryEntry &entry) const
    {
        const QStringList annotatedPaths = normalizedAnnotatedPaths(entry);
        const QVector<qint64> annotatedSizes = normalizedAnnotatedSizeBytes(entry, annotatedPaths);
        qint64 total = normalizedSourceSizeBytes(entry);

        for (qint64 size : annotatedSizes) {
            total += size;
        }

        return total;
    }

    /*
     * entryToJson 的作用：
     *   把 C++ 历史记录转换成 JSON 对象，便于持久化到 SD 卡。
     *
     * 参数：
     *   entry 是待转换的历史记录。
     *
     * 返回值：
     *   返回字段名稳定的 JSON 对象。
     */
    QJsonObject entryToJson(const UploadHistoryEntry &entry) const
    {
        QJsonObject object;
        QJsonArray annotatedImages;
        const QStringList annotatedPaths = normalizedAnnotatedPaths(entry);
        const QStringList annotatedLabels = normalizedAnnotatedLabels(entry, annotatedPaths);
        const QVector<qint64> annotatedSizes = normalizedAnnotatedSizeBytes(entry, annotatedPaths);

        object.insert(QStringLiteral("upload_time"), entry.uploadTime);
        object.insert(QStringLiteral("result_text"), entry.resultText);
        object.insert(QStringLiteral("workflow_text"), entry.workflowText);
        object.insert(QStringLiteral("source_path"), normalizedSourcePath(entry));
        object.insert(QStringLiteral("source_size_bytes"), QString::number(normalizedSourceSizeBytes(entry)));
        object.insert(QStringLiteral("classification_result"), entry.classificationResult);
        object.insert(QStringLiteral("segmentation_result"), entry.segmentationResult);
        object.insert(QStringLiteral("jpg_path"), entry.jpgPath);
        object.insert(QStringLiteral("png_path"), entry.pngPath);
        object.insert(QStringLiteral("upload_status"), entry.uploadStatus);
        object.insert(QStringLiteral("record_id"), entry.recordId);
        object.insert(QStringLiteral("record_no"), entry.recordNo);
        object.insert(QStringLiteral("cloud_result"), entry.cloudResult);
        object.insert(QStringLiteral("part_code"), entry.partCode);
        object.insert(QStringLiteral("class_label"), entry.classLabel);
        object.insert(QStringLiteral("weight_context_json"), entry.weightContextJson);
        object.insert(QStringLiteral("ldc_context_json"), entry.ldcContextJson);
        object.insert(QStringLiteral("f4_flow_context_json"), entry.f4FlowContextJson);
        object.insert(QStringLiteral("decision_context_json"), entry.decisionContextJson);
        object.insert(QStringLiteral("vision_context_json"), entry.visionContextJson);
        object.insert(QStringLiteral("board_result_text"), normalizedBoardResultText(entry));
        object.insert(QStringLiteral("cloud_review_result"), entry.cloudReviewResult);
        object.insert(QStringLiteral("cloud_review_text"), entry.cloudReviewText);
        object.insert(QStringLiteral("cloud_review_time"), entry.cloudReviewTime);
        object.insert(QStringLiteral("cloud_review_operator"), entry.cloudReviewOperator);
        object.insert(QStringLiteral("cloud_review_source"), entry.cloudReviewSource);
        object.insert(QStringLiteral("jpg_size_bytes"), QString::number(entry.jpgSizeBytes));
        object.insert(QStringLiteral("png_size_bytes"), QString::number(entry.pngSizeBytes));

        for (int i = 0; i < annotatedPaths.size(); i++) {
            QJsonObject imageObject;

            imageObject.insert(QStringLiteral("label"), annotatedLabels.value(i, QStringLiteral("检测结果%1").arg(i + 1)));
            imageObject.insert(QStringLiteral("path"), annotatedPaths.at(i));
            imageObject.insert(QStringLiteral("size_bytes"), QString::number(annotatedSizes.value(i, 0)));
            annotatedImages.append(imageObject);
        }

        object.insert(QStringLiteral("annotated_images"), annotatedImages);

        return object;
    }

    /*
     * entryFromJson 的作用：
     *   从 JSON 对象恢复一条历史记录。
     *
     * 参数：
     *   object 是 JSON 中的一条记录。
     *
     * 返回值：
     *   返回 UploadHistoryEntry；缺失字段会保留为空字符串或 0。
     */
    UploadHistoryEntry entryFromJson(const QJsonObject &object) const
    {
        UploadHistoryEntry entry;
        const QJsonArray annotatedImages = object.value(QStringLiteral("annotated_images")).toArray();

        entry.uploadTime = object.value(QStringLiteral("upload_time")).toString();
        entry.resultText = object.value(QStringLiteral("result_text")).toString();
        entry.workflowText = object.value(QStringLiteral("workflow_text")).toString();
        entry.sourcePath = object.value(QStringLiteral("source_path")).toString();
        entry.sourceSizeBytes = jsonIntegerString(object, QStringLiteral("source_size_bytes"));
        entry.classificationResult = object.value(QStringLiteral("classification_result")).toString();
        entry.segmentationResult = object.value(QStringLiteral("segmentation_result")).toString();
        entry.jpgPath = object.value(QStringLiteral("jpg_path")).toString();
        entry.pngPath = object.value(QStringLiteral("png_path")).toString();
        entry.uploadStatus = object.value(QStringLiteral("upload_status")).toString();
        entry.recordId = object.value(QStringLiteral("record_id")).toString();
        entry.recordNo = object.value(QStringLiteral("record_no")).toString();
        entry.cloudResult = object.value(QStringLiteral("cloud_result")).toString();
        entry.partCode = object.value(QStringLiteral("part_code")).toString();
        entry.classLabel = object.value(QStringLiteral("class_label")).toString();
        entry.weightContextJson = object.value(QStringLiteral("weight_context_json")).toString();
        entry.ldcContextJson = object.value(QStringLiteral("ldc_context_json")).toString();
        entry.f4FlowContextJson = object.value(QStringLiteral("f4_flow_context_json")).toString();
        entry.decisionContextJson = object.value(QStringLiteral("decision_context_json")).toString();
        entry.visionContextJson = object.value(QStringLiteral("vision_context_json")).toString();
        entry.boardResultText = object.value(QStringLiteral("board_result_text")).toString();
        entry.cloudReviewResult = object.value(QStringLiteral("cloud_review_result")).toString();
        entry.cloudReviewText = object.value(QStringLiteral("cloud_review_text")).toString();
        entry.cloudReviewTime = object.value(QStringLiteral("cloud_review_time")).toString();
        entry.cloudReviewOperator = object.value(QStringLiteral("cloud_review_operator")).toString();
        entry.cloudReviewSource = object.value(QStringLiteral("cloud_review_source")).toString();
        entry.jpgSizeBytes = jsonIntegerString(object, QStringLiteral("jpg_size_bytes"));
        entry.pngSizeBytes = jsonIntegerString(object, QStringLiteral("png_size_bytes"));

        for (const QJsonValue &value : annotatedImages) {
            if (!value.isObject()) {
                continue;
            }

            const QJsonObject imageObject = value.toObject();
            const QString path = imageObject.value(QStringLiteral("path")).toString();

            if (path.isEmpty()) {
                continue;
            }

            entry.annotatedPaths.append(path);
            entry.annotatedLabels.append(imageObject.value(QStringLiteral("label")).toString(QStringLiteral("检测结果")));
            entry.annotatedSizeBytes.append(jsonIntegerString(imageObject, QStringLiteral("size_bytes")));
        }

        if (entry.sourcePath.isEmpty()) {
            entry.sourcePath = entry.jpgPath;
        }
        if (entry.sourceSizeBytes <= 0) {
            entry.sourceSizeBytes = entry.jpgSizeBytes;
        }
        if (entry.boardResultText.isEmpty()) {
            entry.boardResultText = entry.resultText;
        }

        return entry;
    }

    /*
     * entryToVariantMap 的作用：
     *   把 C++ 记录转换为 QML 易消费的 QVariantMap。
     *
     * 参数：
     *   entry 是待转换记录。
     *
     * 返回值：
     *   返回包含所有详情字段和图片数组的 map。
     */
    QVariantMap entryToVariantMap(const UploadHistoryEntry &entry) const
    {
        QVariantMap map;
        QVariantList images;
        const QString sourcePath = normalizedSourcePath(entry);
        const qint64 sourceSizeBytes = normalizedSourceSizeBytes(entry);
        const QStringList annotatedPaths = normalizedAnnotatedPaths(entry);
        const QStringList annotatedLabels = normalizedAnnotatedLabels(entry, annotatedPaths);
        const QVector<qint64> annotatedSizes = normalizedAnnotatedSizeBytes(entry, annotatedPaths);

        if (!sourcePath.isEmpty()) {
            QVariantMap jpgImage;

            jpgImage.insert(QStringLiteral("label"), QStringLiteral("原始图片"));
            jpgImage.insert(QStringLiteral("path"), sourcePath);
            jpgImage.insert(QStringLiteral("sizeBytes"), sourceSizeBytes);
            images.append(jpgImage);
        }

        for (int i = 0; i < annotatedPaths.size(); i++) {
            QVariantMap annotatedImage;

            annotatedImage.insert(QStringLiteral("label"), annotatedLabels.value(i, QStringLiteral("检测结果%1").arg(i + 1)));
            annotatedImage.insert(QStringLiteral("path"), annotatedPaths.at(i));
            annotatedImage.insert(QStringLiteral("sizeBytes"), annotatedSizes.value(i, 0));
            images.append(annotatedImage);
        }

        map.insert(QStringLiteral("uploadTime"), entry.uploadTime);
        map.insert(QStringLiteral("resultText"), entry.resultText);
        map.insert(QStringLiteral("workflowText"), entry.workflowText);
        map.insert(QStringLiteral("sourcePath"), sourcePath);
        map.insert(QStringLiteral("jpgPath"), entry.jpgPath.isEmpty() ? sourcePath : entry.jpgPath);
        map.insert(QStringLiteral("pngPath"), entry.pngPath);
        map.insert(QStringLiteral("classificationResult"), entry.classificationResult);
        map.insert(QStringLiteral("segmentationResult"), entry.segmentationResult);
        map.insert(QStringLiteral("uploadStatus"), entry.uploadStatus);
        map.insert(QStringLiteral("recordId"), entry.recordId);
        map.insert(QStringLiteral("recordNo"), entry.recordNo);
        map.insert(QStringLiteral("cloudResult"), entry.cloudResult);
        map.insert(QStringLiteral("partCode"), entry.partCode);
        map.insert(QStringLiteral("classLabel"), entry.classLabel);
        map.insert(QStringLiteral("weightContextJson"), entry.weightContextJson);
        map.insert(QStringLiteral("ldcContextJson"), entry.ldcContextJson);
        map.insert(QStringLiteral("f4FlowContextJson"), entry.f4FlowContextJson);
        map.insert(QStringLiteral("decisionContextJson"), entry.decisionContextJson);
        map.insert(QStringLiteral("visionContextJson"), entry.visionContextJson);
        map.insert(QStringLiteral("boardResultText"), normalizedBoardResultText(entry));
        map.insert(QStringLiteral("cloudReviewResult"), entry.cloudReviewResult);
        map.insert(QStringLiteral("cloudReviewText"), entry.cloudReviewText);
        map.insert(QStringLiteral("cloudReviewTime"), entry.cloudReviewTime);
        map.insert(QStringLiteral("cloudReviewOperator"), entry.cloudReviewOperator);
        map.insert(QStringLiteral("cloudReviewSource"), entry.cloudReviewSource);
        map.insert(QStringLiteral("sourceSizeBytes"), sourceSizeBytes);
        map.insert(QStringLiteral("jpgSizeBytes"), entry.jpgSizeBytes > 0 ? entry.jpgSizeBytes : sourceSizeBytes);
        map.insert(QStringLiteral("pngSizeBytes"), entry.pngSizeBytes);
        map.insert(QStringLiteral("totalSizeBytes"), totalSizeBytesForEntry(entry));
        map.insert(QStringLiteral("imageCount"), images.size());
        map.insert(QStringLiteral("images"), images);

        return map;
    }

    /*
     * jsonIntegerString 的作用：
     *   从 JSON 字段中读取 qint64，兼容旧记录把数字保存为字符串的格式。
     *
     * 参数：
     *   object 是 JSON 对象。
     *   key 是字段名。
     *
     * 返回值：
     *   成功返回字段值；缺失或非法时返回 0。
     */
    qint64 jsonIntegerString(const QJsonObject &object, const QString &key) const
    {
        const QJsonValue value = object.value(key);

        if (value.isDouble()) {
            return static_cast<qint64>(value.toDouble());
        }

        if (value.isString()) {
            bool ok = false;
            const qint64 number = value.toString().toLongLong(&ok);

            return ok ? number : 0;
        }

        return 0;
    }

    /*
     * removeHistoryImageFiles 的作用：
     *   删除单条历史记录中保存的 source 和 annotated 图片实体文件。
     *
     * 主要流程：
     *   1. 先处理原始检测图，再逐个处理 annotated 检测结果图，路径为空时跳过。
     *   2. 每个路径必须位于历史 JSON 所在目录下，避免损坏 JSON 时误删其他目录文件。
     *   3. 已经不存在的图片视为可接受状态，因为记录已经没有可展示实体文件。
     *   4. 删除失败只通过返回文本和日志提示，不再恢复历史记录，避免 JSON 与界面反复抖动。
     *
     * 参数：
     *   entry 是刚从历史模型中删除的记录。
     *
     * 返回值：
     *   返回给 QML 的中文删除结果摘要。
     */
    QString removeHistoryImageFiles(const UploadHistoryEntry &entry) const
    {
        int deletedCount = 0;
        int missingCount = 0;
        int failedCount = 0;
        const QString sourcePath = normalizedSourcePath(entry);
        const QStringList annotatedPaths = normalizedAnnotatedPaths(entry);

        removeOneHistoryImageFile(sourcePath, &deletedCount, &missingCount, &failedCount);
        for (const QString &path : annotatedPaths) {
            removeOneHistoryImageFile(path, &deletedCount, &missingCount, &failedCount);
        }

        if (failedCount > 0) {
            return QStringLiteral("删除完成：记录已删除，%1 张图片删除失败").arg(failedCount);
        }

        if (deletedCount > 0) {
            return QStringLiteral("删除完成：记录和 %1 张图片已删除").arg(deletedCount);
        }

        if (missingCount > 0) {
            return QStringLiteral("删除完成：记录已删除，图片文件原本不存在");
        }

        return QStringLiteral("删除完成：记录已删除，无图片路径");
    }

    /*
     * removeOneHistoryImageFile 的作用：
     *   删除一张历史图片，并把删除、缺失、失败数量累加到调用者提供的计数器中。
     *
     * 主要流程：
     *   1. 使用历史文件目录作为允许删除的根目录，每日 JSON 文件和图片都在 `/mnt/sdcard/images` 下。
     *   2. 清理待删图片的绝对路径，确认它仍在允许目录下。
     *   3. 只删除普通文件，不删除目录或其他特殊节点。
     *   4. 调用 QFile::remove 删除图片，并把结果写入 Qt 日志，方便板端排查。
     *
     * 参数：
     *   filePath 是历史记录保存的图片路径。
     *   deletedCount 统计成功删除的图片数量。
     *   missingCount 统计删除前已经不存在的图片数量。
     *   failedCount 统计因路径越界、非文件或 remove 失败而没有删除的图片数量。
     *
     * 返回值：
     *   无返回值；结果通过计数器和日志传出。
     */
    void removeOneHistoryImageFile(const QString &filePath,
                                   int *deletedCount,
                                   int *missingCount,
                                   int *failedCount) const
    {
        if (filePath.isEmpty()) {
            return;
        }

        const QString historyRootPath = historyDirectoryPath();
        const QString historyDirPrefix = historyRootPath.endsWith(QLatin1Char('/'))
            ? historyRootPath
            : historyRootPath + QLatin1Char('/');

        const QFileInfo imageFileInfo(filePath);
        const QString imagePath = QDir::cleanPath(imageFileInfo.absoluteFilePath());

        if (!imagePath.startsWith(historyDirPrefix)) {
            (*failedCount)++;
            qWarning() << "upload history image remove skipped outside history dir"
                       << "image" << imagePath
                       << "historyDir" << historyRootPath;
            return;
        }

        if (!imageFileInfo.exists()) {
            (*missingCount)++;
            qInfo() << "upload history image already missing" << imagePath;
            return;
        }

        if (!imageFileInfo.isFile()) {
            (*failedCount)++;
            qWarning() << "upload history image remove skipped non-file" << imagePath;
            return;
        }

        if (QFile::remove(imagePath)) {
            (*deletedCount)++;
            qInfo() << "upload history image removed" << imagePath;
            return;
        }

        (*failedCount)++;
        qWarning() << "upload history image remove failed" << imagePath;
    }

    /*
     * historyDirectoryPath 的作用：
     *   返回历史 JSON 和检测图片共同所在的目录。
     *
     * 主要流程：
     *   1. 从旧版固定历史路径取目录，保持和既有 `/mnt/sdcard/images` 契约一致。
     *   2. 清理路径中的 `.`、`..` 片段，便于后续越界判断和日志输出。
     *
     * 返回值：
     *   返回清理后的历史根目录路径。
     */
    QString historyDirectoryPath() const
    {
        /* historyFileInfo 用旧版路径推导目录，不代表新版本仍然写这个固定文件。 */
        const QFileInfo historyFileInfo(m_historyFilePath);

        return QDir::cleanPath(historyFileInfo.absolutePath());
    }

    /*
     * dailyHistoryFilePath 的作用：
     *   根据日期生成当天检测历史 JSON 路径。
     *
     * 参数：
     *   dateStamp 是 YYYYMMDD 日期字符串。
     *
     * 返回值：
     *   返回 `/mnt/sdcard/images/upload_history_YYYYMMDD.json` 形式的路径。
     */
    QString dailyHistoryFilePath(const QString &dateStamp) const
    {
        return dailyFilePathFromLegacyPath(m_historyFilePath,
                                           QString::fromLatin1(UPLOAD_HISTORY_DAILY_PREFIX),
                                           dateStamp,
                                           QString::fromLatin1(UPLOAD_HISTORY_DAILY_SUFFIX));
    }

    /*
     * dailyHistoryFilePathForEntry 的作用：
     *   根据历史记录自身的 uploadTime 计算它应该写回哪个日期文件。
     *
     * 参数：
     *   entry 是需要持久化的一条检测历史记录。
     *
     * 返回值：
     *   返回该记录所属自然日的每日 JSON 路径。
     */
    QString dailyHistoryFilePathForEntry(const UploadHistoryEntry &entry) const
    {
        return dailyHistoryFilePath(uploadHistoryDateStampFromText(entry.uploadTime));
    }

    /*
     * currentDailyHistoryFilePath 的作用：
     *   返回今天的检测历史 JSON 路径，用于 SD 卡后挂载时检查是否已有当天文件。
     *
     * 返回值：
     *   返回今天的 `/mnt/sdcard/images/upload_history_YYYYMMDD.json` 路径。
     */
    QString currentDailyHistoryFilePath() const
    {
        return dailyHistoryFilePath(currentDateStampString());
    }

    /*
     * allHistoryFilePaths 的作用：
     *   枚举旧版固定历史文件和新版每日历史文件。
     *
     * 主要流程：
     *   1. 先枚举 `upload_history_*.json`，按文件名排序，保证新版每日文件优先。
     *   2. 如果旧版 `/mnt/sdcard/images/upload_history.json` 存在，再加入兼容读取列表。
     *
     * 返回值：
     *   返回可能存在的历史 JSON 路径列表；文件不存在时不会加入。
     */
    QStringList allHistoryFilePaths() const
    {
        QStringList paths;

        const QDir historyDir(historyDirectoryPath());
        const QFileInfoList dailyFiles = historyDir.entryInfoList(
            QStringList() << QString::fromLatin1(UPLOAD_HISTORY_DAILY_PREFIX) + QStringLiteral("_????????")
                                + QString::fromLatin1(UPLOAD_HISTORY_DAILY_SUFFIX),
            QDir::Files,
            QDir::Name);

        for (const QFileInfo &fileInfo : dailyFiles) {
            const QString dailyPath = fileInfo.absoluteFilePath();
            if (!paths.contains(dailyPath)) {
                paths.append(dailyPath);
            }
        }

        if (QFileInfo::exists(m_historyFilePath) && !paths.contains(m_historyFilePath)) {
            paths.append(m_historyFilePath);
        }

        return paths;
    }

    /*
     * anyHistoryFileExists 的作用：
     *   判断 SD 卡上是否已经存在旧版或新版检测历史文件。
     *
     * 返回值：
     *   有任一历史文件返回 true；没有历史文件返回 false。
     */
    bool anyHistoryFileExists() const
    {
        return QFileInfo::exists(m_historyFilePath)
            || QFileInfo::exists(currentDailyHistoryFilePath())
            || !allHistoryFilePaths().isEmpty();
    }

    /*
     * historyEntryDedupKey 的作用：
     *   为跨多日历史加载生成去重键，避免旧版固定文件和新版每日文件中同一记录重复显示。
     *
     * 参数：
     *   entry 是待去重的历史记录。
     *
     * 返回值：
     *   返回优先级最高的稳定身份字段。
     */
    QString historyEntryDedupKey(const UploadHistoryEntry &entry) const
    {
        if (!entry.recordId.trimmed().isEmpty()) {
            return QStringLiteral("record-id:") + entry.recordId.trimmed();
        }
        if (!entry.recordNo.trimmed().isEmpty()) {
            return QStringLiteral("record-no:") + entry.recordNo.trimmed();
        }
        if (!normalizedSourcePath(entry).isEmpty()) {
            return QStringLiteral("source:") + normalizedSourcePath(entry);
        }
        return entry.uploadTime
            + QLatin1Char('|')
            + entry.resultText
            + QLatin1Char('|')
            + entry.uploadStatus;
    }

    /*
     * isMeaningfulHistoryEntry 的作用：
     *   判断一条 JSON 记录是否包含足够信息，避免损坏项污染历史页。
     *
     * 参数：
     *   entry 是从 JSON 解析出的历史记录。
     *
     * 返回值：
     *   有上传时间、source 图片或 annotated 图片时返回 true。
     */
    bool isMeaningfulHistoryEntry(const UploadHistoryEntry &entry) const
    {
        return !entry.uploadTime.isEmpty()
            || !normalizedSourcePath(entry).isEmpty()
            || !normalizedAnnotatedPaths(entry).isEmpty();
    }

    /*
     * loadHistoryEntriesFromFile 的作用：
     *   从单个历史 JSON 文件读取记录，并追加到调用者提供的数组。
     *
     * 主要流程：
     *   1. 文件不存在直接返回 true，便于首次启动。
     *   2. 文件存在时解析 JSON 数组。
     *   3. 对每条有效记录生成去重键，旧固定文件和每日文件重复时只保留第一条。
     *
     * 参数：
     *   filePath 是要读取的历史 JSON 路径。
     *   loadedEntries 用于追加解析成功的记录。
     *   seenKeys 保存已经加载过的记录身份。
     *
     * 返回值：
     *   读取成功或文件不存在返回 true；打开或解析失败返回 false。
     */
    bool loadHistoryEntriesFromFile(const QString &filePath,
                                    QVector<UploadHistoryEntry> *loadedEntries,
                                    QSet<QString> *seenKeys) const
    {
        QFile file(filePath);

        if (!file.exists()) {
            return true;
        }

        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "upload history open failed" << filePath << file.errorString();
            return false;
        }

        const QByteArray payload = file.readAll();
        const QJsonDocument document = QJsonDocument::fromJson(payload);

        if (!document.isArray()) {
            qWarning() << "upload history json is not array" << filePath;
            return false;
        }

        const QJsonArray array = document.array();
        for (const QJsonValue &value : array) {
            if (!value.isObject()) {
                continue;
            }

            const UploadHistoryEntry entry = entryFromJson(value.toObject());
            if (!isMeaningfulHistoryEntry(entry)) {
                continue;
            }

            const QString dedupKey = historyEntryDedupKey(entry);
            if (seenKeys && seenKeys->contains(dedupKey)) {
                qInfo() << "upload history duplicate skipped" << filePath << dedupKey;
                continue;
            }

            if (seenKeys) {
                seenKeys->insert(dedupKey);
            }
            if (loadedEntries) {
                loadedEntries->append(entry);
            }
        }

        return true;
    }

    /*
     * loadFromDisk 的作用：
     *   程序启动时从 SD 卡每日历史文件恢复记录列表。
     *
     * 主要流程：
     *   1. 同时兼容旧版 `upload_history.json` 和新版 `upload_history_YYYYMMDD.json`。
     *   2. 多文件加载后按 upload_time 稳定排序，界面仍保持从旧到新的横向记录顺序。
     *   3. 只追加包含上传时间或图片路径的记录，避免损坏项污染界面。
     *
     * 返回值：
     *   全部加载成功或文件不存在返回 true；任一存在文件读取/解析失败返回 false。
     */
    bool loadFromDisk()
    {
        QVector<UploadHistoryEntry> loadedEntries;
        QSet<QString> seenKeys;
        bool ok = true;

        for (const QString &filePath : allHistoryFilePaths()) {
            if (!loadHistoryEntriesFromFile(filePath, &loadedEntries, &seenKeys)) {
                ok = false;
            }
        }

        std::stable_sort(loadedEntries.begin(), loadedEntries.end(),
                         [](const UploadHistoryEntry &left, const UploadHistoryEntry &right) {
            return left.uploadTime < right.uploadTime;
        });

        beginResetModel();
        m_entries = loadedEntries;
        endResetModel();
        emit countChanged();
        return ok;
    }

    /*
     * writeHistoryArrayToFile 的作用：
     *   把一组历史记录可靠写入指定 JSON 文件。
     *
     * 主要流程：
     *   1. 确保历史文件所在目录存在。
     *   2. 先写入 `.tmp` 临时文件并 flush。
     *   3. 再调用 fsync 把数据推到内核文件系统。
     *   4. 用 rename 替换正式文件，降低断电时留下半截 JSON 的概率。
     *
     * 参数：
     *   filePath 是目标每日历史 JSON 路径。
     *   entries 是属于该日期的历史记录数组。
     *
     * 返回值：
     *   写入成功返回 true；任一步失败返回 false。
     */
    bool writeHistoryArrayToFile(const QString &filePath,
                                 const QVector<UploadHistoryEntry> &entries) const
    {
        const QFileInfo fileInfo(filePath);
        const QString dirPath = fileInfo.absolutePath();
        const QString tempPath = filePath + QStringLiteral(".tmp");
        QJsonArray array;

        if (!QDir().mkpath(dirPath)) {
            qWarning() << "upload history mkdir failed" << dirPath;
            return false;
        }

        for (const UploadHistoryEntry &entry : entries) {
            array.append(entryToJson(entry));
        }

        QFile tempFile(tempPath);
        if (!tempFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qWarning() << "upload history temp open failed" << tempPath << tempFile.errorString();
            return false;
        }

        tempFile.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
        if (!tempFile.flush()) {
            qWarning() << "upload history flush failed" << tempPath << tempFile.errorString();
            tempFile.close();
            return false;
        }

        /*
         * QFile::flush 只保证 Qt 用户态缓冲写出；这里再调用 fsync，把历史 JSON 推到内核文件系统，
         * 避免用户保存后马上安全卸载或断电时丢失最后一条上传记录。
         */
        if (::fsync(tempFile.handle()) != 0) {
            qWarning() << "upload history fsync failed" << tempPath << QString::fromLocal8Bit(strerror(errno));
            tempFile.close();
            return false;
        }
        tempFile.close();

        QFile::remove(filePath);
        if (!QFile::rename(tempPath, filePath)) {
            qWarning() << "upload history rename failed" << tempPath << filePath;
            QFile::remove(tempPath);
            return false;
        }

        return true;
    }

    /*
     * saveToDisk 的作用：
     *   把当前内存历史记录按自然日写回多个 SD 卡 JSON 文件。
     *
     * 主要流程：
     *   1. 按每条记录的 upload_time 分组成 `upload_history_YYYYMMDD.json`。
     *   2. 逐个写回每日文件，保证同一天所有检测记录保存在当天文件里。
     *   3. 保留旧版 `upload_history.json` 不再写入，仅作为兼容读取来源。
     *
     * 返回值：
     *   所有每日文件写入成功返回 true；任一文件失败返回 false。
     */
    bool saveToDisk() const
    {
        QMap<QString, QVector<UploadHistoryEntry>> entriesByFile;
        QStringList pathsToWrite = allHistoryFilePaths();
        const bool legacyHistoryFileExists = QFileInfo::exists(m_historyFilePath);
        bool ok = true;

        for (const UploadHistoryEntry &entry : m_entries) {
            const QString filePath = dailyHistoryFilePathForEntry(entry);
            entriesByFile[filePath].append(entry);
            if (!pathsToWrite.contains(filePath)) {
                pathsToWrite.append(filePath);
            }
        }

        pathsToWrite.removeDuplicates();
        pathsToWrite.sort();
        pathsToWrite.removeAll(m_historyFilePath);

        for (const QString &filePath : pathsToWrite) {
            if (!writeHistoryArrayToFile(filePath, entriesByFile.value(filePath))) {
                ok = false;
            }
        }

        /*
         * 旧版固定文件只作为兼容读取来源；必须等每日文件全部写成功后再清空旧文件，
         * 防止迁移过程中旧文件先被清空、每日文件又写失败导致历史记录丢失。
         */
        if (ok && legacyHistoryFileExists) {
            ok = writeHistoryArrayToFile(m_historyFilePath, QVector<UploadHistoryEntry>());
        }

        return ok;
    }

    QVector<UploadHistoryEntry> m_entries; /* m_entries 保存内存中的历史记录，顺序就是界面横向叠加顺序。 */
    QString m_historyFilePath;             /* m_historyFilePath 保存 SD 卡历史 JSON 文件路径。 */
};

/*
 * LogFileEntry 的作用：
 *   保存日志查看页面中一条日志文件的摘要信息。
 *
 * 字段说明：
 *   fileName 是日志文件名，只用于列表和弹窗标题显示。
 *   filePath 是日志文件完整路径，点击详情时按它读取全文。
 *   suffix 是文件扩展名，用于区分 .log 告警日志和 .txt 诊断快照。
 *   typeText 是面向操作员的日志类型文案。
 *   modifiedTime/modifiedText 保存最后修改时间，列表按该时间倒序排列。
 *   sizeBytes/sizeText 保存文件大小，列表用 sizeText 避免 QML 重复格式化。
 */
struct LogFileEntry
{
    QString fileName;
    QString filePath;
    QString suffix;
    QString typeText;
    QDateTime modifiedTime;
    QString modifiedText;
    qint64 sizeBytes = 0;
    QString sizeText;
};

/*
 * LogFileModel 的作用：
 *   把 `/mnt/sdcard/logs` 下的日志文件提供给 QML 日志查看页面。
 *
 * 主要流程：
 *   1. refresh() 扫描日志目录中的 .log 和 .txt 文件。
 *   2. 按最后修改时间倒序保存到 m_entries，方便最新日志显示在最上方。
 *   3. QML 列表通过 roleNames() 读取文件名、路径、大小、时间和类型。
 *   4. 点击某条日志时，QML 调用 readLogContent() 读取完整 UTF-8 内容放进滚动弹窗。
 *
 * 关键说明：
 *   这个模型只读日志，不删除、不改名、不截断，避免界面操作破坏现场诊断证据。
 */
class LogFileModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)

public:
    /*
     * LogRole 枚举定义 QML 可读取的日志字段。
     * Qt::UserRole 之后的值只在模型内部使用，QML 通过 roleNames() 暴露的字段名访问。
     */
    enum LogRole {
        FileNameRole = Qt::UserRole + 1,
        FilePathRole,
        SuffixRole,
        TypeTextRole,
        ModifiedTextRole,
        SizeBytesRole,
        SizeTextRole
    };

    /*
     * 构造函数的作用：
     *   保存日志目录路径，并立即执行一次扫描，让页面首次打开时已有数据。
     *
     * 参数：
     *   logDirPath 是板端日志目录，默认 `/mnt/sdcard/logs`。
     *   parent 是 Qt 对象树父对象。
     */
    explicit LogFileModel(const QString &logDirPath, QObject *parent = nullptr)
        : QAbstractListModel(parent),
          m_logDirPath(logDirPath),
          m_statusText(QStringLiteral("尚未扫描日志目录"))
    {
        refresh();
    }

    /*
     * rowCount 的作用：
     *   返回当前可展示的日志文件数量。
     *
     * 参数：
     *   parent 是 Qt 模型树父索引，列表模型不使用它。
     *
     * 返回值：
     *   顶层返回日志数量；非根索引返回 0。
     */
    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid()) {
            return 0;
        }

        return m_entries.size();
    }

    /*
     * data 的作用：
     *   按 QML 请求的角色返回日志文件摘要字段。
     *
     * 参数：
     *   index 是日志行号。
     *   role 是 QML 请求的字段角色。
     *
     * 返回值：
     *   索引有效时返回对应字段；索引无效时返回空 QVariant。
     */
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) {
            return QVariant();
        }

        const LogFileEntry &entry = m_entries.at(index.row());

        switch (role) {
        case FileNameRole:
            return entry.fileName;
        case FilePathRole:
            return entry.filePath;
        case SuffixRole:
            return entry.suffix;
        case TypeTextRole:
            return entry.typeText;
        case ModifiedTextRole:
            return entry.modifiedText;
        case SizeBytesRole:
            return entry.sizeBytes;
        case SizeTextRole:
            return entry.sizeText;
        default:
            return QVariant();
        }
    }

    /*
     * roleNames 的作用：
     *   把 C++ 角色枚举映射成 QML 中可直接使用的字段名。
     *
     * 返回值：
     *   返回 role -> name 的映射，例如 QML 中可写 `model.fileName`。
     */
    QHash<int, QByteArray> roleNames() const override
    {
        QHash<int, QByteArray> roles;

        roles.insert(FileNameRole, "fileName");
        roles.insert(FilePathRole, "filePath");
        roles.insert(SuffixRole, "suffix");
        roles.insert(TypeTextRole, "typeText");
        roles.insert(ModifiedTextRole, "modifiedText");
        roles.insert(SizeBytesRole, "sizeBytes");
        roles.insert(SizeTextRole, "sizeText");

        return roles;
    }

    /*
     * count 的作用：
     *   给 QML 提供日志数量属性，避免页面直接调用 rowCount()。
     *
     * 返回值：
     *   返回当前日志文件数量。
     */
    Q_INVOKABLE int count() const
    {
        return m_entries.size();
    }

    /*
     * statusText 的作用：
     *   返回最近一次扫描日志目录的状态。
     *
     * 返回值：
     *   返回“已加载 N 个日志文件”、目录不存在或暂无日志等中文提示。
     */
    QString statusText() const
    {
        return m_statusText;
    }

    /*
     * entryAt 的作用：
     *   按索引返回一条日志文件摘要，弹窗标题和空状态都复用它。
     *
     * 参数：
     *   row 是日志列表索引。
     *
     * 返回值：
     *   返回 QVariantMap；索引非法时返回空字段。
     */
    Q_INVOKABLE QVariantMap entryAt(int row) const
    {
        if (row < 0 || row >= m_entries.size()) {
            return entryToVariantMap(LogFileEntry());
        }

        return entryToVariantMap(m_entries.at(row));
    }

    /*
     * refresh 的作用：
     *   重新扫描板端日志目录，并通知 QML 列表刷新。
     *
     * 主要流程：
     *   1. 检查日志目录是否存在，目录缺失通常代表 SD 卡未挂载或部署路径异常。
     *   2. 只收集 .log 和 .txt 文件，避免把临时文件、图片或其它资源误当日志展示。
     *   3. 按最后修改时间倒序排序，让最新告警和快照显示在最前面。
     *   4. 用 beginResetModel/endResetModel 一次性刷新模型，避免多次插入导致 QML 跳动。
     *
     * 返回值：
     *   无返回值；扫描结果通过模型和 statusText 暴露给 QML。
     */
    Q_INVOKABLE void refresh()
    {
        const int oldCount = m_entries.size();
        QVector<LogFileEntry> refreshedEntries;
        QDir logDir(m_logDirPath);

        if (!logDir.exists()) {
            beginResetModel();
            m_entries.clear();
            endResetModel();

            if (oldCount != m_entries.size()) {
                emit countChanged();
            }
            setStatusText(m_logDirPath + QStringLiteral(" 不存在，请确认 SD 卡已挂载"));
            return;
        }

        const QFileInfoList fileInfos = logDir.entryInfoList(QStringList()
                                                             << QStringLiteral("*.log")
                                                             << QStringLiteral("*.txt"),
                                                             QDir::Files | QDir::NoSymLinks,
                                                             QDir::NoSort);

        for (const QFileInfo &fileInfo : fileInfos) {
            LogFileEntry entry;

            entry.fileName = fileInfo.fileName();
            entry.filePath = fileInfo.absoluteFilePath();
            entry.suffix = fileInfo.suffix().toLower();
            entry.typeText = typeTextForSuffix(entry.suffix);
            entry.modifiedTime = fileInfo.lastModified();
            entry.modifiedText = entry.modifiedTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            entry.sizeBytes = fileInfo.size();
            entry.sizeText = formatSizeText(entry.sizeBytes);

            refreshedEntries.append(entry);
        }

        std::sort(refreshedEntries.begin(), refreshedEntries.end(),
                  [](const LogFileEntry &left, const LogFileEntry &right) {
            if (left.modifiedTime == right.modifiedTime) {
                return left.fileName < right.fileName;
            }
            return left.modifiedTime > right.modifiedTime;
        });

        beginResetModel();
        m_entries = refreshedEntries;
        endResetModel();

        if (oldCount != m_entries.size()) {
            emit countChanged();
        }

        if (m_entries.isEmpty()) {
            setStatusText(m_logDirPath + QStringLiteral(" 暂无 .log 或 .txt 日志文件"));
        } else {
            setStatusText(QStringLiteral("已加载 %1 个日志文件").arg(m_entries.size()));
        }
    }

    /*
     * readLogContent 的作用：
     *   读取指定日志文件的完整内容，供 QML 详情弹窗显示。
     *
     * 主要流程：
     *   1. 校验 row，避免 QML 使用过期索引。
     *   2. 按 entry.filePath 打开文件，只读不写。
     *   3. 使用 readAll() 读取完整内容，满足“点击日志看全部内容”的页面需求。
     *   4. 优先按 UTF-8 解码；日志为空时返回明确占位，避免弹窗空白像读取失败。
     *
     * 参数：
     *   row 是日志列表索引。
     *
     * 返回值：
     *   成功返回日志全文；失败返回“日志读取失败：...”中文原因。
     */
    Q_INVOKABLE QString readLogContent(int row) const
    {
        if (row < 0 || row >= m_entries.size()) {
            return QStringLiteral("日志读取失败：记录不存在");
        }

        const LogFileEntry &entry = m_entries.at(row);
        QFile file(entry.filePath);

        if (!file.exists()) {
            return QStringLiteral("日志读取失败：文件不存在\n路径：") + entry.filePath;
        }

        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QStringLiteral("日志读取失败：无法打开 ")
                + entry.filePath
                + QStringLiteral("：")
                + file.errorString();
        }

        const QByteArray contentBytes = file.readAll();
        if (file.error() != QFile::NoError) {
            return QStringLiteral("日志读取失败：读取 ")
                + entry.filePath
                + QStringLiteral(" 时出错：")
                + file.errorString();
        }

        if (contentBytes.isEmpty()) {
            return QStringLiteral("（空日志文件）");
        }

        return QString::fromUtf8(contentBytes);
    }

signals:
    /* countChanged 在刷新后日志数量变化时通知 QML 更新计数。 */
    void countChanged();

    /* statusTextChanged 在刷新状态变化时通知 QML 更新顶部提示。 */
    void statusTextChanged();

private:
    /*
     * setStatusText 的作用：
     *   统一更新扫描状态，并避免相同文本重复发信号。
     *
     * 参数：
     *   text 是新的中文状态文本。
     *
     * 返回值：
     *   无返回值。
     */
    void setStatusText(const QString &text)
    {
        if (m_statusText == text) {
            return;
        }

        m_statusText = text;
        emit statusTextChanged();
    }

    /*
     * typeTextForSuffix 的作用：
     *   根据扩展名生成面向操作员的日志类型。
     *
     * 参数：
     *   suffix 是小写扩展名，不包含点号。
     *
     * 返回值：
     *   .log 返回“告警日志”，.txt 返回“诊断快照”，其它返回“日志文件”。
     */
    QString typeTextForSuffix(const QString &suffix) const
    {
        if (suffix == QStringLiteral("log")) {
            return QStringLiteral("告警日志");
        }
        if (suffix == QStringLiteral("txt")) {
            return QStringLiteral("诊断快照");
        }
        return QStringLiteral("日志文件");
    }

    /*
     * formatSizeText 的作用：
     *   把字节数格式化成适合 1024x600 列表卡片展示的短文本。
     *
     * 参数：
     *   sizeBytes 是文件大小，单位为字节。
     *
     * 返回值：
     *   小文件返回 B，中等文件返回 KB，大文件返回 MB。
     */
    QString formatSizeText(qint64 sizeBytes) const
    {
        if (sizeBytes < 1024) {
            return QStringLiteral("%1 B").arg(sizeBytes);
        }
        if (sizeBytes < 1024 * 1024) {
            return QStringLiteral("%1 KB").arg(QString::number(sizeBytes / 1024.0, 'f', 1));
        }
        return QStringLiteral("%1 MB").arg(QString::number(sizeBytes / 1024.0 / 1024.0, 'f', 2));
    }

    /*
     * entryToVariantMap 的作用：
     *   把 C++ 日志条目转换成 QML 容易读取的 QVariantMap。
     *
     * 参数：
     *   entry 是日志条目；默认构造的空条目会生成空字段。
     *
     * 返回值：
     *   返回包含 fileName、filePath、typeText、modifiedText、sizeText 等字段的 map。
     */
    QVariantMap entryToVariantMap(const LogFileEntry &entry) const
    {
        QVariantMap map;

        map.insert(QStringLiteral("fileName"), entry.fileName);
        map.insert(QStringLiteral("filePath"), entry.filePath);
        map.insert(QStringLiteral("suffix"), entry.suffix);
        map.insert(QStringLiteral("typeText"), entry.typeText);
        map.insert(QStringLiteral("modifiedText"), entry.modifiedText);
        map.insert(QStringLiteral("sizeBytes"), entry.sizeBytes);
        map.insert(QStringLiteral("sizeText"), entry.sizeText);

        return map;
    }

    QVector<LogFileEntry> m_entries; /* m_entries 保存扫描到的日志文件摘要，顺序就是 QML 列表顺序。 */
    QString m_logDirPath;            /* m_logDirPath 保存板端日志目录，默认 /mnt/sdcard/logs。 */
    QString m_statusText;            /* m_statusText 保存最近一次扫描状态，顶部状态栏直接显示它。 */
};

/*
 * CloudReviewServer 的作用：
 *   提供一个板端 HTTP 小服务，接收云端“修正板端结果”按钮下发的复核结论。
 *
 * 主要流程：
 *   1. Qt 启动时读取 BOARD_REVIEW_LISTEN、BOARD_REVIEW_PORT 和 BOARD_REVIEW_TOKEN。
 *   2. 监听 `/api/v1/review-result`，只接受 POST JSON 请求。
 *   3. 校验 `X-Board-Token` 或 `Authorization: Bearer <token>`，防止任意内网客户端改写历史。
 *   4. 解析 record_id、record_no、cloud_result、cloud_reason、operator、review_time。
 *   5. 调用 UploadHistoryModel::applyCloudReviewResult() 更新本地每日 upload_history_YYYYMMDD.json。
 *
 * 关键说明：
 *   这个类只做本机历史回写，不访问云端数据库。云端自己的复核记录和同步状态必须由云端后端保存。
 */
class CloudReviewServer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString listenStatusText READ listenStatusText NOTIFY listenStatusChanged)

public:
    /*
     * 构造函数的作用：
     *   保存历史模型指针，并连接 QTcpServer 的新连接信号。
     *
     * 参数：
     *   historyModel 是要更新的本地上传历史模型。
     *   parent 是 Qt 对象树父对象。
     */
    explicit CloudReviewServer(UploadHistoryModel *historyModel, QObject *parent = nullptr)
        : QObject(parent),
          m_historyModel(historyModel),
          m_listenStatusText(QStringLiteral("未启动"))
    {
        connect(&m_server, &QTcpServer::newConnection, this, &CloudReviewServer::handleNewConnection);
    }

    /*
     * listenStatusText 的作用：
     *   返回云端复核回写服务当前监听状态，QML 或日志可用于排查云端无法连接的问题。
     *
     * 返回值：
     *   返回“监听 ...”或“启动失败 ...”这类中文状态。
     */
    QString listenStatusText() const
    {
        return m_listenStatusText;
    }

    /*
     * startFromEnvironment 的作用：
     *   根据环境变量启动板端 HTTP 回写服务。
     *
     * 主要流程：
     *   1. BOARD_REVIEW_ENABLE=0 时跳过监听，便于现场临时关闭回写入口。
     *   2. BOARD_REVIEW_LISTEN 默认 0.0.0.0，BOARD_REVIEW_PORT 默认 18080。
     *   3. BOARD_REVIEW_TOKEN 为空时允许无 token，但日志明确警告；正式部署必须设置。
     *
     * 返回值：
     *   监听成功返回 true；禁用或启动失败返回 false。
     */
    bool startFromEnvironment()
    {
        const QByteArray enableValue = qgetenv("BOARD_REVIEW_ENABLE");

        if (!enableValue.isEmpty() && QString::fromLatin1(enableValue).trimmed() == QStringLiteral("0")) {
            setListenStatus(QStringLiteral("云端复核回写已禁用"));
            return false;
        }

        const QString listenAddress = environmentString(QStringLiteral("BOARD_REVIEW_LISTEN"),
                                                        QString::fromLatin1(DEFAULT_BOARD_REVIEW_LISTEN));
        const quint16 listenPort = static_cast<quint16>(
            environmentInt(QStringLiteral("BOARD_REVIEW_PORT"), DEFAULT_BOARD_REVIEW_PORT, 1, 65535));

        m_token = loadReviewTokenFromEnvironment();
        if (m_token.isEmpty()) {
            qWarning() << "BOARD_REVIEW_TOKEN is empty; cloud review writeback accepts unauthenticated requests";
        }

        const QHostAddress address(listenAddress);
        const QHostAddress effectiveAddress = address.isNull() && listenAddress != QStringLiteral("0.0.0.0")
            ? QHostAddress::Any
            : address;

        if (!m_server.listen(effectiveAddress, listenPort)) {
            setListenStatus(QStringLiteral("云端复核回写启动失败：") + m_server.errorString());
            qWarning() << "cloud review server listen failed"
                       << listenAddress
                       << listenPort
                       << m_server.errorString();
            return false;
        }

        setListenStatus(QStringLiteral("云端复核回写监听 %1:%2")
                        .arg(effectiveAddress.toString())
                        .arg(m_server.serverPort()));
        qInfo() << "cloud review server listening"
                << effectiveAddress.toString()
                << m_server.serverPort();
        return true;
    }

signals:
    /* listenStatusChanged 在监听状态变化时通知 QML。 */
    void listenStatusChanged();

private:
    /*
     * handleNewConnection 的作用：
     *   逐个接收 QTcpServer 队列里的客户端连接，并挂接读取完成处理。
     */
    void handleNewConnection()
    {
        while (m_server.hasPendingConnections()) {
            QTcpSocket *socket = m_server.nextPendingConnection();

            if (socket == nullptr) {
                continue;
            }

            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                handleReadyRead(socket);
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

    /*
     * handleReadyRead 的作用：
     *   读取完整 HTTP 请求，达到 Content-Length 后交给 processHttpRequest()。
     *
     * 参数：
     *   socket 是当前客户端连接。
     */
    void handleReadyRead(QTcpSocket *socket)
    {
        QByteArray buffer = socket->property("requestBuffer").toByteArray();

        buffer.append(socket->readAll());
        socket->setProperty("requestBuffer", buffer);

        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }

        const QByteArray headerBytes = buffer.left(headerEnd);
        const int contentLength = httpContentLength(headerBytes);
        const int bodyStart = headerEnd + 4;

        if (buffer.size() < bodyStart + contentLength) {
            return;
        }

        const QByteArray body = buffer.mid(bodyStart, contentLength);
        processHttpRequest(socket, headerBytes, body);
    }

    /*
     * processHttpRequest 的作用：
     *   解析 HTTP 请求行、请求头和 JSON body，并输出对应 JSON 响应。
     *
     * 参数：
     *   socket 是当前客户端连接。
     *   headerBytes 是 HTTP 头部原始字节。
     *   body 是请求体 JSON 字节。
     */
    void processHttpRequest(QTcpSocket *socket, const QByteArray &headerBytes, const QByteArray &body)
    {
        const QList<QByteArray> headerLines = headerBytes.split('\n');
        const QByteArray requestLine = headerLines.isEmpty() ? QByteArray() : headerLines.first().trimmed();
        const QList<QByteArray> requestParts = requestLine.split(' ');
        const QString method = requestParts.size() > 0 ? QString::fromLatin1(requestParts.at(0)).trimmed() : QString();
        const QString path = requestParts.size() > 1 ? QString::fromLatin1(requestParts.at(1)).trimmed() : QString();
        const QMap<QString, QString> headers = parseHeaders(headerLines);

        if (method == QStringLiteral("OPTIONS")) {
            sendJsonResponse(socket, 200, jsonOkObject(QStringLiteral("preflight")));
            return;
        }

        if (method != QStringLiteral("POST") || path != QStringLiteral("/api/v1/review-result")) {
            sendJsonResponse(socket, 404, jsonErrorObject(QStringLiteral("接口不存在")));
            return;
        }

        if (!requestAuthorized(headers)) {
            sendJsonResponse(socket, 401, jsonErrorObject(QStringLiteral("token 校验失败")));
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);

        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            sendJsonResponse(socket, 400, jsonErrorObject(QStringLiteral("请求 JSON 格式错误")));
            return;
        }

        const QJsonObject request = document.object();
        const QString recordId = request.value(QStringLiteral("record_id")).toVariant().toString();
        const QString recordNo = request.value(QStringLiteral("record_no")).toString();
        const QString cloudResult = request.value(QStringLiteral("cloud_result")).toString();
        const QString cloudReason = request.value(QStringLiteral("cloud_reason")).toString();
        const QString reviewOperator = request.value(QStringLiteral("operator")).toString();
        const QString reviewTime = request.value(QStringLiteral("review_time")).toString();
        const QString reviewSource = request.value(QStringLiteral("source")).toString(QString::fromLatin1(DEFAULT_BOARD_REVIEW_SOURCE));
        QVariantMap updatedRecord;
        QString errorText;

        if (m_historyModel == nullptr) {
            sendJsonResponse(socket, 500, jsonErrorObject(QStringLiteral("历史模型未初始化")));
            return;
        }

        if (!m_historyModel->applyCloudReviewResult(recordId,
                                                    recordNo,
                                                    cloudResult,
                                                    cloudReason,
                                                    reviewOperator,
                                                    reviewTime,
                                                    reviewSource,
                                                    &updatedRecord,
                                                    &errorText)) {
            const int statusCode = errorText == QStringLiteral("记录不存在") ? 404 : 400;

            sendJsonResponse(socket, statusCode, jsonErrorObject(errorText));
            return;
        }

        QJsonObject response = jsonOkObject(QStringLiteral("updated"));

        response.insert(QStringLiteral("updated"), true);
        response.insert(QStringLiteral("record_id"), updatedRecord.value(QStringLiteral("recordId")).toString());
        response.insert(QStringLiteral("record_no"), updatedRecord.value(QStringLiteral("recordNo")).toString());
        response.insert(QStringLiteral("board_result_text"), updatedRecord.value(QStringLiteral("boardResultText")).toString());
        response.insert(QStringLiteral("effective_result_text"), updatedRecord.value(QStringLiteral("resultText")).toString());
        response.insert(QStringLiteral("cloud_review_result"), updatedRecord.value(QStringLiteral("cloudReviewResult")).toString());
        sendJsonResponse(socket, 200, response);
    }

    /*
     * requestAuthorized 的作用：
     *   校验云端请求携带的 token。
     *
     * 参数：
     *   headers 是小写 header 名到原始值的映射。
     *
     * 返回值：
     *   未配置 token 时返回 true；配置 token 时必须匹配请求头。
     */
    bool requestAuthorized(const QMap<QString, QString> &headers) const
    {
        if (m_token.isEmpty()) {
            return true;
        }

        const QString headerToken = headers.value(QStringLiteral("x-board-token")).trimmed();
        const QString authorization = headers.value(QStringLiteral("authorization")).trimmed();
        const QString bearerPrefix = QStringLiteral("Bearer ");

        if (headerToken == m_token) {
            return true;
        }

        if (authorization.startsWith(bearerPrefix, Qt::CaseInsensitive)
                && authorization.mid(bearerPrefix.length()).trimmed() == m_token) {
            return true;
        }

        return false;
    }

    /*
     * sendJsonResponse 的作用：
     *   把 JSON 对象编码成 HTTP 响应并关闭连接。
     *
     * 参数：
     *   socket 是当前客户端连接。
     *   statusCode 是 HTTP 状态码。
     *   object 是响应 JSON 对象。
     */
    void sendJsonResponse(QTcpSocket *socket, int statusCode, const QJsonObject &object)
    {
        const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
        const QByteArray reason = httpReasonPhrase(statusCode);
        QByteArray response;

        response += "HTTP/1.1 " + QByteArray::number(statusCode) + " " + reason + "\r\n";
        response += "Content-Type: application/json; charset=utf-8\r\n";
        response += "Access-Control-Allow-Origin: *\r\n";
        response += "Access-Control-Allow-Headers: Content-Type, X-Board-Token, Authorization\r\n";
        response += "Access-Control-Allow-Methods: POST, OPTIONS\r\n";
        response += "Connection: close\r\n";
        response += "Content-Length: " + QByteArray::number(payload.size()) + "\r\n\r\n";
        response += payload;

        socket->write(response);
        socket->flush();
        socket->disconnectFromHost();
    }

    /*
     * parseHeaders 的作用：
     *   把 HTTP 头部行解析成小写键值映射。
     *
     * 参数：
     *   headerLines 是按换行拆分后的 HTTP 头部。
     *
     * 返回值：
     *   返回 header 名到值的映射；请求行会被跳过。
     */
    QMap<QString, QString> parseHeaders(const QList<QByteArray> &headerLines) const
    {
        QMap<QString, QString> headers;

        for (int i = 1; i < headerLines.size(); i++) {
            const QByteArray line = headerLines.at(i).trimmed();
            const int colon = line.indexOf(':');

            if (colon <= 0) {
                continue;
            }

            const QString key = QString::fromLatin1(line.left(colon)).trimmed().toLower();
            const QString value = QString::fromUtf8(line.mid(colon + 1)).trimmed();

            headers.insert(key, value);
        }

        return headers;
    }

    /*
     * httpContentLength 的作用：
     *   从 HTTP 头部读取 Content-Length，未提供时按 0 处理。
     *
     * 参数：
     *   headerBytes 是 HTTP 头部原始字节。
     *
     * 返回值：
     *   返回请求体字节数；非法时返回 0。
     */
    int httpContentLength(const QByteArray &headerBytes) const
    {
        const QList<QByteArray> lines = headerBytes.split('\n');

        for (const QByteArray &line : lines) {
            const QByteArray trimmed = line.trimmed();

            if (!trimmed.toLower().startsWith("content-length:")) {
                continue;
            }

            bool ok = false;
            const int value = trimmed.mid(strlen("content-length:")).trimmed().toInt(&ok);

            return ok && value > 0 ? value : 0;
        }

        return 0;
    }

    /*
     * jsonOkObject 的作用：
     *   生成统一成功响应基础对象。
     */
    QJsonObject jsonOkObject(const QString &message) const
    {
        QJsonObject object;

        object.insert(QStringLiteral("ok"), true);
        object.insert(QStringLiteral("message"), message);
        return object;
    }

    /*
     * jsonErrorObject 的作用：
     *   生成统一失败响应基础对象。
     */
    QJsonObject jsonErrorObject(const QString &errorText) const
    {
        QJsonObject object;

        object.insert(QStringLiteral("ok"), false);
        object.insert(QStringLiteral("error"), errorText);
        return object;
    }

    /*
     * httpReasonPhrase 的作用：
     *   把常用 HTTP 状态码转换成响应行说明。
     */
    QByteArray httpReasonPhrase(int statusCode) const
    {
        switch (statusCode) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 404:
            return "Not Found";
        default:
            return "Internal Server Error";
        }
    }

    /*
     * environmentString 的作用：
     *   读取字符串环境变量，空值时返回默认值。
     */
    QString environmentString(const QString &name, const QString &defaultValue) const
    {
        const QByteArray value = qgetenv(name.toLatin1().constData());

        return value.isEmpty() ? defaultValue : QString::fromUtf8(value).trimmed();
    }

    /*
     * environmentInt 的作用：
     *   读取整数环境变量，并限制到指定范围。
     */
    int environmentInt(const QString &name, int defaultValue, int minValue, int maxValue) const
    {
        bool ok = false;
        const int value = QString::fromUtf8(qgetenv(name.toLatin1().constData())).trimmed().toInt(&ok);

        if (!ok) {
            return defaultValue;
        }

        return qBound(minValue, value, maxValue);
    }

    /*
     * loadReviewTokenFromEnvironment 的作用：
     *   读取 BOARD_REVIEW_TOKEN；若环境变量未设置，则从 cos-upload.env 中读取同名配置。
     *
     * 返回值：
     *   返回 token 文本；不存在时返回空字符串。
     */
    QString loadReviewTokenFromEnvironment() const
    {
        const QString envToken = QString::fromUtf8(qgetenv("BOARD_REVIEW_TOKEN")).trimmed();

        if (!envToken.isEmpty()) {
            return envToken;
        }

        return readTokenFromEnvFile(QString::fromUtf8(qgetenv("CLOUD_UPLOAD_ENV_FILE")).trimmed().isEmpty()
                                    ? QStringLiteral("/root/qt_camera_display/cos-upload.env")
                                    : QString::fromUtf8(qgetenv("CLOUD_UPLOAD_ENV_FILE")).trimmed());
    }

    /*
     * readTokenFromEnvFile 的作用：
     *   从 shell 风格 env 文件中读取 BOARD_REVIEW_TOKEN='...' 这类配置。
     *
     * 参数：
     *   filePath 是 env 文件路径。
     *
     * 返回值：
     *   找到 token 返回其值；文件不存在或未配置时返回空字符串。
     */
    QString readTokenFromEnvFile(const QString &filePath) const
    {
        QFile file(filePath);

        if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString();
        }

        while (!file.atEnd()) {
            const QString line = QString::fromUtf8(file.readLine()).trimmed();

            if (!line.startsWith(QStringLiteral("BOARD_REVIEW_TOKEN="))) {
                continue;
            }

            QString value = line.mid(QStringLiteral("BOARD_REVIEW_TOKEN=").length()).trimmed();

            if ((value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))
                    || (value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))) {
                value = value.mid(1, value.length() - 2);
            }

            return value.trimmed();
        }

        return QString();
    }

    /*
     * setListenStatus 的作用：
     *   更新监听状态并通知 QML。
     */
    void setListenStatus(const QString &text)
    {
        if (m_listenStatusText == text) {
            return;
        }

        m_listenStatusText = text;
        emit listenStatusChanged();
    }

    QTcpServer m_server;                 /* m_server 是板端复核回写 HTTP 监听器。 */
    UploadHistoryModel *m_historyModel;  /* m_historyModel 指向本地历史模型，不拥有生命周期。 */
    QString m_token;                     /* m_token 保存 BOARD_REVIEW_TOKEN，用于校验云端请求。 */
    QString m_listenStatusText;          /* m_listenStatusText 保存监听状态，便于 QML 或日志排查。 */
};

/*
 * DetectSettingsController 的作用：
 *   管理参数设置页的真实检测配置，并把配置持久化到 SD 卡 JSON 文件。
 *
 * 主要流程：
 *   1. 程序启动时尝试读取 /mnt/sdcard/config/defect_ui_config.json。
 *   2. QML 调整阈值、ROI、UNet 像素阈值、叠加透明度和上传开关时直接写入本对象属性。
 *   3. 用户点击保存时用 QSaveFile 原子写 JSON，避免断电或拔卡留下半截配置。
 *   4. CameraStorageController 检测前只读取 settingsSnapshot()，保证后台线程使用稳定快照。
 */
class DetectSettingsController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap currentSettings READ currentSettings NOTIFY settingsChanged)
    Q_PROPERTY(QString configPath READ configPath CONSTANT)
    Q_PROPERTY(QString partType READ partType WRITE setPartType NOTIFY settingsChanged)
    Q_PROPERTY(double modelThreshold READ modelThreshold WRITE setModelThreshold NOTIFY settingsChanged)
    Q_PROPERTY(double reviewThreshold READ reviewThreshold WRITE setReviewThreshold NOTIFY settingsChanged)
    Q_PROPERTY(int roiSize READ roiSize WRITE setRoiSize NOTIFY settingsChanged)
    Q_PROPERTY(int segmentMinComponentPixels READ segmentMinComponentPixels WRITE setSegmentMinComponentPixels NOTIFY settingsChanged)
    Q_PROPERTY(int segmentReviewPixels READ segmentReviewPixels WRITE setSegmentReviewPixels NOTIFY settingsChanged)
    Q_PROPERTY(int segmentBadPixels READ segmentBadPixels WRITE setSegmentBadPixels NOTIFY settingsChanged)
    Q_PROPERTY(int segmentStrongComponentPixels READ segmentStrongComponentPixels WRITE setSegmentStrongComponentPixels NOTIFY settingsChanged)
    Q_PROPERTY(double overlayAlpha READ overlayAlpha WRITE setOverlayAlpha NOTIFY settingsChanged)
    Q_PROPERTY(bool autoUploadEnabled READ autoUploadEnabled WRITE setAutoUploadEnabled NOTIFY settingsChanged)
    Q_PROPERTY(int f4ArmResultTimeoutMs READ f4ArmResultTimeoutMs WRITE setF4ArmResultTimeoutMs NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList stepperMotorSettings READ stepperMotorSettings NOTIFY settingsChanged)
    Q_PROPERTY(QString lastStatusText READ lastStatusText NOTIFY lastStatusTextChanged)

public:
    /*
     * 构造函数的作用：
     *   初始化配置文件路径，并在对象创建时尝试读取已有 JSON。
     *
     * 参数：
     *   parent 是 Qt 对象树父对象。
     */
    explicit DetectSettingsController(QObject *parent = nullptr)
        : QObject(parent),
          m_configPath(QString::fromLatin1(DEFAULT_DETECT_SETTINGS_FILE)),
          m_lastStatusText(QStringLiteral("真实检测配置：使用默认值"))
    {
        loadSettingsFromDisk();
    }

    /*
     * currentSettings 的作用：
     *   把当前配置一次性返回给 QML，便于调试和摘要展示。
     *
     * 返回值：
     *   返回 QVariantMap 形式的完整配置。
     */
    QVariantMap currentSettings() const
    {
        return detectSettingsToVariantMap(m_settings);
    }

    /*
     * settingsSnapshot 的作用：
     *   给 C++ 后台检测线程读取当前配置快照。
     *
     * 返回值：
     *   返回 DetectSettingsSnapshot 值对象，后续线程使用不依赖 QObject 生命周期。
     */
    DetectSettingsSnapshot settingsSnapshot() const
    {
        return m_settings;
    }

    /*
     * configPath 的作用：
     *   返回参数 JSON 的绝对路径，QML 会显示给现场人员确认保存位置。
     *
     * 返回值：
     *   返回 /mnt/sdcard/config/defect_ui_config.json。
     */
    QString configPath() const
    {
        return m_configPath;
    }

    /*
     * partType 的作用：
     *   返回参数页当前选择的零件中文名。
     */
    QString partType() const
    {
        return m_settings.partType;
    }

    /*
     * modelThreshold 的作用：
     *   返回分类模型 bad_total 判坏阈值，范围 0.50~0.99。
     */
    double modelThreshold() const
    {
        return m_settings.modelThreshold;
    }

    /*
     * reviewThreshold 的作用：
     *   返回低可信度复核阈值，低于该值时综合结果进入 REVIEW。
     */
    double reviewThreshold() const
    {
        return m_settings.reviewThreshold;
    }

    /*
     * roiSize 的作用：
     *   返回分类和分割模型共同使用的中心 ROI 边长。
     */
    int roiSize() const
    {
        return m_settings.roiSize;
    }

    /*
     * segmentMinComponentPixels 的作用：
     *   返回 UNet 保留单个缺陷连通域所需的最小面积。
     */
    int segmentMinComponentPixels() const
    {
        return m_settings.segmentMinComponentPixels;
    }

    /*
     * segmentReviewPixels 的作用：
     *   返回过滤后缺陷面积进入 WEAK 待复核区的下限。
     */
    int segmentReviewPixels() const
    {
        return m_settings.segmentReviewPixels;
    }

    /*
     * segmentBadPixels 的作用：
     *   返回过滤后缺陷总面积进入 STRONG 的下限。
     */
    int segmentBadPixels() const
    {
        return m_settings.segmentBadPixels;
    }

    /*
     * segmentStrongComponentPixels 的作用：
     *   返回最大连续缺陷区域进入 STRONG 的面积下限。
     */
    int segmentStrongComponentPixels() const
    {
        return m_settings.segmentStrongComponentPixels;
    }

    /*
     * overlayAlpha 的作用：
     *   返回 UNet overlay 结果图缺陷颜色叠加强度。
     */
    double overlayAlpha() const
    {
        return m_settings.overlayAlpha;
    }

    /*
     * autoUploadEnabled 的作用：
     *   返回检测完成后是否自动调用 COS 上传。
     */
    bool autoUploadEnabled() const
    {
        return m_settings.autoUploadEnabled;
    }

    /*
     * f4ArmResultTimeoutMs 的作用：
     *   返回 MP157 等待 F4 主动上报 WEIGHT_RESULT、LDC_RESULT 和 CYCLE_DONE 的最大时间。
     *
     * 返回值：
     *   返回毫秒数，合法范围由 normalizedSettings() 统一限制为 10000~300000。
     */
    int f4ArmResultTimeoutMs() const
    {
        return m_settings.f4ArmResultTimeoutMs;
    }

    /*
     * stepperMotorSettings 的作用：
     *   返回三台步进电机当前参数，供 QML 弹窗按页展示。
     *
     * 返回值：
     *   返回 QVariantList；每个元素包含 name/address/minStep/normalSpeedRpm/scanSpeedRpm/direction 等字段。
     */
    QVariantList stepperMotorSettings() const
    {
        return stepperMotorSettingsToVariantList(m_settings.stepperMotors);
    }

    /*
     * lastStatusText 的作用：
     *   返回最近一次加载、保存或恢复默认的中文状态。
     */
    QString lastStatusText() const
    {
        return m_lastStatusText;
    }

    /*
     * setPartType 的作用：
     *   设置零件中文名，只允许三类真实垫圈名称。
     *
     * 参数：
     *   value 是 QML 传入的零件名称。
     */
    void setPartType(const QString &value)
    {
        QString normalized = value.trimmed();
        const QStringList allowed = supportedPartTypes();

        if (!allowed.contains(normalized)) {
            normalized = allowed.constFirst();
        }

        if (m_settings.partType == normalized) {
            return;
        }

        m_settings.partType = normalized;
        setLastStatusText(QStringLiteral("真实检测配置：零件已切换为 ") + normalized);
        emit settingsChanged();
    }

    /*
     * setModelThreshold 的作用：
     *   设置分类模型 bad_total 判坏阈值，并保证复核阈值不会高于模型阈值。
     *
     * 参数：
     *   value 是 0~1 小数阈值。
     */
    void setModelThreshold(double value)
    {
        DetectSettingsSnapshot next = m_settings;

        next.modelThreshold = clampedDouble(value, 0.50, 0.99);
        next.reviewThreshold = clampedDouble(next.reviewThreshold, 0.30, next.modelThreshold);
        applySettings(next, QStringLiteral("真实检测配置：模型阈值已调整"));
    }

    /*
     * setReviewThreshold 的作用：
     *   设置低可信度复核阈值，避免低置信度 GOOD/BAD 直接成为最终结果。
     *
     * 参数：
     *   value 是 0~1 小数阈值。
     */
    void setReviewThreshold(double value)
    {
        DetectSettingsSnapshot next = m_settings;

        next.reviewThreshold = clampedDouble(value, 0.30, next.modelThreshold);
        applySettings(next, QStringLiteral("真实检测配置：复核阈值已调整"));
    }

    /*
     * setRoiSize 的作用：
     *   设置模型中心 ROI 边长，并限制到板端当前模型可接受范围。
     *
     * 参数：
     *   value 是 ROI 像素边长。
     */
    void setRoiSize(int value)
    {
        DetectSettingsSnapshot next = m_settings;

        next.roiSize = clampedInt(value, 160, 640);
        applySettings(next, QStringLiteral("真实检测配置：ROI大小已调整"));
    }

    /*
     * setSegmentMinComponentPixels 的作用：
     *   设置 UNet 小连通域过滤阈值，并由 normalizedSettings() 联动修正其它阈值顺序。
     */
    void setSegmentMinComponentPixels(int value)
    {
        DetectSettingsSnapshot next = m_settings;

        next.segmentMinComponentPixels = value;
        applySettings(next, QStringLiteral("真实检测配置：UNet噪点过滤阈值已调整"));
    }

    /*
     * setSegmentReviewPixels 的作用：
     *   设置过滤后缺陷面积进入 WEAK 待复核区的下限。
     */
    void setSegmentReviewPixels(int value)
    {
        DetectSettingsSnapshot next = m_settings;

        next.segmentReviewPixels = value;
        applySettings(next, QStringLiteral("真实检测配置：UNet复核像素阈值已调整"));
    }

    /*
     * setSegmentBadPixels 的作用：
     *   设置过滤后缺陷总面积进入 STRONG 明确缺陷区的下限。
     */
    void setSegmentBadPixels(int value)
    {
        DetectSettingsSnapshot next = m_settings;

        next.segmentBadPixels = value;
        applySettings(next, QStringLiteral("真实检测配置：UNet坏品像素阈值已调整"));
    }

    /*
     * setSegmentStrongComponentPixels 的作用：
     *   设置最大连续缺陷区域进入 STRONG 明确缺陷区的下限。
     */
    void setSegmentStrongComponentPixels(int value)
    {
        DetectSettingsSnapshot next = m_settings;

        next.segmentStrongComponentPixels = value;
        applySettings(next, QStringLiteral("真实检测配置：UNet强连通域阈值已调整"));
    }

    /*
     * setOverlayAlpha 的作用：
     *   设置 UNet overlay 图片的叠加透明度。
     *
     * 参数：
     *   value 是 0~1 小数。
     */
    void setOverlayAlpha(double value)
    {
        DetectSettingsSnapshot next = m_settings;

        next.overlayAlpha = clampedDouble(value, 0.0, 1.0);
        applySettings(next, QStringLiteral("真实检测配置：overlay透明度已调整"));
    }

    /*
     * setAutoUploadEnabled 的作用：
     *   设置检测完成后是否自动上传 COS。
     *
     * 参数：
     *   enabled 为 true 时检测后自动上传；false 时只保存本地历史。
     */
    void setAutoUploadEnabled(bool enabled)
    {
        if (m_settings.autoUploadEnabled == enabled) {
            return;
        }

        m_settings.autoUploadEnabled = enabled;
        setLastStatusText(enabled
            ? QStringLiteral("真实检测配置：已启用自动上传")
            : QStringLiteral("真实检测配置：已关闭自动上传"));
        emit settingsChanged();
    }

    /*
     * setF4ArmResultTimeoutMs 的作用：
     *   设置 MP157 等待 F4 机械臂主动结果帧的最大时间。
     *
     * 主要流程：
     *   1. 接收 QML 参数页加减按钮传入的毫秒数。
     *   2. 通过 applySettings() 统一限制到 10~300 秒，避免过短误判，同时允许现场机械臂最慢等待到 5 分钟。
     *   3. 只影响 MP157 等待 WEIGHT_RESULT/LDC_RESULT/CYCLE_DONE，不会写进 F4 发给 ESP32S3 的动作 payload。
     *
     * 参数：
     *   value 是新的等待窗口，单位 ms。
     */
    void setF4ArmResultTimeoutMs(int value)
    {
        DetectSettingsSnapshot next = m_settings;

        next.f4ArmResultTimeoutMs = value;
        applySettings(next, QStringLiteral("真实检测配置：机械臂等待超时已调整"));
    }

    /*
     * setStepperMotorValue 的作用：
     *   从 QML 步进电机参数弹窗更新某一台电机的单个参数。
     *
     * 主要流程：
     *   1. 校验 index 是否指向三台已知电机，非法页号直接拒绝。
     *   2. 根据 key 更新 ID 地址、最小步长、常规速度或方向。
     *   3. 调用 applySettings() 统一清洗范围并发出 settingsChanged。
     *
     * 参数：
     *   index 是弹窗页序号，0=传送带，1=摄像头左右，2=摄像头上下。
     *   key 是字段名，支持 address/minStep/normalSpeedRpm/scanSpeedRpm/direction/zMotionTimeoutMs。
     *   value 是字段新值，函数内部会再次限幅。
     *
     * 返回值：
     *   true 表示字段已接受并进入内存配置；false 表示页号或字段名非法。
     */
    Q_INVOKABLE bool setStepperMotorValue(int index, const QString &key, int value)
    {
        if (index < 0 || index >= m_settings.stepperMotors.size()) {
            setLastStatusText(QStringLiteral("步进电机参数：页号无效"));
            return false;
        }

        DetectSettingsSnapshot next = m_settings;
        StepperMotorSettings &motor = next.stepperMotors[index];

        if (key == QStringLiteral("address")) {
            motor.address = value;
        } else if (key == QStringLiteral("minStep")) {
            motor.minStep = value;
        } else if (key == QStringLiteral("normalSpeedRpm")) {
            motor.normalSpeedRpm = value;
        } else if (key == QStringLiteral("scanSpeedRpm")) {
            motor.scanSpeedRpm = value;
        } else if (key == QStringLiteral("direction")) {
            motor.direction = value >= 0 ? 1 : -1;
        } else if (key == QStringLiteral("zMotionTimeoutMs")) {
            motor.zMotionTimeoutMs = value;
        } else {
            setLastStatusText(QStringLiteral("步进电机参数：字段无效 ") + key);
            return false;
        }

        applySettings(next, QStringLiteral("步进电机参数：")
            + motor.name
            + QStringLiteral(" 已更新"));
        return true;
    }

    /*
     * setStepperMotorStepValue 的作用：
     *   从 QML 数字键盘更新上下步进电机的固定下探或回升步数。
     *
     * 主要流程：
     *   1. 校验 index 指向三台已知电机中的一台，通常只有摄像头上下电机页和左右电机页会显示该入口。
     *   2. 使用 clampedUInt32FromText() 解析十进制文本，确保范围完整覆盖 Emm42 位置模式 4 字节脉冲数。
     *   3. 只接受 zDownFixedSteps、zUpFixedSteps 和 lateralFineTuneFixedSteps 三个字段，避免 QML 误把其它参数绕过 int 限幅。
     *   4. 通过 applySettings() 统一归一化并通知 QML 刷新。
     *
     * 参数：
     *   index 是弹窗页序号，0=传送带，1=摄像头左右，2=摄像头上下。
     *   key 是字段名，只支持 zDownFixedSteps/zUpFixedSteps/lateralFineTuneFixedSteps。
     *   valueText 是用户输入的十进制 step 文本，合法范围为 0~4294967295。
     *
     * 返回值：
     *   true 表示字段已接受；false 表示页号、字段名或数值文本非法。
     */
    Q_INVOKABLE bool setStepperMotorStepValue(int index, const QString &key, const QString &valueText)
    {
        bool ok = false;                                      /* ok 标记十进制文本是否能完整解析为 32 位步数。 */
        const quint32 steps = clampedUInt32FromText(valueText, &ok);

        if (index < 0 || index >= m_settings.stepperMotors.size()) {
            setLastStatusText(QStringLiteral("步进电机位置参数：页号无效"));
            return false;
        }

        if (!ok) {
            setLastStatusText(QStringLiteral("步进电机位置参数必须是 0~4294967295 的整数 step"));
            return false;
        }

        DetectSettingsSnapshot next = m_settings;
        StepperMotorSettings &motor = next.stepperMotors[index];

        if (key == QStringLiteral("zDownFixedSteps")) {
            motor.zDownFixedSteps = steps;
        } else if (key == QStringLiteral("zUpFixedSteps")) {
            motor.zUpFixedSteps = steps;
        } else if (key == QStringLiteral("lateralFineTuneFixedSteps")) {
            if (motor.role != QStringLiteral("camera_lateral")) {
                setLastStatusText(QStringLiteral("左右轴微调步数只能在摄像头左右电机页设置"));
                return false;
            }
            if (steps < MP157_CAMERA_LATERAL_FINE_TUNE_MIN_STEPS) {
                setLastStatusText(QStringLiteral("左右轴微调步数必须至少为 1 step"));
                return false;
            }
            motor.lateralFineTuneFixedSteps = steps;
        } else {
            setLastStatusText(QStringLiteral("步进电机位置参数：字段无效 ") + key);
            return false;
        }

        applySettings(next, QStringLiteral("步进电机位置参数：")
            + motor.name
            + QStringLiteral(" 已更新"));
        return true;
    }

    /*
     * loadSettingsFromDisk 的作用：
     *   从 /mnt/sdcard/config/defect_ui_config.json 读取检测配置。
     *
     * 返回值：
     *   成功读取或文件不存在使用默认值时返回 true；JSON 无法解析时返回 false。
     */
    Q_INVOKABLE bool loadSettingsFromDisk()
    {
        QFile file(m_configPath);

        if (!file.exists()) {
            setLastStatusText(QStringLiteral("真实检测配置：未找到JSON，使用默认值"));
            return true;
        }

        if (!file.open(QIODevice::ReadOnly)) {
            setLastStatusText(QStringLiteral("读取失败：") + file.errorString());
            return false;
        }

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        file.close();

        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            setLastStatusText(QStringLiteral("读取失败：JSON格式错误 ") + parseError.errorString());
            return false;
        }

        DetectSettingsSnapshot next = m_settings;
        const QJsonObject object = doc.object();
        const int schemaVersion = object.value(QStringLiteral("schema_version")).toInt(1);

        next.partType = object.value(QStringLiteral("part_type")).toString(
            object.value(QStringLiteral("partType")).toString(next.partType));
        next.modelThreshold = object.value(QStringLiteral("model_threshold")).toDouble(
            object.value(QStringLiteral("modelThreshold")).toDouble(next.modelThreshold));
        next.reviewThreshold = object.value(QStringLiteral("review_threshold")).toDouble(
            object.value(QStringLiteral("reviewThreshold")).toDouble(next.reviewThreshold));
        next.roiSize = object.value(QStringLiteral("roi_size")).toInt(
            object.value(QStringLiteral("roiSize")).toInt(next.roiSize));
        if (schemaVersion >= 2) {
            next.segmentMinComponentPixels = object.value(QStringLiteral("segment_min_component_pixels")).toInt(
                object.value(QStringLiteral("segmentMinComponentPixels")).toInt(next.segmentMinComponentPixels));
            next.segmentReviewPixels = object.value(QStringLiteral("segment_review_pixels")).toInt(
                object.value(QStringLiteral("segmentReviewPixels")).toInt(next.segmentReviewPixels));
            next.segmentBadPixels = object.value(QStringLiteral("segment_bad_pixels")).toInt(
                object.value(QStringLiteral("segmentBadPixels")).toInt(next.segmentBadPixels));
            next.segmentStrongComponentPixels = object.value(QStringLiteral("segment_strong_component_pixels")).toInt(
                object.value(QStringLiteral("segmentStrongComponentPixels")).toInt(next.segmentStrongComponentPixels));
        } else {
            /* schema 1 的 segment_min_pixels 只有单阈值语义，不能安全映射到三级证据，统一迁移到新默认值。 */
            const DetectSettingsSnapshot defaults;
            next.segmentMinComponentPixels = defaults.segmentMinComponentPixels;
            next.segmentReviewPixels = defaults.segmentReviewPixels;
            next.segmentBadPixels = defaults.segmentBadPixels;
            next.segmentStrongComponentPixels = defaults.segmentStrongComponentPixels;
        }
        next.overlayAlpha = object.value(QStringLiteral("overlay_alpha")).toDouble(
            object.value(QStringLiteral("overlayAlpha")).toDouble(next.overlayAlpha));
        next.autoUploadEnabled = object.value(QStringLiteral("auto_upload_enabled")).toBool(
            object.value(QStringLiteral("autoUploadEnabled")).toBool(next.autoUploadEnabled));
        next.f4ArmResultTimeoutMs = object.value(QStringLiteral("f4_arm_result_timeout_ms")).toInt(
            object.value(QStringLiteral("f4ArmResultTimeoutMs")).toInt(next.f4ArmResultTimeoutMs));
        /* stepperArray 保存配置文件中的三台电机参数数组；兼容旧 camelCase 键，便于调试期间手写 JSON。 */
        const QJsonArray stepperArray = object.value(QStringLiteral("stepper_motors")).toArray(
            object.value(QStringLiteral("stepperMotors")).toArray());
        for (int index = 0; index < stepperArray.size() && index < next.stepperMotors.size(); ++index) {
            /* motorObject 保存当前页电机的 JSON 对象，字段缺失时继续沿用默认值。 */
            const QJsonObject motorObject = stepperArray.at(index).toObject();
            /* motor 保存当前页待更新的电机配置副本，最后写回 next.stepperMotors。 */
            StepperMotorSettings motor = next.stepperMotors.at(index);

            motor.address = motorObject.value(QStringLiteral("address")).toInt(
                motorObject.value(QStringLiteral("id")).toInt(motor.address));
            motor.minStep = motorObject.value(QStringLiteral("min_step")).toInt(
                motorObject.value(QStringLiteral("minStep")).toInt(motor.minStep));
            motor.normalSpeedRpm = motorObject.value(QStringLiteral("normal_speed_rpm")).toInt(
                motorObject.value(QStringLiteral("normalSpeedRpm")).toInt(motor.normalSpeedRpm));
            motor.scanSpeedRpm = motorObject.value(QStringLiteral("scan_speed_rpm")).toInt(
                motorObject.value(QStringLiteral("scanSpeedRpm")).toInt(motor.normalSpeedRpm));
            motor.direction = motorObject.value(QStringLiteral("direction")).toInt(motor.direction);
            motor.lateralFineTuneFixedSteps = clampedUInt32FromDouble(
                motorObject.value(QStringLiteral("lateral_fine_tune_fixed_steps")).toDouble(
                    motorObject.value(QStringLiteral("lateralFineTuneFixedSteps")).toDouble(
                        static_cast<double>(motor.lateralFineTuneFixedSteps))));
            motor.zDownFixedSteps = clampedUInt32FromDouble(
                motorObject.value(QStringLiteral("z_down_fixed_steps")).toDouble(
                    motorObject.value(QStringLiteral("zDownFixedSteps")).toDouble(
                        static_cast<double>(motor.zDownFixedSteps))));
            motor.zUpFixedSteps = clampedUInt32FromDouble(
                motorObject.value(QStringLiteral("z_up_fixed_steps")).toDouble(
                    motorObject.value(QStringLiteral("zUpFixedSteps")).toDouble(
                        static_cast<double>(motor.zUpFixedSteps))));
            motor.zMotionTimeoutMs = motorObject.value(QStringLiteral("z_motion_timeout_ms")).toInt(
                motorObject.value(QStringLiteral("zMotionTimeoutMs")).toInt(motor.zMotionTimeoutMs));
            next.stepperMotors[index] = motor;
        }

        applySettings(normalizedSettings(next), QStringLiteral("真实检测配置：已读取 ") + m_configPath);
        return true;
    }

    /*
     * saveSettingsToDisk 的作用：
     *   把当前检测配置写入 /mnt/sdcard/config/defect_ui_config.json。
     *
     * 返回值：
     *   返回可直接显示在 QML 底部提示条的中文结果。
     */
    Q_INVOKABLE QString saveSettingsToDisk()
    {
        QJsonObject object;
        const QFileInfo fileInfo(m_configPath);
        const QString dirPath = fileInfo.absolutePath();
        QString mountError;

        if (!isConfigMountReady(&mountError)) {
            const QString result = QStringLiteral("保存失败：") + mountError;
            setLastStatusText(result);
            return result;
        }

        if (!QDir().mkpath(dirPath)) {
            const QString result = QStringLiteral("保存失败：无法创建 ") + dirPath;
            setLastStatusText(result);
            return result;
        }

        object.insert(QStringLiteral("schema_version"), 2);
        object.insert(QStringLiteral("part_type"), m_settings.partType);
        object.insert(QStringLiteral("model_threshold"), m_settings.modelThreshold);
        object.insert(QStringLiteral("review_threshold"), m_settings.reviewThreshold);
        object.insert(QStringLiteral("roi_size"), m_settings.roiSize);
        object.insert(QStringLiteral("segment_min_component_pixels"), m_settings.segmentMinComponentPixels);
        object.insert(QStringLiteral("segment_review_pixels"), m_settings.segmentReviewPixels);
        object.insert(QStringLiteral("segment_bad_pixels"), m_settings.segmentBadPixels);
        object.insert(QStringLiteral("segment_strong_component_pixels"), m_settings.segmentStrongComponentPixels);
        object.insert(QStringLiteral("overlay_alpha"), m_settings.overlayAlpha);
        object.insert(QStringLiteral("auto_upload_enabled"), m_settings.autoUploadEnabled);
        object.insert(QStringLiteral("f4_arm_result_timeout_ms"), m_settings.f4ArmResultTimeoutMs);
        /* stepperMotorsArray 保存三台步进电机参数，和界面三页顺序保持一致。 */
        QJsonArray stepperMotorsArray;
        for (const StepperMotorSettings &motor : m_settings.stepperMotors) {
            /* motorObject 保存单台电机的可追溯字段，后续 F4 参数协议可按 role 定位目标电机。 */
            QJsonObject motorObject;

            motorObject.insert(QStringLiteral("name"), motor.name);
            motorObject.insert(QStringLiteral("role"), motor.role);
            motorObject.insert(QStringLiteral("serial"), motor.serialName);
            motorObject.insert(QStringLiteral("address"), motor.address);
            motorObject.insert(QStringLiteral("min_step"), motor.minStep);
            motorObject.insert(QStringLiteral("normal_speed_rpm"), motor.normalSpeedRpm);
            motorObject.insert(QStringLiteral("scan_speed_rpm"), motor.scanSpeedRpm);
            motorObject.insert(QStringLiteral("direction"), motor.direction);
            motorObject.insert(QStringLiteral("lateral_fine_tune_fixed_steps"),
                               static_cast<double>(motor.lateralFineTuneFixedSteps));
            motorObject.insert(QStringLiteral("z_down_fixed_steps"), static_cast<double>(motor.zDownFixedSteps));
            motorObject.insert(QStringLiteral("z_up_fixed_steps"), static_cast<double>(motor.zUpFixedSteps));
            motorObject.insert(QStringLiteral("z_motion_timeout_ms"), motor.zMotionTimeoutMs);
            stepperMotorsArray.append(motorObject);
        }
        object.insert(QStringLiteral("stepper_motors"), stepperMotorsArray);
        object.insert(QStringLiteral("saved_at"), QDateTime::currentDateTime().toString(Qt::ISODate));

        QSaveFile file(m_configPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            const QString result = QStringLiteral("保存失败：") + file.errorString();
            setLastStatusText(result);
            return result;
        }

        file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
        if (!file.commit()) {
            const QString result = QStringLiteral("保存失败：") + file.errorString();
            setLastStatusText(result);
            return result;
        }

        const QString result = QStringLiteral("保存成功：") + m_configPath;
        setLastStatusText(result);
        return result;
    }

    /*
     * resetToDefaults 的作用：
     *   恢复比赛演示推荐检测配置，但不自动写盘，用户仍需点击保存。
     *
     * 返回值：
     *   返回中文结果，供 QML 提示。
     */
    Q_INVOKABLE QString resetToDefaults()
    {
        applySettings(DetectSettingsSnapshot(), QStringLiteral("真实检测配置：已恢复默认，保存后写入JSON"));
        return m_lastStatusText;
    }

signals:
    /* settingsChanged 在任一检测参数变化后通知 QML 刷新显示，并通知 C++ 后续检测读取新快照。 */
    void settingsChanged();

    /* lastStatusTextChanged 在加载、保存或恢复默认结果变化后通知 QML 刷新提示。 */
    void lastStatusTextChanged();

private:
    /*
     * supportedPartTypes 的作用：
     *   返回参数页允许保存的真实零件名称列表。
     */
    QStringList supportedPartTypes() const
    {
        return QStringList()
            << QStringLiteral("波形垫圈")
            << QStringLiteral("平垫圈")
            << QStringLiteral("弹性垫圈");
    }

    /*
     * isConfigMountReady 的作用：
     *   保存配置前确认 /mnt/sdcard 已真实挂载，防止 SD 卡缺失时误写 rootfs。
     *
     * 参数：
     *   errorText 用于返回中文失败原因。
     *
     * 返回值：
     *   true 表示可以写配置；false 表示挂载点不可用。
     */
    bool isConfigMountReady(QString *errorText) const
    {
        FILE *mounts = std::fopen("/proc/mounts", "r");
        char device[256];
        char path[4096];

        if (!m_configPath.startsWith(QString::fromLatin1(DEFAULT_SDCARD_MOUNT_POINT))) {
            return true;
        }

        if (mounts == nullptr) {
            if (errorText) {
                *errorText = QStringLiteral("无法读取 /proc/mounts：")
                    + QString::fromLocal8Bit(strerror(errno));
            }
            return false;
        }

        while (std::fscanf(mounts, "%255s %4095s %*s %*s %*d %*d\n", device, path) == 2) {
            if (std::strcmp(path, DEFAULT_SDCARD_MOUNT_POINT) == 0) {
                std::fclose(mounts);
                return true;
            }
        }

        std::fclose(mounts);
        if (errorText) {
            *errorText = QString::fromLatin1(DEFAULT_SDCARD_MOUNT_POINT)
                + QStringLiteral(" 未挂载，参数JSON未写入");
        }
        return false;
    }

    /*
     * normalizedStepperMotors 的作用：
     *   清洗三台步进电机参数，避免 JSON 或 QML 输入越过 F4 固件安全边界。
     *
     * 主要流程：
     *   1. 以 defaultStepperMotorSettings() 为基准，确保始终只有三台已知电机。
     *   2. 只继承用户可调的 address/minStep/normalSpeedRpm/scanSpeedRpm/direction/zMotionTimeoutMs，不允许 JSON 改写 name/role/serialName。
     *   3. 对地址、步长、速度和方向做统一限幅；速度允许 0~5000，0 表示配置为常规停止速度。
     *
     * 参数：
     *   input 是待清洗的电机参数列表。
     *
     * 返回值：
     *   返回清洗后的三台电机配置。
     */
    QVector<StepperMotorSettings> normalizedStepperMotors(const QVector<StepperMotorSettings> &input) const
    {
        QVector<StepperMotorSettings> normalized = defaultStepperMotorSettings();
        const int motorCount = std::min(normalized.size(), input.size());

        for (int index = 0; index < motorCount; ++index) {
            const StepperMotorSettings source = input.at(index);
            StepperMotorSettings motor = normalized.at(index);

            motor.address = clampedInt(source.address, 1, 247);
            motor.minStep = clampedInt(source.minStep, 1, 10000);
            motor.normalSpeedRpm = clampedInt(source.normalSpeedRpm, 0, 5000);
            motor.scanSpeedRpm = clampedInt(source.scanSpeedRpm, 0, 5000);
            motor.direction = source.direction >= 0 ? 1 : -1;
            if (motor.role == QStringLiteral("camera_lateral")) {
                motor.lateralFineTuneFixedSteps = static_cast<quint32>(
                    clampedInt(static_cast<int>(std::min<quint32>(source.lateralFineTuneFixedSteps,
                                                                  MP157_CAMERA_LATERAL_FINE_TUNE_MAX_STEPS)),
                               static_cast<int>(MP157_CAMERA_LATERAL_FINE_TUNE_MIN_STEPS),
                               static_cast<int>(MP157_CAMERA_LATERAL_FINE_TUNE_MAX_STEPS)));
            } else {
                motor.lateralFineTuneFixedSteps = 0U;
            }
            motor.zDownFixedSteps = source.zDownFixedSteps;
            motor.zUpFixedSteps = source.zUpFixedSteps;
            motor.zMotionTimeoutMs = clampedInt(source.zMotionTimeoutMs,
                                                MP157_CAMERA_Z_TIMEOUT_MIN_MS,
                                                MP157_CAMERA_Z_TIMEOUT_MAX_MS);
            normalized[index] = motor;
        }

        return normalized;
    }

    /*
     * stepperMotorSettingsEqual 的作用：
     *   比较两组三台电机参数是否一致，用于避免重复发送 settingsChanged 信号。
     *
     * 参数：
     *   left/right 是两组已经或即将进入内存的步进电机配置。
     *
     * 返回值：
     *   完全一致返回 true；任一地址、步长、速度或方向不同返回 false。
     */
    bool stepperMotorSettingsEqual(const QVector<StepperMotorSettings> &left,
                                   const QVector<StepperMotorSettings> &right) const
    {
        if (left.size() != right.size()) {
            return false;
        }

        for (int index = 0; index < left.size(); ++index) {
            const StepperMotorSettings leftMotor = left.at(index);
            const StepperMotorSettings rightMotor = right.at(index);

            if (leftMotor.address != rightMotor.address
                    || leftMotor.minStep != rightMotor.minStep
                    || leftMotor.normalSpeedRpm != rightMotor.normalSpeedRpm
                    || leftMotor.scanSpeedRpm != rightMotor.scanSpeedRpm
                    || leftMotor.direction != rightMotor.direction
                    || leftMotor.lateralFineTuneFixedSteps != rightMotor.lateralFineTuneFixedSteps
                    || leftMotor.zDownFixedSteps != rightMotor.zDownFixedSteps
                    || leftMotor.zUpFixedSteps != rightMotor.zUpFixedSteps
                    || leftMotor.zMotionTimeoutMs != rightMotor.zMotionTimeoutMs) {
                return false;
            }
        }

        return true;
    }

    /*
     * normalizedSettings 的作用：
     *   统一清洗配置值，保证 JSON、QML 和默认值都落在同一合法范围内。
     *
     * 参数：
     *   input 是待清洗配置。
     *
     * 返回值：
     *   返回清洗后的配置。
     */
    DetectSettingsSnapshot normalizedSettings(const DetectSettingsSnapshot &input) const
    {
        DetectSettingsSnapshot next = input;
        const QStringList allowed = supportedPartTypes();

        if (!allowed.contains(next.partType)) {
            next.partType = allowed.constFirst();
        }

        next.modelThreshold = clampedDouble(next.modelThreshold, 0.50, 0.99);
        next.reviewThreshold = clampedDouble(next.reviewThreshold, 0.30, next.modelThreshold);
        next.roiSize = clampedInt(next.roiSize, 160, 640);
        next.segmentMinComponentPixels = clampedInt(next.segmentMinComponentPixels, 1, 50000);
        next.segmentReviewPixels = clampedInt(next.segmentReviewPixels,
                                              next.segmentMinComponentPixels,
                                              50000);
        next.segmentBadPixels = clampedInt(next.segmentBadPixels,
                                           next.segmentReviewPixels,
                                           50000);
        next.segmentStrongComponentPixels = clampedInt(next.segmentStrongComponentPixels,
                                                       next.segmentMinComponentPixels,
                                                       next.segmentBadPixels);
        next.overlayAlpha = clampedDouble(next.overlayAlpha, 0.0, 1.0);
        next.f4ArmResultTimeoutMs = clampedInt(next.f4ArmResultTimeoutMs,
                                               F4_ARM_ACTIVE_FRAME_TIMEOUT_MIN_MS,
                                               F4_ARM_ACTIVE_FRAME_TIMEOUT_MAX_MS);
        next.stepperMotors = normalizedStepperMotors(next.stepperMotors);
        return next;
    }

    /*
     * settingsEqual 的作用：
     *   判断两份配置是否完全一致，避免无意义 signal 抖动。
     */
    bool settingsEqual(const DetectSettingsSnapshot &left,
                       const DetectSettingsSnapshot &right) const
    {
        return left.partType == right.partType
            && qFuzzyCompare(left.modelThreshold + 1.0, right.modelThreshold + 1.0)
            && qFuzzyCompare(left.reviewThreshold + 1.0, right.reviewThreshold + 1.0)
            && left.roiSize == right.roiSize
            && left.segmentMinComponentPixels == right.segmentMinComponentPixels
            && left.segmentReviewPixels == right.segmentReviewPixels
            && left.segmentBadPixels == right.segmentBadPixels
            && left.segmentStrongComponentPixels == right.segmentStrongComponentPixels
            && qFuzzyCompare(left.overlayAlpha + 1.0, right.overlayAlpha + 1.0)
            && left.autoUploadEnabled == right.autoUploadEnabled
            && left.f4ArmResultTimeoutMs == right.f4ArmResultTimeoutMs
            && stepperMotorSettingsEqual(left.stepperMotors, right.stepperMotors);
    }

    /*
     * applySettings 的作用：
     *   应用一份新配置，并在有变化时发出 settingsChanged。
     *
     * 参数：
     *   next 是待应用配置。
     *   statusText 是要显示给 QML 的结果说明。
     */
    void applySettings(const DetectSettingsSnapshot &next,
                       const QString &statusText)
    {
        const DetectSettingsSnapshot normalized = normalizedSettings(next);
        const bool changed = !settingsEqual(m_settings, normalized);

        m_settings = normalized;
        setLastStatusText(statusText);
        if (changed) {
            emit settingsChanged();
        }
    }

    /*
     * setLastStatusText 的作用：
     *   集中更新最近状态文本，避免 QML 状态提示与 C++ 结果不一致。
     */
    void setLastStatusText(const QString &text)
    {
        if (m_lastStatusText == text) {
            return;
        }

        m_lastStatusText = text;
        emit lastStatusTextChanged();
    }

    QString m_configPath;               /* m_configPath 保存检测参数 JSON 绝对路径。 */
    DetectSettingsSnapshot m_settings;  /* m_settings 保存当前已加载或已修改的检测配置。 */
    QString m_lastStatusText;           /* m_lastStatusText 保存最近一次配置操作结果。 */
};

/*
 * CameraStorageController 的作用：
 *   给 QML 提供真实的 SD 卡图片保存和安全卸载操作。
 *
 * 主要流程：
 *   1. saveCurrentFrameToSdCard() 先确认 /mnt/sdcard 是真实挂载点，再连接 overlay 控制 socket。
 *   2. 发送 SAVE /mnt/sdcard/images，让 uvc_kms_overlay 保存当前正在显示的摄像头帧。
 *   3. safeRemoveSdCard() 调用现有 sdcard-safe-remove 命令，复用已经验证的同步和卸载脚本。
 *
 * 关键说明：
 *   KMS overlay 模式下摄像头画面不在 Qt Quick scene 内，Qt 截屏不会得到真实视频；
 *   因此保存图片必须让 overlay 进程自己从当前显示帧落盘。
 */
class CameraStorageController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool saveInProgress READ saveInProgress NOTIFY saveInProgressChanged)
    Q_PROPERTY(bool detectInProgress READ detectInProgress NOTIFY detectInProgressChanged)
    Q_PROPERTY(bool retryUploadInProgress READ retryUploadInProgress NOTIFY retryUploadInProgressChanged)

    /*
     * FusedDetectResult 的作用：
     *   保存分类模型和 UNet 分割模型综合后的最终判定。
     *
     * 字段说明：
     *   cloudResult 是云端 records.result 使用的小写结果，取值 good/bad/review。
     *   historyText 是本地历史列表展示的中文主结果，良品表示两个模型均未发现缺陷。
     *   uiStatus 是 QML 首页结果卡片使用的状态，取值 GOOD/BAD/REVIEW。
     *   reason 是不带空格的中文短原因，会追加到 RESULT 行供 QML 和日志读取。
     *   segmentEvidence 保存 CLEAR/WEAK/STRONG 原始证据等级。
     *   classifyBad/segmentBad/segmentReview 保存分类坏品、UNet 强缺陷和 UNet 弱缺陷状态。
     */
    struct FusedDetectResult
    {
        QString cloudResult;
        QString historyText;
        QString uiStatus;
        QString reason;
        QString segmentEvidence;
        bool classifyBad = false;
        bool segmentBad = false;
        bool segmentReview = false;
    };

public:
    explicit CameraStorageController(QObject *parent = nullptr)
        : QObject(parent),
          m_socketPath(QString::fromLatin1(DEFAULT_OVERLAY_CONTROL_SOCKET)),
          m_mountPoint(QString::fromLatin1(DEFAULT_SDCARD_MOUNT_POINT)),
          m_imageDir(QString::fromLatin1(DEFAULT_SDCARD_IMAGE_DIR)),
          m_logDir(QString::fromLatin1(DEFAULT_SDCARD_LOG_DIR)),
          m_historyModel(nullptr),
          m_detectSettingsController(nullptr),
          m_latestDetectHistoryRow(-1),
          m_appendHistoryInSave(true),
          m_saveInProgress(false),
          m_detectInProgress(false),
          m_retryUploadInProgress(false),
          m_hasLatestDetectBundle(false)
    {
    }

    /*
     * saveInProgress 的作用：
     *   告诉 QML 当前是否已有保存图片后台任务正在运行。
     *
     * 主要流程：
     *   直接返回主线程维护的 m_saveInProgress 标志；后台线程不能直接写这个标志，
     *   必须通过 Qt queued signal 回到控制器线程后再更新。
     *
     * 返回值：
     *   true 表示保存和上传任务尚未结束，QML 应禁止重复点击保存；
     *   false 表示可以发起新的保存请求。
     */
    bool saveInProgress() const
    {
        return m_saveInProgress;
    }

    /*
     * detectInProgress 的作用：
     *   告诉 QML 当前是否已有检测后台任务正在运行。
     *
     * 主要流程：
     *   直接返回主线程维护的 m_detectInProgress 标志；后台线程只能通过 queued signal
     *   回到主线程后改变它，避免跨线程直接写 QObject 状态。
     *
     * 返回值：
     *   true 表示当前帧保存和 defect-classify 推理尚未结束；
     *   false 表示检测按钮可以再次点击。
     */
    bool detectInProgress() const
    {
        return m_detectInProgress;
    }

    /*
     * retryUploadInProgress 的作用：
     *   告诉 QML 当前是否已有历史图片重新发送任务正在运行。
     *
     * 主要流程：
     *   直接返回主线程维护的 m_retryUploadInProgress；后台线程结束后必须通过
     *   Qt queued connection 回到主线程清除该标志。
     *
     * 返回值：
     *   true 表示正在重发某条失败历史记录；false 表示可以点击“重新发送”。
     */
    bool retryUploadInProgress() const
    {
        return m_retryUploadInProgress;
    }

    /*
     * setHistoryModel 的作用：
     *   把 main 中创建的上传历史模型交给保存控制器。
     *
     * 主要流程：
     *   保存控制器只持有指针，不拥有模型生命周期；模型对象由 main 栈变量和 Qt 对象树管理。
     *
     * 参数：
     *   model 是要追加记录的 UploadHistoryModel。
     *
     * 返回值：
     *   无返回值。
     */
    void setHistoryModel(UploadHistoryModel *model)
    {
        m_historyModel = model;
    }

    /*
     * setDetectSettingsController 的作用：
     *   把参数设置控制器交给检测控制器，后续每次检测前读取最新配置快照。
     *
     * 参数：
     *   controller 是 main 中创建的 DetectSettingsController，生命周期长于 CameraStorageController。
     *
     * 返回值：
     *   无返回值。
     */
    void setDetectSettingsController(DetectSettingsController *controller)
    {
        m_detectSettingsController = controller;
        if (m_detectSettingsController != nullptr) {
            m_detectSettingsSnapshot = m_detectSettingsController->settingsSnapshot();
        }
    }

    /*
     * detectSettingsController 的作用：
     *   返回当前绑定的参数控制器指针，主要用于静态契约和必要调试。
     *
     * 返回值：
     *   返回 DetectSettingsController 指针；未绑定时返回 nullptr。
     */
    DetectSettingsController *detectSettingsController() const
    {
        return m_detectSettingsController;
    }

    /*
     * setDetectSettingsSnapshot 的作用：
     *   给后台 worker 控制器注入主线程复制出的检测配置。
     *
     * 参数：
     *   settings 是检测开始瞬间的配置快照。
     *
     * 返回值：
     *   无返回值。
     */
    void setDetectSettingsSnapshot(const DetectSettingsSnapshot &settings)
    {
        m_detectSettingsSnapshot = settings;
    }

    /*
     * detectSettings 的作用：
     *   读取当前检测配置；主线程优先读控制器，后台线程使用复制出来的快照。
     *
     * 返回值：
     *   返回参与本次检测的 DetectSettingsSnapshot。
     */
    DetectSettingsSnapshot detectSettings() const
    {
        if (m_detectSettingsController != nullptr) {
            return m_detectSettingsController->settingsSnapshot();
        }

        return m_detectSettingsSnapshot;
    }

    /*
     * setStoragePaths 的作用：
     *   允许后台保存线程复用主控制器的 socket、挂载点、图片目录和诊断目录配置。
     *
     * 主要流程：
     *   只复制路径字符串，不复制 QObject 指针或 QML 模型，避免跨线程访问主线程对象。
     *
     * 参数：
     *   socketPath 是 overlay 控制 socket 路径。
     *   mountPoint 是 SD 卡挂载点。
     *   imageDir 是 JPG/PNG 图片保存目录。
     *   logDir 是诊断日志目录。
     * 返回值：
     *   无返回值。
     */
    void setStoragePaths(const QString &socketPath,
                         const QString &mountPoint,
                         const QString &imageDir,
                         const QString &logDir)
    {
        m_socketPath = socketPath;
        m_mountPoint = mountPoint;
        m_imageDir = imageDir;
        m_logDir = logDir;
    }

    /*
     * setAppendHistoryInSave 的作用：
     *   控制 saveCurrentFrameToSdCard() 内部是否直接追加上传历史记录。
     *
     * 主要流程：
     *   同步自检和旧同步调用保持默认 true；异步保存的后台控制器设置为 false，
     *   因为异步路径必须回到 Qt 主线程后再更新 UploadHistoryModel。
     *
     * 参数：
     *   enabled 为 true 时同步保存函数内部追加历史；false 时只返回保存/上传结果。
     *
     * 返回值：
     *   无返回值。
     */
    void setAppendHistoryInSave(bool enabled)
    {
        m_appendHistoryInSave = enabled;
    }

    /*
     * setOverlayVisible 的作用：
     *   通知 KMS overlay 进程隐藏或恢复实时视频 plane。
     *
     * 主要流程：
     *   1. 根据 visible 拼出 `VISIBLE 1` 或 `VISIBLE 0` 控制命令。
     *   2. 通过同一个 Unix socket 发给 overlay 进程。
     *   3. 返回 overlay 的中文结果，便于必要时放进日志或界面诊断。
     *
     * 参数：
     *   visible 为 true 时恢复实时视频，false 时隐藏实时视频。
     *
     * 返回值：
     *   成功返回“视频层已显示/已隐藏”；失败返回原因文本。
     */
    Q_INVOKABLE QString setOverlayVisible(bool visible)
    {
        const QString result = sendOverlayCommand(visible
            ? QStringLiteral("VISIBLE 1")
            : QStringLiteral("VISIBLE 0"));

        qInfo() << "overlay visibility requested" << visible << "result" << result;
        return result;
    }

    /*
     * saveCurrentFrameToSdCard 的作用：
     *   响应 QML 的“保存图片”按钮，把 overlay 当前帧保存到 SD 卡。
     *
     * 返回值：
     *   成功返回“保存成功：<路径>”；失败返回“保存失败：<原因>”。
     */
    Q_INVOKABLE QString saveCurrentFrameToSdCard()
    {
        QString mountError;
        QString result;

        qInfo() << "storage action save-image requested"
                << "mount" << m_mountPoint
                << "imageDir" << m_imageDir
                << "socket" << m_socketPath;

        if (!isMountPointMounted(m_mountPoint, &mountError)) {
            result = QStringLiteral("保存失败：") + mountError;
            qWarning() << "storage action save-image result" << result;
            return result;
        }

        if (!QDir().mkpath(m_imageDir)) {
            result = QStringLiteral("保存失败：无法创建 ") + m_imageDir;
            qWarning() << "storage action save-image result" << result;
            return result;
        }

        result = sendOverlayCommand(QStringLiteral("SAVE_DUAL ") + m_imageDir);
        if (result.startsWith(QStringLiteral("保存成功："))) {
            qInfo() << "storage action save-image result" << result;
        } else {
            qWarning() << "storage action save-image result" << result;
        }
        return result;
    }

    /*
     * requestSaveCurrentFrameToSdCard 的作用：
     *   给 QML 使用的异步保存入口，避免点击“保存图片”时阻塞 Qt 主线程和其它页面触摸。
     *
     * 主要流程：
     *   1. 如果已有保存任务在运行，只记录日志并忽略重复请求，防止 QML 忙状态被提前清除。
     *   2. 把保存目录、挂载点、socket 等当前配置复制出来，交给后台线程使用。
     *   3. 后台线程创建独立 CameraStorageController，复用原同步保存逻辑完成本地保存和 COS 上传。
     *   4. 任务结束后回到主线程追加历史记录、清除忙标志并发出 saveCurrentFrameFinished。
     *
     * 返回值：
     *   无直接返回值；QML 通过 saveCurrentFrameFinished(resultText) 获取最终结果。
     */
    Q_INVOKABLE void requestSaveCurrentFrameToSdCard()
    {
        if (m_saveInProgress) {
            qWarning() << "storage action save-image ignored because previous save is still running";
            return;
        }

        /* 先把忙标志置位，让 QML 立即显示保存中并阻止重复保存。 */
        setSaveInProgress(true);

        /* socketPath 保存本次后台任务使用的 overlay 控制端点，复制后可安全跨线程读取。 */
        const QString socketPath = m_socketPath;

        /* mountPoint 保存 SD 卡挂载点，后台线程用它确认不会误写 rootfs。 */
        const QString mountPoint = m_mountPoint;

        /* imageDir 保存 JPG/PNG 输出目录，后台线程会请求 overlay 写入这里。 */
        const QString imageDir = m_imageDir;

        /* logDir 保存诊断日志目录，保持后台控制器和主控制器路径配置一致。 */
        const QString logDir = m_logDir;

        /* workerResult 保存后台线程执行结果，线程结束后主线程从这里读取并更新 QML。 */
        const QSharedPointer<QString> workerResult(new QString(QStringLiteral("保存失败：后台保存线程没有返回结果")));

        /* workerThread 承载耗时的保存和上传逻辑，避免阻塞 Qt 主线程的触摸事件循环。 */
        QThread *workerThread = QThread::create([socketPath,
                                                 mountPoint,
                                                 imageDir,
                                                 logDir,
                                                 workerResult]() {
            CameraStorageController workerController;

            workerController.setStoragePaths(socketPath,
                                             mountPoint,
                                             imageDir,
                                             logDir);
            workerController.setAppendHistoryInSave(false);

            *workerResult = workerController.saveCurrentFrameToSdCard();
        });

        if (workerThread == nullptr) {
            setSaveInProgress(false);
            emit saveCurrentFrameFinished(QStringLiteral("保存失败：无法创建后台保存线程"));
            return;
        }

        connect(workerThread, &QThread::finished, this, [this, workerResult]() {
            appendUploadHistoryRecordFromResult(*workerResult);

            setSaveInProgress(false);
            emit saveCurrentFrameFinished(*workerResult);
        }, Qt::QueuedConnection);

        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);

        workerThread->start();
    }

    /*
     * requestDetectCurrentFrame 的作用：
     *   给 QML 首页“检测”按钮使用的异步检测入口。
     *
     * 主要流程：
     *   1. 防止重复点击，先把检测忙标志置位。
     *   2. 复制 overlay socket、临时目录、模型路径和标签路径，交给后台线程使用。
     *   3. 后台线程让 overlay 执行 SAVE_DETECT，只保存一张 JPG 到 /tmp。
     *   4. 后台线程调用 defect-classify 独立程序完成 ONNX Runtime 推理。
     *   5. 任务结束后回到主线程清除忙标志，并把 RESULT 行传给 QML 解析显示。
     *
     * 返回值：
     *   无直接返回值；QML 通过 detectCurrentFrameFinished(resultText) 获取最终结果。
     */
    Q_INVOKABLE void requestDetectCurrentFrame()
    {
        if (m_detectInProgress) {
            qWarning() << "detect action ignored because previous detect is still running";
            return;
        }

        /* 先置位忙状态，让 QML 立即把按钮切到“检测中”。 */
        setDetectInProgress(true);

        /* socketPath 保存 overlay 控制端点，后台线程通过它请求当前帧 JPG。 */
        const QString socketPath = m_socketPath;

        /* imageDir 保存检测图片目录；正式检测结果写入 SD 卡，才能进入历史记录并重启后继续预览。 */
        const QString imageDir = m_imageDir;

        /* mountPoint 保存 SD 卡挂载点，检测历史图写入前必须确认真实挂载。 */
        const QString mountPoint = m_mountPoint;

        /* classifyBin 保存独立推理程序路径，部署后默认在 /root/qt_camera_display 下。 */
        const QString classifyBin = QString::fromLatin1(DEFAULT_DEFECT_CLASSIFY_BIN);

        /* classifyModelPath 保存分类 INT8 ONNX 模型路径，必须和部署脚本复制位置一致。 */
        const QString classifyModelPath = QString::fromLatin1(DEFAULT_DEFECT_CLASSIFY_MODEL);

        /* labelsPath 保存类别映射路径，保证板端输出类别顺序不靠硬编码猜测。 */
        const QString labelsPath = QString::fromLatin1(DEFAULT_DEFECT_CLASSIFY_LABELS);

        /* segmentBin 保存 UNet 分割推理程序路径，分类结束后再启动它。 */
        const QString segmentBin = QString::fromLatin1(DEFAULT_DEFECT_SEGMENT_BIN);

        /* segmentModelPath 保存 UNet INT8 ONNX 模型路径，必须和部署脚本复制位置一致。 */
        const QString segmentModelPath = QString::fromLatin1(DEFAULT_DEFECT_SEGMENT_MODEL);

        /* settings 保存检测开始瞬间的真实参数快照，后台线程使用它而不是读取会变化的 QML 属性。 */
        const DetectSettingsSnapshot settings = detectSettings();

        /* workerResult 保存后台线程最终结果，线程结束后由主线程读取并通知 QML。 */
        const QSharedPointer<QString> workerResult(new QString(QStringLiteral("检测失败：后台检测线程没有返回结果")));

        /* workerBundle 保存后台线程产出的图片路径和模型输出，线程结束后由主线程追加历史。 */
        const QSharedPointer<DetectResultBundle> workerBundle(new DetectResultBundle);

        /* controllerPtr 是安全指针；如果界面关闭导致控制器销毁，后台线程不会再投递进度信号。 */
        const QPointer<CameraStorageController> controllerPtr(this);

        /* workerThread 承载保存当前帧和模型推理两个耗时动作，避免阻塞触摸事件循环。 */
        QThread *workerThread = QThread::create([socketPath,
                                                 mountPoint,
                                                 imageDir,
                                                 classifyBin,
                                                 classifyModelPath,
                                                 labelsPath,
                                                 segmentBin,
                                                 segmentModelPath,
                                                 settings,
                                                 controllerPtr,
                                                 workerBundle,
                                                 workerResult]() {
            CameraStorageController workerController;
            const auto emitClassificationReady = [controllerPtr](const QString &classificationResult) {
                /* 分类模型结束后立即把零件类型、类别和 GOOD/BAD 结果送回 QML；不等待 UNet 或上传。 */
                if (controllerPtr.isNull()) {
                    return;
                }

                QMetaObject::invokeMethod(controllerPtr.data(),
                                          "detectClassificationReady",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, classificationResult));
            };
            const auto emitModelsReady = [controllerPtr](const QString &modelResult) {
                /* 两个模型都结束后立即把 total_time_ms 送回 QML；后续 COS 上传继续在后台执行。 */
                if (controllerPtr.isNull()) {
                    return;
                }

                QMetaObject::invokeMethod(controllerPtr.data(),
                                          "detectModelsReady",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, modelResult));
            };

            workerController.setStoragePaths(socketPath,
                                             mountPoint,
                                             imageDir,
                                             QString::fromLatin1(DEFAULT_SDCARD_LOG_DIR));
            workerController.setAppendHistoryInSave(false);
            workerController.setDetectSettingsSnapshot(settings);

            *workerResult = workerController.detectCurrentFrameOnce(mountPoint,
                                                                    imageDir,
                                                                    classifyBin,
                                                                    classifyModelPath,
                                                                    labelsPath,
                                                                    segmentBin,
                                                                    segmentModelPath,
                                                                    settings,
                                                                    workerBundle.data(),
                                                                    emitClassificationReady,
                                                                    emitModelsReady);
        });

        if (workerThread == nullptr) {
            setDetectInProgress(false);
            emit detectCurrentFrameFinished(QStringLiteral("检测失败：无法创建后台检测线程"));
            return;
        }

        connect(workerThread, &QThread::finished, this, [this, workerResult, workerBundle]() {
            if (workerResult->startsWith(QStringLiteral("RESULT "))
                    && !workerBundle->sourcePath.isEmpty()) {
                m_latestDetectHistoryRow = appendDetectHistoryRecord(*workerBundle);
                m_latestDetectBundle = *workerBundle;
                m_hasLatestDetectBundle = true;
                qInfo() << "detect latest bundle cached"
                        << "row" << m_latestDetectHistoryRow
                        << "source" << m_latestDetectBundle.sourcePath
                        << "annotated" << m_latestDetectBundle.annotatedPaths.size();
            }

            setDetectInProgress(false);
            emit detectCurrentFrameFinished(*workerResult);
        }, Qt::QueuedConnection);

        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);

        workerThread->start();
    }

    /*
     * detectCurrentFrameForSelfTest 的作用：
     *   给 SSH `--detect-self-test` 使用的同步双模型检测入口。
     *
     * 主要流程：
     *   1. 直接复用 detectCurrentFrameOnce()，执行 SAVE_DETECT、分类、UNet、上传。
     *   2. 检测成功时在当前线程追加历史记录，保证 SSH 自检和屏幕点击写同一种 JSON。
     *   3. 返回 RESULT 或“检测失败”，由命令行入口打印到 stdout。
     *
     * 返回值：
     *   成功返回 RESULT 行；失败返回“检测失败：...”。
     */
    QString detectCurrentFrameForSelfTest()
    {
        DetectResultBundle bundle;
        const QString result = detectCurrentFrameOnce(
            m_mountPoint,
            m_imageDir,
            QString::fromLatin1(DEFAULT_DEFECT_CLASSIFY_BIN),
            QString::fromLatin1(DEFAULT_DEFECT_CLASSIFY_MODEL),
            QString::fromLatin1(DEFAULT_DEFECT_CLASSIFY_LABELS),
            QString::fromLatin1(DEFAULT_DEFECT_SEGMENT_BIN),
            QString::fromLatin1(DEFAULT_DEFECT_SEGMENT_MODEL),
            detectSettings(),
            &bundle);

        if (result.startsWith(QStringLiteral("RESULT "))
                && !bundle.sourcePath.isEmpty()) {
            m_latestDetectHistoryRow = appendDetectHistoryRecord(bundle);
            m_latestDetectBundle = bundle;
            m_hasLatestDetectBundle = true;
        }

        return result;
    }

    /*
     * safeRemoveSdCard 的作用：
     *   响应 QML 的“安全卸载”按钮，执行现有 sdcard-safe-remove 命令。
     *
     * 返回值：
     *   命令成功返回“卸载完成：...”；失败返回“卸载失败：...”。
     */
    Q_INVOKABLE QString safeRemoveSdCard()
    {
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QString result;

        qInfo() << "storage action safe-remove requested";

        /* Qt 按钮只负责发起同步和卸载；物理拔卡检测继续由后台脚本维护，避免界面长时间卡在等待拔卡。 */
        env.insert(QStringLiteral("EJECT_WAIT_TIMEOUT"), QStringLiteral("1"));

        process.setProcessEnvironment(env);
        process.setProgram(QStringLiteral("sdcard-safe-remove"));
        /* 板端 /usr/bin/sdcard-safe-remove 当前复用 S85sdcard-mount 入口，必须显式传入 safe-remove 子命令；裸命令只会打印 Usage。 */
        process.setArguments(QStringList() << QStringLiteral("safe-remove"));
        process.start();

        if (!process.waitForStarted(2000)) {
            result = QStringLiteral("卸载失败：无法启动 sdcard-safe-remove");
            qWarning() << "storage action safe-remove result" << result;
            return result;
        }

        if (!process.waitForFinished(70000)) {
            process.kill();
            process.waitForFinished(1000);
            result = QStringLiteral("卸载失败：sdcard-safe-remove 超时");
            qWarning() << "storage action safe-remove result" << result;
            return result;
        }

        const QString stdoutText = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        const QString stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();

        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            if (!stderrText.isEmpty()) {
                result = QStringLiteral("卸载失败：") + firstUsefulLine(stderrText);
                qWarning() << "storage action safe-remove result" << result;
                return result;
            }
            if (!stdoutText.isEmpty()) {
                result = QStringLiteral("卸载失败：") + firstUsefulLine(stdoutText);
                qWarning() << "storage action safe-remove result" << result;
                return result;
            }
            result = QStringLiteral("卸载失败：sdcard-safe-remove 返回异常");
            qWarning() << "storage action safe-remove result" << result;
            return result;
        }

        if (!stdoutText.isEmpty()) {
            result = QStringLiteral("卸载完成：") + firstUsefulLine(stdoutText);
            qInfo() << "storage action safe-remove result" << result;
            return result;
        }

        result = QStringLiteral("卸载完成：SD 卡已同步并卸载");
        qInfo() << "storage action safe-remove result" << result;
        return result;
    }

    /*
     * saveAlarmSnapshotToSdCard 的作用：
     *   响应告警维护页“保存诊断”按钮，把 QML 汇总的告警状态追加到当天诊断快照文件。
     *
     * 主要流程：
     *   1. 通过 saveAlarmTextToSdCard() 复用挂载点检查、目录创建、可写校验和 fsync 写入逻辑。
     *   2. 每次调用都写入 qt_alarm_snapshot_YYYYMMDD.txt，当天多次点击追加到同一个文件。
     *   3. 把真实文件路径返回给 QML，现场人员可直接 SSH 打开当天快照集合。
     *
     * 参数：
     *   snapshotText 是 QML 组装的告警码、处理状态、设备健康和参数摘要。
     *
     * 返回值：
     *   成功返回“诊断已保存：/mnt/sdcard/logs/qt_alarm_snapshot_YYYYMMDD.txt”；
     *   失败返回“诊断保存失败：<中文原因>”。
     */
    Q_INVOKABLE QString saveAlarmSnapshotToSdCard(const QString &snapshotText)
    {
        return saveAlarmTextToSdCard(QStringLiteral("alarm-snapshot"),
                                     QStringLiteral("诊断"),
                                     QString::fromLatin1(ALARM_SNAPSHOT_PREFIX),
                                     QString(),
                                     QStringLiteral(".txt"),
                                     snapshotText);
    }

    /*
     * recordAlarmIssueToSdCard 的作用：
     *   响应 QML 自动告警触发，把新出现的真实问题追加到当天告警日志。
     *
     * 主要流程：
     *   1. QML 在健康状态、保存/上传/模型结果变化时判定是否出现新问题。
     *   2. C++ 按当天日期写入 qt_alarm_YYYYMMDD.log，并在每段中记录告警来源。
     *   3. 复用 appendTextFileWithFsync() 完成 UTF-8 追加、flush 和 fsync。
     *
     * 参数：
     *   sourceKey 是告警来源标识，例如 camera-kms-no-frame、cloud-offline。
     *   alarmText 是 QML 组装的发生时间、问题、状态和排查建议。
     *
     * 返回值：
     *   成功返回“告警日志已保存：/mnt/sdcard/logs/qt_alarm_YYYYMMDD.log”；
     *   失败返回“告警日志保存失败：<中文原因>”。
     */
    Q_INVOKABLE QString recordAlarmIssueToSdCard(const QString &sourceKey, const QString &alarmText)
    {
        return saveAlarmTextToSdCard(QStringLiteral("alarm-log"),
                                     QStringLiteral("告警日志"),
                                     QString::fromLatin1(ALARM_LOG_PREFIX),
                                     sourceKey,
                                     QStringLiteral(".log"),
                                     alarmText);
    }

    /*
     * recordSettingsSummaryToSdCard 的作用：
     *   响应参数设置页“保存配置”和“导出摘要”，把当前参数摘要追加到日志查看页可见的每日文件。
     *
     * 主要流程：
     *   1. QML 负责组装当前界面参数、JSON 路径、保存结果和操作来源。
     *   2. C++ 复用 saveAlarmTextToSdCard() 的挂载检查、目录创建、flush 和 fsync 逻辑。
     *   3. 日志写入 /mnt/sdcard/logs/qt_settings_YYYYMMDD.log，日志查看页刷新后可以直接点开全文。
     *
     * 参数：
     *   actionKey 是 settings-save 或 settings-export 等安全来源标识。
     *   summaryText 是 QML 组装的参数摘要正文。
     *
     * 返回值：
     *   成功返回“参数日志已保存：/mnt/sdcard/logs/qt_settings_YYYYMMDD.log”；
     *   失败返回“参数日志保存失败：<中文原因>”。
     */
    Q_INVOKABLE QString recordSettingsSummaryToSdCard(const QString &actionKey, const QString &summaryText)
    {
        return saveAlarmTextToSdCard(QStringLiteral("settings-summary"),
                                     QStringLiteral("参数日志"),
                                     QString::fromLatin1(SETTINGS_LOG_PREFIX),
                                     actionKey,
                                     QStringLiteral(".log"),
                                     summaryText);
    }

    /*
     * retryUploadRecord 的作用：
     *   响应历史详情页“重新发送”按钮，把已保存的本地 source/annotated 图片重新上传到云端。
     *
     * 主要流程：
     *   1. 校验历史模型和 row，读取该记录原始图片、结果图和分类结果。
     *   2. 优先读取历史中保存的 cloud_result、零件身份和传感器上下文，保证断网重传仍是同一条完整记录。
     *   3. 旧记录缺少完整上下文时，根据分类 RESULT 恢复云端 good/bad/review 判定，证据不足时保守使用 review。
     *   4. 后台线程复用 defect-cos-upload 执行网络上传，避免阻塞 Qt 主线程。
     *   5. 上传结束后回到主线程原地更新同一条历史记录的 upload_status、record_id 和 record_no。
     *
     * 参数：
     *   row 是 QML 当前详情页对应的历史记录索引。
     *
     * 返回值：
     *   无直接返回值；QML 通过 retryUploadFinished(row, resultText) 显示最终结果。
     */
    Q_INVOKABLE void retryUploadRecord(int row)
    {
        QString sourcePath;
        QStringList annotatedPaths;
        QString classificationResult;
        QString segmentationResult;
        QString storedCloudResult;
        QString storedPartCode;
        QString storedClassLabel;
        QString weightContextJson;
        QString ldcContextJson;
        QString f4FlowContextJson;
        QString decisionContextJson;
        QString visionContextJson;
        QString errorText;

        if (m_retryUploadInProgress) {
            qWarning() << "history retry upload ignored because previous retry is still running";
            emit retryUploadFinished(row, QStringLiteral("重新发送失败：已有图片正在重新发送"));
            return;
        }

        if (m_historyModel == nullptr) {
            emit retryUploadFinished(row, QStringLiteral("重新发送失败：历史模型未初始化"));
            return;
        }

        if (!m_historyModel->retryPayloadAt(row,
                                            &sourcePath,
                                            &annotatedPaths,
                                            &classificationResult,
                                            &segmentationResult,
                                            &storedCloudResult,
                                            &storedPartCode,
                                            &storedClassLabel,
                                            &weightContextJson,
                                            &ldcContextJson,
                                            &f4FlowContextJson,
                                            &decisionContextJson,
                                            &visionContextJson,
                                            &errorText)) {
            emit retryUploadFinished(row, QStringLiteral("重新发送失败：") + errorText);
            return;
        }

        /*
         * fusedResult 保存分类模型和 UNet 分割模型共同生成的最终判定。
         * 旧历史缺少任一模型结果时会落到 review，避免重新发送时把证据不完整的记录误写成良品。
         */
        const FusedDetectResult fusedResult =
            fusedResultFromModelResults(classificationResult, segmentationResult, detectSettings());

        /* cloudResult 保存云端 records.result 字段，必须来自综合判定而不是单个分类模型。 */
        const QString fallbackCloudResult = cloudResultFromFusedResult(fusedResult);
        const QString cloudResult = (storedCloudResult == QStringLiteral("good")
                                     || storedCloudResult == QStringLiteral("bad")
                                     || storedCloudResult == QStringLiteral("review"))
            ? storedCloudResult
            : fallbackCloudResult;

        /* workerResult 保存后台上传脚本返回的完整中文状态，线程结束后主线程读取并更新历史记录。 */
        const QSharedPointer<QString> workerResult(new QString(QStringLiteral("上传失败：后台重新发送线程没有返回结果")));

        setRetryUploadInProgress(true);

        QThread *workerThread = QThread::create([sourcePath,
                                                 annotatedPaths,
                                                 classificationResult,
                                                 cloudResult,
                                                 storedPartCode,
                                                 storedClassLabel,
                                                 weightContextJson,
                                                 ldcContextJson,
                                                 f4FlowContextJson,
                                                 decisionContextJson,
                                                 visionContextJson,
                                                 workerResult]() {
            CameraStorageController workerController;

            workerController.setAppendHistoryInSave(false);
            const QString retryPartCode = storedPartCode.trimmed().isEmpty()
                ? workerController.partCodeFromClassificationResult(classificationResult)
                : storedPartCode.trimmed();
            const QString retryClassLabel = storedClassLabel.trimmed().isEmpty()
                ? workerController.parseTokenValue(classificationResult, QStringLiteral("class"))
                : storedClassLabel.trimmed();

            *workerResult = workerController.uploadDetectImagesToCos(sourcePath,
                                                                     annotatedPaths,
                                                                     cloudResult,
                                                                     retryPartCode,
                                                                     retryClassLabel,
                                                                     weightContextJson,
                                                                     ldcContextJson,
                                                                     f4FlowContextJson,
                                                                     decisionContextJson,
                                                                     visionContextJson);
        });

        if (workerThread == nullptr) {
            setRetryUploadInProgress(false);
            emit retryUploadFinished(row, QStringLiteral("重新发送失败：无法创建后台上传线程"));
            return;
        }

        connect(workerThread, &QThread::finished, this, [this, row, workerResult]() {
            QString errorText;
            const QString recordId = parseTokenValue(*workerResult, QStringLiteral("record_id"));
            const QString recordNo = parseTokenValue(*workerResult, QStringLiteral("record_no"));
            const QString compactStatus = compactUploadStatus(*workerResult);
            QString resultText;

            if (m_historyModel == nullptr) {
                resultText = QStringLiteral("重新发送失败：历史模型已释放");
            } else if (!m_historyModel->updateRecordUploadResult(row,
                                                                 compactStatus,
                                                                 recordId,
                                                                 recordNo,
                                                                 &errorText)) {
                resultText = QStringLiteral("重新发送失败：") + errorText;
            } else if (isUploadStatusSuccess(*workerResult)) {
                resultText = QStringLiteral("重新发送成功：") + compactStatus;
            } else {
                const QString failureDetail = isUploadStatusFailure(*workerResult)
                    ? workerResult->mid(QStringLiteral("上传失败：").length())
                    : *workerResult;

                resultText = QStringLiteral("重新发送失败：") + failureDetail.left(80);
            }

            setRetryUploadInProgress(false);
            emit retryUploadFinished(row, resultText);
        }, Qt::QueuedConnection);

        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);

        workerThread->start();
    }

    /*
     * uploadCompletedInspectionBundle 的作用：
     *   自动流程在收齐模型、称重、电感和 F4/ESP32 流程上下文后，一次性上传完整检测记录。
     *
     * 主要流程：
     *   1. 校验最近一次模型检测 bundle 已经保存到 SD 卡，且没有其它上传线程正在运行。
     *   2. 根据 bundle 中的分类、UNet 和参数快照生成云端 `vision_context` 与 `decision_context`。
     *   3. 把 F4 主动上报转换出的称重、电感和流程 JSON 通过环境变量传给 `defect-cos-upload`。
     *   4. 上传完成后回到主线程，更新刚才那条本地历史记录的 upload_status、record_id 和 record_no。
     *
     * 参数：
     *   weightContextJson 是 `WEIGHT_RESULT` 解析后的称重上下文 JSON。
     *   ldcContextJson 是 `LDC_RESULT` 解析后的电感上下文 JSON。
     *   f4FlowContextJson 是 F4/ESP32 阶段事件和完成状态上下文 JSON。
     *
     * 返回值：
     *   true 表示后台完整上传任务已启动；false 表示缺少本地检测结果或当前已有上传任务。
     */
    Q_INVOKABLE bool uploadCompletedInspectionBundle(const QString &weightContextJson,
                                                     const QString &ldcContextJson,
                                                     const QString &f4FlowContextJson)
    {
        if (m_retryUploadInProgress) {
            emit completedInspectionBundleUploaded(false,
                                                   QStringLiteral("完整上传失败：已有上传任务正在运行"),
                                                   QStringLiteral("review"));
            return false;
        }

        if (!m_hasLatestDetectBundle || m_latestDetectBundle.sourcePath.isEmpty()) {
            emit completedInspectionBundleUploaded(false,
                                                   QStringLiteral("完整上传失败：没有可上传的最近检测结果"),
                                                   QStringLiteral("review"));
            return false;
        }

        const DetectResultBundle bundle = m_latestDetectBundle;
        const int historyRow = m_latestDetectHistoryRow;
        const QString decisionContextJson = buildDecisionContextJson(bundle);
        const QString visionContextJson = buildVisionContextJson(bundle);
        const FusedDetectResult fusedResult =
            fusedResultFromModelResults(bundle.classificationResult,
                                        bundle.segmentationResult,
                                        bundle.settings);
        const QString cloudResult = cloudResultFromFusedResult(fusedResult);
        const QString partCode = partCodeFromClassificationResult(bundle.classificationResult);
        const QString classLabel = parseTokenValue(bundle.classificationResult, QStringLiteral("class"));
        QString persistError;

        if (m_historyModel == nullptr || historyRow < 0) {
            emit completedInspectionBundleUploaded(false,
                                                   QStringLiteral("完整上传失败：没有可写入完整上下文的历史记录"),
                                                   cloudResult);
            return false;
        }

        const bool contextPersisted = m_historyModel->updateRecordInspectionContexts(historyRow,
                                                                                     cloudResult,
                                                                                     partCode,
                                                                                     classLabel,
                                                                                     weightContextJson,
                                                                                     ldcContextJson,
                                                                                     f4FlowContextJson,
                                                                                     decisionContextJson,
                                                                                     visionContextJson,
                                                                                     &persistError);
        if (!contextPersisted) {
            qWarning() << "completed inspection context persist failed before upload" << persistError;
            emit completedInspectionBundleUploaded(false,
                                                   QStringLiteral("完整上传失败：") + persistError,
                                                   cloudResult);
            return false;
        }

        setRetryUploadInProgress(true);

        const QSharedPointer<QString> workerResult(new QString(QStringLiteral("上传失败：完整自动检测上传线程没有返回结果")));
        QThread *workerThread = QThread::create([bundle,
                                                 cloudResult,
                                                 partCode,
                                                 classLabel,
                                                 weightContextJson,
                                                 ldcContextJson,
                                                 f4FlowContextJson,
                                                 decisionContextJson,
                                                 visionContextJson,
                                                 workerResult]() {
            CameraStorageController workerController;

            workerController.setAppendHistoryInSave(false);
            *workerResult = workerController.uploadDetectImagesToCos(bundle.sourcePath,
                                                                     bundle.annotatedPaths,
                                                                     cloudResult,
                                                                     partCode,
                                                                     classLabel,
                                                                     weightContextJson,
                                                                     ldcContextJson,
                                                                     f4FlowContextJson,
                                                                     decisionContextJson,
                                                                     visionContextJson);
        });

        if (workerThread == nullptr) {
            setRetryUploadInProgress(false);
            emit completedInspectionBundleUploaded(false,
                                                   QStringLiteral("完整上传失败：无法创建后台上传线程"),
                                                   cloudResult);
            return false;
        }

        connect(workerThread, &QThread::finished, this, [this, workerResult, historyRow, cloudResult]() {
            QString resultText = *workerResult;
            const QString recordId = parseTokenValue(resultText, QStringLiteral("record_id"));
            const QString recordNo = parseTokenValue(resultText, QStringLiteral("record_no"));
            const QString compactStatus = compactUploadStatus(resultText);
            QString historyError;
            bool historyUpdated = true;

            if (m_historyModel != nullptr && historyRow >= 0) {
                historyUpdated = m_historyModel->updateRecordUploadResult(historyRow,
                                                                          compactStatus,
                                                                          recordId,
                                                                          recordNo,
                                                                          &historyError);
            }

            if (!historyUpdated) {
                resultText += QStringLiteral(" history_update_error=") + historyError;
                qWarning() << "completed inspection upload history update failed" << historyError;
            }

            setRetryUploadInProgress(false);
            emit completedInspectionBundleUploaded(isUploadStatusSuccess(*workerResult),
                                                   resultText,
                                                   cloudResult);
        }, Qt::QueuedConnection);

        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);

        workerThread->start();
        return true;
    }

signals:
    /* saveInProgressChanged 在后台保存开始或结束时通知 QML 刷新按钮状态。 */
    void saveInProgressChanged();

    /* detectInProgressChanged 在后台检测开始或结束时通知 QML 刷新按钮状态。 */
    void detectInProgressChanged();

    /* retryUploadInProgressChanged 在历史图片重新发送开始或结束时通知 QML 刷新按钮状态。 */
    void retryUploadInProgressChanged();

    /* saveCurrentFrameFinished 在异步保存任务结束后发送完整中文结果，QML 用它更新提示条。 */
    void saveCurrentFrameFinished(const QString &resultText);

    /* detectClassificationReady 在第一个分类模型结束后立即发送 RESULT，QML 用它提前显示零件和类别。 */
    void detectClassificationReady(const QString &resultText);

    /* detectModelsReady 在分类和 UNet 都结束后立即发送 RESULT，QML 用它提前显示双模型总耗时。 */
    void detectModelsReady(const QString &resultText);

    /* detectCurrentFrameFinished 在异步检测任务结束后发送 RESULT 或错误文本，QML 用它更新当前结果。 */
    void detectCurrentFrameFinished(const QString &resultText);

    /* retryUploadFinished 在历史图片重新发送结束后发送 row 和结果文本，QML 用它刷新当前详情页。 */
    void retryUploadFinished(int row, const QString &resultText);

    /* completedInspectionBundleUploaded 在自动流程完整上传结束后通知 QML，再由 QML 下发 FINAL_SORT_RESULT。 */
    void completedInspectionBundleUploaded(bool ok, const QString &resultText, const QString &cloudResult);

private:
    /*
     * SavedImagePair 的作用：
     *   保存 overlay 一次 SAVE_DUAL 请求返回的两种本地图片路径。
     *
     * 字段说明：
     *   jpgPath 是 JPG 原图路径，后续按云端 file_kind=source 上传。
     *   pngPath 是 PNG 结果图路径，后续按云端 file_kind=annotated 上传。
     */
    struct SavedImagePair
    {
        QString jpgPath;
        QString pngPath;
    };

    /*
     * DetectResultBundle 的作用：
     *   保存一次点击“检测”后两个模型串行输出的全部本地结果。
     *
     * 字段说明：
     *   sourcePath 是 overlay 保存的当前帧 JPG，MobileNetV3-Small 和 UNet 都基于它推理。
     *   annotatedPaths 保存 UNet raw/overlay/mask 等检测结果图，云端统一通过 --annotated 上传。
     *   annotatedLabels 保存每张结果图在历史页图片轮播上的显示名称。
     *   classificationResult 是 defect-classify 输出的 RESULT 行。
     *   segmentationResult 是 defect-segment 输出的 RESULT_SEG 行。
     *   uploadResult 是 defect-cos-upload 返回的上传状态。
     *   settings 是本次检测开始时复制的真实参数配置。
     */
    struct DetectResultBundle
    {
        QString sourcePath;
        QStringList annotatedPaths;
        QStringList annotatedLabels;
        QString classificationResult;
        QString segmentationResult;
        QString uploadResult;
        DetectSettingsSnapshot settings;
    };

    /*
     * DetectProgressCallback 的作用：
     *   让同步检测流程在关键阶段向异步 UI 汇报进度。
     *
     * 参数：
     *   QString 是该阶段可被 QML 复用解析的 RESULT 文本。
     *
     * 返回值：
     *   无返回值；同步 SSH 自检路径传空回调即可保持原行为。
     */
    using DetectProgressCallback = std::function<void(const QString &)>;

    /*
     * firstUsefulLine 的作用：
     *   从脚本多行输出中提取适合放到界面状态栏的一行。
     */
    QString firstUsefulLine(const QString &text) const
    {
        const QStringList lines = text.split(QLatin1Char('\n'), QString::SkipEmptyParts);

        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();

            if (!trimmed.isEmpty() && !trimmed.startsWith(QStringLiteral("===="))) {
                return trimmed;
            }
        }

        return text.left(80);
    }

    /*
     * firstLineWithPrefix 的作用：
     *   从脚本多行输出中优先提取指定前缀的业务结果行。
     *
     * 主要流程：
     *   1. 按行遍历 stdout/stderr。
     *   2. 返回第一条以 prefix 开头的非空行。
     *   3. 找不到时返回空字符串，调用方再回退到 firstUsefulLine()。
     *
     * 参数：
     *   text 是脚本输出。
     *   prefix 是要查找的中文业务前缀，例如“上传失败：”。
     *
     * 返回值：
     *   找到匹配行时返回该行；否则返回空字符串。
     */
    QString firstLineWithPrefix(const QString &text, const QString &prefix) const
    {
        const QStringList lines = text.split(QLatin1Char('\n'), QString::SkipEmptyParts);

        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();

            if (trimmed.startsWith(prefix)) {
                return trimmed;
            }
        }

        return QString();
    }

    /*
     * isMountPointMounted 的作用：
     *   用 POSIX 文件接口读取 /proc/mounts，确认保存目标是真实 SD 卡挂载点。
     *
     * 主要流程：
     *   1. 打开 /proc/mounts，这个文件由内核动态生成，不能依赖普通文件大小。
     *   2. 逐行解析设备名和挂载点字段，只要挂载点等于 /mnt/sdcard 就认为 SD 卡在线。
     *   3. 没找到时返回明确错误，让界面提示和 SSH 自检都能定位到挂载问题。
     *
     * 关键说明：
     *   这里不用 QTextStream::atEnd()，因为 procfs 文件大小经常显示为 0，
     *   在板端会导致 Qt 侧误判“未挂载”，而 shell/overlay 进程都能看到真实挂载。
     */
    bool isMountPointMounted(const QString &mountPoint, QString *errorText) const
    {
        FILE *mounts = std::fopen("/proc/mounts", "r");
        QByteArray expectedMount = mountPoint.toLocal8Bit();
        char device[256];
        char path[4096];

        if (mounts == nullptr) {
            if (errorText) {
                *errorText = QStringLiteral("无法读取 /proc/mounts：")
                    + QString::fromLocal8Bit(strerror(errno));
            }
            return false;
        }

        while (std::fscanf(mounts, "%255s %4095s %*s %*s %*d %*d\n", device, path) == 2) {
            if (std::strcmp(path, expectedMount.constData()) == 0) {
                std::fclose(mounts);
                return true;
            }
        }

        std::fclose(mounts);

        if (errorText) {
            *errorText = mountPoint + QStringLiteral(" 未挂载");
        }
        return false;
    }

    /*
     * sanitizeLogFileToken 的作用：
     *   把 QML 传入的告警来源转换成安全文件名片段，防止斜杠、空格或中文标点破坏路径结构。
     *
     * 主要流程：
     *   逐字符保留英文字母、数字、横线和下划线，其它字符统一替换成下划线；
     *   如果清洗后为空，使用 fallback 兜底，保证日志文件名始终可预测。
     *
     * 参数：
     *   token 是原始来源标识。
     *   fallback 是 token 为空或全非法时使用的默认片段。
     *
     * 返回值：
     *   返回只包含 [A-Za-z0-9_-] 的文件名片段。
     */
    QString sanitizeLogFileToken(const QString &token, const QString &fallback) const
    {
        QString sanitized;

        for (const QChar &ch : token) {
            const ushort code = ch.unicode();
            if ((code >= 'a' && code <= 'z')
                    || (code >= 'A' && code <= 'Z')
                    || (code >= '0' && code <= '9')
                    || code == '-'
                    || code == '_') {
                sanitized.append(ch);
            } else {
                sanitized.append(QLatin1Char('_'));
            }
        }

        sanitized = sanitized.trimmed();
        while (sanitized.contains(QStringLiteral("__"))) {
            sanitized.replace(QStringLiteral("__"), QStringLiteral("_"));
        }

        if (sanitized.isEmpty()) {
            return fallback;
        }
        return sanitized;
    }

    /*
     * dailyLogFilePath 的作用：
     *   为告警日志和诊断快照生成当天文件路径。
     *
     * 主要流程：
     *   1. 使用当前板端本地日期，格式化成 YYYYMMDD。
     *   2. 拼出 `<prefix>_YYYYMMDD<suffix>` 文件名。
     *   3. 返回位于 /mnt/sdcard/logs 下的完整文件路径。
     *
     * 参数：
     *   prefix 是文件名前缀，例如 qt_alarm_snapshot 或 qt_alarm。
     *   suffix 是扩展名，例如 .txt 或 .log。
     *
     * 返回值：
     *   返回可直接交给 QFile 打开的当天完整路径。
     */
    QString dailyLogFilePath(const QString &prefix,
                             const QString &suffix) const
    {
        const QString safePrefix = sanitizeLogFileToken(prefix, QStringLiteral("qt_alarm"));
        const QString fileName = safePrefix
            + QLatin1Char('_')
            + currentDateStampString()
            + suffix;

        return QDir(m_logDir).filePath(fileName);
    }

    /*
     * appendTextFileWithFsync 的作用：
     *   把 UTF-8 文本可靠追加到当天日志文件，并在成功返回前完成 flush 和 fsync。
     *
     * 主要流程：
     *   1. 使用 WriteOnly|Append 打开文件，不存在时自动创建当天文件。
     *   2. 如果文件已有内容，先写一个空行和分隔线，让同一天多段日志容易阅读。
     *   3. 写入本次记录头和正文，必要时补一个换行。
     *   4. 先 flush Qt 缓冲，再 fsync 文件描述符，最后关闭文件。
     *
     * 参数：
     *   filePath 是目标完整路径。
     *   actionName 是动作名，例如 alarm-snapshot 或 alarm-log，会写入分隔头。
     *   sourceKey 是告警来源，诊断快照可为空。
     *   text 是要写入的 UTF-8 文本。
     *   errorText 用于带出中文失败原因。
     *
     * 返回值：
     *   true 表示文件追加、flush 和 fsync 都成功；false 表示失败，errorText 保存原因。
     */
    bool appendTextFileWithFsync(const QString &filePath,
                                 const QString &actionName,
                                 const QString &sourceKey,
                                 const QString &text,
                                 QString *errorText) const
    {
        QFile file(filePath);
        const bool hadContent = QFileInfo::exists(filePath) && QFileInfo(filePath).size() > 0;

        if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            if (errorText) {
                *errorText = QStringLiteral("无法打开 ")
                    + filePath
                    + QStringLiteral("：")
                    + file.errorString();
            }
            return false;
        }

        QTextStream stream(&file);
        stream.setCodec("UTF-8");

        if (hadContent) {
            stream << '\n';
        }
        stream << QStringLiteral("========== ")
               << actionName
               << QStringLiteral(" ")
               << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
               << QStringLiteral(" ==========\n");

        if (!sourceKey.trimmed().isEmpty()) {
            stream << QStringLiteral("log_source=")
                   << sanitizeLogFileToken(sourceKey, QStringLiteral("runtime"))
                   << '\n';
        }

        stream << text;
        if (!text.endsWith(QLatin1Char('\n'))) {
            stream << '\n';
        }
        stream.flush();

        if (stream.status() != QTextStream::Ok) {
            if (errorText) {
                *errorText = QStringLiteral("写入文本流失败");
            }
            file.close();
            return false;
        }

        if (!file.flush()) {
            if (errorText) {
                *errorText = QStringLiteral("flush 失败：") + file.errorString();
            }
            file.close();
            return false;
        }

        if (::fsync(file.handle()) != 0) {
            if (errorText) {
                *errorText = QStringLiteral("fsync 失败：")
                    + QString::fromLocal8Bit(strerror(errno));
            }
            file.close();
            return false;
        }

        file.close();
        return true;
    }

    /*
     * saveAlarmTextToSdCard 的作用：
     *   为“保存诊断”和“自动告警日志”提供同一条可靠落盘控制路径。
     *
     * 主要流程：
     *   1. 先确认 /mnt/sdcard 已挂载，防止 SD 卡异常时误写 rootfs。
     *   2. 创建 /mnt/sdcard/logs 并检查目录可写，失败时返回明确中文原因。
     *   3. 生成当天文件名并调用 appendTextFileWithFsync() 追加文本。
     *
     * 参数：
     *   actionName 是日志里的动作名，例如 alarm-snapshot 或 alarm-log。
     *   successLabel 是返回给 QML 的中文对象名，例如“诊断”或“告警日志”。
     *   filePrefix 是文件名前缀。
     *   sourceKey 是可选来源标识。
     *   suffix 是扩展名。
     *   text 是要写入的文本内容。
     *
     * 返回值：
     *   成功返回“<对象>已保存：<路径>”；失败返回“<对象>保存失败：<原因>”。
     */
    QString saveAlarmTextToSdCard(const QString &actionName,
                                  const QString &successLabel,
                                  const QString &filePrefix,
                                  const QString &sourceKey,
                                  const QString &suffix,
                                  const QString &text)
    {
        QString mountError;
        QString writeError;
        QString result;

        qInfo() << "storage action" << actionName << "requested"
                << "mount" << m_mountPoint
                << "logDir" << m_logDir
                << "source" << sourceKey;
        if (actionName == QStringLiteral("alarm-snapshot")) {
            qInfo() << "storage action alarm-snapshot marker";
        } else if (actionName == QStringLiteral("alarm-log")) {
            qInfo() << "storage action alarm-log marker";
        }

        if (!isMountPointMounted(m_mountPoint, &mountError)) {
            result = successLabel + QStringLiteral("保存失败：") + mountError;
            qWarning() << "storage action" << actionName << "result" << result;
            return result;
        }

        if (!QDir().mkpath(m_logDir)) {
            result = successLabel + QStringLiteral("保存失败：无法创建 ") + m_logDir;
            qWarning() << "storage action" << actionName << "result" << result;
            return result;
        }

        const QFileInfo logDirInfo(m_logDir);
        if (!logDirInfo.isDir() || !logDirInfo.isWritable()) {
            result = successLabel + QStringLiteral("保存失败：日志目录不可写 ") + m_logDir;
            qWarning() << "storage action" << actionName << "result" << result;
            return result;
        }

        const QString filePath = dailyLogFilePath(filePrefix, suffix);
        if (!appendTextFileWithFsync(filePath, actionName, sourceKey, text, &writeError)) {
            result = successLabel + QStringLiteral("保存失败：") + writeError;
            qWarning() << "storage action" << actionName << "result" << result;
            return result;
        }

        result = successLabel + QStringLiteral("已保存：") + filePath;
        qInfo() << "storage action" << actionName << "result" << result;
        return result;
    }

    /*
     * writeAllToFd 的作用：
     *   向 Unix socket 完整发送命令文本，处理短写和 EINTR。
     */
    bool writeAllToFd(int fd, const QByteArray &payload, QString *errorText) const
    {
        const char *cursor = payload.constData();
        qint64 remaining = payload.size();

        while (remaining > 0) {
            const ssize_t written = ::write(fd, cursor, static_cast<size_t>(remaining));

            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errorText) {
                    *errorText = QString::fromLocal8Bit(strerror(errno));
                }
                return false;
            }

            if (written == 0) {
                if (errorText) {
                    *errorText = QStringLiteral("socket 写入返回 0");
                }
                return false;
            }

            cursor += written;
            remaining -= written;
        }

        return true;
    }

    /*
     * readReplyFromFd 的作用：
     *   读取 overlay 返回的一行 OK/ERR 结果。
     */
    QString readReplyFromFd(int fd, QString *errorText) const
    {
        QByteArray reply;
        char buffer[256];

        while (true) {
            const ssize_t nread = ::read(fd, buffer, sizeof(buffer));

            if (nread < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errorText) {
                    *errorText = QString::fromLocal8Bit(strerror(errno));
                }
                return QString();
            }

            if (nread == 0) {
                break;
            }

            reply.append(buffer, static_cast<int>(nread));
            if (reply.contains('\n')) {
                break;
            }
        }

        return QString::fromUtf8(reply).trimmed();
    }

    /*
     * parseDualSaveReply 的作用：
     *   解析 overlay 返回的 "OK JPG <path> PNG <path>" 双格式保存结果。
     *
     * 参数：
     *   reply 是 overlay 返回的完整一行文本。
     *   pair 用于返回 JPG/PNG 路径。
     *   errorText 用于返回解析失败原因。
     *
     * 返回值：
     *   解析成功返回 true；格式错误或路径缺失返回 false。
     */
    bool parseDualSaveReply(const QString &reply, SavedImagePair *pair, QString *errorText) const
    {
        const QString markerJpg = QStringLiteral("OK JPG ");
        const QString markerPng = QStringLiteral(" PNG ");
        const int pngMarkerIndex = reply.indexOf(markerPng);

        if (!reply.startsWith(markerJpg) || pngMarkerIndex <= markerJpg.length()) {
            if (errorText) {
                *errorText = QStringLiteral("overlay 返回格式不是双格式图片路径");
            }
            return false;
        }

        pair->jpgPath = reply.mid(markerJpg.length(), pngMarkerIndex - markerJpg.length()).trimmed();
        pair->pngPath = reply.mid(pngMarkerIndex + markerPng.length()).trimmed();

        if (pair->jpgPath.isEmpty() || pair->pngPath.isEmpty()) {
            if (errorText) {
                *errorText = QStringLiteral("overlay 返回的 JPG/PNG 路径为空");
            }
            return false;
        }

        return true;
    }

    /*
     * parseDetectSaveReply 的作用：
     *   解析 overlay 返回的 "OK DETECT_JPG <path>" 检测临时图片路径。
     *
     * 参数：
     *   reply 是 overlay 返回的一行文本。
     *   imagePath 用于返回当前帧 JPG 路径。
     *   errorText 用于返回解析失败原因。
     *
     * 返回值：
     *   解析成功返回 true；格式错误或路径为空返回 false。
     */
    bool parseDetectSaveReply(const QString &reply, QString *imagePath, QString *errorText) const
    {
        const QString marker = QStringLiteral("OK DETECT_JPG ");

        if (!reply.startsWith(marker)) {
            if (errorText) {
                *errorText = QStringLiteral("overlay 返回格式不是检测图片路径");
            }
            return false;
        }

        if (imagePath) {
            *imagePath = reply.mid(marker.length()).trimmed();
        }

        if (imagePath == nullptr || imagePath->isEmpty()) {
            if (errorText) {
                *errorText = QStringLiteral("overlay 返回的检测图片路径为空");
            }
            return false;
        }

        return true;
    }

    /*
     * ensureCurrentFrameHasLocateTarget 的作用：
     *   在真正保存图片和运行模型前，先让 overlay 对当前原始帧执行一次 `LOCATE`，
     *   确认绿色中心 ROI 附近确实存在零件，避免空皮带、空 ROI 或旧画面直接进入分类模型。
     *
     * 主要流程：
     *   1. 通过 overlay 控制 socket 发送 `LOCATE`，复用底层找零件算法，不新增另一套图像判断。
     *   2. 校验回复必须是 `OK LOCATE ...`；socket 错误或 overlay 异常直接阻断检测。
     *   3. 读取 `has_target` 和 `ring` 字段，只有已找到候选且候选带中心孔结构时才允许后续 `SAVE_DETECT` 和双模型推理。
     *   4. 目标不存在或候选没有中心孔时把 `bbox/confidence/diag/cand_*` 等诊断字段拼进错误文本，
     *      现场可以直接判断是空皮带、候选过小、无中心孔还是搜索带位置不对。
     *
     * 参数：
     *   errorText 用于返回给 QML 的中文失败原因；调用方会统一加上“检测失败：”前缀。
     *
     * 返回值：
     *   true 表示当前帧已经由 `LOCATE has_target=1 ring=1` 确认存在垫圈类零件；
     *   false 表示当前帧不允许进入模型检测，errorText 内保存阻断原因。
     */
    bool ensureCurrentFrameHasLocateTarget(QString *errorText)
    {
        const QString reply = sendRawOverlayCommand(QStringLiteral("LOCATE"));

        if (reply.isEmpty()) {
            if (errorText) {
                *errorText = QStringLiteral("检测入口 LOCATE 无回复，已取消模型检测");
            }
            return false;
        }

        if (reply.startsWith(QStringLiteral("ERR "))) {
            if (errorText) {
                *errorText = QStringLiteral("检测入口 LOCATE 失败：") + reply.mid(4);
            }
            return false;
        }

        if (!reply.startsWith(QStringLiteral("OK LOCATE "))) {
            if (errorText) {
                *errorText = QStringLiteral("检测入口 LOCATE 回复格式异常：") + reply.left(160);
            }
            return false;
        }

        /* hasTargetText 表示 overlay 是否找到了候选目标；只有 1 才说明可以继续检查候选细节。 */
        const QString hasTargetText = parseTokenValue(reply, QStringLiteral("has_target"));

        /* ringText 表示候选是否具备垫圈中心孔结构；黑色传送带误报通常是 has_target=1 但 ring=0。 */
        const QString ringText = parseTokenValue(reply, QStringLiteral("ring"));

        /* bboxWText/bboxHText 保存候选外框尺寸，现场可用它判断是否抓到了皮带亮边或支架。 */
        const QString bboxWText = parseTokenValue(reply, QStringLiteral("bbox_w"));
        const QString bboxHText = parseTokenValue(reply, QStringLiteral("bbox_h"));

        /* confidenceText 保存 overlay 对当前候选的置信度；黑带反光可能出现高置信但无中心孔。 */
        const QString confidenceText = parseTokenValue(reply, QStringLiteral("confidence"));

        /* 以下诊断字段来自最接近但可能被拒绝的候选，用于定位搜索带、尺寸、密度或 ring 门槛问题。 */
        const QString diagText = parseTokenValue(reply, QStringLiteral("diag"));
        const QString roiYText = parseTokenValue(reply, QStringLiteral("roi_y"));
        const QString roiHText = parseTokenValue(reply, QStringLiteral("roi_h"));
        const QString candBoxText = parseTokenValue(reply, QStringLiteral("cand_box"));
        const QString candConfText = parseTokenValue(reply, QStringLiteral("cand_conf"));
        const QString candRingText = parseTokenValue(reply, QStringLiteral("cand_ring"));
        /*
         * kDetectLocateMinRingBboxSide/kDetectLocateMinRingBboxArea 是模型入口的最后一道尺寸兜底。
         * 真实垫圈现场完整入 ROI 时 bbox 约 173x173；黑色传送带小突起即使偶发被判 ring=1，
         * 只要尺寸明显不够，就不能继续保存图片和运行双模型。
         */
        const int kDetectLocateMinRingBboxSide = 45;
        const int kDetectLocateMinRingBboxArea = 2000;
        bool bboxWOk = false;
        bool bboxHOk = false;
        const int bboxW = bboxWText.toInt(&bboxWOk);
        const int bboxH = bboxHText.toInt(&bboxHOk);

        /* has_target 不为 1 表示当前帧没有可用候选，直接阻断模型入口，防止空 ROI 被分类模型硬分成某个类别。 */
        if (hasTargetText != QStringLiteral("1")) {
            QString detail = QStringLiteral("当前 ROI 未识别到零件，请放入零件后再检测");

            if (!diagText.isEmpty()) {
                detail += QStringLiteral("；diag=") + diagText;
            }
            if (!roiYText.isEmpty() || !roiHText.isEmpty()) {
                detail += QStringLiteral(" roi=") + roiYText + QStringLiteral("/") + roiHText;
            }
            if (!candBoxText.isEmpty()) {
                detail += QStringLiteral(" cand_box=") + candBoxText;
            }
            if (!candConfText.isEmpty()) {
                detail += QStringLiteral(" cand_conf=") + candConfText;
            }
            if (!candRingText.isEmpty()) {
                detail += QStringLiteral(" cand_ring=") + candRingText;
            }

            if (errorText) {
                *errorText = detail;
            }
            return false;
        }

        /* has_target=1 但 ring!=1 是本次现场黑色传送带误报的关键形态，不能再放行到模型检测。 */
        if (ringText != QStringLiteral("1")) {
            QString detail = QStringLiteral("当前 ROI 候选没有中心孔结构，疑似黑色传送带反光，已取消模型检测");

            if (!ringText.isEmpty()) {
                detail += QStringLiteral("；ring=") + ringText;
            }
            if (!confidenceText.isEmpty()) {
                detail += QStringLiteral(" confidence=") + confidenceText;
            }
            if (!bboxWText.isEmpty() || !bboxHText.isEmpty()) {
                detail += QStringLiteral(" bbox=") + bboxWText + QLatin1Char('x') + bboxHText;
            }
            if (!diagText.isEmpty()) {
                detail += QStringLiteral(" diag=") + diagText;
            }
            if (!roiYText.isEmpty() || !roiHText.isEmpty()) {
                detail += QStringLiteral(" roi=") + roiYText + QStringLiteral("/") + roiHText;
            }
            if (!candBoxText.isEmpty()) {
                detail += QStringLiteral(" cand_box=") + candBoxText;
            }
            if (!candConfText.isEmpty()) {
                detail += QStringLiteral(" cand_conf=") + candConfText;
            }
            if (!candRingText.isEmpty()) {
                detail += QStringLiteral(" cand_ring=") + candRingText;
            }

            qInfo() << "detect entry LOCATE rejected no-ring candidate"
                    << "frame" << parseTokenValue(reply, QStringLiteral("frame_id"))
                    << "bbox" << (bboxWText + QLatin1Char('x') + bboxHText)
                    << "confidence" << confidenceText
                    << "ring" << ringText
                    << "diag" << diagText
                    << "cand_box" << candBoxText
                    << "cand_conf" << candConfText
                    << "cand_ring" << candRingText;

            if (errorText) {
                *errorText = detail;
            }
            return false;
        }

        /*
         * ring=1 只能证明“像中心孔”，不能证明候选尺寸像真实零件。
         * 空转时黑色传送带突起可能靠亮边和暗心形成假 ring，因此模型入口继续做最小 bbox 兜底。
         */
        if (bboxWOk && bboxHOk &&
            (bboxW < kDetectLocateMinRingBboxSide ||
             bboxH < kDetectLocateMinRingBboxSide ||
             bboxW * bboxH < kDetectLocateMinRingBboxArea)) {
            QString detail = QStringLiteral("当前 ROI 候选尺寸过小，疑似黑色传送带突起，已取消模型检测");

            detail += QStringLiteral("；ring=") + ringText;
            detail += QStringLiteral(" confidence=") + confidenceText;
            detail += QStringLiteral(" bbox=") + bboxWText + QLatin1Char('x') + bboxHText;
            if (!diagText.isEmpty()) {
                detail += QStringLiteral(" diag=") + diagText;
            }
            if (!candBoxText.isEmpty()) {
                detail += QStringLiteral(" cand_box=") + candBoxText;
            }
            if (!candConfText.isEmpty()) {
                detail += QStringLiteral(" cand_conf=") + candConfText;
            }
            if (!candRingText.isEmpty()) {
                detail += QStringLiteral(" cand_ring=") + candRingText;
            }

            qInfo() << "detect entry LOCATE rejected small ring candidate"
                    << "frame" << parseTokenValue(reply, QStringLiteral("frame_id"))
                    << "bbox" << (bboxWText + QLatin1Char('x') + bboxHText)
                    << "confidence" << confidenceText
                    << "ring" << ringText
                    << "diag" << diagText
                    << "cand_box" << candBoxText
                    << "cand_conf" << candConfText
                    << "cand_ring" << candRingText;

            if (errorText) {
                *errorText = detail;
            }
            return false;
        }

        qInfo() << "detect entry LOCATE accepted"
                << "frame" << parseTokenValue(reply, QStringLiteral("frame_id"))
                << "bbox" << (bboxWText + QLatin1Char('x') + bboxHText)
                << "confidence" << confidenceText
                << "ring" << ringText;
        return true;
    }

    /*
     * parseTokenValue 的作用：
     *   从脚本返回行中提取 `key=value` 形式的短字段。
     *
     * 参数：
     *   text 是上传脚本 stdout/stderr 中的一行或多行文本。
     *   key 是要查找的字段名，例如 record_id 或 record_no。
     *
     * 返回值：
     *   找到时返回 value；找不到时返回空字符串。
     */
    QString parseTokenValue(const QString &text, const QString &key) const
    {
        const QString marker = key + QLatin1Char('=');
        const int markerIndex = text.indexOf(marker);

        if (markerIndex < 0) {
            return QString();
        }

        const int valueStart = markerIndex + marker.length();
        int valueEnd = valueStart;

        while (valueEnd < text.length()
               && !text.at(valueEnd).isSpace()
               && text.at(valueEnd) != QLatin1Char(';')) {
            valueEnd++;
        }

        return text.mid(valueStart, valueEnd - valueStart).trimmed();
    }

    /*
     * buildDetectModelResultLine 的作用：
     *   把分类 RESULT 和 UNet RESULT_SEG 合成 QML 首页可解析的一行双模型结果。
     *
     * 主要流程：
     *   1. 保留分类 RESULT 原有字段，继续让 QML 读取 status/class/confidence/good_total/bad_total。
     *   2. 追加 UNet 状态、缺陷像素、分割耗时和双模型总耗时。
     *   3. 追加综合判定字段，让首页、历史和云端看到同一个最终结论。
     *   4. 追加 source_path，方便最终结果和日志仍能对齐本次检测原图。
     *
     * 参数：
     *   classificationResult 是 defect-classify 输出的 RESULT 行。
     *   segmentationResult 是 defect-segment 输出的 RESULT_SEG 行。
     *   fusedResult 是分类和 UNet 分割的综合判定。
     *   totalModelTimeMs 是分类和 UNet 两个模型串行耗时，单位毫秒。
     *   sourcePath 是 overlay 保存的本次检测原图。
     *
     * 返回值：
     *   返回以 RESULT 开头的一行文本；不包含 upload_status，表示上传尚未完成。
     */
    QString buildDetectModelResultLine(const QString &classificationResult,
                                       const QString &segmentationResult,
                                       const FusedDetectResult &fusedResult,
                                       qint64 totalModelTimeMs,
                                       const QString &sourcePath) const
    {
        return classificationResult
            + QStringLiteral(" segment_status=")
            + parseTokenValue(segmentationResult, QStringLiteral("status"))
            + QStringLiteral(" defect_pixels=")
            + parseTokenValue(segmentationResult, QStringLiteral("defect_pixels"))
            + QStringLiteral(" segment_evidence=")
            + parseTokenValue(segmentationResult, QStringLiteral("evidence"))
            + QStringLiteral(" filtered_defect_pixels=")
            + parseTokenValue(segmentationResult, QStringLiteral("filtered_defect_pixels"))
            + QStringLiteral(" largest_component_pixels=")
            + parseTokenValue(segmentationResult, QStringLiteral("largest_component_pixels"))
            + QStringLiteral(" component_count=")
            + parseTokenValue(segmentationResult, QStringLiteral("component_count"))
            + QStringLiteral(" retained_component_count=")
            + parseTokenValue(segmentationResult, QStringLiteral("retained_component_count"))
            + QStringLiteral(" segment_time_ms=")
            + parseTokenValue(segmentationResult, QStringLiteral("time_ms"))
            + QStringLiteral(" total_time_ms=")
            + QString::number(totalModelTimeMs)
            + QStringLiteral(" fused_status=")
            + uiStatusFromFusedResult(fusedResult)
            + QStringLiteral(" fused_result=")
            + cloudResultFromFusedResult(fusedResult)
            + QStringLiteral(" fused_reason=")
            + fusedResult.reason
            + QStringLiteral(" source_path=")
            + sourcePath;
    }

    /*
     * buildVisionContextJson 的作用：
     *   把最近一次本地模型检测结果转换成云端 `vision_context` JSON。
     *
     * 主要流程：
     *   1. 保存 source/annotated 本地路径，便于云端记录和板端 SD 卡文件对应。
     *   2. 保存 MobileNetV3-Small 的 top1 标签、状态和置信度。
     *   3. 保存 UNet 的缺陷像素、阈值、结果图和推理耗时。
     *
     * 参数：
     *   bundle 是 detectCurrentFrameOnce() 产出的本地检测结果集合。
     *
     * 返回值：
     *   返回压缩 JSON 字符串；字段缺失时写空字符串或 0，避免上传脚本解析失败。
     */
    QString buildVisionContextJson(const DetectResultBundle &bundle) const
    {
        QJsonArray annotatedFiles;
        for (int i = 0; i < bundle.annotatedPaths.size(); ++i) {
            QJsonObject image;

            image.insert(QStringLiteral("path"), bundle.annotatedPaths.at(i));
            image.insert(QStringLiteral("label"),
                         i < bundle.annotatedLabels.size()
                         ? bundle.annotatedLabels.at(i)
                         : QStringLiteral("annotated"));
            annotatedFiles.append(image);
        }

        QJsonObject mobileNet;
        mobileNet.insert(QStringLiteral("model_name"), QStringLiteral("MobileNetV3-Small"));
        mobileNet.insert(QStringLiteral("status"), parseTokenValue(bundle.classificationResult, QStringLiteral("status")));
        mobileNet.insert(QStringLiteral("top1_label"), parseTokenValue(bundle.classificationResult, QStringLiteral("class")));
        mobileNet.insert(QStringLiteral("top1_score"), parseTokenValue(bundle.classificationResult, QStringLiteral("confidence")).toDouble());
        mobileNet.insert(QStringLiteral("time_ms"), parseTokenValue(bundle.classificationResult, QStringLiteral("time_ms")).toInt());
        mobileNet.insert(QStringLiteral("raw_result"), bundle.classificationResult);

        QJsonObject unet;
        unet.insert(QStringLiteral("model_name"), QStringLiteral("UNet"));
        unet.insert(QStringLiteral("status"), parseTokenValue(bundle.segmentationResult, QStringLiteral("status")));
        unet.insert(QStringLiteral("evidence"), parseTokenValue(bundle.segmentationResult, QStringLiteral("evidence")));
        unet.insert(QStringLiteral("threshold"), 0.5);
        unet.insert(QStringLiteral("min_component_pixels"), bundle.settings.segmentMinComponentPixels);
        unet.insert(QStringLiteral("review_defect_pixels"), bundle.settings.segmentReviewPixels);
        unet.insert(QStringLiteral("bad_defect_pixels"), bundle.settings.segmentBadPixels);
        unet.insert(QStringLiteral("strong_component_pixels"), bundle.settings.segmentStrongComponentPixels);
        unet.insert(QStringLiteral("defect_pixels"), parseTokenValue(bundle.segmentationResult, QStringLiteral("defect_pixels")).toInt());
        unet.insert(QStringLiteral("filtered_defect_pixels"), parseTokenValue(bundle.segmentationResult, QStringLiteral("filtered_defect_pixels")).toInt());
        unet.insert(QStringLiteral("largest_component_pixels"), parseTokenValue(bundle.segmentationResult, QStringLiteral("largest_component_pixels")).toInt());
        unet.insert(QStringLiteral("component_count"), parseTokenValue(bundle.segmentationResult, QStringLiteral("component_count")).toInt());
        unet.insert(QStringLiteral("retained_component_count"), parseTokenValue(bundle.segmentationResult, QStringLiteral("retained_component_count")).toInt());
        unet.insert(QStringLiteral("time_ms"), parseTokenValue(bundle.segmentationResult, QStringLiteral("time_ms")).toInt());
        unet.insert(QStringLiteral("raw_path"), parseTokenValue(bundle.segmentationResult, QStringLiteral("raw_path")));
        unet.insert(QStringLiteral("overlay_path"), parseTokenValue(bundle.segmentationResult, QStringLiteral("overlay_path")));
        unet.insert(QStringLiteral("mask_path"), parseTokenValue(bundle.segmentationResult, QStringLiteral("mask_path")));
        unet.insert(QStringLiteral("raw_result"), bundle.segmentationResult);

        QJsonObject roi;
        roi.insert(QStringLiteral("size_px"), bundle.settings.roiSize);
        roi.insert(QStringLiteral("source"), QStringLiteral("center_crop"));

        QJsonObject context;
        context.insert(QStringLiteral("source_path"), bundle.sourcePath);
        context.insert(QStringLiteral("annotated_files"), annotatedFiles);
        context.insert(QStringLiteral("roi"), roi);
        context.insert(QStringLiteral("mobilenetv3_small"), mobileNet);
        context.insert(QStringLiteral("unet"), unet);
        return compactJsonString(context);
    }

    /*
     * buildDecisionContextJson 的作用：
     *   把分类、UNet 和参数阈值转换成云端 `decision_context` JSON。
     *
     * 主要流程：
     *   1. 复用 fusedResultFromModelResults() 保证 UI、历史和云端最终结果一致。
     *   2. 记录分类阈值、复核阈值、UNet 四个证据阈值和模型耗时。
     *   3. 明确本次上传发生在 F4 称重/电感完成之后，便于云端追溯自动流程时序。
     *
     * 参数：
     *   bundle 是本次检测的本地缓存。
     *
     * 返回值：
     *   返回压缩 JSON 字符串。
     */
    QString buildDecisionContextJson(const DetectResultBundle &bundle) const
    {
        const FusedDetectResult fusedResult =
            fusedResultFromModelResults(bundle.classificationResult,
                                        bundle.segmentationResult,
                                        bundle.settings);
        const int classifyMs = parseTokenValue(bundle.classificationResult, QStringLiteral("time_ms")).toInt();
        const int segmentMs = parseTokenValue(bundle.segmentationResult, QStringLiteral("time_ms")).toInt();

        QJsonObject context;
        context.insert(QStringLiteral("pipeline"), QStringLiteral("UNet + MobileNetV3-Small"));
        context.insert(QStringLiteral("decision_rule"), QStringLiteral("strong UNet or confident bad classification => bad; weak UNet or incomplete confidence => review; confident good plus clear UNet => good"));
        context.insert(QStringLiteral("result"), cloudResultFromFusedResult(fusedResult));
        context.insert(QStringLiteral("ui_status"), uiStatusFromFusedResult(fusedResult));
        context.insert(QStringLiteral("need_ai_review"), cloudResultFromFusedResult(fusedResult) == QStringLiteral("review"));
        context.insert(QStringLiteral("decision_reason"), fusedResult.reason);
        context.insert(QStringLiteral("classification_threshold"), bundle.settings.modelThreshold);
        context.insert(QStringLiteral("classification_review_threshold"), bundle.settings.reviewThreshold);
        context.insert(QStringLiteral("unet_threshold"), 0.5);
        context.insert(QStringLiteral("unet_min_component_pixels"), bundle.settings.segmentMinComponentPixels);
        context.insert(QStringLiteral("unet_review_pixels"), bundle.settings.segmentReviewPixels);
        context.insert(QStringLiteral("unet_bad_pixels"), bundle.settings.segmentBadPixels);
        context.insert(QStringLiteral("unet_strong_component_pixels"), bundle.settings.segmentStrongComponentPixels);
        context.insert(QStringLiteral("classification_ms"), classifyMs);
        context.insert(QStringLiteral("unet_ms"), segmentMs);
        context.insert(QStringLiteral("cycle_ms"), classifyMs + segmentMs);
        context.insert(QStringLiteral("upload_gate"), QStringLiteral("after_weight_and_ldc"));
        return compactJsonString(context);
    }

    /*
     * fusedResultFromModelResults 的作用：
     *   综合分类模型和 UNet 分割模型的详细输出，生成唯一最终判定。
     *
     * 主要流程：
     *   1. 先读取分类 RESULT 的 status/confidence，只有 GOOD/BAD 属于可信输入。
     *   2. 再读取 UNet RESULT_SEG 的 status/evidence，并校验 CLEAR/WEAK/STRONG 与 OK/NG 一致。
     *   3. STRONG 明确缺陷优先判 bad，避免被分类低置信度门控降级。
     *   4. WEAK 可疑缺陷进入 review；只有可信分类 GOOD 且 UNet CLEAR 才判 good。
     *
     * 参数：
     *   classificationResult 是 defect-classify 输出的一行 RESULT。
     *   segmentationResult 是 defect-segment 输出的一行 RESULT_SEG。
     *   settings 保存本次检测使用的阈值、ROI 和上传策略。
     *
     * 返回值：
     *   返回 FusedDetectResult，供云端上传、历史记录和 QML 首页共用。
     */
    FusedDetectResult fusedResultFromModelResults(const QString &classificationResult,
                                                  const QString &segmentationResult,
                                                  const DetectSettingsSnapshot &settings) const
    {
        FusedDetectResult result;
        const QString classifyStatus = parseTokenValue(classificationResult, QStringLiteral("status"));
        const QString confidenceText = parseTokenValue(classificationResult, QStringLiteral("confidence"));
        const QString segmentStatus = parseTokenValue(segmentationResult, QStringLiteral("status"));
        const QString segmentEvidence = parseTokenValue(segmentationResult, QStringLiteral("evidence")).toUpper();
        const bool classifyKnown = classifyStatus == QStringLiteral("GOOD") || classifyStatus == QStringLiteral("BAD");
        const bool segmentEvidenceKnown = segmentEvidence == QStringLiteral("CLEAR")
            || segmentEvidence == QStringLiteral("WEAK")
            || segmentEvidence == QStringLiteral("STRONG");
        const bool segmentStatusConsistent = (segmentEvidence == QStringLiteral("CLEAR")
                                              && segmentStatus == QStringLiteral("OK"))
            || ((segmentEvidence == QStringLiteral("WEAK") || segmentEvidence == QStringLiteral("STRONG"))
                && segmentStatus == QStringLiteral("NG"));
        const bool segmentKnown = segmentEvidenceKnown && segmentStatusConsistent;
        bool confidenceOk = false;
        const double confidence = confidenceText.toDouble(&confidenceOk);

        result.cloudResult = QStringLiteral("review");
        result.historyText = QStringLiteral("待复核");
        result.uiStatus = QStringLiteral("REVIEW");
        result.reason = QStringLiteral("模型结果待复核");
        result.segmentEvidence = segmentEvidence;
        result.classifyBad = classifyStatus == QStringLiteral("BAD");
        result.segmentBad = segmentEvidence == QStringLiteral("STRONG");
        result.segmentReview = segmentEvidence == QStringLiteral("WEAK");

        if (!classifyKnown || !segmentKnown) {
            result.reason = QStringLiteral("模型结果不完整");
            return result;
        }

        if (result.segmentBad) {
            result.cloudResult = QStringLiteral("bad");
            result.historyText = QStringLiteral("坏品");
            result.uiStatus = QStringLiteral("BAD");
            result.reason = result.classifyBad
                ? QStringLiteral("分类判坏且UNet强缺陷")
                : QStringLiteral("UNet发现连续明确缺陷");
            return result;
        }

        if (!confidenceOk || confidence < settings.reviewThreshold) {
            result.reason = QStringLiteral("分类置信度低且UNet无强缺陷");
            return result;
        }

        if (result.classifyBad) {
            result.cloudResult = QStringLiteral("bad");
            result.historyText = QStringLiteral("坏品");
            result.uiStatus = QStringLiteral("BAD");
            result.reason = QStringLiteral("分类模型判定坏品");
            return result;
        }

        if (result.segmentReview) {
            result.reason = QStringLiteral("UNet存在可疑缺陷需复核");
            return result;
        }

        result.cloudResult = QStringLiteral("good");
        result.historyText = QStringLiteral("良品");
        result.uiStatus = QStringLiteral("GOOD");
        result.reason = QStringLiteral("双模型均未发现缺陷");
        return result;
    }

    /*
     * cloudResultFromFusedResult 的作用：
     *   把综合判定转换为云端 records.result 字段。
     *
     * 参数：
     *   fusedResult 是 fusedResultFromModelResults() 的返回值。
     *
     * 返回值：
     *   返回 good、bad 或 review。
     */
    QString cloudResultFromFusedResult(const FusedDetectResult &fusedResult) const
    {
        return fusedResult.cloudResult.isEmpty() ? QStringLiteral("review") : fusedResult.cloudResult;
    }

    /*
     * historyTextFromFusedResult 的作用：
     *   把综合判定转换成本地历史列表展示的中文主结果。
     *
     * 参数：
     *   fusedResult 是 fusedResultFromModelResults() 的返回值。
     *
     * 返回值：
     *   返回“良品”“坏品”或“待复核”，与首页、F4 最终分拣和云端结果保持一致。
     */
    QString historyTextFromFusedResult(const FusedDetectResult &fusedResult) const
    {
        return fusedResult.historyText.isEmpty() ? QStringLiteral("待复核") : fusedResult.historyText;
    }

    /*
     * uiStatusFromFusedResult 的作用：
     *   把综合判定转换成 QML 首页可直接使用的状态值。
     *
     * 参数：
     *   fusedResult 是 fusedResultFromModelResults() 的返回值。
     *
     * 返回值：
     *   返回 GOOD、BAD 或 REVIEW。
     */
    QString uiStatusFromFusedResult(const FusedDetectResult &fusedResult) const
    {
        return fusedResult.uiStatus.isEmpty() ? QStringLiteral("REVIEW") : fusedResult.uiStatus;
    }

    /*
     * compactUploadStatus 的作用：
     *   把 defect-cos-upload 的原始输出压缩成适合历史页显示的短状态。
     *
     * 主要流程：
     *   1. 复用 parseTokenValue 提取 record_id 和 record_no。
     *   2. 上传成功时只保存“上传成功 + 关键编号”，避免脚本长文本或旧编码问题挤爆界面。
     *   3. 上传失败时保留第一行失败原因，但限制长度，避免异常日志占满历史卡片。
     *
     * 参数：
     *   uploadResult 是 uploadSavedImagesToCos 返回的完整状态文本。
     *
     * 返回值：
     *   返回用于 upload_history_YYYYMMDD.json 的短状态文本。
     */
    QString compactUploadStatus(const QString &uploadResult) const
    {
        const QString recordId = parseTokenValue(uploadResult, QStringLiteral("record_id"));
        const QString recordNo = parseTokenValue(uploadResult, QStringLiteral("record_no"));

        if (isUploadStatusSuccess(uploadResult)) {
            QString status = QStringLiteral("上传成功");

            if (!recordId.isEmpty()) {
                status += QStringLiteral(" ID ") + recordId;
            }

            if (!recordNo.isEmpty()) {
                status += QStringLiteral(" ") + recordNo;
            }

            return status;
        }

        if (isUploadStatusFailure(uploadResult)) {
            return uploadResult.left(80);
        }

        return uploadResult.left(80);
    }

    /*
     * appendUploadHistoryRecord 的作用：
     *   把本次保存/上传结果变成历史记录，追加到 UploadHistoryModel。
     *
     * 主要流程：
     *   1. 读取 JPG/PNG 文件大小，详情页可直接显示。
     *   2. 从上传脚本输出中提取云端 record_id 和 record_no。
     *   3. 生成当前本地时间作为第一层历史卡片的时间标题。
     *   4. 调用模型追加并持久化到当天 `upload_history_YYYYMMDD.json`。
     *
     * 参数：
     *   pair 是本地 JPG/PNG 路径。
     *   uploadResult 是上传脚本返回的成功或失败状态。
     *
     * 返回值：
     *   无返回值；没有历史模型时只输出日志。
     */
    void appendUploadHistoryRecord(const SavedImagePair &pair, const QString &uploadResult)
    {
        if (m_historyModel == nullptr) {
            qWarning() << "upload history model missing, skip append";
            return;
        }

        UploadHistoryEntry entry;
        const QFileInfo jpgInfo(pair.jpgPath);
        const QFileInfo pngInfo(pair.pngPath);

        entry.uploadTime = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        entry.resultText = isUploadStatusSuccess(uploadResult)
            ? QStringLiteral("良品")
            : QStringLiteral("待复核");
        entry.boardResultText = entry.resultText;
        entry.workflowText = isUploadStatusSuccess(uploadResult)
            ? QStringLiteral("云端已归档")
            : QStringLiteral("本地已保存");
        entry.jpgPath = pair.jpgPath;
        entry.pngPath = pair.pngPath;
        entry.sourcePath = pair.jpgPath;
        entry.annotatedPaths.append(pair.pngPath);
        entry.annotatedLabels.append(QStringLiteral("PNG结果图"));
        entry.recordId = parseTokenValue(uploadResult, QStringLiteral("record_id"));
        entry.recordNo = parseTokenValue(uploadResult, QStringLiteral("record_no"));
        entry.uploadStatus = compactUploadStatus(uploadResult);
        entry.sourceSizeBytes = jpgInfo.exists() ? jpgInfo.size() : 0;
        entry.annotatedSizeBytes.append(pngInfo.exists() ? pngInfo.size() : 0);
        entry.jpgSizeBytes = jpgInfo.exists() ? jpgInfo.size() : 0;
        entry.pngSizeBytes = pngInfo.exists() ? pngInfo.size() : 0;

        m_historyModel->appendRecord(entry);
    }

    /*
     * appendDetectHistoryRecord 的作用：
     *   把一次完整双模型检测结果追加到本地历史记录。
     *
     * 主要流程：
     *   1. 读取 source 和所有 annotated 图片大小，详情页可直接显示。
     *   2. 从上传脚本输出中提取云端 record_id 和 record_no。
     *   3. 根据分类模型 GOOD/BAD 和 UNet 缺陷像素生成本次历史主结果。
     *   4. 保存两个模型原始 RESULT 行，方便后续排查阈值和图片对应关系。
     *
     * 参数：
     *   bundle 保存本次检测图片、两个模型输出和上传状态。
     *
     * 返回值：
     *   无返回值；没有历史模型时只输出日志。
     */
    int appendDetectHistoryRecord(const DetectResultBundle &bundle)
    {
        if (m_historyModel == nullptr) {
            qWarning() << "detect history model missing, skip append";
            return -1;
        }

        UploadHistoryEntry entry;
        const QFileInfo sourceInfo(bundle.sourcePath);
        const FusedDetectResult fusedResult =
            fusedResultFromModelResults(bundle.classificationResult,
                                        bundle.segmentationResult,
                                        bundle.settings);
        const QString cloudResult = cloudResultFromFusedResult(fusedResult);
        const QString unetWorkflowText = fusedResult.segmentEvidence == QStringLiteral("STRONG")
            ? QStringLiteral("明确连续缺陷")
            : (fusedResult.segmentEvidence == QStringLiteral("WEAK")
               ? QStringLiteral("存在可疑缺陷")
               : (fusedResult.segmentEvidence == QStringLiteral("CLEAR")
                  ? QStringLiteral("小噪点已过滤")
                  : QStringLiteral("证据未知"))); /* unetWorkflowText 保留三级证据含义，避免 WEAK 被写成“未见缺陷”。 */

        entry.uploadTime = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        entry.resultText = historyTextFromFusedResult(fusedResult);
        entry.boardResultText = entry.resultText;
        entry.workflowText = QStringLiteral("综合%1；分类%2；UNet%3；%4")
            .arg(fusedResult.reason)
            .arg(fusedResult.classifyBad ? QStringLiteral("BAD") : QStringLiteral("GOOD"))
            .arg(unetWorkflowText)
            .arg(isUploadStatusSuccess(bundle.uploadResult)
                 ? QStringLiteral("云端已归档")
                 : QStringLiteral("本地已保存"));
        entry.sourcePath = bundle.sourcePath;
        entry.annotatedPaths = bundle.annotatedPaths;
        entry.annotatedLabels = bundle.annotatedLabels;
        entry.classificationResult = bundle.classificationResult;
        entry.segmentationResult = bundle.segmentationResult;
        entry.jpgPath = bundle.sourcePath;
        entry.pngPath = bundle.annotatedPaths.isEmpty() ? QString() : bundle.annotatedPaths.constFirst();
        entry.recordId = parseTokenValue(bundle.uploadResult, QStringLiteral("record_id"));
        entry.recordNo = parseTokenValue(bundle.uploadResult, QStringLiteral("record_no"));
        entry.uploadStatus = compactUploadStatus(bundle.uploadResult);
        entry.cloudResult = cloudResult;
        entry.partCode = partCodeFromClassificationResult(bundle.classificationResult);
        entry.classLabel = parseTokenValue(bundle.classificationResult, QStringLiteral("class"));
        entry.decisionContextJson = buildDecisionContextJson(bundle);
        entry.visionContextJson = buildVisionContextJson(bundle);
        entry.sourceSizeBytes = sourceInfo.exists() ? sourceInfo.size() : 0;
        entry.jpgSizeBytes = entry.sourceSizeBytes;

        for (const QString &path : bundle.annotatedPaths) {
            const QFileInfo info(path);
            const qint64 sizeBytes = info.exists() ? info.size() : 0;

            entry.annotatedSizeBytes.append(sizeBytes);
            if (entry.pngPath == path) {
                entry.pngSizeBytes = sizeBytes;
            }
        }

        return m_historyModel->appendRecordAndReturnRow(entry);
    }

    /*
     * appendUploadHistoryRecordFromResult 的作用：
     *   从后台保存线程返回的完整状态文本中提取 JPG/PNG 路径和上传结果，并在主线程追加历史记录。
     *
     * 主要流程：
     *   1. 只处理以“保存成功：JPG ... PNG ...”开头的结果，保存失败时不产生历史记录。
     *   2. 复用 parseSavedImagePairFromResult() 抽取本地图片路径。
     *   3. 把分号后面的上传状态交给 appendUploadHistoryRecord()，保持历史页字段和同步自检一致。
     *
     * 参数：
     *   resultText 是 saveCurrentFrameToSdCard() 返回给 QML 的完整中文状态。
     *
     * 返回值：
     *   无返回值；解析失败只输出日志，不影响界面显示保存结果。
     */
    void appendUploadHistoryRecordFromResult(const QString &resultText)
    {
        SavedImagePair pair;
        QString uploadResult;
        QString errorText;

        if (!resultText.startsWith(QStringLiteral("保存成功：JPG "))) {
            return;
        }

        if (!parseSavedImagePairFromResult(resultText, &pair, &uploadResult, &errorText)) {
            qWarning() << "upload history async result parse failed" << errorText << resultText;
            return;
        }

        appendUploadHistoryRecord(pair, uploadResult);
    }

    /*
     * parseSavedImagePairFromResult 的作用：
     *   解析“保存成功：JPG <jpg> PNG <png>；<upload>”格式的控制器结果。
     *
     * 参数：
     *   resultText 是完整保存结果文本。
     *   pair 用于返回 JPG/PNG 本地路径。
     *   uploadResult 用于返回 COS 上传结果；没有上传段时使用“本地已保存”。
     *   errorText 用于返回解析失败原因。
     *
     * 返回值：
     *   解析成功返回 true；格式缺失或路径为空返回 false。
     */
    bool parseSavedImagePairFromResult(const QString &resultText,
                                       SavedImagePair *pair,
                                       QString *uploadResult,
                                       QString *errorText) const
    {
        const QString prefix = QStringLiteral("保存成功：JPG ");
        const QString markerPng = QStringLiteral(" PNG ");
        const int pngMarkerIndex = resultText.indexOf(markerPng, prefix.length());

        if (pngMarkerIndex <= prefix.length()) {
            if (errorText) {
                *errorText = QStringLiteral("缺少 PNG 路径标记");
            }
            return false;
        }

        const int uploadMarkerIndex = resultText.indexOf(QStringLiteral("；"), pngMarkerIndex + markerPng.length());
        const int pngPathEnd = uploadMarkerIndex >= 0 ? uploadMarkerIndex : resultText.length();

        pair->jpgPath = resultText.mid(prefix.length(), pngMarkerIndex - prefix.length()).trimmed();
        pair->pngPath = resultText.mid(pngMarkerIndex + markerPng.length(), pngPathEnd - pngMarkerIndex - markerPng.length()).trimmed();

        if (pair->jpgPath.isEmpty() || pair->pngPath.isEmpty()) {
            if (errorText) {
                *errorText = QStringLiteral("JPG 或 PNG 路径为空");
            }
            return false;
        }

        if (uploadResult) {
            *uploadResult = uploadMarkerIndex >= 0
                ? resultText.mid(uploadMarkerIndex + 1).trimmed()
                : QStringLiteral("本地已保存");
        }

        return true;
    }

    /*
     * uploadSavedImagesToCos 的作用：
     *   调用板端 defect-cos-upload 脚本，把本地 JPG/PNG 上传到云端 COS。
     *
     * 主要流程：
     *   1. 优先使用 /root/qt_camera_display/defect-cos-upload，匹配部署脚本路径。
     *   2. 若绝对路径不存在，则退回 PATH 中的 defect-cos-upload，方便 SSH 调试。
     *   3. 捕获 stdout/stderr，返回适合界面提示的一行结果。
     *
     * 参数：
     *   pair 保存本次本地落盘成功的 JPG/PNG 路径。
     *
     * 返回值：
     *   上传成功返回“上传成功：...”；失败返回“上传失败：...”。
     */
    QString uploadSavedImagesToCos(const SavedImagePair &pair) const
    {
        QStringList annotatedPaths;

        annotatedPaths.append(pair.pngPath);
        return uploadDetectImagesToCos(pair.jpgPath, annotatedPaths);
    }

    /*
     * uploadDetectImagesToCos 的作用：
     *   调用板端 defect-cos-upload 脚本，把一次检测的 source 原图和多张 annotated 结果图上传到云端 COS。
     *
     * 主要流程：
     *   1. 优先使用 /root/qt_camera_display/defect-cos-upload，匹配部署脚本路径。
     *   2. 传入 --jpg <source> 和多次 --annotated <result>。
     *   3. 通过 CLOUD_RESULT 把本次模型 GOOD/BAD/REVIEW 显式传给上传脚本。
     *   4. 通过 CLOUD_PART_CODE 把本次零件类型传给上传脚本；好坏后缀已提前剥离。
     *   5. 通过 CLOUD_CLASS_LABEL 保留模型原始标签，便于云端排查零件映射。
     *   6. 自动流程收齐 F4 称重/电感后，可额外通过 CLOUD_*_CONTEXT 传入 JSON 上下文。
     *   7. 捕获 stdout/stderr，返回适合界面提示和历史记录的一行结果。
     *
     * 参数：
     *   sourcePath 是云端 file_kind=source 的原始检测图。
     *   annotatedPaths 是云端 file_kind=annotated 的所有模型结果图。
     *   cloudResult 是云端记录 result 字段，只允许 good、bad 或 review。
     *   partCode 是云端零件类型候选，例如 gasket；同一零件 good/bad 必须传同一个值。
     *   classLabel 是模型原始分类标签，例如 gasket_good；只用于 device_context 排障。
     *   weightContextJson 是 F4 WEIGHT_RESULT 转换后的称重 JSON。
     *   ldcContextJson 是 F4 LDC_RESULT 转换后的电感 JSON。
     *   f4FlowContextJson 是自动检测过程中 F4/ESP32 阶段和流程 JSON。
     *   decisionContextJson 是 MP157 综合判定、阈值和耗时 JSON。
     *   visionContextJson 是模型原始输出、图片路径和 ROI JSON。
     *
     * 返回值：
     *   上传成功返回“上传成功：...”；失败返回“上传失败：...”。
     */
    QString uploadDetectImagesToCos(const QString &sourcePath,
                                    const QStringList &annotatedPaths,
                                    const QString &cloudResult = QStringLiteral("review"),
                                    const QString &partCode = QString(),
                                    const QString &classLabel = QString(),
                                    const QString &weightContextJson = QString(),
                                    const QString &ldcContextJson = QString(),
                                    const QString &f4FlowContextJson = QString(),
                                    const QString &decisionContextJson = QString(),
                                    const QString &visionContextJson = QString()) const
    {
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QString scriptPath = QString::fromLatin1(DEFAULT_COS_UPLOAD_SCRIPT);
        QString stdoutText;
        QString stderrText;
        QString usefulLine;
        QStringList arguments;
        const QString normalizedCloudResult = (cloudResult == QStringLiteral("good")
                                               || cloudResult == QStringLiteral("bad")
                                               || cloudResult == QStringLiteral("review"))
            ? cloudResult
            : QStringLiteral("review");

        if (!QFileInfo::exists(scriptPath)) {
            scriptPath = QStringLiteral("defect-cos-upload");
        }

        arguments << QStringLiteral("--jpg") << sourcePath;
        for (const QString &path : annotatedPaths) {
            if (!path.isEmpty()) {
                arguments << QStringLiteral("--annotated") << path;
            }
        }

        qInfo() << "storage action cos-upload requested"
                << "script" << scriptPath
                << "source" << sourcePath
                << "annotated" << annotatedPaths
                << "cloudResult" << normalizedCloudResult
                << "partCode" << partCode
                << "classLabel" << classLabel
                << "hasWeightContext" << !weightContextJson.trimmed().isEmpty()
                << "hasLdcContext" << !ldcContextJson.trimmed().isEmpty()
                << "hasF4FlowContext" << !f4FlowContextJson.trimmed().isEmpty();

        env.insert(QStringLiteral("CLOUD_RESULT"), normalizedCloudResult);
        if (!partCode.trimmed().isEmpty()) {
            env.insert(QStringLiteral("CLOUD_PART_CODE"), partCode.trimmed());
        }
        if (!classLabel.trimmed().isEmpty()) {
            env.insert(QStringLiteral("CLOUD_CLASS_LABEL"), classLabel.trimmed());
        }
        if (!weightContextJson.trimmed().isEmpty()) {
            env.insert(QStringLiteral("CLOUD_WEIGHT_CONTEXT"), weightContextJson.trimmed());
        }
        if (!ldcContextJson.trimmed().isEmpty()) {
            env.insert(QStringLiteral("CLOUD_LDC_CONTEXT"), ldcContextJson.trimmed());
        }
        if (!f4FlowContextJson.trimmed().isEmpty()) {
            env.insert(QStringLiteral("CLOUD_F4_FLOW_CONTEXT"), f4FlowContextJson.trimmed());
        }
        if (!decisionContextJson.trimmed().isEmpty()) {
            env.insert(QStringLiteral("CLOUD_DECISION_CONTEXT"), decisionContextJson.trimmed());
        }
        if (!visionContextJson.trimmed().isEmpty()) {
            env.insert(QStringLiteral("CLOUD_VISION_CONTEXT"), visionContextJson.trimmed());
        }
        process.setProcessEnvironment(env);
        process.setProgram(scriptPath);
        process.setArguments(arguments);
        process.start();

        if (!process.waitForStarted(3000)) {
            return QStringLiteral("上传失败：无法启动 defect-cos-upload");
        }

        if (!process.waitForFinished(180000)) {
            process.kill();
            process.waitForFinished(1000);
            return QStringLiteral("上传失败：defect-cos-upload 超时");
        }

        stdoutText = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();

        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            if (!stderrText.isEmpty()) {
                usefulLine = firstLineWithPrefix(stderrText, QStringLiteral("上传失败："));
                if (usefulLine.isEmpty()) {
                    usefulLine = firstUsefulLine(stderrText);
                }
                return usefulLine.startsWith(QStringLiteral("上传失败："))
                    ? usefulLine
                    : QStringLiteral("上传失败：") + usefulLine;
            }
            if (!stdoutText.isEmpty()) {
                usefulLine = firstLineWithPrefix(stdoutText, QStringLiteral("上传失败："));
                if (usefulLine.isEmpty()) {
                    usefulLine = firstUsefulLine(stdoutText);
                }
                return usefulLine.startsWith(QStringLiteral("上传失败："))
                    ? usefulLine
                    : QStringLiteral("上传失败：") + usefulLine;
            }
            return QStringLiteral("上传失败：defect-cos-upload 返回异常");
        }

        if (!stdoutText.isEmpty()) {
            usefulLine = firstUsefulLine(stdoutText);
            return usefulLine.startsWith(QStringLiteral("上传成功："))
                ? usefulLine
                : QStringLiteral("上传成功：") + usefulLine;
        }

        return QStringLiteral("上传成功：JPG/PNG 已上传到 COS");
    }

    /*
     * partCodeFromClassificationResult 的作用：
     *   从分类 RESULT 行中提取云端零件类型候选，并剥离 good/bad 好坏后缀。
     *
     * 主要流程：
     *   1. 读取 class=<label>，例如 gasket_good、gasket_bad、washer_good。
     *   2. 只删除最后的 `_good/_bad/-good/-bad` 后缀，保留真实零件名。
     *   3. 返回值只表示零件类型，不表示本次检测好坏；好坏由 CLOUD_RESULT 单独传递。
     *
     * 参数：
     *   classificationResult 是 defect-classify 输出的一行 RESULT。
     *
     * 返回值：
     *   成功返回零件类型候选，例如 gasket；没有 class 字段时返回空字符串。
     */
    QString partCodeFromClassificationResult(const QString &classificationResult) const
    {
        QString classLabel = parseTokenValue(classificationResult, QStringLiteral("class")).trimmed();

        if (classLabel.isEmpty()) {
            return QString();
        }

        const QString lowerLabel = classLabel.toLower();

        if (lowerLabel.endsWith(QStringLiteral("_good"))
            || lowerLabel.endsWith(QStringLiteral("-good"))) {
            classLabel.chop(5);
        } else if (lowerLabel.endsWith(QStringLiteral("_bad"))
                   || lowerLabel.endsWith(QStringLiteral("-bad"))) {
            classLabel.chop(4);
        }

        return classLabel.trimmed();
    }

    /*
     * detectCurrentFrameOnce 的作用：
     *   在后台线程中完成“一次当前帧保存 + 分类模型推理 + UNet 分割推理 + 本地结果缓存”。
     *
     * 主要流程：
     *   1. 确认 /mnt/sdcard 已挂载，并创建图片历史目录。
     *   2. 先执行检测入口 LOCATE 门禁，确认当前帧有零件后才允许模型检测。
     *   3. 发送 SAVE_DETECT 命令，让 overlay 保存当前帧 JPG 作为 source。
     *   4. 调用 defect-classify，得到 MobileNetV3-Small GOOD/BAD 结果。
     *   5. 调用 defect-segment，得到 UNet raw/overlay/mask 结果图和缺陷像素。
     *   6. 把 source 和所有结果图保存到 bundle，先写 SD 卡历史，不在模型刚结束时上传云端。
     *   7. 自动流程后续必须等待 F4 回传 WEIGHT_RESULT/LDC_RESULT，再一次性上传完整数据。
     *
     * 参数：
     *   mountPoint 是 SD 卡挂载点。
     *   imageDir 是检测图片输出目录。
     *   classifyBin 是独立推理程序路径。
     *   modelPath 是分类 INT8 ONNX 模型路径。
     *   labelsPath 是标签 JSON 路径。
     *   segmentBin 是 UNet 分割推理程序路径。
     *   segmentModelPath 是 UNet INT8 ONNX 模型路径。
     *   bundle 用于返回本次检测图片路径、两个模型输出和上传状态。
     *
     * 返回值：
     *   成功返回 "RESULT status=... segment_status=... ..."；
     *   失败返回 "检测失败：<原因>"，供 QML 原样显示。
     */
    QString detectCurrentFrameOnce(const QString &mountPoint,
                                   const QString &imageDir,
                                   const QString &classifyBin,
                                   const QString &modelPath,
                                   const QString &labelsPath,
                                   const QString &segmentBin,
                                   const QString &segmentModelPath,
                                   const DetectSettingsSnapshot &settings,
                                   DetectResultBundle *bundle,
                                   const DetectProgressCallback &classificationReadyCallback = DetectProgressCallback(),
                                   const DetectProgressCallback &modelsReadyCallback = DetectProgressCallback())
    {
        QString detectImagePath;
        QString errorText;
        QString reply;
        QString classificationResult;
        QString segmentationResult;
        QString modelResult;
        QString cloudResult;
        FusedDetectResult fusedResult;
        qint64 totalModelTimeMs = 0;
        QElapsedTimer totalDetectTimer;

        qInfo() << "detect action requested"
                << "imageDir" << imageDir
                << "classifyBin" << classifyBin
                << "classifyModel" << modelPath
                << "labels" << labelsPath
                << "segmentBin" << segmentBin
                << "segmentModel" << segmentModelPath
                << "roi" << settings.roiSize
                << "badThreshold" << settings.modelThreshold
                << "reviewThreshold" << settings.reviewThreshold
                << "segmentMinComponentPixels" << settings.segmentMinComponentPixels
                << "segmentReviewPixels" << settings.segmentReviewPixels
                << "segmentBadPixels" << settings.segmentBadPixels
                << "segmentStrongComponentPixels" << settings.segmentStrongComponentPixels
                << "overlayAlpha" << settings.overlayAlpha
                << "autoUpload" << settings.autoUploadEnabled
                << "uploadMode" << "LOCAL_READY";

        if (bundle == nullptr) {
            return QStringLiteral("检测失败：内部结果缓存为空");
        }

        if (!isMountPointMounted(mountPoint, &errorText)) {
            return QStringLiteral("检测失败：") + errorText;
        }

        if (!QDir().mkpath(imageDir)) {
            return QStringLiteral("检测失败：无法创建检测图片目录 ") + imageDir;
        }

        if (!ensureCurrentFrameHasLocateTarget(&errorText)) {
            return QStringLiteral("检测失败：") + errorText;
        }

        reply = sendRawOverlayCommand(QStringLiteral("SAVE_DETECT ") + imageDir);
        if (reply.isEmpty()) {
            return QStringLiteral("检测失败：overlay 没有返回结果");
        }

        if (reply.startsWith(QStringLiteral("ERR "))) {
            return QStringLiteral("检测失败：") + reply.mid(4);
        }

        if (!parseDetectSaveReply(reply, &detectImagePath, &errorText)) {
            return QStringLiteral("检测失败：") + errorText;
        }

        QFileInfo imageInfo(detectImagePath);
        if (!imageInfo.exists() || imageInfo.size() <= 0) {
            return QStringLiteral("检测失败：当前帧 JPG 不存在或为空：") + detectImagePath;
        }

        /* totalDetectTimer 只覆盖两个模型本身，避免把拍照、文件校验和网络上传算进“检测耗时”。 */
        totalDetectTimer.start();

        classificationResult = runDefectClassify(classifyBin,
                                                 modelPath,
                                                 labelsPath,
                                                 detectImagePath,
                                                 settings);
        if (!classificationResult.startsWith(QStringLiteral("RESULT "))) {
            return classificationResult;
        }
        if (classificationReadyCallback) {
            classificationReadyCallback(classificationResult);
        }

        segmentationResult = runDefectSegment(segmentBin,
                                              segmentModelPath,
                                              detectImagePath,
                                              imageDir,
                                              settings,
                                              bundle);
        if (!segmentationResult.startsWith(QStringLiteral("RESULT_SEG "))) {
            return segmentationResult;
        }
        totalModelTimeMs = totalDetectTimer.elapsed();

        bundle->sourcePath = detectImagePath;
        bundle->classificationResult = classificationResult;
        bundle->segmentationResult = segmentationResult;
        bundle->settings = settings;
        fusedResult = fusedResultFromModelResults(classificationResult, segmentationResult, settings);
        modelResult = buildDetectModelResultLine(classificationResult,
                                                 segmentationResult,
                                                 fusedResult,
                                                 totalModelTimeMs,
                                                 bundle->sourcePath);
        if (modelsReadyCallback) {
            modelsReadyCallback(modelResult);
        }
        cloudResult = cloudResultFromFusedResult(fusedResult);
        bundle->uploadResult = QStringLiteral("本地已保存：upload_status=LOCAL_READY cloud_result=")
            + cloudResult
            + QStringLiteral(" 等待F4称重和电感结果后统一上传");

        return modelResult
            + QStringLiteral(" upload_status=")
            + QStringLiteral("LOCAL_READY");
    }

    /*
     * runDefectClassify 的作用：
     *   调用独立 defect-classify 程序，并把输出压缩成 QML 可解析的一行。
     *
     * 主要流程：
     *   1. 校验程序、模型、标签和图片文件是否存在。
     *   2. 传入 --image/--model/--labels/--roi 和 --bad-threshold，使用参数页真实配置。
     *   3. 等待进程结束，成功时返回 stdout 中第一行 RESULT，失败时返回 stderr/stdout 中的首行错误。
     *
     * 参数：
     *   classifyBin/modelPath/labelsPath/imagePath 分别是推理程序、模型、标签和图片路径。
     *   settings 保存本次检测使用的 ROI 和分类判坏阈值。
     *
     * 返回值：
     *   成功返回 RESULT 行；失败返回“检测失败：...”。
     */
    QString runDefectClassify(const QString &classifyBin,
                              const QString &modelPath,
                              const QString &labelsPath,
                              const QString &imagePath,
                              const DetectSettingsSnapshot &settings) const
    {
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QString stdoutText;
        QString stderrText;
        QString usefulLine;

        if (!QFileInfo::exists(classifyBin)) {
            return QStringLiteral("检测失败：找不到推理程序 ") + classifyBin;
        }

        if (!QFileInfo::exists(modelPath)) {
            return QStringLiteral("检测失败：找不到模型 ") + modelPath;
        }

        if (!QFileInfo::exists(labelsPath)) {
            return QStringLiteral("检测失败：找不到标签 ") + labelsPath;
        }

        if (!QFileInfo::exists(imagePath)) {
            return QStringLiteral("检测失败：找不到当前帧图片 ") + imagePath;
        }

        env.insert(QStringLiteral("LD_LIBRARY_PATH"),
                   QStringLiteral("/root/qt_camera_display/lib:")
                   + env.value(QStringLiteral("LD_LIBRARY_PATH")));
        process.setProcessEnvironment(env);
        process.setProgram(classifyBin);
        process.setArguments(QStringList()
                             << QStringLiteral("--image") << imagePath
                             << QStringLiteral("--model") << modelPath
                             << QStringLiteral("--labels") << labelsPath
                             << QStringLiteral("--roi") << QString::number(settings.roiSize)
                             << QStringLiteral("--bad-threshold") << QString::number(settings.modelThreshold, 'f', 3));
        process.start();

        if (!process.waitForStarted(3000)) {
            return QStringLiteral("检测失败：无法启动 defect-classify");
        }

        if (!process.waitForFinished(120000)) {
            process.kill();
            process.waitForFinished(1000);
            return QStringLiteral("检测失败：defect-classify 超时");
        }

        stdoutText = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();

        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            if (!stderrText.isEmpty()) {
                usefulLine = firstUsefulLine(stderrText);
                return usefulLine.startsWith(QStringLiteral("检测失败："))
                    ? usefulLine
                    : QStringLiteral("检测失败：") + usefulLine;
            }
            if (!stdoutText.isEmpty()) {
                usefulLine = firstUsefulLine(stdoutText);
                return usefulLine.startsWith(QStringLiteral("检测失败："))
                    ? usefulLine
                    : QStringLiteral("检测失败：") + usefulLine;
            }
            return QStringLiteral("检测失败：defect-classify 返回异常");
        }

        if (!stdoutText.isEmpty()) {
            usefulLine = firstUsefulLine(stdoutText);
            return usefulLine.startsWith(QStringLiteral("RESULT "))
                ? usefulLine
                : QStringLiteral("检测失败：推理输出缺少 RESULT：") + usefulLine.left(120);
        }

        return QStringLiteral("检测失败：defect-classify 没有输出");
    }

    /*
     * runDefectSegment 的作用：
     *   调用独立 defect-segment 程序，并把输出路径写入 DetectResultBundle。
     *
     * 主要流程：
     *   1. 校验程序、UNet 模型、输入图片和输出目录是否存在或可用。
     *   2. 传入图片、模型、输出目录、ROI、透明度和四个 UNet 证据阈值，使用参数页真实配置。
     *   3. 等待进程结束，严格校验证据等级、统计字段和 OK/NG 状态的一致性。
     *   4. 解析 RESULT_SEG 中的 raw_path/overlay_path/mask_path，并确认文件已经真实落盘。
     *   5. 把 raw/overlay/mask 三张图都放入 annotatedPaths，后续统一作为 --annotated 上传。
     *
     * 参数：
     *   segmentBin/modelPath/imagePath/outputDir 分别是推理程序、模型、输入图和输出目录。
     *   settings 保存本次检测使用的 ROI、overlay 透明度和 UNet 四个证据阈值。
     *   bundle 用于保存分割结果图路径和标签。
     *
     * 返回值：
     *   成功返回 RESULT_SEG 行；失败返回“检测失败：...”。
     */
    QString runDefectSegment(const QString &segmentBin,
                             const QString &modelPath,
                             const QString &imagePath,
                             const QString &outputDir,
                             const DetectSettingsSnapshot &settings,
                             DetectResultBundle *bundle) const
    {
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QString stdoutText;
        QString stderrText;
        QString usefulLine;
        QString rawPath;
        QString overlayPath;
        QString maskPath;
        QString evidenceText;
        QString segmentStatus;
        bool defectPixelsOk = false;
        bool rawPixelsOk = false;
        bool filteredPixelsOk = false;
        bool largestComponentOk = false;
        bool componentCountOk = false;
        bool retainedComponentCountOk = false;
        bool minComponentPixelsOk = false;
        bool reviewPixelsOk = false;
        bool badPixelsOk = false;
        bool strongComponentPixelsOk = false;

        if (bundle == nullptr) {
            return QStringLiteral("检测失败：UNet 结果缓存为空");
        }

        if (!QFileInfo::exists(segmentBin)) {
            return QStringLiteral("检测失败：找不到 UNet 推理程序 ") + segmentBin;
        }

        if (!QFileInfo::exists(modelPath)) {
            return QStringLiteral("检测失败：找不到 UNet 模型 ") + modelPath;
        }

        if (!QFileInfo::exists(imagePath)) {
            return QStringLiteral("检测失败：找不到 UNet 输入图片 ") + imagePath;
        }

        if (!QDir().mkpath(outputDir)) {
            return QStringLiteral("检测失败：无法创建 UNet 输出目录 ") + outputDir;
        }

        env.insert(QStringLiteral("LD_LIBRARY_PATH"),
                   QStringLiteral("/root/qt_camera_display/lib:")
                   + env.value(QStringLiteral("LD_LIBRARY_PATH")));
        process.setProcessEnvironment(env);
        process.setProgram(segmentBin);
        process.setArguments(QStringList()
                             << QStringLiteral("--image") << imagePath
                             << QStringLiteral("--model") << modelPath
                             << QStringLiteral("--output-dir") << outputDir
                             << QStringLiteral("--roi") << QString::number(settings.roiSize)
                             << QStringLiteral("--alpha") << QString::number(settings.overlayAlpha, 'f', 2)
                             << QStringLiteral("--min-component-pixels")
                             << QString::number(settings.segmentMinComponentPixels)
                             << QStringLiteral("--review-defect-pixels")
                             << QString::number(settings.segmentReviewPixels)
                             << QStringLiteral("--bad-defect-pixels")
                             << QString::number(settings.segmentBadPixels)
                             << QStringLiteral("--strong-component-pixels")
                             << QString::number(settings.segmentStrongComponentPixels));
        process.start();

        if (!process.waitForStarted(3000)) {
            return QStringLiteral("检测失败：无法启动 defect-segment");
        }

        if (!process.waitForFinished(180000)) {
            process.kill();
            process.waitForFinished(1000);
            return QStringLiteral("检测失败：defect-segment 超时");
        }

        stdoutText = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();

        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            if (!stderrText.isEmpty()) {
                usefulLine = firstUsefulLine(stderrText);
                return usefulLine.startsWith(QStringLiteral("检测失败："))
                    ? usefulLine
                    : QStringLiteral("检测失败：") + usefulLine;
            }
            if (!stdoutText.isEmpty()) {
                usefulLine = firstUsefulLine(stdoutText);
                return usefulLine.startsWith(QStringLiteral("检测失败："))
                    ? usefulLine
                    : QStringLiteral("检测失败：") + usefulLine;
            }
            return QStringLiteral("检测失败：defect-segment 返回异常");
        }

        if (stdoutText.isEmpty()) {
            return QStringLiteral("检测失败：defect-segment 没有输出");
        }

        usefulLine = firstUsefulLine(stdoutText);
        if (!usefulLine.startsWith(QStringLiteral("RESULT_SEG "))) {
            return QStringLiteral("检测失败：UNet 输出缺少 RESULT_SEG：") + usefulLine.left(120);
        }

        evidenceText = parseTokenValue(usefulLine, QStringLiteral("evidence")).toUpper();
        segmentStatus = parseTokenValue(usefulLine, QStringLiteral("status")).toUpper();
        const int defectPixels = parseTokenValue(usefulLine,
                                                  QStringLiteral("defect_pixels")).toInt(&defectPixelsOk);
        const int rawDefectPixels = parseTokenValue(usefulLine,
                                                     QStringLiteral("raw_defect_pixels")).toInt(&rawPixelsOk);
        const int filteredDefectPixels = parseTokenValue(usefulLine,
                                                          QStringLiteral("filtered_defect_pixels")).toInt(&filteredPixelsOk);
        const int largestComponentPixels = parseTokenValue(usefulLine,
                                                             QStringLiteral("largest_component_pixels")).toInt(&largestComponentOk);
        const int componentCount = parseTokenValue(usefulLine,
                                                    QStringLiteral("component_count")).toInt(&componentCountOk);
        const int retainedComponentCount = parseTokenValue(usefulLine,
                                                            QStringLiteral("retained_component_count")).toInt(&retainedComponentCountOk);
        const int echoedMinComponentPixels = parseTokenValue(usefulLine,
                                                              QStringLiteral("min_component_pixels")).toInt(&minComponentPixelsOk);
        const int echoedReviewPixels = parseTokenValue(usefulLine,
                                                        QStringLiteral("review_defect_pixels")).toInt(&reviewPixelsOk);
        const int echoedBadPixels = parseTokenValue(usefulLine,
                                                     QStringLiteral("bad_defect_pixels")).toInt(&badPixelsOk);
        const int echoedStrongComponentPixels = parseTokenValue(usefulLine,
                                                                 QStringLiteral("strong_component_pixels")).toInt(&strongComponentPixelsOk);
        const bool evidenceKnown = evidenceText == QStringLiteral("CLEAR")
            || evidenceText == QStringLiteral("WEAK")
            || evidenceText == QStringLiteral("STRONG");
        const bool statusConsistent = (evidenceText == QStringLiteral("CLEAR")
                                       && segmentStatus == QStringLiteral("OK"))
            || ((evidenceText == QStringLiteral("WEAK") || evidenceText == QStringLiteral("STRONG"))
                && segmentStatus == QStringLiteral("NG"));

        /*
         * 这里不接受缺字段被 QString::toInt() 静默转成 0：旧版推理程序没有三级证据时必须明确失败，
         * 防止 Qt 把“不完整结果”误当 CLEAR 良品继续送往最终分拣。
         */
        if (!evidenceKnown || !statusConsistent) {
            return QStringLiteral("检测失败：UNet evidence/status 不合法或不一致：")
                + evidenceText + QLatin1Char('/') + segmentStatus;
        }
        if (!defectPixelsOk || !rawPixelsOk || !filteredPixelsOk || !largestComponentOk
                || !componentCountOk || !retainedComponentCountOk) {
            return QStringLiteral("检测失败：UNet 输出缺少合法的连通域统计字段");
        }
        if (defectPixels < 0
                || rawDefectPixels < 0
                || defectPixels != rawDefectPixels
                || filteredDefectPixels < 0
                || filteredDefectPixels > rawDefectPixels
                || largestComponentPixels < 0
                || largestComponentPixels > rawDefectPixels
                || componentCount < 0
                || retainedComponentCount < 0
                || retainedComponentCount > componentCount) {
            return QStringLiteral("检测失败：UNet 连通域统计字段关系不合法");
        }

        /* helper 必须回显本轮实际使用的四阈值；不匹配说明 Qt 与 defect-segment 版本或参数已经漂移。 */
        if (!minComponentPixelsOk || !reviewPixelsOk || !badPixelsOk || !strongComponentPixelsOk
                || echoedMinComponentPixels != settings.segmentMinComponentPixels
                || echoedReviewPixels != settings.segmentReviewPixels
                || echoedBadPixels != settings.segmentBadPixels
                || echoedStrongComponentPixels != settings.segmentStrongComponentPixels) {
            return QStringLiteral("检测失败：UNet 回显阈值与本轮板端参数不一致");
        }

        const defect_segment_evidence::SegmentEvidenceSettings evidenceSettings{
            settings.segmentMinComponentPixels,
            settings.segmentReviewPixels,
            settings.segmentBadPixels,
            settings.segmentStrongComponentPixels
        }; /* evidenceSettings 把 Qt 快照转换为共用算法输入，确保二次校验和 helper 使用同一公式。 */
        const defect_segment_evidence::SegmentEvidenceLevel expectedLevel =
            defect_segment_evidence::segment_evidence_level_from_stats(
                filteredDefectPixels,
                largestComponentPixels,
                evidenceSettings);
        const QString expectedEvidence = QString::fromLatin1(
            defect_segment_evidence::segment_evidence_level_name(expectedLevel));
        if (evidenceText != expectedEvidence) {
            return QStringLiteral("检测失败：UNet 证据等级与连通域统计不一致，期望")
                + expectedEvidence + QStringLiteral("，实际") + evidenceText;
        }

        rawPath = parseTokenValue(usefulLine, QStringLiteral("raw_path"));
        overlayPath = parseTokenValue(usefulLine, QStringLiteral("overlay_path"));
        maskPath = parseTokenValue(usefulLine, QStringLiteral("mask_path"));

        if (rawPath.isEmpty() || overlayPath.isEmpty() || maskPath.isEmpty()) {
            return QStringLiteral("检测失败：UNet 输出缺少 raw/overlay/mask 路径");
        }

        const QStringList paths = QStringList() << rawPath << overlayPath << maskPath;
        for (const QString &path : paths) {
            const QFileInfo info(path);

            if (!info.exists() || info.size() <= 0) {
                return QStringLiteral("检测失败：UNet 结果图不存在或为空：") + path;
            }
        }

        bundle->annotatedPaths.clear();
        bundle->annotatedLabels.clear();
        bundle->annotatedPaths << rawPath << overlayPath << maskPath;
        bundle->annotatedLabels << QStringLiteral("UNet原图")
                                << QStringLiteral("UNet叠加图")
                                << QStringLiteral("UNet掩膜图");

        return usefulLine;
    }

    /*
     * sendOverlayCommand 的作用：
     *   连接 overlay 控制 socket，发送 SAVE_DUAL 命令并转换为 QML 可显示的中文状态。
     */
    QString sendRawOverlayCommand(const QString &command)
    {
        int fd = -1;
        struct sockaddr_un addr;
        QString errorText;
        QString reply;
        QByteArray socketPathBytes = m_socketPath.toLocal8Bit();
        QByteArray commandBytes = command.toLocal8Bit() + '\n';

        if (socketPathBytes.size() >= static_cast<int>(sizeof(addr.sun_path))) {
            return QStringLiteral("ERR 控制 socket 路径过长");
        }

        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return QStringLiteral("ERR 创建 socket 失败：") + QString::fromLocal8Bit(strerror(errno));
        }

        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socketPathBytes.constData(), sizeof(addr.sun_path) - 1U);

        if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
            const QString detail = QString::fromLocal8Bit(strerror(errno));

            ::close(fd);
            return QStringLiteral("ERR overlay 控制端未连接：") + detail;
        }

        if (!writeAllToFd(fd, commandBytes, &errorText)) {
            ::close(fd);
            return QStringLiteral("ERR 发送请求失败：") + errorText;
        }

        reply = readReplyFromFd(fd, &errorText);
        ::close(fd);

        if (reply.isEmpty()) {
            return QStringLiteral("ERR overlay 没有返回结果：") + errorText;
        }

        return reply;
    }

    /*
     * sendOverlayCommand 的作用：
     *   连接 overlay 控制 socket，发送 SAVE_DUAL/VISIBLE 命令并转换为 QML 可显示的中文状态。
     *
     * 参数：
     *   command 是发给 overlay 的一行命令。
     *
     * 返回值：
     *   保存类命令返回“保存成功/保存失败”；VISIBLE 命令返回视频层切换状态。
     */
    QString sendOverlayCommand(const QString &command)
    {
        QString errorText;
        QString reply;
        SavedImagePair pair;

        reply = sendRawOverlayCommand(command);

        if (reply.startsWith(QStringLiteral("OK JPG "))) {
            if (!parseDualSaveReply(reply, &pair, &errorText)) {
                return QStringLiteral("保存失败：") + errorText;
            }

            const QString localResult = QStringLiteral("保存成功：JPG ")
                + pair.jpgPath
                + QStringLiteral(" PNG ")
                + pair.pngPath;
            const QString uploadResult = uploadSavedImagesToCos(pair);

            if (m_appendHistoryInSave) {
                appendUploadHistoryRecord(pair, uploadResult);
            }

            qInfo() << "storage action cos-upload result" << uploadResult;
            return localResult + QStringLiteral("；") + uploadResult;
        }

        if (reply.startsWith(QStringLiteral("OK "))) {
            if (command.startsWith(QStringLiteral("VISIBLE "))) {
                return reply.mid(3);
            }
            return QStringLiteral("保存成功：") + reply.mid(3);
        }

        if (reply.startsWith(QStringLiteral("ERR "))) {
            if (command.startsWith(QStringLiteral("VISIBLE "))) {
                return QStringLiteral("视频层切换失败：") + reply.mid(4);
            }
            return QStringLiteral("保存失败：") + reply.mid(4);
        }

        return QStringLiteral("保存失败：未知返回：") + reply;
    }

    /*
     * setSaveInProgress 的作用：
     *   集中更新异步保存忙状态，并在状态变化时通知 QML。
     *
     * 参数：
     *   inProgress 为 true 表示后台保存任务开始；false 表示任务结束或启动失败。
     *
     * 返回值：
     *   无返回值。
     */
    void setSaveInProgress(bool inProgress)
    {
        if (m_saveInProgress == inProgress) {
            return;
        }

        m_saveInProgress = inProgress;
        emit saveInProgressChanged();
    }

    /*
     * setDetectInProgress 的作用：
     *   集中更新异步检测忙状态，并在状态变化时通知 QML。
     *
     * 参数：
     *   inProgress 为 true 表示后台检测任务开始；false 表示任务结束或启动失败。
     *
     * 返回值：
     *   无返回值。
     */
    void setDetectInProgress(bool inProgress)
    {
        if (m_detectInProgress == inProgress) {
            return;
        }

        m_detectInProgress = inProgress;
        emit detectInProgressChanged();
    }

    /*
     * setRetryUploadInProgress 的作用：
     *   集中更新历史图片重新发送忙状态，并通知 QML 刷新按钮文案和置灰状态。
     *
     * 参数：
     *   inProgress 为 true 表示重新发送任务开始；false 表示任务结束或启动失败。
     *
     * 返回值：
     *   无返回值。
     */
    void setRetryUploadInProgress(bool inProgress)
    {
        if (m_retryUploadInProgress == inProgress) {
            return;
        }

        m_retryUploadInProgress = inProgress;
        emit retryUploadInProgressChanged();
    }

    QString m_socketPath;  /* m_socketPath 是 overlay 控制 socket 路径。 */
    QString m_mountPoint;  /* m_mountPoint 是 SD 卡挂载点。 */
    QString m_imageDir;    /* m_imageDir 是图片保存目录。 */
    QString m_logDir;      /* m_logDir 是 SD 卡诊断和自动告警日志目录。 */
    UploadHistoryModel *m_historyModel; /* m_historyModel 指向 QML 使用的上传历史模型，保存成功后会追加记录。 */
    DetectSettingsController *m_detectSettingsController; /* m_detectSettingsController 指向真实检测参数控制器，不拥有生命周期。 */
    DetectSettingsSnapshot m_detectSettingsSnapshot; /* m_detectSettingsSnapshot 保存后台线程或自检路径使用的检测配置快照。 */
    DetectResultBundle m_latestDetectBundle; /* m_latestDetectBundle 缓存最近一次模型检测的本地图片、模型输出和参数快照，供自动流程完整上传。 */
    int m_latestDetectHistoryRow; /* m_latestDetectHistoryRow 保存最近一次检测追加到历史模型的行号，完整上传后原地更新该行。 */
    bool m_appendHistoryInSave; /* m_appendHistoryInSave 控制同步保存函数是否立即追加历史记录。 */
    bool m_saveInProgress; /* m_saveInProgress 只在 Qt 主线程维护，用于防止保存图片任务重复启动。 */
    bool m_detectInProgress; /* m_detectInProgress 只在 Qt 主线程维护，用于防止检测任务重复启动。 */
    bool m_retryUploadInProgress; /* m_retryUploadInProgress 只在 Qt 主线程维护，用于防止历史重发任务重复启动。 */
    bool m_hasLatestDetectBundle; /* m_hasLatestDetectBundle 表示 m_latestDetectBundle 已经来自一次成功 RESULT，而不是默认空结构。 */
};

/*
 * DeviceHealthController 的作用：
 *   统一管理 Qt 页面顶部状态栏和告警页里的真实设备在线状态。
 *
 * 主要流程：
 *   1. 周期性异步检测 4G 网络、KMS overlay 相机、F4 串口和云端 health。
 *   2. 每个检测都有忙标志和短超时，避免检测任务堆积或阻塞 Qt 主线程。
 *   3. QML 只读取 status/color/detail 属性，不直接执行 shell、串口或 socket 操作。
 *   4. 相机连续离线时尝试后台重启 overlay 控制脚本，让 USB 摄像头重新插入后能恢复画面。
 *
 * 关键说明：
 *   这个控制器只做“健康探测”，不接管保存、检测和 overlay 可见性控制，避免和 CameraStorageController
 *   的业务动作互相影响。
 */
class DeviceHealthController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString networkStatusText READ networkStatusText NOTIFY networkStatusChanged)
    Q_PROPERTY(QString networkStatusColor READ networkStatusColor NOTIFY networkStatusChanged)
    Q_PROPERTY(QString cameraStatusText READ cameraStatusText NOTIFY cameraStatusChanged)
    Q_PROPERTY(QString cameraStatusColor READ cameraStatusColor NOTIFY cameraStatusChanged)
    Q_PROPERTY(QString f4StatusText READ f4StatusText NOTIFY f4StatusChanged)
    Q_PROPERTY(QString f4StatusColor READ f4StatusColor NOTIFY f4StatusChanged)
    Q_PROPERTY(QString cloudStatusText READ cloudStatusText NOTIFY cloudStatusChanged)
    Q_PROPERTY(QString cloudStatusColor READ cloudStatusColor NOTIFY cloudStatusChanged)
    Q_PROPERTY(QString sdcardStatusText READ sdcardStatusText NOTIFY sdcardStatusChanged)
    Q_PROPERTY(QString sdcardStatusColor READ sdcardStatusColor NOTIFY sdcardStatusChanged)
    Q_PROPERTY(QString locationStatusText READ locationStatusText NOTIFY locationStatusChanged)
    Q_PROPERTY(QString locationDisplayText READ locationDisplayText NOTIFY locationStatusChanged)
    Q_PROPERTY(QString locationShortText READ locationShortText NOTIFY locationStatusChanged)
    Q_PROPERTY(QString locationStatusColor READ locationStatusColor NOTIFY locationStatusChanged)
    Q_PROPERTY(QString detailText READ detailText NOTIFY detailTextChanged)

public:
    /*
     * 构造函数的作用：
     *   初始化状态文本、探测路径和定时器间隔。
     *
     * 参数：
     *   videoBackend 是当前 Qt 视频后端；kms-overlay 时相机状态以 overlay STATUS 为准。
     *   cameraDevice 是摄像头设备节点，用于 qt-safe/qt-gst 或离线提示。
     *   parent 是 Qt 对象树父对象。
     */
    explicit DeviceHealthController(const QString &videoBackend,
                                    const QString &cameraDevice,
                                    QObject *parent = nullptr)
        : QObject(parent),
          m_videoBackend(videoBackend),
          m_cameraDevice(cameraDevice),
          m_overlaySocket(QString::fromLatin1(DEFAULT_OVERLAY_CONTROL_SOCKET)),
          m_overlayRestartScript(QString::fromLatin1(DEFAULT_OVERLAY_RESTART_SCRIPT)),
          m_networkScript(QString::fromLatin1(DEFAULT_4G_PPP_SCRIPT)),
          m_locationScript(QString::fromLatin1(DEFAULT_4G_LOCATION_SCRIPT)),
          m_cloudHealthUrl(QString::fromLatin1(DEFAULT_CLOUD_HEALTH_URL)),
          m_sdcardMount(QString::fromLatin1(DEFAULT_SDCARD_MOUNT_POINT)),
          m_f4Device(QString::fromLatin1(DEFAULT_F4_SERIAL_DEVICE)),
          m_f4Baud(DEFAULT_F4_SERIAL_BAUD),
          m_f4ActuatorStopGeneration(new std::atomic<quint64>(0U)),
          m_f4SerialWriteMutex(new QMutex),
          m_networkStatusText(QStringLiteral("检测中")),
          m_networkStatusColor(QStringLiteral("#f4b942")),
          m_cameraStatusText(QStringLiteral("检测中")),
          m_cameraStatusColor(QStringLiteral("#f4b942")),
          m_f4StatusText(QStringLiteral("待接入")),
          m_f4StatusColor(QStringLiteral("#f4b942")),
          m_cloudStatusText(QStringLiteral("检测中")),
          m_cloudStatusColor(QStringLiteral("#f4b942")),
          m_sdcardStatusText(QStringLiteral("检测中")),
          m_sdcardStatusColor(QStringLiteral("#f4b942")),
          m_locationStatusText(QStringLiteral("未定位")),
          m_locationDisplayText(QStringLiteral("未定位")),
          m_locationShortText(QStringLiteral("未定位")),
          m_locationStatusColor(QStringLiteral("#f4b942")),
          m_detailText(QStringLiteral("设备健康检测启动")),
          m_lastOverlaySerial(0),
          m_cameraOfflineCount(0),
          m_overlayRestartCooldown(0),
          m_networkProbeRunning(false),
          m_locationProbeRunning(false),
          m_cloudProbeRunning(false),
          m_f4ProbeRunning(false),
          m_f4CommandRunning(false),
          m_overlayProbeRunning(false),
          m_autoVisionLocateRunning(false),
          m_networkProbeTimedOut(false),
          m_locationProbeTimedOut(false),
          m_locationBootProbeDone(false),
          m_cloudProbeTimedOut(false),
          m_f4AutoCycleId(0U),
          m_f4BinarySequence(0U),
          m_f4AutoRunning(false),
          m_f4AutoPaused(false),
          m_f4ArmFlowRunning(false),
          m_f4FinalSortRunning(false),
          m_f4LastArmJobId(0U),
          m_f4LastModelResult(0U),
          m_f4LastFinalBin(0U)
    {
        /*
         * m_f4HeartbeatElapsed 只用于控制周期心跳节奏。
         * invalidate() 让第一次 startF4Probe(false) 不受 2 分钟间隔限制，程序启动后能立即确认 F4 是否在线。
         */
        m_f4HeartbeatElapsed.invalidate();

        /* 主健康定时器只调度后台刷新，间隔放慢到 8 秒，避免顶部状态频繁跳动或频繁跑外设测试。 */
        m_healthTimer.setInterval(8000);
        m_healthTimer.setSingleShot(false);
        connect(&m_healthTimer, &QTimer::timeout, this, &DeviceHealthController::refreshAllStatus);

        /* 网络和云端探测独立使用 QProcess；finished/timeout 都回到主线程更新状态。 */
        m_networkProcess.setProcessChannelMode(QProcess::MergedChannels);
        connect(&m_networkProcess,
                static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                this,
                &DeviceHealthController::handleNetworkProcessFinished);
        connect(&m_networkProcess,
                static_cast<void (QProcess::*)(QProcess::ProcessError)>(&QProcess::errorOccurred),
                this,
                &DeviceHealthController::handleNetworkProcessError);
        connect(&m_networkTimeout, &QTimer::timeout, this, &DeviceHealthController::handleNetworkProbeTimeout);
        m_networkTimeout.setSingleShot(true);

        /*
         * 定位进程只调用 4g-location。
         * 4g-location 自身只访问高德 IP 定位 HTTPS 接口，不打开 GPS，也不访问 ttyUSB AT 串口。
         */
        m_locationProcess.setProcessChannelMode(QProcess::MergedChannels);
        connect(&m_locationProcess,
                static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                this,
                &DeviceHealthController::handleLocationProcessFinished);
        connect(&m_locationProcess,
                static_cast<void (QProcess::*)(QProcess::ProcessError)>(&QProcess::errorOccurred),
                this,
                &DeviceHealthController::handleLocationProcessError);
        connect(&m_locationTimeout, &QTimer::timeout, this, &DeviceHealthController::handleLocationProbeTimeout);
        m_locationTimeout.setSingleShot(true);

        m_cloudProcess.setProcessChannelMode(QProcess::MergedChannels);
        connect(&m_cloudProcess,
                static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                this,
                &DeviceHealthController::handleCloudProcessFinished);
        connect(&m_cloudProcess,
                static_cast<void (QProcess::*)(QProcess::ProcessError)>(&QProcess::errorOccurred),
                this,
                &DeviceHealthController::handleCloudProcessError);
        connect(&m_cloudTimeout, &QTimer::timeout, this, &DeviceHealthController::handleCloudProbeTimeout);
        m_cloudTimeout.setSingleShot(true);
    }

    /* networkStatusText 返回 4G 网络状态文本，QML 顶部状态栏直接显示它。 */
    QString networkStatusText() const { return m_networkStatusText; }

    /* networkStatusColor 返回 4G 网络状态颜色，绿色在线、黄色等待、红色离线。 */
    QString networkStatusColor() const { return m_networkStatusColor; }

    /* cameraStatusText 返回摄像头状态文本，kms-overlay 下来自 overlay STATUS。 */
    QString cameraStatusText() const { return m_cameraStatusText; }

    /* cameraStatusColor 返回摄像头状态颜色。 */
    QString cameraStatusColor() const { return m_cameraStatusColor; }

    /* f4StatusText 返回 F4 串口握手状态文本。 */
    QString f4StatusText() const { return m_f4StatusText; }

    /* f4StatusColor 返回 F4 串口握手状态颜色。 */
    QString f4StatusColor() const { return m_f4StatusColor; }

    /* cloudStatusText 返回云端 health 状态文本。 */
    QString cloudStatusText() const { return m_cloudStatusText; }

    /* cloudStatusColor 返回云端 health 状态颜色。 */
    QString cloudStatusColor() const { return m_cloudStatusColor; }

    /* sdcardStatusText 返回 SD 卡挂载状态文本。 */
    QString sdcardStatusText() const { return m_sdcardStatusText; }

    /* sdcardStatusColor 返回 SD 卡挂载状态颜色。 */
    QString sdcardStatusColor() const { return m_sdcardStatusColor; }

    /* locationStatusText 返回定位状态，例如 IP定位、缺少Key、定位失败或未定位。 */
    QString locationStatusText() const { return m_locationStatusText; }

    /* locationDisplayText 返回完整位置文本；当前高德 IP 省份定位模式只返回省份，例如 河南省。 */
    QString locationDisplayText() const { return m_locationDisplayText; }

    /* locationShortText 返回顶部状态栏短位置文本；当前同样只返回省份或缺少Key。 */
    QString locationShortText() const { return m_locationShortText; }

    /* locationStatusColor 返回定位状态颜色，绿色成功、黄色等待/缺 Key、红色失败或超时。 */
    QString locationStatusColor() const { return m_locationStatusColor; }

    /* detailText 返回最近一次健康检测详情，用于告警页和日志排查。 */
    QString detailText() const { return m_detailText; }

    /*
     * start 的作用：
     *   启动周期性健康检测，并立即跑第一轮，避免开机后长时间显示旧占位状态。
     */
    Q_INVOKABLE void start()
    {
        refreshAllStatus();
        m_healthTimer.start();
    }

    /*
     * refreshAllStatus 的作用：
     *   调度一轮设备健康检测。
     *
     * 返回值：
     *   无返回值；每个子检测完成后通过属性通知 QML。
     */
    Q_INVOKABLE void refreshAllStatus()
    {
        refreshSdcardStatus();
        refreshOverlayCameraStatus();
        startNetworkProbe();
        if (!m_locationBootProbeDone) {
            /*
             * 位置只在 Qt 进程启动后的第一轮健康检测执行一次。
             * 后续 8 秒周期刷新不再启动 4g-location，避免反复访问高德接口和占用 4G 网络。
             */
            startLocationProbe();
        }
        startCloudProbe();
        startF4Probe(false);
    }

    /*
     * refreshF4StatusNow 的作用：
     *   供 QML 或人工操作立即触发一次 F4 二进制 HEARTBEAT 握手。
     *
     * 主要流程：
     *   1. 不改变 4G、相机、云端和 SD 卡状态，只操作 F4 串口。
     *   2. 使用 forceNow=true 绕过 2 分钟心跳节流，便于用户刚接好线后立刻验证。
     *
     * 返回值：
     *   无返回值；握手完成后通过 f4StatusChanged 和 detailTextChanged 通知 QML。
     */
    Q_INVOKABLE void refreshF4StatusNow()
    {
        startF4Probe(true);
    }

    /*
     * sendF4Command 的作用：
     *   从 QML 发送一条二进制 F4 称重标定命令。
     *
     * 主要流程：
     *   1. 保留 QML 现有 `CAL <克重>` 调用形状，避免 QML 直接关心二进制 payload 偏移；
     *   2. C++ 再次解析并校验 1~5000g，防止 QML 被绕过时发送非法克重；
     *   3. 组装 WEIGHT_CALIBRATE 负载：cycle_id=0、known_weight_g、flags=0；
     *   4. 后台线程写入 `/dev/ttySTM2`，并等待 F4 返回匹配 ACK/NACK。
     *
     * 参数：
     *   commandText 是 QML 传入的原始命令文本，格式必须是 `CAL <1~5000整数克重>`。
     *
     * 返回值：
     *   true 表示后台发送任务已启动；false 表示参数非法或已有命令正在发送。
     */
    Q_INVOKABLE bool sendF4Command(const QString &commandText)
    {
        QString command = commandText.trimmed();
        const QStringList commandParts = command.split(QRegExp(QStringLiteral("\\s+")), QString::SkipEmptyParts);
        bool weightOk = false;
        quint32 knownWeight = 0U;
        QByteArray payload;

        if (command.isEmpty()) {
            emit f4CommandFinished(false, QStringLiteral("F4命令为空"));
            return false;
        }

        if (commandParts.size() != 2
                || commandParts.at(0).compare(QStringLiteral("CAL"), Qt::CaseInsensitive) != 0) {
            emit f4CommandFinished(false, QStringLiteral("当前界面只允许发起 CAL 二进制称重标定命令"));
            return false;
        }

        knownWeight = commandParts.at(1).toUInt(&weightOk, 10);
        if (!weightOk || knownWeight < 1U || knownWeight > 5000U) {
            emit f4CommandFinished(false, QStringLiteral("标定克重必须是1~5000g整数"));
            return false;
        }

        if (m_f4CommandRunning) {
            emit f4CommandFinished(false, QStringLiteral("上一条F4命令仍在发送中"));
            return false;
        }

        if (m_f4ProbeRunning) {
            emit f4CommandFinished(false, QStringLiteral("F4状态刷新仍在进行，请稍后再发送CAL"));
            return false;
        }

        m_f4CommandRunning = true;

        QPointer<DeviceHealthController> self(this);
        const QString dev = m_f4Device;
        const int baud = m_f4Baud;
        const quint16 sequence = m_f4BinarySequence++;
        appendLe16(&payload, 0U);                                    /* cycle_id=0，人工称重标定不绑定自动检测流程。 */
        appendLe16(&payload, static_cast<quint16>(knownWeight));      /* known_weight_g，单位克，F4 仍会按 HX711 量程复核。 */
        payload.append(static_cast<char>(0U));                        /* flags=0，首版不自动去皮、不保存 Flash、不扩展动作。 */
        const QByteArray frame = buildF4BinaryFrame(BINARY_PROTOCOL_CMD_WEIGHT_CALIBRATE, sequence, payload);

        QThread *workerThread = QThread::create([self, dev, baud, frame, sequence]() {
            QString detail;
            const bool ok = sendF4BinaryCommand(dev,
                                                baud,
                                                frame,
                                                BINARY_PROTOCOL_CMD_WEIGHT_CALIBRATE,
                                                sequence,
                                                0U,
                                                &detail);

            if (!self) {
                return;
            }

            QMetaObject::invokeMethod(self.data(),
                                      "handleF4CommandFinished",
                                      Qt::QueuedConnection,
                                      Q_ARG(bool, ok),
                                      Q_ARG(QString, detail));
        });

        if (workerThread == nullptr) {
            m_f4CommandRunning = false;
            emit f4CommandFinished(false, QStringLiteral("F4命令线程创建失败"));
            return false;
        }

        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);
        workerThread->start();
        return true;
    }

    /*
     * sendF4BeltCommand 的作用：
     *   从 QML 手动控制页发送 F407 二进制传送带调试命令。
     *
     * 主要流程：
     *   1. QML 只传入 `BELT_MANUAL_SCAN`、`BELT_MANUAL_STOP`、`QUERY_STATUS` 这类按钮语义名；
     *   2. `BELT_MANUAL_SCAN/STOP` 发送 BELT_MANUAL_CONTROL，成功只认 ACK，失败只认 NACK；
     *   3. `QUERY_STATUS` 发送 QUERY_STATUS，成功只认 STATUS_REPORT，失败只认 NACK；
     *   4. 复用同一个串口忙标志，避免自动流程、传送带手动命令和 F4 心跳同时抢 `/dev/ttySTM2`。
     *
     * 参数：
     *   commandText 是 QML 传入的传送带按钮语义名，不会被原样写到 F407 串口。
     *
     * 返回值：
     *   true 表示后台发送任务已启动；false 表示命令不在白名单、串口忙或线程创建失败。
     */
    Q_INVOKABLE bool sendF4BeltCommand(const QString &commandText)
    {
        QString command = commandText.trimmed().toUpper();
        const QString commandLabel = command;
        quint8 binaryCommand = 0U;        /* binaryCommand 保存实际发送给 F4 的二进制 CMD。 */
        quint16 cycleId = 0U;             /* 手动传送带调试不绑定自动流程，cycle_id 固定为 0。 */
        QByteArray payload;               /* payload 保存手动命令或状态查询负载。 */

        if (command.isEmpty()) {
            emit f4ManualCommandFinished(false, commandLabel, QStringLiteral("F4传送带命令为空"));
            return false;
        }

        /*
         * 白名单必须和 F407 侧已有二进制回包能力一致。
         * 当前位置只开放扫描、停止和状态查询，避免 UI 发送 F407 尚未定义 ACK/NACK 的运动命令。
         */
        if (!isAllowedF4BeltCommand(command)) {
            emit f4ManualCommandFinished(false,
                                         commandLabel,
                                         QStringLiteral("当前界面只允许发送 BELT_MANUAL_SCAN/BELT_MANUAL_STOP/QUERY_STATUS"));
            return false;
        }

        if (m_f4CommandRunning) {
            emit f4ManualCommandFinished(false, commandLabel, QStringLiteral("上一条F4命令仍在发送中"));
            return false;
        }

        if (m_f4ProbeRunning) {
            emit f4ManualCommandFinished(false,
                                         commandLabel,
                                         QStringLiteral("F4状态刷新仍在进行，请稍后再发送传送带命令"));
            return false;
        }

        if (command == QStringLiteral("BELT_MANUAL_SCAN")) {
            binaryCommand = BINARY_PROTOCOL_CMD_BELT_MANUAL_CONTROL;
            appendLe16(&payload, cycleId);
            payload.append(static_cast<char>(0x01)); /* action=1，表示传送带进入扫描巡航。 */
            payload.append(static_cast<char>(0x00)); /* flags=0，首版无额外标志。 */
        } else if (command == QStringLiteral("BELT_MANUAL_STOP")) {
            binaryCommand = BINARY_PROTOCOL_CMD_BELT_MANUAL_CONTROL;
            appendLe16(&payload, cycleId);
            payload.append(static_cast<char>(0x00)); /* action=0，表示传送带停止。 */
            payload.append(static_cast<char>(0x00)); /* flags=0，首版无额外标志。 */
        } else if (command == QStringLiteral("QUERY_STATUS")) {
            binaryCommand = BINARY_PROTOCOL_CMD_QUERY_STATUS;
            appendLe16(&payload, cycleId);
            payload.append(static_cast<char>(0x03)); /* query_mask bit0/bit1：查询协议状态和传送带状态。 */
        }

        const quint16 sequence = m_f4BinarySequence++;
        const QByteArray frame = buildF4BinaryFrame(binaryCommand, sequence, payload);
        m_f4CommandRunning = true;

        QPointer<DeviceHealthController> self(this);
        const QString dev = m_f4Device;
        const int baud = m_f4Baud;

        QThread *workerThread = QThread::create([self, dev, baud, frame, commandLabel, binaryCommand, sequence, cycleId]() {
            QString detail;
            bool ok = false;

            if (binaryCommand == BINARY_PROTOCOL_CMD_QUERY_STATUS) {
                ok = sendF4BinaryStatusQuery(dev, baud, frame, sequence, cycleId, &detail);
            } else {
                ok = sendF4BinaryCommand(dev, baud, frame, binaryCommand, sequence, cycleId, &detail);
            }

            if (!self) {
                return;
            }

            QMetaObject::invokeMethod(self.data(),
                                      "handleF4ManualCommandFinished",
                                      Qt::QueuedConnection,
                                      Q_ARG(bool, ok),
                                      Q_ARG(QString, commandLabel),
                                      Q_ARG(QString, detail));
        });

        if (workerThread == nullptr) {
            m_f4CommandRunning = false;
            emit f4ManualCommandFinished(false, commandLabel, QStringLiteral("F4传送带命令线程创建失败"));
            return false;
        }

        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);
        workerThread->start();
        return true;
    }

    /*
     * sendF4StepperSettings 的作用：
     *   把参数页当前三台步进电机配置下发给 F407，让 F4 侧电机服务更新运行参数。
     *
     * 主要流程：
     *   1. 从 QML 传入的 stepperMotorSettings 数组提取三台电机配置。
     *   2. 组装 STEPPER_PARAM_SET 固定负载：cycle_id、数量、保留位和三条电机记录。
     *   3. 后台线程写入 /dev/ttySTM2 并等待 F4 返回匹配 ACK/NACK。
     *
     * 参数：
     *   motors 是 DetectSettingsController::stepperMotorSettings 暴露给 QML 的 QVariantList。
     *
     * 返回值：
     *   true 表示后台发送任务已启动；false 表示参数非法、串口忙或线程创建失败。
     */
    Q_INVOKABLE bool sendF4StepperSettings(const QVariantList &motors)
    {
        QByteArray payload;       /* payload 保存将写入 F4 的三台电机参数负载。 */
        QString rejectText;       /* rejectText 保存本地校验失败原因，直接显示给参数弹窗。 */

        if (!buildStepperSettingsPayload(motors, &payload, &rejectText)) {
            emit f4StepperSettingsFinished(false, rejectText);
            return false;
        }

        if (m_f4CommandRunning) {
            emit f4StepperSettingsFinished(false, QStringLiteral("上一条F4命令仍在发送中"));
            return false;
        }

        if (m_f4ProbeRunning) {
            emit f4StepperSettingsFinished(false, QStringLiteral("F4状态刷新仍在进行，请稍后再下发步进参数"));
            return false;
        }

        const quint16 sequence = m_f4BinarySequence++;
        const QByteArray frame = buildF4BinaryFrame(BINARY_PROTOCOL_CMD_STEPPER_PARAM_SET, sequence, payload);
        m_f4CommandRunning = true;

        QPointer<DeviceHealthController> self(this);
        const QString dev = m_f4Device;
        const int baud = m_f4Baud;

        QThread *workerThread = QThread::create([self, dev, baud, frame, sequence]() {
            QString detail;
            const bool ok = sendF4BinaryCommand(dev,
                                                baud,
                                                frame,
                                                BINARY_PROTOCOL_CMD_STEPPER_PARAM_SET,
                                                sequence,
                                                0U,
                                                &detail);

            if (!self) {
                return;
            }

            QMetaObject::invokeMethod(self.data(),
                                      "handleF4StepperSettingsFinished",
                                      Qt::QueuedConnection,
                                      Q_ARG(bool, ok),
                                      Q_ARG(QString, detail));
        });

        if (workerThread == nullptr) {
            m_f4CommandRunning = false;
            emit f4StepperSettingsFinished(false, QStringLiteral("F4步进参数线程创建失败"));
            return false;
        }

        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);
        workerThread->start();
        return true;
    }

    /*
     * sendF4ActuatorPositionMove 的作用：
     *   让 QML 通过统一二进制协议请求 F4 控制某个执行器做固定步数位置运动。
     *
     * 主要流程：
     *   1. 校验 actuator/direction/mode/speed/steps，防止 QML 或触摸误操作把非法值发到 F4。
     *   2. 自动流程运行时复用当前 cycle_id，手动调试时使用 cycle_id=0。
     *   3. 按 `cycle_id, actuator, direction, mode, speed_rpm, steps, flags` 编码负载。
     *   4. 复用串口忙标志和 ACK/NACK 等待逻辑，保证不会和视觉闭环、称重标定、参数下发抢串口。
     *
     * 参数：
     *   actuator 是执行器编号：0=传送带，1=摄像头左右，2=摄像头上下。
     *   direction 是逻辑方向：传送带 0=后退/1=前进，左右轴 0=左移/1=右移，上下轴 0=下降/1=上升。
     *   mode 是位置运动模式，首版使用 0 表示相对位置模式。
     *   speedRpm 是本次运动速度，0 表示让 F4 使用该轴运行时默认速度，最大 5000 rpm。
     *   stepsValue 是相对移动步数，合法范围 1~4294967295 step。
     *   flags 是扩展标志，首版填 0。
     *
     * 返回值：
     *   true 表示后台串口任务已启动；false 表示参数非法、串口忙或线程创建失败。
     */
    Q_INVOKABLE bool sendF4ActuatorPositionMove(int actuator,
                                                int direction,
                                                int mode,
                                                int speedRpm,
                                                double stepsValue,
                                                int flags)
    {
        return sendF4ActuatorPositionMoveWithTimeout(actuator,
                                                     direction,
                                                     mode,
                                                     speedRpm,
                                                     stepsValue,
                                                     flags,
                                                     MP157_ACTUATOR_FALLBACK_MAX_MS);
    }

    /*
     * sendF4ActuatorPositionMoveWithTimeout 的作用：
     *   发送 ACTUATOR_POS_MOVE，并允许 QML 为本次位置运动指定 MP157 本地最大等待时间。
     *
     * 主要流程：
     *   1. 和 sendF4ActuatorPositionMove() 使用同一套参数校验和负载编码。
     *   2. timeoutMs 只影响 MP157 等待 F4 DONE 或本地估算完成的最大时长，不写入 F4 协议负载。
     *   3. 摄像头上下轴自动下降/回升用参数页 zMotionTimeoutMs 调用这里，避免把 10 秒写死在代码里。
     *
     * 参数：
     *   timeoutMs 是本次 MP157 本地等待上限，单位 ms，会被限制在 1~60 秒。
     *
     * 返回值：
     *   true 表示后台串口任务已启动；false 表示参数非法、串口忙或线程创建失败。
     */
    Q_INVOKABLE bool sendF4ActuatorPositionMoveWithTimeout(int actuator,
                                                           int direction,
                                                           int mode,
                                                           int speedRpm,
                                                           double stepsValue,
                                                           int flags,
                                                           int timeoutMs)
    {
        const quint16 cycleId = (m_f4AutoRunning || m_f4AutoPaused) ? m_f4AutoCycleId : 0U;
        const quint32 steps = clampedUInt32FromDouble(stepsValue);
        const int fallbackMaxWaitMs = clampedInt(timeoutMs,
                                                 MP157_CAMERA_Z_TIMEOUT_MIN_MS,
                                                 MP157_CAMERA_Z_TIMEOUT_MAX_MS);
        QByteArray payload;        /* payload 保存 ACTUATOR_POS_MOVE 的固定 12 字节负载。 */
        QString rejectText;        /* rejectText 保存本地参数校验失败原因。 */

        if (actuator < 0 || actuator > 2) {
            rejectText = QStringLiteral("执行器编号必须是 0=传送带、1=左右轴、2=上下轴");
        } else if (direction != 0 && direction != 1) {
            rejectText = QStringLiteral("执行器方向必须是 0=后退/下降 或 1=前进/上升");
        } else if (mode != 0) {
            rejectText = QStringLiteral("执行器位置模式首版只支持 mode=0 相对移动");
        } else if (speedRpm < 0 || speedRpm > 5000) {
            rejectText = QStringLiteral("执行器速度必须是 0~5000 rpm");
        } else if (steps == 0U) {
            rejectText = QStringLiteral("执行器位置移动步数必须大于 0");
        } else if (flags < 0 || flags > 255) {
            rejectText = QStringLiteral("执行器 flags 必须是 0~255");
        }

        if (!rejectText.isEmpty()) {
            emit f4ActuatorCommandFinished(false,
                                           QStringLiteral("ACTUATOR_POS_MOVE"),
                                           cycleId,
                                           rejectText);
            return false;
        }

        appendLe16(&payload, cycleId);                                  /* cycle_id：自动流程中用于和本轮检测绑定。 */
        payload.append(static_cast<char>(actuator & 0xFF));             /* actuator：0 传送带，1 左右轴，2 上下轴。 */
        payload.append(static_cast<char>(direction & 0xFF));            /* direction：传送带后退/前进，左右轴左移/右移，上下轴下降/上升。 */
        payload.append(static_cast<char>(mode & 0xFF));                 /* mode：0 相对位置模式。 */
        appendLe16(&payload, static_cast<quint16>(speedRpm));           /* speed_rpm：位置运动速度，0 交给 F4 用默认速度。 */
        appendLe32(&payload, steps);                                    /* steps：Emm42 0xFD 位置模式 4 字节脉冲数。 */
        payload.append(static_cast<char>(flags & 0xFF));                /* flags：首版保留，当前填 0。 */

        return startF4ActuatorCommand(QStringLiteral("ACTUATOR_POS_MOVE"),
                                      BINARY_PROTOCOL_CMD_ACTUATOR_POS_MOVE,
                                      cycleId,
                                      payload,
                                      fallbackMaxWaitMs);
    }

    /*
     * sendF4ActuatorVelocityMove 的作用：
     *   让 QML 通过统一二进制协议请求 F4 控制某个执行器持续速度运动。
     *
     * 主要流程：
     *   1. 校验 actuator/direction/speed/flags，防止触摸误操作把危险命令发给 F4。
     *   2. 按 `cycle_id, actuator, direction, speed_rpm, flags` 编码 ACTUATOR_VEL_MOVE 负载。
     *   3. 复用串口忙标志和 ACK/NACK 等待逻辑，保证和自动视觉、称重标定、参数下发互斥。
     *
     * 参数：
     *   actuator 是执行器编号：0=传送带，1=摄像头左右；上下轴不允许速度连续运动。
     *   direction 是逻辑方向：传送带 0=后退/1=前进，左右轴 0=左移/1=右移。
     *   speedRpm 是持续运动速度，必须是 1~5000 rpm。
     *   flags 是扩展标志，首版填 0。
     *
     * 返回值：
     *   true 表示后台串口任务已启动；false 表示参数非法、串口忙或线程创建失败。
     */
    Q_INVOKABLE bool sendF4ActuatorVelocityMove(int actuator,
                                                int direction,
                                                int speedRpm,
                                                int flags)
    {
        const quint16 cycleId = (m_f4AutoRunning || m_f4AutoPaused) ? m_f4AutoCycleId : 0U;
        QByteArray payload;        /* payload 保存 ACTUATOR_VEL_MOVE 的固定 7 字节负载。 */
        QString rejectText;        /* rejectText 保存本地参数校验失败原因。 */

        if (actuator < 0 || actuator > 1) {
            rejectText = QStringLiteral("速度模式只允许 0=传送带、1=左右轴；上下轴必须用固定步数位置模式");
        } else if (direction != 0 && direction != 1) {
            rejectText = QStringLiteral("执行器方向必须是 0=后退/左移 或 1=前进/右移");
        } else if (speedRpm <= 0 || speedRpm > 5000) {
            rejectText = QStringLiteral("持续运动速度必须是 1~5000 rpm，请先在参数设置中配置常规速度");
        } else if (flags < 0 || flags > 255) {
            rejectText = QStringLiteral("执行器 flags 必须是 0~255");
        }

        if (!rejectText.isEmpty()) {
            emit f4ActuatorCommandFinished(false,
                                           QStringLiteral("ACTUATOR_VEL_MOVE"),
                                           cycleId,
                                           rejectText);
            return false;
        }

        appendLe16(&payload, cycleId);                                  /* cycle_id：手动调试通常为 0。 */
        payload.append(static_cast<char>(actuator & 0xFF));             /* actuator：0 传送带，1 左右轴。 */
        payload.append(static_cast<char>(direction & 0xFF));            /* direction：传送带后退/前进，左右轴左移/右移。 */
        appendLe16(&payload, static_cast<quint16>(speedRpm));           /* speed_rpm：持续速度，必须大于 0。 */
        payload.append(static_cast<char>(flags & 0xFF));                /* flags：首版保留，当前填 0。 */

        return startF4ActuatorCommand(QStringLiteral("ACTUATOR_VEL_MOVE"),
                                      BINARY_PROTOCOL_CMD_ACTUATOR_VEL_MOVE,
                                      cycleId,
                                      payload);
    }

    /*
     * sendF4ActuatorStop 的作用：
     *   让 QML 通过二进制协议请求 F4 停止指定执行器或全部执行器。
     *
     * 主要流程：
     *   1. 校验 actuator：0=传送带，1=左右轴，2=上下轴，0xFF=全部停止。
     *   2. 自动流程运行时带当前 cycle_id，手动急停或调试时 cycle_id=0。
     *   3. 下发 ACTUATOR_STOP 后等待 F4 ACK/NACK，成功与否通过 f4ActuatorCommandFinished 返回 QML。
     *
     * 参数：
     *   actuator 是执行器编号，0xFF 表示全部。
     *   flags 是扩展标志，首版填 0。
     *
     * 返回值：
     *   true 表示后台串口任务已启动；false 表示参数非法、串口忙或线程创建失败。
     */
    Q_INVOKABLE bool sendF4ActuatorStop(int actuator, int flags)
    {
        const quint16 cycleId = (m_f4AutoRunning || m_f4AutoPaused) ? m_f4AutoCycleId : 0U;
        QByteArray payload;        /* payload 保存 ACTUATOR_STOP 的固定 4 字节负载。 */
        QString rejectText;        /* rejectText 保存本地参数校验失败原因。 */

        if (!((actuator >= 0 && actuator <= 2) || actuator == 0xFF)) {
            rejectText = QStringLiteral("停止执行器编号必须是 0/1/2 或 0xFF");
        } else if (flags < 0 || flags > 255) {
            rejectText = QStringLiteral("停止执行器 flags 必须是 0~255");
        }

        if (!rejectText.isEmpty()) {
            emit f4ActuatorCommandFinished(false,
                                           QStringLiteral("ACTUATOR_STOP"),
                                           cycleId,
                                           rejectText);
            return false;
        }

        appendLe16(&payload, cycleId);                         /* cycle_id：自动流程中用于和本轮检测绑定。 */
        payload.append(static_cast<char>(actuator & 0xFF));    /* actuator：0/1/2 指定轴，0xFF 表示全部执行器。 */
        payload.append(static_cast<char>(flags & 0xFF));       /* flags：首版保留，当前填 0。 */

        return startF4ActuatorCommand(QStringLiteral("ACTUATOR_STOP"),
                                      BINARY_PROTOCOL_CMD_ACTUATOR_STOP,
                                      cycleId,
                                      payload);
    }

    /*
     * sendF4ActuatorStopNow 的作用：
     *   给手动停止键和模拟急停提供安全优先的 ACTUATOR_STOP 写入通道。
     *
     * 主要流程：
     *   1. 校验 actuator 和 flags，仍然只允许 0/1/2 或 0xFF。
     *   2. 强制 STOP 使用 cycle_id=0，表示安全停机不绑定当前自动流程号。
     *   3. 不检查 m_f4CommandRunning，也不等待 ACK，只要求后台线程把完整帧写入串口并 tcdrain。
     *
     * 关键原因：
     *   手动左右轴采用 ACTUATOR_VEL_MOVE 连续速度模式。若上一条运动命令线程正在等待 ACK，
     *   普通 startF4ActuatorCommand() 会拒绝 STOP，现场就会表现为“停止键没有反应”。
     *   这里不抢读 ACK，避免两个后台线程同时读取 `/dev/ttySTM2` 导致回包被错误线程消费；
     *   F4 收到 STOP 后会在自己的摄像头电机队列中插队停止。
     *   另外旧 F4 固件可能仍按 cycle_id 判断 STOP 是否属于当前流程；cycle_id=0 可以绕开
     *   MP157 与 F4 自动流程号漂移，让“微调结束/超时/强制停机”优先停住真实电机。
     *
     * 返回值：
     *   true 表示强制停止写入线程已启动；false 表示参数非法或线程创建失败。
     */
    Q_INVOKABLE bool sendF4ActuatorStopNow(int actuator, int flags)
    {
        const quint16 cycleId = 0U; /* 强制 STOP 使用 cycle_id=0，避免旧 F4 因流程号漂移拒绝安全停机。 */
        QByteArray payload;        /* payload 保存 ACTUATOR_STOP 的固定 4 字节负载。 */
        QString rejectText;        /* rejectText 保存本地参数校验失败原因。 */

        if (!((actuator >= 0 && actuator <= 2) || actuator == 0xFF)) {
            rejectText = QStringLiteral("强制停止执行器编号必须是 0/1/2 或 0xFF");
        } else if (flags < 0 || flags > 255) {
            rejectText = QStringLiteral("强制停止 flags 必须是 0~255");
        }

        if (!rejectText.isEmpty()) {
            emit f4ActuatorCommandFinished(false,
                                           QStringLiteral("ACTUATOR_STOP_NOW"),
                                           cycleId,
                                           rejectText);
            return false;
        }

        appendLe16(&payload, cycleId);                         /* cycle_id=0：强制 STOP 是安全命令，不和某一轮检测流程绑定。 */
        payload.append(static_cast<char>(actuator & 0xFF));    /* actuator：0/1/2 指定轴，0xFF 表示全部执行器。 */
        payload.append(static_cast<char>(flags & 0xFF));       /* flags：首版保留，当前填 0。 */

        return startF4ActuatorStopNowCommand(QStringLiteral("ACTUATOR_STOP_NOW"),
                                             cycleId,
                                             payload);
    }

    /*
     * sendF4ActuatorHome 的作用：
     *   让参数设置页通过二进制协议请求 F4 把某个执行器当前位置设为新的零点。
     *
     * 主要流程：
     *   1. 校验 actuator/flags，禁止把 0xFF 当作全部清零，避免误改多个电机的标定基准。
     *   2. 按 `cycle_id, actuator, flags` 编码 ACTUATOR_HOME 负载。
     *   3. 复用执行器串口命令线程，等待 F4 ACK/NACK 后由 QML 显示结果。
     *
     * 参数：
     *   actuator 是执行器编号：0=传送带，1=摄像头左右轴，2=摄像头上下轴。
     *   flags 是扩展标志，首版填 0。
     *
     * 返回值：
     *   true 表示后台串口任务已启动；false 表示参数非法、串口忙或线程创建失败。
     */
    Q_INVOKABLE bool sendF4ActuatorHome(int actuator, int flags)
    {
        const quint16 cycleId = (m_f4AutoRunning || m_f4AutoPaused) ? m_f4AutoCycleId : 0U;
        QByteArray payload;        /* payload 保存 ACTUATOR_HOME 的固定 4 字节负载。 */
        QString rejectText;        /* rejectText 保存本地参数校验失败原因。 */

        if (actuator < 0 || actuator > 2) {
            rejectText = QStringLiteral("设零执行器编号必须是 0=传送带、1=左右轴、2=上下轴");
        } else if (flags < 0 || flags > 255) {
            rejectText = QStringLiteral("设零 flags 必须是 0~255");
        }

        if (!rejectText.isEmpty()) {
            emit f4ActuatorCommandFinished(false,
                                           QStringLiteral("ACTUATOR_HOME"),
                                           cycleId,
                                           rejectText);
            return false;
        }

        appendLe16(&payload, cycleId);                         /* cycle_id：参数页标定通常为 0。 */
        payload.append(static_cast<char>(actuator & 0xFF));    /* actuator：0/1/2 指定当前页电机。 */
        payload.append(static_cast<char>(flags & 0xFF));       /* flags：首版保留，当前填 0。 */

        return startF4ActuatorCommand(QStringLiteral("ACTUATOR_HOME"),
                                      BINARY_PROTOCOL_CMD_ACTUATOR_HOME,
                                      cycleId,
                                      payload);
    }

    /*
     * sendF4AutoControlCommand 的作用：
     *   把首页“开始、暂停、继续、停止”四个按钮映射为 MP157->F407 二进制自动检测协议帧。
     *
     * 主要流程：
     *   1. 根据当前 MP157 本地自动流程状态校验 action 是否允许，避免停止后继续、空闲时暂停等误操作。
     *   2. 为开始命令创建新的 cycle_id；暂停、继续和停止复用当前 cycle_id。
     *   3. 组装协议帧并在后台线程打开 `/dev/ttySTM2` 发送，等待 F4 返回 ACK/NACK 二进制帧。
     *   4. 后台线程结束后回到 Qt 主线程更新本地 running/paused 状态，再通知 QML 更新首页文案。
     *
     * 参数：
     *   action 是 QML 传入的动作标识，只允许 start、pause、resume、stop。
     *
     * 返回值：
     *   true 表示后台发送任务已启动；false 表示动作非法、串口忙、状态不允许或线程创建失败。
     */
    Q_INVOKABLE bool sendF4AutoControlCommand(const QString &action)
    {
        const QString normalizedAction = action.trimmed().toLower();
        quint8 command = 0U;             /* command 保存要写入协议 CMD 字段的命令字。 */
        quint16 cycleId = m_f4AutoCycleId; /* cycleId 保存本次自动流程命令归属的流程号。 */
        QByteArray payload;              /* payload 保存当前命令的固定负载，字段均按协议小端写入。 */
        QString rejectText;              /* rejectText 保存本地状态机拒绝动作时给 QML 的中文原因。 */

        if (m_f4CommandRunning) {
            emit f4AutoControlFinished(false,
                                       normalizedAction,
                                       m_f4AutoCycleId,
                                       QStringLiteral("上一条F4串口命令仍在发送中"));
            return false;
        }

        if (m_f4ProbeRunning) {
            emit f4AutoControlFinished(false,
                                       normalizedAction,
                                       m_f4AutoCycleId,
                                       QStringLiteral("F4状态刷新仍在进行，请稍后再操作自动流程"));
            return false;
        }

        /*
         * 首页四按钮的本地状态机：
         * start 只在空闲、停止或完成后新建 cycle；
         * pause 只允许暂停正在运行且未暂停的 cycle；
         * resume 只允许恢复已暂停的同一个 cycle；
         * stop 固定发送 cycle_id=0 强制清理 F4 残留流程，保证停止按钮任何时候都能作为恢复入口。
         */
        if (normalizedAction == QStringLiteral("start")) {
            if (m_f4AutoRunning || m_f4AutoPaused) {
                rejectText = QStringLiteral("当前流程未停止，请先按停止后再开始新检测");
            } else {
                cycleId = nextF4AutoCycleId();
                command = BINARY_PROTOCOL_CMD_START_CYCLE;
                appendLe16(&payload, cycleId);
                payload.append(static_cast<char>(0x00)); /* mode=0，表示完整自动检测。 */
                appendLe16(&payload, 0x0007U);           /* option_bits bit0/1/2：称重、电感、分拣均启用。 */
                payload.append(static_cast<char>(0x00)); /* camera_profile=0，调试阶段使用默认相机位置方案。 */
            }
        } else if (normalizedAction == QStringLiteral("pause")) {
            if (!m_f4AutoRunning || m_f4AutoPaused || cycleId == 0U) {
                rejectText = QStringLiteral("当前没有正在运行的自动检测流程可暂停");
            } else {
                command = BINARY_PROTOCOL_CMD_PAUSE_CYCLE;
                appendLe16(&payload, cycleId);
                payload.append(static_cast<char>(0x00)); /* pause_reason=0，表示用户按下暂停。 */
                payload.append(static_cast<char>(0x01)); /* pause_mode=1，表示尽量进入安全静止点。 */
            }
        } else if (normalizedAction == QStringLiteral("resume")) {
            if (!m_f4AutoPaused || cycleId == 0U) {
                rejectText = QStringLiteral("停止或空闲状态不能继续，请按开始创建新检测流程");
            } else {
                command = BINARY_PROTOCOL_CMD_RESUME_CYCLE;
                appendLe16(&payload, cycleId);
                payload.append(static_cast<char>(0x00)); /* resume_mode=0，表示从暂停点继续。 */
            }
        } else if (normalizedAction == QStringLiteral("stop")) {
            /*
             * 停止按钮固定发送 cycle_id=0：
             *   1. F4 协议已经约定 cycle_id=0 表示无条件清理当前 active_cycle_id。
             *   2. MP157 可能因为 Qt 重启、上一条 STOP 超时、旧 ACK/NACK 延迟等原因不知道 F4 当前真实 cycle。
             *   3. 若这里继续携带 MP157 本地旧 cycle_id，F4 active_cycle_id 不一致时会 NACK，
             *      用户就会遇到“停止后马上开始下一轮仍被拒绝”的现场问题。
             */
            command = BINARY_PROTOCOL_CMD_STOP_CYCLE;
            appendLe16(&payload, static_cast<quint16>(0U));
            payload.append(static_cast<char>((m_f4AutoRunning || m_f4AutoPaused) ? 0x00 : 0x01));
            payload.append(static_cast<char>(0x00)); /* stop_level=0，表示普通停止而非急停。 */
            cycleId = 0U;
        } else {
            rejectText = QStringLiteral("未知自动流程动作：") + action;
        }

        if (!rejectText.isEmpty()) {
            emit f4AutoControlFinished(false, normalizedAction, cycleId, rejectText);
            return false;
        }

        const quint16 sequence = m_f4BinarySequence++;
        const QByteArray frame = buildF4BinaryFrame(command, sequence, payload);
        const QString dev = m_f4Device;
        const int baud = m_f4Baud;
        m_f4CommandRunning = true;

        QPointer<DeviceHealthController> self(this);
        QThread *workerThread = QThread::create([self, dev, baud, frame, normalizedAction, cycleId, command, sequence]() {
            QString detail;
            const bool ok = sendF4BinaryCommand(dev, baud, frame, command, sequence, cycleId, &detail);

            if (!self) {
                return;
            }

            QMetaObject::invokeMethod(self.data(),
                                      "handleF4AutoControlFinished",
                                      Qt::QueuedConnection,
                                      Q_ARG(bool, ok),
                                      Q_ARG(QString, normalizedAction),
                                      Q_ARG(quint16, cycleId),
                                      Q_ARG(QString, detail));
        });

        if (workerThread == nullptr) {
            m_f4CommandRunning = false;
            emit f4AutoControlFinished(false,
                                       normalizedAction,
                                       cycleId,
                                       QStringLiteral("F4自动流程命令线程创建失败"));
            return false;
        }

        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);
        workerThread->start();
        return true;
    }

    /*
     * requestAutoVisionLocate 的作用：
     *   请求 KMS overlay 进程在当前原始 YUYV 帧中定位零件，供首页自动流程做传送带闭环。
     *
     * 主要流程：
     *   1. 只允许 kms-overlay 后端使用该接口，因为 Qt 自身没有直接拿到 overlay 的原始帧。
     *   2. 后台线程通过 Unix socket 发送 `LOCATE`，避免 socket 超时阻塞 QML 触摸线程。
     *   3. 主线程解析 `OK LOCATE key=value...` 并通过 autoVisionLocateFinished 返回 QVariantMap。
     *
     * 返回值：
     *   true 表示后台定位请求已启动；false 表示后端不匹配、请求重入或线程创建失败。
     */
    Q_INVOKABLE bool requestAutoVisionLocate()
    {
        if (m_videoBackend != QString::fromLatin1(BACKEND_KMS_OVERLAY)) {
            QVariantMap emptyResult;

            emit autoVisionLocateFinished(false,
                                          emptyResult,
                                          QStringLiteral("当前视频后端不是 kms-overlay，无法读取 overlay 原始帧"));
            return false;
        }

        if (m_autoVisionLocateRunning) {
            return false;
        }

        m_autoVisionLocateRunning = true;

        QPointer<DeviceHealthController> self(this);
        const QString socketPath = m_overlaySocket;

        QThread *workerThread = QThread::create([self, socketPath]() {
            const QString reply = queryOverlayControlCommand(socketPath, QByteArrayLiteral("LOCATE\n"));

            if (!self) {
                return;
            }

            QMetaObject::invokeMethod(self.data(),
                                      "handleAutoVisionLocateReply",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, reply));
        });

        if (workerThread == nullptr) {
            QVariantMap emptyResult;

            m_autoVisionLocateRunning = false;
            emit autoVisionLocateFinished(false,
                                          emptyResult,
                                          QStringLiteral("自动视觉定位线程创建失败"));
            return false;
        }

        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);
        workerThread->start();
        return true;
    }

    /*
     * sendF4VisionPosition 的作用：
     *   把 overlay LOCATE 返回的零件坐标编码为 F4 `VISION_POS` 二进制命令。
     *
     * 关键说明：
     *   用户已经确认零件从画面上方进入，所以首版把 `center_y` 作为传送带控制轴，
     *   目标线固定为 `height / 2`。F4 侧继续用 `axis_px - target_px` 计算速度和方向。
     *
     * 参数：
     *   locateResult 是 autoVisionLocateFinished 返回的 QVariantMap。
     *
     * 返回值：
     *   true 表示后台串口发送任务已启动；false 表示流程未运行、坐标无效或串口忙。
     */
    Q_INVOKABLE bool sendF4VisionPosition(const QVariantMap &locateResult)
    {
        const quint16 cycleId = m_f4AutoCycleId;
        const int hasTarget = locateResult.value(QStringLiteral("has_target")).toInt();
        const int frameWidth = locateResult.value(QStringLiteral("width")).toInt();
        const int frameHeight = locateResult.value(QStringLiteral("height")).toInt();
        const int centerX = locateResult.value(QStringLiteral("center_x")).toInt();
        const int centerY = locateResult.value(QStringLiteral("center_y")).toInt();
        const int bboxX = locateResult.value(QStringLiteral("bbox_x")).toInt();
        const int bboxY = locateResult.value(QStringLiteral("bbox_y")).toInt();
        const int bboxW = locateResult.value(QStringLiteral("bbox_w")).toInt();
        const int bboxH = locateResult.value(QStringLiteral("bbox_h")).toInt();
        const int confidence = clampedInt(locateResult.value(QStringLiteral("confidence")).toInt(), 0, 100);
        const quint16 frameId = static_cast<quint16>(locateResult.value(QStringLiteral("frame_id")).toUInt());
        const int targetY = frameHeight / 2;
        const int errorY = centerY - targetY;
        const bool centered = (errorY >= -AUTO_VISION_CENTER_TOLERANCE_PX
                               && errorY <= AUTO_VISION_CENTER_TOLERANCE_PX);
        const quint32 captureMs = static_cast<quint32>(
                    QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFFLL);
        QByteArray payload;

        if (!m_f4AutoRunning || m_f4AutoPaused || cycleId == 0U) {
            emit f4VisionCommandFinished(false,
                                         QStringLiteral("VISION_POS"),
                                         cycleId,
                                         QStringLiteral("自动流程未运行，不能发送视觉坐标"));
            return false;
        }

        if (hasTarget != 1 || frameWidth <= 0 || frameHeight <= 0) {
            emit f4VisionCommandFinished(false,
                                         QStringLiteral("VISION_POS"),
                                         cycleId,
                                         QStringLiteral("视觉定位结果无目标或图像尺寸无效"));
            return false;
        }

        appendLe16(&payload, cycleId);                                      /* cycle_id：归属当前首页自动流程。 */
        appendLe16(&payload, frameId);                                      /* frame_id：overlay 当前帧序号低 16 位。 */
        payload.append(static_cast<char>(centered ? 0x03U : 0x01U));        /* flags bit0=坐标有效，bit1=已进入中心死区。 */
        payload.append(static_cast<char>(0x00U));                           /* part_type=0，运动阶段只定位不分类。 */
        appendLeI16(&payload, centerY);                                     /* axis_px：上方来料时沿传送带方向使用 Y 坐标。 */
        appendLeI16(&payload, targetY);                                     /* target_px：当前帧高度的一半。 */
        appendLeI16(&payload, clampedInt(centerX, -32768, 32767));          /* center_x_px：零件中心 X。 */
        appendLeI16(&payload, clampedInt(centerY, -32768, 32767));          /* center_y_px：零件中心 Y。 */
        appendLeI16(&payload, clampedInt(bboxX, -32768, 32767));            /* bbox_x_px：定位框左上角 X。 */
        appendLeI16(&payload, clampedInt(bboxY, -32768, 32767));            /* bbox_y_px：定位框左上角 Y。 */
        appendLeI16(&payload, clampedInt(bboxW, -32768, 32767));            /* bbox_w_px：定位框宽度。 */
        appendLeI16(&payload, clampedInt(bboxH, -32768, 32767));            /* bbox_h_px：定位框高度。 */
        payload.append(static_cast<char>(confidence));                      /* confidence：overlay 定位置信度 0~100。 */
        payload.append(static_cast<char>(0x00U));                           /* reserved：协议保留字段首版填 0。 */
        appendLe32(&payload, captureMs);                                    /* capture_ms：MP157 当前毫秒计数低 32 位。 */

        return startF4VisionCommand(QStringLiteral("VISION_POS"),
                                    BINARY_PROTOCOL_CMD_VISION_POS,
                                    cycleId,
                                    payload);
    }

    /*
     * sendF4VisionLost 的作用：
     *   在当前帧未找到零件或相机异常时通知 F4，避免 F4 使用上一帧坐标继续运动。
     *
     * 参数：
     *   reason 是视觉丢失原因：1=未找到目标，2=多目标，3=置信度低，4=相机离线。
     *
     * 返回值：
     *   true 表示后台串口发送任务已启动；false 表示 reason 非法、流程未运行或串口忙。
     */
    Q_INVOKABLE bool sendF4VisionLost(int reason)
    {
        const quint16 cycleId = m_f4AutoCycleId;
        QByteArray payload;

        if (!m_f4AutoRunning || m_f4AutoPaused || cycleId == 0U) {
            emit f4VisionCommandFinished(false,
                                         QStringLiteral("VISION_LOST"),
                                         cycleId,
                                         QStringLiteral("自动流程未运行，不能发送视觉丢失"));
            return false;
        }

        if (reason < 1 || reason > 4) {
            emit f4VisionCommandFinished(false,
                                         QStringLiteral("VISION_LOST"),
                                         cycleId,
                                         QStringLiteral("视觉丢失 reason 必须是 1~4"));
            return false;
        }

        appendLe16(&payload, cycleId);                /* cycle_id：归属当前首页自动流程。 */
        appendLe16(&payload, 0U);                     /* frame_id：接口未传帧号时填 0，表示最近一次定位失败。 */
        payload.append(static_cast<char>(reason));    /* reason：1 未找到、2 多目标、3 低置信度、4 相机离线。 */
        payload.append(static_cast<char>(0U));        /* confidence：丢失时填 0。 */
        appendLe16(&payload, 0U);                     /* ms_since_seen：首版由 QML 节流，不在 C++ 里累计。 */

        return startF4VisionCommand(QStringLiteral("VISION_LOST"),
                                    BINARY_PROTOCOL_CMD_VISION_LOST,
                                    cycleId,
                                    payload);
    }

    /*
     * sendF4BeltStopCentered 的作用：
     *   当 QML 连续多帧确认零件已处于中心 ROI 时，要求 F4 停止传送带并保持静止。
     *
     * 参数：
     *   frameId 是触发居中停止的 overlay 帧号；协议只取低 16 位。
     *
     * 返回值：
     *   true 表示后台串口发送任务已启动；false 表示流程未运行或串口忙。
     */
    Q_INVOKABLE bool sendF4BeltStopCentered(int frameId)
    {
        const quint16 cycleId = m_f4AutoCycleId;
        QByteArray payload;

        if (!m_f4AutoRunning || m_f4AutoPaused || cycleId == 0U) {
            emit f4VisionCommandFinished(false,
                                         QStringLiteral("BELT_STOP_CENTERED"),
                                         cycleId,
                                         QStringLiteral("自动流程未运行，不能发送居中停止"));
            return false;
        }

        appendLe16(&payload, cycleId);                                      /* cycle_id：归属当前首页自动流程。 */
        appendLe16(&payload, static_cast<quint16>(frameId));                /* frame_id：触发停止的视觉帧号低 16 位。 */
        payload.append(static_cast<char>(0x00U));                           /* reason=0，表示进入中心 ROI。 */
        appendLe16(&payload, static_cast<quint16>(AUTO_VISION_CENTER_HOLD_MS)); /* hold_ms：建议静止 2 秒再检测。 */
        payload.append(static_cast<char>(0x00U));                           /* reserved：协议保留字段首版填 0。 */

        return startF4VisionCommand(QStringLiteral("BELT_STOP_CENTERED"),
                                    BINARY_PROTOCOL_CMD_BELT_STOP_CENTERED,
                                    cycleId,
                                    payload);
    }

    /*
     * requestF4ArmInspectionFlow 的作用：
     *   在 Z 轴回升完成后通知 F4/ESP32S3 执行“抓取到称重、转移到电感”的机械臂检测流程。
     *
     * 主要流程：
     *   1. 从模型 RESULT 行解析 good/bad/review、置信度、模型耗时和零件类别。
     *   2. 先发送 MODEL_READY，让 F4 缓存模型结果，但不触发最终分拣。
     *   3. 再发送 ARM_JOB_START，让 F4 通知 ESP32S3 机械臂把零件放到称重和电感模块。
     *   4. 后台线程持续读取 F4 主动 WEIGHT_RESULT/LDC_RESULT，并逐帧 ACK，收齐后回传 JSON 给 QML。
     *
     * 参数：
     *   modelResultText 是 storageController 检测完成返回的 RESULT 行。
     *   activeFrameTimeoutMs 是参数页配置的 F4 主动结果帧等待窗口，单位 ms。
     *
     * 返回值：
     *   true 表示长流程后台线程已启动；false 表示当前 cycle、串口或模型结果不满足启动条件。
     */
    Q_INVOKABLE bool requestF4ArmInspectionFlow(const QString &modelResultText,
                                                int activeFrameTimeoutMs)
    {
        const quint16 cycleId = m_f4AutoCycleId;
        const quint8 modelResult = modelResultCodeFromText(modelResultText);
        const quint8 finalBinHint = finalBinFromModelResult(modelResult);
        const quint8 top1Confidence = confidencePercentFromModelText(modelResultText);
        const quint16 modelMs = static_cast<quint16>(
                    clampedInt(tokenValue(modelResultText, QStringLiteral("total_time_ms")).toInt(), 0, 65535));
        const quint8 partType = partTypeCodeFromModelText(modelResultText);
        const quint8 defectType = defectTypeCodeFromModelText(modelResultText);
        const quint16 modelSequence = m_f4BinarySequence++;
        const quint16 armSequence = m_f4BinarySequence++;
        const quint16 ackSequenceBase = m_f4BinarySequence;
        const int armResultTimeoutMs = clampedInt(activeFrameTimeoutMs,
                                                  F4_ARM_ACTIVE_FRAME_TIMEOUT_MIN_MS,
                                                  F4_ARM_ACTIVE_FRAME_TIMEOUT_MAX_MS);
        QByteArray modelPayload;
        QByteArray armPayload;

        if (m_f4CommandRunning || m_f4ArmFlowRunning || m_f4FinalSortRunning) {
            emit f4ArmInspectionFlowFinished(false,
                                             QStringLiteral("F4串口已有命令或机械臂流程正在运行"),
                                             QString(),
                                             QString(),
                                             QString());
            return false;
        }

        if (m_f4ProbeRunning) {
            emit f4ArmInspectionFlowFinished(false,
                                             QStringLiteral("F4状态刷新仍在进行，请稍后再启动机械臂检测流程"),
                                             QString(),
                                             QString(),
                                             QString());
            return false;
        }

        if (!m_f4AutoRunning || m_f4AutoPaused || cycleId == 0U) {
            emit f4ArmInspectionFlowFinished(false,
                                             QStringLiteral("自动检测 cycle 未运行，不能启动机械臂检测流程"),
                                             QString(),
                                             QString(),
                                             QString());
            return false;
        }

        if (modelResult == 0U) {
            emit f4ArmInspectionFlowFinished(false,
                                             QStringLiteral("模型结果无法解析，不能下发 MODEL_READY"),
                                             QString(),
                                             QString(),
                                             QString());
            return false;
        }

        /*
         * 一次长流程最多需要 ACK WEIGHT_RESULT、ACK LDC_RESULT 和若干保留 ACK。
         * 这里提前预留 8 个 sequence，避免长线程里复用主线程随后发出的普通命令序号。
         */
        m_f4BinarySequence = static_cast<quint16>(m_f4BinarySequence + 8U);

        appendLe16(&modelPayload, cycleId);              /* cycle_id：归属当前自动检测件。 */
        modelPayload.append(static_cast<char>(modelResult)); /* model_result：1 good、2 bad、3 review。 */
        modelPayload.append(static_cast<char>(partType));    /* part_type：由模型类别映射，未知为 0。 */
        modelPayload.append(static_cast<char>(defectType));  /* defect_type：当前先用轻量枚举，未知为 0。 */
        modelPayload.append(static_cast<char>(top1Confidence)); /* top1_confidence：0~100 百分比。 */
        appendLe16(&modelPayload, 0U);                    /* image_seq：首版没有单独图像序号，填 0。 */
        appendLe16(&modelPayload, modelMs);               /* model_ms：分类+UNet 本地耗时。 */
        appendLe16(&modelPayload, 0x0001U);               /* option_bits bit0=图片和模型结果已落 SD 卡。 */

        const quint16 jobId = cycleId;
        appendLe16(&armPayload, cycleId);                 /* cycle_id：机械臂任务绑定当前检测件。 */
        appendLe16(&armPayload, jobId);                   /* job_id：首版直接复用 cycle_id，便于对账。 */
        armPayload.append(static_cast<char>(0U));         /* job_profile=0，使用默认动作组。 */
        armPayload.append(static_cast<char>(partType));   /* part_type：传给 F4/ESP32S3 做动作细节选择。 */
        armPayload.append(static_cast<char>(finalBinHint)); /* final_bin_hint：只作提示，最终以 FINAL_SORT_RESULT 为准。 */
        appendLe16(&armPayload, 0x0007U);                 /* option_bits bit0称重、bit1电感、bit2最终分拣均启用。 */

        const QByteArray modelFrame = buildF4BinaryFrame(BINARY_PROTOCOL_CMD_MODEL_READY,
                                                         modelSequence,
                                                         modelPayload);
        const QByteArray armFrame = buildF4BinaryFrame(BINARY_PROTOCOL_CMD_ARM_JOB_START,
                                                       armSequence,
                                                       armPayload);
        const QString dev = m_f4Device;
        const int baud = m_f4Baud;

        m_f4CommandRunning = true;
        m_f4ArmFlowRunning = true;
        m_f4LastArmJobId = jobId;
        m_f4LastModelResult = modelResult;
        m_f4LastFinalBin = finalBinHint;

        QPointer<DeviceHealthController> self(this);
        QThread *workerThread = QThread::create([self,
                                                 dev,
                                                 baud,
                                                 modelFrame,
                                                 armFrame,
                                                 modelSequence,
                                                 armSequence,
                                                 ackSequenceBase,
                                                 cycleId,
                                                 jobId,
                                                 armResultTimeoutMs]() {
            QString detail;
            QString weightContextJson;
            QString ldcContextJson;
            QString f4FlowContextJson;
            const bool ok = runF4ArmInspectionFlow(dev,
                                                   baud,
                                                   modelFrame,
                                                   modelSequence,
                                                   armFrame,
                                                   armSequence,
                                                   ackSequenceBase,
                                                   cycleId,
                                                   jobId,
                                                   armResultTimeoutMs,
                                                   &detail,
                                                   &weightContextJson,
                                                   &ldcContextJson,
                                                   &f4FlowContextJson);

            if (!self) {
                return;
            }

            QMetaObject::invokeMethod(self.data(),
                                      "handleF4ArmInspectionFlowFinished",
                                      Qt::QueuedConnection,
                                      Q_ARG(bool, ok),
                                      Q_ARG(QString, detail),
                                      Q_ARG(QString, weightContextJson),
                                      Q_ARG(QString, ldcContextJson),
                                      Q_ARG(QString, f4FlowContextJson));
        });

        if (workerThread == nullptr) {
            m_f4CommandRunning = false;
            m_f4ArmFlowRunning = false;
            emit f4ArmInspectionFlowFinished(false,
                                             QStringLiteral("F4机械臂检测流程线程创建失败"),
                                             QString(),
                                             QString(),
                                             QString());
            return false;
        }

        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);
        workerThread->start();
        return true;
    }

    /*
     * requestF4FinalSortResult 的作用：
     *   MP157 完整上传图片、模型、重量和电感数据后，再通知 F4 执行最终分拣。
     *
     * 主要流程：
     *   1. 上传成功时按云端最终结果映射良品盘、不良品盘或待复核盘。
     *   2. 上传失败但本地历史已经保存时，强制 final_result=review、final_bin=待复核盘。
     *   3. 发送 FINAL_SORT_RESULT，等待 F4 ACK 后继续等待 CYCLE_DONE 主动帧。
     *
     * 参数：
     *   cloudResult 是本次云端记录 result，取值 good/bad/review。
     *   uploadResultText 是上传脚本返回的一行状态，必须包含成功语义。
     *   activeFrameTimeoutMs 是参数页配置的 CYCLE_DONE 等待窗口，单位 ms。
     *
     * 返回值：
     *   true 表示最终分拣线程已启动；false 表示上传未成功或 F4 当前状态不允许。
     */
    Q_INVOKABLE bool requestF4FinalSortResult(const QString &cloudResult,
                                              const QString &uploadResultText,
                                              int activeFrameTimeoutMs)
    {
        const quint16 cycleId = m_f4AutoCycleId;
        const quint16 jobId = m_f4LastArmJobId;
        const bool uploadSucceeded = isUploadStatusSuccess(uploadResultText);
        const quint8 uploadStatusCode = uploadSucceeded ? 1U : 2U;
        const quint8 finalResult = uploadSucceeded ? modelResultCodeFromCloudResult(cloudResult) : 3U;
        const quint8 finalBin = uploadSucceeded ? finalBinFromModelResult(finalResult) : 3U;
        const quint16 finalSequence = m_f4BinarySequence++;
        const quint16 ackSequenceBase = m_f4BinarySequence;
        const int armResultTimeoutMs = clampedInt(activeFrameTimeoutMs,
                                                  F4_ARM_ACTIVE_FRAME_TIMEOUT_MIN_MS,
                                                  F4_ARM_ACTIVE_FRAME_TIMEOUT_MAX_MS);
        QByteArray payload;

        if (m_f4CommandRunning || m_f4ArmFlowRunning || m_f4FinalSortRunning) {
            emit f4FinalSortFinished(false,
                                     QStringLiteral("F4串口已有命令或机械臂流程正在运行"),
                                     QString());
            return false;
        }

        if (cycleId == 0U || jobId == 0U || finalResult == 0U || finalBin == 0U) {
            emit f4FinalSortFinished(false,
                                     QStringLiteral("最终分拣缺少 cycle/job/model 结果上下文"),
                                     QString());
            return false;
        }

        m_f4BinarySequence = static_cast<quint16>(m_f4BinarySequence + 4U);

        appendLe16(&payload, cycleId);                       /* cycle_id：当前检测件。 */
        appendLe16(&payload, jobId);                         /* job_id：前一阶段 ARM_JOB_START 的任务号。 */
        payload.append(static_cast<char>(finalResult));      /* final_result：1 good、2 bad、3 review。 */
        payload.append(static_cast<char>(finalBin));         /* final_bin：1 良品盘、2 不良品盘、3 待复核盘。 */
        payload.append(static_cast<char>(uploadStatusCode)); /* upload_status=1 表示云端成功，2 表示上传失败但本地已保存。 */
        payload.append(static_cast<char>((finalResult == 3U || !uploadSucceeded) ? 50U : 100U)); /* final_confidence：复核件保守填 50。 */
        appendLe16(&payload, uploadSucceeded ? 0x0001U : 0x0003U); /* bit0=允许分拣，bit1=上传失败待复核。 */

        const QByteArray frame = buildF4BinaryFrame(BINARY_PROTOCOL_CMD_FINAL_SORT_RESULT,
                                                    finalSequence,
                                                    payload);
        const QString dev = m_f4Device;
        const int baud = m_f4Baud;

        m_f4CommandRunning = true;
        m_f4FinalSortRunning = true;
        m_f4LastFinalBin = finalBin;

        QPointer<DeviceHealthController> self(this);
        QThread *workerThread = QThread::create([self,
                                                 dev,
                                                 baud,
                                                 frame,
                                                 finalSequence,
                                                 ackSequenceBase,
                                                 cycleId,
                                                 jobId,
                                                 armResultTimeoutMs]() {
            QString detail;
            QString cycleDoneContextJson;
            const bool ok = runF4FinalSortAndWaitCycleDone(dev,
                                                           baud,
                                                           frame,
                                                           finalSequence,
                                                           ackSequenceBase,
                                                           cycleId,
                                                           jobId,
                                                           armResultTimeoutMs,
                                                           &detail,
                                                           &cycleDoneContextJson);

            if (!self) {
                return;
            }

            QMetaObject::invokeMethod(self.data(),
                                      "handleF4FinalSortFinished",
                                      Qt::QueuedConnection,
                                      Q_ARG(bool, ok),
                                      Q_ARG(QString, detail),
                                      Q_ARG(QString, cycleDoneContextJson));
        });

        if (workerThread == nullptr) {
            m_f4CommandRunning = false;
            m_f4FinalSortRunning = false;
            emit f4FinalSortFinished(false,
                                     QStringLiteral("F4最终分拣线程创建失败"),
                                     QString());
            return false;
        }

        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);
        workerThread->start();
        return true;
    }

signals:
    /* networkStatusChanged 通知 QML 网络状态和颜色已更新。 */
    void networkStatusChanged();

    /* cameraStatusChanged 通知 QML 摄像头状态和颜色已更新。 */
    void cameraStatusChanged();

    /* f4StatusChanged 通知 QML F4 接入状态和颜色已更新。 */
    void f4StatusChanged();

    /* cloudStatusChanged 通知 QML 云端状态和颜色已更新。 */
    void cloudStatusChanged();

    /* sdcardStatusChanged 通知 QML SD 卡状态和颜色已更新。 */
    void sdcardStatusChanged();

    /* locationStatusChanged 通知 QML 高德 IP 省份定位状态、显示文本和颜色已更新。 */
    void locationStatusChanged();

    /* detailTextChanged 通知 QML 最近检测详情已更新。 */
    void detailTextChanged();

    /* f4CommandFinished 通知 QML 称重标定命令发送完成，并带回成功/失败详情。 */
    void f4CommandFinished(bool ok, const QString &detail);

    /* f4ManualCommandFinished 通知 QML 手动控制页的 F4 命令发送完成，并带回命令名和回复详情。 */
    void f4ManualCommandFinished(bool ok, const QString &command, const QString &detail);

    /* f4StepperSettingsFinished 通知 QML 步进电机参数下发完成，并带回 ACK/NACK 详情。 */
    void f4StepperSettingsFinished(bool ok, const QString &detail);

    /* f4AutoControlFinished 通知 QML 首页自动流程命令发送完成，并带回动作、流程号和 ACK/NACK 详情。 */
    void f4AutoControlFinished(bool ok, const QString &action, quint16 cycleId, const QString &detail);

    /* autoVisionLocateFinished 通知 QML overlay LOCATE 定位完成，并返回零件坐标或失败原因。 */
    void autoVisionLocateFinished(bool ok, const QVariantMap &result, const QString &detail);

    /* f4VisionCommandFinished 通知 QML 视觉闭环命令发送完成，并带回 ACK/NACK 详情。 */
    void f4VisionCommandFinished(bool ok, const QString &action, quint16 cycleId, const QString &detail);

    /* f4ActuatorCommandFinished 通知 QML 执行器位置运动或停止命令完成，并带回 ACK/NACK 详情。 */
    void f4ActuatorCommandFinished(bool ok, const QString &action, quint16 cycleId, const QString &detail);

    /* f4ArmInspectionFlowFinished 通知 QML 已收齐称重和电感上下文，可以启动云端完整上传。 */
    void f4ArmInspectionFlowFinished(bool ok,
                                     const QString &detail,
                                     const QString &weightContextJson,
                                     const QString &ldcContextJson,
                                     const QString &f4FlowContextJson);

    /* f4FinalSortFinished 通知 QML 最终分拣命令和 CYCLE_DONE 是否完成。 */
    void f4FinalSortFinished(bool ok, const QString &detail, const QString &cycleDoneContextJson);

private slots:
    /*
     * handleNetworkProcessFinished 的作用：
     *   接收 `4g-ppp test` 的异步结果，只有返回 0 才认为网络在线。
     */
    void handleNetworkProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
    {
        const QString output = QString::fromUtf8(m_networkProcess.readAll()).trimmed();

        m_networkTimeout.stop();
        m_networkProbeRunning = false;

        if (m_networkProbeTimedOut) {
            m_networkProbeTimedOut = false;
            return;
        }

        if (exitStatus == QProcess::NormalExit && exitCode == 0) {
            setNetworkStatus(QStringLiteral("在线"), QStringLiteral("#35d07f"));
            setDetailText(QStringLiteral("4G 联网测试通过"));
        } else {
            setNetworkStatus(QStringLiteral("离线"), QStringLiteral("#ef5b5b"));
            setDetailText(QStringLiteral("4G 测试未通过：") + compactText(output, 96));
        }
    }

    /*
     * handleNetworkProbeTimeout 的作用：
     *   网络测试超过短超时后主动结束进程，避免 4G 命令长时间占用资源。
     */
    void handleNetworkProbeTimeout()
    {
        if (m_networkProcess.state() != QProcess::NotRunning) {
            m_networkProbeTimedOut = true;
            m_networkProcess.kill();
        } else {
            m_networkProbeRunning = false;
        }
        setNetworkStatus(QStringLiteral("超时"), QStringLiteral("#ef5b5b"));
        setDetailText(QStringLiteral("4G 联网测试超时"));
    }

    /*
     * handleNetworkProcessError 的作用：
     *   异步接收 4G 测试进程启动失败等错误。
     *
     * 关键说明：
     *   不在 startNetworkProbe() 中等待进程启动，避免健康检测定时器让 QML 主线程短暂停顿；
     *   Qt 后续通过该信号告诉我们启动失败，再更新界面状态。
     */
    void handleNetworkProcessError(QProcess::ProcessError error)
    {
        if (error != QProcess::FailedToStart) {
            return;
        }

        m_networkTimeout.stop();
        m_networkProbeRunning = false;
        m_networkProbeTimedOut = false;
        setNetworkStatus(QStringLiteral("未安装"), QStringLiteral("#f4b942"));
        setDetailText(QStringLiteral("无法启动 4G 测试命令：") + m_networkProcess.errorString());
    }

    /*
     * handleLocationProcessFinished 的作用：
     *   接收开机单次 `4g-location once` 输出的 key=value 状态，并更新 Qt 位置显示。
     *
     * 主要流程：
     *   1. 停止定位超时定时器并释放忙标志。
     *   2. 解析脚本输出中的 state/display/short_display/detail 字段。
     *   3. 只有 state=ip_ok 时显示高德 IP 定位成功；其它状态转成缺 Key、失败或未定位。
     *
     * 参数：
     *   exitCode 是 4g-location 退出码。
     *   exitStatus 表示进程是否正常退出。
     *
     * 返回值：
     *   无返回值；结果通过 locationStatusChanged 通知 QML。
     */
    void handleLocationProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
    {
        const QString output = QString::fromUtf8(m_locationProcess.readAll()).trimmed();

        m_locationTimeout.stop();
        m_locationProbeRunning = false;

        if (m_locationProbeTimedOut) {
            m_locationProbeTimedOut = false;
            return;
        }

        const QMap<QString, QString> values = parseKeyValueOutput(output);
        const QString state = values.value(QStringLiteral("state"));
        const QString display = values.value(QStringLiteral("display"), QStringLiteral("未定位"));
        const QString shortDisplay = values.value(QStringLiteral("short_display"), display);
        const QString detail = values.value(QStringLiteral("detail"), compactText(output, 96));

        if (exitStatus == QProcess::NormalExit && exitCode == 0 && state == QStringLiteral("ip_ok")) {
            /*
             * state=ip_ok 表示 4g-location 已用高德 IP 定位拿到省份。
             * 当前 UI 只显示省份，不显示城市、区县、经纬度或卫星数，避免把运营商出口城市误当现场位置。
             */
            setLocationStatus(QStringLiteral("IP定位"),
                              QStringLiteral("#35d07f"),
                              display,
                              shortDisplay);
            setDetailText(QStringLiteral("IP定位：") + display);
            return;
        }

        if (state == QStringLiteral("no_key")) {
            setLocationStatus(QStringLiteral("缺少Key"),
                              QStringLiteral("#f4b942"),
                              display,
                              shortDisplay);
        } else if (state == QStringLiteral("ip_failed")) {
            setLocationStatus(QStringLiteral("定位失败"),
                              QStringLiteral("#ef5b5b"),
                              display,
                              shortDisplay);
        } else {
            setLocationStatus(QStringLiteral("未定位"),
                              QStringLiteral("#f4b942"),
                              display,
                              shortDisplay);
        }

        setDetailText(QStringLiteral("定位未完成：") + detail);
    }

    /*
     * handleLocationProbeTimeout 的作用：
     *   定位脚本超过短超时后主动结束，避免高德 IP 定位请求拖住健康刷新。
     */
    void handleLocationProbeTimeout()
    {
        if (m_locationProcess.state() != QProcess::NotRunning) {
            m_locationProbeTimedOut = true;
            m_locationProcess.kill();
        } else {
            m_locationProbeRunning = false;
        }
        setLocationStatus(QStringLiteral("超时"),
                          QStringLiteral("#ef5b5b"),
                          m_locationDisplayText,
                          m_locationShortText);
        setDetailText(QStringLiteral("4G IP 定位超时"));
    }

    /*
     * handleLocationProcessError 的作用：
     *   异步接收 4G 定位脚本启动失败错误，避免 startLocationProbe() 阻塞 QML 主线程。
     */
    void handleLocationProcessError(QProcess::ProcessError error)
    {
        if (error != QProcess::FailedToStart) {
            return;
        }

        m_locationTimeout.stop();
        m_locationProbeRunning = false;
        m_locationProbeTimedOut = false;
        setLocationStatus(QStringLiteral("未安装"),
                          QStringLiteral("#f4b942"),
                          QStringLiteral("未定位"),
                          QStringLiteral("未定位"));
        setDetailText(QStringLiteral("无法启动 4G 定位命令：") + m_locationProcess.errorString());
    }

    /*
     * handleCloudProcessFinished 的作用：
     *   接收云端 health curl 结果，只有 HTTP 请求返回成功才显示已连接。
     */
    void handleCloudProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
    {
        const QString output = QString::fromUtf8(m_cloudProcess.readAll()).trimmed();

        m_cloudTimeout.stop();
        m_cloudProbeRunning = false;

        if (m_cloudProbeTimedOut) {
            m_cloudProbeTimedOut = false;
            return;
        }

        if (exitStatus == QProcess::NormalExit && exitCode == 0) {
            setCloudStatus(QStringLiteral("已连接"), QStringLiteral("#35d07f"));
        } else {
            setCloudStatus(QStringLiteral("离线"), QStringLiteral("#f4b942"));
            setDetailText(QStringLiteral("云端 health 未通过：") + compactText(output, 96));
        }
    }

    /*
     * handleCloudProbeTimeout 的作用：
     *   云端 health 超时后主动结束 curl，避免弱网时卡住状态刷新。
     */
    void handleCloudProbeTimeout()
    {
        if (m_cloudProcess.state() != QProcess::NotRunning) {
            m_cloudProbeTimedOut = true;
            m_cloudProcess.kill();
        } else {
            m_cloudProbeRunning = false;
        }
        setCloudStatus(QStringLiteral("超时"), QStringLiteral("#f4b942"));
    }

    /*
     * handleCloudProcessError 的作用：
     *   异步接收 curl 启动失败错误。
     *
     * 关键说明：
     *   云端 health 探测不能因为 curl 不存在或启动异常阻塞界面，所以启动失败只通过信号回写状态。
     */
    void handleCloudProcessError(QProcess::ProcessError error)
    {
        if (error != QProcess::FailedToStart) {
            return;
        }

        m_cloudTimeout.stop();
        m_cloudProbeRunning = false;
        m_cloudProbeTimedOut = false;
        setCloudStatus(QStringLiteral("待确认"), QStringLiteral("#f4b942"));
        setDetailText(QStringLiteral("无法启动云端 health 命令：") + m_cloudProcess.errorString());
    }

    /*
     * handleOverlayProbeFinished 的作用：
     *   接收后台 overlay STATUS 查询结果，并在 Qt 主线程更新相机状态。
     *
     * 参数：
     *   reply 是后台线程读取到的 `OK STATUS ...` 或 `ERR ...` 文本。
     *
     * 返回值：
     *   无返回值。
     */
    void handleOverlayProbeFinished(const QString &reply)
    {
        m_overlayProbeRunning = false;
        applyOverlayStatusReply(reply);
    }

    /*
     * handleAutoVisionLocateReply 的作用：
     *   接收后台 overlay `LOCATE` 回复，解析成 QML 可直接访问的 QVariantMap。
     *
     * 主要流程：
     *   1. 释放 m_autoVisionLocateRunning，允许下一次 100ms 定位请求继续执行。
     *   2. 校验回复必须以 `OK LOCATE` 开头；socket 或 overlay 错误直接通知 QML。
     *   3. 逐项提取 has_target、frame_id、width、height、center、bbox、confidence 和 ring。
     *
     * 参数：
     *   reply 是后台线程从 overlay 控制 socket 读取的一行文本。
     *
     * 返回值：
     *   无返回值；通过 autoVisionLocateFinished 通知 QML。
     */
    void handleAutoVisionLocateReply(const QString &reply)
    {
        QVariantMap result;

        m_autoVisionLocateRunning = false;

        if (!reply.startsWith(QStringLiteral("OK LOCATE "))) {
            emit autoVisionLocateFinished(false, result, reply.isEmpty()
                                          ? QStringLiteral("overlay LOCATE 无回复")
                                          : reply);
            return;
        }

        result.insert(QStringLiteral("has_target"), tokenValue(reply, QStringLiteral("has_target")).toInt());
        result.insert(QStringLiteral("frame_id"), tokenValue(reply, QStringLiteral("frame_id")).toUInt());
        result.insert(QStringLiteral("width"), tokenValue(reply, QStringLiteral("width")).toInt());
        result.insert(QStringLiteral("height"), tokenValue(reply, QStringLiteral("height")).toInt());
        result.insert(QStringLiteral("center_x"), tokenValue(reply, QStringLiteral("center_x")).toInt());
        result.insert(QStringLiteral("center_y"), tokenValue(reply, QStringLiteral("center_y")).toInt());
        result.insert(QStringLiteral("bbox_x"), tokenValue(reply, QStringLiteral("bbox_x")).toInt());
        result.insert(QStringLiteral("bbox_y"), tokenValue(reply, QStringLiteral("bbox_y")).toInt());
        result.insert(QStringLiteral("bbox_w"), tokenValue(reply, QStringLiteral("bbox_w")).toInt());
        result.insert(QStringLiteral("bbox_h"), tokenValue(reply, QStringLiteral("bbox_h")).toInt());
        result.insert(QStringLiteral("confidence"), tokenValue(reply, QStringLiteral("confidence")).toInt());
        result.insert(QStringLiteral("has_ring"), tokenValue(reply, QStringLiteral("ring")).toInt());
        result.insert(QStringLiteral("diag"), tokenValue(reply, QStringLiteral("diag")).toInt());
        result.insert(QStringLiteral("roi_y"), tokenValue(reply, QStringLiteral("roi_y")).toInt());
        result.insert(QStringLiteral("roi_h"), tokenValue(reply, QStringLiteral("roi_h")).toInt());
        result.insert(QStringLiteral("thresholds"), tokenValue(reply, QStringLiteral("thr")));
        result.insert(QStringLiteral("cand_box"), tokenValue(reply, QStringLiteral("cand_box")));
        result.insert(QStringLiteral("cand_area"), tokenValue(reply, QStringLiteral("cand_area")).toInt());
        result.insert(QStringLiteral("cand_density"), tokenValue(reply, QStringLiteral("cand_density")).toInt());
        result.insert(QStringLiteral("cand_conf"), tokenValue(reply, QStringLiteral("cand_conf")).toInt());
        result.insert(QStringLiteral("cand_ring"), tokenValue(reply, QStringLiteral("cand_ring")).toInt());

        emit autoVisionLocateFinished(true, result, reply);
    }

    /*
     * handleF4ProbeFinished 的作用：
     *   接收后台 F4 串口握手结果，并在 Qt 主线程更新 F4 接入状态。
     *
     * 参数：
     *   ok 为 true 表示收到匹配 HEARTBEAT 的二进制 ACK。
     *   detail 保存 ACK/NACK 解析结果、FAULT_REPORT 摘要或串口失败原因。
     *
     * 返回值：
     *   无返回值。
     */
    void handleF4ProbeFinished(bool ok, const QString &detail)
    {
        m_f4ProbeRunning = false;
        if (ok) {
            setF4Status(QStringLiteral("接入"), QStringLiteral("#35d07f"));
            setDetailText(QStringLiteral("F4 串口握手成功：") + detail);
        } else {
            setF4Status(QStringLiteral("待接入"), QStringLiteral("#f4b942"));
            setDetailText(QStringLiteral("F4 待接入：") + detail);
        }
    }

    /*
     * handleF4CommandFinished 的作用：
     *   接收后台 F4 命令发送结果，并把结果同步给 QML 标定弹窗和顶部健康详情。
     *
     * 参数：
     *   ok 为 true 表示命令已写入串口且收到匹配的二进制 ACK。
     *   detail 是 ACK/NACK 二进制解析结果、FAULT_REPORT 摘要或失败原因。
     *
     * 返回值：
     *   无返回值；函数会释放发送忙标志并发出 f4CommandFinished 信号。
     */
    void handleF4CommandFinished(bool ok, const QString &detail)
    {
        m_f4CommandRunning = false;

        if (ok) {
            setF4Status(QStringLiteral("接入"), QStringLiteral("#35d07f"));
            setDetailText(QStringLiteral("F4 命令完成：") + detail);
        } else {
            setDetailText(QStringLiteral("F4 命令失败：") + detail);
        }

        emit f4CommandFinished(ok, detail);
    }

    /*
     * handleF4ManualCommandFinished 的作用：
     *   接收后台 F4 手动控制命令结果，并把命令名和回复详情同步给 QML 手动控制页。
     *
     * 参数：
     *   ok 为 true 表示命令已写入串口且收到匹配的二进制 ACK 或 STATUS_REPORT。
     *   command 是本次按钮语义名称，例如 BELT_MANUAL_SCAN；底层不会把该字符串写给 F4。
     *   detail 是 ACK/NACK/STATUS_REPORT 二进制解析结果或失败原因。
     *
     * 返回值：
     *   无返回值；函数会释放发送忙标志并发出 f4ManualCommandFinished 信号。
     */
    void handleF4ManualCommandFinished(bool ok, const QString &command, const QString &detail)
    {
        m_f4CommandRunning = false;

        if (ok) {
            setF4Status(QStringLiteral("接入"), QStringLiteral("#35d07f"));
            setDetailText(QStringLiteral("F4 手动命令完成：") + command + QStringLiteral(" ") + detail);
        } else {
            setDetailText(QStringLiteral("F4 手动命令失败：") + command + QStringLiteral(" ") + detail);
        }

        emit f4ManualCommandFinished(ok, command, detail);
    }

    /*
     * handleF4ArmInspectionFlowFinished 的作用：
     *   接收 MODEL_READY/ARM_JOB_START 后台长流程结果，释放串口互斥并把称重、电感上下文发给 QML。
     *
     * 参数：
     *   ok 表示是否已经收到本轮 WEIGHT_RESULT 和 LDC_RESULT。
     *   detail 是串口 ACK、主动帧和等待过程摘要。
     *   weightContextJson 是称重结果 JSON，供完整上传写入 sensor_context.weighing。
     *   ldcContextJson 是电感结果 JSON，供完整上传写入 sensor_context.ldc1614_eddy_current。
     *   f4FlowContextJson 是 F4/ESP32S3 阶段流程 JSON，供云端追踪本轮动作链路。
     */
    void handleF4ArmInspectionFlowFinished(bool ok,
                                           const QString &detail,
                                           const QString &weightContextJson,
                                           const QString &ldcContextJson,
                                           const QString &f4FlowContextJson)
    {
        m_f4CommandRunning = false;
        m_f4ArmFlowRunning = false;

        if (ok) {
            setF4Status(QStringLiteral("接入"), QStringLiteral("#35d07f"));
            setDetailText(QStringLiteral("F4 机械臂检测流程完成：") + detail);
        } else {
            setDetailText(QStringLiteral("F4 机械臂检测流程失败：") + detail);
        }

        emit f4ArmInspectionFlowFinished(ok,
                                         detail,
                                         weightContextJson,
                                         ldcContextJson,
                                         f4FlowContextJson);
    }

    /*
     * handleF4FinalSortFinished 的作用：
     *   接收 FINAL_SORT_RESULT 后台长流程结果，释放串口互斥并把 CYCLE_DONE 上下文发给 QML。
     *
     * 参数：
     *   ok 表示 F4 是否 ACK 最终分拣并主动回传 CYCLE_DONE。
     *   detail 是最终分拣 ACK 和完成等待摘要。
     *   cycleDoneContextJson 是 F4 最终完成负载解析出的 JSON。
     */
    void handleF4FinalSortFinished(bool ok,
                                   const QString &detail,
                                   const QString &cycleDoneContextJson)
    {
        m_f4CommandRunning = false;
        m_f4FinalSortRunning = false;

        /*
         * CYCLE_DONE 表示这一件零件对应的自动流程已经真正闭环完成：
         *   1. 当前旧 cycle 后续不能再接受 pause/resume/stop。
         *   2. 如果 QML 侧要继续跑下一件，必须重新发送 START_CYCLE 生成新 cycle_id。
         *   3. 因此这里无论成功还是失败，都先释放本地 running/paused 状态，避免后续误把旧 cycle 当成仍在运行。
         */
        m_f4AutoRunning = false;
        m_f4AutoPaused = false;

        if (ok) {
            setF4Status(QStringLiteral("接入"), QStringLiteral("#35d07f"));
            setDetailText(QStringLiteral("F4 最终分拣完成：") + detail);
        } else {
            setDetailText(QStringLiteral("F4 最终分拣失败：") + detail);
        }

        emit f4FinalSortFinished(ok, detail, cycleDoneContextJson);
    }

    /*
     * handleF4StepperSettingsFinished 的作用：
     *   接收后台步进电机参数下发结果，并把 ACK/NACK 详情同步给 QML 参数弹窗。
     *
     * 参数：
     *   ok 为 true 表示 F4 已接受三台电机参数。
     *   detail 是 ACK/NACK 二进制解析结果或串口失败原因。
     *
     * 返回值：
     *   无返回值；函数会释放发送忙标志并发出 f4StepperSettingsFinished 信号。
     */
    void handleF4StepperSettingsFinished(bool ok, const QString &detail)
    {
        m_f4CommandRunning = false;

        if (ok) {
            setF4Status(QStringLiteral("接入"), QStringLiteral("#35d07f"));
            setDetailText(QStringLiteral("F4 步进参数已接收：") + detail);
        } else {
            setDetailText(QStringLiteral("F4 步进参数下发失败：") + detail);
        }

        emit f4StepperSettingsFinished(ok, detail);
    }

    /*
     * handleF4VisionCommandFinished 的作用：
     *   接收后台视觉闭环命令发送结果，并释放 F4 串口忙标志。
     *
     * 参数：
     *   ok 为 true 表示 F4 返回了匹配当前命令、SEQ 和 cycle_id 的 ACK。
     *   action 是视觉闭环动作名称，例如 VISION_POS 或 BELT_STOP_CENTERED。
     *   cycleId 是本次命令所属自动流程号。
     *   detail 是 ACK/NACK 解析结果或串口失败原因。
     *
     * 返回值：
     *   无返回值；结果通过 f4VisionCommandFinished 通知 QML 自动流程状态机。
     */
    void handleF4VisionCommandFinished(bool ok, const QString &action, quint16 cycleId, const QString &detail)
    {
        m_f4CommandRunning = false;

        if (ok) {
            setF4Status(QStringLiteral("接入"), QStringLiteral("#35d07f"));
            setDetailText(QStringLiteral("F4视觉闭环命令完成：") + action + QStringLiteral(" ") + detail);
        } else {
            setDetailText(QStringLiteral("F4视觉闭环命令失败：") + action + QStringLiteral(" ") + detail);
        }

        emit f4VisionCommandFinished(ok, action, cycleId, detail);
    }

    /*
     * handleF4ActuatorCommandFinished 的作用：
     *   接收后台执行器位置运动或停止命令发送结果，并释放 F4 串口忙标志。
     *
     * 参数：
     *   ok 为 true 表示 F4 返回了匹配当前命令、SEQ 和 cycle_id 的 ACK。
     *   action 是执行器命令名称，例如 ACTUATOR_POS_MOVE 或 ACTUATOR_STOP。
     *   cycleId 是本次命令所属自动流程号；手动命令通常为 0。
     *   detail 是 ACK/NACK 解析结果或串口失败原因。
     *
     * 返回值：
     *   无返回值；结果通过 f4ActuatorCommandFinished 通知 QML 自动流程或手动弹窗。
     */
    void handleF4ActuatorCommandFinished(bool ok, const QString &action, quint16 cycleId, const QString &detail)
    {
        m_f4CommandRunning = false;

        if (ok) {
            setF4Status(QStringLiteral("接入"), QStringLiteral("#35d07f"));
            setDetailText(QStringLiteral("F4执行器命令完成：") + action + QStringLiteral(" ") + detail);
        } else {
            setDetailText(QStringLiteral("F4执行器命令失败：") + action + QStringLiteral(" ") + detail);
        }

        emit f4ActuatorCommandFinished(ok, action, cycleId, detail);
    }

    /*
     * handleF4ActuatorStopNowFinished 的作用：
     *   接收手动强制 STOP 写入线程结果，并通知 QML 更新按钮反馈。
     *
     * 关键说明：
     *   该槽不能修改 m_f4CommandRunning。强制 STOP 是安全旁路，可能在上一条普通执行器命令
     *   仍等待 ACK 时并行写入；如果这里清除普通 busy 标志，后续普通命令可能再次并发抢串口。
     *
     * 参数：
     *   ok 为 true 表示 STOP 二进制帧已完整写入并通过 tcdrain 排空内核发送队列。
     *   action 固定为 ACTUATOR_STOP_NOW。
     *   cycleId 是本次 STOP 使用的 cycle_id，手动通常为 0。
     *   detail 是写入结果或失败原因。
     */
    void handleF4ActuatorStopNowFinished(bool ok, const QString &action, quint16 cycleId, const QString &detail)
    {
        if (ok) {
            setF4Status(QStringLiteral("接入"), QStringLiteral("#35d07f"));
            setDetailText(QStringLiteral("F4强制停止帧已写入：") + detail);
        } else {
            setDetailText(QStringLiteral("F4强制停止帧写入失败：") + detail);
        }

        emit f4ActuatorCommandFinished(ok, action, cycleId, detail);
    }

    /*
     * handleF4AutoControlFinished 的作用：
     *   接收后台二进制自动流程命令结果，并在 Qt 主线程维护 MP157 本地 cycle 状态。
     *
     * 主要流程：
     *   1. 释放 F4 串口命令忙标志，让后续 CAL、BELT 或自动流程命令可以继续下发。
     *   2. ACK 成功时根据 action 更新 running/paused 状态；NACK 或超时时不推进本地状态。
     *   3. 把 ACK/NACK 详情写入设备健康详情，并发信号给 QML 刷新首页状态和底部提示。
     *
     * 参数：
     *   ok 为 true 表示 F4 返回了匹配当前命令、SEQ 和 cycle_id 的 ACK。
     *   action 是本次首页动作，取值 start、pause、resume、stop。
     *   cycleId 是本次命令归属的自动检测流程号。
     *   detail 是 ACK/NACK 解析结果或串口失败原因。
     *
     * 返回值：
     *   无返回值。
     */
    void handleF4AutoControlFinished(bool ok, const QString &action, quint16 cycleId, const QString &detail)
    {
        m_f4CommandRunning = false;

        if (ok) {
            setF4Status(QStringLiteral("接入"), QStringLiteral("#35d07f"));
            if (action == QStringLiteral("start")) {
                m_f4AutoCycleId = cycleId;
                m_f4AutoRunning = true;
                m_f4AutoPaused = false;
            } else if (action == QStringLiteral("pause")) {
                m_f4AutoRunning = true;
                m_f4AutoPaused = true;
            } else if (action == QStringLiteral("resume")) {
                m_f4AutoRunning = true;
                m_f4AutoPaused = false;
            } else if (action == QStringLiteral("stop")) {
                m_f4AutoRunning = false;
                m_f4AutoPaused = false;
            }
            setDetailText(QStringLiteral("F4自动流程命令完成：") + detail);
        } else {
            setDetailText(QStringLiteral("F4自动流程命令失败：") + detail);
        }

        emit f4AutoControlFinished(ok, action, cycleId, detail);
    }

private:
    /*
     * F4BinaryReply 的作用：
     *   保存从 F407 收到的一帧二进制协议解析结果，避免 ACK/NACK 处理函数反复解析字节偏移。
     */
    struct F4BinaryReply
    {
        quint8 command;       /* command 保存回复帧 CMD 字段，例如 ACK=0x80 或 NACK=0x81。 */
        quint16 sequence;     /* sequence 保存回复帧自身 SEQ，不等于被确认的请求 SEQ。 */
        QByteArray payload;   /* payload 保存回复帧负载，ACK/NACK 字段从这里按小端读取。 */
        QByteArray rawFrame;  /* rawFrame 保存完整原始帧，用于 CRC 错误或未知命令时打印十六进制排查。 */
    };

    /*
     * isAllowedF4BeltCommand 的作用：
     *   校验 QML 手动传送带按钮是否只发送 F407 当前已实现且能同步回包的安全命令。
     *
     * 参数：
     *   command 是已经 trim 并转成大写的命令正文。
     *
     * 返回值：
     *   true 表示命令可下发；false 表示命令不是当前 Qt 手动页开放的传送带命令。
     */
    static bool isAllowedF4BeltCommand(const QString &command)
    {
        return command == QStringLiteral("BELT_MANUAL_SCAN")
                || command == QStringLiteral("BELT_MANUAL_STOP")
                || command == QStringLiteral("QUERY_STATUS");
    }

    /*
     * nextF4AutoCycleId 的作用：
     *   为首页“开始”创建新的自动检测流程号。
     *
     * 关键说明：
     *   cycle_id=0 保留为空闲/无流程语义，因此自增溢出到 0 时继续加到 1。
     *
     * 返回值：
     *   返回新的非 0 cycle_id。
     */
    quint16 nextF4AutoCycleId()
    {
        quint16 nextId = static_cast<quint16>(m_f4AutoCycleId + 1U);

        if (nextId == 0U) {
            nextId = 1U;
        }

        m_f4AutoCycleId = nextId;
        return m_f4AutoCycleId;
    }

    /*
     * appendLe16 的作用：
     *   按二进制协议小端序向负载追加一个 16 位无符号整数。
     *
     * 参数：
     *   payload 是要追加字段的负载缓冲，不能为 NULL。
     *   value 是要写入的字段值。
     *
     * 返回值：
     *   无返回值；函数直接修改 payload。
     */
    static void appendLe16(QByteArray *payload, quint16 value)
    {
        payload->append(static_cast<char>(value & 0x00FFU));
        payload->append(static_cast<char>((value >> 8) & 0x00FFU));
    }

    /*
     * appendLeI16 的作用：
     *   按二进制协议小端序向负载追加一个 16 位有符号整数。
     *
     * 参数：
     *   payload 是要追加字段的负载缓冲，不能为 NULL。
     *   value 是像素坐标或误差类字段，函数会先限幅到 int16 范围。
     *
     * 返回值：
     *   无返回值；函数直接修改 payload。
     */
    static void appendLeI16(QByteArray *payload, int value)
    {
        const qint16 signedValue = static_cast<qint16>(clampedInt(value, -32768, 32767));
        const quint16 rawValue = static_cast<quint16>(signedValue);

        appendLe16(payload, rawValue);
    }

    /*
     * appendLe32 的作用：
     *   按二进制协议小端序向负载追加一个 32 位无符号整数。
     *
     * 参数：
     *   payload 是要追加字段的负载缓冲，不能为 NULL。
     *   value 是要写入的 32 位数，例如 capture_ms。
     *
     * 返回值：
     *   无返回值；函数直接修改 payload。
     */
    static void appendLe32(QByteArray *payload, quint32 value)
    {
        payload->append(static_cast<char>(value & 0x000000FFU));
        payload->append(static_cast<char>((value >> 8) & 0x000000FFU));
        payload->append(static_cast<char>((value >> 16) & 0x000000FFU));
        payload->append(static_cast<char>((value >> 24) & 0x000000FFU));
    }

    /*
     * stepperMotorRoleId 的作用：
     *   把 QML/JSON 中稳定的英文 role 映射成 F4 二进制协议中的电机编号。
     *
     * 参数：
     *   role 是电机角色名，当前允许 conveyor/camera_lateral/camera_z。
     *
     * 返回值：
     *   返回 1/2/3；未知角色返回 0，调用方据此拒绝下发。
     */
    static quint8 stepperMotorRoleId(const QString &role)
    {
        if (role == QStringLiteral("conveyor")) {
            return 1U;
        }
        if (role == QStringLiteral("camera_lateral") || role == QStringLiteral("camera_forward")) {
            return 2U;
        }
        if (role == QStringLiteral("camera_z")) {
            return 3U;
        }
        return 0U;
    }

    /*
     * buildStepperSettingsPayload 的作用：
     *   把三台步进电机设置编码成 F4 STEPPER_PARAM_SET 负载。
     *
     * 主要流程：
     *   1. 校验 QML 传来的数组必须正好包含三台电机，避免 F4 和 MP157 页序错位。
     *   2. 每台电机编码 role_id、address、min_step、normal_speed_rpm、scan_speed_rpm 和 direction。
     *      record_size=9，固定记录长度让 F4 能按版本化协议解析传送带双速度。
     *   3. 地址、步长、速度和方向在 MP157 再做一次限幅，F4 收到后还会重复校验。
     *
     * 参数：
     *   motors 是 QML 传入的 stepperMotorSettings。
     *   payload 是输出负载缓存，不能为空。
     *   errorText 是本地校验失败原因输出，可为 NULL。
     *
     * 返回值：
     *   true 表示负载可发送；false 表示参数缺失或 role 非法。
     */
    static bool buildStepperSettingsPayload(const QVariantList &motors,
                                            QByteArray *payload,
                                            QString *errorText)
    {
        if (payload == nullptr) {
            return false;
        }

        payload->clear();
        if (motors.size() != 3) {
            if (errorText) {
                *errorText = QStringLiteral("步进参数数量错误：") + QString::number(motors.size());
            }
            return false;
        }

        appendLe16(payload, 0U);                 /* cycle_id=0，参数下发不绑定某一轮自动检测流程。 */
        payload->append(static_cast<char>(3U));  /* motor_count=3，固定三台已规划 Emm42。 */
        payload->append(static_cast<char>(0U));  /* flags=0，首版没有持久化到 F4 Flash 的含义。 */

        for (int index = 0; index < motors.size(); ++index) {
            const QVariantMap motor = motors.at(index).toMap();
            const QString role = motor.value(QStringLiteral("role")).toString();
            const quint8 roleId = stepperMotorRoleId(role);

            if (roleId == 0U) {
                if (errorText) {
                    *errorText = QStringLiteral("步进参数角色非法：") + role;
                }
                payload->clear();
                return false;
            }

            const int address = clampedInt(motor.value(QStringLiteral("address")).toInt(), 1, 247);
            const int minStep = clampedInt(motor.value(QStringLiteral("minStep")).toInt(), 1, 10000);
            const int normalSpeed = clampedInt(motor.value(QStringLiteral("normalSpeedRpm")).toInt(), 0, 5000);
            const int scanSpeed = clampedInt(motor.value(QStringLiteral("scanSpeedRpm")).toInt(), 0, 5000);
            /* directionRaw 保存 QML 传来的方向数值，Qt 5.12 的 QVariant::toInt() 不能传默认整数。 */
            const int directionRaw = motor.value(QStringLiteral("direction")).toInt();
            const int direction = directionRaw >= 0 ? 1 : -1;

            payload->append(static_cast<char>(roleId));
            payload->append(static_cast<char>(address & 0xFF));
            appendLe16(payload, static_cast<quint16>(minStep));
            appendLe16(payload, static_cast<quint16>(normalSpeed));
            appendLe16(payload, static_cast<quint16>(scanSpeed));
            payload->append(static_cast<char>(direction));
        }

        return true;
    }

    /*
     * readLe16 的作用：
     *   从二进制协议负载或帧头中按小端序读取一个 16 位无符号整数。
     *
     * 参数：
     *   data 是源字节数组。
     *   offset 是低字节所在下标，调用方必须先保证 offset+1 未越界。
     *
     * 返回值：
     *   返回解析出的 16 位整数。
     */
    static quint16 readLe16(const QByteArray &data, int offset)
    {
        const quint16 low = static_cast<quint8>(data.at(offset));
        const quint16 high = static_cast<quint8>(data.at(offset + 1));
        return static_cast<quint16>(low | static_cast<quint16>(high << 8));
    }

    /*
     * readLe32Signed 的作用：
     *   从二进制协议负载中按小端序读取一个 32 位有符号整数。
     *
     * 参数：
     *   data 是源字节数组。
     *   offset 是最低字节所在下标，调用方必须先保证 offset+3 未越界。
     *
     * 返回值：
     *   返回解析出的 32 位有符号整数，当前用于 STATUS_REPORT 的传送带像素误差。
     */
    static qint32 readLe32Signed(const QByteArray &data, int offset)
    {
        const quint32 b0 = static_cast<quint8>(data.at(offset));
        const quint32 b1 = static_cast<quint8>(data.at(offset + 1));
        const quint32 b2 = static_cast<quint8>(data.at(offset + 2));
        const quint32 b3 = static_cast<quint8>(data.at(offset + 3));
        return static_cast<qint32>(b0 | (b1 << 8) | (b2 << 16) | (b3 << 24));
    }

    /*
     * f4BinaryCommandName 的作用：
     *   把二进制协议命令字转换成便于界面、日志和串口调试阅读的短名称。
     *
     * 参数：
     *   command 是协议 CMD 字段。
     *
     * 返回值：
     *   返回命令名称；未知命令返回带十六进制值的 UNKNOWN。
     */
    static QString f4BinaryCommandName(quint8 command)
    {
        switch (command) {
        case BINARY_PROTOCOL_CMD_HEARTBEAT:
            return QStringLiteral("HEARTBEAT");
        case BINARY_PROTOCOL_CMD_START_CYCLE:
            return QStringLiteral("START_CYCLE");
        case BINARY_PROTOCOL_CMD_PAUSE_CYCLE:
            return QStringLiteral("PAUSE_CYCLE");
        case BINARY_PROTOCOL_CMD_RESUME_CYCLE:
            return QStringLiteral("RESUME_CYCLE");
        case BINARY_PROTOCOL_CMD_STOP_CYCLE:
            return QStringLiteral("STOP_CYCLE");
        case BINARY_PROTOCOL_CMD_VISION_POS:
            return QStringLiteral("VISION_POS");
        case BINARY_PROTOCOL_CMD_VISION_LOST:
            return QStringLiteral("VISION_LOST");
        case BINARY_PROTOCOL_CMD_BELT_STOP_CENTERED:
            return QStringLiteral("BELT_STOP_CENTERED");
        case BINARY_PROTOCOL_CMD_WEIGHT_CALIBRATE:
            return QStringLiteral("WEIGHT_CALIBRATE");
        case BINARY_PROTOCOL_CMD_ARM_JOB_START:
            return QStringLiteral("ARM_JOB_START");
        case BINARY_PROTOCOL_CMD_MODEL_READY:
            return QStringLiteral("MODEL_READY");
        case BINARY_PROTOCOL_CMD_FINAL_SORT_RESULT:
            return QStringLiteral("FINAL_SORT_RESULT");
        case BINARY_PROTOCOL_CMD_QUERY_STATUS:
            return QStringLiteral("QUERY_STATUS");
        case BINARY_PROTOCOL_CMD_BELT_MANUAL_CONTROL:
            return QStringLiteral("BELT_MANUAL_CONTROL");
        case BINARY_PROTOCOL_CMD_STEPPER_PARAM_SET:
            return QStringLiteral("STEPPER_PARAM_SET");
        case BINARY_PROTOCOL_CMD_ACTUATOR_POS_MOVE:
            return QStringLiteral("ACTUATOR_POS_MOVE");
        case BINARY_PROTOCOL_CMD_ACTUATOR_STOP:
            return QStringLiteral("ACTUATOR_STOP");
        case BINARY_PROTOCOL_CMD_ACTUATOR_VEL_MOVE:
            return QStringLiteral("ACTUATOR_VEL_MOVE");
        case BINARY_PROTOCOL_CMD_ACTUATOR_HOME:
            return QStringLiteral("ACTUATOR_HOME");
        case BINARY_PROTOCOL_CMD_ACK:
            return QStringLiteral("ACK");
        case BINARY_PROTOCOL_CMD_NACK:
            return QStringLiteral("NACK");
        case BINARY_PROTOCOL_CMD_STATUS_REPORT:
            return QStringLiteral("STATUS_REPORT");
        case BINARY_PROTOCOL_CMD_FAULT_REPORT:
            return QStringLiteral("FAULT_REPORT");
        case BINARY_PROTOCOL_CMD_EVENT_REPORT:
            return QStringLiteral("EVENT_REPORT");
        case BINARY_PROTOCOL_CMD_WEIGHT_RESULT:
            return QStringLiteral("WEIGHT_RESULT");
        case BINARY_PROTOCOL_CMD_LDC_RESULT:
            return QStringLiteral("LDC_RESULT");
        case BINARY_PROTOCOL_CMD_CYCLE_DONE:
            return QStringLiteral("CYCLE_DONE");
        default:
            return QStringLiteral("UNKNOWN_0x") + QString::number(command, 16).toUpper();
        }
    }

    /*
     * isMp157ToF4RequestCommand 的作用：
     *   判断当前 CMD 是否属于 MP157 发给 F4 的请求帧，而不是 F4 返回给 MP157 的回包帧。
     *
     * 关键说明：
     *   现场 RS485/串口助手/半双工转接环境可能把 MP157 刚发出的 HEARTBEAT 等请求帧回显到 RX。
     *   F4->MP157 的正式回包统一从 ACK=0x80 开始；因此小于 ACK 的命令只能作为请求帧跳过，
     *   不能让上层 sendF4BinaryHeartbeat() 把 HEARTBEAT 回显误判成“非 ACK/NACK 回包”。
     *
     * 参数：
     *   command 是协议 CMD 字段。
     *
     * 返回值：
     *   true 表示这是 MP157->F4 方向的请求命令；false 表示可以继续按 F4 回包处理。
     */
    static bool isMp157ToF4RequestCommand(quint8 command)
    {
        return command < BINARY_PROTOCOL_CMD_ACK;
    }

    /*
     * f4FaultSourceName 的作用：
     *   把 F4 FAULT_REPORT 中的故障来源编号转换成界面可读短名称。
     *
     * 参数：
     *   source 是协议中的 fault_source 字段。
     *
     * 返回值：
     *   返回 UART/CONVEYOR/CAMERA_MOTOR/ARM/WEIGHT/LDC 等短名称；未知值返回 SOURCE_<数字>。
     */
    static QString f4FaultSourceName(quint8 source)
    {
        switch (source) {
        case 1:
            return QStringLiteral("UART");
        case 2:
            return QStringLiteral("CONVEYOR");
        case 3:
            return QStringLiteral("CAMERA_MOTOR");
        case 4:
            return QStringLiteral("ARM");
        case 5:
            return QStringLiteral("WEIGHT");
        case 6:
            return QStringLiteral("LDC");
        default:
            return QStringLiteral("SOURCE_") + QString::number(source);
        }
    }

    /*
     * f4FaultSeverityName 的作用：
     *   把 F4 FAULT_REPORT 中的严重等级转换成界面可读短名称。
     *
     * 参数：
     *   severity 是协议中的 severity 字段。
     *
     * 返回值：
     *   返回 INFO/WARNING/STOP；未知值返回 SEVERITY_<数字>。
     */
    static QString f4FaultSeverityName(quint8 severity)
    {
        switch (severity) {
        case 1:
            return QStringLiteral("INFO");
        case 2:
            return QStringLiteral("WARNING");
        case 3:
            return QStringLiteral("STOP");
        default:
            return QStringLiteral("SEVERITY_") + QString::number(severity);
        }
    }

    /*
     * describeF4FaultReport 的作用：
     *   把 F4 异步 FAULT_REPORT 二进制负载转换成状态栏可读摘要。
     *
     * 参数：
     *   reply 是已经通过 CRC 校验的二进制帧。
     *
     * 返回值：
     *   返回故障摘要；如果负载长度不对，返回长度错误并带原始字节。
     */
    static QString describeF4FaultReport(const F4BinaryReply &reply)
    {
        if (reply.payload.size() != 16) {
            return QStringLiteral("FAULT_REPORT负载长度错误：")
                    + QString::number(reply.payload.size())
                    + QStringLiteral(" raw=")
                    + hexByteString(reply.rawFrame);
        }

        const quint16 cycleId = readLe16(reply.payload, 0);
        const quint16 faultCode = readLe16(reply.payload, 2);
        const quint8 source = static_cast<quint8>(reply.payload.at(4));
        const quint8 severity = static_cast<quint8>(reply.payload.at(5));
        const quint8 state = static_cast<quint8>(reply.payload.at(6));
        const qint32 detailValue = readLe32Signed(reply.payload, 8);
        const quint16 relatedSequence = readLe16(reply.payload, 12);
        const quint16 faultBits = readLe16(reply.payload, 14);

        return QStringLiteral("FAULT_REPORT cycle=") + QString::number(cycleId)
                + QStringLiteral(" source=") + f4FaultSourceName(source)
                + QStringLiteral(" severity=") + f4FaultSeverityName(severity)
                + QStringLiteral(" code=") + QString::number(faultCode)
                + QStringLiteral(" state=") + f4ProtocolStateName(state)
                + QStringLiteral(" detail=") + QString::number(detailValue)
                + QStringLiteral(" related_seq=") + QString::number(relatedSequence)
                + QStringLiteral(" fault=0x") + QString::number(faultBits, 16).toUpper();
    }

    /*
     * f4EventCodeName 的作用：
     *   把 F4 EVENT_REPORT 中的事件编号转换成界面、日志和测试脚本都能识别的短名称。
     *
     * 参数：
     *   eventCode 是 EVENT_REPORT payload[2] 事件编号。
     *
     * 返回值：
     *   对执行器完成和超时返回固定 marker；其它事件返回 EVENT_<数字>。
     */
    static QString f4EventCodeName(quint8 eventCode)
    {
        switch (eventCode) {
        case BINARY_PROTOCOL_EVENT_ACTUATOR_MOVE_DONE:
            return QStringLiteral("actuator-move-done");
        case BINARY_PROTOCOL_EVENT_ACTUATOR_MOVE_TIMEOUT:
            return QStringLiteral("actuator-move-timeout");
        default:
            return QStringLiteral("EVENT_") + QString::number(eventCode);
        }
    }

    /*
     * f4ActuatorMoveStatusName 的作用：
     *   把执行器完成事件 detail 低 16 位 status_code 转换成现场可读的完成来源。
     *
     * 主要流程：
     *   1. status=0 表示 F4 确实收到了张大头 Emm42 的主动到位回包。
     *   2. status=5 表示官方位置模式未主动返回完成帧时，F4 按运动时间估算完成并继续流程。
     *   3. 其它状态保留原始数字，避免未来 F4 增加状态后 MP157 日志丢信息。
     *
     * 参数：
     *   statusCode 是 EVENT_REPORT detail 低 16 位状态码。
     *
     * 返回值：
     *   返回 reached-ack、estimated-done 或 status-N，用于 QML、日志和静态检查定位问题。
     */
    static QString f4ActuatorMoveStatusName(quint16 statusCode)
    {
        switch (statusCode) {
        case F4_ACTUATOR_MOVE_STATUS_REACHED_ACK:
            return QStringLiteral("reached-ack");
        case F4_ACTUATOR_MOVE_STATUS_ESTIMATED_DONE:
            return QStringLiteral("estimated-done");
        default:
            return QStringLiteral("status-") + QString::number(statusCode);
        }
    }

    /*
     * describeF4EventReport 的作用：
     *   把 F4 异步 EVENT_REPORT 二进制负载转换成自动流程可读摘要。
     *
     * 主要流程：
     *   1. 校验固定 16 字节负载长度。
     *   2. 解析 cycle、event、state、step、source、detail、related_seq 和 fault_bits。
     *   3. 对执行器运动事件，把 detail_i32 拆成 actuator、direction 和 status_code。
     *
     * 返回值：
     *   返回包含 actuator-move-done 或 actuator-move-timeout marker 的摘要；
     *   负载错误时返回长度错误和原始帧，便于串口排查。
     */
    static QString describeF4EventReport(const F4BinaryReply &reply)
    {
        if (reply.payload.size() != 16) {
            return QStringLiteral("EVENT_REPORT负载长度错误：")
                    + QString::number(reply.payload.size())
                    + QStringLiteral(" raw=")
                    + hexByteString(reply.rawFrame);
        }

        const quint16 cycleId = readLe16(reply.payload, 0);
        const quint8 eventCode = static_cast<quint8>(reply.payload.at(2));
        const quint8 state = static_cast<quint8>(reply.payload.at(3));
        const quint8 stepCode = static_cast<quint8>(reply.payload.at(4));
        const quint8 source = static_cast<quint8>(reply.payload.at(5));
        const qint32 detailValue = readLe32Signed(reply.payload, 6);
        const quint32 detailBits = static_cast<quint32>(detailValue);
        const quint16 relatedSequence = readLe16(reply.payload, 10);
        const quint16 faultBits = readLe16(reply.payload, 12);
        const quint16 reserved = readLe16(reply.payload, 14);
        const quint8 actuator = static_cast<quint8>((detailBits >> 24) & 0xFFU);
        const quint8 direction = static_cast<quint8>((detailBits >> 16) & 0xFFU);
        const quint16 statusCode = static_cast<quint16>(detailBits & 0xFFFFU);

        return QStringLiteral("EVENT_REPORT ")
                + f4EventCodeName(eventCode)
                + QStringLiteral(" cycle=") + QString::number(cycleId)
                + QStringLiteral(" state=") + f4ProtocolStateName(state)
                + QStringLiteral(" step=") + QString::number(stepCode)
                + QStringLiteral(" source=") + f4FaultSourceName(source)
                + QStringLiteral(" actuator=") + QString::number(actuator)
                + QStringLiteral(" direction=") + QString::number(direction)
                + QStringLiteral(" status=") + QString::number(statusCode)
                + QStringLiteral("(") + f4ActuatorMoveStatusName(statusCode) + QStringLiteral(")")
                + QStringLiteral(" related_seq=") + QString::number(relatedSequence)
                + QStringLiteral(" fault=0x") + QString::number(faultBits, 16).toUpper()
                + QStringLiteral(" reserved=") + QString::number(reserved);
    }

    /*
     * f4BeltModeName 的作用：
     *   把 F4 STATUS_REPORT 中的传送带模式编号转换成界面可读短文本。
     *
     * 参数：
     *   mode 是传送带模式，0=STOP，1=SCAN，2=TRACK，3=POSITION，4=JOG。
     *
     * 返回值：
     *   返回 STOP/SCAN/TRACK/POSITION/JOG；未知模式返回 BELT_<数字>。
     */
    static QString f4BeltModeName(quint8 mode)
    {
        switch (mode) {
        case 0:
            return QStringLiteral("STOP");
        case 1:
            return QStringLiteral("SCAN");
        case 2:
            return QStringLiteral("TRACK");
        case 3:
            return QStringLiteral("POSITION");
        case 4:
            return QStringLiteral("JOG");
        default:
            return QStringLiteral("BELT_") + QString::number(mode);
        }
    }

    /*
     * f4ProtocolStateName 的作用：
     *   把 F4 ACK/NACK 中携带的主状态枚举转换成中文短文本，便于现场定位当前流程阶段。
     *
     * 参数：
     *   state 是协议 8.4 定义的 F4 主状态枚举。
     *
     * 返回值：
     *   返回状态中文名；未知值返回 STATE_<数字>。
     */
    static QString f4ProtocolStateName(quint8 state)
    {
        switch (state) {
        case 0:
            return QStringLiteral("IDLE");
        case 1:
            return QStringLiteral("SCANNING");
        case 2:
            return QStringLiteral("TRACKING");
        case 3:
            return QStringLiteral("CENTERED_HOLD");
        case 4:
            return QStringLiteral("WAIT_MODEL");
        case 5:
            return QStringLiteral("ARM_PICKING");
        case 6:
            return QStringLiteral("WEIGHING");
        case 7:
            return QStringLiteral("LDC_TESTING");
        case 8:
            return QStringLiteral("SORTING");
        case 9:
            return QStringLiteral("DONE");
        case 10:
            return QStringLiteral("PAUSED");
        case 11:
            return QStringLiteral("STOPPED");
        case 12:
            return QStringLiteral("FAULT");
        default:
            return QStringLiteral("STATE_") + QString::number(state);
        }
    }

    /*
     * f4NackErrorName 的作用：
     *   把 NACK error_code 转换成协议文档中的错误名称，便于判断是状态不允许、忙还是 cycle 不匹配。
     *
     * 参数：
     *   errorCode 是协议 8.5 定义的 NACK 错误码。
     *
     * 返回值：
     *   返回错误名称；未知值返回 ERR_<数字>。
     */
    static QString f4NackErrorName(quint8 errorCode)
    {
        switch (errorCode) {
        case 1:
            return QStringLiteral("ERR_CRC");
        case 2:
            return QStringLiteral("ERR_FRAME_LENGTH");
        case 3:
            return QStringLiteral("ERR_CMD_UNKNOWN");
        case 4:
            return QStringLiteral("ERR_PAYLOAD_LENGTH");
        case 5:
            return QStringLiteral("ERR_FIELD_RANGE");
        case 6:
            return QStringLiteral("ERR_STATE_NOT_ALLOWED");
        case 7:
            return QStringLiteral("ERR_BUSY");
        case 8:
            return QStringLiteral("ERR_CYCLE_MISMATCH");
        case 9:
            return QStringLiteral("ERR_TIMEOUT");
        case 10:
            return QStringLiteral("ERR_HARDWARE_FAULT");
        default:
            return QStringLiteral("ERR_") + QString::number(errorCode);
        }
    }

    /*
     * crc16CcittFalse 的作用：
     *   计算本文档二进制协议使用的 CRC16-CCITT-FALSE。
     *
     * 主要流程：
     *   1. 使用 0xFFFF 初始化 CRC。
     *   2. 每个输入字节先移入 CRC 高 8 位。
     *   3. 每位按多项式 0x1021 左移计算，最终不反射、不异或。
     *
     * 参数：
     *   data 是需要参与 CRC 的字节数组，调用方应传入 VER 到 PAYLOAD 范围。
     *
     * 返回值：
     *   返回 16 位 CRC，组帧时按低字节、高字节发送。
     */
    static quint16 crc16CcittFalse(const QByteArray &data)
    {
        quint16 crc = 0xFFFFU;

        for (int i = 0; i < data.size(); ++i) {
            crc ^= static_cast<quint16>(static_cast<quint8>(data.at(i)) << 8);
            for (int bit = 0; bit < 8; ++bit) {
                if ((crc & 0x8000U) != 0U) {
                    crc = static_cast<quint16>((crc << 1) ^ 0x1021U);
                } else {
                    crc = static_cast<quint16>(crc << 1);
                }
            }
        }

        return crc;
    }

    /*
     * buildF4BinaryFrame 的作用：
     *   按 `A5 5A VER CMD LEN SEQ PAYLOAD CRC 6B` 格式组装 MP157 发给 F407 的二进制短帧。
     *
     * 参数：
     *   command 是 CMD 字段。
     *   sequence 是 MP157 本地帧序号。
     *   payload 是命令负载，长度不能超过 BINARY_PROTOCOL_MAX_PAYLOAD。
     *
     * 返回值：
     *   返回完整可直接写入串口的二进制帧；负载过长时返回空数组。
     */
    static QByteArray buildF4BinaryFrame(quint8 command, quint16 sequence, const QByteArray &payload)
    {
        if (payload.size() > BINARY_PROTOCOL_MAX_PAYLOAD) {
            return QByteArray();
        }

        QByteArray frame;       /* frame 保存最终完整帧，写串口时不再二次拼接。 */
        QByteArray crcScope;    /* crcScope 保存 VER 到 PAYLOAD 范围，用于 CRC16-CCITT-FALSE。 */
        const quint8 length = static_cast<quint8>(payload.size());

        frame.append(static_cast<char>(BINARY_PROTOCOL_SOF0));
        frame.append(static_cast<char>(BINARY_PROTOCOL_SOF1));
        frame.append(static_cast<char>(BINARY_PROTOCOL_VERSION));
        frame.append(static_cast<char>(command));
        frame.append(static_cast<char>(length));
        frame.append(static_cast<char>(sequence & 0x00FFU));
        frame.append(static_cast<char>((sequence >> 8) & 0x00FFU));
        frame.append(payload);

        crcScope = frame.mid(2, 5 + payload.size());
        const quint16 crc = crc16CcittFalse(crcScope);
        frame.append(static_cast<char>(crc & 0x00FFU));
        frame.append(static_cast<char>((crc >> 8) & 0x00FFU));
        frame.append(static_cast<char>(BINARY_PROTOCOL_EOF));

        return frame;
    }

    /*
     * hexByteString 的作用：
     *   把二进制帧转换成 `A5 5A 01 ...` 形式的十六进制文本，便于底部提示和串口日志核对。
     *
     * 参数：
     *   data 是待显示的二进制数据。
     *
     * 返回值：
     *   返回空格分隔的大写十六进制字符串。
     */
    static QString hexByteString(const QByteArray &data)
    {
        QStringList parts;

        for (int i = 0; i < data.size(); ++i) {
            parts << QStringLiteral("%1")
                     .arg(static_cast<int>(static_cast<quint8>(data.at(i))), 2, 16, QLatin1Char('0'))
                     .toUpper();
        }

        return parts.join(QLatin1Char(' '));
    }

    /*
     * parseF4BinaryFrameFromBuffer 的作用：
     *   从串口累计字节流中寻找并解析一帧完整 F4 二进制回复。
     *
     * 主要流程：
     *   1. 丢弃帧头前的噪声字节，保留可能成为下一帧 SOF0 的尾字节。
     *   2. 根据 LEN 计算完整帧长度，等待数据足够后再校验 EOF 和 CRC。
     *   3. CRC 覆盖 VER 到 PAYLOAD，解析成功后填充 F4BinaryReply。
     *
     * 参数：
     *   buffer 是持续追加 read 数据的缓冲，成功解析或丢弃噪声时会被修改。
     *   reply 用于返回解析结果。
     *   errorText 用于返回明确错误原因，可为 NULL。
     *
     * 返回值：
     *   true 表示成功解析出一帧；false 表示数据不足或帧校验失败。
     */
    static bool parseF4BinaryFrameFromBuffer(QByteArray *buffer, F4BinaryReply *reply, QString *errorText)
    {
        while (buffer->size() >= 2) {
            int headerIndex = -1;

            for (int i = 0; i + 1 < buffer->size(); ++i) {
                if (static_cast<quint8>(buffer->at(i)) == BINARY_PROTOCOL_SOF0
                        && static_cast<quint8>(buffer->at(i + 1)) == BINARY_PROTOCOL_SOF1) {
                    headerIndex = i;
                    break;
                }
            }

            if (headerIndex < 0) {
                const bool keepLastSof0 = static_cast<quint8>(buffer->at(buffer->size() - 1)) == BINARY_PROTOCOL_SOF0;
                buffer->clear();
                if (keepLastSof0) {
                    buffer->append(static_cast<char>(BINARY_PROTOCOL_SOF0));
                }
                return false;
            }

            if (headerIndex > 0) {
                buffer->remove(0, headerIndex);
            }

            if (buffer->size() < BINARY_PROTOCOL_MIN_FRAME_SIZE) {
                return false;
            }

            const quint8 payloadLength = static_cast<quint8>(buffer->at(4));
            if (payloadLength > BINARY_PROTOCOL_MAX_PAYLOAD) {
                if (errorText) {
                    *errorText = QStringLiteral("F4二进制帧负载过长：") + QString::number(payloadLength);
                }
                buffer->remove(0, 1);
                continue;
            }

            const int frameSize = BINARY_PROTOCOL_MIN_FRAME_SIZE + payloadLength;
            if (buffer->size() < frameSize) {
                return false;
            }

            const QByteArray frame = buffer->left(frameSize);
            buffer->remove(0, frameSize);

            if (static_cast<quint8>(frame.at(frameSize - 1)) != BINARY_PROTOCOL_EOF) {
                if (errorText) {
                    *errorText = QStringLiteral("F4二进制帧帧尾错误：") + hexByteString(frame);
                }
                continue;
            }

            if (static_cast<quint8>(frame.at(2)) != BINARY_PROTOCOL_VERSION) {
                if (errorText) {
                    *errorText = QStringLiteral("F4二进制协议版本不匹配：") + QString::number(static_cast<quint8>(frame.at(2)));
                }
                continue;
            }

            const quint16 expectedCrc = readLe16(frame, 7 + payloadLength);
            const quint16 actualCrc = crc16CcittFalse(frame.mid(2, 5 + payloadLength));
            if (expectedCrc != actualCrc) {
                if (errorText) {
                    *errorText = QStringLiteral("F4二进制帧CRC错误，recv=0x")
                            + QString::number(expectedCrc, 16).toUpper()
                            + QStringLiteral(" calc=0x")
                            + QString::number(actualCrc, 16).toUpper();
                }
                continue;
            }

            reply->command = static_cast<quint8>(frame.at(3));
            reply->sequence = readLe16(frame, 5);
            reply->payload = frame.mid(7, payloadLength);
            reply->rawFrame = frame;
            return true;
        }

        return false;
    }

    /*
     * compactText 的作用：
     *   把外部命令输出压缩成适合界面显示的一行。
     */
    QString compactText(const QString &text, int maxLen) const
    {
        QString compact = text;

        compact.replace(QLatin1Char('\n'), QLatin1Char(' '));
        compact.replace(QLatin1Char('\r'), QLatin1Char(' '));
        compact = compact.simplified();
        if (compact.length() > maxLen) {
            compact = compact.left(maxLen);
        }
        return compact;
    }

    /*
     * startNetworkProbe 的作用：
     *   异步启动 4G 联网测试，忙时跳过本轮，防止弱网下任务堆积。
     *
     * 关键说明：
     *   周期刷新不能每轮把界面改成“检测中”，否则网络正常时会在“检测中/在线”之间闪烁。
     *   初始状态已经是“检测中”，后续后台静默刷新，只在成功、失败或超时时更新稳定状态。
     */
    void startNetworkProbe()
    {
        if (m_networkProbeRunning) {
            return;
        }

        m_networkProbeRunning = true;
        m_networkProbeTimedOut = false;
        m_networkProcess.start(m_networkScript, QStringList() << QStringLiteral("test"));
        if (m_networkProbeRunning) {
            m_networkTimeout.start(7000);
        }
    }

    /*
     * startLocationProbe 的作用：
     *   在 Qt 启动后的第一轮健康检测中执行一次高德 IP 省份定位。
     *
     * 主要流程：
     *   1. 如果已经调度过开机定位，直接返回，保证后续健康刷新不再访问高德接口。
     *   2. 把 m_locationBootProbeDone 立即置为 true，即使本次失败也不在本进程内自动重试。
     *   3. 只调用 `4g-location once`；脚本内部只走高德 IP 定位，不走 GPS/AT 串口。
     *
     * 返回值：
     *   无返回值；定位完成后通过 handleLocationProcessFinished() 更新 QML 属性。
     */
    void startLocationProbe()
    {
        if (m_locationBootProbeDone || m_locationProbeRunning) {
            return;
        }

        m_locationBootProbeDone = true;
        m_locationProbeRunning = true;
        m_locationProbeTimedOut = false;
        m_locationProcess.start(m_locationScript, QStringList() << QStringLiteral("once"));
        if (m_locationProbeRunning) {
            m_locationTimeout.start(LOCATION_PROBE_TIMEOUT_MS);
        }
    }

    /*
     * startCloudProbe 的作用：
     *   异步访问云端 health；如果没有 curl，则状态显示待确认，不影响其它设备检测。
     *
     * 关键说明：
     *   周期刷新时保留上一轮“已连接/离线/超时”等稳定结果，不再每轮显示“检测中”。
     */
    void startCloudProbe()
    {
        if (m_cloudProbeRunning) {
            return;
        }

        m_cloudProbeRunning = true;
        m_cloudProbeTimedOut = false;
        m_cloudProcess.start(QStringLiteral("curl"),
                             QStringList()
                             << QStringLiteral("-fsS")
                             << QStringLiteral("--max-time")
                             << QStringLiteral("2")
                             << m_cloudHealthUrl);
        if (m_cloudProbeRunning) {
            m_cloudTimeout.start(4000);
        }
    }

    /*
     * refreshOverlayCameraStatus 的作用：
     *   调度摄像头真实状态检测；kms-overlay 不再直接假定在线。
     *
     * 主要流程：
     *   1. 非 kms-overlay 后端只检查设备节点是否存在，避免影响旧兜底路线。
     *   2. kms-overlay 后端把 Unix socket STATUS 查询放到后台线程，避免 overlay 异常时主界面卡顿。
     *   3. 后台线程只返回一行状态文本，最终解析和属性更新仍回到 Qt 主线程执行。
     *
     * 返回值：
     *   无返回值；检测结果通过 cameraStatusChanged 通知 QML。
     */
    void refreshOverlayCameraStatus()
    {
        if (m_overlayProbeRunning) {
            return;
        }

        if (m_videoBackend != QString::fromLatin1(BACKEND_KMS_OVERLAY)) {
            const QFileInfo deviceInfo(m_cameraDevice);
            if (deviceInfo.exists()) {
                setCameraStatus(QStringLiteral("待出帧"), QStringLiteral("#f4b942"));
            } else {
                setCameraStatus(QStringLiteral("离线"), QStringLiteral("#ef5b5b"));
            }
            return;
        }

        m_overlayProbeRunning = true;

        /* self 用于后台线程结束后判断控制器是否还存在，避免窗口关闭时访问悬空对象。 */
        QPointer<DeviceHealthController> self(this);

        /* socketPath 复制当前 overlay 控制端点，后台线程只读副本，不读写 QObject 成员。 */
        const QString socketPath = m_overlaySocket;

        /* workerThread 承载 socket connect/read 的短超时等待，确保 QML 主线程只负责调度和显示。 */
        QThread *workerThread = QThread::create([self, socketPath]() {
            const QString reply = queryOverlayStatus(socketPath);

            if (!self) {
                return;
            }

            QMetaObject::invokeMethod(self.data(),
                                      "handleOverlayProbeFinished",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, reply));
        });

        if (workerThread == nullptr) {
            m_overlayProbeRunning = false;
            handleCameraOffline(QStringLiteral("相机检测线程创建失败"));
            return;
        }

        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);
        workerThread->start();
    }

    /*
     * applyOverlayStatusReply 的作用：
     *   在 Qt 主线程解析 overlay STATUS 回复，并更新摄像头状态。
     *
     * 参数：
     *   reply 是后台线程通过 overlay 控制 socket 读取到的一行状态文本。
     *
     * 返回值：
     *   无返回值；成功时显示“在线”，失败时进入离线处理和重启冷却逻辑。
     */
    void applyOverlayStatusReply(const QString &reply)
    {
        if (!reply.startsWith(QStringLiteral("OK STATUS "))) {
            handleCameraOffline(QStringLiteral("Overlay未连接"));
            return;
        }

        const bool hasFrame = tokenValue(reply, QStringLiteral("has_frame")) == QStringLiteral("1");
        const unsigned int serial = tokenValue(reply, QStringLiteral("serial")).toUInt();
        if (!hasFrame || serial == 0U) {
            handleCameraOffline(QStringLiteral("相机未出帧"));
            return;
        }

        if (m_lastOverlaySerial != 0U && serial == m_lastOverlaySerial) {
            handleCameraOffline(QStringLiteral("相机画面停滞"));
            return;
        }

        m_cameraOfflineCount = 0;
        m_lastOverlaySerial = serial;
        setCameraStatus(QStringLiteral("在线"), QStringLiteral("#35d07f"));
    }

    /*
     * handleCameraOffline 的作用：
     *   更新相机离线状态，并在连续失败后尝试重启 overlay 链路。
     */
    void handleCameraOffline(const QString &reason)
    {
        m_cameraOfflineCount++;
        setCameraStatus(reason, QStringLiteral("#ef5b5b"));
        setDetailText(QStringLiteral("相机离线：") + reason);

        if (m_overlayRestartCooldown > 0) {
            m_overlayRestartCooldown--;
            return;
        }

        if (m_cameraOfflineCount >= 2) {
            startDetachedOverlay();
            m_lastOverlaySerial = 0U;
            m_overlayRestartCooldown = 2;
            m_cameraOfflineCount = 0;
        }
    }

    /*
     * startDetachedOverlay 的作用：
     *   在相机离线后后台重启 KMS overlay 视频进程。
     *
     * 关键说明：
     *   使用 restart-overlay 而不是 restart，只重新初始化 UVC/DRM 视频进程，不杀 Qt 主界面。
     *   使用 startDetached，不等待脚本执行完成，避免摄像头热拔插恢复时卡住 Qt 事件循环。
     */
    void startDetachedOverlay()
    {
        const QFileInfo scriptInfo(m_overlayRestartScript);

        if (!scriptInfo.exists() || !scriptInfo.isExecutable()) {
            setDetailText(QStringLiteral("相机 overlay 重启脚本不可执行"));
            return;
        }

        if (QProcess::startDetached(m_overlayRestartScript, QStringList() << QStringLiteral("restart-overlay"))) {
            setDetailText(QStringLiteral("已请求重启相机 overlay 进程"));
        } else {
            setDetailText(QStringLiteral("相机 overlay 重启命令启动失败"));
        }
    }

    /*
     * startF4Probe 的作用：
     *   在后台线程里执行 F4 二进制心跳，避免串口等待阻塞 QML。
     *
     * 参数：
     *   forceNow 为 true 时立即发送 HEARTBEAT，适合人工点击刷新或标定前检查；
     *   forceNow 为 false 时按 2 分钟间隔发送心跳，避免 8 秒健康刷新频繁占用 RS485。
     *
     * 返回值：
     *   无返回值；后台线程完成后调用 handleF4ProbeFinished() 回写状态。
     */
    void startF4Probe(bool forceNow)
    {
        if (m_f4ProbeRunning || m_f4CommandRunning) {
            return;
        }

        if (!forceNow
                && m_f4HeartbeatElapsed.isValid()
                && m_f4HeartbeatElapsed.elapsed() < F4_HEARTBEAT_INTERVAL_MS) {
            return;
        }

        m_f4ProbeRunning = true;
        m_f4HeartbeatElapsed.restart();
        QPointer<DeviceHealthController> self(this);
        const QString dev = m_f4Device;
        const int baud = m_f4Baud;
        const quint16 sequence = m_f4BinarySequence++;
        const QByteArray frame = buildF4BinaryFrame(BINARY_PROTOCOL_CMD_HEARTBEAT, sequence, QByteArray());

        QThread *workerThread = QThread::create([self, dev, baud, frame, sequence]() {
            QString detail;
            const bool ok = sendF4BinaryHeartbeat(dev, baud, frame, sequence, &detail);

            if (!self) {
                return;
            }

            QMetaObject::invokeMethod(self.data(),
                                      "handleF4ProbeFinished",
                                      Qt::QueuedConnection,
                                      Q_ARG(bool, ok),
                                      Q_ARG(QString, detail));
        });

        if (workerThread == nullptr) {
            m_f4ProbeRunning = false;
            setF4Status(QStringLiteral("待接入"), QStringLiteral("#f4b942"));
            setDetailText(QStringLiteral("F4 检测线程创建失败"));
            return;
        }

        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);
        workerThread->start();
    }

    /*
     * startF4VisionCommand 的作用：
     *   统一发送自动视觉闭环相关的 F4 二进制命令。
     *
     * 主要流程：
     *   1. 复用 m_f4CommandRunning，避免视觉坐标、手动命令、标定和首页自动控制同时抢串口。
     *   2. 使用当前全局二进制 sequence 组帧，保证 F4 ACK/NACK 能精确匹配本次命令。
     *   3. 后台线程调用 sendF4BinaryCommand 等待匹配 ACK/NACK，完成后回到主线程释放忙标志。
     *
     * 参数：
     *   action 是给 QML 和日志看的动作名，例如 VISION_POS。
     *   command 是要写入协议 CMD 字段的命令字。
     *   cycleId 是本次命令归属的自动流程号。
     *   payload 是已经按协议编码好的负载。
     *
     * 返回值：
     *   true 表示后台发送任务已启动；false 表示串口忙或线程创建失败。
     */
    bool startF4VisionCommand(const QString &action,
                              quint8 command,
                              quint16 cycleId,
                              const QByteArray &payload)
    {
        if (m_f4CommandRunning) {
            emit f4VisionCommandFinished(false,
                                         action,
                                         cycleId,
                                         QStringLiteral("上一条F4串口命令仍在发送中"));
            return false;
        }

        if (m_f4ProbeRunning) {
            emit f4VisionCommandFinished(false,
                                         action,
                                         cycleId,
                                         QStringLiteral("F4状态刷新仍在进行，请稍后再发送视觉闭环命令"));
            return false;
        }

        const quint16 sequence = m_f4BinarySequence++;
        const QByteArray frame = buildF4BinaryFrame(command, sequence, payload);
        const QString dev = m_f4Device;
        const int baud = m_f4Baud;
        m_f4CommandRunning = true;

        QPointer<DeviceHealthController> self(this);
        QThread *workerThread = QThread::create([self, dev, baud, frame, action, command, sequence, cycleId]() {
            QString detail;
            const bool ok = (command == BINARY_PROTOCOL_CMD_ACTUATOR_POS_MOVE)
                    ? runF4ActuatorPositionMoveAndWaitDone(dev,
                                                           baud,
                                                           frame,
                                                           sequence,
                                                           cycleId,
                                                           MP157_ACTUATOR_FALLBACK_MAX_MS,
                                                           &detail)
                    : sendF4BinaryCommand(dev, baud, frame, command, sequence, cycleId, &detail);

            if (!self) {
                return;
            }

            QMetaObject::invokeMethod(self.data(),
                                      "handleF4VisionCommandFinished",
                                      Qt::QueuedConnection,
                                      Q_ARG(bool, ok),
                                      Q_ARG(QString, action),
                                      Q_ARG(quint16, cycleId),
                                      Q_ARG(QString, detail));
        });

        if (workerThread == nullptr) {
            m_f4CommandRunning = false;
            emit f4VisionCommandFinished(false,
                                         action,
                                         cycleId,
                                         QStringLiteral("F4视觉闭环命令线程创建失败"));
            return false;
        }

        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);
        workerThread->start();
        return true;
    }

    /*
     * startF4ActuatorCommand 的作用：
     *   统一发送执行器位置移动和停止类 F4 二进制命令。
     *
     * 主要流程：
     *   1. 使用 m_f4CommandRunning 统一串口互斥，避免自动视觉、手动运动和心跳同抢 `/dev/ttySTM2`。
     *   2. 生成全局 sequence 并组装完整二进制帧。
     *   3. 后台线程调用 sendF4BinaryCommand() 等待匹配 ACK/NACK。
     *   4. 主线程 handleF4ActuatorCommandFinished() 释放忙标志并通知 QML。
     *
     * 参数：
     *   action 是给 QML 和日志使用的动作名。
     *   command 是要写入协议 CMD 字段的命令字。
     *   cycleId 是本次命令归属的流程号，手动命令允许为 0。
     *   payload 是已经编码好的协议负载。
     *   positionFallbackMaxWaitMs 是 ACTUATOR_POS_MOVE 等待 F4 DONE 的 MP157 本地最大兜底时长。
     *
     * 返回值：
     *   true 表示后台任务已启动；false 表示串口忙或线程创建失败。
     */
    bool startF4ActuatorCommand(const QString &action,
                                quint8 command,
                                quint16 cycleId,
                                const QByteArray &payload,
                                int positionFallbackMaxWaitMs = MP157_ACTUATOR_FALLBACK_MAX_MS)
    {
        if (m_f4CommandRunning) {
            emit f4ActuatorCommandFinished(false,
                                           action,
                                           cycleId,
                                           QStringLiteral("上一条F4串口命令仍在发送中"));
            return false;
        }

        if (m_f4ProbeRunning) {
            emit f4ActuatorCommandFinished(false,
                                           action,
                                           cycleId,
                                           QStringLiteral("F4状态刷新仍在进行，请稍后再发送执行器命令"));
            return false;
        }

        const quint16 sequence = m_f4BinarySequence++;
        const QByteArray frame = buildF4BinaryFrame(command, sequence, payload);
        const QString dev = m_f4Device;
        const int baud = m_f4Baud;
        const QSharedPointer<QMutex> serialWriteMutex = m_f4SerialWriteMutex;
        const QSharedPointer<std::atomic<quint64>> stopGeneration = m_f4ActuatorStopGeneration;
        const quint64 stopGenerationAtStart = stopGeneration.isNull()
                ? 0U
                : stopGeneration->load(std::memory_order_acquire);
        m_f4CommandRunning = true;

        QPointer<DeviceHealthController> self(this);
        QThread *workerThread = QThread::create([self,
                                                  dev,
                                                  baud,
                                                  frame,
                                                  action,
                                                  command,
                                                  sequence,
                                                  cycleId,
                                                  positionFallbackMaxWaitMs,
                                                  serialWriteMutex,
                                                  stopGeneration,
                                                  stopGenerationAtStart]() {
            QString detail;
            const bool ok = (command == BINARY_PROTOCOL_CMD_ACTUATOR_POS_MOVE)
                    ? runF4ActuatorPositionMoveAndWaitDone(dev,
                                                           baud,
                                                           frame,
                                                           sequence,
                                                           cycleId,
                                                           positionFallbackMaxWaitMs,
                                                           &detail,
                                                           serialWriteMutex,
                                                           stopGeneration,
                                                           stopGenerationAtStart,
                                                           action)
                    : sendF4BinaryCommand(dev,
                                          baud,
                                          frame,
                                          command,
                                          sequence,
                                          cycleId,
                                          &detail,
                                          serialWriteMutex,
                                          stopGeneration,
                                          stopGenerationAtStart,
                                          action);

            if (!self) {
                return;
            }

            QMetaObject::invokeMethod(self.data(),
                                      "handleF4ActuatorCommandFinished",
                                      Qt::QueuedConnection,
                                      Q_ARG(bool, ok),
                                      Q_ARG(QString, action),
                                      Q_ARG(quint16, cycleId),
                                      Q_ARG(QString, detail));
        });

        if (workerThread == nullptr) {
            m_f4CommandRunning = false;
            emit f4ActuatorCommandFinished(false,
                                           action,
                                           cycleId,
                                           QStringLiteral("F4执行器命令线程创建失败"));
            return false;
        }

        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);
        workerThread->start();
        return true;
    }

    /*
     * startF4ActuatorStopNowCommand 的作用：
     *   为手动停止键启动一个不占用普通 F4 命令 busy 标志的强制 STOP 写入线程。
     *
     * 主要流程：
     *   1. 生成新的二进制 sequence，组装 ACTUATOR_STOP 完整帧。
     *   2. 后台线程调用 writeF4BinaryFrameWithoutReply()，只负责把 STOP 帧写入并 tcdrain。
     *   3. 回到主线程调用 handleF4ActuatorStopNowFinished()，该槽不清 m_f4CommandRunning。
     *
     * 参数：
     *   action 固定为 ACTUATOR_STOP_NOW，用于 QML 区分这是强制停止写入结果。
     *   cycleId 是本次 STOP 使用的流程号，手动调试通常为 0。
     *   payload 是已经编码好的 ACTUATOR_STOP 负载。
     *
     * 返回值：
     *   true 表示后台线程已启动；false 表示线程创建失败。
     */
    bool startF4ActuatorStopNowCommand(const QString &action,
                                       quint16 cycleId,
                                       const QByteArray &payload)
    {
        /*
         * STOP 是安全抢占动作。先递增代际，再启动写线程：
         * - 如果旧运动线程还没写帧，它进入写锁后会发现代际变化并取消；
         * - 如果旧运动线程已经在写帧，STOP 会在同一写锁后排队写出，覆盖刚启动的运动。
         */
        if (!m_f4ActuatorStopGeneration.isNull()) {
            m_f4ActuatorStopGeneration->fetch_add(1U, std::memory_order_acq_rel);
        }

        const quint16 sequence = m_f4BinarySequence++;
        const QByteArray frame = buildF4BinaryFrame(BINARY_PROTOCOL_CMD_ACTUATOR_STOP, sequence, payload);
        const QString dev = m_f4Device;
        const int baud = m_f4Baud;
        const QSharedPointer<QMutex> serialWriteMutex = m_f4SerialWriteMutex;

        QPointer<DeviceHealthController> self(this);
        QThread *workerThread = QThread::create([self, dev, baud, frame, action, cycleId, sequence, serialWriteMutex]() {
            QString detail;        /* detail 保存写入线程生成的结果说明，回到主线程后显示到 QML。 */
            const bool ok = writeF4BinaryFrameWithoutReply(dev,
                                                           baud,
                                                           frame,
                                                           &detail,
                                                           F4_ACTUATOR_STOP_NOW_REPEAT_COUNT,
                                                           F4_ACTUATOR_STOP_NOW_REPEAT_DELAY_US,
                                                           serialWriteMutex);

            if (!detail.isEmpty()) {
                detail += QStringLiteral(" seq=") + QString::number(sequence);
            }

            if (!self) {
                return;
            }

            QMetaObject::invokeMethod(self.data(),
                                      "handleF4ActuatorStopNowFinished",
                                      Qt::QueuedConnection,
                                      Q_ARG(bool, ok),
                                      Q_ARG(QString, action),
                                      Q_ARG(quint16, cycleId),
                                      Q_ARG(QString, detail));
        });

        if (workerThread == nullptr) {
            emit f4ActuatorCommandFinished(false,
                                           action,
                                           cycleId,
                                           QStringLiteral("F4强制停止线程创建失败"));
            return false;
        }

        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);
        workerThread->start();
        return true;
    }

    /*
     * refreshSdcardStatus 的作用：
     *   读取 /proc/mounts 判断 SD 卡挂载状态；这是轻量本地读取，可以同步执行。
     */
    void refreshSdcardStatus()
    {
        FILE *mounts = std::fopen("/proc/mounts", "r");
        QByteArray expected = m_sdcardMount.toLocal8Bit();
        char device[256];
        char path[4096];
        bool mounted = false;

        if (mounts != nullptr) {
            while (std::fscanf(mounts, "%255s %4095s %*s %*s %*d %*d\n", device, path) == 2) {
                if (std::strcmp(path, expected.constData()) == 0) {
                    mounted = true;
                    break;
                }
            }
            std::fclose(mounts);
        }

        if (!mounted) {
            setSdcardStatus(QStringLiteral("未挂载"), QStringLiteral("#f4b942"));
            return;
        }

        const QFileInfo mountInfo(m_sdcardMount);
        if (!mountInfo.isDir() || !mountInfo.isWritable()) {
            setSdcardStatus(QStringLiteral("不可写"), QStringLiteral("#ef5b5b"));
            setDetailText(QStringLiteral("SD 卡已挂载但不可写：") + m_sdcardMount);
            return;
        }

        setSdcardStatus(QStringLiteral("可写"), QStringLiteral("#35d07f"));
    }

    /*
     * tokenValue 的作用：
     *   从 `key=value` 状态文本中提取字段值。
     */
    QString tokenValue(const QString &text, const QString &key) const
    {
        const QString prefix = key + QLatin1Char('=');
        const QStringList parts = text.split(QLatin1Char(' '), QString::SkipEmptyParts);

        for (const QString &part : parts) {
            if (part.startsWith(prefix)) {
                return part.mid(prefix.length());
            }
        }
        return QString();
    }

    /*
     * parseKeyValueOutput 的作用：
     *   解析 4g-location 输出的多行 key=value 文本，供定位完成回调读取 state/display 等字段。
     *
     * 主要流程：
     *   1. 按 CR/LF 拆分输出，兼容 BusyBox shell 在不同环境下的换行。
     *   2. 每行只按第一个等号切分，避免 detail 中带等号时被截断。
     *   3. 空键名跳过，保留空值字段，便于 city/latitude 这类 IP 定位空字段保持明确语义。
     *
     * 参数：
     *   output 是 `4g-location once` 的 stdout。
     *
     * 返回值：
     *   返回字段名到字段值的映射；未出现的字段由调用方使用默认值处理。
     */
    QMap<QString, QString> parseKeyValueOutput(const QString &output) const
    {
        QMap<QString, QString> values;
        const QStringList lines = output.split(QRegExp(QStringLiteral("[\\r\\n]+")), QString::SkipEmptyParts);

        for (const QString &line : lines) {
            const int equalIndex = line.indexOf(QLatin1Char('='));
            if (equalIndex <= 0) {
                continue;
            }

            const QString key = line.left(equalIndex).trimmed();
            const QString value = line.mid(equalIndex + 1).trimmed();
            if (key.isEmpty()) {
                continue;
            }

            values.insert(key, value);
        }

        return values;
    }

    /*
     * overlayReplyTimeoutMs 的作用：
     *   根据 overlay 控制命令类型选择回复等待时间。
     *
     * 主要流程：
     *   1. LOCATE 是自动检测启动后的高频视觉定位命令，overlay 需要等采集主循环轮询到 socket 后再运行定位算法。
     *   2. STATUS 只返回短状态行，仍使用短等待，避免健康刷新在 overlay 异常时占住后台线程。
     *
     * 参数：
     *   commandBytes 是准备写入 overlay socket 的原始命令，通常带换行。
     *
     * 返回值：
     *   返回本命令读取回复时允许等待的毫秒数。
     */
    static int overlayReplyTimeoutMs(const QByteArray &commandBytes)
    {
        if (commandBytes.startsWith(QByteArrayLiteral("LOCATE"))) {
            return OVERLAY_CONTROL_LOCATE_REPLY_TIMEOUT_MS;
        }

        return OVERLAY_CONTROL_STATUS_REPLY_TIMEOUT_MS;
    }

    /*
     * fillTimeoutValue 的作用：
     *   把毫秒超时值转换成 POSIX select() 需要的 timeval。
     *
     * 参数：
     *   timeoutMs 是等待时间，单位 ms；负数会被按 0 处理。
     *   tv 是输出 timeval 指针。
     *
     * 返回值：
     *   无返回值；tv 会被写入秒和微秒字段。
     */
    static void fillTimeoutValue(int timeoutMs, struct timeval *tv)
    {
        const int safeTimeoutMs = std::max(0, timeoutMs);

        tv->tv_sec = safeTimeoutMs / 1000;
        tv->tv_usec = (safeTimeoutMs % 1000) * 1000;
    }

    /*
     * queryOverlayControlCommand 的作用：
     *   连接 overlay 控制 socket，发送一条短命令并读取一行回复。
     *
     * 参数：
     *   socketPath 是 overlay 控制 socket 路径。
     *   commandBytes 是带换行的 overlay 命令，例如 `STATUS\n` 或 `LOCATE\n`。
     *
     * 返回值：
     *   成功返回 overlay 的 `OK ...` 文本；失败返回 `ERR ...`，由调用方决定如何显示。
     */
    static QString queryOverlayControlCommand(const QString &socketPath, const QByteArray &commandBytes)
    {
        int fd = -1;                                           /* fd 保存本次 Unix socket 连接，函数退出前必须关闭。 */
        struct sockaddr_un addr;                                /* addr 保存 overlay 控制 socket 的本地路径地址。 */
        QByteArray socketPathBytes = socketPath.toLocal8Bit();   /* socketPathBytes 是 POSIX connect() 需要的窄字节路径。 */
        char buffer[OVERLAY_CONTROL_REPLY_CHUNK_SIZE];           /* buffer 是单次 read 缓冲，完整回复由 reply 循环追加。 */
        QByteArray reply;                                       /* reply 保存读到换行为止的完整 overlay 回复。 */
        fd_set wfds;                                            /* wfds 用于等待非阻塞 connect 完成。 */
        fd_set rfds;                                            /* rfds 用于等待 overlay 回复可读。 */
        struct timeval tv;                                      /* tv 是每次 select() 的剩余等待时间。 */
        int optError = 0;                                       /* optError 保存 getsockopt(SO_ERROR) 返回的连接结果。 */
        socklen_t optLen = sizeof(optError);                    /* optLen 是 getsockopt() 参数长度。 */
        const int replyTimeoutMs = overlayReplyTimeoutMs(commandBytes); /* replyTimeoutMs 按命令类型区分 STATUS 短等待和 LOCATE 长等待。 */
        QElapsedTimer replyTimer;                               /* replyTimer 统计读取完整回复的总耗时，避免循环 read 无限等待。 */

        if (commandBytes.isEmpty()) {
            return QStringLiteral("ERR overlay命令为空");
        }

        if (socketPathBytes.size() >= static_cast<int>(sizeof(addr.sun_path))) {
            return QStringLiteral("ERR socket路径过长");
        }

        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return QStringLiteral("ERR 创建socket失败");
        }

        /* 设置非阻塞，避免 overlay 进程异常时 connect/read 长时间等待。 */
        const int oldFlags = fcntl(fd, F_GETFL, 0);
        if (oldFlags >= 0) {
            fcntl(fd, F_SETFL, oldFlags | O_NONBLOCK);
        }

        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socketPathBytes.constData(), sizeof(addr.sun_path) - 1U);

        if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
            if (errno != EINPROGRESS) {
                ::close(fd);
                return QStringLiteral("ERR overlay未连接");
            }

            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            fillTimeoutValue(OVERLAY_CONTROL_CONNECT_TIMEOUT_MS, &tv);
            if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0) {
                ::close(fd);
                return QStringLiteral("ERR overlay连接超时");
            }

            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &optError, &optLen) != 0 || optError != 0) {
                ::close(fd);
                return QStringLiteral("ERR overlay连接失败");
            }
        }

        if (oldFlags >= 0) {
            fcntl(fd, F_SETFL, oldFlags);
        }

        if (!writeAllToFd(fd, commandBytes)) {
            ::close(fd);
            return QStringLiteral("ERR 发送失败");
        }

        replyTimer.start();
        while (true) {
            const int elapsedMs = static_cast<int>(replyTimer.elapsed());
            const int remainingMs = replyTimeoutMs - elapsedMs;

            if (remainingMs <= 0) {
                ::close(fd);
                return QStringLiteral("ERR overlay回复超时");
            }

            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            fillTimeoutValue(remainingMs, &tv);

            const int selectRet = select(fd + 1, &rfds, NULL, NULL, &tv);
            if (selectRet < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ::close(fd);
                return QStringLiteral("ERR overlay读取失败");
            }
            if (selectRet == 0) {
                ::close(fd);
                return QStringLiteral("ERR overlay回复超时");
            }

            const ssize_t nread = ::read(fd, buffer, sizeof(buffer));
            if (nread < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ::close(fd);
                return QStringLiteral("ERR overlay读取失败");
            }
            if (nread == 0) {
                break;
            }

            reply.append(buffer, static_cast<int>(nread));
            if (reply.size() > OVERLAY_CONTROL_MAX_REPLY_BYTES) {
                ::close(fd);
                return QStringLiteral("ERR overlay回复过长");
            }
            if (reply.contains('\n')) {
                break;
            }
        }

        ::close(fd);
        if (reply.isEmpty()) {
            return QStringLiteral("ERR 无回复");
        }

        reply = reply.trimmed();
        return QString::fromLocal8Bit(reply);
    }

    /*
     * queryOverlayStatus 的作用：
     *   发送 overlay `STATUS` 查询并读取回复。
     *
     * 参数：
     *   socketPath 是 overlay 控制 socket 路径。
     *
     * 返回值：
     *   成功返回 `OK STATUS ...`；失败返回 `ERR ...`，由主线程统一转为离线状态。
     */
    static QString queryOverlayStatus(const QString &socketPath)
    {
        return queryOverlayControlCommand(socketPath, QByteArrayLiteral("STATUS\n"));
    }

    /*
     * baudToSpeed 的作用：
     *   把整数波特率转换成 termios 常量。
     */
    static speed_t baudToSpeed(int baud)
    {
        switch (baud) {
        case 9600:
            return B9600;
        case 19200:
            return B19200;
        case 38400:
            return B38400;
        case 57600:
            return B57600;
        case 115200:
            return B115200;
        default:
            return B57600;
        }
    }

    /*
     * f4ActuatorStopGenerationChanged 的作用：
     *   判断当前强制 STOP 代际是否已经不同于普通运动命令启动时捕获的代际。
     *
     * 主要流程：
     *   1. 如果没有传入代际对象，说明调用方不需要取消保护，直接返回 false。
     *   2. 用 acquire 读取原子值，保证后台线程能看到 STOP 线程已经发布的代际递增。
     *   3. 只比较代际值，不读取 QObject 成员，避免后台线程在对象销毁时访问悬空指针。
     *
     * 参数：
     *   stopGeneration 是共享的 STOP 代际原子对象。
     *   generationAtStart 是普通运动命令启动时捕获的代际。
     *
     * 返回值：
     *   true 表示 STOP 已经抢占，旧运动命令不能再写入串口或继续等待完成事件。
     */
    static bool f4ActuatorStopGenerationChanged(const QSharedPointer<std::atomic<quint64>> &stopGeneration,
                                                quint64 generationAtStart)
    {
        if (stopGeneration.isNull()) {
            return false;
        }

        return stopGeneration->load(std::memory_order_acquire) != generationAtStart;
    }

    /*
     * f4ActuatorStopCanceledDetail 的作用：
     *   生成普通执行器命令被强制 STOP 抢占后的统一诊断文本。
     *
     * 参数：
     *   action 是被取消的命令名称，例如 ACTUATOR_VEL_MOVE 或 ACTUATOR_POS_MOVE。
     *
     * 返回值：
     *   返回给 QML/F4 日志显示的中文取消原因。
     */
    static QString f4ActuatorStopCanceledDetail(const QString &action)
    {
        return QStringLiteral("ACTUATOR_STOP 抢占：取消尚未写入或仍在等待完成的 ")
                + action;
    }

    /*
     * readF4BinaryReply 的作用：
     *   从 F4 串口读取一帧完整二进制协议回复，并完成帧头、长度、帧尾和 CRC 校验。
     *
     * 主要流程：
     *   1. 通过 select 进行短周期等待，避免 F4 没回复时后台线程长时间阻塞。
     *   2. 把多次 read 的数据追加到 buffer，允许串口粘包、半包和上电调试日志。
     *   3. 调用 parseF4BinaryFrameFromBuffer() 找到第一帧合法二进制帧。
     *
     * 参数：
     *   fd 是已经打开并配置好的 F4 串口文件描述符。
     *   reply 用于返回合法二进制帧解析结果，不能为 NULL。
     *   errorText 用于返回失败原因，可为 NULL。
     *
     * 返回值：
     *   成功解析出合法帧返回 true；超时、read 失败或只收到坏帧返回 false。
     */
    static bool readF4BinaryReply(int fd, F4BinaryReply *reply, QString *errorText)
    {
        QByteArray buffer;       /* buffer 保存累计收到的串口字节，解析函数会从中丢弃噪声和已消费帧。 */
        char chunk[128];         /* chunk 是单次 read 的临时缓冲，大小足够容纳首版 58 字节以内短帧。 */
        QElapsedTimer elapsed;   /* elapsed 限制总等待时间，避免 F4 未回 ACK 时后台线程长时间占用。 */
        QString lastFrameError;  /* lastFrameError 保存最近一帧坏帧原因，超时时优先反馈给界面。 */

        elapsed.start();
        while (elapsed.elapsed() < 700) {
            fd_set rfds;         /* rfds 是 select 读取集合，只等待当前串口 fd 可读。 */
            struct timeval tv;   /* tv 是每轮 80ms 短等待，兼顾 ACK 及时性和 CPU 占用。 */

            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            tv.tv_sec = 0;
            tv.tv_usec = 80000;

            const int selected = select(fd + 1, &rfds, NULL, NULL, &tv);
            if (selected < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errorText) {
                    *errorText = QStringLiteral("读取 F4 二进制回复失败");
                }
                return false;
            }

            if (selected == 0) {
                continue;
            }

            const ssize_t nread = ::read(fd, chunk, sizeof(chunk));
            if (nread > 0) {
                buffer.append(chunk, static_cast<int>(nread));
                if (parseF4BinaryFrameFromBuffer(&buffer, reply, &lastFrameError)) {
                    /*
                     * FAULT_REPORT 是 F4 的异步故障上报，不一定对应当前按钮命令。
                     * 例如当前未接 LDC 时，F4 会周期上报 LDC 故障；此时 Qt 记录故障摘要，
                     * 但继续等待当前 HEARTBEAT/ACK/NACK/STATUS_REPORT，避免把异步故障帧误当作控制命令失败。
                     */
                    if (reply->command == BINARY_PROTOCOL_CMD_FAULT_REPORT) {
                        lastFrameError = describeF4FaultReport(*reply);
                        continue;
                    }
                    /*
                     * 如果读到 HEARTBEAT/START_CYCLE 等小于 0x80 的合法帧，
                     * 说明当前串口链路把 MP157 自己发出的请求帧回显到了 RX，
                     * 或者读取到了写入前遗留的旧请求帧。它不是 F4 回包，必须丢弃后继续等 ACK/NACK。
                     */
                    if (isMp157ToF4RequestCommand(reply->command)) {
                        lastFrameError = QStringLiteral("跳过MP157请求帧/串口回显：")
                                + f4BinaryCommandName(reply->command)
                                + QStringLiteral(" raw=")
                                + hexByteString(reply->rawFrame);
                        continue;
                    }
                    return true;
                }
                continue;
            }

            if (nread == 0) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }

            if (errorText) {
                *errorText = QStringLiteral("读取 F4 二进制回复失败");
            }
            return false;
        }

        if (errorText) {
            if (!lastFrameError.isEmpty()) {
                *errorText = lastFrameError;
            } else if (!buffer.isEmpty()) {
                *errorText = QStringLiteral("F4二进制回复不完整：") + hexByteString(buffer);
            } else {
                *errorText = QStringLiteral("未收到 F4 二进制 ACK/NACK");
            }
        }
        return false;
    }

    /*
     * exchangeF4BinaryFrame 的作用：
     *   打开 F4 串口、发送一帧二进制协议、读取一帧合法二进制回复。
     *
     * 主要流程：
     *   1. 使用 termios raw 配置 `/dev/ttySTM2`，避免文本终端处理修改二进制字节。
     *   2. 写入完整帧并等待内核发送队列排空。
     *   3. 调用 readF4BinaryReply() 从混有调试文本的串口流中抓取合法二进制帧。
     *
     * 参数：
     *   device 是 Linux 串口节点。
     *   baud 是串口波特率。
     *   frame 是要发送的完整二进制帧。
     *   reply 用于返回解析后的二进制回复，不能为 NULL。
     *   detail 用于返回失败原因，可为 NULL。
     *
     * 返回值：
     *   成功收到合法二进制回复返回 true；打开、配置、写入、超时或 CRC 失败返回 false。
     */
    static bool exchangeF4BinaryFrame(const QString &device,
                                      int baud,
                                      const QByteArray &frame,
                                      F4BinaryReply *reply,
                                      QString *detail,
                                      const QSharedPointer<QMutex> &serialWriteMutex = QSharedPointer<QMutex>(),
                                      const QSharedPointer<std::atomic<quint64>> &stopGeneration = QSharedPointer<std::atomic<quint64>>(),
                                      quint64 stopGenerationAtStart = 0U,
                                      const QString &cancelAction = QString())
    {
        const QByteArray devBytes = device.toLocal8Bit();
        int fd = ::open(devBytes.constData(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        struct termios tio;
        QString readErrorText;

        if (frame.isEmpty()) {
            if (detail) {
                *detail = QStringLiteral("F4二进制命令帧为空");
            }
            return false;
        }

        if (reply == nullptr) {
            if (detail) {
                *detail = QStringLiteral("F4二进制回复对象为空");
            }
            return false;
        }

        if (fd < 0) {
            if (detail) {
                *detail = QStringLiteral("无法打开 ") + device;
            }
            return false;
        }

        if (tcgetattr(fd, &tio) != 0) {
            if (detail) {
                *detail = QStringLiteral("读取串口属性失败");
            }
            ::close(fd);
            return false;
        }

        cfmakeraw(&tio);
        cfsetispeed(&tio, baudToSpeed(baud));
        cfsetospeed(&tio, baudToSpeed(baud));
        tio.c_cflag |= CLOCAL | CREAD;
#ifdef CRTSCTS
        tio.c_cflag &= ~CRTSCTS;
#endif
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;

        if (tcsetattr(fd, TCSANOW, &tio) != 0) {
            if (detail) {
                *detail = QStringLiteral("配置串口失败");
            }
            ::close(fd);
            return false;
        }

        {
            /*
             * 普通运动帧和强制 STOP 帧共用 `/dev/ttySTM2`。
             * 这里只把配置后的 flush/write/drain 短窗口纳入互斥，读取 ACK 阶段不持锁，
             * 否则 STOP 会被普通命令的 ACK 等待卡住。
             */
            QMutexLocker writeLocker(serialWriteMutex.data());

            if (f4ActuatorStopGenerationChanged(stopGeneration, stopGenerationAtStart)) {
                if (detail) {
                    *detail = f4ActuatorStopCanceledDetail(cancelAction);
                }
                ::close(fd);
                return false;
            }

            tcflush(fd, TCIOFLUSH);
            if (!writeAllToFd(fd, frame)) {
                if (detail) {
                    *detail = QStringLiteral("写入 F4 二进制命令失败：") + hexByteString(frame);
                }
                ::close(fd);
                return false;
            }

            if (tcdrain(fd) != 0) {
                if (detail) {
                    *detail = QStringLiteral("等待 F4 二进制命令发送完成失败");
                }
                ::close(fd);
                return false;
            }
        }

        if (!readF4BinaryReply(fd, reply, &readErrorText)) {
            if (detail) {
                *detail = readErrorText.isEmpty() ? QStringLiteral("F4二进制回复为空") : readErrorText;
            }
            ::close(fd);
            return false;
        }

        ::close(fd);
        return true;
    }

    /*
     * writeF4BinaryFrameWithoutReply 的作用：
     *   只向 F4 串口写入一帧二进制协议，不读取 ACK/NACK。
     *
     * 主要流程：
     *   1. 打开 `/dev/ttySTM2` 并配置为 57600 8N1 raw 模式。
     *   2. 写入完整二进制帧，并调用 tcdrain() 等待内核发送队列排空。
     *   3. STOP 是幂等安全动作，允许按 repeatCount 重复写入，覆盖手动停止和普通 ACK 等待重叠的窗口。
     *   4. 不调用 readF4BinaryReply()，避免强制 STOP 和上一条普通命令线程同时抢读回包。
     *
     * 参数：
     *   device 是 Linux 串口节点。
     *   baud 是串口波特率。
     *   frame 是要写入的完整二进制帧。
     *   detail 返回写入结果或失败原因，可为 NULL。
     *   repeatCount 是同一 STOP 帧重复写入次数，小于 1 时按 1 次处理。
     *   repeatDelayUs 是重复写入之间的短间隔，单位 us。
     *
     * 返回值：
     *   完整写入并排空发送队列返回 true；打开、配置、写入或 tcdrain 失败返回 false。
     */
    static bool writeF4BinaryFrameWithoutReply(const QString &device,
                                               int baud,
                                               const QByteArray &frame,
                                               QString *detail,
                                               int repeatCount = 1,
                                               int repeatDelayUs = 0,
                                               const QSharedPointer<QMutex> &serialWriteMutex = QSharedPointer<QMutex>())
    {
        const QByteArray devBytes = device.toLocal8Bit();
        int fd = -1;              /* fd 保存本次强制 STOP 独立打开的串口文件描述符。 */
        struct termios tio;       /* tio 保存串口 raw 配置，保证二进制帧不会被行规程改写。 */
        const int safeRepeatCount = std::max(1, repeatCount);

        if (frame.isEmpty()) {
            if (detail) {
                *detail = QStringLiteral("F4强制停止帧为空");
            }
            return false;
        }

        fd = ::open(devBytes.constData(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            if (detail) {
                *detail = QStringLiteral("无法打开 ") + device;
            }
            return false;
        }

        if (tcgetattr(fd, &tio) != 0) {
            if (detail) {
                *detail = QStringLiteral("读取串口属性失败");
            }
            ::close(fd);
            return false;
        }

        cfmakeraw(&tio);
        cfsetispeed(&tio, baudToSpeed(baud));
        cfsetospeed(&tio, baudToSpeed(baud));
        tio.c_cflag |= CLOCAL | CREAD;
#ifdef CRTSCTS
        tio.c_cflag &= ~CRTSCTS;
#endif
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;

        if (tcsetattr(fd, TCSANOW, &tio) != 0) {
            if (detail) {
                *detail = QStringLiteral("配置串口失败");
            }
            ::close(fd);
            return false;
        }

        {
            /*
             * STOP 与普通运动命令共用同一条 MP157-F4 TTY。
             * 这里持有短写锁，避免 STOP 写入时被普通命令线程的 tcflush/write 交叉覆盖。
             */
            QMutexLocker writeLocker(serialWriteMutex.data());

            /*
             * 只清理本 fd 的待发送输出队列，不使用 TCIOFLUSH。
             * TCIOFLUSH 会丢弃输入数据，可能影响上一条普通命令线程正在等待的 ACK。
             */
            tcflush(fd, TCOFLUSH);
            for (int attempt = 0; attempt < safeRepeatCount; ++attempt) {
                /*
                 * 每一轮都完整写入并等待发送队列排空。
                 * 如果 F4 正在处理上一条位置/速度命令，重复 STOP 可以确保下一次 USART1 取帧仍能看到停止请求。
                 */
                if (!writeAllToFd(fd, frame)) {
                    if (detail) {
                        *detail = QStringLiteral("写入 F4 强制停止帧失败：")
                                + hexByteString(frame)
                                + QStringLiteral(" attempt=")
                                + QString::number(attempt + 1);
                    }
                    ::close(fd);
                    return false;
                }

                if (tcdrain(fd) != 0) {
                    if (detail) {
                        *detail = QStringLiteral("等待 F4 强制停止帧发送完成失败 attempt=")
                                + QString::number(attempt + 1);
                    }
                    ::close(fd);
                    return false;
                }

                if ((attempt + 1 < safeRepeatCount) && (repeatDelayUs > 0)) {
                    usleep(static_cast<useconds_t>(repeatDelayUs));
                }
            }
        }

        ::close(fd);
        if (detail) {
            *detail = QStringLiteral("ACTUATOR_STOP 已写入串口，bytes=")
                    + QString::number(frame.size())
                    + QStringLiteral("，repeat=")
                    + QString::number(safeRepeatCount)
                    + QStringLiteral("，不等待ACK");
        }
        return true;
    }

    /*
     * sendF4BinaryCommand 的作用：
     *   打开 MP157 到 F4 的串口，发送一帧自动流程二进制命令，并等待匹配的 ACK 或 NACK。
     *
     * 主要流程：
     *   1. 使用现有 F4 串口节点和 57600 8N1 原始模式配置。
     *   2. 写入完整二进制帧并等待 F4 回复合法二进制帧。
     *   3. 对 ACK 校验 cycle_id、acked_seq 和 acked_cmd，防止旧 ACK 被误用。
     *   4. 对 NACK 提取 error_code、state 和 detail，返回给 QML 显示。
     *
     * 参数：
     *   device 是 Linux TTY 节点，当前默认 `/dev/ttySTM2`。
     *   baud 是串口波特率，当前默认 57600。
     *   frame 是已经组好的完整二进制帧。
     *   expectedCommand 是本次期望被 ACK 的命令字。
     *   expectedSequence 是本次期望被 ACK 的 MP157 帧序号。
     *   expectedCycleId 是本次命令所属 cycle_id。
     *   detail 用于返回 ACK/NACK 详情或失败原因，可为 NULL。
     *
     * 返回值：
     *   收到匹配 ACK 返回 true；收到 NACK、旧 ACK、串口失败或超时返回 false。
     */
    static bool sendF4BinaryCommand(const QString &device,
                                    int baud,
                                    const QByteArray &frame,
                                    quint8 expectedCommand,
                                    quint16 expectedSequence,
                                    quint16 expectedCycleId,
                                    QString *detail,
                                    const QSharedPointer<QMutex> &serialWriteMutex = QSharedPointer<QMutex>(),
                                    const QSharedPointer<std::atomic<quint64>> &stopGeneration = QSharedPointer<std::atomic<quint64>>(),
                                    quint64 stopGenerationAtStart = 0U,
                                    const QString &cancelAction = QString())
    {
        F4BinaryReply reply;

        if (!exchangeF4BinaryFrame(device,
                                   baud,
                                   frame,
                                   &reply,
                                   detail,
                                   serialWriteMutex,
                                   stopGeneration,
                                   stopGenerationAtStart,
                                   cancelAction)) {
            return false;
        }

        if (reply.command == BINARY_PROTOCOL_CMD_ACK) {
            if (reply.payload.size() != 7) {
                if (detail) {
                    *detail = QStringLiteral("ACK负载长度错误：") + QString::number(reply.payload.size());
                }
                return false;
            }

            const quint16 cycleId = readLe16(reply.payload, 0);
            const quint16 ackedSequence = readLe16(reply.payload, 2);
            const quint8 ackedCommand = static_cast<quint8>(reply.payload.at(4));
            const quint8 status = static_cast<quint8>(reply.payload.at(5));
            const quint8 state = static_cast<quint8>(reply.payload.at(6));

            if (cycleId != expectedCycleId
                    || ackedSequence != expectedSequence
                    || ackedCommand != expectedCommand) {
                if (detail) {
                    *detail = QStringLiteral("ACK不匹配：cycle=") + QString::number(cycleId)
                            + QStringLiteral(" seq=") + QString::number(ackedSequence)
                            + QStringLiteral(" cmd=") + f4BinaryCommandName(ackedCommand)
                            + QStringLiteral(" raw=") + hexByteString(reply.rawFrame);
                }
                return false;
            }

            if (detail) {
                *detail = QStringLiteral("ACK ")
                        + f4BinaryCommandName(ackedCommand)
                        + QStringLiteral(" cycle=") + QString::number(cycleId)
                        + QStringLiteral(" seq=") + QString::number(ackedSequence)
                        + QStringLiteral(" status=") + QString::number(status)
                        + QStringLiteral(" state=") + f4ProtocolStateName(state);
            }

            /*
             * ACK.status 的含义来自二进制协议：
             * 0 表示 F4 已接受并执行本次命令，1 表示重复帧已忽略但状态正常。
             * 首页“开始”必须推动传送带重新进入扫描，不能把重复帧 ACK 当成新的启动成功，
             * 否则会出现绿色提示成功但电机没有新动作的现场误判。
             */
            if (status != 0U) {
                if (detail) {
                    if (status == 1U) {
                        *detail = QStringLiteral("ACK重复帧：F4认为该命令已处理，未重新执行 ")
                                + f4BinaryCommandName(ackedCommand)
                                + QStringLiteral(" cycle=") + QString::number(cycleId)
                                + QStringLiteral(" state=") + f4ProtocolStateName(state);
                    } else {
                        *detail = QStringLiteral("ACK未确认执行：")
                                + f4BinaryCommandName(ackedCommand)
                                + QStringLiteral(" cycle=") + QString::number(cycleId)
                                + QStringLiteral(" status=") + QString::number(status)
                                + QStringLiteral(" state=") + f4ProtocolStateName(state);
                    }
                }
                return false;
            }
            return true;
        }

        if (reply.command == BINARY_PROTOCOL_CMD_NACK) {
            if (reply.payload.size() != 9) {
                if (detail) {
                    *detail = QStringLiteral("NACK负载长度错误：") + QString::number(reply.payload.size());
                }
                return false;
            }

            const quint16 cycleId = readLe16(reply.payload, 0);
            const quint16 rejectedSequence = readLe16(reply.payload, 2);
            const quint8 rejectedCommand = static_cast<quint8>(reply.payload.at(4));
            const quint8 errorCode = static_cast<quint8>(reply.payload.at(5));
            const quint8 state = static_cast<quint8>(reply.payload.at(6));
            const quint16 nackDetail = readLe16(reply.payload, 7);

            if (detail) {
                *detail = QStringLiteral("NACK ")
                        + f4BinaryCommandName(rejectedCommand)
                        + QStringLiteral(" cycle=") + QString::number(cycleId)
                        + QStringLiteral(" seq=") + QString::number(rejectedSequence)
                        + QStringLiteral(" error=") + f4NackErrorName(errorCode)
                        + QStringLiteral(" state=") + f4ProtocolStateName(state)
                        + QStringLiteral(" detail=") + QString::number(nackDetail);
            }
            return false;
        }

        if (detail) {
            *detail = QStringLiteral("收到非ACK/NACK回复：")
                    + f4BinaryCommandName(reply.command)
                    + QStringLiteral(" raw=")
                    + hexByteString(reply.rawFrame);
        }
        return false;
    }

    /*
     * estimateActuatorPositionMoveFallbackMs 的作用：
     *   当 MP157 已收到 ACTUATOR_POS_MOVE 的 ACK、但没有等到 F4 EVENT_REPORT 时，
     *   根据“本次实际发送的命令帧”估算一个兜底完成时间，避免自动流程无声卡在 Z 轴下降后。
     *
     * 主要流程：
     *   1. 从完整二进制帧中校验 CMD 和 payload 长度，确保解析的是 ACTUATOR_POS_MOVE。
     *   2. 直接读取帧内 actuator、direction、speed_rpm 和 steps，因此用户在参数页修改速度或步数后，
     *      下一次命令会自动得到新的估算等待时间，不使用写死 sleep。
     *   3. 用 steps / 每圈步数 / rpm 换算运动时间，再叠加安全余量和短稳定时间。
     *   4. 用 fallbackMaxWaitMs 作为参数页或默认策略传入的最大等待上限，避免固定等待时间。
     *   5. 对异常输入做限幅；如果帧格式不对则返回 -1，调用方继续只等 F4 事件。
     *
     * 参数：
     *   frame 是已经写给 F4 的完整 ACTUATOR_POS_MOVE 二进制帧。
     *   fallbackMaxWaitMs 是本次位置运动允许 MP157 本地兜底等待的最大时间，单位 ms。
     *   summary 用于返回本次估算使用的 actuator、direction、speed、steps 和 wait_ms，便于现场日志对账。
     *
     * 返回值：
     *   返回正整数表示 MP157 本地兜底等待毫秒数；返回 -1 表示无法安全估算。
     */
    static int estimateActuatorPositionMoveFallbackMs(const QByteArray &frame,
                                                      int fallbackMaxWaitMs,
                                                      QString *summary)
    {
        const int payloadOffset = 7;       /* payloadOffset 指向完整帧中 PAYLOAD 的首字节。 */
        const int payloadLengthOffset = 4; /* payloadLengthOffset 是 LEN 字段下标。 */
        const int commandOffset = 3;       /* commandOffset 是 CMD 字段下标。 */

        if (summary) {
            summary->clear();
        }

        if (frame.size() < BINARY_PROTOCOL_MIN_FRAME_SIZE + BINARY_PROTOCOL_ACTUATOR_POS_MOVE_PAYLOAD_SIZE) {
            return -1;
        }

        if (static_cast<quint8>(frame.at(commandOffset)) != BINARY_PROTOCOL_CMD_ACTUATOR_POS_MOVE) {
            return -1;
        }

        if (static_cast<quint8>(frame.at(payloadLengthOffset)) != BINARY_PROTOCOL_ACTUATOR_POS_MOVE_PAYLOAD_SIZE) {
            return -1;
        }

        const quint8 actuator = static_cast<quint8>(frame.at(payloadOffset + 2));
        const quint8 direction = static_cast<quint8>(frame.at(payloadOffset + 3));
        const quint16 speedRpmRaw = readLe16(frame, payloadOffset + 5);
        const quint32 steps = readLe32Unsigned(frame, payloadOffset + 7);
        int fallbackSpeedRpm = 40;

        if (actuator == 1U) {
            fallbackSpeedRpm = 120;
        } else if (actuator == 2U) {
            fallbackSpeedRpm = 80;
        }

        const int speedRpm = speedRpmRaw > 0U
                ? static_cast<int>(std::min<quint16>(speedRpmRaw, static_cast<quint16>(5000U)))
                : fallbackSpeedRpm;
        const double moveMsDouble = (static_cast<double>(steps) * 60000.0)
                / (static_cast<double>(MP157_ACTUATOR_FALLBACK_STEPS_PER_REV) * static_cast<double>(speedRpm));
        const int safeFallbackMaxWaitMs = clampedInt(fallbackMaxWaitMs,
                                                     MP157_CAMERA_Z_TIMEOUT_MIN_MS,
                                                     MP157_ACTUATOR_FALLBACK_MAX_MS);
        int waitMs = static_cast<int>(std::ceil(moveMsDouble))
                + MP157_ACTUATOR_FALLBACK_SAFETY_MS
                + MP157_ACTUATOR_FALLBACK_SETTLE_MS;

        waitMs = std::max(MP157_ACTUATOR_FALLBACK_MIN_MS, waitMs);
        waitMs = std::min(safeFallbackMaxWaitMs, waitMs);

        if (summary) {
            *summary = QStringLiteral("mp157-local-estimate actuator=") + QString::number(actuator)
                    + QStringLiteral(" direction=") + QString::number(direction)
                    + QStringLiteral(" speed_rpm=") + QString::number(speedRpm)
                    + QStringLiteral(" speed_source=") + (speedRpmRaw > 0U ? QStringLiteral("frame") : QStringLiteral("axis-default"))
                    + QStringLiteral(" steps=") + QString::number(steps)
                    + QStringLiteral(" fallbackMaxWaitMs=") + QString::number(safeFallbackMaxWaitMs)
                    + QStringLiteral(" wait_ms=") + QString::number(waitMs);
        }

        return waitMs;
    }

    /*
     * runF4ActuatorPositionMoveAndWaitDone 的作用：
     *   专门发送 ACTUATOR_POS_MOVE，并在同一个串口连接中等待“ACK + 完成事件”。
     *
     * 主要流程：
     *   1. 打开并配置 F4 串口，写入完整二进制位置运动帧。
     *   2. 先等待匹配本 sequence 的 ACK/NACK；ACK 只说明 F4 接收并入队。
     *   3. ACK 成功后继续读取 EVENT_REPORT，必须匹配同一个 cycle_id 和 related_seq。
     *   4. 收到 actuator-move-done 返回成功，status 会说明主动到位或估算完成；收到 actuator-move-timeout 或本地保护超时返回失败。
     *
     * 参数：
     *   device 是 Linux TTY 节点，当前默认 `/dev/ttySTM2`。
     *   baud 是串口波特率，当前默认 57600。
     *   frame 是已经组好的 ACTUATOR_POS_MOVE 完整帧。
     *   expectedSequence 是本次位置运动命令序号。
     *   expectedCycleId 是自动流程 ID，手动命令允许为 0。
     *   fallbackMaxWaitMs 是 MP157 本地估算完成的最大等待时间，自动 Z 轴来自参数页 zMotionTimeoutMs。
     *   detail 返回 ACK 和 DONE/TIMEOUT 的完整诊断文本。
     *
     * 返回值：
     *   只有收到同一 sequence 的 `actuator-move-done` 才返回 true。
     *   这能避免 MP157 在 F4 没确认 Z 轴下降/上升完成时提前检测或启动机械臂。
     */
    static bool runF4ActuatorPositionMoveAndWaitDone(const QString &device,
                                                     int baud,
                                                     const QByteArray &frame,
                                                     quint16 expectedSequence,
                                                     quint16 expectedCycleId,
                                                     int fallbackMaxWaitMs,
                                                     QString *detail,
                                                     const QSharedPointer<QMutex> &serialWriteMutex = QSharedPointer<QMutex>(),
                                                     const QSharedPointer<std::atomic<quint64>> &stopGeneration = QSharedPointer<std::atomic<quint64>>(),
                                                     quint64 stopGenerationAtStart = 0U,
                                                     const QString &cancelAction = QString())
    {
        const QByteArray devBytes = device.toLocal8Bit();
        int fd = -1;                         /* fd 保存本次位置运动专用串口连接，ACK 和 DONE 都从这里读取。 */
        struct termios tio;                  /* tio 保存 raw 串口配置，避免二进制协议被行规程改写。 */
        QString ackDetail;                   /* ackDetail 保存 F4 接收命令后的 ACK 文本。 */
        QString lastReadDetail;              /* lastReadDetail 保存等待过程中最近一次非目标帧或读失败原因。 */
        QString fallbackEstimateDetail;       /* fallbackEstimateDetail 保存 MP157 用帧内速度和步数算出的兜底等待依据。 */
        bool ackMatched = false;             /* ackMatched 标记是否已经收到本命令对应的 ACK。 */
        QElapsedTimer ackTimer;              /* ackTimer 限制 ACK 阶段等待时间。 */
        QElapsedTimer moveTimer;             /* moveTimer 限制到位事件阶段等待时间。 */
        const int fallbackMoveMs = estimateActuatorPositionMoveFallbackMs(frame,
                                                                          fallbackMaxWaitMs,
                                                                          &fallbackEstimateDetail);

        if (frame.isEmpty()) {
            if (detail) {
                *detail = QStringLiteral("ACTUATOR_POS_MOVE帧为空");
            }
            return false;
        }

        fd = ::open(devBytes.constData(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            if (detail) {
                *detail = QStringLiteral("无法打开 ") + device;
            }
            return false;
        }

        if (tcgetattr(fd, &tio) != 0) {
            if (detail) {
                *detail = QStringLiteral("读取串口属性失败");
            }
            ::close(fd);
            return false;
        }

        cfmakeraw(&tio);
        cfsetispeed(&tio, baudToSpeed(baud));
        cfsetospeed(&tio, baudToSpeed(baud));
        tio.c_cflag |= CLOCAL | CREAD;
#ifdef CRTSCTS
        tio.c_cflag &= ~CRTSCTS;
#endif
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;

        if (tcsetattr(fd, TCSANOW, &tio) != 0) {
            if (detail) {
                *detail = QStringLiteral("配置串口失败");
            }
            ::close(fd);
            return false;
        }

        {
            /*
             * 写 ACTUATOR_POS_MOVE 前必须和强制 STOP 互斥。
             * 如果 STOP 已经递增代际，就不能再 flush/write 旧的位置运动帧，
             * 否则会把停止意图覆盖成新的运动。
             */
            QMutexLocker writeLocker(serialWriteMutex.data());

            if (f4ActuatorStopGenerationChanged(stopGeneration, stopGenerationAtStart)) {
                if (detail) {
                    *detail = f4ActuatorStopCanceledDetail(cancelAction);
                }
                ::close(fd);
                return false;
            }

            tcflush(fd, TCIOFLUSH);
            if (!writeAllToFd(fd, frame)) {
                if (detail) {
                    *detail = QStringLiteral("写入 ACTUATOR_POS_MOVE 失败：") + hexByteString(frame);
                }
                ::close(fd);
                return false;
            }

            if (tcdrain(fd) != 0) {
                if (detail) {
                    *detail = QStringLiteral("等待 ACTUATOR_POS_MOVE 发送完成失败");
                }
                ::close(fd);
                return false;
            }
        }

        ackTimer.start();
        while (ackTimer.elapsed() < 2500) {
            F4BinaryReply reply;
            QString readErrorText;

            if (f4ActuatorStopGenerationChanged(stopGeneration, stopGenerationAtStart)) {
                if (detail) {
                    *detail = f4ActuatorStopCanceledDetail(cancelAction);
                }
                ::close(fd);
                return false;
            }

            if (!readF4BinaryReply(fd, &reply, &readErrorText)) {
                lastReadDetail = readErrorText;
                continue;
            }

            if (reply.command == BINARY_PROTOCOL_CMD_ACK) {
                if (reply.payload.size() != 7) {
                    if (detail) {
                        *detail = QStringLiteral("ACK负载长度错误：") + QString::number(reply.payload.size());
                    }
                    ::close(fd);
                    return false;
                }

                const quint16 cycleId = readLe16(reply.payload, 0);
                const quint16 ackedSequence = readLe16(reply.payload, 2);
                const quint8 ackedCommand = static_cast<quint8>(reply.payload.at(4));
                const quint8 status = static_cast<quint8>(reply.payload.at(5));
                const quint8 state = static_cast<quint8>(reply.payload.at(6));

                if (cycleId != expectedCycleId
                        || ackedSequence != expectedSequence
                        || ackedCommand != BINARY_PROTOCOL_CMD_ACTUATOR_POS_MOVE) {
                    lastReadDetail = QStringLiteral("跳过非本次位置命令ACK：cycle=") + QString::number(cycleId)
                            + QStringLiteral(" seq=") + QString::number(ackedSequence)
                            + QStringLiteral(" cmd=") + f4BinaryCommandName(ackedCommand);
                    continue;
                }

                ackDetail = QStringLiteral("ACK ACTUATOR_POS_MOVE cycle=") + QString::number(cycleId)
                        + QStringLiteral(" seq=") + QString::number(ackedSequence)
                        + QStringLiteral(" status=") + QString::number(status)
                        + QStringLiteral(" state=") + f4ProtocolStateName(state);

                if (status != 0U) {
                    if (detail) {
                        *detail = QStringLiteral("ACK未确认执行：") + ackDetail;
                    }
                    ::close(fd);
                    return false;
                }

                ackMatched = true;
                break;
            }

            if (reply.command == BINARY_PROTOCOL_CMD_NACK) {
                if (reply.payload.size() != 9) {
                    if (detail) {
                        *detail = QStringLiteral("NACK负载长度错误：") + QString::number(reply.payload.size());
                    }
                    ::close(fd);
                    return false;
                }

                const quint16 cycleId = readLe16(reply.payload, 0);
                const quint16 rejectedSequence = readLe16(reply.payload, 2);
                const quint8 rejectedCommand = static_cast<quint8>(reply.payload.at(4));
                const quint8 errorCode = static_cast<quint8>(reply.payload.at(5));
                const quint8 state = static_cast<quint8>(reply.payload.at(6));
                const quint16 nackDetail = readLe16(reply.payload, 7);

                if (detail) {
                    *detail = QStringLiteral("NACK ")
                            + f4BinaryCommandName(rejectedCommand)
                            + QStringLiteral(" cycle=") + QString::number(cycleId)
                            + QStringLiteral(" seq=") + QString::number(rejectedSequence)
                            + QStringLiteral(" error=") + f4NackErrorName(errorCode)
                            + QStringLiteral(" state=") + f4ProtocolStateName(state)
                            + QStringLiteral(" detail=") + QString::number(nackDetail);
                }
                ::close(fd);
                return false;
            }

            if (reply.command == BINARY_PROTOCOL_CMD_EVENT_REPORT) {
                lastReadDetail = QStringLiteral("ACK前收到事件：") + describeF4EventReport(reply);
            } else {
                lastReadDetail = QStringLiteral("ACK前收到非ACK回复：")
                        + f4BinaryCommandName(reply.command)
                        + QStringLiteral(" raw=")
                        + hexByteString(reply.rawFrame);
            }
        }

        if (!ackMatched) {
            if (detail) {
                *detail = QStringLiteral("ACTUATOR_POS_MOVE未收到匹配ACK seq=")
                        + QString::number(expectedSequence)
                        + QStringLiteral(" last=")
                        + lastReadDetail;
            }
            ::close(fd);
            return false;
        }

        moveTimer.start();
        lastReadDetail.clear();
        while (moveTimer.elapsed() < 70000) {
            F4BinaryReply reply;
            QString readErrorText;

            if (f4ActuatorStopGenerationChanged(stopGeneration, stopGenerationAtStart)) {
                if (detail) {
                    *detail = ackDetail
                            + QStringLiteral("；")
                            + f4ActuatorStopCanceledDetail(cancelAction);
                }
                ::close(fd);
                return false;
            }

            if (fallbackMoveMs > 0 && moveTimer.elapsed() >= fallbackMoveMs) {
                /*
                 * F4 的 EVENT_REPORT 仍然是首选完成依据；走到这里说明 ACK 已收到，
                 * 但同一 related_seq 的 DONE 没有及时回来。为了避免老 F4 固件、RX 接线或事件丢失
                 * 让自动流程一直停在 Z 轴下降，MP157 使用本次帧内 speed_rpm/steps 的保守估算继续流程。
                 */
                if (detail) {
                    *detail = ackDetail
                            + QStringLiteral("；EVENT_REPORT actuator-move-done cycle=")
                            + QString::number(expectedCycleId)
                            + QStringLiteral(" status=5(estimated-done,mp157-local-estimated-done) related_seq=")
                            + QString::number(expectedSequence)
                            + QStringLiteral("；")
                            + fallbackEstimateDetail
                            + QStringLiteral("；last=")
                            + lastReadDetail;
                }
                ::close(fd);
                return true;
            }

            if (!readF4BinaryReply(fd, &reply, &readErrorText)) {
                lastReadDetail = readErrorText;
                if (fallbackMoveMs > 0 && moveTimer.elapsed() >= fallbackMoveMs) {
                    /*
                     * readF4BinaryReply() 每次最多等待 700ms；如果这次短等待结束后已经超过估算时间，
                     * 立即返回本地兜底完成，避免再多等一轮导致现场看起来没有反应。
                     */
                    if (detail) {
                        *detail = ackDetail
                                + QStringLiteral("；EVENT_REPORT actuator-move-done cycle=")
                                + QString::number(expectedCycleId)
                                + QStringLiteral(" status=5(estimated-done,mp157-local-estimated-done) related_seq=")
                                + QString::number(expectedSequence)
                                + QStringLiteral("；")
                                + fallbackEstimateDetail
                                + QStringLiteral("；last=")
                                + lastReadDetail;
                    }
                    ::close(fd);
                    return true;
                }
                continue;
            }

            if (reply.command != BINARY_PROTOCOL_CMD_EVENT_REPORT) {
                lastReadDetail = QStringLiteral("等待到位时收到其它回复：")
                        + f4BinaryCommandName(reply.command)
                        + QStringLiteral(" raw=")
                        + hexByteString(reply.rawFrame);
                continue;
            }

            if (reply.payload.size() != 16) {
                lastReadDetail = describeF4EventReport(reply);
                continue;
            }

            const quint16 cycleId = readLe16(reply.payload, 0);
            const quint8 eventCode = static_cast<quint8>(reply.payload.at(2));
            const quint16 relatedSequence = readLe16(reply.payload, 10);
            const QString eventDetail = describeF4EventReport(reply);

            if (cycleId != expectedCycleId || relatedSequence != expectedSequence) {
                lastReadDetail = QStringLiteral("跳过非本次运动事件：") + eventDetail;
                continue;
            }

            if (eventCode == BINARY_PROTOCOL_EVENT_ACTUATOR_MOVE_DONE) {
                if (detail) {
                    *detail = ackDetail + QStringLiteral("；") + eventDetail;
                }
                ::close(fd);
                return true;
            }

            if (eventCode == BINARY_PROTOCOL_EVENT_ACTUATOR_MOVE_TIMEOUT) {
                if (detail) {
                    *detail = ackDetail + QStringLiteral("；") + eventDetail;
                }
                ::close(fd);
                return false;
            }

            lastReadDetail = QStringLiteral("等待到位时收到其它事件：") + eventDetail;
        }

        if (detail) {
            *detail = ackDetail
                    + QStringLiteral("；等待 actuator-move-done 超时 seq=")
                    + QString::number(expectedSequence)
                    + QStringLiteral(" last=")
                    + lastReadDetail;
        }
        ::close(fd);
        return false;
    }

    /*
     * readLe32Unsigned 的作用：
     *   从二进制协议负载中按小端序读取 32 位无符号整数。
     *
     * 参数：
     *   data 是源负载。
     *   offset 是最低字节下标，调用方必须保证 offset+3 未越界。
     *
     * 返回值：
     *   返回解析出的 32 位无符号值。
     */
    static quint32 readLe32Unsigned(const QByteArray &data, int offset)
    {
        const quint32 b0 = static_cast<quint8>(data.at(offset));
        const quint32 b1 = static_cast<quint8>(data.at(offset + 1));
        const quint32 b2 = static_cast<quint8>(data.at(offset + 2));
        const quint32 b3 = static_cast<quint8>(data.at(offset + 3));
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }

    /*
     * f4DecisionName 的作用：
     *   把 F4 称重/电感判定枚举转换成云端 JSON 中可读的短字符串。
     */
    static QString f4DecisionName(quint8 decision)
    {
        switch (decision) {
        case 1U:
            return QStringLiteral("pass");
        case 2U:
            return QStringLiteral("fail");
        case 3U:
            return QStringLiteral("review");
        default:
            return QStringLiteral("unknown");
        }
    }

    /*
     * weightResultContextJson 的作用：
     *   把 F4 主动 WEIGHT_RESULT 负载转换成云端 sensor_context.weighing JSON。
     *
     * 参数：
     *   reply 是 readF4BinaryReply() 已解析出的 WEIGHT_RESULT 帧。
     *
     * 返回值：
     *   返回压缩 JSON；负载长度错误时返回空对象。
     */
    static QString weightResultContextJson(const F4BinaryReply &reply)
    {
        if (reply.payload.size() != 28) {
            return QStringLiteral("{}");
        }

        QJsonObject context;
        context.insert(QStringLiteral("module_type"), QStringLiteral("hx711"));
        context.insert(QStringLiteral("module_name"), QStringLiteral("HX711称重模块"));
        context.insert(QStringLiteral("cycle_id"), static_cast<int>(readLe16(reply.payload, 0)));
        context.insert(QStringLiteral("sample_id"), static_cast<int>(readLe16(reply.payload, 2)));
        context.insert(QStringLiteral("stable"), static_cast<quint8>(reply.payload.at(4)) != 0U);
        context.insert(QStringLiteral("decision_code"), static_cast<int>(static_cast<quint8>(reply.payload.at(5))));
        context.insert(QStringLiteral("decision"), f4DecisionName(static_cast<quint8>(reply.payload.at(5))));
        context.insert(QStringLiteral("gross_weight_g"), readLe32Signed(reply.payload, 6) / 1000.0);
        context.insert(QStringLiteral("net_weight_g"), readLe32Signed(reply.payload, 10) / 1000.0);
        context.insert(QStringLiteral("raw_adc"), static_cast<int>(readLe32Signed(reply.payload, 14)));
        context.insert(QStringLiteral("sample_count"), static_cast<int>(readLe16(reply.payload, 18)));
        context.insert(QStringLiteral("stable_window_g"), readLe16(reply.payload, 20) / 1000.0);
        context.insert(QStringLiteral("duration_ms"), static_cast<int>(readLe16(reply.payload, 22)));
        context.insert(QStringLiteral("option_bits"), static_cast<int>(readLe32Unsigned(reply.payload, 24)));
        return compactJsonString(context);
    }

    /*
     * ldcResultContextJson 的作用：
     *   把 F4 主动 LDC_RESULT 负载转换成云端 sensor_context.ldc1614_eddy_current JSON。
     */
    static QString ldcResultContextJson(const F4BinaryReply &reply)
    {
        if (reply.payload.size() != 28) {
            return QStringLiteral("{}");
        }

        QJsonArray channels;
        QJsonObject ch0;
        QJsonObject ch1;
        QJsonObject context;

        ch0.insert(QStringLiteral("channel"), 0);
        ch0.insert(QStringLiteral("enabled"), (static_cast<quint8>(reply.payload.at(4)) & 0x01U) != 0U);
        ch0.insert(QStringLiteral("raw_code"), static_cast<int>(readLe32Unsigned(reply.payload, 8)));
        ch0.insert(QStringLiteral("delta_raw_code"), static_cast<int>(readLe32Signed(reply.payload, 12)));
        ch0.insert(QStringLiteral("decision"), f4DecisionName(static_cast<quint8>(reply.payload.at(5))));
        channels.append(ch0);

        ch1.insert(QStringLiteral("channel"), 1);
        ch1.insert(QStringLiteral("enabled"), (static_cast<quint8>(reply.payload.at(4)) & 0x02U) != 0U);
        ch1.insert(QStringLiteral("raw_code"), static_cast<int>(readLe32Unsigned(reply.payload, 16)));
        ch1.insert(QStringLiteral("delta_raw_code"), static_cast<int>(readLe32Signed(reply.payload, 20)));
        ch1.insert(QStringLiteral("decision"), f4DecisionName(static_cast<quint8>(reply.payload.at(5))));
        channels.append(ch1);

        context.insert(QStringLiteral("module_name"), QStringLiteral("LDC1614电感检测模块"));
        context.insert(QStringLiteral("chip_vendor"), QStringLiteral("TI"));
        context.insert(QStringLiteral("chip_model"), QStringLiteral("LDC1614"));
        context.insert(QStringLiteral("cycle_id"), static_cast<int>(readLe16(reply.payload, 0)));
        context.insert(QStringLiteral("sample_id"), static_cast<int>(readLe16(reply.payload, 2)));
        context.insert(QStringLiteral("channel_mask"), static_cast<int>(static_cast<quint8>(reply.payload.at(4))));
        context.insert(QStringLiteral("decision_code"), static_cast<int>(static_cast<quint8>(reply.payload.at(5))));
        context.insert(QStringLiteral("overall_decision"), f4DecisionName(static_cast<quint8>(reply.payload.at(5))));
        context.insert(QStringLiteral("status"), static_cast<int>(static_cast<quint8>(reply.payload.at(6))));
        context.insert(QStringLiteral("duration_ms"), static_cast<int>(readLe16(reply.payload, 24)));
        context.insert(QStringLiteral("option_bits"), static_cast<int>(readLe16(reply.payload, 26)));
        context.insert(QStringLiteral("channels"), channels);
        return compactJsonString(context);
    }

    /*
     * cycleDoneContextJson 的作用：
     *   把 F4 主动 CYCLE_DONE 负载转换成云端和 QML 都能展示的流程完成 JSON。
     */
    static QString cycleDoneContextJson(const F4BinaryReply &reply)
    {
        if (reply.payload.size() != 16) {
            return QStringLiteral("{}");
        }

        QJsonObject context;
        context.insert(QStringLiteral("cycle_id"), static_cast<int>(readLe16(reply.payload, 0)));
        context.insert(QStringLiteral("job_id"), static_cast<int>(readLe16(reply.payload, 2)));
        context.insert(QStringLiteral("final_bin"), static_cast<int>(static_cast<quint8>(reply.payload.at(4))));
        context.insert(QStringLiteral("model_result"), static_cast<int>(static_cast<quint8>(reply.payload.at(5))));
        context.insert(QStringLiteral("weight_decision"), f4DecisionName(static_cast<quint8>(reply.payload.at(6))));
        context.insert(QStringLiteral("ldc_decision"), f4DecisionName(static_cast<quint8>(reply.payload.at(7))));
        context.insert(QStringLiteral("f4_state"), static_cast<int>(static_cast<quint8>(reply.payload.at(8))));
        context.insert(QStringLiteral("fault_level"), static_cast<int>(static_cast<quint8>(reply.payload.at(9))));
        context.insert(QStringLiteral("fault_bits"), static_cast<int>(readLe16(reply.payload, 10)));
        context.insert(QStringLiteral("duration_ms"), static_cast<int>(readLe16(reply.payload, 12)));
        context.insert(QStringLiteral("option_bits"), static_cast<int>(readLe16(reply.payload, 14)));
        return compactJsonString(context);
    }

    /*
     * waitForF4ActiveFrame 的作用：
     *   在长流程中等待 F4 主动上报的 WEIGHT_RESULT、LDC_RESULT 或 CYCLE_DONE。
     *
     * 主要流程：
     *   1. 打开并配置 F4 串口 raw 模式。
     *   2. 复用 readF4BinaryReply() 循环读取合法二进制帧。
     *   3. 忽略非目标主动帧，但把 FAULT_REPORT 写入 detail 便于现场排查。
     *   4. 收到目标命令且 cycle_id 匹配后返回对应 JSON。
     *
     * 参数：
     *   expectedCommand 是要等待的主动帧命令。
     *   timeoutMs 是总等待时间，称重/电感/机械臂动作现场可能需要数秒。
     *
     * 返回值：
     *   收到目标主动帧返回 true；超时、串口错误或负载错误返回 false。
     */
    static bool waitForF4ActiveFrame(const QString &device,
                                     int baud,
                                     quint8 expectedCommand,
                                     quint16 expectedCycleId,
                                     int timeoutMs,
                                     QString *detail,
                                     QString *contextJson)
    {
        const QByteArray devBytes = device.toLocal8Bit();
        int fd = ::open(devBytes.constData(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        struct termios tio;
        QElapsedTimer elapsed;
        QString lastReadError;

        if (contextJson) {
            *contextJson = QStringLiteral("{}");
        }

        if (fd < 0) {
            if (detail) {
                *detail = QStringLiteral("无法打开 ") + device + QStringLiteral(" 等待主动帧");
            }
            return false;
        }

        if (tcgetattr(fd, &tio) != 0) {
            if (detail) {
                *detail = QStringLiteral("读取串口属性失败，无法等待主动帧");
            }
            ::close(fd);
            return false;
        }

        cfmakeraw(&tio);
        cfsetispeed(&tio, baudToSpeed(baud));
        cfsetospeed(&tio, baudToSpeed(baud));
        tio.c_cflag |= CLOCAL | CREAD;
#ifdef CRTSCTS
        tio.c_cflag &= ~CRTSCTS;
#endif
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;

        if (tcsetattr(fd, TCSANOW, &tio) != 0) {
            if (detail) {
                *detail = QStringLiteral("配置串口失败，无法等待主动帧");
            }
            ::close(fd);
            return false;
        }

        elapsed.start();
        while (elapsed.elapsed() < timeoutMs) {
            F4BinaryReply reply;

            if (!readF4BinaryReply(fd, &reply, &lastReadError)) {
                continue;
            }

            if (reply.command == BINARY_PROTOCOL_CMD_FAULT_REPORT) {
                if (detail) {
                    *detail += QStringLiteral(" fault=") + describeF4FaultReport(reply);
                }
                continue;
            }

            if (reply.command != expectedCommand) {
                if (detail) {
                    *detail += QStringLiteral(" ignore=") + f4BinaryCommandName(reply.command);
                }
                continue;
            }

            if (reply.payload.size() < 2 || readLe16(reply.payload, 0) != expectedCycleId) {
                if (detail) {
                    *detail += QStringLiteral(" cycle_mismatch=") + hexByteString(reply.rawFrame);
                }
                continue;
            }

            if (expectedCommand == BINARY_PROTOCOL_CMD_WEIGHT_RESULT) {
                if (reply.payload.size() != 28) {
                    if (detail) {
                        *detail += QStringLiteral(" WEIGHT_RESULT长度错误=") + QString::number(reply.payload.size());
                    }
                    ::close(fd);
                    return false;
                }
                if (contextJson) {
                    *contextJson = weightResultContextJson(reply);
                }
            } else if (expectedCommand == BINARY_PROTOCOL_CMD_LDC_RESULT) {
                if (reply.payload.size() != 28) {
                    if (detail) {
                        *detail += QStringLiteral(" LDC_RESULT长度错误=") + QString::number(reply.payload.size());
                    }
                    ::close(fd);
                    return false;
                }
                if (contextJson) {
                    *contextJson = ldcResultContextJson(reply);
                }
            } else if (expectedCommand == BINARY_PROTOCOL_CMD_CYCLE_DONE) {
                if (reply.payload.size() != 16) {
                    if (detail) {
                        *detail += QStringLiteral(" CYCLE_DONE长度错误=") + QString::number(reply.payload.size());
                    }
                    ::close(fd);
                    return false;
                }
                if (contextJson) {
                    *contextJson = cycleDoneContextJson(reply);
                }
            }

            if (detail) {
                *detail += QStringLiteral(" got=")
                        + f4BinaryCommandName(expectedCommand)
                        + QStringLiteral(" seq=")
                        + QString::number(reply.sequence);
            }
            ::close(fd);
            return true;
        }

        if (detail) {
            *detail += QStringLiteral(" timeout_wait=")
                    + f4BinaryCommandName(expectedCommand)
                    + QStringLiteral(" timeout_ms=")
                    + QString::number(timeoutMs)
                    + QStringLiteral(" last=")
                    + lastReadError;
        }

        ::close(fd);
        return false;
    }

    /*
     * runF4ArmInspectionFlow 的作用：
     *   完成 MODEL_READY、ARM_JOB_START，并等待 F4 回传称重和电感结果。
     */
    static bool runF4ArmInspectionFlow(const QString &device,
                                       int baud,
                                       const QByteArray &modelFrame,
                                       quint16 modelSequence,
                                       const QByteArray &armFrame,
                                       quint16 armSequence,
                                       quint16 ackSequenceBase,
                                       quint16 cycleId,
                                       quint16 jobId,
                                       int activeFrameTimeoutMs,
                                       QString *detail,
                                       QString *weightContextJson,
                                       QString *ldcContextJson,
                                       QString *f4FlowContextJson)
    {
        QString modelAck;
        QString armAck;
        QString waitDetail;
        QString weightJson;
        QString ldcJson;

        Q_UNUSED(ackSequenceBase);

        if (!sendF4BinaryCommand(device,
                                 baud,
                                 modelFrame,
                                 BINARY_PROTOCOL_CMD_MODEL_READY,
                                 modelSequence,
                                 cycleId,
                                 &modelAck)) {
            if (detail) {
                *detail = QStringLiteral("MODEL_READY失败：") + modelAck;
            }
            return false;
        }

        if (!sendF4BinaryCommand(device,
                                 baud,
                                 armFrame,
                                 BINARY_PROTOCOL_CMD_ARM_JOB_START,
                                 armSequence,
                                 cycleId,
                                 &armAck)) {
            if (detail) {
                *detail = QStringLiteral("ARM_JOB_START失败：") + armAck;
            }
            return false;
        }

        if (!waitForF4ActiveFrame(device,
                                  baud,
                                  BINARY_PROTOCOL_CMD_WEIGHT_RESULT,
                                  cycleId,
                                  activeFrameTimeoutMs,
                                  &waitDetail,
                                  &weightJson)) {
            if (detail) {
                *detail = QStringLiteral("等待称重结果失败：") + waitDetail;
            }
            return false;
        }

        if (!waitForF4ActiveFrame(device,
                                  baud,
                                  BINARY_PROTOCOL_CMD_LDC_RESULT,
                                  cycleId,
                                  activeFrameTimeoutMs,
                                  &waitDetail,
                                  &ldcJson)) {
            if (detail) {
                *detail = QStringLiteral("等待电感结果失败：") + waitDetail;
            }
            return false;
        }

        QJsonObject flow;
        flow.insert(QStringLiteral("cycle_id"), static_cast<int>(cycleId));
        flow.insert(QStringLiteral("job_id"), static_cast<int>(jobId));
        flow.insert(QStringLiteral("model_ready_ack"), modelAck);
        flow.insert(QStringLiteral("arm_job_start_ack"), armAck);
        flow.insert(QStringLiteral("active_frame_timeout_ms"), activeFrameTimeoutMs);
        flow.insert(QStringLiteral("weight_result_received"), true);
        flow.insert(QStringLiteral("ldc_result_received"), true);
        flow.insert(QStringLiteral("next_step"), QStringLiteral("upload_then_final_sort"));

        if (weightContextJson) {
            *weightContextJson = weightJson;
        }
        if (ldcContextJson) {
            *ldcContextJson = ldcJson;
        }
        if (f4FlowContextJson) {
            *f4FlowContextJson = compactJsonString(flow);
        }
        if (detail) {
            *detail = QStringLiteral("MODEL_READY和ARM_JOB_START已ACK，称重/电感已收齐");
        }
        return true;
    }

    /*
     * runF4FinalSortAndWaitCycleDone 的作用：
     *   下发 FINAL_SORT_RESULT，并等待 F4 主动回传 CYCLE_DONE 作为本轮闭环完成标志。
     */
    static bool runF4FinalSortAndWaitCycleDone(const QString &device,
                                               int baud,
                                               const QByteArray &frame,
                                               quint16 finalSequence,
                                               quint16 ackSequenceBase,
                                               quint16 cycleId,
                                               quint16 jobId,
                                               int activeFrameTimeoutMs,
                                               QString *detail,
                                               QString *cycleDoneContextJson)
    {
        QString finalAck;
        QString waitDetail;
        QString doneJson;

        Q_UNUSED(ackSequenceBase);

        if (!sendF4BinaryCommand(device,
                                 baud,
                                 frame,
                                 BINARY_PROTOCOL_CMD_FINAL_SORT_RESULT,
                                 finalSequence,
                                 cycleId,
                                 &finalAck)) {
            if (detail) {
                *detail = QStringLiteral("FINAL_SORT_RESULT失败：") + finalAck;
            }
            return false;
        }

        if (!waitForF4ActiveFrame(device,
                                  baud,
                                  BINARY_PROTOCOL_CMD_CYCLE_DONE,
                                  cycleId,
                                  activeFrameTimeoutMs,
                                  &waitDetail,
                                  &doneJson)) {
            if (detail) {
                *detail = QStringLiteral("等待CYCLE_DONE失败：") + waitDetail;
            }
            return false;
        }

        if (cycleDoneContextJson) {
            *cycleDoneContextJson = doneJson;
        }
        if (detail) {
            *detail = QStringLiteral("FINAL_SORT_RESULT已ACK，CYCLE_DONE已收到 job_id=")
                    + QString::number(jobId)
                    + QStringLiteral(" ack=")
                    + finalAck;
        }
        return true;
    }

    /*
     * sendF4BinaryHeartbeat 的作用：
     *   发送 HEARTBEAT 二进制帧，只确认 F4 在线和协议链路正常。
     *
     * 关键说明：
     *   心跳不能强制校验 cycle_id，因为 Qt 进程可能重启而 F4 仍保留旧的 active_cycle_id。
     *   对心跳而言，只要 ACK 的 seq/cmd 匹配，就说明二进制链路可用；ACK 中的 cycle_id 只作为状态文本展示。
     *
     * 参数：
     *   device 是 Linux 串口节点。
     *   baud 是串口波特率。
     *   frame 是完整 HEARTBEAT 帧。
     *   expectedSequence 是本次心跳帧序号。
     *   detail 返回 ACK 摘要或 NACK 失败详情。
     *
     * 返回值：
     *   收到匹配 HEARTBEAT ACK 返回 true；收到 NACK、旧帧、串口失败或负载错误返回 false。
     */
    static bool sendF4BinaryHeartbeat(const QString &device,
                                      int baud,
                                      const QByteArray &frame,
                                      quint16 expectedSequence,
                                      QString *detail)
    {
        F4BinaryReply reply;

        if (!exchangeF4BinaryFrame(device, baud, frame, &reply, detail)) {
            return false;
        }

        if (reply.command == BINARY_PROTOCOL_CMD_ACK) {
            if (reply.payload.size() != 7) {
                if (detail) {
                    *detail = QStringLiteral("HEARTBEAT ACK负载长度错误：") + QString::number(reply.payload.size());
                }
                return false;
            }

            const quint16 cycleId = readLe16(reply.payload, 0);
            const quint16 ackedSequence = readLe16(reply.payload, 2);
            const quint8 ackedCommand = static_cast<quint8>(reply.payload.at(4));
            const quint8 status = static_cast<quint8>(reply.payload.at(5));
            const quint8 state = static_cast<quint8>(reply.payload.at(6));

            if (ackedSequence != expectedSequence || ackedCommand != BINARY_PROTOCOL_CMD_HEARTBEAT) {
                if (detail) {
                    *detail = QStringLiteral("HEARTBEAT ACK不匹配：cycle=") + QString::number(cycleId)
                            + QStringLiteral(" seq=") + QString::number(ackedSequence)
                            + QStringLiteral(" cmd=") + f4BinaryCommandName(ackedCommand)
                            + QStringLiteral(" raw=") + hexByteString(reply.rawFrame);
                }
                return false;
            }

            if (status != 0U) {
                if (detail) {
                    *detail = QStringLiteral("HEARTBEAT ACK未确认：cycle=") + QString::number(cycleId)
                            + QStringLiteral(" status=") + QString::number(status)
                            + QStringLiteral(" state=") + f4ProtocolStateName(state);
                }
                return false;
            }

            if (detail) {
                *detail = QStringLiteral("ACK HEARTBEAT cycle=") + QString::number(cycleId)
                        + QStringLiteral(" seq=") + QString::number(ackedSequence)
                        + QStringLiteral(" state=") + f4ProtocolStateName(state);
            }
            return true;
        }

        if (reply.command == BINARY_PROTOCOL_CMD_NACK) {
            if (reply.payload.size() != 9) {
                if (detail) {
                    *detail = QStringLiteral("HEARTBEAT NACK负载长度错误：") + QString::number(reply.payload.size());
                }
                return false;
            }

            const quint16 cycleId = readLe16(reply.payload, 0);
            const quint16 rejectedSequence = readLe16(reply.payload, 2);
            const quint8 rejectedCommand = static_cast<quint8>(reply.payload.at(4));
            const quint8 errorCode = static_cast<quint8>(reply.payload.at(5));
            const quint8 state = static_cast<quint8>(reply.payload.at(6));
            const quint16 nackDetail = readLe16(reply.payload, 7);

            if (detail) {
                *detail = QStringLiteral("NACK ")
                        + f4BinaryCommandName(rejectedCommand)
                        + QStringLiteral(" cycle=") + QString::number(cycleId)
                        + QStringLiteral(" seq=") + QString::number(rejectedSequence)
                        + QStringLiteral(" error=") + f4NackErrorName(errorCode)
                        + QStringLiteral(" state=") + f4ProtocolStateName(state)
                        + QStringLiteral(" detail=") + QString::number(nackDetail);
            }
            return false;
        }

        if (detail) {
            *detail = QStringLiteral("收到非HEARTBEAT ACK/NACK回复：")
                    + f4BinaryCommandName(reply.command)
                    + QStringLiteral(" raw=")
                    + hexByteString(reply.rawFrame);
        }
        return false;
    }

    /*
     * sendF4BinaryStatusQuery 的作用：
     *   发送 QUERY_STATUS 二进制帧，并把 F4 返回的 STATUS_REPORT 转成界面可读文本。
     *
     * 参数：
     *   device 是 Linux 串口节点。
     *   baud 是串口波特率。
     *   frame 是完整 QUERY_STATUS 帧。
     *   expectedSequence 是本次查询帧序号。
     *   expectedCycleId 是本次查询关注的 cycle_id，手动查询通常为 0。
     *   detail 返回结构化状态摘要或 NACK 失败详情。
     *
     * 返回值：
     *   收到匹配 STATUS_REPORT 返回 true；收到 NACK、旧帧、串口失败或负载错误返回 false。
     */
    static bool sendF4BinaryStatusQuery(const QString &device,
                                        int baud,
                                        const QByteArray &frame,
                                        quint16 expectedSequence,
                                        quint16 expectedCycleId,
                                        QString *detail)
    {
        F4BinaryReply reply;

        if (!exchangeF4BinaryFrame(device, baud, frame, &reply, detail)) {
            return false;
        }

        if (reply.command == BINARY_PROTOCOL_CMD_STATUS_REPORT) {
            if (reply.payload.size() != 24) {
                if (detail) {
                    *detail = QStringLiteral("STATUS_REPORT负载长度错误：") + QString::number(reply.payload.size());
                }
                return false;
            }

            const quint16 cycleId = readLe16(reply.payload, 0);
            const quint16 repliedSequence = readLe16(reply.payload, 2);
            const quint8 repliedCommand = static_cast<quint8>(reply.payload.at(4));
            const quint8 f4State = static_cast<quint8>(reply.payload.at(5));
            const quint16 activeCycleId = readLe16(reply.payload, 6);
            const quint8 pausedState = static_cast<quint8>(reply.payload.at(8));
            const quint8 beltDesired = static_cast<quint8>(reply.payload.at(9));
            const quint8 beltApplied = static_cast<quint8>(reply.payload.at(10));
            const quint8 beltDirection = static_cast<quint8>(reply.payload.at(11));
            const quint8 beltCentered = static_cast<quint8>(reply.payload.at(12));
            const quint8 beltStable = static_cast<quint8>(reply.payload.at(13));
            const quint16 beltSpeedRpm = readLe16(reply.payload, 14);
            const qint32 beltErrorPx = readLe32Signed(reply.payload, 16);
            const quint16 featureBits = readLe16(reply.payload, 20);
            const quint16 faultBits = readLe16(reply.payload, 22);

            if (cycleId != expectedCycleId
                    || repliedSequence != expectedSequence
                    || repliedCommand != BINARY_PROTOCOL_CMD_QUERY_STATUS) {
                if (detail) {
                    *detail = QStringLiteral("STATUS_REPORT不匹配：cycle=") + QString::number(cycleId)
                            + QStringLiteral(" seq=") + QString::number(repliedSequence)
                            + QStringLiteral(" cmd=") + f4BinaryCommandName(repliedCommand)
                            + QStringLiteral(" raw=") + hexByteString(reply.rawFrame);
                }
                return false;
            }

            if (detail) {
                *detail = QStringLiteral("STATUS_REPORT cycle=") + QString::number(cycleId)
                        + QStringLiteral(" active=") + QString::number(activeCycleId)
                        + QStringLiteral(" state=") + f4ProtocolStateName(f4State)
                        + QStringLiteral(" paused=") + f4ProtocolStateName(pausedState)
                        + QStringLiteral(" belt_desired=") + f4BeltModeName(beltDesired)
                        + QStringLiteral(" belt_applied=") + f4BeltModeName(beltApplied)
                        + QStringLiteral(" speed=") + QString::number(beltSpeedRpm)
                        + QStringLiteral("rpm dir=") + ((beltDirection == 0U) ? QStringLiteral("CW") : QStringLiteral("CCW"))
                        + QStringLiteral(" error=") + QString::number(beltErrorPx)
                        + QStringLiteral("px stable=") + QString::number(beltStable)
                        + QStringLiteral(" centered=") + QString::number(beltCentered)
                        + QStringLiteral(" feature=0x") + QString::number(featureBits, 16).toUpper()
                        + QStringLiteral(" fault=0x") + QString::number(faultBits, 16).toUpper();
            }
            return true;
        }

        if (reply.command == BINARY_PROTOCOL_CMD_NACK) {
            if (reply.payload.size() != 9) {
                if (detail) {
                    *detail = QStringLiteral("NACK负载长度错误：") + QString::number(reply.payload.size());
                }
                return false;
            }

            const quint16 cycleId = readLe16(reply.payload, 0);
            const quint16 rejectedSequence = readLe16(reply.payload, 2);
            const quint8 rejectedCommand = static_cast<quint8>(reply.payload.at(4));
            const quint8 errorCode = static_cast<quint8>(reply.payload.at(5));
            const quint8 state = static_cast<quint8>(reply.payload.at(6));
            const quint16 nackDetail = readLe16(reply.payload, 7);

            if (detail) {
                *detail = QStringLiteral("NACK ")
                        + f4BinaryCommandName(rejectedCommand)
                        + QStringLiteral(" cycle=") + QString::number(cycleId)
                        + QStringLiteral(" seq=") + QString::number(rejectedSequence)
                        + QStringLiteral(" error=") + f4NackErrorName(errorCode)
                        + QStringLiteral(" state=") + f4ProtocolStateName(state)
                        + QStringLiteral(" detail=") + QString::number(nackDetail);
            }
            return false;
        }

        if (detail) {
            *detail = QStringLiteral("收到非STATUS_REPORT/NACK回复：")
                    + f4BinaryCommandName(reply.command)
                    + QStringLiteral(" raw=")
                    + hexByteString(reply.rawFrame);
        }
        return false;
    }

    /*
     * writeAllToFd 的作用：
     *   对短 socket/串口命令执行完整写入，避免 write 短写导致 overlay 或 F4 收到半条命令。
     *
     * 参数：
     *   fd 是已经打开的文件描述符。
     *   data 是需要写入的一整条命令。
     *
     * 返回值：
     *   全部写入返回 true；遇到不可恢复错误返回 false。
     */
    static bool writeAllToFd(int fd, const QByteArray &data)
    {
        const char *cursor = data.constData();
        ssize_t remaining = static_cast<ssize_t>(data.size());

        while (remaining > 0) {
            const ssize_t written = ::write(fd, cursor, static_cast<size_t>(remaining));

            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }

            if (written == 0) {
                return false;
            }

            cursor += written;
            remaining -= written;
        }

        return true;
    }

    /*
     * setXxxStatus 系列函数的作用：
     *   集中更新状态属性，只有变化时才发信号，减少 QML 无意义重绘。
     */
    void setNetworkStatus(const QString &text, const QString &color)
    {
        if (m_networkStatusText == text && m_networkStatusColor == color) {
            return;
        }
        m_networkStatusText = text;
        m_networkStatusColor = color;
        emit networkStatusChanged();
    }

    void setCameraStatus(const QString &text, const QString &color)
    {
        if (m_cameraStatusText == text && m_cameraStatusColor == color) {
            return;
        }
        m_cameraStatusText = text;
        m_cameraStatusColor = color;
        emit cameraStatusChanged();
    }

    void setF4Status(const QString &text, const QString &color)
    {
        if (m_f4StatusText == text && m_f4StatusColor == color) {
            return;
        }
        m_f4StatusText = text;
        m_f4StatusColor = color;
        emit f4StatusChanged();
    }

    void setCloudStatus(const QString &text, const QString &color)
    {
        if (m_cloudStatusText == text && m_cloudStatusColor == color) {
            return;
        }
        m_cloudStatusText = text;
        m_cloudStatusColor = color;
        emit cloudStatusChanged();
    }

    void setSdcardStatus(const QString &text, const QString &color)
    {
        if (m_sdcardStatusText == text && m_sdcardStatusColor == color) {
            return;
        }
        m_sdcardStatusText = text;
        m_sdcardStatusColor = color;
        emit sdcardStatusChanged();
    }

    void setLocationStatus(const QString &text,
                           const QString &color,
                           const QString &display,
                           const QString &shortDisplay)
    {
        if (m_locationStatusText == text
                && m_locationStatusColor == color
                && m_locationDisplayText == display
                && m_locationShortText == shortDisplay) {
            return;
        }
        m_locationStatusText = text;
        m_locationStatusColor = color;
        m_locationDisplayText = display;
        m_locationShortText = shortDisplay;
        emit locationStatusChanged();
    }

    void setDetailText(const QString &text)
    {
        if (m_detailText == text) {
            return;
        }
        m_detailText = text;
        emit detailTextChanged();
    }

    QString m_videoBackend;             /* m_videoBackend 保存当前视频后端，用于决定相机状态判定来源。 */
    QString m_cameraDevice;             /* m_cameraDevice 保存摄像头节点路径。 */
    QString m_overlaySocket;            /* m_overlaySocket 保存 overlay 控制 socket 路径。 */
    QString m_overlayRestartScript;     /* m_overlayRestartScript 保存相机重连时要后台执行的控制脚本。 */
    QString m_networkScript;            /* m_networkScript 保存 4G PPP 管理命令。 */
    QString m_locationScript;           /* m_locationScript 保存高德 IP 定位脚本命令，Qt 只在开机第一轮调用一次 once。 */
    QString m_cloudHealthUrl;           /* m_cloudHealthUrl 保存云端 health 地址。 */
    QString m_sdcardMount;              /* m_sdcardMount 保存 SD 卡挂载点。 */
    QString m_f4Device;                 /* m_f4Device 保存 F4 串口设备节点。 */
    int m_f4Baud;                       /* m_f4Baud 保存 F4 串口波特率。 */
    QElapsedTimer m_f4HeartbeatElapsed;  /* m_f4HeartbeatElapsed 记录上一次二进制心跳发送时间，用于把周期心跳限制为 2 分钟一次。 */
    QSharedPointer<std::atomic<quint64>> m_f4ActuatorStopGeneration; /* m_f4ActuatorStopGeneration 是强制 STOP 代际；STOP 点击会递增它，旧运动线程写帧前发现代际变化就取消，避免停止后又被晚到运动帧重新启动。 */
    QSharedPointer<QMutex> m_f4SerialWriteMutex; /* m_f4SerialWriteMutex 只保护 F4 串口 open/config/flush/write/drain 这一小段，保证普通运动帧和强制 STOP 帧不会同时对同一个 TTY 做 flush/write。 */
    QString m_networkStatusText;        /* m_networkStatusText 保存网络状态文本。 */
    QString m_networkStatusColor;       /* m_networkStatusColor 保存网络状态颜色。 */
    QString m_cameraStatusText;         /* m_cameraStatusText 保存摄像头状态文本。 */
    QString m_cameraStatusColor;        /* m_cameraStatusColor 保存摄像头状态颜色。 */
    QString m_f4StatusText;             /* m_f4StatusText 保存 F4 状态文本。 */
    QString m_f4StatusColor;            /* m_f4StatusColor 保存 F4 状态颜色。 */
    QString m_cloudStatusText;          /* m_cloudStatusText 保存云端状态文本。 */
    QString m_cloudStatusColor;         /* m_cloudStatusColor 保存云端状态颜色。 */
    QString m_sdcardStatusText;         /* m_sdcardStatusText 保存 SD 卡状态文本。 */
    QString m_sdcardStatusColor;        /* m_sdcardStatusColor 保存 SD 卡状态颜色。 */
    QString m_locationStatusText;       /* m_locationStatusText 保存定位状态文本，例如 IP定位、缺少Key或定位失败。 */
    QString m_locationDisplayText;      /* m_locationDisplayText 保存完整位置显示；当前 IP 定位模式只显示省份。 */
    QString m_locationShortText;        /* m_locationShortText 保存顶部状态栏短位置显示；当前同样只显示省份。 */
    QString m_locationStatusColor;      /* m_locationStatusColor 保存定位状态颜色。 */
    QString m_detailText;               /* m_detailText 保存最近一次健康检测详情。 */
    unsigned int m_lastOverlaySerial;   /* m_lastOverlaySerial 保存上一次 overlay 帧序号，后续可用于卡帧判断。 */
    int m_cameraOfflineCount;           /* m_cameraOfflineCount 记录相机连续离线次数。 */
    int m_overlayRestartCooldown;       /* m_overlayRestartCooldown 防止相机离线时反复高频重启 overlay。 */
    bool m_networkProbeRunning;         /* m_networkProbeRunning 防止网络检测任务堆积。 */
    bool m_locationProbeRunning;        /* m_locationProbeRunning 防止开机定位进程尚未退出时被重复调度。 */
    bool m_cloudProbeRunning;           /* m_cloudProbeRunning 防止云端检测任务堆积。 */
    bool m_f4ProbeRunning;              /* m_f4ProbeRunning 防止串口检测线程堆积。 */
    bool m_f4CommandRunning;            /* m_f4CommandRunning 防止 CAL 标定等手动命令并发写同一个 RS485 串口。 */
    bool m_overlayProbeRunning;         /* m_overlayProbeRunning 防止 overlay socket 查询重入。 */
    bool m_autoVisionLocateRunning;     /* m_autoVisionLocateRunning 防止自动视觉 LOCATE 请求线程重入。 */
    bool m_networkProbeTimedOut;        /* m_networkProbeTimedOut 标记当前 4G 进程已超时，finished 时不再覆盖超时状态。 */
    bool m_locationProbeTimedOut;       /* m_locationProbeTimedOut 标记开机定位进程已超时，finished 时不再覆盖超时状态。 */
    bool m_locationBootProbeDone;       /* m_locationBootProbeDone 标记本 Qt 进程已经调度过一次 IP 定位，后续周期刷新不再调用。 */
    bool m_cloudProbeTimedOut;          /* m_cloudProbeTimedOut 标记当前云端进程已超时，finished 时不再覆盖超时状态。 */
    quint16 m_f4AutoCycleId;            /* m_f4AutoCycleId 保存 MP157 当前自动检测流程号，开始新流程时自增，停止后不复用旧值。 */
    quint16 m_f4BinarySequence;         /* m_f4BinarySequence 保存 MP157 二进制协议发送帧序号，每下发一帧自动流程命令自增一次。 */
    bool m_f4AutoRunning;               /* m_f4AutoRunning 表示 MP157 本地认为 F4 当前存在运行中的自动检测流程。 */
    bool m_f4AutoPaused;                /* m_f4AutoPaused 表示当前自动检测流程已暂停，只有继续或停止能改变该状态。 */
    bool m_f4ArmFlowRunning;            /* m_f4ArmFlowRunning 表示 MODEL_READY/ARM_JOB_START 后正在等待 WEIGHT_RESULT 和 LDC_RESULT。 */
    bool m_f4FinalSortRunning;          /* m_f4FinalSortRunning 表示 FINAL_SORT_RESULT 已下发，正在等待 F4 CYCLE_DONE。 */
    quint16 m_f4LastArmJobId;           /* m_f4LastArmJobId 保存最近一次 ARM_JOB_START 的 job_id，最终分拣必须带同一编号。 */
    quint8 m_f4LastModelResult;         /* m_f4LastModelResult 保存最近一次模型综合结果，供最终分拣和日志追踪。 */
    quint8 m_f4LastFinalBin;            /* m_f4LastFinalBin 保存最近一次最终分拣盘编号，便于 CYCLE_DONE 对账。 */
    QTimer m_healthTimer;               /* m_healthTimer 周期性调度整轮健康检测。 */
    QTimer m_networkTimeout;            /* m_networkTimeout 是 4G 测试短超时。 */
    QTimer m_locationTimeout;           /* m_locationTimeout 是开机单次高德 IP 定位超时。 */
    QTimer m_cloudTimeout;              /* m_cloudTimeout 是云端测试短超时。 */
    QProcess m_networkProcess;          /* m_networkProcess 异步执行 4g-ppp test。 */
    QProcess m_locationProcess;         /* m_locationProcess 异步执行 4g-location once，完成后自动退出。 */
    QProcess m_cloudProcess;            /* m_cloudProcess 异步执行 curl health。 */
};

/*
 * set_default_environment 的作用：
 *   设置 Qt Quick 在板端运行时的默认渲染环境。
 *
 * 主要流程：
 *   1. 若用户没有显式设置 TZ，则默认指定 CST-8，让 QML new Date() 显示北京时间。
 *   2. 调用 tzset() 让 libc/Qt 在进程启动阶段刷新本地时区缓存。
 *   3. 若用户没有显式设置 QT_OPENGL，则默认指定 es2。
 *   4. 若用户没有显式设置 QSG_RENDER_LOOP，则默认使用 threaded 渲染循环。
 *
 * 关键说明：
 *   不设置 QT_QUICK_BACKEND。Qt 5.12 在 eglfs 下默认使用 OpenGL scene graph；
 *   如果强制设置为 opengl，某些构建会尝试寻找名为 opengl 的 scenegraph 插件，
 *   反而导致 “Could not create scene graph context for backend 'opengl'”。
 *
 * 参数：
 *   无。
 *
 * 返回值：
 *   无返回值；环境变量会在当前进程中生效。
 */
static void set_default_environment()
{
    /* TZ 必须在 QGuiApplication 创建前设置，否则 QML 顶部时钟可能继续按 UTC 显示。 */
    if (qEnvironmentVariableIsEmpty("TZ")) {
        qputenv("TZ", DEFAULT_BOARD_TIME_ZONE);
    }

    /* qputenv 只改变环境变量；tzset 让当前进程立即按新的 TZ 计算本地时间。 */
    tzset();

    /* QT_OPENGL=es2 与 STM32MP157 的 Vivante/Nano OpenGL ES 驱动匹配。 */
    if (qEnvironmentVariableIsEmpty("QT_OPENGL")) {
        qputenv("QT_OPENGL", "es2");
    }

    /* threaded 渲染循环让 QML 渲染线程和 UI 线程分离，降低界面卡顿概率。 */
    if (qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP")) {
        qputenv("QSG_RENDER_LOOP", "threaded");
    }
}

/*
 * set_surface_format 的作用：
 *   在创建 QGuiApplication 前声明默认 OpenGL ES surface 格式。
 *
 * 主要流程：
 *   1. 设置 Qt 应用属性 AA_UseOpenGLES，让 Qt 避免选择桌面 OpenGL。
 *   2. 设置 QSurfaceFormat 为 OpenGLES 2.0，匹配板端 GPU 驱动能力。
 *
 * 参数：
 *   无。
 *
 * 返回值：
 *   无返回值；默认格式影响后续 QQuickView 创建的渲染 surface。
 */
static void set_surface_format()
{
    /* AA_UseOpenGLES 必须在 QGuiApplication 构造前设置。 */
    QCoreApplication::setAttribute(Qt::AA_UseOpenGLES);

    /* format 保存默认 surface 的颜色深度、深度缓冲和 OpenGL ES 版本。 */
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(2, 0);
    format.setDepthBufferSize(16);
    format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
}

/*
 * configure_parser 的作用：
 *   定义本程序支持的命令行参数。
 *
 * 参数：
 *   parser 是待配置的命令行解析器，由 main 创建并传入。
 *
 * 返回值：
 *   无返回值；函数会向 parser 注册 help、version、camera、windowed 选项。
 */
static void configure_parser(QCommandLineParser *parser)
{
    /* 描述文本用于 --help 输出，方便串口或 SSH 调试时确认程序用途。 */
    parser->setApplicationDescription(QStringLiteral("STM32MP157 Qt Quick UVC camera display"));

    /* --help 由 Qt 自动生成帮助信息。 */
    parser->addHelpOption();

    /* --version 输出应用版本，便于确认板端运行的是新程序。 */
    parser->addVersionOption();

    /* --camera 指定 UVC 摄像头节点，默认 /dev/video0。 */
    parser->addOption(QCommandLineOption(QStringList() << QStringLiteral("c") << QStringLiteral("camera"),
                                         QStringLiteral("指定 UVC 摄像头设备节点。"),
                                         QStringLiteral("device"),
                                         QString::fromLatin1(DEFAULT_CAMERA_DEVICE)));

    /* --width 指定 V4L2 采集宽度，用于在 CPU 占用和预览清晰度之间取舍。 */
    parser->addOption(QCommandLineOption(QStringLiteral("width"),
                                         QStringLiteral("指定 V4L2 采集宽度。"),
                                         QStringLiteral("pixels"),
                                         QString::number(DEFAULT_CAPTURE_WIDTH)));

    /* --height 指定 V4L2 采集高度，用于在 CPU 占用和预览清晰度之间取舍。 */
    parser->addOption(QCommandLineOption(QStringLiteral("height"),
                                         QStringLiteral("指定 V4L2 采集高度。"),
                                         QStringLiteral("pixels"),
                                         QString::number(DEFAULT_CAPTURE_HEIGHT)));

    /* --fps 指定 V4L2 采集帧率，帧率越高，拷贝和纹理上传次数越多。 */
    parser->addOption(QCommandLineOption(QStringLiteral("fps"),
                                         QStringLiteral("指定 V4L2 采集帧率。"),
                                         QStringLiteral("frames"),
                                         QString::number(DEFAULT_CAPTURE_FPS)));

    /* --video-backend 指定视频显示后端，默认保守走 V4L2VideoItem。 */
    parser->addOption(QCommandLineOption(QStringLiteral("video-backend"),
                                         QStringLiteral("指定视频后端：qt-safe、gst-qml 或 kms-overlay。"),
                                         QStringLiteral("backend"),
                                         QString::fromLatin1(BACKEND_QT_SAFE)));

    /* --gst-io-mode 指定 gst-qml 后端的 v4l2src 采集模式，便于在 mmap 稳定路线和 dmabuf 攻关路线之间切换。 */
    parser->addOption(QCommandLineOption(QStringLiteral("gst-io-mode"),
                                         QStringLiteral("指定 gst-qml 后端的 v4l2src io-mode：mmap 或 dmabuf。"),
                                         QStringLiteral("mode"),
                                         QString::fromLatin1(DEFAULT_GST_IO_MODE)));

    /* --windowed 用于桌面调试；板端正式运行默认全屏。 */
    parser->addOption(QCommandLineOption(QStringLiteral("windowed"),
                                         QStringLiteral("使用 1024x600 窗口模式而不是全屏。")));
}

/*
 * bounded_int_option 的作用：
 *   从命令行读取整数参数，并限制到安全范围。
 *
 * 主要流程：
 *   1. 使用 parser.value 读取字符串。
 *   2. 调用 QString::toInt 转成整数。
 *   3. 转换失败时返回 defaultValue。
 *   4. 转换成功时限制在 minValue 到 maxValue 之间。
 *
 * 参数：
 *   parser 是已经解析完成的命令行解析器。
 *   optionName 是参数名，例如 width、height 或 fps。
 *   defaultValue 是转换失败时使用的默认值。
 *   minValue/maxValue 是允许范围。
 *
 * 返回值：
 *   返回最终可用于 V4L2 配置的整数值。
 */
static int bounded_int_option(const QCommandLineParser &parser,
                              const QString &optionName,
                              int defaultValue,
                              int minValue,
                              int maxValue)
{
    bool ok = false;
    int value = parser.value(optionName).toInt(&ok);

    if (!ok) {
        return defaultValue;
    }

    if (value < minValue) {
        return minValue;
    }

    if (value > maxValue) {
        return maxValue;
    }

    return value;
}

/*
 * normalize_video_backend 的作用：
 *   把命令行传入的视频后端名称归一化，避免脚本别名导致 QML 判断分叉。
 *
 * 参数：
 *   backendName 是 --video-backend 传入的原始字符串。
 *
 * 返回值：
 *   返回 BACKEND_GST_QML、BACKEND_KMS_OVERLAY 或 BACKEND_QT_SAFE；未知值按安全预览处理。
 */
static QString normalize_video_backend(const QString &backendName)
{
    /* normalized 保存去空白和小写后的后端名，便于接受脚本中的大小写差异。 */
    const QString normalized = backendName.trimmed().toLower();

    /* gst-qml 是正式名称；qt-gst、qml-gl、gst-gl-in-qt 是兼容调试别名。 */
    if (normalized == QStringLiteral("gst-qml")
            || normalized == QStringLiteral("qt-gst")
            || normalized == QStringLiteral("qml-gl")
            || normalized == QStringLiteral("gst-gl-in-qt")) {
        return QString::fromLatin1(BACKEND_GST_QML);
    }

    /* kms-overlay 表示视频已经由外部 KMS plane 进程绘制，Qt 不再打开 /dev/video0。 */
    if (normalized == QStringLiteral("kms-overlay")
            || normalized == QStringLiteral("kms")
            || normalized == QStringLiteral("overlay")
            || normalized == QStringLiteral("drm-overlay")) {
        return QString::fromLatin1(BACKEND_KMS_OVERLAY);
    }

    /* 任何未知值都退回 qt-safe，避免现场误输参数后直接黑屏。 */
    return QString::fromLatin1(BACKEND_QT_SAFE);
}

/*
 * normalize_gst_io_mode 的作用：
 *   归一化 gst-qml 后端的 v4l2src io-mode 参数。
 *
 * 主要流程：
 *   1. 去除首尾空白并转成小写。
 *   2. 只接受已经用于本项目验证的 mmap 和 dmabuf。
 *   3. 未知值回退到 mmap，避免现场误传参数后再次走到已知会崩的 DMABUF 嵌入路线。
 *
 * 参数：
 *   ioModeName 是 --gst-io-mode 传入的原始字符串。
 *
 * 返回值：
 *   返回可直接传给 v4l2src io-mode 属性的字符串。
 */
static QString normalize_gst_io_mode(const QString &ioModeName)
{
    /* normalized 保存规整后的模式名，便于接受脚本或手工命令中的大小写差异。 */
    const QString normalized = ioModeName.trimmed().toLower();

    /* mmap 是当前 qmlglsink 集成优先稳定路线，dmabuf 保留给零拷贝继续攻关和复现实验。 */
    if (normalized == QStringLiteral("mmap") || normalized == QStringLiteral("dmabuf")) {
        return normalized;
    }

    /* 未知模式回退 mmap，避免误拼写导致 GStreamer 属性解析失败或进入危险路径。 */
    return QString::fromLatin1(DEFAULT_GST_IO_MODE);
}

/*
 * has_raw_argument 的作用：
 *   在 QGuiApplication 创建前检查是否传入某个调试参数。
 *
 * 主要流程：
 *   1. 遍历 argv 中的原始命令行字符串。
 *   2. 与目标参数名做精确匹配。
 *
 * 参数：
 *   argc/argv 是 main 收到的原始参数。
 *   optionName 是要查找的完整参数名，例如 --storage-self-test。
 *
 * 返回值：
 *   找到返回 true；没有找到返回 false。
 */
static bool has_raw_argument(int argc, char *argv[], const char *optionName)
{
    /* i 从 1 开始跳过程序名，只检查用户传入的参数。 */
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], optionName) == 0) {
            return true;
        }
    }

    return false;
}

/*
 * run_storage_self_test 的作用：
 *   不启动 QML 界面，直接复用 CameraStorageController 保存当前 overlay 帧。
 *
 * 主要流程：
 *   1. 创建 QCoreApplication，保证 Qt 文本、环境变量和事件基础设施可用。
 *   2. 调用 saveCurrentFrameToSdCard()，走和 QML 保存按钮相同的 C++ 控制器逻辑。
 *   3. 把完整返回文本打印到 stdout，便于 SSH 自动化判断。
 *
 * 参数：
 *   argc/argv 是 main 收到的原始参数。
 *
 * 返回值：
 *   返回 EXIT_SUCCESS 表示保存成功；返回 EXIT_FAILURE 表示保存失败。
 */
static int run_storage_self_test(int argc, char *argv[])
{
    /* SSH 自检也设置默认时区，保证写入历史记录的 upload_time 与屏幕顶部时间一致。 */
    set_default_environment();

    QCoreApplication app(argc, argv);
    CameraStorageController storageController;
    UploadHistoryModel uploadHistory(QString::fromLatin1(DEFAULT_UPLOAD_HISTORY_FILE));

    /* 自检入口也复用同一个历史模型，保证 SSH 保存自检产生的真实上传记录能在历史页中看到。 */
    storageController.setHistoryModel(&uploadHistory);

    const QString result = storageController.saveCurrentFrameToSdCard();
    QTextStream(stdout) << result << '\n';
    return result.startsWith(QStringLiteral("保存成功：")) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/*
 * run_detect_self_test 的作用：
 *   不启动 QML 界面，直接复用 CameraStorageController 的双模型检测链路。
 *
 * 主要流程：
 *   1. 创建 QCoreApplication，保证 Qt 文本、环境变量、QProcess 和文件接口可用。
 *   2. 创建 UploadHistoryModel，检测成功后和屏幕点击一样追加当天 upload_history_YYYYMMDD.json。
 *   3. 调用 detectCurrentFrameOnce()，串行执行 SAVE_DETECT、defect-classify、defect-segment 和 COS 上传。
 *   4. 把最终 RESULT 或失败原因打印到 stdout，便于 SSH 自动化判断。
 *
 * 参数：
 *   argc/argv 是 main 收到的原始参数。
 *
 * 返回值：
 *   返回 EXIT_SUCCESS 表示双模型检测主链路跑通；返回 EXIT_FAILURE 表示任一步失败。
 */
static int run_detect_self_test(int argc, char *argv[])
{
    /* 自检入口也设置默认时区，保证历史记录 upload_time 与屏幕顶部时间一致。 */
    set_default_environment();

    QCoreApplication app(argc, argv);
    CameraStorageController storageController;
    DetectSettingsController detectSettings;
    UploadHistoryModel uploadHistory(QString::fromLatin1(DEFAULT_UPLOAD_HISTORY_FILE));

    /* 自检入口复用同一个历史模型，保证 SSH 检测成功后屏幕历史页能看到这条记录。 */
    storageController.setHistoryModel(&uploadHistory);
    storageController.setDetectSettingsController(&detectSettings);

    const QString result = storageController.detectCurrentFrameForSelfTest();
    QTextStream(stdout) << result << '\n';
    return result.startsWith(QStringLiteral("RESULT ")) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/*
 * run_alarm_snapshot_self_test 的作用：
 *   不启动 QML 界面，直接复用 CameraStorageController 写一份时间戳告警诊断快照。
 *
 * 主要流程：
 *   1. 创建 QCoreApplication，保证 Qt 文本编码、时区和文件接口可用。
 *   2. 构造一份包含告警码、相机状态、存储状态和历史段落的最小诊断文本。
 *   3. 调用 saveAlarmSnapshotToSdCard() 追加 /mnt/sdcard/logs/qt_alarm_snapshot_YYYYMMDD.txt。
 *   4. 把返回结果打印到 stdout，便于 SSH 自动化判断文件落盘是否成功。
 *
 * 参数：
 *   argc/argv 是 main 收到的原始参数。
 *
 * 返回值：
 *   返回 EXIT_SUCCESS 表示诊断快照保存成功；返回 EXIT_FAILURE 表示保存失败。
 */
static int run_alarm_snapshot_self_test(int argc, char *argv[])
{
    /* 自检入口也设置默认业务时区，保证 snapshot_time 与屏幕顶部北京时间一致。 */
    set_default_environment();

    QCoreApplication app(argc, argv);
    CameraStorageController storageController;

    /* snapshotText 保存最小但可判定的诊断内容，字段名与 QML alarmSnapshotText() 保持一致。 */
    const QString snapshotText =
        QStringLiteral("STM32MP157 Qt Alarm Snapshot\n")
        + QStringLiteral("snapshot_time=")
        + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
        + QLatin1Char('\n')
        + QStringLiteral("alarm_code=0x0007\n")
        + QStringLiteral("alarm_title=SSH self test\n")
        + QStringLiteral("alarm_level=记录\n")
        + QStringLiteral("alarm_status=自检\n")
        + QStringLiteral("camera_status=SSH自检\n")
        + QStringLiteral("storage_state=alarm-snapshot-self-test\n")
        + QStringLiteral("[recent_alarm_history]\n")
        + QStringLiteral("--:--:-- 0x0007 记录 SSH告警诊断快照自检 自检\n");

    const QString result = storageController.saveAlarmSnapshotToSdCard(snapshotText);
    QTextStream(stdout) << result << '\n';
    return result.startsWith(QStringLiteral("诊断已保存：")) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/*
 * run_alarm_log_self_test 的作用：
 *   不启动 QML 界面，直接复用 CameraStorageController 写一份时间戳自动告警日志。
 *
 * 主要流程：
 *   1. 创建 QCoreApplication，保证 Qt 文本编码、时区和文件接口可用。
 *   2. 构造一份模拟“模型检测失败”的告警文本，字段与 QML buildAlarmLogText() 保持一致。
 *   3. 调用 recordAlarmIssueToSdCard() 追加 /mnt/sdcard/logs/qt_alarm_YYYYMMDD.log。
 *   4. 把返回结果打印到 stdout，便于 SSH 自动化判断文件落盘是否成功。
 *
 * 参数：
 *   argc/argv 是 main 收到的原始参数。
 *
 * 返回值：
 *   返回 EXIT_SUCCESS 表示告警日志保存成功；返回 EXIT_FAILURE 表示保存失败。
 */
static int run_alarm_log_self_test(int argc, char *argv[])
{
    /* 自检入口也设置默认业务时区，保证 occurrence_time 与屏幕顶部北京时间一致。 */
    set_default_environment();

    QCoreApplication app(argc, argv);
    CameraStorageController storageController;

    /* alarmText 保存最小但可判定的自动告警内容，便于静态和板端 SSH 验证日志字段。 */
    const QString alarmText =
        QStringLiteral("STM32MP157 Qt Alarm Log\n")
        + QStringLiteral("occurrence_time=")
        + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
        + QLatin1Char('\n')
        + QStringLiteral("source=model-detect-failed\n")
        + QStringLiteral("alarm_code=ALM-MODEL-001\n")
        + QStringLiteral("alarm_title=SSH automatic alarm log self test\n")
        + QStringLiteral("alarm_level=预警\n")
        + QStringLiteral("current_status=自检\n")
        + QStringLiteral("device_health=SSH自检\n")
        + QStringLiteral("[troubleshooting]\n")
        + QStringLiteral("1. 检查模型程序、模型文件、标签文件和 SD 卡图片路径。\n");

    const QString result = storageController.recordAlarmIssueToSdCard(QStringLiteral("model-detect-failed"),
                                                                      alarmText);
    QTextStream(stdout) << result << '\n';
    return result.startsWith(QStringLiteral("告警日志已保存：")) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/*
 * run_settings_log_self_test 的作用：
 *   不启动 QML 界面，直接复用参数日志落盘路径，并验证日志查看模型能扫描到当天参数日志。
 *
 * 主要流程：
 *   1. 创建 QCoreApplication，保证 Qt 文件、时间和文本接口可用。
 *   2. 构造与 QML settingsLogText() 同字段的保存配置和导出摘要两段参数日志。
 *   3. 调用 recordSettingsSummaryToSdCard() 追加 qt_settings_YYYYMMDD.log。
 *   4. 刷新 LogFileModel settingsLogModel，检查日志查看页同源模型能看到 qt_settings 文件。
 *
 * 参数：
 *   argc/argv 是 main 收到的原始参数。
 *
 * 返回值：
 *   两次日志写入成功且模型扫描到 qt_settings 日志时返回 EXIT_SUCCESS，否则返回 EXIT_FAILURE。
 */
static int run_settings_log_self_test(int argc, char *argv[])
{
    /* 自检入口也设置默认业务时区，保证参数日志文件名和屏幕日期一致。 */
    set_default_environment();

    /* app 提供 Qt 文件系统、日期时间和 QTextStream 所需的应用上下文。 */
    QCoreApplication app(argc, argv);

    /* storageController 复用 QML 保存配置和导出摘要使用的同一条日志落盘路径。 */
    CameraStorageController storageController;

    /* settingsLogModel 与 QML 日志查看页使用同一个模型类，确保自检覆盖“写入后能被列表看到”。 */
    LogFileModel settingsLogModel(QString::fromLatin1(DEFAULT_SDCARD_LOG_DIR));

    /* nowText 保存本次自检时间，写入日志正文便于 SSH tail 直接定位本次自检段落。 */
    const QString nowText = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));

    /* summaryPrefix 保存参数日志通用字段，字段名与 QML settingsLogText() 保持一致。 */
    const QString summaryPrefix =
        QStringLiteral("STM32MP157 Qt Settings Summary\n")
        + QStringLiteral("time=") + nowText + QLatin1Char('\n')
        + QStringLiteral("config_path=") + QString::fromLatin1(DEFAULT_DETECT_SETTINGS_FILE) + QLatin1Char('\n')
        + QStringLiteral("part_type=波形垫圈\n")
        + QStringLiteral("model_threshold=0.650 (65.0%)\n")
        + QStringLiteral("review_threshold=0.820 (82.0%)\n")
        + QStringLiteral("roi_size=352\n")
        + QStringLiteral("segment_min_component_pixels=20\n")
        + QStringLiteral("segment_review_pixels=80\n")
        + QStringLiteral("segment_bad_pixels=300\n")
        + QStringLiteral("segment_strong_component_pixels=120\n")
        + QStringLiteral("overlay_alpha=0.45\n")
        + QStringLiteral("auto_upload_enabled=true\n")
        + QStringLiteral("f4_arm_result_timeout_ms=75000\n")
        + QStringLiteral("stepper_motor[0].name=传送带电机\n")
        + QStringLiteral("stepper_motor[0].role=conveyor\n")
        + QStringLiteral("stepper_motor[0].serial=UART4 PC10/PC11\n")
        + QStringLiteral("stepper_motor[0].address=1\n")
        + QStringLiteral("stepper_motor[0].min_step=20\n")
        + QStringLiteral("stepper_motor[0].normal_speed_rpm=300\n")
        + QStringLiteral("stepper_motor[0].scan_speed_rpm=40\n")
        + QStringLiteral("stepper_motor[0].direction=1 (正向)\n")
        + QStringLiteral("stepper_motor[1].name=摄像头左右电机\n")
        + QStringLiteral("stepper_motor[1].role=camera_lateral\n")
        + QStringLiteral("stepper_motor[1].serial=USART6 PC6/PC7\n")
        + QStringLiteral("stepper_motor[1].address=3\n")
        + QStringLiteral("stepper_motor[1].min_step=5\n")
        + QStringLiteral("stepper_motor[1].normal_speed_rpm=137\n")
        + QStringLiteral("stepper_motor[1].scan_speed_rpm=0\n")
        + QStringLiteral("stepper_motor[1].direction=1 (正向)\n")
        + QStringLiteral("stepper_motor[2].name=摄像头上下电机\n")
        + QStringLiteral("stepper_motor[2].role=camera_z\n")
        + QStringLiteral("stepper_motor[2].serial=USART6 PC6/PC7\n")
        + QStringLiteral("stepper_motor[2].address=2\n")
        + QStringLiteral("stepper_motor[2].min_step=5\n")
        + QStringLiteral("stepper_motor[2].normal_speed_rpm=5000\n")
        + QStringLiteral("stepper_motor[2].scan_speed_rpm=0\n")
        + QStringLiteral("stepper_motor[2].direction=1 (正向)\n")
        + QStringLiteral("stepper_motor[2].z_motion_timeout_ms=10000\n")
        + QStringLiteral("classify_args=--roi 352 --bad-threshold 0.650\n")
        + QStringLiteral("segment_args=--roi 352 --alpha 0.45 --min-component-pixels 20 --review-defect-pixels 80 --bad-defect-pixels 300 --strong-component-pixels 120\n");

    /* saveSummary 模拟参数页点击“保存配置”后写入的日志段。 */
    const QString saveSummary =
        QStringLiteral("action=保存配置\n")
        + QStringLiteral("action_result=settings-log-self-test save\n")
        + summaryPrefix;

    /* exportSummary 模拟参数页点击“导出摘要”后写入的日志段。 */
    const QString exportSummary =
        QStringLiteral("action=导出摘要\n")
        + QStringLiteral("action_result=settings-log-self-test export\n")
        + summaryPrefix;

    /* saveResult 保存第一段日志写入结果，成功时应以“参数日志已保存：”开头。 */
    const QString saveResult = storageController.recordSettingsSummaryToSdCard(QStringLiteral("settings-save"),
                                                                               saveSummary);

    /* exportResult 保存第二段日志写入结果，成功时应写到同一天 qt_settings_YYYYMMDD.log。 */
    const QString exportResult = storageController.recordSettingsSummaryToSdCard(QStringLiteral("settings-export"),
                                                                                 exportSummary);

    /* 写入完成后刷新模型，验证日志查看页面实际使用的扫描路径能看到这份参数日志。 */
    settingsLogModel.refresh();

    /* logModelContains 记录是否在日志模型中找到 qt_settings 文件。 */
    bool logModelContains = false;

    /* 遍历日志模型条目，检查文件名是否以 qt_settings_ 开头且后缀为 .log。 */
    for (int row = 0; row < settingsLogModel.count(); ++row) {
        const QVariantMap entry = settingsLogModel.entryAt(row);
        const QString fileName = entry.value(QStringLiteral("fileName")).toString();

        if (fileName.startsWith(QString::fromLatin1(SETTINGS_LOG_PREFIX) + QLatin1Char('_'))
                && fileName.endsWith(QStringLiteral(".log"))) {
            logModelContains = true;
            break;
        }
    }

    /* 输出完整自检摘要，方便 README 中的 SSH 命令用 grep/tail 直接判断。 */
    QTextStream(stdout)
            << "settings-log-self-test\n"
            << "save_result=" << saveResult << '\n'
            << "export_result=" << exportResult << '\n'
            << "log_model_contains=" << (logModelContains ? "qt_settings_YYYYMMDD.log" : "missing") << '\n'
            << "log_model_status=" << settingsLogModel.statusText() << '\n';

    /* 两段日志都成功且模型能扫描到参数日志，才认为自检通过。 */
    if (saveResult.startsWith(QStringLiteral("参数日志已保存："))
            && exportResult.startsWith(QStringLiteral("参数日志已保存："))
            && logModelContains) {
        return EXIT_SUCCESS;
    }

    return EXIT_FAILURE;
}

/*
 * create_gst_qml_pipeline 的作用：
 *   创建 “v4l2src(io-mode) -> capsfilter -> glupload -> qmlglsink” 管线。
 *
 * 主要流程：
 *   1. 创建 pipeline 和每个 element。
 *   2. 配置 v4l2src 设备节点与指定采集模式。
 *   3. 用 capsfilter 固定 YUY2、分辨率和帧率，复用已验证的板端输入格式。
 *   4. mmap 模式先用 videoconvert 转 RGBA，保证 glupload 能协商系统内存帧。
 *   5. dmabuf 模式在 glupload 后增加 glcolorconvert 和 gleffects_identity，强制 GPU 重新渲染一遍纹理后再交给 Qt。
 *   6. 把 qmlglsink 返回给调用者，后续绑定 QML 中的 GstGLVideoItem。
 *
 * 参数：
 *   cameraDevice 是 /dev/video0 这类 V4L2 摄像头节点。
 *   captureWidth/captureHeight/captureFps 是请求的采集格式。
 *   gstIoMode 是 v4l2src 的 io-mode，mmap 优先保证 Qt 嵌入稳定，dmabuf 用于零拷贝攻关。
 *   sinkOut 用于返回 qmlglsink 指针；该指针归 pipeline 持有，调用者不能单独 unref。
 *   errorText 用于返回失败原因，便于 QML 状态和串口日志诊断。
 *
 * 返回值：
 *   成功返回 GstElement* pipeline，调用者负责 gst_object_unref；
 *   失败返回 nullptr，并写入 errorText。
 */
static GstElement *create_gst_qml_pipeline(const QString &cameraDevice,
                                           int captureWidth,
                                           int captureHeight,
                                           int captureFps,
                                           const QString &gstIoMode,
                                           GstElement **sinkOut,
                                           QString *errorText)
{
    /* sinkOut 先置空，避免失败路径留下旧指针。 */
    if (sinkOut) {
        *sinkOut = nullptr;
    }

    /* useCpuColorConvert 表示当前是否需要 CPU 颜色转换；mmap 系统内存 YUY2 不能直接进入 glupload。 */
    const bool useCpuColorConvert = (gstIoMode == QStringLiteral("mmap"));

    /* pipeline 是整条 GStreamer 管线的父对象。 */
    GstElement *pipeline = gst_pipeline_new("qt-camera-gst-qml-pipeline");

    /* src 从 UVC 摄像头取帧；capsfilter 固定格式；glupload 上传到 GL；sink 交给 QML Item 显示。 */
    GstElement *src = gst_element_factory_make("v4l2src", "camera-source");
    GstElement *capsFilter = gst_element_factory_make("capsfilter", "camera-caps");
    GstElement *preConvert = useCpuColorConvert ? gst_element_factory_make("videoconvert", "camera-videoconvert") : nullptr;
    GstElement *rgbaCapsFilter = useCpuColorConvert ? gst_element_factory_make("capsfilter", "camera-rgba-caps") : nullptr;
    GstElement *glupload = gst_element_factory_make("glupload", "camera-glupload");
    GstElement *glcolorconvert = gst_element_factory_make("glcolorconvert", "camera-glcolorconvert");
    GstElement *glidentity = gst_element_factory_make("gleffects_identity", "camera-glidentity");
    GstElement *sink = gst_element_factory_make("qmlglsink", "camera-qmlglsink");

    /* deviceBytes 保存 Qt 字符串转成的本地编码，放在函数顶层避免 goto 跨越对象初始化。 */
    QByteArray deviceBytes;

    /* ioModeBytes 保存 v4l2src io-mode 字符串，便于传给 GObject 属性解析器。 */
    QByteArray ioModeBytes;

    /* caps 保存输入 capsfilter 使用的视频格式描述，失败清理时按是否为空判断是否释放。 */
    GstCaps *caps = nullptr;

    /* rgbaCaps 保存 mmap 路径 videoconvert 输出的 RGBA caps，让 glupload 接收普通 RGBA 系统内存。 */
    GstCaps *rgbaCaps = nullptr;

    /* elementsAdded 标记 element 是否已经被 pipeline 接管，决定失败时如何释放资源。 */
    bool elementsAdded = false;

    /* 任一 element 创建失败都说明 rootfs 插件或 GStreamer 安装不完整。 */
    if (!pipeline || !src || !capsFilter || !glupload || !glcolorconvert || !glidentity || !sink
            || (useCpuColorConvert && (!preConvert || !rgbaCapsFilter))) {
        if (errorText) {
            *errorText = QStringLiteral("GStreamer 元素缺失：需要 v4l2src、capsfilter、videoconvert、glupload、glcolorconvert、gleffects_identity、qmlglsink");
        }
        goto fail;
    }

    /* device 属性指定 UVC 节点；GObject 会复制字符串，因此临时 QByteArray 可以安全释放。 */
    deviceBytes = cameraDevice.toLocal8Bit();
    g_object_set(src, "device", deviceBytes.constData(), NULL);

    /* io-mode 使用字符串设置，避免硬编码 enum 数值导致不同 GStreamer 版本不兼容。 */
    ioModeBytes = gstIoMode.toLatin1();
    gst_util_set_object_arg(G_OBJECT(src), "io-mode", ioModeBytes.constData());

    /* caps 固定为已验证的 YUY2 格式，保持和独立 gst-gl 管线一致。 */
    caps = gst_caps_new_simple("video/x-raw",
                               "format", G_TYPE_STRING, "YUY2",
                               "width", G_TYPE_INT, captureWidth,
                               "height", G_TYPE_INT, captureHeight,
                               "framerate", GST_TYPE_FRACTION, captureFps, 1,
                               NULL);

    /* capsfilter 持有 caps 后，需要释放本地引用。 */
    g_object_set(capsFilter, "caps", caps, NULL);
    gst_caps_unref(caps);
    caps = nullptr;

    /* mmap 路径把 YUY2 系统内存转成 RGBA，避免 glupload 对 YUY2 系统内存协商失败。 */
    if (useCpuColorConvert) {
        rgbaCaps = gst_caps_new_simple("video/x-raw",
                                       "format", G_TYPE_STRING, "RGBA",
                                       NULL);
        g_object_set(rgbaCapsFilter, "caps", rgbaCaps, NULL);
        gst_caps_unref(rgbaCaps);
        rgbaCaps = nullptr;
    }

    /* sync=false 与独立 glimagesink 验证保持一致，避免显示时钟阻塞影响现场观察。 */
    g_object_set(sink, "sync", FALSE, NULL);

    /* element 加入 pipeline 后，生命周期由 pipeline 统一管理；mmap 路径额外插入 CPU 色彩转换。 */
    if (useCpuColorConvert) {
        gst_bin_add_many(GST_BIN(pipeline), src, capsFilter, preConvert, rgbaCapsFilter, glupload, glcolorconvert, glidentity, sink, NULL);
    } else {
        gst_bin_add_many(GST_BIN(pipeline), src, capsFilter, glupload, glcolorconvert, glidentity, sink, NULL);
    }
    elementsAdded = true;

    /* link 失败通常是 caps、插件能力或 qmlglsink 缺依赖导致。 */
    if (useCpuColorConvert
            ? !gst_element_link_many(src, capsFilter, preConvert, rgbaCapsFilter, glupload, glcolorconvert, glidentity, sink, NULL)
            : !gst_element_link_many(src, capsFilter, glupload, glcolorconvert, glidentity, sink, NULL)) {
        if (errorText) {
            *errorText = QStringLiteral("GStreamer 管线连接失败：v4l2src->glupload/glcolorconvert/gleffects_identity->qmlglsink caps 不兼容");
        }
        goto fail;
    }

    /* sinkOut 返回给 main，用于在 QML 加载后设置 widget 属性。 */
    if (sinkOut) {
        *sinkOut = sink;
    }

    /* 成功时返回 pipeline，后续由 main 在退出时置 NULL 并释放。 */
    return pipeline;

fail:
    /* 如果 element 已加入 pipeline，释放 pipeline 会递归释放它们。 */
    if (caps) {
        gst_caps_unref(caps);
    }

    /* mmap 路径的 RGBA caps 若在失败前尚未交给 capsfilter，需要在这里释放。 */
    if (rgbaCaps) {
        gst_caps_unref(rgbaCaps);
    }

    /* 如果 element 已加入 pipeline，释放 pipeline 会递归释放它们。 */
    if (pipeline) {
        gst_object_unref(pipeline);
    }

    /* 如果失败发生在加入 pipeline 之前，需要逐个释放已创建的裸 element。 */
    if (!elementsAdded) {
        if (src) {
            gst_object_unref(src);
        }
        if (capsFilter) {
            gst_object_unref(capsFilter);
        }
        if (preConvert) {
            gst_object_unref(preConvert);
        }
        if (rgbaCapsFilter) {
            gst_object_unref(rgbaCapsFilter);
        }
        if (glupload) {
            gst_object_unref(glupload);
        }
        if (glcolorconvert) {
            gst_object_unref(glcolorconvert);
        }
        if (glidentity) {
            gst_object_unref(glidentity);
        }
        if (sink) {
            gst_object_unref(sink);
        }
    }

    return nullptr;
}

/*
 * main 的作用：
 *   程序主入口，完成 Qt 图形环境初始化并启动 QML 界面。
 *
 * 参数：
 *   argc 是命令行参数数量。
 *   argv 是命令行参数内容。
 *
 * 返回值：
 *   QML 加载失败返回 EXIT_FAILURE；正常运行返回 Qt 事件循环退出码。
 */
int main(int argc, char *argv[])
{
    /* --storage-self-test 用于 SSH 验证保存按钮同一条 C++ 控制路径，不需要启动 Qt Quick/eglfs。 */
    if (has_raw_argument(argc, argv, "--storage-self-test")) {
        return run_storage_self_test(argc, argv);
    }

    /* --detect-self-test 用于 SSH 验证双模型串行检测链路，不需要启动 Qt Quick/eglfs。 */
    if (has_raw_argument(argc, argv, "--detect-self-test")) {
        return run_detect_self_test(argc, argv);
    }

    /* --alarm-snapshot-self-test 用于 SSH 验证告警维护保存诊断同一条 C++ 落盘路径。 */
    if (has_raw_argument(argc, argv, "--alarm-snapshot-self-test")) {
        return run_alarm_snapshot_self_test(argc, argv);
    }

    /* --alarm-log-self-test 用于 SSH 验证自动告警日志同一条 C++ 落盘路径。 */
    if (has_raw_argument(argc, argv, "--alarm-log-self-test")) {
        return run_alarm_log_self_test(argc, argv);
    }

    /* --settings-log-self-test 用于 SSH 验证参数保存/导出日志和日志查看模型同一条路径。 */
    if (has_raw_argument(argc, argv, "--settings-log-self-test")) {
        return run_settings_log_self_test(argc, argv);
    }

    /* 先初始化 GStreamer，让 qmlglsink 插件能在 QML 加载前注册 GstGLVideoItem。 */
    gst_init(&argc, &argv);

    /* 先设置 OpenGL ES 渲染属性，再创建 Qt 应用对象。 */
    set_surface_format();

    /* 设置缺省环境变量，用户在脚本中显式设置时不会被覆盖。 */
    set_default_environment();

    /* app 管理 Qt 事件循环、平台插件和图形资源生命周期。 */
    QGuiApplication app(argc, argv);

    /* 注册 V4L2VideoItem，QML 可通过 import IndustrialCamera 1.0 直接使用它。 */
    qmlRegisterType<V4L2VideoItem>("IndustrialCamera", 1, 0, "V4L2VideoItem");

    /* 应用元信息用于日志、窗口标题和 --version 输出。 */
    QCoreApplication::setApplicationName(QStringLiteral("qt_camera_display"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("STM32MP157"));

    /* parser 解析 --camera 和 --windowed，Qt 自身参数会先由 QGuiApplication 处理。 */
    QCommandLineParser parser;
    configure_parser(&parser);
    parser.process(app);

    /* cameraDevice 保存传给 QML Camera 的设备 ID；在 Linux/GStreamer 后端通常就是 /dev/video0。 */
    const QString cameraDevice = parser.value(QStringLiteral("camera"));

    /* captureWidth/captureHeight/captureFps 保存 V4L2 实际请求参数，默认偏低以降低 CPU 占用。 */
    const int captureWidth = bounded_int_option(parser, QStringLiteral("width"), DEFAULT_CAPTURE_WIDTH, 160, 1920);
    const int captureHeight = bounded_int_option(parser, QStringLiteral("height"), DEFAULT_CAPTURE_HEIGHT, 120, 1080);
    const int captureFps = bounded_int_option(parser, QStringLiteral("fps"), DEFAULT_CAPTURE_FPS, 1, 30);

    /* requestedVideoBackend 保存用户请求的后端；actualVideoBackend 在 qmlglsink 缺失时会回退为 qt-safe。 */
    const QString requestedVideoBackend = normalize_video_backend(parser.value(QStringLiteral("video-backend")));
    QString actualVideoBackend = requestedVideoBackend;

    /* gstIoMode 保存 gst-qml 后端使用的 V4L2 采集模式，mmap 稳定嵌入，dmabuf 保留给零拷贝验证。 */
    const QString gstIoMode = normalize_gst_io_mode(parser.value(QStringLiteral("gst-io-mode")));

    /* gstPipeline 是 gst-qml 后端的运行管线；gstSink 是待绑定 QML Item 的 qmlglsink。 */
    GstElement *gstPipeline = nullptr;
    GstElement *gstSink = nullptr;

    /* gstStatusText 会暴露给 QML，用于在 GL 后端初始化失败时显示明确原因。 */
    QString gstStatusText = QStringLiteral("GStreamer GL 未启用");

    /* 请求 gst-qml 时，先创建 qmlglsink，让 QML import 能找到 GstGLVideoItem 类型。 */
    if (requestedVideoBackend == QString::fromLatin1(BACKEND_GST_QML)) {
        gstPipeline = create_gst_qml_pipeline(cameraDevice,
                                              captureWidth,
                                              captureHeight,
                                              captureFps,
                                              gstIoMode,
                                              &gstSink,
                                              &gstStatusText);

        /* qmlglsink 创建失败时退回安全预览，避免现场只看到黑屏。 */
        if (gstPipeline && gstSink) {
            actualVideoBackend = QString::fromLatin1(BACKEND_GST_QML);
            gstStatusText = QStringLiteral("GStreamer GL 就绪(") + gstIoMode + QStringLiteral(")");
        } else {
            actualVideoBackend = QString::fromLatin1(BACKEND_QT_SAFE);
            qWarning() << "gst-qml 后端不可用，回退 qt-safe:" << gstStatusText;
        }
    }

    /* windowed 记录是否使用窗口模式；没有该参数时按嵌入式全屏界面运行。 */
    const bool windowed = parser.isSet(QStringLiteral("windowed"));

    /* view 负责加载 QML 根对象，并把它显示到 eglfs/wayland 平台窗口。 */
    QQuickView view;

    /* storageController 提供 SD 卡保存图片和安全卸载的真实动作入口。 */
    CameraStorageController storageController;

    /* detectSettings 管理参数设置页真实 JSON 配置，并为检测线程提供稳定参数快照。 */
    DetectSettingsController detectSettings;

    /* deviceHealth 负责异步探测 4G、相机、F4、云端和 SD 卡真实状态。 */
    DeviceHealthController deviceHealth(actualVideoBackend, cameraDevice);

    /* uploadHistory 保存每次保存/上传动作的本地历史记录，QML 历史页直接读取它。 */
    UploadHistoryModel uploadHistory(QString::fromLatin1(DEFAULT_UPLOAD_HISTORY_FILE));

    /* logFileModel 保存 SD 卡日志目录的只读文件列表，QML 日志查看页直接读取它。 */
    LogFileModel logFileModel(QString::fromLatin1(DEFAULT_SDCARD_LOG_DIR));

    /* cloudReviewServer 接收云端按钮回写的最终复核结论，并更新每日 upload_history_YYYYMMDD.json。 */
    CloudReviewServer cloudReviewServer(&uploadHistory);

    /* 保存控制器拿到历史模型后，保存/上传完成时可以立即追加一条记录。 */
    storageController.setHistoryModel(&uploadHistory);

    /* 保存控制器拿到检测参数控制器后，分类阈值、复核阈值、ROI 和上传开关才会进入真实检测链路。 */
    storageController.setDetectSettingsController(&detectSettings);

    /* SizeRootObjectToView 让 QML 根界面跟随窗口尺寸，适配 1024x600 全屏。 */
    view.setResizeMode(QQuickView::SizeRootObjectToView);

    /* 把摄像头节点暴露给 QML，QML Camera 会用它选择 UVC 设备。 */
    view.rootContext()->setContextProperty(QStringLiteral("cameraDeviceId"), cameraDevice);

    /* 把采集参数暴露给 QML，V4L2VideoItem 会按这些参数打开摄像头。 */
    view.rootContext()->setContextProperty(QStringLiteral("cameraCaptureWidth"), captureWidth);
    view.rootContext()->setContextProperty(QStringLiteral("cameraCaptureHeight"), captureHeight);
    view.rootContext()->setContextProperty(QStringLiteral("cameraCaptureFps"), captureFps);

    /* 把实际视频后端暴露给 QML，让界面选择 V4L2VideoItem 或 GstGLVideoItem。 */
    view.rootContext()->setContextProperty(QStringLiteral("cameraVideoBackend"), actualVideoBackend);

    /* 把 GStreamer 后端状态暴露给 QML，便于界面和日志区显示初始化结果。 */
    view.rootContext()->setContextProperty(QStringLiteral("cameraGstStatusText"), gstStatusText);

    /* 把版本号暴露给 QML，便于界面底部显示当前程序版本。 */
    view.rootContext()->setContextProperty(QStringLiteral("appVersion"), QCoreApplication::applicationVersion());

    /* 把 SD 卡动作控制器暴露给 QML，按钮点击时调用真实 C++/overlay/脚本链路。 */
    view.rootContext()->setContextProperty(QStringLiteral("storageController"), &storageController);

    /* 把真实检测参数控制器暴露给 QML，参数页保存到 JSON 后检测线程会读取同一份配置。 */
    view.rootContext()->setContextProperty(QStringLiteral("detectSettings"), &detectSettings);

    /* 把真实设备健康控制器暴露给 QML，顶部状态栏不再显示固定在线文案。 */
    view.rootContext()->setContextProperty(QStringLiteral("deviceHealth"), &deviceHealth);

    /* 把上传历史模型暴露给 QML，历史记录页面用它生成横向滑动卡片和详情页。 */
    view.rootContext()->setContextProperty(QStringLiteral("uploadHistory"), &uploadHistory);

    /* 把日志文件模型暴露给 QML，日志查看页面用它生成日志列表和详情弹窗。 */
    view.rootContext()->setContextProperty(QStringLiteral("logFileModel"), &logFileModel);

    /* 把云端复核回写服务暴露给 QML，后续状态栏或告警页可展示监听状态。 */
    view.rootContext()->setContextProperty(QStringLiteral("cloudReviewServer"), &cloudReviewServer);

    /* 从 qrc 资源加载主界面，避免板端部署时遗漏单独的 QML 文件。 */
    view.setSource(QUrl(QStringLiteral("qrc:/qml/Main.qml")));

    /* 如果 QML 语法或模块加载失败，直接返回失败，避免黑屏后误认为程序在运行。 */
    if (view.status() == QQuickView::Error) {
        if (gstPipeline) {
            gst_element_set_state(gstPipeline, GST_STATE_NULL);
            gst_object_unref(gstPipeline);
        }
        gst_deinit();
        return EXIT_FAILURE;
    }

    /* 设置窗口标题，窗口模式调试时能明确识别当前程序。 */
    view.setTitle(QStringLiteral("STM32MP157 工业缺陷检测界面"));

    /* 板端正式运行使用全屏；调试时可通过 --windowed 保持 1024x600 窗口。 */
    if (windowed) {
        view.resize(1024, 600);
        view.show();
    } else {
        view.showFullScreen();
    }

    /* 窗口显示后启动第一轮设备健康检测；所有耗时探测均异步执行，不阻塞界面触摸。 */
    deviceHealth.start();

    /* Qt 事件循环启动前先打开云端复核回写端口，云端按钮可按 record_id 修改本地历史。 */
    cloudReviewServer.startFromEnvironment();

    /* gst-qml 后端需要在窗口 show 之后绑定 qmlglsink，确保 Qt Quick 窗口已进入可曝光状态。 */
    if (actualVideoBackend == QString::fromLatin1(BACKEND_GST_QML)) {
        /* rootItem 是 QML 根节点；GstVideoSurface.qml 中 objectName 固定为 gstVideoItem。 */
        QQuickItem *rootItem = view.rootObject();
        QQuickItem *videoItem = rootItem ? rootItem->findChild<QQuickItem *>(QStringLiteral("gstVideoItem")) : nullptr;

        /* 找不到视频 Item 说明 QML Loader 没有实例化 GL 表面，继续运行只会黑屏。 */
        if (!videoItem) {
            qWarning() << "找不到 QML GstGLVideoItem：gstVideoItem";
            if (gstPipeline) {
                gst_element_set_state(gstPipeline, GST_STATE_NULL);
                gst_object_unref(gstPipeline);
            }
            gst_deinit();
            return EXIT_FAILURE;
        }

        /* qmlglsink 的 widget 属性接收 QQuickItem 指针，之后视频帧会绘制到该 Item。 */
        g_object_set(gstSink, "widget", videoItem, NULL);

        /* 按官方示例，在 Qt Quick 渲染同步阶段切到 PLAYING，避免 GL 上下文未就绪。 */
        view.scheduleRenderJob(new SetGstPipelineStateJob(gstPipeline, GST_STATE_PLAYING),
                               QQuickWindow::BeforeSynchronizingStage);
    }

    /* 进入 Qt 事件循环，直到窗口关闭或系统发送退出信号。 */
    const int ret = app.exec();

    /* 退出时先停止 GStreamer 管线，确保 /dev/video0 和 GL 资源释放干净。 */
    if (gstPipeline) {
        gst_element_set_state(gstPipeline, GST_STATE_NULL);
        gst_object_unref(gstPipeline);
    }

    /* 对称释放 GStreamer 全局资源，方便后续进程重新扫描插件。 */
    gst_deinit();

    /* 返回 Qt 事件循环退出码。 */
    return ret;
}

#include "main.moc" /* main.cpp 内定义了 CameraStorageController，qmake 需要包含 moc 生成的元对象代码。 */
