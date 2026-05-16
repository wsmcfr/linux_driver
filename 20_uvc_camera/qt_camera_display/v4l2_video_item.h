/*
 * v4l2_video_item.h
 *
 * 作用：
 *   定义一个可在 QML 中直接使用的 V4L2VideoItem。
 *   这个 Item 不使用 QtMultimedia 的 Camera/VideoOutput，
 *   而是直接从 /dev/videoX 读取 YUYV 帧，并在 Qt Quick 渲染线程中用 OpenGL ES 显示。
 *
 * 设计原因：
 *   当前 STM32MP157 板端的 QtMultimedia VideoOutput 会加载 Vivante 视频节点，
 *   该路径会调用 glTexDirectVIVMap，并在 galcore 的 _UserMemoryAttach/dma_map_sg 中触发内核 Oops。
 *   这里改为普通纹理上传，避免把用户态 UVC 缓冲区交给 galcore 做零拷贝包装。
 */

#ifndef V4L2_VIDEO_ITEM_H
#define V4L2_VIDEO_ITEM_H

#include <QByteArray>                 /* QByteArray 保存一帧 YUYV 原始数据，跨线程传递时自动深拷贝。 */
#include <QMutex>                     /* QMutex 保护 GUI 线程和渲染线程之间共享的最新帧。 */
#include <QObject>                    /* QObject 提供信号槽和属性系统。 */
#include <QQuickFramebufferObject>    /* QQuickFramebufferObject 让 QML Item 使用 OpenGL ES 渲染。 */
#include <QSize>                      /* QSize 表示帧尺寸和 FBO 尺寸。 */
#include <QString>                    /* QString 保存设备节点和状态文本。 */
#include <QThread>                    /* QThread 用于后台阻塞读取 V4L2 帧，避免卡住 UI 线程。 */

#include <atomic>                     /* std::atomic_bool 用于安全请求采集线程退出。 */

/*
 * V4L2CaptureThread 的作用：
 *   后台采集线程，负责打开 UVC 设备、配置 YUYV 格式、mmap 缓冲区并持续取帧。
 *
 * 主要流程：
 *   1. open(/dev/videoX) 打开摄像头节点。
 *   2. VIDIOC_S_FMT 请求 YUYV 分辨率。
 *   3. VIDIOC_REQBUFS/QUERYBUF/mmap 建立 V4L2 mmap 缓冲区。
 *   4. VIDIOC_STREAMON 启动视频流。
 *   5. select 等待帧就绪，DQBUF 取帧，复制到 QByteArray 后 QBUF 归还。
 *   6. stopCapture 请求退出时 STREAMOFF、munmap、close，释放所有资源。
 */
class V4L2CaptureThread : public QThread
{
    Q_OBJECT

public:
    /*
     * Buffer 的作用：
     *   保存 V4L2 mmap 缓冲区的用户态映射地址和长度。
     */
    struct Buffer {
        void *start;                  /* start 是 mmap 返回的缓冲区起始地址，由内核填充视频数据。 */
        size_t length;                /* length 是该缓冲区长度，munmap 时必须原样传回。 */
    };

    explicit V4L2CaptureThread(QObject *parent = nullptr);
    ~V4L2CaptureThread() override;

    void configure(const QString &device, int width, int height, int fps);
    void stopCapture();

signals:
    void frameReady(const QByteArray &frameData, int width, int height, quint64 serial);
    void statusChanged(const QString &statusText, bool active);

protected:
    void run() override;

private:
    bool openDevice(int *fd);
    bool configureDevice(int fd, int *actualWidth, int *actualHeight);
    bool setupBuffers(int fd, Buffer *buffers, int *bufferCount);
    bool startStream(int fd, Buffer *buffers, int bufferCount);
    void captureLoop(int fd, Buffer *buffers, int bufferCount, int width, int height);
    void stopStreamAndCleanup(int fd, Buffer *buffers, int bufferCount);
    void emitStatus(const QString &text, bool active);

    QString m_device;                 /* m_device 保存摄像头节点，例如 /dev/video0。 */
    int m_requestedWidth;             /* m_requestedWidth 是期望采集宽度，默认由 QML 设置为 640。 */
    int m_requestedHeight;            /* m_requestedHeight 是期望采集高度，默认由 QML 设置为 480。 */
    int m_requestedFps;               /* m_requestedFps 是期望帧率，降低帧率可降低总线和 CPU 压力。 */
    std::atomic_bool m_stopRequested; /* m_stopRequested 为 true 时采集循环尽快退出并释放设备。 */
};

/*
 * V4L2VideoItem 的作用：
 *   暴露给 QML 的视频显示 Item。
 *   它持有一个 V4L2CaptureThread，并把最新 YUYV 帧交给 Renderer 在 GPU 中绘制。
 */
class V4L2VideoItem : public QQuickFramebufferObject
{
    Q_OBJECT
    Q_PROPERTY(QString device READ device WRITE setDevice NOTIFY deviceChanged)
    Q_PROPERTY(int captureWidth READ captureWidth WRITE setCaptureWidth NOTIFY captureSizeChanged)
    Q_PROPERTY(int captureHeight READ captureHeight WRITE setCaptureHeight NOTIFY captureSizeChanged)
    Q_PROPERTY(int fps READ fps WRITE setFps NOTIFY fpsChanged)
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)

public:
    explicit V4L2VideoItem(QQuickItem *parent = nullptr);
    ~V4L2VideoItem() override;

    Renderer *createRenderer() const override;

    QString device() const;
    void setDevice(const QString &device);

    int captureWidth() const;
    void setCaptureWidth(int width);

    int captureHeight() const;
    void setCaptureHeight(int height);

    int fps() const;
    void setFps(int fps);

    bool running() const;
    void setRunning(bool running);

    bool active() const;
    QString statusText() const;

    bool copyLatestFrame(QByteArray *frameData, int *width, int *height, quint64 *serial) const;

signals:
    void deviceChanged();
    void captureSizeChanged();
    void fpsChanged();
    void runningChanged();
    void activeChanged();
    void statusTextChanged();

protected:
    void componentComplete() override;

private slots:
    void handleFrameReady(const QByteArray &frameData, int width, int height, quint64 serial);
    void handleStatusChanged(const QString &statusText, bool active);

private:
    void restartCaptureIfNeeded();
    void startCapture();
    void stopCapture();

    QString m_device;                 /* m_device 是 QML 传入的摄像头节点。 */
    int m_captureWidth;               /* m_captureWidth 是 V4L2 请求采集宽度。 */
    int m_captureHeight;              /* m_captureHeight 是 V4L2 请求采集高度。 */
    int m_fps;                        /* m_fps 是 V4L2 请求采集帧率。 */
    bool m_running;                   /* m_running 表示 QML 是否希望采集线程保持运行。 */
    bool m_active;                    /* m_active 表示最近是否已经成功收到有效视频帧。 */
    bool m_componentReady;            /* m_componentReady 防止 QML 属性尚未初始化就打开摄像头。 */
    QString m_statusText;             /* m_statusText 保存界面可显示的采集状态或错误原因。 */
    V4L2CaptureThread *m_thread;      /* m_thread 是后台 V4L2 采集线程，归本对象管理。 */

    mutable QMutex m_frameMutex;      /* m_frameMutex 保护 m_frameData 等最新帧字段。 */
    QByteArray m_frameData;           /* m_frameData 保存最近一帧 YUYV 数据。 */
    int m_frameWidth;                 /* m_frameWidth 是最近一帧实际宽度。 */
    int m_frameHeight;                /* m_frameHeight 是最近一帧实际高度。 */
    quint64 m_frameSerial;            /* m_frameSerial 每收到一帧递增，用于渲染线程判断是否需要上传纹理。 */
};

#endif /* V4L2_VIDEO_ITEM_H */
