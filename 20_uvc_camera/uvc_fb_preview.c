/*
 * uvc_fb_preview.c
 *
 * 作用：
 *   这个程序用于 STM32MP157 Linux 开发板的最小 UVC 摄像头显示验证。
 *   它直接使用 V4L2 从 USB UVC 摄像头读取 YUYV 帧，再把画面转换成
 *   framebuffer 支持的 RGB565 或 XRGB8888 像素格式，写入 LCD 对应的 /dev/fb0。
 *
 * 主要流程：
 *   1. 解析命令行，确定 video 设备、fb 设备、采集宽高和帧数。
 *   2. 打开 framebuffer，查询 LCD 分辨率、位深和显存长度，并 mmap 到用户态。
 *   3. 打开 UVC 摄像头，设置 YUYV 格式，申请 V4L2 mmap 采集缓冲区。
 *   4. 启动视频流，循环 DQBUF 取帧、转换绘制、QBUF 还回缓冲区。
 *   5. Ctrl+C 或达到指定帧数后释放所有资源。
 *
 * 限制：
 *   本示例只直接处理 YUYV/YUY2。若摄像头只支持 MJPEG，需要使用 GStreamer
 *   的 jpegdec，或后续加入 libjpeg 解码。
 */

#include <errno.h>              /* errno 保存系统调用失败原因，便于打印具体错误。 */
#include <fcntl.h>              /* open 使用的 O_RDWR、O_NONBLOCK 等文件打开标志。 */
#include <linux/fb.h>           /* framebuffer 的 fb_fix_screeninfo、fb_var_screeninfo 结构。 */
#include <linux/videodev2.h>    /* V4L2 的格式、缓冲区、ioctl 命令和像素格式宏。 */
#include <signal.h>             /* signal 用于捕获 Ctrl+C，安全退出采集循环。 */
#include <stdint.h>             /* uint8_t/uint16_t/uint32_t 等明确宽度整数类型。 */
#include <stdio.h>              /* printf/perror/fprintf 用于输出运行状态和错误。 */
#include <stdlib.h>             /* EXIT_SUCCESS/EXIT_FAILURE 和 strtol。 */
#include <string.h>             /* memset/strcmp/strncpy 等字符串与内存工具。 */
#include <sys/ioctl.h>          /* ioctl 是 V4L2 和 framebuffer 配置的核心入口。 */
#include <sys/mman.h>           /* mmap/munmap 用于映射摄像头缓冲区和 framebuffer 显存。 */
#include <sys/select.h>         /* select 用于等待摄像头帧就绪，避免忙等占满 CPU。 */
#include <sys/time.h>           /* timeval 是 select 超时时间结构。 */
#include <sys/types.h>          /* POSIX 基础类型，配合 open/mmap/select 使用。 */
#include <unistd.h>             /* close/read/write/usleep 等 POSIX 系统调用声明。 */

/* 默认摄像头设备：UVC 驱动枚举成功后通常生成 /dev/video0。 */
#define DEFAULT_VIDEO_DEVICE "/dev/video0"

/* 默认 framebuffer 设备：正点原子 LCD 通常通过 /dev/fb0 暴露。 */
#define DEFAULT_FB_DEVICE "/dev/fb0"

/* 默认采集宽度：640x480 对 UVC 摄像头兼容性较好，CPU 转换压力也可控。 */
#define DEFAULT_WIDTH 640U

/* 默认采集高度：与 DEFAULT_WIDTH 组合成 VGA 分辨率。 */
#define DEFAULT_HEIGHT 480U

/* V4L2 mmap 缓冲区数量：4 个缓冲区能兼顾流畅性和内存占用。 */
#define CAMERA_BUFFER_COUNT 4U

/* select 等待单帧的超时时间，单位秒；摄像头异常时避免永久卡死。 */
#define FRAME_TIMEOUT_SEC 2

/*
 * camera_buffer 保存一个 V4L2 mmap 缓冲区的用户态地址和长度。
 * start 由 mmap 返回，采集过程中由内核写入图像数据，本程序读取。
 * length 来自 VIDIOC_QUERYBUF，释放时必须原样传给 munmap。
 */
struct camera_buffer {
    void *start;
    size_t length;
};

/*
 * framebuffer_device 保存 LCD framebuffer 的运行时信息。
 * fd 是 /dev/fb0 的文件描述符。
 * mem 是 mmap 后的显存起始地址，向它写像素即可显示到屏幕。
 * mem_len 是显存映射长度，用于 munmap 和边界保护。
 * fix 保存固定信息，例如一行字节数 line_length。
 * var 保存可变信息，例如 xres/yres/bits_per_pixel。
 */
struct framebuffer_device {
    int fd;
    uint8_t *mem;
    size_t mem_len;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
};

/*
 * camera_device 保存 UVC 摄像头的运行时信息。
 * fd 是 /dev/video0 的文件描述符。
 * buffers 是 mmap 后的采集缓冲区数组。
 * buffer_count 是成功申请到的缓冲区数量。
 * width/height 是驱动最终接受的采集尺寸。
 * pixelformat 是驱动最终接受的像素格式，本示例要求为 YUYV。
 */
struct camera_device {
    int fd;
    struct camera_buffer buffers[CAMERA_BUFFER_COUNT];
    unsigned int buffer_count;
    unsigned int width;
    unsigned int height;
    uint32_t pixelformat;
};

/*
 * app_config 保存命令行参数。
 * video_device 指向要打开的 V4L2 设备节点。
 * fb_device 指向要打开的 framebuffer 设备节点。
 * width/height 是希望摄像头输出的采集尺寸。
 * fps 为 0 表示使用摄像头默认帧率，非 0 表示通过 VIDIOC_S_PARM 请求指定帧率。
 * native_size 为 1 时不把画面缩放到全屏，只把原始采集画面居中绘制，便于测低 CPU 原型。
 * frame_limit 为 0 表示一直预览，非 0 表示采集指定帧数后退出。
 */
struct app_config {
    const char *video_device;
    const char *fb_device;
    unsigned int width;
    unsigned int height;
    unsigned int fps;
    int native_size;
    unsigned int frame_limit;
};

/* g_stop 由 Ctrl+C 信号处理函数置位，主循环看到后主动退出。 */
static volatile sig_atomic_t g_stop = 0;

/*
 * handle_signal 的作用：
 *   捕获 SIGINT/SIGTERM，把退出请求记录到 g_stop。
 *
 * 参数：
 *   signo 是系统传入的信号编号，本程序不需要区分具体信号。
 *
 * 返回值：
 *   无返回值；信号处理函数只做最小状态修改，避免在异步上下文里调用复杂函数。
 */
static void handle_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

/*
 * print_usage 的作用：
 *   打印命令行使用说明，方便在开发板串口上快速查看参数。
 *
 * 参数：
 *   prog 是 argv[0]，用于显示当前程序名。
 *
 * 返回值：
 *   无返回值，只输出帮助文本。
 */
static void print_usage(const char *prog)
{
    printf("用法: %s [-d /dev/video0] [-f /dev/fb0] [-w width] [-h height] [-r fps] [-p] [-n frames]\n", prog);
    printf("  -d  指定 UVC 摄像头设备，默认 %s\n", DEFAULT_VIDEO_DEVICE);
    printf("  -f  指定 framebuffer 设备，默认 %s\n", DEFAULT_FB_DEVICE);
    printf("  -w  指定采集宽度，默认 %u\n", DEFAULT_WIDTH);
    printf("  -h  指定采集高度，默认 %u\n", DEFAULT_HEIGHT);
    printf("  -r  指定采集帧率，0 表示使用摄像头默认帧率，默认 0\n");
    printf("  -p  原始尺寸居中绘制，不做全屏缩放，用于低 CPU 路线验证\n");
    printf("  -n  指定显示帧数，0 表示持续显示，默认 0\n");
}

/*
 * parse_positive 的作用：
 *   把命令行字符串解析为正整数，并做范围检查。
 *
 * 参数：
 *   text 是待解析的字符串。
 *   out 保存解析成功后的无符号整数。
 *
 * 返回值：
 *   成功返回 0；字符串非法、溢出或为 0 时返回 -1。
 */
static int parse_positive(const char *text, unsigned int *out)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);

    if (text[0] == '\0' || end == NULL || *end != '\0') {
        return -1;
    }

    if (value <= 0 || value > 8192) {
        return -1;
    }

    *out = (unsigned int)value;
    return 0;
}

/*
 * parse_non_negative 的作用：
 *   把命令行字符串解析为非负整数，专门用于 frame_limit。
 *
 * 参数：
 *   text 是待解析的字符串。
 *   out 保存解析成功后的无符号整数。
 *
 * 返回值：
 *   成功返回 0；字符串非法、溢出或小于 0 时返回 -1。
 */
static int parse_non_negative(const char *text, unsigned int *out)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);

    if (text[0] == '\0' || end == NULL || *end != '\0') {
        return -1;
    }

    if (value < 0 || value > 1000000) {
        return -1;
    }

    *out = (unsigned int)value;
    return 0;
}

/*
 * parse_args 的作用：
 *   读取命令行参数并填充 app_config。
 *
 * 参数：
 *   argc/argv 是 main 传入的命令行参数。
 *   cfg 是输出配置结构，调用者需要提前分配。
 *
 * 返回值：
 *   成功返回 0；参数错误或请求帮助时返回 -1。
 */
static int parse_args(int argc, char **argv, struct app_config *cfg)
{
    int opt;

    cfg->video_device = DEFAULT_VIDEO_DEVICE;
    cfg->fb_device = DEFAULT_FB_DEVICE;
    cfg->width = DEFAULT_WIDTH;
    cfg->height = DEFAULT_HEIGHT;
    cfg->fps = 0;
    cfg->native_size = 0;
    cfg->frame_limit = 0;

    while ((opt = getopt(argc, argv, "d:f:w:h:r:pn:?")) != -1) {
        switch (opt) {
        case 'd':
            cfg->video_device = optarg;
            break;
        case 'f':
            cfg->fb_device = optarg;
            break;
        case 'w':
            if (parse_positive(optarg, &cfg->width) != 0) {
                fprintf(stderr, "错误：宽度参数非法：%s\n", optarg);
                return -1;
            }
            break;
        case 'h':
            if (parse_positive(optarg, &cfg->height) != 0) {
                fprintf(stderr, "错误：高度参数非法：%s\n", optarg);
                return -1;
            }
            break;
        case 'r':
            if (parse_non_negative(optarg, &cfg->fps) != 0 || cfg->fps > 240U) {
                fprintf(stderr, "错误：帧率参数非法：%s\n", optarg);
                return -1;
            }
            break;
        case 'p':
            cfg->native_size = 1;
            break;
        case 'n':
            if (parse_non_negative(optarg, &cfg->frame_limit) != 0) {
                fprintf(stderr, "错误：帧数参数非法：%s\n", optarg);
                return -1;
            }
            break;
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    return 0;
}

/*
 * xioctl 的作用：
 *   对 ioctl 做 EINTR 重试封装。
 *
 * 参数：
 *   fd 是设备文件描述符。
 *   request 是 ioctl 命令号。
 *   arg 是命令参数结构体指针。
 *
 * 返回值：
 *   成功返回 ioctl 原始返回值；失败返回 -1 并保留 errno。
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
 * clamp_u8 的作用：
 *   把整数限制到 RGB 分量允许的 0~255 范围。
 *
 * 参数：
 *   value 是待限制的整数。
 *
 * 返回值：
 *   返回 uint8_t 类型的颜色分量。
 */
static uint8_t clamp_u8(int value)
{
    if (value < 0) {
        return 0;
    }

    if (value > 255) {
        return 255;
    }

    return (uint8_t)value;
}

/*
 * yuv_to_rgb 的作用：
 *   把一个 YUV 像素转换成 8bit RGB 分量。
 *
 * 参数：
 *   y/u/v 是 YUYV 数据中的亮度和色度分量。
 *   r/g/b 是输出的 RGB 分量指针。
 *
 * 返回值：
 *   无返回值；结果通过 r/g/b 写回。
 */
static void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    int c = (int)y - 16;
    int d = (int)u - 128;
    int e = (int)v - 128;

    if (c < 0) {
        c = 0;
    }

    *r = clamp_u8((298 * c + 409 * e + 128) >> 8);
    *g = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
    *b = clamp_u8((298 * c + 516 * d + 128) >> 8);
}

/*
 * rgb_to_rgb565 的作用：
 *   把 8bit RGB 分量压缩成 framebuffer 常见的 RGB565 像素。
 *
 * 参数：
 *   r/g/b 是 8bit RGB 分量。
 *
 * 返回值：
 *   返回 16bit RGB565 像素值。
 */
static uint16_t rgb_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                      ((uint16_t)(g & 0xFC) << 3) |
                      ((uint16_t)(b & 0xF8) >> 3));
}

/*
 * rgb_to_xrgb8888 的作用：
 *   把 8bit RGB 分量组装成 32bit XRGB8888 像素。
 *
 * 参数：
 *   r/g/b 是 8bit RGB 分量。
 *
 * 返回值：
 *   返回 32bit 像素值，最高 8bit 填 0。
 */
static uint32_t rgb_to_xrgb8888(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/*
 * open_framebuffer 的作用：
 *   打开并映射 framebuffer，准备向 LCD 显存写像素。
 *
 * 参数：
 *   device 是 framebuffer 设备节点路径。
 *   fb 是输出结构，成功后保存 fd、屏幕信息和 mmap 地址。
 *
 * 返回值：
 *   成功返回 0；打开、查询或 mmap 失败返回 -1。
 */
static int open_framebuffer(const char *device, struct framebuffer_device *fb)
{
    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;

    fb->fd = open(device, O_RDWR);
    if (fb->fd < 0) {
        perror("打开 framebuffer 失败");
        return -1;
    }

    if (xioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->fix) < 0) {
        perror("读取 framebuffer 固定信息失败");
        close(fb->fd);
        fb->fd = -1;
        return -1;
    }

    if (xioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) < 0) {
        perror("读取 framebuffer 可变信息失败");
        close(fb->fd);
        fb->fd = -1;
        return -1;
    }

    fb->mem_len = fb->fix.smem_len;
    fb->mem = mmap(NULL, fb->mem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED) {
        perror("映射 framebuffer 显存失败");
        close(fb->fd);
        fb->fd = -1;
        fb->mem = NULL;
        return -1;
    }

    memset(fb->mem, 0, fb->mem_len);
    printf("framebuffer: %s, %ux%u, %u bpp, line_length=%u\n",
           device, fb->var.xres, fb->var.yres, fb->var.bits_per_pixel, fb->fix.line_length);

    return 0;
}

/*
 * close_framebuffer 的作用：
 *   释放 framebuffer mmap 和文件描述符。
 *
 * 参数：
 *   fb 是 open_framebuffer 初始化过的结构。
 *
 * 返回值：
 *   无返回值；允许重复调用，内部会检查资源是否有效。
 */
static void close_framebuffer(struct framebuffer_device *fb)
{
    if (fb->mem != NULL && fb->mem != MAP_FAILED) {
        munmap(fb->mem, fb->mem_len);
        fb->mem = NULL;
    }

    if (fb->fd >= 0) {
        close(fb->fd);
        fb->fd = -1;
    }
}

/*
 * print_camera_formats 的作用：
 *   枚举摄像头支持的像素格式，设置 YUYV 失败时帮助定位问题。
 *
 * 参数：
 *   fd 是已经打开的 V4L2 摄像头文件描述符。
 *
 * 返回值：
 *   无返回值；只打印枚举结果。
 */
static void print_camera_formats(int fd)
{
    struct v4l2_fmtdesc desc;

    memset(&desc, 0, sizeof(desc));
    desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    printf("摄像头支持的像素格式：\n");
    while (xioctl(fd, VIDIOC_ENUM_FMT, &desc) == 0) {
        printf("  [%u] %.4s : %s\n",
               desc.index, (const char *)&desc.pixelformat, desc.description);
        desc.index++;
    }
}

/*
 * open_camera 的作用：
 *   打开 V4L2 摄像头并确认它支持视频采集和 streaming mmap。
 *
 * 参数：
 *   device 是摄像头设备节点路径。
 *   cam 是输出结构，成功后保存 fd。
 *
 * 返回值：
 *   成功返回 0；打开失败或能力不满足返回 -1。
 */
static int open_camera(const char *device, struct camera_device *cam)
{
    struct v4l2_capability cap;

    memset(cam, 0, sizeof(*cam));
    cam->fd = -1;

    cam->fd = open(device, O_RDWR | O_NONBLOCK);
    if (cam->fd < 0) {
        perror("打开摄像头失败");
        return -1;
    }

    memset(&cap, 0, sizeof(cap));
    if (xioctl(cam->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("查询摄像头能力失败");
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        fprintf(stderr, "错误：设备不支持 V4L2 视频采集\n");
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    if ((cap.capabilities & V4L2_CAP_STREAMING) == 0) {
        fprintf(stderr, "错误：设备不支持 streaming mmap 采集\n");
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    printf("camera: %s, driver=%s, card=%s, bus=%s\n", device, cap.driver, cap.card, cap.bus_info);
    return 0;
}

/*
 * setup_camera_format 的作用：
 *   请求摄像头输出 YUYV/YUY2 格式，并记录驱动最终确认的尺寸。
 *
 * 参数：
 *   cam 是已打开的摄像头结构。
 *   width/height 是期望采集尺寸。
 *
 * 返回值：
 *   成功返回 0；驱动无法设置格式或最终不是 YUYV 时返回 -1。
 */
static int setup_camera_format(struct camera_device *cam, unsigned int width, unsigned int height)
{
    struct v4l2_format fmt;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("设置摄像头 YUYV 格式失败");
        print_camera_formats(cam->fd);
        return -1;
    }

    cam->width = fmt.fmt.pix.width;
    cam->height = fmt.fmt.pix.height;
    cam->pixelformat = fmt.fmt.pix.pixelformat;

    printf("camera format: %ux%u, pixelformat=%.4s, bytesperline=%u\n",
           cam->width,
           cam->height,
           (const char *)&cam->pixelformat,
           fmt.fmt.pix.bytesperline);

    if (cam->pixelformat != V4L2_PIX_FMT_YUYV) {
        fprintf(stderr, "错误：驱动最终没有接受 YUYV，当前格式为 %.4s\n",
                (const char *)&cam->pixelformat);
        print_camera_formats(cam->fd);
        return -1;
    }

    return 0;
}

/*
 * set_camera_frame_rate 的作用：
 *   在摄像头已经设置好格式后，请求 UVC 设备输出指定帧率。
 *
 * 主要流程：
 *   1. 先读取当前 stream 参数，保留驱动已有的其它字段。
 *   2. 把 timeperframe 设置成 1/fps，请求驱动切到目标帧率。
 *   3. 再打印驱动最终确认的帧率，方便和 CPU 实测记录对应。
 *
 * 参数：
 *   cam 是已打开并已设置格式的摄像头结构。
 *   fps 是期望帧率；为 0 时调用者不应进入本函数。
 *
 * 返回值：
 *   成功返回 0；ioctl 失败或驱动返回非法时间基时返回 -1。
 */
static int set_camera_frame_rate(struct camera_device *cam, unsigned int fps)
{
    struct v4l2_streamparm parm;

    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (xioctl(cam->fd, VIDIOC_G_PARM, &parm) < 0) {
        perror("读取摄像头帧率参数失败");
        return -1;
    }

    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps;

    if (xioctl(cam->fd, VIDIOC_S_PARM, &parm) < 0) {
        perror("设置摄像头帧率失败");
        return -1;
    }

    if (parm.parm.capture.timeperframe.numerator == 0 ||
        parm.parm.capture.timeperframe.denominator == 0) {
        fprintf(stderr, "错误：驱动返回了非法帧率时间基\n");
        return -1;
    }

    printf("camera fps: requested=%u, actual=%u/%u fps\n",
           fps,
           parm.parm.capture.timeperframe.denominator,
           parm.parm.capture.timeperframe.numerator);

    return 0;
}

/*
 * init_camera_mmap 的作用：
 *   向 V4L2 驱动申请 mmap 缓冲区，并把每个缓冲区映射到用户态。
 *
 * 参数：
 *   cam 是已设置格式的摄像头结构。
 *
 * 返回值：
 *   成功返回 0；申请、查询或 mmap 任一失败返回 -1。
 */
static int init_camera_mmap(struct camera_device *cam)
{
    struct v4l2_requestbuffers req;
    unsigned int i;

    memset(&req, 0, sizeof(req));
    req.count = CAMERA_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("申请摄像头 mmap 缓冲区失败");
        return -1;
    }

    if (req.count < 2) {
        fprintf(stderr, "错误：摄像头驱动返回的缓冲区数量过少：%u\n", req.count);
        return -1;
    }

    cam->buffer_count = req.count;
    if (cam->buffer_count > CAMERA_BUFFER_COUNT) {
        cam->buffer_count = CAMERA_BUFFER_COUNT;
    }

    for (i = 0; i < cam->buffer_count; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("查询摄像头缓冲区失败");
            return -1;
        }

        cam->buffers[i].length = buf.length;
        cam->buffers[i].start = mmap(NULL,
                                     buf.length,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED,
                                     cam->fd,
                                     buf.m.offset);
        if (cam->buffers[i].start == MAP_FAILED) {
            perror("映射摄像头缓冲区失败");
            cam->buffers[i].start = NULL;
            return -1;
        }
    }

    return 0;
}

/*
 * start_camera_stream 的作用：
 *   把所有 mmap 缓冲区放入驱动队列，并启动摄像头数据流。
 *
 * 参数：
 *   cam 是已经完成 mmap 初始化的摄像头结构。
 *
 * 返回值：
 *   成功返回 0；QBUF 或 STREAMON 失败返回 -1。
 */
static int start_camera_stream(struct camera_device *cam)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    unsigned int i;

    for (i = 0; i < cam->buffer_count; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("摄像头缓冲区入队失败");
            return -1;
        }
    }

    if (xioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("启动摄像头视频流失败");
        return -1;
    }

    return 0;
}

/*
 * stop_camera_stream 的作用：
 *   请求 V4L2 驱动停止视频流。
 *
 * 参数：
 *   cam 是已经打开的摄像头结构。
 *
 * 返回值：
 *   无返回值；停止失败时只打印警告，因为后续仍要释放资源。
 */
static void stop_camera_stream(struct camera_device *cam)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (cam->fd >= 0 && xioctl(cam->fd, VIDIOC_STREAMOFF, &type) < 0) {
        perror("停止摄像头视频流失败");
    }
}

/*
 * close_camera 的作用：
 *   释放摄像头 mmap 缓冲区并关闭设备文件。
 *
 * 参数：
 *   cam 是摄像头结构。
 *
 * 返回值：
 *   无返回值；允许在部分初始化失败后调用。
 */
static void close_camera(struct camera_device *cam)
{
    unsigned int i;

    for (i = 0; i < cam->buffer_count; i++) {
        if (cam->buffers[i].start != NULL && cam->buffers[i].start != MAP_FAILED) {
            munmap(cam->buffers[i].start, cam->buffers[i].length);
            cam->buffers[i].start = NULL;
        }
    }

    if (cam->fd >= 0) {
        close(cam->fd);
        cam->fd = -1;
    }
}

/*
 * draw_yuyv_native_to_framebuffer 的作用：
 *   把摄像头原始尺寸画面直接居中写入 framebuffer，不做全屏缩放。
 *
 * 为什么需要这个函数：
 *   原始 draw_yuyv_to_framebuffer 为了适配全屏，会为每个目标像素计算
 *   src_x/src_y 比例映射。低 CPU 实验时我们只关心 640x480 原始画面，
 *   可以跳过这些除法和额外像素，直接一对 YUYV 像素转换成两个屏幕像素。
 *
 * 参数：
 *   fb 是已映射的 framebuffer。
 *   frame 是摄像头输出的 YUYV 原始帧。
 *   src_width/src_height 是摄像头帧尺寸。
 *
 * 返回值：
 *   成功返回 0；如果 framebuffer 位深不支持或源图超过屏幕则返回 -1。
 */
static int draw_yuyv_native_to_framebuffer(struct framebuffer_device *fb,
                                           const uint8_t *frame,
                                           unsigned int src_width,
                                           unsigned int src_height)
{
    unsigned int x_offset;
    unsigned int y_offset;
    unsigned int y;

    if (fb->var.bits_per_pixel != 16 && fb->var.bits_per_pixel != 32) {
        fprintf(stderr, "错误：当前只支持 16bpp 或 32bpp framebuffer，实际为 %u bpp\n",
                fb->var.bits_per_pixel);
        return -1;
    }

    if (src_width > fb->var.xres || src_height > fb->var.yres) {
        fprintf(stderr, "错误：原始画面 %ux%u 大于 framebuffer %ux%u，无法居中绘制\n",
                src_width, src_height, fb->var.xres, fb->var.yres);
        return -1;
    }

    x_offset = (fb->var.xres - src_width) / 2U;
    y_offset = (fb->var.yres - src_height) / 2U;

    for (y = 0; y < src_height; y++) {
        unsigned int x;
        const uint8_t *src_line = frame + y * src_width * 2U;
        uint8_t *dst_line = fb->mem + (y + y_offset) * fb->fix.line_length;

        for (x = 0; x < src_width; x += 2U) {
            const uint8_t *p = src_line + x * 2U;
            uint8_t y0 = p[0];
            uint8_t u = p[1];
            uint8_t y1 = p[2];
            uint8_t v = p[3];
            uint8_t r;
            uint8_t g;
            uint8_t b;

            yuv_to_rgb(y0, u, v, &r, &g, &b);
            if (fb->var.bits_per_pixel == 16) {
                uint16_t *pixel = (uint16_t *)(dst_line + (x + x_offset) * 2U);
                pixel[0] = rgb_to_rgb565(r, g, b);
            } else {
                uint32_t *pixel = (uint32_t *)(dst_line + (x + x_offset) * 4U);
                pixel[0] = rgb_to_xrgb8888(r, g, b);
            }

            yuv_to_rgb(y1, u, v, &r, &g, &b);
            if (fb->var.bits_per_pixel == 16) {
                uint16_t *pixel = (uint16_t *)(dst_line + (x + 1U + x_offset) * 2U);
                pixel[0] = rgb_to_rgb565(r, g, b);
            } else {
                uint32_t *pixel = (uint32_t *)(dst_line + (x + 1U + x_offset) * 4U);
                pixel[0] = rgb_to_xrgb8888(r, g, b);
            }
        }
    }

    return 0;
}

/*
 * draw_yuyv_to_framebuffer 的作用：
 *   把一帧 YUYV 图像绘制到 framebuffer 中央。
 *
 * 参数：
 *   fb 是已映射的 framebuffer。
 *   frame 是摄像头 YUYV 帧数据。
 *   src_width/src_height 是摄像头帧尺寸。
 *   native_size 为 1 时使用原始尺寸居中绘制，为 0 时保持旧的全屏等比缩放。
 *
 * 返回值：
 *   成功返回 0；当前 framebuffer 位深不是 16/32bpp 时返回 -1。
 */
static int draw_yuyv_to_framebuffer(struct framebuffer_device *fb,
                                    const uint8_t *frame,
                                    unsigned int src_width,
                                    unsigned int src_height,
                                    int native_size)
{
    unsigned int dst_width = fb->var.xres;
    unsigned int dst_height = fb->var.yres;
    unsigned int draw_width = dst_width;
    unsigned int draw_height = dst_height;
    unsigned int x_offset = 0;
    unsigned int y_offset = 0;
    unsigned int y;

    if (fb->var.bits_per_pixel != 16 && fb->var.bits_per_pixel != 32) {
        fprintf(stderr, "错误：当前只支持 16bpp 或 32bpp framebuffer，实际为 %u bpp\n",
                fb->var.bits_per_pixel);
        return -1;
    }

    if (native_size) {
        return draw_yuyv_native_to_framebuffer(fb, frame, src_width, src_height);
    }

    if (src_width * dst_height > dst_width * src_height) {
        draw_height = (dst_width * src_height) / src_width;
        y_offset = (dst_height - draw_height) / 2;
    } else {
        draw_width = (dst_height * src_width) / src_height;
        x_offset = (dst_width - draw_width) / 2;
    }

    for (y = 0; y < draw_height; y++) {
        unsigned int x;
        unsigned int src_y = (y * src_height) / draw_height;
        uint8_t *dst_line = fb->mem + (y + y_offset) * fb->fix.line_length;

        for (x = 0; x < draw_width; x++) {
            unsigned int src_x = (x * src_width) / draw_width;
            unsigned int pair_x = src_x & ~1U;
            const uint8_t *p = frame + (src_y * src_width + pair_x) * 2U;
            uint8_t y_value = (src_x & 1U) ? p[2] : p[0];
            uint8_t u_value = p[1];
            uint8_t v_value = p[3];
            uint8_t r;
            uint8_t g;
            uint8_t b;

            yuv_to_rgb(y_value, u_value, v_value, &r, &g, &b);

            if (fb->var.bits_per_pixel == 16) {
                uint16_t *pixel = (uint16_t *)(dst_line + (x + x_offset) * 2U);
                *pixel = rgb_to_rgb565(r, g, b);
            } else {
                uint32_t *pixel = (uint32_t *)(dst_line + (x + x_offset) * 4U);
                *pixel = rgb_to_xrgb8888(r, g, b);
            }
        }
    }

    return 0;
}

/*
 * capture_loop 的作用：
 *   等待摄像头帧就绪，取出帧、绘制到屏幕，再把缓冲区还给驱动。
 *
 * 参数：
 *   cam 是已启动 streaming 的摄像头。
 *   fb 是已映射的 framebuffer。
 *   native_size 控制是否按原始尺寸居中绘制，便于验证低 CPU 原型。
 *   frame_limit 是最大显示帧数，0 表示持续显示。
 *
 * 返回值：
 *   正常结束返回 0；等待超时、DQBUF/QBUF 或绘制失败返回 -1。
 */
static int capture_loop(struct camera_device *cam,
                        struct framebuffer_device *fb,
                        int native_size,
                        unsigned int frame_limit)
{
    unsigned int frames = 0;

    while (!g_stop && (frame_limit == 0 || frames < frame_limit)) {
        fd_set fds;
        struct timeval tv;
        int ret;
        struct v4l2_buffer buf;

        FD_ZERO(&fds);
        FD_SET(cam->fd, &fds);

        tv.tv_sec = FRAME_TIMEOUT_SEC;
        tv.tv_usec = 0;

        ret = select(cam->fd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("等待摄像头帧失败");
            return -1;
        }

        if (ret == 0) {
            fprintf(stderr, "错误：等待摄像头帧超时，请检查 USB 摄像头是否正常输出\n");
            return -1;
        }

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) {
                continue;
            }
            perror("取出摄像头缓冲区失败");
            return -1;
        }

        if (buf.index >= cam->buffer_count) {
            fprintf(stderr, "错误：驱动返回了越界缓冲区索引：%u\n", buf.index);
            return -1;
        }

        if (draw_yuyv_to_framebuffer(fb,
                                     (const uint8_t *)cam->buffers[buf.index].start,
                                     cam->width,
                                     cam->height,
                                     native_size) != 0) {
            return -1;
        }

        if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("摄像头缓冲区重新入队失败");
            return -1;
        }

        frames++;
        if ((frames % 30U) == 0U) {
            printf("已显示 %u 帧\n", frames);
            fflush(stdout);
        }
    }

    printf("预览结束，共显示 %u 帧\n", frames);
    return 0;
}

/*
 * main 的作用：
 *   程序入口，负责串联参数解析、设备初始化、采集显示和资源释放。
 *
 * 参数：
 *   argc/argv 是命令行参数。
 *
 * 返回值：
 *   EXIT_SUCCESS 表示预览正常结束；EXIT_FAILURE 表示初始化或采集过程中失败。
 */
int main(int argc, char **argv)
{
    struct app_config cfg;
    struct framebuffer_device fb;
    struct camera_device cam;
    int ret = EXIT_FAILURE;
    int stream_started = 0;

    if (parse_args(argc, argv, &cfg) != 0) {
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (open_framebuffer(cfg.fb_device, &fb) != 0) {
        return EXIT_FAILURE;
    }

    if (open_camera(cfg.video_device, &cam) != 0) {
        close_framebuffer(&fb);
        return EXIT_FAILURE;
    }

    if (setup_camera_format(&cam, cfg.width, cfg.height) != 0) {
        goto out;
    }

    if (cfg.fps != 0U && set_camera_frame_rate(&cam, cfg.fps) != 0) {
        goto out;
    }

    if (init_camera_mmap(&cam) != 0) {
        goto out;
    }

    if (start_camera_stream(&cam) != 0) {
        goto out;
    }
    stream_started = 1;

    if (capture_loop(&cam, &fb, cfg.native_size, cfg.frame_limit) != 0) {
        goto out;
    }

    ret = EXIT_SUCCESS;

out:
    if (stream_started) {
        stop_camera_stream(&cam);
    }
    close_camera(&cam);
    close_framebuffer(&fb);
    return ret;
}
