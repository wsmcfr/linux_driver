/*
 * v4l2_video_item.cpp
 *
 * 作用：
 *   实现 V4L2VideoItem 和 V4L2CaptureThread。
 *   采集端使用 Linux V4L2 mmap 接口读取 UVC 摄像头 YUYV 帧；
 *   显示端使用 Qt Quick FBO 和 OpenGL ES shader 把 YUYV 转成 RGB 画到界面中。
 */

#include "v4l2_video_item.h"          /* 本文件实现 V4L2VideoItem 和 V4L2CaptureThread。 */

#include <QDebug>                     /* QDebug 用于输出板端诊断日志。 */
#include <QMutexLocker>               /* QMutexLocker 用 RAII 方式自动释放互斥锁。 */
#include <QOpenGLContext>             /* QOpenGLContext 用于取得当前渲染线程 OpenGL 函数表。 */
#include <QOpenGLFramebufferObject>   /* QOpenGLFramebufferObject 是 QQuickFramebufferObject 的渲染目标。 */
#include <QOpenGLFramebufferObjectFormat> /* QOpenGLFramebufferObjectFormat 设置 FBO 附件格式。 */
#include <QOpenGLFunctions>           /* QOpenGLFunctions 提供跨平台 OpenGL ES 函数入口。 */
#include <QOpenGLShaderProgram>       /* QOpenGLShaderProgram 编译和链接 YUYV 转 RGB shader。 */
#include <QSize>                      /* QSize 表示 FBO 大小。 */

#include <algorithm>                  /* std::max/std::min 用于限制宽高和帧率范围。 */
#include <cerrno>                     /* errno 保存 Linux 系统调用失败原因。 */
#include <cstring>                    /* strerror/memset 用于错误文本和结构体清零。 */

#include <fcntl.h>                    /* open 使用 O_RDWR/O_NONBLOCK 打开 V4L2 设备。 */
#include <linux/videodev2.h>          /* videodev2.h 提供 V4L2 ioctl 结构体和常量。 */
#include <sys/ioctl.h>                /* ioctl 调用 V4L2 控制命令。 */
#include <sys/mman.h>                 /* mmap/munmap 映射和释放 V4L2 缓冲区。 */
#include <sys/select.h>               /* select 等待摄像头帧就绪。 */
#include <unistd.h>                   /* close 关闭设备文件。 */

/*
 * xioctl 的作用：
 *   封装 ioctl，并在被信号中断时自动重试。
 *
 * 参数：
 *   fd 是已经打开的 V4L2 设备文件描述符。
 *   request 是 V4L2 ioctl 命令。
 *   arg 是对应命令的参数结构体指针。
 *
 * 返回值：
 *   成功返回 ioctl 的返回值；失败返回 -1，errno 保存失败原因。
 */
static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);

    return ret;
}

/*
 * fourccToString 的作用：
 *   把 V4L2 pixelformat 四字符码转为可读字符串，便于日志定位格式协商问题。
 *
 * 参数：
 *   format 是 V4L2_PIX_FMT_* 这类四字符码。
 *
 * 返回值：
 *   返回长度为 4 的字符串，例如 "YUYV" 或 "MJPG"。
 */
static QString fourccToString(unsigned int format)
{
    char text[5];
    text[0] = static_cast<char>(format & 0xff);
    text[1] = static_cast<char>((format >> 8) & 0xff);
    text[2] = static_cast<char>((format >> 16) & 0xff);
    text[3] = static_cast<char>((format >> 24) & 0xff);
    text[4] = '\0';
    return QString::fromLatin1(text);
}

/*
 * V4L2CaptureThread 构造函数：
 *   设置默认采集参数，实际参数会由 V4L2VideoItem::startCapture 传入。
 */
V4L2CaptureThread::V4L2CaptureThread(QObject *parent)
    : QThread(parent),
      m_device(QStringLiteral("/dev/video0")),
      m_requestedWidth(640),
      m_requestedHeight(480),
      m_requestedFps(15),
      m_stopRequested(false)
{
}

/*
 * V4L2CaptureThread 析构函数：
 *   请求采集线程退出并等待结束，避免对象销毁后线程仍访问成员变量。
 */
V4L2CaptureThread::~V4L2CaptureThread()
{
    stopCapture();
    wait(1500);
}

/*
 * configure 的作用：
 *   在启动线程前写入摄像头节点、分辨率和帧率。
 */
void V4L2CaptureThread::configure(const QString &device, int width, int height, int fps)
{
    m_device = device;
    m_requestedWidth = width;
    m_requestedHeight = height;
    m_requestedFps = fps;
}

/*
 * stopCapture 的作用：
 *   设置退出标志，让 captureLoop 在 select 超时或下一帧到达后安全退出。
 */
void V4L2CaptureThread::stopCapture()
{
    m_stopRequested.store(true);
}

/*
 * emitStatus 的作用：
 *   统一发送状态文本和 active 标志，QML 侧用它显示“在线/不可用/错误原因”。
 */
void V4L2CaptureThread::emitStatus(const QString &text, bool active)
{
    emit statusChanged(text, active);
}

/*
 * openDevice 的作用：
 *   打开 V4L2 设备节点，并确认它具备视频采集能力。
 */
bool V4L2CaptureThread::openDevice(int *fd)
{
    struct v4l2_capability capability;

    *fd = open(m_device.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
    if (*fd < 0) {
        emitStatus(QStringLiteral("打开失败: %1").arg(QString::fromLocal8Bit(strerror(errno))), false);
        return false;
    }

    std::memset(&capability, 0, sizeof(capability));
    if (xioctl(*fd, VIDIOC_QUERYCAP, &capability) < 0) {
        emitStatus(QStringLiteral("QUERYCAP失败: %1").arg(QString::fromLocal8Bit(strerror(errno))), false);
        close(*fd);
        *fd = -1;
        return false;
    }

    if ((capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        emitStatus(QStringLiteral("不是采集设备"), false);
        close(*fd);
        *fd = -1;
        return false;
    }

    if ((capability.capabilities & V4L2_CAP_STREAMING) == 0) {
        emitStatus(QStringLiteral("不支持mmap流采集"), false);
        close(*fd);
        *fd = -1;
        return false;
    }

    return true;
}

/*
 * configureDevice 的作用：
 *   请求摄像头输出 YUYV 格式，并设置目标帧率。
 */
bool V4L2CaptureThread::configureDevice(int fd, int *actualWidth, int *actualHeight)
{
    struct v4l2_format format;
    struct v4l2_streamparm streamParm;

    std::memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = static_cast<unsigned int>(m_requestedWidth);
    format.fmt.pix.height = static_cast<unsigned int>(m_requestedHeight);
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    format.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(fd, VIDIOC_S_FMT, &format) < 0) {
        emitStatus(QStringLiteral("设置YUYV失败: %1").arg(QString::fromLocal8Bit(strerror(errno))), false);
        return false;
    }

    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        emitStatus(QStringLiteral("摄像头未接受YUYV，实际为%1").arg(fourccToString(format.fmt.pix.pixelformat)), false);
        return false;
    }

    *actualWidth = static_cast<int>(format.fmt.pix.width);
    *actualHeight = static_cast<int>(format.fmt.pix.height);

    std::memset(&streamParm, 0, sizeof(streamParm));
    streamParm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    streamParm.parm.capture.timeperframe.numerator = 1;
    streamParm.parm.capture.timeperframe.denominator = static_cast<unsigned int>(std::max(1, m_requestedFps));
    xioctl(fd, VIDIOC_S_PARM, &streamParm);

    emitStatus(QStringLiteral("采集中 %1x%2 YUYV").arg(*actualWidth).arg(*actualHeight), true);
    return true;
}

/*
 * setupBuffers 的作用：
 *   向 V4L2 驱动申请 mmap 缓冲区，并映射到用户态。
 */
bool V4L2CaptureThread::setupBuffers(int fd, Buffer *buffers, int *bufferCount)
{
    struct v4l2_requestbuffers requestBuffers;

    std::memset(&requestBuffers, 0, sizeof(requestBuffers));
    requestBuffers.count = 4;
    requestBuffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    requestBuffers.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_REQBUFS, &requestBuffers) < 0) {
        emitStatus(QStringLiteral("REQBUFS失败: %1").arg(QString::fromLocal8Bit(strerror(errno))), false);
        return false;
    }

    if (requestBuffers.count < 2) {
        emitStatus(QStringLiteral("V4L2缓冲区不足"), false);
        return false;
    }

    *bufferCount = static_cast<int>(std::min(requestBuffers.count, 4U));

    for (int index = 0; index < *bufferCount; ++index) {
        struct v4l2_buffer buffer;

        std::memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = static_cast<unsigned int>(index);

        if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            emitStatus(QStringLiteral("QUERYBUF失败: %1").arg(QString::fromLocal8Bit(strerror(errno))), false);
            return false;
        }

        buffers[index].length = buffer.length;
        buffers[index].start = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buffer.m.offset);

        if (buffers[index].start == MAP_FAILED) {
            buffers[index].start = nullptr;
            buffers[index].length = 0;
            emitStatus(QStringLiteral("mmap失败: %1").arg(QString::fromLocal8Bit(strerror(errno))), false);
            return false;
        }
    }

    return true;
}

/*
 * startStream 的作用：
 *   把所有 mmap 缓冲区放入驱动队列，然后启动视频流。
 */
bool V4L2CaptureThread::startStream(int fd, Buffer *buffers, int bufferCount)
{
    Q_UNUSED(buffers);

    for (int index = 0; index < bufferCount; ++index) {
        struct v4l2_buffer buffer;

        std::memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = static_cast<unsigned int>(index);

        if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
            emitStatus(QStringLiteral("QBUF失败: %1").arg(QString::fromLocal8Bit(strerror(errno))), false);
            return false;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        emitStatus(QStringLiteral("STREAMON失败: %1").arg(QString::fromLocal8Bit(strerror(errno))), false);
        return false;
    }

    return true;
}

/*
 * captureLoop 的作用：
 *   循环等待、取出、复制并归还摄像头帧。
 */
void V4L2CaptureThread::captureLoop(int fd, Buffer *buffers, int bufferCount, int width, int height)
{
    quint64 serial = 0;

    while (!m_stopRequested.load()) {
        fd_set readSet;
        struct timeval timeout;

        FD_ZERO(&readSet);
        FD_SET(fd, &readSet);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        const int ready = select(fd + 1, &readSet, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            emitStatus(QStringLiteral("select失败: %1").arg(QString::fromLocal8Bit(strerror(errno))), false);
            break;
        }

        if (ready == 0) {
            emitStatus(QStringLiteral("等待视频帧"), false);
            continue;
        }

        struct v4l2_buffer buffer;
        std::memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;

        if (xioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) {
                continue;
            }
            emitStatus(QStringLiteral("DQBUF失败: %1").arg(QString::fromLocal8Bit(strerror(errno))), false);
            break;
        }

        if (buffer.index < static_cast<unsigned int>(bufferCount) && buffers[buffer.index].start != nullptr) {
            const char *data = static_cast<const char *>(buffers[buffer.index].start);
            const int expectedBytes = width * height * 2;
            const int validBytes = static_cast<int>(std::min<unsigned int>(buffer.bytesused, static_cast<unsigned int>(expectedBytes)));
            emit frameReady(QByteArray(data, validBytes), width, height, ++serial);
        }

        if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
            emitStatus(QStringLiteral("重新QBUF失败: %1").arg(QString::fromLocal8Bit(strerror(errno))), false);
            break;
        }
    }
}

/*
 * stopStreamAndCleanup 的作用：
 *   停止视频流，解除 mmap 映射并关闭设备文件。
 */
void V4L2CaptureThread::stopStreamAndCleanup(int fd, Buffer *buffers, int bufferCount)
{
    if (fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd, VIDIOC_STREAMOFF, &type);
    }

    for (int index = 0; index < bufferCount; ++index) {
        if (buffers[index].start != nullptr && buffers[index].length > 0) {
            munmap(buffers[index].start, buffers[index].length);
            buffers[index].start = nullptr;
            buffers[index].length = 0;
        }
    }

    if (fd >= 0) {
        close(fd);
    }
}

/*
 * run 的作用：
 *   采集线程主函数，串联打开设备、配置格式、启动流和采集循环。
 */
void V4L2CaptureThread::run()
{
    int fd = -1;
    int actualWidth = 0;
    int actualHeight = 0;
    int bufferCount = 0;
    Buffer buffers[4];

    for (int index = 0; index < 4; ++index) {
        buffers[index].start = nullptr;
        buffers[index].length = 0;
    }

    m_stopRequested.store(false);
    emitStatus(QStringLiteral("打开%1").arg(m_device), false);

    if (openDevice(&fd)
        && configureDevice(fd, &actualWidth, &actualHeight)
        && setupBuffers(fd, buffers, &bufferCount)
        && startStream(fd, buffers, bufferCount)) {
        captureLoop(fd, buffers, bufferCount, actualWidth, actualHeight);
    }

    stopStreamAndCleanup(fd, buffers, bufferCount);
    emitStatus(QStringLiteral("已停止"), false);
}

/*
 * V4L2VideoItem 构造函数：
 *   初始化 QML 属性默认值，并设置 Item 有内容可渲染。
 */
V4L2VideoItem::V4L2VideoItem(QQuickItem *parent)
    : QQuickFramebufferObject(parent),
      m_device(QStringLiteral("/dev/video0")),
      m_captureWidth(640),
      m_captureHeight(480),
      m_fps(15),
      m_running(true),
      m_active(false),
      m_componentReady(false),
      m_statusText(QStringLiteral("等待启动")),
      m_thread(nullptr),
      m_frameWidth(0),
      m_frameHeight(0),
      m_frameSerial(0)
{
    setMirrorVertically(false);
}

/*
 * V4L2VideoItem 析构函数：
 *   停止采集线程，避免程序退出时摄像头设备仍被占用。
 */
V4L2VideoItem::~V4L2VideoItem()
{
    stopCapture();
}

QString V4L2VideoItem::device() const
{
    return m_device;
}

/*
 * setDevice 的作用：
 *   更新摄像头设备节点，并在运行中自动重启采集线程。
 */
void V4L2VideoItem::setDevice(const QString &device)
{
    if (m_device == device) {
        return;
    }

    m_device = device;
    emit deviceChanged();
    restartCaptureIfNeeded();
}

int V4L2VideoItem::captureWidth() const
{
    return m_captureWidth;
}

void V4L2VideoItem::setCaptureWidth(int width)
{
    const int clampedWidth = std::max(160, width);
    if (m_captureWidth == clampedWidth) {
        return;
    }

    m_captureWidth = clampedWidth;
    emit captureSizeChanged();
    restartCaptureIfNeeded();
}

int V4L2VideoItem::captureHeight() const
{
    return m_captureHeight;
}

void V4L2VideoItem::setCaptureHeight(int height)
{
    const int clampedHeight = std::max(120, height);
    if (m_captureHeight == clampedHeight) {
        return;
    }

    m_captureHeight = clampedHeight;
    emit captureSizeChanged();
    restartCaptureIfNeeded();
}

int V4L2VideoItem::fps() const
{
    return m_fps;
}

void V4L2VideoItem::setFps(int fps)
{
    const int clampedFps = std::max(1, std::min(30, fps));
    if (m_fps == clampedFps) {
        return;
    }

    m_fps = clampedFps;
    emit fpsChanged();
    restartCaptureIfNeeded();
}

bool V4L2VideoItem::running() const
{
    return m_running;
}

/*
 * setRunning 的作用：
 *   响应 QML 开始/暂停按钮，启动或停止后台 V4L2 采集。
 */
void V4L2VideoItem::setRunning(bool running)
{
    if (m_running == running) {
        return;
    }

    m_running = running;
    emit runningChanged();

    if (m_running) {
        startCapture();
    } else {
        stopCapture();
    }
}

bool V4L2VideoItem::active() const
{
    return m_active;
}

QString V4L2VideoItem::statusText() const
{
    return m_statusText;
}

/*
 * componentComplete 的作用：
 *   等 QML 属性初始化完成后再启动摄像头，避免 device/width/height 还是默认中间值。
 */
void V4L2VideoItem::componentComplete()
{
    QQuickFramebufferObject::componentComplete();
    m_componentReady = true;

    if (m_running) {
        startCapture();
    }
}

/*
 * handleFrameReady 的作用：
 *   接收采集线程发来的新帧，保存为最新帧并请求 Qt Quick 重绘。
 */
void V4L2VideoItem::handleFrameReady(const QByteArray &frameData, int width, int height, quint64 serial)
{
    {
        QMutexLocker locker(&m_frameMutex);
        m_frameData = frameData;
        m_frameWidth = width;
        m_frameHeight = height;
        m_frameSerial = serial;
    }

    if (!m_active) {
        m_active = true;
        emit activeChanged();
    }

    update();
}

/*
 * handleStatusChanged 的作用：
 *   接收采集线程状态，更新 QML 可见文本和 active 标志。
 */
void V4L2VideoItem::handleStatusChanged(const QString &statusText, bool active)
{
    if (m_statusText != statusText) {
        m_statusText = statusText;
        emit statusTextChanged();
    }

    if (m_active != active) {
        m_active = active;
        emit activeChanged();
    }
}

/*
 * copyLatestFrame 的作用：
 *   供渲染线程在 synchronize 阶段复制最新帧。
 */
bool V4L2VideoItem::copyLatestFrame(QByteArray *frameData, int *width, int *height, quint64 *serial) const
{
    QMutexLocker locker(&m_frameMutex);

    if (m_frameData.isEmpty() || m_frameWidth <= 0 || m_frameHeight <= 0) {
        return false;
    }

    *frameData = m_frameData;
    *width = m_frameWidth;
    *height = m_frameHeight;
    *serial = m_frameSerial;
    return true;
}

/*
 * restartCaptureIfNeeded 的作用：
 *   当设备节点、分辨率或帧率变化时，在运行状态下重启采集线程。
 */
void V4L2VideoItem::restartCaptureIfNeeded()
{
    if (!m_componentReady || !m_running) {
        return;
    }

    stopCapture();
    startCapture();
}

/*
 * startCapture 的作用：
 *   创建并启动后台 V4L2 采集线程。
 */
void V4L2VideoItem::startCapture()
{
    if (!m_componentReady || m_thread != nullptr) {
        return;
    }

    m_thread = new V4L2CaptureThread(this);
    m_thread->configure(m_device, m_captureWidth, m_captureHeight, m_fps);

    connect(m_thread, &V4L2CaptureThread::frameReady,
            this, &V4L2VideoItem::handleFrameReady, Qt::QueuedConnection);
    connect(m_thread, &V4L2CaptureThread::statusChanged,
            this, &V4L2VideoItem::handleStatusChanged, Qt::QueuedConnection);
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, this, [this]() {
        if (m_thread != nullptr && m_thread->isFinished()) {
            m_thread = nullptr;
        }
    });

    m_thread->start();
}

/*
 * stopCapture 的作用：
 *   请求采集线程退出，并等待资源释放完成。
 */
void V4L2VideoItem::stopCapture()
{
    V4L2CaptureThread *thread = m_thread;
    m_thread = nullptr;

    if (thread != nullptr) {
        thread->stopCapture();
        thread->wait(1500);
    }

    if (m_active) {
        m_active = false;
        emit activeChanged();
    }
}

/*
 * YuyvRenderer 的作用：
 *   QQuickFramebufferObject 的渲染器，在 Qt Quick 渲染线程中执行 OpenGL ES 绘制。
 */
class YuyvRenderer : public QQuickFramebufferObject::Renderer
{
public:
    YuyvRenderer();
    ~YuyvRenderer() override;

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override;
    void synchronize(QQuickFramebufferObject *item) override;
    void render() override;

private:
    bool ensureProgram(QOpenGLFunctions *functions);
    void ensureTexture(QOpenGLFunctions *functions);
    void uploadFrameIfNeeded(QOpenGLFunctions *functions);

    QOpenGLShaderProgram *m_program;   /* m_program 保存 YUYV 转 RGB 的 OpenGL ES shader 程序。 */
    unsigned int m_textureId;          /* m_textureId 是保存 YUYV 数据的 GL_RGBA 纹理。 */
    QByteArray m_pendingFrame;         /* m_pendingFrame 是渲染线程待上传的新帧。 */
    int m_frameWidth;                  /* m_frameWidth 是待显示帧宽度。 */
    int m_frameHeight;                 /* m_frameHeight 是待显示帧高度。 */
    quint64 m_seenSerial;              /* m_seenSerial 是已经同步到渲染器的最新帧序号。 */
    quint64 m_uploadedSerial;          /* m_uploadedSerial 是已经上传到 GPU 纹理的帧序号。 */
};

YuyvRenderer::YuyvRenderer()
    : m_program(nullptr),
      m_textureId(0),
      m_frameWidth(0),
      m_frameHeight(0),
      m_seenSerial(0),
      m_uploadedSerial(0)
{
}

YuyvRenderer::~YuyvRenderer()
{
    QOpenGLFunctions *functions = QOpenGLContext::currentContext() != nullptr
        ? QOpenGLContext::currentContext()->functions()
        : nullptr;

    if (functions != nullptr && m_textureId != 0) {
        functions->glDeleteTextures(1, &m_textureId);
        m_textureId = 0;
    }

    delete m_program;
    m_program = nullptr;
}

/*
 * createFramebufferObject 的作用：
 *   为当前 Item 创建 FBO，Qt Quick 会把这个 FBO 合成到主场景里。
 */
QOpenGLFramebufferObject *YuyvRenderer::createFramebufferObject(const QSize &size)
{
    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    return new QOpenGLFramebufferObject(size, format);
}

/*
 * synchronize 的作用：
 *   在 GUI 线程和渲染线程同步点复制最新帧，避免渲染线程直接访问 GUI 对象内部数据。
 */
void YuyvRenderer::synchronize(QQuickFramebufferObject *item)
{
    V4L2VideoItem *videoItem = static_cast<V4L2VideoItem *>(item);
    QByteArray frameData;
    int width = 0;
    int height = 0;
    quint64 serial = 0;

    if (videoItem->copyLatestFrame(&frameData, &width, &height, &serial) && serial != m_seenSerial) {
        m_pendingFrame = frameData;
        m_frameWidth = width;
        m_frameHeight = height;
        m_seenSerial = serial;
    }
}

/*
 * ensureProgram 的作用：
 *   懒加载并编译 YUYV 转 RGB shader。
 */
bool YuyvRenderer::ensureProgram(QOpenGLFunctions *functions)
{
    Q_UNUSED(functions);

    if (m_program != nullptr) {
        return true;
    }

    static const char *vertexShader =
        "attribute highp vec4 vertexPosition;\n"
        "attribute highp vec2 vertexTexCoord;\n"
        "varying highp vec2 vTexCoord;\n"
        "void main() {\n"
        "    gl_Position = vertexPosition;\n"
        "    vTexCoord = vertexTexCoord;\n"
        "}\n";

    static const char *fragmentShader =
        "precision mediump float;\n"
        "uniform sampler2D yuyvTexture;\n"
        "uniform highp float frameWidth;\n"
        "varying highp vec2 vTexCoord;\n"
        "void main() {\n"
        "    highp float pixelX = floor(vTexCoord.x * frameWidth);\n"
        "    highp float pairX = floor(pixelX * 0.5);\n"
        "    highp float textureWidth = frameWidth * 0.5;\n"
        "    lowp vec4 yuyv = texture2D(yuyvTexture, vec2((pairX + 0.5) / textureWidth, vTexCoord.y));\n"
        "    lowp float y = (mod(pixelX, 2.0) < 0.5) ? yuyv.r : yuyv.b;\n"
        "    mediump float u = yuyv.g - 0.5;\n"
        "    mediump float v = yuyv.a - 0.5;\n"
        "    mediump float r = y + 1.402 * v;\n"
        "    mediump float g = y - 0.344136 * u - 0.714136 * v;\n"
        "    mediump float b = y + 1.772 * u;\n"
        "    gl_FragColor = vec4(r, g, b, 1.0);\n"
        "}\n";

    m_program = new QOpenGLShaderProgram();
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader)
        || !m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader)
        || !m_program->link()) {
        qWarning() << "V4L2VideoItem shader 编译失败:" << m_program->log();
        delete m_program;
        m_program = nullptr;
        return false;
    }

    return true;
}

/*
 * ensureTexture 的作用：
 *   懒创建 OpenGL 纹理，并设置为最近邻采样，避免 YUYV 字节被线性插值破坏。
 */
void YuyvRenderer::ensureTexture(QOpenGLFunctions *functions)
{
    if (m_textureId != 0) {
        return;
    }

    functions->glGenTextures(1, &m_textureId);
    functions->glBindTexture(GL_TEXTURE_2D, m_textureId);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/*
 * uploadFrameIfNeeded 的作用：
 *   当采集线程送来新帧时，把 YUYV 数据作为 GL_RGBA 纹理上传到 GPU。
 */
void YuyvRenderer::uploadFrameIfNeeded(QOpenGLFunctions *functions)
{
    if (m_pendingFrame.isEmpty() || m_frameWidth <= 0 || m_frameHeight <= 0 || m_seenSerial == m_uploadedSerial) {
        return;
    }

    ensureTexture(functions);

    const int expectedBytes = m_frameWidth * m_frameHeight * 2;
    if (m_pendingFrame.size() < expectedBytes) {
        return;
    }

    functions->glBindTexture(GL_TEXTURE_2D, m_textureId);
    functions->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    functions->glTexImage2D(GL_TEXTURE_2D,
                            0,
                            GL_RGBA,
                            m_frameWidth / 2,
                            m_frameHeight,
                            0,
                            GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            m_pendingFrame.constData());
    m_uploadedSerial = m_seenSerial;
}

/*
 * render 的作用：
 *   清屏、上传新帧、绘制全屏四边形。
 */
void YuyvRenderer::render()
{
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        return;
    }

    QOpenGLFunctions *functions = context->functions();
    functions->glViewport(0, 0, framebufferObject()->width(), framebufferObject()->height());
    functions->glDisable(GL_DEPTH_TEST);
    functions->glDisable(GL_BLEND);
    functions->glClearColor(0.02f, 0.025f, 0.025f, 1.0f);
    functions->glClear(GL_COLOR_BUFFER_BIT);

    if (!ensureProgram(functions)) {
        return;
    }

    uploadFrameIfNeeded(functions);
    if (m_textureId == 0 || m_frameWidth <= 0) {
        return;
    }

    static const float vertices[] = {
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
        -1.0f,  1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 0.0f
    };

    m_program->bind();
    functions->glActiveTexture(GL_TEXTURE0);
    functions->glBindTexture(GL_TEXTURE_2D, m_textureId);
    m_program->setUniformValue("yuyvTexture", 0);
    m_program->setUniformValue("frameWidth", static_cast<float>(m_frameWidth));

    const int positionLocation = m_program->attributeLocation("vertexPosition");
    const int texCoordLocation = m_program->attributeLocation("vertexTexCoord");

    m_program->enableAttributeArray(positionLocation);
    m_program->enableAttributeArray(texCoordLocation);
    m_program->setAttributeArray(positionLocation, GL_FLOAT, vertices, 2, 4 * sizeof(float));
    m_program->setAttributeArray(texCoordLocation, GL_FLOAT, vertices + 2, 2, 4 * sizeof(float));
    functions->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_program->disableAttributeArray(positionLocation);
    m_program->disableAttributeArray(texCoordLocation);
    m_program->release();
}

/*
 * createRenderer 的作用：
 *   创建与 V4L2VideoItem 配套的渲染器对象。
 */
QQuickFramebufferObject::Renderer *V4L2VideoItem::createRenderer() const
{
    return new YuyvRenderer();
}
