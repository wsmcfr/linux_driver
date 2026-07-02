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

#include "v4l2_video_item.h"  /* V4L2VideoItem 提供不依赖 QtMultimedia 的 UVC 预览控件。 */

#include <QAbstractListModel>   /* QAbstractListModel 用于把上传历史记录以模型形式暴露给 QML ListView。 */
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
#include <cstdlib>              /* EXIT_SUCCESS/EXIT_FAILURE 是 main 返回值语义。 */
#include <ctime>                /* tzset 用于让运行时立刻重新读取 TZ 时区变量。 */
#include <functional>           /* std::function 用于给检测同步流程注入“分类完成/双模型完成”进度回调。 */

#include <gst/gst.h>            /* GStreamer C API 用于创建 v4l2src->glupload->qmlglsink 管线。 */

#include <cerrno>               /* errno 保存 Unix socket 调用失败原因。 */
#include <cstdio>               /* fopen/fscanf/fclose 用于可靠读取 procfs；stdout 用于自检入口输出保存结果。 */
#include <cstring>              /* strerror 用于把 errno 转成人可读文本。 */
#include <fcntl.h>              /* open/O_NOCTTY 用于后台 F4 串口握手检测。 */
#include <termios.h>            /* termios 用于配置 F4 串口 115200 8N1 原始模式。 */

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

/* 默认 F4 串口波特率；当前项目串口测试工具和文档均使用 115200 8N1。 */
static const int DEFAULT_F4_SERIAL_BAUD = 115200;

/* 默认 F4 握手命令；用户已确认没有现成协议时先按 STATUS 查询实现。 */
static const char *DEFAULT_F4_HEALTH_QUERY = "STATUS\r\n";

/* F4 心跳发送间隔，单位毫秒；120000ms 等于 2 分钟，避免 Qt 每 8 秒健康刷新都占用 RS485 串口。 */
static const int F4_HEARTBEAT_INTERVAL_MS = 120000;

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
 * DetectSettingsSnapshot 的作用：
 *   保存参数设置页真正参与检测链路的配置快照。
 *
 * 字段说明：
 *   partType 是界面选择的真实零件中文名，当前用于摘要和历史诊断，不直接改模型类别顺序。
 *   modelThreshold 是分类模型 bad_total 判坏阈值，传给 defect-classify 的 --bad-threshold。
 *   reviewThreshold 是综合判定的复核阈值，分类置信度低于该值时进入 REVIEW。
 *   roiSize 是分类和 UNet 使用的中心 ROI 边长，传给两个模型程序的 --roi。
 *   segmentMinPixels 是 UNet 判 NG 的最小缺陷像素数，传给 defect-segment 的 --min-defect-pixels。
 *   overlayAlpha 是 UNet 叠加图透明度，传给 defect-segment 的 --alpha。
 *   autoUploadEnabled 为 false 时检测仍写本地历史，但跳过 COS 上传并返回 upload_status=SKIP。
 */
struct DetectSettingsSnapshot
{
    QString partType = QStringLiteral("波形垫圈");
    double modelThreshold = 0.85;
    double reviewThreshold = 0.65;
    int roiSize = 300;
    int segmentMinPixels = 1;
    double overlayAlpha = 0.45;
    bool autoUploadEnabled = true;
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
 * detectSettingsToVariantMap 的作用：
 *   把检测配置快照转换成 QML 可直接读取的 QVariantMap。
 *
 * 参数：
 *   settings 是要转换的检测配置快照。
 *
 * 返回值：
 *   返回包含 partType/modelThreshold/reviewThreshold/roiSize/segmentMinPixels/overlayAlpha/autoUploadEnabled 的 map。
 */
static QVariantMap detectSettingsToVariantMap(const DetectSettingsSnapshot &settings)
{
    QVariantMap map;

    map.insert(QStringLiteral("partType"), settings.partType);
    map.insert(QStringLiteral("modelThreshold"), settings.modelThreshold);
    map.insert(QStringLiteral("reviewThreshold"), settings.reviewThreshold);
    map.insert(QStringLiteral("roiSize"), settings.roiSize);
    map.insert(QStringLiteral("segmentMinPixels"), settings.segmentMinPixels);
    map.insert(QStringLiteral("overlayAlpha"), settings.overlayAlpha);
    map.insert(QStringLiteral("autoUploadEnabled"), settings.autoUploadEnabled);
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
        const bool uploadSucceeded = uploadStatus.startsWith(QStringLiteral("上传成功"));
        const QString refreshedUploadTime = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));

        m_entries[row].uploadStatus = uploadStatus;
        if (!recordId.isEmpty()) {
            m_entries[row].recordId = recordId;
        }
        if (!recordNo.isEmpty()) {
            m_entries[row].recordNo = recordNo;
        }
        m_entries[row].workflowText = uploadStatus.startsWith(QStringLiteral("上传成功"))
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
    Q_PROPERTY(int segmentMinPixels READ segmentMinPixels WRITE setSegmentMinPixels NOTIFY settingsChanged)
    Q_PROPERTY(double overlayAlpha READ overlayAlpha WRITE setOverlayAlpha NOTIFY settingsChanged)
    Q_PROPERTY(bool autoUploadEnabled READ autoUploadEnabled WRITE setAutoUploadEnabled NOTIFY settingsChanged)
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
     * segmentMinPixels 的作用：
     *   返回 UNet 判定 NG 所需的最小缺陷像素数。
     */
    int segmentMinPixels() const
    {
        return m_settings.segmentMinPixels;
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
     * setSegmentMinPixels 的作用：
     *   设置 UNet 判 NG 所需的最小缺陷像素数。
     *
     * 参数：
     *   value 是像素数量，0 表示只要有缺陷类像素就判 NG。
     */
    void setSegmentMinPixels(int value)
    {
        DetectSettingsSnapshot next = m_settings;

        next.segmentMinPixels = clampedInt(value, 0, 50000);
        applySettings(next, QStringLiteral("真实检测配置：UNet像素阈值已调整"));
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

        next.partType = object.value(QStringLiteral("part_type")).toString(
            object.value(QStringLiteral("partType")).toString(next.partType));
        next.modelThreshold = object.value(QStringLiteral("model_threshold")).toDouble(
            object.value(QStringLiteral("modelThreshold")).toDouble(next.modelThreshold));
        next.reviewThreshold = object.value(QStringLiteral("review_threshold")).toDouble(
            object.value(QStringLiteral("reviewThreshold")).toDouble(next.reviewThreshold));
        next.roiSize = object.value(QStringLiteral("roi_size")).toInt(
            object.value(QStringLiteral("roiSize")).toInt(next.roiSize));
        next.segmentMinPixels = object.value(QStringLiteral("segment_min_pixels")).toInt(
            object.value(QStringLiteral("segmentMinPixels")).toInt(next.segmentMinPixels));
        next.overlayAlpha = object.value(QStringLiteral("overlay_alpha")).toDouble(
            object.value(QStringLiteral("overlayAlpha")).toDouble(next.overlayAlpha));
        next.autoUploadEnabled = object.value(QStringLiteral("auto_upload_enabled")).toBool(
            object.value(QStringLiteral("autoUploadEnabled")).toBool(next.autoUploadEnabled));

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

        object.insert(QStringLiteral("schema_version"), 1);
        object.insert(QStringLiteral("part_type"), m_settings.partType);
        object.insert(QStringLiteral("model_threshold"), m_settings.modelThreshold);
        object.insert(QStringLiteral("review_threshold"), m_settings.reviewThreshold);
        object.insert(QStringLiteral("roi_size"), m_settings.roiSize);
        object.insert(QStringLiteral("segment_min_pixels"), m_settings.segmentMinPixels);
        object.insert(QStringLiteral("overlay_alpha"), m_settings.overlayAlpha);
        object.insert(QStringLiteral("auto_upload_enabled"), m_settings.autoUploadEnabled);
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
        next.segmentMinPixels = clampedInt(next.segmentMinPixels, 0, 50000);
        next.overlayAlpha = clampedDouble(next.overlayAlpha, 0.0, 1.0);
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
            && left.segmentMinPixels == right.segmentMinPixels
            && qFuzzyCompare(left.overlayAlpha + 1.0, right.overlayAlpha + 1.0)
            && left.autoUploadEnabled == right.autoUploadEnabled;
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
     *   classifyBad/segmentBad 保存两个模型各自是否发现缺陷，便于工作流文案说明冲突来源。
     */
    struct FusedDetectResult
    {
        QString cloudResult;
        QString historyText;
        QString uiStatus;
        QString reason;
        bool classifyBad = false;
        bool segmentBad = false;
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
          m_appendHistoryInSave(true),
          m_saveInProgress(false),
          m_detectInProgress(false),
          m_retryUploadInProgress(false)
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
                appendDetectHistoryRecord(*workerBundle);
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
            appendDetectHistoryRecord(bundle);
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
     *   2. 根据分类 RESULT 恢复云端 good/bad/review 判定；旧记录缺少模型结果时保守使用 review。
     *   3. 后台线程复用 defect-cos-upload 执行网络上传，避免阻塞 Qt 主线程。
     *   4. 上传结束后回到主线程原地更新同一条历史记录的 upload_status、record_id 和 record_no。
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
        const QString cloudResult = cloudResultFromFusedResult(fusedResult);

        /* workerResult 保存后台上传脚本返回的完整中文状态，线程结束后主线程读取并更新历史记录。 */
        const QSharedPointer<QString> workerResult(new QString(QStringLiteral("上传失败：后台重新发送线程没有返回结果")));

        setRetryUploadInProgress(true);

        QThread *workerThread = QThread::create([sourcePath,
                                                 annotatedPaths,
                                                 classificationResult,
                                                 cloudResult,
                                                 workerResult]() {
            CameraStorageController workerController;

            workerController.setAppendHistoryInSave(false);
            const QString retryPartCode =
                workerController.partCodeFromClassificationResult(classificationResult);
            const QString retryClassLabel =
                workerController.parseTokenValue(classificationResult, QStringLiteral("class"));

            *workerResult = workerController.uploadDetectImagesToCos(sourcePath,
                                                                     annotatedPaths,
                                                                     cloudResult,
                                                                     retryPartCode,
                                                                     retryClassLabel);
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
            } else if (workerResult->startsWith(QStringLiteral("上传成功："))) {
                resultText = QStringLiteral("重新发送成功：") + compactStatus;
            } else {
                const QString failureDetail = workerResult->startsWith(QStringLiteral("上传失败："))
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
     * segmentationHasDefect 的作用：
     *   判断 defect-segment 的 RESULT_SEG 行是否输出了非背景缺陷像素。
     *
     * 参数：
     *   segmentationResult 是 defect-segment 输出的一行 RESULT_SEG。
     *
     * 返回值：
     *   defect_pixels 大于 0 或 status=NG 时返回 true；否则返回 false。
     */
    bool segmentationHasDefect(const QString &segmentationResult,
                               const DetectSettingsSnapshot &settings) const
    {
        const QString status = parseTokenValue(segmentationResult, QStringLiteral("status"));
        const QString defectPixelsText = parseTokenValue(segmentationResult, QStringLiteral("defect_pixels"));
        bool ok = false;
        const int defectPixels = defectPixelsText.toInt(&ok);

        if (status == QStringLiteral("NG")) {
            return true;
        }

        return ok && defectPixels >= settings.segmentMinPixels && defectPixels > 0;
    }

    /*
     * fusedResultFromModelResults 的作用：
     *   综合分类模型和 UNet 分割模型的详细输出，生成唯一最终判定。
     *
     * 主要流程：
     *   1. 先读取分类 RESULT 的 status/confidence，只有 GOOD/BAD 属于可信输入。
     *   2. 再读取 UNet RESULT_SEG 的 status 和 defect_pixels，按 segmentMinPixels 判断缺陷是否有效。
     *   3. 任一模型结果缺失或分类置信度低于复核阈值时最终判为 review，避免低可信样本默认 good。
     *   4. 任一模型发现缺陷时最终判为 bad，禁止把 UNet 检出缺陷的样本放进良品流。
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
        const bool classifyKnown = classifyStatus == QStringLiteral("GOOD") || classifyStatus == QStringLiteral("BAD");
        const bool segmentKnown = segmentStatus == QStringLiteral("OK") || segmentStatus == QStringLiteral("NG");
        bool confidenceOk = false;
        const double confidence = confidenceText.toDouble(&confidenceOk);

        result.cloudResult = QStringLiteral("review");
        result.historyText = QStringLiteral("待复核");
        result.uiStatus = QStringLiteral("REVIEW");
        result.reason = QStringLiteral("模型结果待复核");
        result.classifyBad = classifyStatus == QStringLiteral("BAD");
        result.segmentBad = segmentationHasDefect(segmentationResult, settings);

        if (!classifyKnown || !segmentKnown) {
            result.reason = QStringLiteral("模型结果不完整");
            return result;
        }

        if (!confidenceOk || confidence < settings.reviewThreshold) {
            result.reason = QStringLiteral("分类置信度低于复核阈值");
            return result;
        }

        if (result.classifyBad && result.segmentBad) {
            result.cloudResult = QStringLiteral("bad");
            result.uiStatus = QStringLiteral("BAD");
            result.reason = QStringLiteral("分类和UNet均发现缺陷");
            return result;
        }

        if (result.classifyBad) {
            result.cloudResult = QStringLiteral("bad");
            result.uiStatus = QStringLiteral("BAD");
            result.reason = QStringLiteral("分类模型判定坏品");
            return result;
        }

        if (result.segmentBad) {
            result.cloudResult = QStringLiteral("bad");
            result.uiStatus = QStringLiteral("BAD");
            result.reason = QStringLiteral("UNet发现疑似缺陷");
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
     *   返回“良品”或“待复核”；当前坏品也先进入待复核，避免未接自动分拣前直接下最终人工结论。
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

        if (uploadResult.startsWith(QStringLiteral("上传成功："))) {
            QString status = QStringLiteral("上传成功");

            if (!recordId.isEmpty()) {
                status += QStringLiteral(" ID ") + recordId;
            }

            if (!recordNo.isEmpty()) {
                status += QStringLiteral(" ") + recordNo;
            }

            return status;
        }

        if (uploadResult.startsWith(QStringLiteral("上传失败："))) {
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
        entry.resultText = uploadResult.startsWith(QStringLiteral("上传成功："))
            ? QStringLiteral("良品")
            : QStringLiteral("待复核");
        entry.boardResultText = entry.resultText;
        entry.workflowText = uploadResult.startsWith(QStringLiteral("上传成功："))
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
    void appendDetectHistoryRecord(const DetectResultBundle &bundle)
    {
        if (m_historyModel == nullptr) {
            qWarning() << "detect history model missing, skip append";
            return;
        }

        UploadHistoryEntry entry;
        const QFileInfo sourceInfo(bundle.sourcePath);
        const FusedDetectResult fusedResult =
            fusedResultFromModelResults(bundle.classificationResult,
                                        bundle.segmentationResult,
                                        bundle.settings);

        entry.uploadTime = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        entry.resultText = historyTextFromFusedResult(fusedResult);
        entry.boardResultText = entry.resultText;
        entry.workflowText = QStringLiteral("综合%1；分类%2；UNet%3；%4")
            .arg(fusedResult.reason)
            .arg(fusedResult.classifyBad ? QStringLiteral("BAD") : QStringLiteral("GOOD"))
            .arg(fusedResult.segmentBad ? QStringLiteral("发现缺陷") : QStringLiteral("未见缺陷"))
            .arg(bundle.uploadResult.startsWith(QStringLiteral("上传成功："))
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

        m_historyModel->appendRecord(entry);
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
     *   6. 捕获 stdout/stderr，返回适合界面提示和历史记录的一行结果。
     *
     * 参数：
     *   sourcePath 是云端 file_kind=source 的原始检测图。
     *   annotatedPaths 是云端 file_kind=annotated 的所有模型结果图。
     *   cloudResult 是云端记录 result 字段，只允许 good、bad 或 review。
     *   partCode 是云端零件类型候选，例如 gasket；同一零件 good/bad 必须传同一个值。
     *   classLabel 是模型原始分类标签，例如 gasket_good；只用于 device_context 排障。
     *
     * 返回值：
     *   上传成功返回“上传成功：...”；失败返回“上传失败：...”。
     */
    QString uploadDetectImagesToCos(const QString &sourcePath,
                                    const QStringList &annotatedPaths,
                                    const QString &cloudResult = QStringLiteral("review"),
                                    const QString &partCode = QString(),
                                    const QString &classLabel = QString()) const
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
                << "classLabel" << classLabel;

        env.insert(QStringLiteral("CLOUD_RESULT"), normalizedCloudResult);
        if (!partCode.trimmed().isEmpty()) {
            env.insert(QStringLiteral("CLOUD_PART_CODE"), partCode.trimmed());
        }
        if (!classLabel.trimmed().isEmpty()) {
            env.insert(QStringLiteral("CLOUD_CLASS_LABEL"), classLabel.trimmed());
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
                usefulLine = firstUsefulLine(stderrText);
                return usefulLine.startsWith(QStringLiteral("上传失败："))
                    ? usefulLine
                    : QStringLiteral("上传失败：") + usefulLine;
            }
            if (!stdoutText.isEmpty()) {
                usefulLine = firstUsefulLine(stdoutText);
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
     *   在后台线程中完成“一次当前帧保存 + 分类模型推理 + UNet 分割推理 + 云端上传”。
     *
     * 主要流程：
     *   1. 确认 /mnt/sdcard 已挂载，并创建图片历史目录。
     *   2. 发送 SAVE_DETECT 命令，让 overlay 保存当前帧 JPG 作为 source。
     *   3. 调用 defect-classify，得到 MobileNetV3-Small GOOD/BAD 结果。
     *   4. 调用 defect-segment，得到 UNet raw/overlay/mask 结果图和缺陷像素。
     *   5. 把 source 和所有结果图上传到 COS，并把 bundle 交给主线程写历史记录。
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
        QString uploadResult;
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
                << "segmentMinPixels" << settings.segmentMinPixels
                << "overlayAlpha" << settings.overlayAlpha
                << "autoUpload" << settings.autoUploadEnabled;

        if (bundle == nullptr) {
            return QStringLiteral("检测失败：内部结果缓存为空");
        }

        if (!isMountPointMounted(mountPoint, &errorText)) {
            return QStringLiteral("检测失败：") + errorText;
        }

        if (!QDir().mkpath(imageDir)) {
            return QStringLiteral("检测失败：无法创建检测图片目录 ") + imageDir;
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
        if (settings.autoUploadEnabled) {
            uploadResult = uploadDetectImagesToCos(bundle->sourcePath,
                                                   bundle->annotatedPaths,
                                                   cloudResult,
                                                   partCodeFromClassificationResult(classificationResult),
                                                   parseTokenValue(classificationResult, QStringLiteral("class")));
        } else {
            uploadResult = QStringLiteral("manualUploadDisabled upload_status=SKIP 本地已保存，自动上传已关闭");
        }
        bundle->uploadResult = uploadResult;

        return modelResult
            + QStringLiteral(" upload_status=")
            + (settings.autoUploadEnabled
               ? (uploadResult.startsWith(QStringLiteral("上传成功：")) ? QStringLiteral("OK") : QStringLiteral("FAIL"))
               : QStringLiteral("SKIP"));
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
     *   2. 传入 --image/--model/--output-dir/--roi/--alpha/--min-defect-pixels，使用参数页真实配置。
     *   3. 等待进程结束，成功时解析 RESULT_SEG 中的 raw_path/overlay_path/mask_path。
     *   4. 把 raw/overlay/mask 三张图都放入 annotatedPaths，后续统一作为 --annotated 上传。
     *
     * 参数：
     *   segmentBin/modelPath/imagePath/outputDir 分别是推理程序、模型、输入图和输出目录。
     *   settings 保存本次检测使用的 ROI、overlay 透明度和 UNet 像素阈值。
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
                             << QStringLiteral("--min-defect-pixels") << QString::number(settings.segmentMinPixels));
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
    bool m_appendHistoryInSave; /* m_appendHistoryInSave 控制同步保存函数是否立即追加历史记录。 */
    bool m_saveInProgress; /* m_saveInProgress 只在 Qt 主线程维护，用于防止保存图片任务重复启动。 */
    bool m_detectInProgress; /* m_detectInProgress 只在 Qt 主线程维护，用于防止检测任务重复启动。 */
    bool m_retryUploadInProgress; /* m_retryUploadInProgress 只在 Qt 主线程维护，用于防止历史重发任务重复启动。 */
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
          m_f4Query(QString::fromLatin1(DEFAULT_F4_HEALTH_QUERY)),
          m_f4Baud(DEFAULT_F4_SERIAL_BAUD),
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
          m_networkProbeTimedOut(false),
          m_locationProbeTimedOut(false),
          m_locationBootProbeDone(false),
          m_cloudProbeTimedOut(false)
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
     *   供 QML 或人工操作立即触发一次 F4 STATUS 握手。
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
     *   从 QML 发送一条 F4 称重文本命令，例如称重标定 `CAL 1000`。
     *
     * 主要流程：
     *   1. 拒绝空命令和非 CAL 命令，避免参数页误变成任意运动控制串口终端。
     *   2. 自动补齐 `\r\n`，保证与 F407 文本命令解析入口一致。
     *   3. 在后台线程打开 `/dev/ttySTM2` 发送命令并短暂等待 F4 文本回复。
     *
     * 参数：
     *   commandText 是 QML 传入的命令正文，不要求自带行结束符。
     *
     * 返回值：
     *   true 表示后台发送任务已启动；false 表示参数非法或已有命令正在发送。
     */
    Q_INVOKABLE bool sendF4Command(const QString &commandText)
    {
        QString command = commandText.trimmed();

        if (command.isEmpty()) {
            emit f4CommandFinished(false, QStringLiteral("F4命令为空"));
            return false;
        }

        /*
         * 目前称重标定弹窗只开放 CAL 命令。
         * 运动类命令必须走更窄的 sendF4BeltCommand() 白名单，避免参数页绕开安全边界。
         */
        if (!command.startsWith(QStringLiteral("CAL "))) {
            emit f4CommandFinished(false, QStringLiteral("当前界面只允许发送 CAL <克重> 标定命令"));
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

        if (!command.endsWith(QStringLiteral("\r\n"))) {
            command += QStringLiteral("\r\n");
        }

        m_f4CommandRunning = true;

        QPointer<DeviceHealthController> self(this);
        const QString dev = m_f4Device;
        const int baud = m_f4Baud;

        QThread *workerThread = QThread::create([self, dev, command, baud]() {
            QString detail;
            const bool ok = sendF4SerialCommand(dev, command, baud, &detail);

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
     *   从 QML 手动控制页发送 F407 已经实现的传送带 ASCII 命令。
     *
     * 主要流程：
     *   1. 只允许 BELTSCAN、BELTSTOP、BELTINFO 三条已在 F407 `conveyor_motor_service.c` 中实现且有回包的命令。
     *   2. 自动补齐 `\r\n`，保证落到 F407 USART1 文本命令入口。
     *   3. 复用同一个串口忙标志，避免称重标定、传送带手动命令和 F4 心跳同时抢 `/dev/ttySTM2`。
     *
     * 参数：
     *   commandText 是 QML 传入的传送带命令正文，不要求自带行结束符。
     *
     * 返回值：
     *   true 表示后台发送任务已启动；false 表示命令不在白名单、串口忙或线程创建失败。
     */
    Q_INVOKABLE bool sendF4BeltCommand(const QString &commandText)
    {
        QString command = commandText.trimmed().toUpper();
        const QString commandLabel = command;

        if (command.isEmpty()) {
            emit f4ManualCommandFinished(false, commandLabel, QStringLiteral("F4传送带命令为空"));
            return false;
        }

        /*
         * 白名单必须和 F407 侧已有回包能力一致。
         * BELTTRACK/BELTENABLE/BELTCAM 当前可能不返回固定 OK 文本，不适合做手动页按钮的同步回执。
         */
        if (!isAllowedF4BeltCommand(command)) {
            emit f4ManualCommandFinished(false,
                                         commandLabel,
                                         QStringLiteral("当前界面只允许发送 BELTSCAN/BELTSTOP/BELTINFO"));
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

        if (!command.endsWith(QStringLiteral("\r\n"))) {
            command += QStringLiteral("\r\n");
        }

        m_f4CommandRunning = true;

        QPointer<DeviceHealthController> self(this);
        const QString dev = m_f4Device;
        const int baud = m_f4Baud;

        QThread *workerThread = QThread::create([self, dev, command, commandLabel, baud]() {
            QString detail;
            const bool ok = sendF4SerialCommand(dev, command, baud, &detail);

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
     * handleF4ProbeFinished 的作用：
     *   接收后台 F4 串口握手结果，并在 Qt 主线程更新 F4 接入状态。
     *
     * 参数：
     *   ok 为 true 表示收到 ACK/OK/F4/READY 之一。
     *   detail 保存失败原因或辅助说明。
     *
     * 返回值：
     *   无返回值。
     */
    void handleF4ProbeFinished(bool ok, const QString &detail)
    {
        m_f4ProbeRunning = false;
        if (ok) {
            setF4Status(QStringLiteral("接入"), QStringLiteral("#35d07f"));
            setDetailText(QStringLiteral("F4 串口握手成功"));
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
     *   ok 为 true 表示命令已写入串口且回复中包含成功关键字。
     *   detail 是串口回复文本或失败原因。
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
     *   ok 为 true 表示命令已写入串口且回复中包含成功关键字。
     *   command 是本次下发的高层文本命令，例如 BELTSCAN。
     *   detail 是串口回复文本或失败原因。
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

private:
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
        return command == QStringLiteral("BELTSCAN")
                || command == QStringLiteral("BELTSTOP")
                || command == QStringLiteral("BELTINFO");
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
     *   在后台线程里执行 F4 串口握手，避免串口等待阻塞 QML。
     *
     * 参数：
     *   forceNow 为 true 时立即发送 STATUS，适合人工点击刷新或标定前检查；
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
        const QString query = m_f4Query;
        const int baud = m_f4Baud;

        QThread *workerThread = QThread::create([self, dev, query, baud]() {
            QString detail;
            const bool ok = probeF4Serial(dev, query, baud, &detail);

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
        int fd = -1;
        struct sockaddr_un addr;
        QByteArray socketPathBytes = socketPath.toLocal8Bit();
        QByteArray commandBytes = QByteArrayLiteral("STATUS\n");
        char buffer[256];
        QByteArray reply;
        fd_set wfds;
        fd_set rfds;
        struct timeval tv;
        int optError = 0;
        socklen_t optLen = sizeof(optError);

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
            tv.tv_sec = 0;
            tv.tv_usec = 150000;
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

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 150000;
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            ::close(fd);
            return QStringLiteral("ERR overlay回复超时");
        }

        const ssize_t nread = ::read(fd, buffer, sizeof(buffer) - 1U);
        ::close(fd);
        if (nread <= 0) {
            return QStringLiteral("ERR 无回复");
        }

        buffer[nread] = '\0';
        reply = QByteArray(buffer, static_cast<int>(nread)).trimmed();
        return QString::fromLocal8Bit(reply);
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
            return B115200;
        }
    }

    /*
     * sendF4SerialCommand 的作用：
     *   打开 MP157 到 F4 的 RS485/USART 串口，发送一条文本命令并等待短回复。
     *
     * 参数：
     *   device 是 Linux TTY 节点，当前默认 `/dev/ttySTM2`。
     *   command 是要发送的完整命令，调用方应保证已经包含 `\r\n`。
     *   baud 是串口波特率，当前默认 115200。
     *   detail 用于返回 F4 回复内容或失败原因，可为 NULL。
     *
     * 返回值：
     *   回复中包含 ACK、OK、F4 或 READY 时返回 true；
     *   回复中包含 ERROR、打开失败、配置失败、写入失败或等待超时时返回 false。
     */
    static QString readF4ReplyText(int fd, QString *errorText)
    {
        QByteArray reply;                /* reply 保存本次串口命令收到的原始字节，最多保留 512 字节用于界面展示。 */
        char buffer[128];                /* buffer 是单次 read 的临时缓冲，避免一次性栈空间过大。 */
        QElapsedTimer elapsed;           /* elapsed 用于限制总等待时间，防止 F4 回包不带换行时线程长时间阻塞。 */

        elapsed.start();
        while (elapsed.elapsed() < 450 && reply.size() < 512) {
            fd_set rfds;                 /* rfds 是 select 监听集合，只等待当前串口 fd 可读。 */
            struct timeval tv;           /* tv 是每轮短等待时间，既能拼接多段回包，也不会卡住后台线程太久。 */

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
                    *errorText = QStringLiteral("读取 F4 回复失败");
                }
                return QString();
            }

            if (selected == 0) {
                if (!reply.isEmpty()) {
                    break;
                }
                continue;
            }

            const ssize_t nread = ::read(fd, buffer, sizeof(buffer));
            if (nread > 0) {
                reply.append(buffer, static_cast<int>(nread));
                if (reply.contains('\n')) {
                    break;
                }
                continue;
            }

            if (nread == 0) {
                break;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }

            if (errorText) {
                *errorText = QStringLiteral("读取 F4 回复失败");
            }
            return QString();
        }

        if (reply.isEmpty()) {
            if (errorText) {
                *errorText = QStringLiteral("未收到 F4 回复");
            }
            return QString();
        }

        return QString::fromLocal8Bit(reply).trimmed();
    }

    static bool sendF4SerialCommand(const QString &device,
                                    const QString &command,
                                    int baud,
                                    QString *detail)
    {
        const QByteArray devBytes = device.toLocal8Bit();
        const QByteArray commandBytes = command.toLocal8Bit();
        int fd = ::open(devBytes.constData(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        struct termios tio;
        QString replyText;
        QString readErrorText;
        QByteArray replyUpper;

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

        tcflush(fd, TCIOFLUSH);
        if (!writeAllToFd(fd, commandBytes)) {
            if (detail) {
                *detail = QStringLiteral("写入 F4 命令失败");
            }
            ::close(fd);
            return false;
        }
        tcdrain(fd);

        replyText = readF4ReplyText(fd, &readErrorText);
        ::close(fd);
        if (replyText.isEmpty()) {
            if (detail) {
                *detail = readErrorText.isEmpty() ? QStringLiteral("串口回复为空") : readErrorText;
            }
            return false;
        }

        replyUpper = replyText.toLocal8Bit().toUpper();
        if (replyUpper.contains("ERROR")) {
            if (detail) {
                *detail = QStringLiteral("F4返回错误：") + replyText;
            }
            return false;
        }

        if (replyUpper.contains("ACK")
                || replyUpper.contains("OK")
                || replyUpper.contains("F4")
                || replyUpper.contains("READY")
                || replyUpper.contains("[INFO][BELT]")
                || replyUpper.contains("[OK][BELT]")) {
            if (detail) {
                *detail = replyText;
            }
            return true;
        }

        if (detail) {
            *detail = QStringLiteral("回复不匹配：") + replyText;
        }
        return false;
    }

    /*
     * probeF4Serial 的作用：
     *   发送 STATUS 查询并复用通用串口命令等待逻辑，判断 F4 是否真实接入。
     *
     * 参数：
     *   device 是 Linux TTY 节点。
     *   query 是心跳查询命令，当前为 `STATUS\r\n`。
     *   baud 是串口波特率。
     *   detail 返回 F4 回复内容或失败原因。
     *
     * 返回值：
     *   收到成功关键字返回 true；否则返回 false。
     */
    static bool probeF4Serial(const QString &device, const QString &query, int baud, QString *detail)
    {
        return sendF4SerialCommand(device, query, baud, detail);
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
    QString m_f4Query;                  /* m_f4Query 保存发给 F4 的握手查询。 */
    int m_f4Baud;                       /* m_f4Baud 保存 F4 串口波特率。 */
    QElapsedTimer m_f4HeartbeatElapsed;  /* m_f4HeartbeatElapsed 记录上一次 STATUS 心跳发送时间，用于把周期心跳限制为 2 分钟一次。 */
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
    bool m_networkProbeTimedOut;        /* m_networkProbeTimedOut 标记当前 4G 进程已超时，finished 时不再覆盖超时状态。 */
    bool m_locationProbeTimedOut;       /* m_locationProbeTimedOut 标记开机定位进程已超时，finished 时不再覆盖超时状态。 */
    bool m_locationBootProbeDone;       /* m_locationBootProbeDone 标记本 Qt 进程已经调度过一次 IP 定位，后续周期刷新不再调用。 */
    bool m_cloudProbeTimedOut;          /* m_cloudProbeTimedOut 标记当前云端进程已超时，finished 时不再覆盖超时状态。 */
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
        + QStringLiteral("segment_min_pixels=120\n")
        + QStringLiteral("overlay_alpha=0.45\n")
        + QStringLiteral("auto_upload_enabled=true\n")
        + QStringLiteral("classify_args=--roi 352 --bad-threshold 0.650\n")
        + QStringLiteral("segment_args=--roi 352 --alpha 0.45 --min-defect-pixels 120\n");

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
