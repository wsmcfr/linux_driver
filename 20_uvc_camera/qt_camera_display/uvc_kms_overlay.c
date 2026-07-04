/*
 * uvc_kms_overlay.c
 *
 * 作用：
 *   这是 STM32MP157 Qt 工业检测界面的 KMS overlay 摄像头显示辅助进程。
 *   它负责从 UVC 摄像头采集 640x480 YUYV 帧，使用 NEON/标量路径转换为
 *   ARGB8888，并把结果写入 DRM overlay plane 36，让 Qt eglfs 界面保留在
 *   primary plane 上。
 *
 * 主要流程：
 *   1. 使用 V4L2 mmap 从 UVC 摄像头采集 YUYV 帧。
 *   2. 使用 libdrm 创建与摄像头同尺寸的 dumb framebuffer。
 *   3. 把 YUYV 图像转换成 KMS overlay plane 支持的 ARGB8888/RGB 类格式。
 *   4. 使用 drmModeSetPlane 把 framebuffer 放到 Qt 预留的视频窗口。
 *   5. 收到 SIGTERM/SIGINT 后释放 V4L2、DRM 和 mmap 资源。
 *
 * 安全边界：
 *   - 本程序只负责摄像头视频 plane，不绘制 Qt UI。
 *   - 运行前由控制脚本停止当前 gst-launch 恢复线，避免 /dev/video0 被占用。
 *   - 显示实验必须用 nohup 后台运行并把日志写到 /tmp。
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <arm_neon.h>
#include <drm.h>
#include <drm_fourcc.h>
#include <jpeglib.h>
#include <png.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/* 默认 V4L2 摄像头节点。 */
#define DEFAULT_VIDEO_DEVICE "/dev/video0"

/* 默认 DRM 设备节点，STM32 LTDC 暴露为 /dev/dri/card0。 */
#define DEFAULT_DRM_DEVICE "/dev/dri/card0"

/* 默认控制 socket；Qt 界面通过它请求 overlay 进程保存当前正在上屏的摄像头帧。 */
#define DEFAULT_CONTROL_SOCKET "/tmp/uvc-kms-overlay-control.sock"

/* 默认 SD 卡挂载点；保存请求会确认该挂载点存在，避免误写到 rootfs 的空目录。 */
#define DEFAULT_SDCARD_MOUNT "/mnt/sdcard"

/* 默认采集分辨率，保持当前探索目标的 640x480。 */
#define DEFAULT_WIDTH 640U
#define DEFAULT_HEIGHT 480U

/* 默认采集帧率；传入 -r 15 可测试高清 15fps 路线。 */
#define DEFAULT_FPS 10U

/* 检测模型当前使用中心 300x300 ROI；屏幕观察框必须与 defect-classify 的中心裁剪口径一致。 */
#define DEFAULT_DETECT_ROI_SIZE 300U

/* ROI 观察框线宽；3 像素在 1024x600 LCD 上足够清楚，同时不会明显遮挡零件边缘。 */
#define DETECT_ROI_BORDER_THICKNESS 3U

/* ROI 观察框颜色；ARGB8888 数值为不透明绿色，方便在金属和白色背景上观察。 */
#define DETECT_ROI_COLOR_ARGB8888 0xff35d07fU

/* ROI 观察框 RGB565 颜色；用于实验性 RGB565 输出格式，颜色来源同上面的绿色。 */
#define DETECT_ROI_COLOR_RGB565 0x368fU

/* 自动视觉定位的横向搜索宽度，保持和模型检测 ROI 宽度一致，避免左右支架误入定位。 */
#define AUTO_LOCATE_SEARCH_WIDTH DEFAULT_DETECT_ROI_SIZE

/* 自动视觉定位的最小连通域面积，过滤相机噪声、反光点和压缩杂点。 */
#define AUTO_LOCATE_MIN_COMPONENT_AREA 40U

/* 自动视觉定位的最大连通域面积比例分母，避免把整片背景误判成零件。 */
#define AUTO_LOCATE_MAX_COMPONENT_AREA_DIVISOR 2U

/* 自动视觉定位的最小外接框边长，太窄的亮线或暗线不作为完整零件。 */
#define AUTO_LOCATE_MIN_BBOX_SIDE 6U

/* 自动视觉定位的最大外接框边长，首版零件必须小于中心 ROI 的大部分区域。 */
#define AUTO_LOCATE_MAX_BBOX_SIDE 260U

/*
 * 自动视觉定位的基础亮度差阈值。
 *
 * 黑色波形零件在传送带、亚克力反光或曝光变化下，边缘亮度差有时低于 18，
 * 会造成“肉眼已经入画，但 LOCATE 间歇返回 has_target=0”的漏检。
 * 这里先降到 12，优先提高黑色零件的连续识别概率；若现场误检背景，再回调到 15~18。
 */
#define AUTO_LOCATE_MIN_LUMA_DELTA 12U

/* V4L2 mmap 缓冲区数量；4 个缓冲区能避免偶发抖动。 */
#define CAMERA_BUFFER_COUNT 4U

/* 等待单帧超时时间，避免摄像头异常时永久阻塞。 */
#define FRAME_TIMEOUT_SEC 2

/*
 * camera_buffer 表示一个 V4L2 mmap 缓冲区。
 * start 是用户态映射地址；length 是释放时必须传回 munmap 的长度。
 */
struct camera_buffer {
    void *start;
    size_t length;
};

/*
 * camera_device 保存摄像头运行时状态。
 * fd 是 /dev/video0 的文件描述符；buffers 是 mmap 缓冲数组。
 * width/height/pixelformat 记录驱动最终确认的采集格式。
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
 * kms_device 保存 DRM/KMS 显示资源。
 * old_crtc 用于退出时尽量恢复进入程序前的 CRTC 状态。
 * fb_map 指向 dumb buffer 的 mmap 地址，转换后的 XRGB8888 像素写入这里。
 */
struct kms_device {
    int fd;
    uint32_t connector_id;
    uint32_t crtc_id;
    drmModeModeInfo mode;
    drmModeCrtc *old_crtc;
    uint32_t dumb_handle;
    uint32_t fb_id;
    uint32_t pitch;
    uint64_t size;
    uint8_t *fb_map;
    unsigned int fb_width;
    unsigned int fb_height;
    int use_plane;
    uint32_t plane_id;
    int32_t dst_x;
    int32_t dst_y;
    uint32_t dst_w;
    uint32_t dst_h;
    int plane_visible;
};

/*
 * output_format 表示临时探针写入 KMS dumb framebuffer 的像素格式。
 * XRGB8888 是已验证的默认路径；RGB565 是本轮只用于比较写带宽和转换开销的实验路径。
 */
enum output_format {
    OUTPUT_FORMAT_XRGB8888 = 0,
    OUTPUT_FORMAT_RGB565 = 1,
    OUTPUT_FORMAT_ARGB8888 = 2,
};

/*
 * app_config 保存命令行配置。
 * frame_limit 为 0 表示持续运行；非 0 用于短跑自测。
 */
struct app_config {
    const char *video_device;
    const char *drm_device;
    const char *control_socket_path;
    unsigned int width;
    unsigned int height;
    unsigned int fps;
    unsigned int frame_limit;
    int use_neon;
    int use_staging;
    enum output_format output_format;
    int use_plane;
    unsigned int plane_id;
    int32_t dst_x;
    int32_t dst_y;
    uint32_t dst_w;
    uint32_t dst_h;
    int initial_visible;
};

/*
 * control_server 保存 Qt UI 与 overlay 进程之间的 Unix socket 控制端点。
 * fd 是监听 socket；socket_path 是文件系统中的 socket 路径，退出时必须 unlink。
 */
struct control_server {
    int fd;
    char socket_path[PATH_MAX];
};

/*
 * latest_frame 描述已经转换并写入 KMS framebuffer 的最新一帧。
 * yuyv_map 指向当前已从 V4L2 队列取出的原始 YUYV 缓冲，用于点击保存/检测时生成无 ROI 框图片。
 */
struct latest_frame {
    uint8_t *fb_map;
    struct kms_device *kms;
    const uint8_t *yuyv_map;
    size_t yuyv_size;
    uint32_t pitch;
    unsigned int fb_width;
    unsigned int fb_height;
    unsigned int frame_width;
    unsigned int frame_height;
    unsigned int bytes_per_pixel;
    unsigned int serial;
    enum output_format output_format;
    int has_frame;
};

/*
 * locate_result 保存一次内存级零件定位的结果。
 * has_target 表示是否找到可信连通域；frame_id 对应 latest_frame.serial，便于 Qt 和 F4 对齐日志。
 * frame_width/frame_height 是原始摄像头尺寸；center/bbox 都使用原始 YUYV 帧坐标，不含 KMS 居中偏移。
 * confidence 是 0~100 的粗略置信度，供 MP157 下发给 F4 和现场调参时观察。
 */
struct locate_result {
    int has_target;
    unsigned int frame_id;
    unsigned int frame_width;
    unsigned int frame_height;
    int center_x;
    int center_y;
    int bbox_x;
    int bbox_y;
    int bbox_w;
    int bbox_h;
    unsigned int confidence;
};

/*
 * jpeg_error_context 保存 libjpeg 发生错误时的跳转现场。
 * pub 必须作为第一个字段，因为 libjpeg 会把 jpeg_error_mgr 指针传回错误回调，
 * 这里通过结构体首地址把错误管理器重新转回本程序自己的上下文。
 */
struct jpeg_error_context {
    struct jpeg_error_mgr pub;
    jmp_buf jump_buffer;
};

/* g_stop 由信号处理函数置位，主循环据此安全退出。 */
static volatile sig_atomic_t g_stop = 0;

/*
 * handle_signal 的作用：
 *   捕获 SIGINT/SIGTERM，通知主循环退出。
 *
 * 参数：
 *   signo 是信号编号，本程序不需要区分具体信号。
 *
 * 返回值：
 *   无返回值；信号上下文只做简单赋值，避免不可重入操作。
 */
static void handle_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

/*
 * xioctl 的作用：
 *   对 ioctl 做 EINTR 重试封装，减少信号打断带来的误失败。
 *
 * 参数：
 *   fd 是设备文件描述符。
 *   request 是 ioctl 命令。
 *   arg 是命令参数指针。
 *
 * 返回值：
 *   成功返回 ioctl 原始返回值；失败返回 -1 并保留 errno。
 */
static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);

    return ret;
}

/*
 * parse_uint 的作用：
 *   把命令行中的正整数或非负整数解析为 unsigned int。
 *
 * 参数：
 *   text 是输入字符串。
 *   out 是输出整数。
 *   allow_zero 表示是否允许 0。
 *
 * 返回值：
 *   成功返回 0；格式非法或越界返回 -1。
 */
static int parse_uint(const char *text, unsigned int *out, int allow_zero)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);

    if (text[0] == '\0' || end == NULL || *end != '\0') {
        return -1;
    }

    if ((!allow_zero && value <= 0) || value < 0 || value > 1000000L) {
        return -1;
    }

    *out = (unsigned int)value;
    return 0;
}

/*
 * print_usage 的作用：
 *   打印临时探针的参数说明。
 *
 * 参数：
 *   prog 是 argv[0]。
 *
 * 返回值：
 *   无返回值，只输出帮助信息。
 */
static void print_usage(const char *prog)
{
    printf("用法: %s [-d /dev/video0] [-D /dev/dri/card0] [-S /tmp/socket] [-w width] [-h height] [-r fps] [-n frames] [-m scalar|neon] [-F xrgb8888|argb8888|rgb565] [-s direct|staging] [-P plane-id] [-x dst-x] [-y dst-y] [-W dst-width] [-H dst-height] [-V 0|1]\n", prog);
    printf("  -d  V4L2 摄像头节点，默认 %s\n", DEFAULT_VIDEO_DEVICE);
    printf("  -D  DRM 设备节点，默认 %s\n", DEFAULT_DRM_DEVICE);
    printf("  -S  Qt 控制 socket 路径，默认 %s\n", DEFAULT_CONTROL_SOCKET);
    printf("  -w  采集宽度，默认 %u\n", DEFAULT_WIDTH);
    printf("  -h  采集高度，默认 %u\n", DEFAULT_HEIGHT);
    printf("  -r  请求帧率，默认 %u\n", DEFAULT_FPS);
    printf("  -n  显示帧数，0 表示持续运行，默认 0\n");
    printf("  -m  转换模式：scalar 或 neon，默认 scalar\n");
    printf("  -F  KMS 输出格式：xrgb8888、argb8888 或 rgb565，默认 xrgb8888\n");
    printf("  -s  写入策略：direct 直接写 KMS mmap；staging 先写 malloc 缓冲再 memcpy，默认 direct\n");
    printf("  -P  使用指定 DRM plane 显示，不调用 drmModeSetCrtc；例如 -P 36 用 overlay plane\n");
    printf("  -x  plane 目标左上角 X 坐标；未指定时按目标宽度居中\n");
    printf("  -y  plane 目标左上角 Y 坐标；未指定时按目标高度居中\n");
    printf("  -W  plane 目标显示宽度；未指定时使用 framebuffer 宽度\n");
    printf("  -H  plane 目标显示高度；未指定时使用 framebuffer 高度\n");
    printf("  -V  初始视频层可见性：1 表示启动后立即显示，0 表示先隐藏等待 Qt 启动画面结束，默认 1\n");
}

/*
 * output_format_name 的作用：
 *   把输出格式枚举转换为日志字符串，便于板端日志记录准确的测试路线。
 *
 * 参数：
 *   format 是命令行解析得到的输出格式枚举。
 *
 * 返回值：
 *   返回静态字符串，不需要调用者释放。
 */
static const char *output_format_name(enum output_format format)
{
    switch (format) {
    case OUTPUT_FORMAT_ARGB8888:
        return "argb8888";
    case OUTPUT_FORMAT_RGB565:
        return "rgb565";
    case OUTPUT_FORMAT_XRGB8888:
    default:
        return "xrgb8888";
    }
}

/*
 * bytes_per_pixel_for_format 的作用：
 *   返回输出像素格式对应的单像素字节数，供 staging 缓冲和行拷贝使用。
 *
 * 参数：
 *   format 是 KMS 输出格式。
 *
 * 返回值：
 *   XRGB8888 返回 4，RGB565 返回 2。
 */
static unsigned int bytes_per_pixel_for_format(enum output_format format)
{
    if (format == OUTPUT_FORMAT_RGB565) {
        return 2U;
    }

    return 4U;
}

/*
 * parse_args 的作用：
 *   解析命令行并填充 app_config。
 *
 * 参数：
 *   argc/argv 是 main 传入的命令行参数。
 *   cfg 是输出配置。
 *
 * 返回值：
 *   成功返回 0；参数错误返回 -1。
 */
static int parse_args(int argc, char **argv, struct app_config *cfg)
{
    int opt;

    cfg->video_device = DEFAULT_VIDEO_DEVICE;
    cfg->drm_device = DEFAULT_DRM_DEVICE;
    cfg->control_socket_path = DEFAULT_CONTROL_SOCKET;
    cfg->width = DEFAULT_WIDTH;
    cfg->height = DEFAULT_HEIGHT;
    cfg->fps = DEFAULT_FPS;
    cfg->frame_limit = 0;
    cfg->use_neon = 0;
    cfg->use_staging = 0;
    cfg->output_format = OUTPUT_FORMAT_XRGB8888;
    cfg->use_plane = 0;
    cfg->plane_id = 0;
    cfg->dst_x = -1;
    cfg->dst_y = -1;
    cfg->dst_w = 0;
    cfg->dst_h = 0;
    cfg->initial_visible = 1;

    while ((opt = getopt(argc, argv, "d:D:S:w:h:r:n:m:F:s:P:x:y:W:H:V:?")) != -1) {
        switch (opt) {
        case 'd':
            cfg->video_device = optarg;
            break;
        case 'D':
            cfg->drm_device = optarg;
            break;
        case 'S':
            cfg->control_socket_path = optarg;
            break;
        case 'w':
            if (parse_uint(optarg, &cfg->width, 0) != 0) {
                fprintf(stderr, "宽度参数非法: %s\n", optarg);
                return -1;
            }
            break;
        case 'h':
            if (parse_uint(optarg, &cfg->height, 0) != 0) {
                fprintf(stderr, "高度参数非法: %s\n", optarg);
                return -1;
            }
            break;
        case 'r':
            if (parse_uint(optarg, &cfg->fps, 0) != 0 || cfg->fps > 240U) {
                fprintf(stderr, "帧率参数非法: %s\n", optarg);
                return -1;
            }
            break;
        case 'n':
            if (parse_uint(optarg, &cfg->frame_limit, 1) != 0) {
                fprintf(stderr, "帧数参数非法: %s\n", optarg);
                return -1;
            }
            break;
        case 'm':
            if (strcmp(optarg, "scalar") == 0) {
                cfg->use_neon = 0;
            } else if (strcmp(optarg, "neon") == 0) {
                cfg->use_neon = 1;
            } else {
                fprintf(stderr, "转换模式非法: %s\n", optarg);
                return -1;
            }
            break;
        case 'F':
            if (strcmp(optarg, "xrgb8888") == 0) {
                cfg->output_format = OUTPUT_FORMAT_XRGB8888;
            } else if (strcmp(optarg, "argb8888") == 0) {
                cfg->output_format = OUTPUT_FORMAT_ARGB8888;
            } else if (strcmp(optarg, "rgb565") == 0) {
                cfg->output_format = OUTPUT_FORMAT_RGB565;
            } else {
                fprintf(stderr, "输出格式非法: %s\n", optarg);
                return -1;
            }
            break;
        case 's':
            if (strcmp(optarg, "direct") == 0) {
                cfg->use_staging = 0;
            } else if (strcmp(optarg, "staging") == 0) {
                cfg->use_staging = 1;
            } else {
                fprintf(stderr, "写入策略非法: %s\n", optarg);
                return -1;
            }
            break;
        case 'P':
            if (parse_uint(optarg, &cfg->plane_id, 0) != 0) {
                fprintf(stderr, "plane-id 参数非法: %s\n", optarg);
                return -1;
            }
            cfg->use_plane = 1;
            break;
        case 'x': {
            unsigned int value;

            /* x 坐标只接受非负值，避免临时探针把视频窗口放到屏幕外导致肉眼误判黑屏。 */
            if (parse_uint(optarg, &value, 1) != 0) {
                fprintf(stderr, "plane 目标 X 坐标非法: %s\n", optarg);
                return -1;
            }
            cfg->dst_x = (int32_t)value;
            break;
        }
        case 'y': {
            unsigned int value;

            /* y 坐标只接受非负值；需要裁剪实验时再单独扩展负坐标支持。 */
            if (parse_uint(optarg, &value, 1) != 0) {
                fprintf(stderr, "plane 目标 Y 坐标非法: %s\n", optarg);
                return -1;
            }
            cfg->dst_y = (int32_t)value;
            break;
        }
        case 'W':
            /* 目标宽度必须大于 0；是否支持缩放由 DRM plane 在 drmModeSetPlane 时判定。 */
            if (parse_uint(optarg, &cfg->dst_w, 0) != 0) {
                fprintf(stderr, "plane 目标宽度非法: %s\n", optarg);
                return -1;
            }
            break;
        case 'H':
            /* 目标高度必须大于 0；失败时保持原错误，便于区分参数错误和硬件不支持缩放。 */
            if (parse_uint(optarg, &cfg->dst_h, 0) != 0) {
                fprintf(stderr, "plane 目标高度非法: %s\n", optarg);
                return -1;
            }
            break;
        case 'V': {
            unsigned int value;

            /*
             * 初始可见性只允许 0/1：
             *   0 用于开机启动动画阶段，overlay 进程先采集和建立 socket，但不把视频 plane 盖到 Qt 上。
             *   1 保留原来的调试行为，便于单独运行 overlay 时马上看到摄像头画面。
             */
            if (parse_uint(optarg, &value, 1) != 0 || value > 1U) {
                fprintf(stderr, "初始视频层可见性非法: %s\n", optarg);
                return -1;
            }
            cfg->initial_visible = (int)value;
            break;
        }
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    return 0;
}

/*
 * narrow_rgb_u8 的作用：
 *   把两个 int32x4_t RGB 计算结果饱和收窄为 uint8x8_t。
 *
 * 参数：
 *   lo/hi 分别是低 4 个和高 4 个像素的 32bit 计算结果。
 *
 * 返回值：
 *   返回 8 个 0~255 范围内的颜色分量。
 */
static inline uint8x8_t narrow_rgb_u8(int32x4_t lo, int32x4_t hi)
{
    int16x4_t lo16 = vqmovn_s32(lo);
    int16x4_t hi16 = vqmovn_s32(hi);
    int16x8_t all16 = vcombine_s16(lo16, hi16);

    return vqmovun_s16(all16);
}

/*
 * convert_8px_yuyv_to_xrgb_neon 的作用：
 *   使用 NEON 一次转换 8 个 YUYV 像素为 XRGB8888。
 *
 * 关键逻辑：
 *   - 输入 16 字节 YUYV，表示 8 个像素。
 *   - 使用 vuzp/vzip 把 Y、U、V 拆开并把每个 U/V 复制给相邻两个像素。
 *   - 使用和 scalar 路径相同的 BT.601 limited range 整数公式。
 *   - 用 vst4_u8 按 little-endian XRGB8888 的 B/G/R/X 字节序写出。
 *
 * 参数：
 *   src 指向 16 字节 YUYV 数据。
 *   dst 指向 8 个 XRGB8888 像素的输出起点。
 *
 * 返回值：
 *   无返回值，结果直接写入 dst。
 */
static inline void convert_8px_yuyv_to_xrgb_neon(const uint8_t *src, uint8_t *dst)
{
    uint8x16_t raw = vld1q_u8(src);
    uint8x8x2_t split = vuzp_u8(vget_low_u8(raw), vget_high_u8(raw));
    uint8x8_t y_u8 = split.val[0];
    uint8x8_t uv_interleaved = split.val[1];
    uint8x8x2_t uv_compact = vuzp_u8(uv_interleaved, uv_interleaved);
    uint8x8_t u_u8 = vzip_u8(uv_compact.val[0], uv_compact.val[0]).val[0];
    uint8x8_t v_u8 = vzip_u8(uv_compact.val[1], uv_compact.val[1]).val[0];
    int16x8_t c_s16 = vreinterpretq_s16_u16(vmovl_u8(y_u8));
    int16x8_t d_s16 = vreinterpretq_s16_u16(vmovl_u8(u_u8));
    int16x8_t e_s16 = vreinterpretq_s16_u16(vmovl_u8(v_u8));
    int32x4_t c_lo;
    int32x4_t c_hi;
    int32x4_t d_lo;
    int32x4_t d_hi;
    int32x4_t e_lo;
    int32x4_t e_hi;
    int32x4_t r_lo;
    int32x4_t r_hi;
    int32x4_t g_lo;
    int32x4_t g_hi;
    int32x4_t b_lo;
    int32x4_t b_hi;
    uint8x8x4_t bgra;

    c_s16 = vmaxq_s16(vsubq_s16(c_s16, vdupq_n_s16(16)), vdupq_n_s16(0));
    d_s16 = vsubq_s16(d_s16, vdupq_n_s16(128));
    e_s16 = vsubq_s16(e_s16, vdupq_n_s16(128));

    c_lo = vmovl_s16(vget_low_s16(c_s16));
    c_hi = vmovl_s16(vget_high_s16(c_s16));
    d_lo = vmovl_s16(vget_low_s16(d_s16));
    d_hi = vmovl_s16(vget_high_s16(d_s16));
    e_lo = vmovl_s16(vget_low_s16(e_s16));
    e_hi = vmovl_s16(vget_high_s16(e_s16));

    r_lo = vmlaq_n_s32(vmulq_n_s32(c_lo, 298), e_lo, 409);
    r_hi = vmlaq_n_s32(vmulq_n_s32(c_hi, 298), e_hi, 409);
    g_lo = vmlsq_n_s32(vmlsq_n_s32(vmulq_n_s32(c_lo, 298), d_lo, 100), e_lo, 208);
    g_hi = vmlsq_n_s32(vmlsq_n_s32(vmulq_n_s32(c_hi, 298), d_hi, 100), e_hi, 208);
    b_lo = vmlaq_n_s32(vmulq_n_s32(c_lo, 298), d_lo, 516);
    b_hi = vmlaq_n_s32(vmulq_n_s32(c_hi, 298), d_hi, 516);

    r_lo = vshrq_n_s32(vaddq_s32(r_lo, vdupq_n_s32(128)), 8);
    r_hi = vshrq_n_s32(vaddq_s32(r_hi, vdupq_n_s32(128)), 8);
    g_lo = vshrq_n_s32(vaddq_s32(g_lo, vdupq_n_s32(128)), 8);
    g_hi = vshrq_n_s32(vaddq_s32(g_hi, vdupq_n_s32(128)), 8);
    b_lo = vshrq_n_s32(vaddq_s32(b_lo, vdupq_n_s32(128)), 8);
    b_hi = vshrq_n_s32(vaddq_s32(b_hi, vdupq_n_s32(128)), 8);

    bgra.val[0] = narrow_rgb_u8(b_lo, b_hi);
    bgra.val[1] = narrow_rgb_u8(g_lo, g_hi);
    bgra.val[2] = narrow_rgb_u8(r_lo, r_hi);
    bgra.val[3] = vdup_n_u8(255);
    vst4_u8(dst, bgra);
}

/*
 * convert_8px_yuyv_to_rgb565_neon 的作用：
 *   使用 NEON 一次转换 8 个 YUYV 像素为 RGB565。
 *
 * 关键逻辑：
 *   - 复用 BT.601 limited range 整数公式，保证颜色口径与 XRGB8888 路径一致。
 *   - 把 8bit R/G/B 分量压缩为 5/6/5 bit 后写入 16bit 像素。
 *   - 该路径用于验证 direct KMS 写入量减半是否能继续降低 CPU。
 *
 * 参数：
 *   src 指向 16 字节 YUYV 数据。
 *   dst 指向 8 个 RGB565 像素的输出起点。
 *
 * 返回值：
 *   无返回值，结果直接写入 dst。
 */
static inline void convert_8px_yuyv_to_rgb565_neon(const uint8_t *src, uint16_t *dst)
{
    uint8x16_t raw = vld1q_u8(src);
    uint8x8x2_t split = vuzp_u8(vget_low_u8(raw), vget_high_u8(raw));
    uint8x8_t y_u8 = split.val[0];
    uint8x8_t uv_interleaved = split.val[1];
    uint8x8x2_t uv_compact = vuzp_u8(uv_interleaved, uv_interleaved);
    uint8x8_t u_u8 = vzip_u8(uv_compact.val[0], uv_compact.val[0]).val[0];
    uint8x8_t v_u8 = vzip_u8(uv_compact.val[1], uv_compact.val[1]).val[0];
    int16x8_t c_s16 = vreinterpretq_s16_u16(vmovl_u8(y_u8));
    int16x8_t d_s16 = vreinterpretq_s16_u16(vmovl_u8(u_u8));
    int16x8_t e_s16 = vreinterpretq_s16_u16(vmovl_u8(v_u8));
    int32x4_t c_lo;
    int32x4_t c_hi;
    int32x4_t d_lo;
    int32x4_t d_hi;
    int32x4_t e_lo;
    int32x4_t e_hi;
    int32x4_t r_lo;
    int32x4_t r_hi;
    int32x4_t g_lo;
    int32x4_t g_hi;
    int32x4_t b_lo;
    int32x4_t b_hi;
    uint8x8_t r_u8;
    uint8x8_t g_u8;
    uint8x8_t b_u8;
    uint16x8_t r_u16;
    uint16x8_t g_u16;
    uint16x8_t b_u16;
    uint16x8_t rgb565;

    c_s16 = vmaxq_s16(vsubq_s16(c_s16, vdupq_n_s16(16)), vdupq_n_s16(0));
    d_s16 = vsubq_s16(d_s16, vdupq_n_s16(128));
    e_s16 = vsubq_s16(e_s16, vdupq_n_s16(128));

    c_lo = vmovl_s16(vget_low_s16(c_s16));
    c_hi = vmovl_s16(vget_high_s16(c_s16));
    d_lo = vmovl_s16(vget_low_s16(d_s16));
    d_hi = vmovl_s16(vget_high_s16(d_s16));
    e_lo = vmovl_s16(vget_low_s16(e_s16));
    e_hi = vmovl_s16(vget_high_s16(e_s16));

    r_lo = vmlaq_n_s32(vmulq_n_s32(c_lo, 298), e_lo, 409);
    r_hi = vmlaq_n_s32(vmulq_n_s32(c_hi, 298), e_hi, 409);
    g_lo = vmlsq_n_s32(vmlsq_n_s32(vmulq_n_s32(c_lo, 298), d_lo, 100), e_lo, 208);
    g_hi = vmlsq_n_s32(vmlsq_n_s32(vmulq_n_s32(c_hi, 298), d_hi, 100), e_hi, 208);
    b_lo = vmlaq_n_s32(vmulq_n_s32(c_lo, 298), d_lo, 516);
    b_hi = vmlaq_n_s32(vmulq_n_s32(c_hi, 298), d_hi, 516);

    r_lo = vshrq_n_s32(vaddq_s32(r_lo, vdupq_n_s32(128)), 8);
    r_hi = vshrq_n_s32(vaddq_s32(r_hi, vdupq_n_s32(128)), 8);
    g_lo = vshrq_n_s32(vaddq_s32(g_lo, vdupq_n_s32(128)), 8);
    g_hi = vshrq_n_s32(vaddq_s32(g_hi, vdupq_n_s32(128)), 8);
    b_lo = vshrq_n_s32(vaddq_s32(b_lo, vdupq_n_s32(128)), 8);
    b_hi = vshrq_n_s32(vaddq_s32(b_hi, vdupq_n_s32(128)), 8);

    r_u8 = narrow_rgb_u8(r_lo, r_hi);
    g_u8 = narrow_rgb_u8(g_lo, g_hi);
    b_u8 = narrow_rgb_u8(b_lo, b_hi);

    r_u16 = vmovl_u8(r_u8);
    g_u16 = vmovl_u8(g_u8);
    b_u16 = vmovl_u8(b_u8);

    rgb565 = vorrq_u16(vorrq_u16(vshlq_n_u16(vandq_u16(r_u16, vdupq_n_u16(0x00f8U)), 8),
                                  vshlq_n_u16(vandq_u16(g_u16, vdupq_n_u16(0x00fcU)), 3)),
                       vshrq_n_u16(b_u16, 3));
    vst1q_u16(dst, rgb565);
}

/*
 * clamp_u8 的作用：
 *   把整数裁剪到 8bit 颜色分量范围。
 *
 * 参数：
 *   value 是待裁剪值。
 *
 * 返回值：
 *   返回 0~255 范围内的 uint8_t。
 */
static inline uint8_t clamp_u8(int value)
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
 * pack_xrgb_from_yuv 的作用：
 *   把单个 YUV 像素转换成 DRM_FORMAT_XRGB8888 的 32bit 像素值。
 *
 * 参数：
 *   y/u/v 是 YUYV 中的亮度和色度分量。
 *
 * 返回值：
 *   返回 0x00RRGGBB；在 little-endian 内存中对应 B/G/R/X 字节序。
 */
static inline uint32_t pack_xrgb_from_yuv(uint8_t y, uint8_t u, uint8_t v)
{
    int c = (int)y - 16;
    int d = (int)u - 128;
    int e = (int)v - 128;
    uint8_t r;
    uint8_t g;
    uint8_t b;

    if (c < 0) {
        c = 0;
    }

    r = clamp_u8((298 * c + 409 * e + 128) >> 8);
    g = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
    b = clamp_u8((298 * c + 516 * d + 128) >> 8);

    return 0xff000000U | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/*
 * pack_rgb565_from_yuv 的作用：
 *   把单个 YUV 像素转换成 DRM_FORMAT_RGB565 的 16bit 像素值。
 *
 * 参数：
 *   y/u/v 是 YUYV 中的亮度和色度分量。
 *
 * 返回值：
 *   返回 RGB565 低端序内存可直接写入的 16bit 像素值。
 */
static inline uint16_t pack_rgb565_from_yuv(uint8_t y, uint8_t u, uint8_t v)
{
    int c = (int)y - 16;
    int d = (int)u - 128;
    int e = (int)v - 128;
    uint8_t r;
    uint8_t g;
    uint8_t b;

    if (c < 0) {
        c = 0;
    }

    r = clamp_u8((298 * c + 409 * e + 128) >> 8);
    g = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
    b = clamp_u8((298 * c + 516 * d + 128) >> 8);

    return (uint16_t)(((uint16_t)(r & 0xf8U) << 8) |
                      ((uint16_t)(g & 0xfcU) << 3) |
                      ((uint16_t)b >> 3));
}

/*
 * roi_coordinate_in_border 的作用：
 *   判断摄像头图像内部的某个坐标是否落在中心 ROI 观察框边线上。
 *
 * 主要流程：
 *   1. 根据 src_width/src_height 计算中心 300x300 ROI 的左上角。
 *   2. 判断当前 x/y 是否在 ROI 矩形范围内。
 *   3. 判断该坐标是否属于上、下、左、右任意一条边框。
 *
 * 参数：
 *   x/y 是摄像头图像内部坐标，不包含 KMS framebuffer 居中偏移。
 *   src_width/src_height 是摄像头图像尺寸。
 *
 * 返回值：
 *   在 ROI 边框上返回 1；不在边框上返回 0。
 */
static int roi_coordinate_in_border(unsigned int x,
                                    unsigned int y,
                                    unsigned int src_width,
                                    unsigned int src_height)
{
    unsigned int roi_x;
    unsigned int roi_y;
    unsigned int roi_right;
    unsigned int roi_bottom;

    if (src_width < DEFAULT_DETECT_ROI_SIZE || src_height < DEFAULT_DETECT_ROI_SIZE) {
        return 0;
    }

    roi_x = (src_width - DEFAULT_DETECT_ROI_SIZE) / 2U;
    roi_y = (src_height - DEFAULT_DETECT_ROI_SIZE) / 2U;
    roi_right = roi_x + DEFAULT_DETECT_ROI_SIZE;
    roi_bottom = roi_y + DEFAULT_DETECT_ROI_SIZE;

    if (x < roi_x || x >= roi_right || y < roi_y || y >= roi_bottom) {
        return 0;
    }

    if (x < roi_x + DETECT_ROI_BORDER_THICKNESS ||
        x >= roi_right - DETECT_ROI_BORDER_THICKNESS ||
        y < roi_y + DETECT_ROI_BORDER_THICKNESS ||
        y >= roi_bottom - DETECT_ROI_BORDER_THICKNESS) {
        return 1;
    }

    return 0;
}

/*
 * draw_roi_overlay_row_xrgb 的作用：
 *   在已经完成 YUYV->XRGB8888 转换的一行像素上覆盖 ROI 边框。
 *
 * 关键说明：
 *   这个函数在每一行视频像素转换完成后立即执行，避免上一版“整帧转换后再补画框”
 *   造成的单 framebuffer 扫描闪烁；它只改一行上属于 ROI 边框的少量像素。
 *
 * 参数：
 *   dst_line 是当前输出行的 XRGB/ARGB 像素数组。
 *   y 是摄像头图像内部行号。
 *   src_width/src_height 是摄像头图像尺寸。
 *
 * 返回值：
 *   无返回值，ROI 边框像素直接写入 dst_line。
 */
static void draw_roi_overlay_row_xrgb(uint32_t *dst_line,
                                      unsigned int y,
                                      unsigned int src_width,
                                      unsigned int src_height)
{
    unsigned int x;

    if (dst_line == NULL) {
        return;
    }

    for (x = 0; x < src_width; x++) {
        if (roi_coordinate_in_border(x, y, src_width, src_height)) {
            dst_line[x] = DETECT_ROI_COLOR_ARGB8888;
        }
    }
}

/*
 * draw_roi_overlay_row_rgb565 的作用：
 *   在已经完成 YUYV->RGB565 转换的一行像素上覆盖 ROI 边框。
 *
 * 参数：
 *   dst_line 是当前输出行的 RGB565 像素数组。
 *   y 是摄像头图像内部行号。
 *   src_width/src_height 是摄像头图像尺寸。
 *
 * 返回值：
 *   无返回值，ROI 边框像素直接写入 dst_line。
 */
static void draw_roi_overlay_row_rgb565(uint16_t *dst_line,
                                        unsigned int y,
                                        unsigned int src_width,
                                        unsigned int src_height)
{
    unsigned int x;

    if (dst_line == NULL) {
        return;
    }

    for (x = 0; x < src_width; x++) {
        if (roi_coordinate_in_border(x, y, src_width, src_height)) {
            dst_line[x] = DETECT_ROI_COLOR_RGB565;
        }
    }
}

/*
 * convert_yuyv_to_xrgb_center 的作用：
 *   把一帧 YUYV 原始图像转换成居中的 XRGB8888 图像区域。
 *
 * 关键逻辑：
 *   - 每 4 字节 YUYV 对应两个像素，共用 U/V 色度。
 *   - 只更新源图对应的矩形区域，避免每帧清屏造成额外内存写入。
 *   - 输出格式固定为 XRGB8888，以匹配 STM32 LTDC 支持的 RGB 类格式。
 *
 * 参数：
 *   dst 是 DRM dumb buffer 映射地址。
 *   dst_pitch 是显示 framebuffer 每行字节数。
 *   dst_width/dst_height 是显示模式尺寸。
 *   src 是摄像头 YUYV 帧。
 *   src_width/src_height 是摄像头帧尺寸。
 *
 * 返回值：
 *   成功返回 0；源图大于目标 framebuffer 时返回 -1。
 */
static int convert_yuyv_to_xrgb_center(uint8_t *dst,
                                       uint32_t dst_pitch,
                                       unsigned int dst_width,
                                       unsigned int dst_height,
                                       const uint8_t *src,
                                       unsigned int src_width,
                                       unsigned int src_height,
                                       int use_neon)
{
    unsigned int x_offset;
    unsigned int y_offset;
    unsigned int y;

    if (src_width > dst_width || src_height > dst_height) {
        fprintf(stderr, "源图 %ux%u 大于 KMS framebuffer %ux%u\n",
                src_width, src_height, dst_width, dst_height);
        return -1;
    }

    x_offset = (dst_width - src_width) / 2U;
    y_offset = (dst_height - src_height) / 2U;

    for (y = 0; y < src_height; y++) {
        unsigned int x;
        const uint8_t *src_line = src + y * src_width * 2U;
        uint32_t *dst_line = (uint32_t *)(dst + (y + y_offset) * dst_pitch) + x_offset;

        if (use_neon) {
            for (x = 0; x + 7U < src_width; x += 8U) {
                convert_8px_yuyv_to_xrgb_neon(src_line + x * 2U,
                                              (uint8_t *)(dst_line + x));
            }
        } else {
            x = 0;
        }

        for (; x < src_width; x += 2U) {
            const uint8_t *p = src_line + x * 2U;
            uint8_t y0 = p[0];
            uint8_t u = p[1];
            uint8_t y1 = p[2];
            uint8_t v = p[3];

            dst_line[x] = pack_xrgb_from_yuv(y0, u, v);
            dst_line[x + 1U] = pack_xrgb_from_yuv(y1, u, v);
        }

        draw_roi_overlay_row_xrgb(dst_line, y, src_width, src_height);
    }

    return 0;
}

/*
 * convert_yuyv_to_rgb565_center 的作用：
 *   把一帧 YUYV 原始图像转换成居中的 RGB565 图像区域。
 *
 * 关键逻辑：
 *   - 与 XRGB8888 路径保持相同的居中策略和色彩公式。
 *   - 每个输出像素只写 16bit，用于验证 direct KMS dumb buffer 写带宽减半是否有收益。
 *   - 只更新摄像头矩形区域，避免每帧清空整屏 framebuffer。
 *
 * 参数：
 *   dst 是 DRM dumb buffer 映射地址。
 *   dst_pitch 是显示 framebuffer 每行字节数。
 *   dst_width/dst_height 是显示模式尺寸。
 *   src 是摄像头 YUYV 帧。
 *   src_width/src_height 是摄像头帧尺寸。
 *   use_neon 为非 0 时使用 NEON 批量转换。
 *
 * 返回值：
 *   成功返回 0；源图大于目标 framebuffer 时返回 -1。
 */
static int convert_yuyv_to_rgb565_center(uint8_t *dst,
                                         uint32_t dst_pitch,
                                         unsigned int dst_width,
                                         unsigned int dst_height,
                                         const uint8_t *src,
                                         unsigned int src_width,
                                         unsigned int src_height,
                                         int use_neon)
{
    unsigned int x_offset;
    unsigned int y_offset;
    unsigned int y;

    if (src_width > dst_width || src_height > dst_height) {
        fprintf(stderr, "源图 %ux%u 大于 KMS framebuffer %ux%u\n",
                src_width, src_height, dst_width, dst_height);
        return -1;
    }

    x_offset = (dst_width - src_width) / 2U;
    y_offset = (dst_height - src_height) / 2U;

    for (y = 0; y < src_height; y++) {
        unsigned int x;
        const uint8_t *src_line = src + y * src_width * 2U;
        uint16_t *dst_line = (uint16_t *)(dst + (y + y_offset) * dst_pitch) + x_offset;

        if (use_neon) {
            for (x = 0; x + 7U < src_width; x += 8U) {
                convert_8px_yuyv_to_rgb565_neon(src_line + x * 2U, dst_line + x);
            }
        } else {
            x = 0;
        }

        for (; x < src_width; x += 2U) {
            const uint8_t *p = src_line + x * 2U;
            uint8_t y0 = p[0];
            uint8_t u = p[1];
            uint8_t y1 = p[2];
            uint8_t v = p[3];

            dst_line[x] = pack_rgb565_from_yuv(y0, u, v);
            dst_line[x + 1U] = pack_rgb565_from_yuv(y1, u, v);
        }

        draw_roi_overlay_row_rgb565(dst_line, y, src_width, src_height);
    }

    return 0;
}

/*
 * copy_staging_to_center 的作用：
 *   把已经转换好的连续 staging 图像复制到 KMS framebuffer 的居中区域。
 *
 * 关键逻辑：
 *   - staging 缓冲是普通 malloc 内存，CPU 写入通常比直接写 KMS mmap 更友好。
 *   - 复制阶段按行 memcpy 到 KMS framebuffer，便于验证“转换写入”和“KMS 写入”分离后是否降低 CPU。
 *   - 本函数只复制摄像头矩形区域，不改变屏幕边框区域。
 *
 * 参数：
 *   dst 是 KMS framebuffer 映射地址。
 *   dst_pitch 是 KMS framebuffer 每行字节数。
 *   dst_width/dst_height 是显示模式尺寸。
 *   staging 是已转换好的源图。
 *   staging_pitch 是 staging 每行字节数。
 *   src_width/src_height 是摄像头图像尺寸。
 *   bytes_per_pixel 是输出格式的每像素字节数。
 *
 * 返回值：
 *   成功返回 0；源图大于目标 framebuffer 时返回 -1。
 */
static int copy_staging_to_center(uint8_t *dst,
                                  uint32_t dst_pitch,
                                  unsigned int dst_width,
                                  unsigned int dst_height,
                                  const uint8_t *staging,
                                  uint32_t staging_pitch,
                                  unsigned int src_width,
                                  unsigned int src_height,
                                  unsigned int bytes_per_pixel)
{
    unsigned int x_offset;
    unsigned int y_offset;
    unsigned int y;
    size_t row_bytes;

    if (src_width > dst_width || src_height > dst_height) {
        fprintf(stderr, "源图 %ux%u 大于 KMS framebuffer %ux%u\n",
                src_width, src_height, dst_width, dst_height);
        return -1;
    }

    x_offset = (dst_width - src_width) / 2U;
    y_offset = (dst_height - src_height) / 2U;
    row_bytes = (size_t)src_width * bytes_per_pixel;

    for (y = 0; y < src_height; y++) {
        uint8_t *dst_line = dst + (y + y_offset) * dst_pitch + x_offset * bytes_per_pixel;
        const uint8_t *src_line = staging + y * staging_pitch;

        memcpy(dst_line, src_line, row_bytes);
    }

    return 0;
}

/*
 * write_all 的作用：
 *   把指定长度的数据完整写入文件描述符，处理短写和 EINTR。
 *
 * 参数：
 *   fd 是已经打开的输出文件。
 *   data 是要写入的缓冲区。
 *   length 是必须写完的字节数。
 *
 * 返回值：
 *   全部写完返回 0；写入失败返回 -1，errno 保存失败原因。
 */
static int write_all(int fd, const void *data, size_t length)
{
    const uint8_t *cursor = (const uint8_t *)data;
    size_t remaining = length;

    while (remaining > 0U) {
        ssize_t written = write(fd, cursor, remaining);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (written == 0) {
            errno = EIO;
            return -1;
        }

        cursor += written;
        remaining -= (size_t)written;
    }

    return 0;
}

/*
 * mkdir_p 的作用：
 *   递归创建保存图片所需目录，等价于最小版 mkdir -p。
 *
 * 参数：
 *   path 是要创建的目录路径。
 *
 * 返回值：
 *   目录存在或创建成功返回 0；失败返回 -1。
 */
static int mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    size_t len;
    char *p;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    len = strlen(path);
    if (len >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(tmp, path, len + 1U);

    if (len > 1U && tmp[len - 1U] == '/') {
        tmp[len - 1U] = '\0';
    }

    for (p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

/*
 * mount_point_is_mounted 的作用：
 *   检查指定路径是否真的出现在 /proc/mounts 中。
 *
 * 参数：
 *   mount_point 是挂载点路径，例如 /mnt/sdcard。
 *
 * 返回值：
 *   已挂载返回 1；未挂载或无法读取 /proc/mounts 返回 0。
 */
static int mount_point_is_mounted(const char *mount_point)
{
    FILE *fp;
    char device[256];
    char path[PATH_MAX];
    int found = 0;

    fp = fopen("/proc/mounts", "r");
    if (fp == NULL) {
        return 0;
    }

    while (fscanf(fp, "%255s %4095s %*s %*s %*d %*d\n", device, path) == 2) {
        if (strcmp(path, mount_point) == 0) {
            found = 1;
            break;
        }
    }

    fclose(fp);
    return found;
}

/*
 * path_under_sdcard 的作用：
 *   判断保存目录是否位于 /mnt/sdcard 下，避免测试按钮把图片误写到 rootfs。
 *
 * 参数：
 *   path 是 Qt 传入的保存目录。
 *
 * 返回值：
 *   位于 /mnt/sdcard 或等于 /mnt/sdcard 返回 1；其它路径返回 0。
 */
static int path_under_sdcard(const char *path)
{
    const size_t mount_len = strlen(DEFAULT_SDCARD_MOUNT);

    if (path == NULL) {
        return 0;
    }

    if (strcmp(path, DEFAULT_SDCARD_MOUNT) == 0) {
        return 1;
    }

    return strncmp(path, DEFAULT_SDCARD_MOUNT, mount_len) == 0 && path[mount_len] == '/';
}

/*
 * path_under_detect_output 的作用：
 *   判断检测按钮的图片目录是否位于允许的检测输出根目录下。
 *
 * 关键说明：
 *   旧调试路径允许写 /tmp/qt-defect-detect，方便 SSH 快速验证模型。
 *   正式界面路径允许写 /mnt/sdcard/images，因为一次检测的 source 和结果图需要进入历史记录。
 *   其它目录一律拒绝，避免 Qt socket 被误用成任意文件写入口。
 *
 * 参数：
 *   path 是 Qt 传入的检测保存目录。
 *
 * 返回值：
 *   位于 /tmp/qt-defect-detect、/mnt/sdcard/images 或其子目录时返回 1；其它路径返回 0。
 */
static int path_under_detect_output(const char *path)
{
    const char *tmp_root = "/tmp/qt-defect-detect";
    const char *sdcard_root = "/mnt/sdcard/images";
    const size_t root_len = strlen(tmp_root);
    const size_t sdcard_root_len = strlen(sdcard_root);

    if (path == NULL) {
        return 0;
    }

    if (strcmp(path, tmp_root) == 0) {
        return 1;
    }

    if (strncmp(path, tmp_root, root_len) == 0 && path[root_len] == '/') {
        return 1;
    }

    if (strcmp(path, sdcard_root) == 0) {
        return 1;
    }

    return strncmp(path, sdcard_root, sdcard_root_len) == 0 && path[sdcard_root_len] == '/';
}

/*
 * copy_yuyv_frame_to_rgb24 的作用：
 *   从当前 V4L2 原始 YUYV 缓冲生成一份无 ROI 框的连续 RGB24 图像。
 *
 * 主要流程：
 *   1. 校验 latest_frame 中的 yuyv_map 是否指向当前 dequeue 后的摄像头帧。
 *   2. 按 YUYV 两像素一组复用 U/V 的规则转换为 RGB24。
 *   3. 不读取 KMS framebuffer，因此不会把屏幕 ROI 观察框写进检测/保存图片。
 *
 * 参数：
 *   frame 是最新帧描述。
 *   rgb 是调用者提供的连续 RGB24 输出缓冲。
 *   rgb_size 是 rgb 缓冲长度，必须至少等于 width * height * 3。
 *
 * 返回值：
 *   成功返回 0；失败返回 -1，并设置 errno。
 */
static int copy_yuyv_frame_to_rgb24(const struct latest_frame *frame,
                                    uint8_t *rgb,
                                    size_t rgb_size)
{
    unsigned int y;
    size_t need_size;
    size_t need_yuyv_size;

    if (!frame->has_frame || frame->yuyv_map == NULL || rgb == NULL) {
        errno = ENODATA;
        return -1;
    }

    need_size = (size_t)frame->frame_width * frame->frame_height * 3U;
    if (rgb_size < need_size) {
        errno = ENOBUFS;
        return -1;
    }

    need_yuyv_size = (size_t)frame->frame_width * frame->frame_height * 2U;
    if (frame->yuyv_size < need_yuyv_size) {
        errno = ENODATA;
        return -1;
    }

    for (y = 0; y < frame->frame_height; y++) {
        const uint8_t *src = frame->yuyv_map + (size_t)y * frame->frame_width * 2U;
        uint8_t *dst = rgb + (size_t)y * frame->frame_width * 3U;
        unsigned int x;

        for (x = 0; x + 1U < frame->frame_width; x += 2U) {
            const uint8_t *p = src + x * 2U;
            uint8_t y0 = p[0];
            uint8_t u = p[1];
            uint8_t y1 = p[2];
            uint8_t v = p[3];
            uint32_t pixel0 = pack_xrgb_from_yuv(y0, u, v);
            uint32_t pixel1 = pack_xrgb_from_yuv(y1, u, v);

            dst[x * 3U + 0U] = (uint8_t)((pixel0 >> 16U) & 0xffU);
            dst[x * 3U + 1U] = (uint8_t)((pixel0 >> 8U) & 0xffU);
            dst[x * 3U + 2U] = (uint8_t)(pixel0 & 0xffU);
            dst[(x + 1U) * 3U + 0U] = (uint8_t)((pixel1 >> 16U) & 0xffU);
            dst[(x + 1U) * 3U + 1U] = (uint8_t)((pixel1 >> 8U) & 0xffU);
            dst[(x + 1U) * 3U + 2U] = (uint8_t)(pixel1 & 0xffU);
        }
    }

    return 0;
}

/*
 * build_snapshot_base_path 的作用：
 *   根据当前时间和帧序号生成不会轻易重复的图片基础路径。
 *
 * 参数：
 *   output_dir 是保存目录。
 *   frame 是最新帧信息，serial 会参与文件名。
 *   out_path/out_size 是输出基础路径缓冲区和长度。
 *
 * 返回值：
 *   成功返回 0；路径过长或时间格式化失败返回 -1。
 */
static int build_snapshot_base_path(const char *output_dir,
                                    const struct latest_frame *frame,
                                    char *out_path,
                                    size_t out_size)
{
    time_t now;
    struct tm tm_now;
    char time_text[32];
    int written;

    now = time(NULL);
    if (localtime_r(&now, &tm_now) == NULL) {
        return -1;
    }

    if (strftime(time_text, sizeof(time_text), "%Y%m%d_%H%M%S", &tm_now) == 0U) {
        return -1;
    }

    written = snprintf(out_path,
                       out_size,
                       "%s/uvc_%s_%06u",
                       output_dir,
                       time_text,
                       frame->serial);
    if (written < 0 || (size_t)written >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

/*
 * append_path_suffix 的作用：
 *   给基础路径追加扩展名或临时后缀，统一处理路径长度检查。
 *
 * 参数：
 *   base_path 是不带扩展名的基础路径。
 *   suffix 是要追加的后缀，例如 ".jpg" 或 ".png.tmp"。
 *   out_path/out_size 是输出路径缓冲区和长度。
 *
 * 返回值：
 *   成功返回 0；路径过长返回 -1 并设置 errno=ENAMETOOLONG。
 */
static int append_path_suffix(const char *base_path,
                              const char *suffix,
                              char *out_path,
                              size_t out_size)
{
    int written;

    written = snprintf(out_path, out_size, "%s%s", base_path, suffix);
    if (written < 0 || (size_t)written >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

/*
 * build_tmp_path 的作用：
 *   为最终图片路径生成同目录临时文件路径。
 *
 * 关键说明：
 *   先写入 .tmp 再 rename，可以避免 Qt 或上传脚本看到半截 JPG/PNG。
 *
 * 参数：
 *   final_path 是最终文件路径。
 *   tmp_path/tmp_size 是输出临时路径缓冲区和长度。
 *
 * 返回值：
 *   成功返回 0；路径过长返回 -1。
 */
static int build_tmp_path(const char *final_path, char *tmp_path, size_t tmp_size)
{
    return append_path_suffix(final_path, ".tmp", tmp_path, tmp_size);
}

/*
 * fsync_file_stream 的作用：
 *   把 stdio 缓冲和内核页缓存都刷到文件描述符。
 *
 * 参数：
 *   fp 是已经打开并写入的 FILE。
 *
 * 返回值：
 *   fflush、fsync 任一步失败返回 -1；全部成功返回 0。
 */
static int fsync_file_stream(FILE *fp)
{
    int fd;

    if (fflush(fp) != 0) {
        return -1;
    }

    fd = fileno(fp);
    if (fd < 0) {
        return -1;
    }

    if (fsync(fd) != 0) {
        return -1;
    }

    return 0;
}

/*
 * copy_latest_frame_to_rgb24 的作用：
 *   把当前摄像头原始 YUYV 帧复制成连续 RGB24 缓冲。
 *
 * 主要流程：
 *   1. 为当前帧申请一份 RGB24 输出缓冲。
 *   2. 调用 copy_yuyv_frame_to_rgb24 从 V4L2 原始帧转换，避开带 ROI 的显示 framebuffer。
 *   3. 调用者拿到的是 malloc 分配的新缓冲，可以安全用于 JPEG/PNG 编码。
 *
 * 参数：
 *   frame 是最新显示帧。
 *
 * 返回值：
 *   成功返回 malloc 分配的 RGB24 缓冲，调用者负责 free；
 *   失败返回 NULL，并通过 errno 说明原因。
 */
static uint8_t *copy_latest_frame_to_rgb24(const struct latest_frame *frame)
{
    uint8_t *rgb = NULL;
    size_t rgb_size;

    if (!frame->has_frame || frame->fb_map == NULL) {
        errno = ENODATA;
        return NULL;
    }

    rgb_size = (size_t)frame->frame_width * frame->frame_height * 3U;
    rgb = malloc(rgb_size);
    if (rgb == NULL) {
        return NULL;
    }

    if (copy_yuyv_frame_to_rgb24(frame, rgb, rgb_size) != 0) {
        free(rgb);
        return NULL;
    }

    return rgb;
}

/*
 * jpeg_error_exit 的作用：
 *   把 libjpeg 的致命错误转换为 longjmp，避免库默认直接退出进程。
 *
 * 参数：
 *   cinfo 是 libjpeg 传入的压缩上下文。
 *
 * 返回值：
 *   不直接返回，跳回 write_rgb24_as_jpeg 的错误处理分支。
 */
static void jpeg_error_exit(j_common_ptr cinfo)
{
    struct jpeg_error_context *ctx = (struct jpeg_error_context *)cinfo->err;

    longjmp(ctx->jump_buffer, 1);
}

/*
 * write_rgb24_as_jpeg 的作用：
 *   把连续 RGB24 缓冲编码成 JPEG 文件，并保证成功返回前完成 fsync。
 *
 * 主要流程：
 *   1. 先写入同目录 .tmp 文件，避免上传脚本读到半成品。
 *   2. 使用 libjpeg quality=85 写入每一行 RGB 数据。
 *   3. fflush + fsync 后关闭文件，再 rename 为最终 .jpg 路径。
 *
 * 参数：
 *   rgb 是连续 RGB24 图像数据。
 *   width/height 是图像尺寸。
 *   output_path 是最终 JPG 文件路径。
 *
 * 返回值：
 *   成功返回 0；失败返回 -1。
 */
static int write_rgb24_as_jpeg(const uint8_t *rgb,
                               unsigned int width,
                               unsigned int height,
                               const char *output_path)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_context jerr;
    FILE * volatile fp = NULL;
    char tmp_path[PATH_MAX];
    volatile int ret = -1;

    if (build_tmp_path(output_path, tmp_path, sizeof(tmp_path)) != 0) {
        return -1;
    }

    fp = fopen(tmp_path, "wb");
    if (fp == NULL) {
        return -1;
    }

    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    if (setjmp(jerr.jump_buffer) != 0) {
        errno = EIO;
        goto out_destroy;
    }

    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 85, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row_pointer[1];

        row_pointer[0] = (JSAMPROW)(rgb + (size_t)cinfo.next_scanline * width * 3U);
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);

    if (fsync_file_stream(fp) != 0) {
        goto out_destroy;
    }

    if (fclose(fp) != 0) {
        fp = NULL;
        goto out_destroy;
    }
    fp = NULL;

    if (rename(tmp_path, output_path) != 0) {
        goto out_destroy;
    }

    ret = 0;

out_destroy:
    jpeg_destroy_compress(&cinfo);
    if (fp != NULL) {
        fclose(fp);
    }
    if (ret != 0) {
        unlink(tmp_path);
        unlink(output_path);
    }
    return ret;
}

/*
 * write_rgb24_as_png 的作用：
 *   把连续 RGB24 缓冲编码成 PNG 文件，并保证成功返回前完成 fsync。
 *
 * 主要流程：
 *   1. 先写入同目录 .tmp 文件。
 *   2. 使用 libpng 写入 RGB 8bit 图像。
 *   3. fflush + fsync 后关闭文件，再 rename 为最终 .png 路径。
 *
 * 参数：
 *   rgb 是连续 RGB24 图像数据。
 *   width/height 是图像尺寸。
 *   output_path 是最终 PNG 文件路径。
 *
 * 返回值：
 *   成功返回 0；失败返回 -1。
 */
static int write_rgb24_as_png(const uint8_t *rgb,
                              unsigned int width,
                              unsigned int height,
                              const char *output_path)
{
    FILE * volatile fp = NULL;
    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    png_bytep * volatile rows = NULL;
    char tmp_path[PATH_MAX];
    unsigned int y;
    volatile int ret = -1;

    if (build_tmp_path(output_path, tmp_path, sizeof(tmp_path)) != 0) {
        return -1;
    }

    fp = fopen(tmp_path, "wb");
    if (fp == NULL) {
        return -1;
    }

    png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png_ptr == NULL) {
        errno = ENOMEM;
        goto out;
    }

    info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == NULL) {
        errno = ENOMEM;
        goto out;
    }

    if (setjmp(png_jmpbuf(png_ptr)) != 0) {
        errno = EIO;
        goto out;
    }

    rows = malloc((size_t)height * sizeof(*rows));
    if (rows == NULL) {
        goto out;
    }

    for (y = 0; y < height; y++) {
        rows[y] = (png_bytep)(rgb + (size_t)y * width * 3U);
    }

    png_init_io(png_ptr, fp);
    png_set_IHDR(png_ptr,
                 info_ptr,
                 width,
                 height,
                 8,
                 PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);
    png_write_image(png_ptr, rows);
    png_write_end(png_ptr, info_ptr);

    if (fsync_file_stream(fp) != 0) {
        goto out;
    }

    if (fclose(fp) != 0) {
        fp = NULL;
        goto out;
    }
    fp = NULL;

    if (rename(tmp_path, output_path) != 0) {
        goto out;
    }

    ret = 0;

out:
    if (png_ptr != NULL) {
        png_destroy_write_struct(&png_ptr, info_ptr != NULL ? &info_ptr : NULL);
    }
    free(rows);
    if (fp != NULL) {
        fclose(fp);
    }
    if (ret != 0) {
        unlink(tmp_path);
        unlink(output_path);
    }
    return ret;
}

/*
 * write_latest_frame_as_ppm 的作用：
 *   把最新摄像头矩形保存为 PPM 图片。
 *
 * 主要流程：
 *   1. 先写 P6 PPM 头，格式为 RGB24，避免引入 JPEG/PNG 编码依赖。
 *   2. 使用 copy_latest_frame_to_rgb24 取得无 ROI 框的干净 RGB24 数据。
 *   3. 写完整个 RGB 缓冲并调用 fsync，确保按钮返回成功前数据已经提交给内核块层。
 *
 * 参数：
 *   frame 是最新显示帧。
 *   output_path 是最终保存文件路径。
 *
 * 返回值：
 *   成功返回 0；失败返回 -1。
 */
static int write_latest_frame_as_ppm(const struct latest_frame *frame, const char *output_path)
{
    int fd = -1;
    char header[64];
    int header_len;
    uint8_t *rgb = NULL;
    size_t rgb_size;
    int ret = -1;

    if (!frame->has_frame || frame->fb_map == NULL) {
        errno = ENODATA;
        return -1;
    }

    rgb = copy_latest_frame_to_rgb24(frame);
    if (rgb == NULL) {
        return -1;
    }

    rgb_size = (size_t)frame->frame_width * frame->frame_height * 3U;

    fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        goto out;
    }

    header_len = snprintf(header,
                          sizeof(header),
                          "P6\n%u %u\n255\n",
                          frame->frame_width,
                          frame->frame_height);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        errno = EINVAL;
        goto out;
    }

    if (write_all(fd, header, (size_t)header_len) != 0) {
        goto out;
    }

    if (write_all(fd, rgb, rgb_size) != 0) {
        goto out;
    }

    if (fsync(fd) != 0) {
        goto out;
    }

    ret = 0;

out:
    if (fd >= 0) {
        close(fd);
    }

    if (ret != 0) {
        unlink(output_path);
    }

    free(rgb);
    return ret;
}

/*
 * write_latest_frame_as_jpeg_and_png 的作用：
 *   把同一帧摄像头画面同时保存为 JPG 和 PNG。
 *
 * 主要流程：
 *   1. 从 latest_frame 拷贝一份连续 RGB24 缓冲。
 *   2. 用同一份 RGB 数据先写 JPG，再写 PNG，确保两种格式来自同一帧。
 *   3. 任一格式失败时删除两个最终文件，避免 Qt 误以为双格式都完整。
 *
 * 参数：
 *   frame 是最新显示帧。
 *   jpg_path 是最终 JPG 路径。
 *   png_path 是最终 PNG 路径。
 *
 * 返回值：
 *   成功返回 0；失败返回 -1。
 */
static int write_latest_frame_as_jpeg_and_png(const struct latest_frame *frame,
                                              const char *jpg_path,
                                              const char *png_path)
{
    uint8_t *rgb;
    int ret = -1;

    rgb = copy_latest_frame_to_rgb24(frame);
    if (rgb == NULL) {
        return -1;
    }

    if (write_rgb24_as_jpeg(rgb, frame->frame_width, frame->frame_height, jpg_path) != 0) {
        goto out;
    }

    if (write_rgb24_as_png(rgb, frame->frame_width, frame->frame_height, png_path) != 0) {
        goto out;
    }

    ret = 0;

out:
    if (ret != 0) {
        unlink(jpg_path);
        unlink(png_path);
    }
    free(rgb);
    return ret;
}

/*
 * write_latest_frame_as_jpeg 的作用：
 *   把最新摄像头帧保存成单张 JPG，供首页“检测”按钮临时推理使用。
 *
 * 主要流程：
 *   1. 从 latest_frame 拷贝一份连续 RGB24 缓冲。
 *   2. 调用 write_rgb24_as_jpeg 写入同目录 .tmp，再 rename 为最终 JPG。
 *   3. 失败时删除最终路径，避免 Qt 读取半成品图片。
 *
 * 参数：
 *   frame 是最新显示帧。
 *   jpg_path 是最终 JPG 路径。
 *
 * 返回值：
 *   成功返回 0；失败返回 -1。
 */
static int write_latest_frame_as_jpeg(const struct latest_frame *frame,
                                      const char *jpg_path)
{
    uint8_t *rgb;
    int ret = -1;

    rgb = copy_latest_frame_to_rgb24(frame);
    if (rgb == NULL) {
        return -1;
    }

    if (write_rgb24_as_jpeg(rgb, frame->frame_width, frame->frame_height, jpg_path) != 0) {
        goto out;
    }

    ret = 0;

out:
    if (ret != 0) {
        unlink(jpg_path);
    }
    free(rgb);
    return ret;
}

/*
 * yuyv_luma_at 的作用：
 *   从 YUYV422 原始帧中读取指定像素的 Y 亮度分量。
 *
 * 主要流程：
 *   1. 按 `y * width * 2 + x * 2` 定位当前像素的 Y 字节。
 *   2. 不读取 U/V 色度，因为自动居中阶段只需要稳定的几何坐标。
 *
 * 参数：
 *   frame 是最新摄像头帧。
 *   x/y 是原始摄像头坐标。
 *
 * 返回值：
 *   返回 0~255 的亮度值；调用者必须保证坐标已经在帧范围内。
 */
static unsigned int yuyv_luma_at(const struct latest_frame *frame,
                                 unsigned int x,
                                 unsigned int y)
{
    const uint8_t *line = frame->yuyv_map + (size_t)y * frame->frame_width * 2U;

    return (unsigned int)line[x * 2U];
}

/*
 * clamp_luma_threshold 的作用：
 *   把均值加减阈值后的结果限制到 0~255，避免无符号计算下溢或越界。
 *
 * 参数：
 *   value 是可能超出亮度范围的临时整数。
 *
 * 返回值：
 *   返回可以与 Y 分量直接比较的 0~255 阈值。
 */
static unsigned int clamp_luma_threshold(int value)
{
    if (value < 0) {
        return 0U;
    }
    if (value > 255) {
        return 255U;
    }
    return (unsigned int)value;
}

/*
 * auto_locate_is_candidate_luma 的作用：
 *   判断一个像素亮度是否属于零件候选区域。
 *
 * 关键说明：
 *   金属零件在现场可能表现为亮边，也可能因为角度和阴影表现为暗边。
 *   因此首版同时接受“明显亮于背景”和“明显暗于背景”的像素，
 *   后续再通过连通域面积、边框尺寸和长宽比过滤误检。
 *
 * 参数：
 *   luma 是当前像素亮度。
 *   bright_threshold 是亮候选阈值。
 *   dark_threshold 是暗候选阈值。
 *
 * 返回值：
 *   属于候选像素返回 1；否则返回 0。
 */
static int auto_locate_is_candidate_luma(unsigned int luma,
                                         unsigned int bright_threshold,
                                         unsigned int dark_threshold)
{
    if (luma >= bright_threshold || luma <= dark_threshold) {
        return 1;
    }

    return 0;
}

/*
 * locate_part_in_yuyv_frame 的作用：
 *   在最新 YUYV 原始帧的中心 ROI 内定位传送带上的零件。
 *
 * 主要流程：
 *   1. 初始化输出结果，把 frame_id 和图像尺寸先写入 result，保证无目标时也能回传上下文。
 *   2. 只扫描水平居中的竖向搜索带，提前发现从画面上方进入的零件，同时避开左右支架干扰。
 *   3. 统计 ROI 的亮度均值、最暗值和最亮值，得到当前背景的自适应阈值。
 *   4. 对明显亮于或暗于背景的像素做四邻域连通域搜索。
 *   5. 选择面积、外接框和长宽比都合理的最佳连通域，输出中心点、bbox 和置信度。
 *
 * 参数：
 *   frame 是最新摄像头帧，必须包含原始 YUYV 指针。
 *   result 是输出定位结果，函数会完整写入该结构。
 *
 * 返回值：
 *   成功完成定位流程返回 0；内存申请失败或帧数据尺寸异常返回 -1。
 *   没找到目标不算错误，此时返回 0 且 result->has_target 为 0。
 */
static int locate_part_in_yuyv_frame(const struct latest_frame *frame,
                                     struct locate_result *result)
{
    unsigned int roi_w;
    unsigned int roi_h;
    unsigned int roi_x;
    unsigned int roi_y;
    unsigned int pixel_count;
    uint64_t luma_sum = 0U;
    unsigned int min_luma = 255U;
    unsigned int max_luma = 0U;
    unsigned int mean_luma;
    unsigned int contrast_span;
    unsigned int luma_delta;
    unsigned int bright_threshold;
    unsigned int dark_threshold;
    unsigned char *visited = NULL;
    unsigned int *queue = NULL;
    unsigned int best_score = 0U;
    unsigned int max_component_area;
    unsigned int y;
    int ret = 0;

    if (result == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(result, 0, sizeof(*result));

    if (frame != NULL) {
        result->frame_id = frame->serial;
        result->frame_width = frame->frame_width;
        result->frame_height = frame->frame_height;
    }

    if (frame == NULL || !frame->has_frame || frame->yuyv_map == NULL) {
        return 0;
    }

    if (frame->frame_width == 0U || frame->frame_height == 0U) {
        errno = EINVAL;
        return -1;
    }

    if (frame->yuyv_size < (size_t)frame->frame_width * frame->frame_height * 2U) {
        errno = EINVAL;
        return -1;
    }

    roi_w = frame->frame_width < AUTO_LOCATE_SEARCH_WIDTH ? frame->frame_width : AUTO_LOCATE_SEARCH_WIDTH;
    roi_h = frame->frame_height;
    roi_x = (frame->frame_width - roi_w) / 2U;
    roi_y = 0U;
    pixel_count = roi_w * roi_h;

    if (pixel_count == 0U) {
        errno = EINVAL;
        return -1;
    }

    for (y = 0; y < roi_h; y++) {
        unsigned int x;

        for (x = 0; x < roi_w; x++) {
            unsigned int luma = yuyv_luma_at(frame, roi_x + x, roi_y + y);

            luma_sum += luma;
            if (luma < min_luma) {
                min_luma = luma;
            }
            if (luma > max_luma) {
                max_luma = luma;
            }
        }
    }

    mean_luma = (unsigned int)(luma_sum / pixel_count);
    contrast_span = max_luma > min_luma ? max_luma - min_luma : 0U;
    if (contrast_span < AUTO_LOCATE_MIN_LUMA_DELTA) {
        return 0;
    }

    luma_delta = contrast_span / 3U;
    if (luma_delta < AUTO_LOCATE_MIN_LUMA_DELTA) {
        luma_delta = AUTO_LOCATE_MIN_LUMA_DELTA;
    }

    bright_threshold = clamp_luma_threshold((int)mean_luma + (int)luma_delta);
    dark_threshold = clamp_luma_threshold((int)mean_luma - (int)luma_delta);

    visited = calloc(pixel_count, sizeof(*visited));
    queue = malloc((size_t)pixel_count * sizeof(*queue));
    if (visited == NULL || queue == NULL) {
        errno = ENOMEM;
        ret = -1;
        goto out;
    }

    max_component_area = pixel_count / AUTO_LOCATE_MAX_COMPONENT_AREA_DIVISOR;
    if (max_component_area < AUTO_LOCATE_MIN_COMPONENT_AREA) {
        max_component_area = AUTO_LOCATE_MIN_COMPONENT_AREA;
    }

    for (y = 0; y < roi_h; y++) {
        unsigned int x;

        for (x = 0; x < roi_w; x++) {
            unsigned int start_index = y * roi_w + x;
            unsigned int start_luma;
            unsigned int head = 0U;
            unsigned int tail = 0U;
            unsigned int area = 0U;
            unsigned int contrast_sum = 0U;
            unsigned int min_x = x;
            unsigned int max_x = x;
            unsigned int min_y = y;
            unsigned int max_y = y;
            unsigned int bbox_w;
            unsigned int bbox_h;
            unsigned int bbox_area;
            unsigned int contrast_avg;
            unsigned int density;
            unsigned int score;
            unsigned int confidence;

            if (visited[start_index]) {
                continue;
            }

            start_luma = yuyv_luma_at(frame, roi_x + x, roi_y + y);
            if (!auto_locate_is_candidate_luma(start_luma, bright_threshold, dark_threshold)) {
                visited[start_index] = 1U;
                continue;
            }

            visited[start_index] = 1U;
            queue[tail++] = start_index;

            while (head < tail) {
                unsigned int index = queue[head++];
                unsigned int local_x = index % roi_w;
                unsigned int local_y = index / roi_w;
                unsigned int luma = yuyv_luma_at(frame, roi_x + local_x, roi_y + local_y);
                static const int neighbor_dx[4] = { -1, 1, 0, 0 };
                static const int neighbor_dy[4] = { 0, 0, -1, 1 };
                unsigned int neighbor_index;
                unsigned int i;

                area++;
                contrast_sum += (unsigned int)abs((int)luma - (int)mean_luma);

                if (local_x < min_x) {
                    min_x = local_x;
                }
                if (local_x > max_x) {
                    max_x = local_x;
                }
                if (local_y < min_y) {
                    min_y = local_y;
                }
                if (local_y > max_y) {
                    max_y = local_y;
                }

                for (i = 0; i < 4U; i++) {
                    int next_x = (int)local_x + neighbor_dx[i];
                    int next_y = (int)local_y + neighbor_dy[i];
                    unsigned int next_luma;

                    if (next_x < 0 || next_y < 0 ||
                        next_x >= (int)roi_w || next_y >= (int)roi_h) {
                        continue;
                    }

                    neighbor_index = (unsigned int)next_y * roi_w + (unsigned int)next_x;
                    if (visited[neighbor_index]) {
                        continue;
                    }

                    next_luma = yuyv_luma_at(frame,
                                             roi_x + (unsigned int)next_x,
                                             roi_y + (unsigned int)next_y);
                    if (!auto_locate_is_candidate_luma(next_luma,
                                                       bright_threshold,
                                                       dark_threshold)) {
                        visited[neighbor_index] = 1U;
                        continue;
                    }

                    visited[neighbor_index] = 1U;
                    queue[tail++] = neighbor_index;
                }
            }

            bbox_w = max_x - min_x + 1U;
            bbox_h = max_y - min_y + 1U;
            bbox_area = bbox_w * bbox_h;

            if (area < AUTO_LOCATE_MIN_COMPONENT_AREA || area > max_component_area) {
                continue;
            }

            if (bbox_w < AUTO_LOCATE_MIN_BBOX_SIDE ||
                bbox_h < AUTO_LOCATE_MIN_BBOX_SIDE ||
                bbox_w > AUTO_LOCATE_MAX_BBOX_SIDE ||
                bbox_h > AUTO_LOCATE_MAX_BBOX_SIDE) {
                continue;
            }

            if (bbox_w * 100U < bbox_h * 25U ||
                bbox_h * 100U < bbox_w * 25U) {
                continue;
            }

            contrast_avg = contrast_sum / area;
            density = bbox_area > 0U ? (area * 100U) / bbox_area : 0U;
            score = area + bbox_area / 4U + contrast_avg * 8U;

            if (score <= best_score) {
                continue;
            }

            confidence = contrast_avg * 2U + density / 2U + area / 20U;
            if (confidence > 100U) {
                confidence = 100U;
            }

            best_score = score;
            result->has_target = 1;
            result->center_x = (int)(roi_x + (min_x + max_x) / 2U);
            result->center_y = (int)(roi_y + (min_y + max_y) / 2U);
            result->bbox_x = (int)(roi_x + min_x);
            result->bbox_y = (int)(roi_y + min_y);
            result->bbox_w = (int)bbox_w;
            result->bbox_h = (int)bbox_h;
            result->confidence = confidence;
        }
    }

out:
    free(queue);
    free(visited);
    return ret;
}

/*
 * init_control_server 的作用：
 *   创建 Unix domain socket 监听端点，供 Qt UI 请求保存当前帧。
 *
 * 参数：
 *   server 是输出控制服务结构。
 *   socket_path 是文件系统 socket 路径。
 *
 * 返回值：
 *   成功返回 0；失败返回 -1。
 */
static int init_control_server(struct control_server *server, const char *socket_path)
{
    struct sockaddr_un addr;
    size_t socket_len;
    int flags;

    memset(server, 0, sizeof(*server));
    server->fd = -1;

    if (socket_path == NULL || socket_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    socket_len = strlen(socket_path);
    if (socket_len >= sizeof(server->socket_path) ||
        socket_len >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(server->socket_path, socket_path, socket_len + 1U);

    server->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server->fd < 0) {
        perror("创建控制 socket 失败");
        return -1;
    }

    flags = fcntl(server->fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(server->fd, F_SETFL, flags | O_NONBLOCK);
    }

    unlink(server->socket_path);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, server->socket_path, socket_len + 1U);

    if (bind(server->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("绑定控制 socket 失败");
        close(server->fd);
        server->fd = -1;
        return -1;
    }

    if (listen(server->fd, 4) != 0) {
        perror("监听控制 socket 失败");
        close(server->fd);
        unlink(server->socket_path);
        server->fd = -1;
        return -1;
    }

    printf("control socket: %s\n", server->socket_path);
    return 0;
}

/*
 * close_control_server 的作用：
 *   关闭监听 socket 并删除 socket 文件，避免下次启动 bind 失败。
 *
 * 参数：
 *   server 是控制服务结构。
 *
 * 返回值：
 *   无返回值。
 */
static void close_control_server(struct control_server *server)
{
    if (server->fd >= 0) {
        close(server->fd);
        server->fd = -1;
    }

    if (server->socket_path[0] != '\0') {
        unlink(server->socket_path);
        server->socket_path[0] = '\0';
    }
}

/*
 * send_control_reply 的作用：
 *   向 Qt 客户端返回一行结果文本。
 *
 * 参数：
 *   client_fd 是 accept 得到的连接。
 *   prefix 是 OK 或 ERR。
 *   detail 是结果路径或错误原因。
 *
 * 返回值：
 *   无返回值；失败只影响本次按钮反馈。
 */
static void send_control_reply(int client_fd, const char *prefix, const char *detail)
{
    char reply[PATH_MAX * 2 + 128];
    int len;

    len = snprintf(reply, sizeof(reply), "%s %s\n", prefix, detail ? detail : "");
    if (len < 0) {
        return;
    }
    if ((size_t)len >= sizeof(reply)) {
        len = (int)sizeof(reply) - 1;
    }
    if (len > 0) {
        write_all(client_fd, reply, (size_t)len);
    }
}

/*
 * set_kms_plane_visible 的作用：
 *   在不退出 overlay 进程的前提下隐藏或恢复视频 plane。
 *
 * 主要流程：
 *   1. 如果当前不是 plane 显示模式，则返回错误，避免误操作 primary CRTC。
 *   2. visible 为 0 时把 plane 绑定到 fb=0/crtc=0，相当于关闭实时视频层。
 *   3. visible 为 1 时重新把原 framebuffer 放回原目标矩形，恢复实时画面。
 *
 * 参数：
 *   kms 是 DRM/KMS 显示资源。
 *   visible 为非 0 表示显示，为 0 表示隐藏。
 *   error_text 用于返回失败原因，长度由 error_len 指定。
 *
 * 返回值：
 *   成功返回 0；失败返回 -1。
 */
static int set_kms_plane_visible(struct kms_device *kms,
                                 int visible,
                                 char *error_text,
                                 size_t error_len)
{
    int ret;

    if (kms == NULL || !kms->use_plane || kms->plane_id == 0U || kms->fd < 0) {
        snprintf(error_text, error_len, "当前不是 overlay plane 模式");
        return -1;
    }

    if (visible) {
        ret = drmModeSetPlane(kms->fd,
                              kms->plane_id,
                              kms->crtc_id,
                              kms->fb_id,
                              0,
                              kms->dst_x,
                              kms->dst_y,
                              kms->dst_w,
                              kms->dst_h,
                              0,
                              0,
                              kms->fb_width << 16,
                              kms->fb_height << 16);
    } else {
        ret = drmModeSetPlane(kms->fd,
                              kms->plane_id,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0);
    }

    if (ret != 0) {
        snprintf(error_text, error_len, "设置视频层可见性失败: %s", strerror(errno));
        return -1;
    }

    kms->plane_visible = visible ? 1 : 0;
    return 0;
}

/*
 * handle_visible_command 的作用：
 *   响应 Qt 历史页的 VISIBLE 0/1 命令，控制实时视频层是否遮挡历史图片。
 *
 * 参数：
 *   client_fd 是 Qt 客户端连接。
 *   frame 是最新显示帧，其中保存了 kms 指针。
 *   value 是命令参数，允许 0、1、hide、show。
 *
 * 返回值：
 *   无返回值；通过 socket 回复 OK/ERR。
 */
static void handle_visible_command(int client_fd,
                                   const struct latest_frame *frame,
                                   const char *value)
{
    char error_text[256];
    int visible;

    if (value == NULL || value[0] == '\0') {
        send_control_reply(client_fd, "ERR", "VISIBLE 缺少 0/1 参数");
        return;
    }

    if (strcmp(value, "1") == 0 || strcmp(value, "show") == 0) {
        visible = 1;
    } else if (strcmp(value, "0") == 0 || strcmp(value, "hide") == 0) {
        visible = 0;
    } else {
        send_control_reply(client_fd, "ERR", "VISIBLE 参数只能是 0/1");
        return;
    }

    if (set_kms_plane_visible(frame ? frame->kms : NULL,
                              visible,
                              error_text,
                              sizeof(error_text)) != 0) {
        send_control_reply(client_fd, "ERR", error_text);
        return;
    }

    send_control_reply(client_fd,
                       "OK",
                       visible ? "视频层已显示" : "视频层已隐藏");
}

/*
 * handle_locate_command 的作用：
 *   响应 Qt 自动流程的 LOCATE 命令，只计算当前帧零件坐标，不保存图片、不运行模型。
 *
 * 主要流程：
 *   1. 调用 locate_part_in_yuyv_frame 从原始 YUYV 帧中提取零件 bbox 和中心点。
 *   2. 把结果整理成 key=value 文本，保持与 STATUS/SAVE_DETECT 同一条 socket 通道。
 *   3. 无目标时仍返回 OK，并把 has_target 置 0，让 Qt 可以按“暂时未入画”处理。
 *
 * 参数：
 *   client_fd 是 Qt 客户端连接。
 *   frame 是最新显示帧。
 *
 * 返回值：
 *   无返回值；通过 socket 回复 `OK LOCATE ...` 或 `ERR ...`。
 */
static void handle_locate_command(int client_fd, const struct latest_frame *frame)
{
    struct locate_result result;
    char detail[320];
    int len;

    if (locate_part_in_yuyv_frame(frame, &result) != 0) {
        char error_text[160];

        snprintf(error_text, sizeof(error_text), "LOCATE 定位失败: %s", strerror(errno));
        send_control_reply(client_fd, "ERR", error_text);
        return;
    }

    len = snprintf(detail,
                   sizeof(detail),
                   "LOCATE has_target=%d frame_id=%u width=%u height=%u "
                   "center_x=%d center_y=%d bbox_x=%d bbox_y=%d "
                   "bbox_w=%d bbox_h=%d confidence=%u",
                   result.has_target,
                   result.frame_id,
                   result.frame_width,
                   result.frame_height,
                   result.center_x,
                   result.center_y,
                   result.bbox_x,
                   result.bbox_y,
                   result.bbox_w,
                   result.bbox_h,
                   result.confidence);
    if (len < 0 || (size_t)len >= sizeof(detail)) {
        send_control_reply(client_fd, "ERR", "LOCATE 回复过长");
        return;
    }

    send_control_reply(client_fd, "OK", detail);
}

/*
 * handle_status_command 的作用：
 *   把 overlay 进程当前掌握的摄像头和视频层状态返回给 Qt。
 *
 * 主要流程：
 *   1. 只读取 latest_frame 里的缓存字段，不重新访问 V4L2 或 DRM，避免状态查询拖慢采集主循环。
 *   2. has_frame 表示是否已经成功取到至少一帧真实摄像头数据。
 *   3. serial 表示最新帧序号，Qt 可以用它判断画面是否还在更新。
 *   4. visible 表示 KMS overlay plane 当前是否处于显示状态。
 *
 * 参数：
 *   client_fd 是 Qt 客户端连接。
 *   frame 是最新显示帧。
 *
 * 返回值：
 *   无返回值；通过 socket 回复 `OK STATUS ...`。
 */
static void handle_status_command(int client_fd, const struct latest_frame *frame)
{
    char detail[256];
    const int has_frame = (frame != NULL && frame->has_frame) ? 1 : 0;
    const unsigned int serial = frame != NULL ? frame->serial : 0U;
    const unsigned int width = frame != NULL ? frame->frame_width : 0U;
    const unsigned int height = frame != NULL ? frame->frame_height : 0U;
    const int visible = (frame != NULL && frame->kms != NULL) ? frame->kms->plane_visible : 0;

    snprintf(detail,
             sizeof(detail),
             "STATUS has_frame=%d serial=%u visible=%d width=%u height=%u",
             has_frame,
             serial,
             visible,
             width,
             height);
    send_control_reply(client_fd, "OK", detail);
}

/*
 * handle_save_command 的作用：
 *   执行 SAVE 请求，把当前显示帧保存到 SD 卡目录。
 *
 * 参数：
 *   client_fd 是 Qt 客户端连接。
 *   frame 是最新显示帧。
 *   output_dir 是 Qt 请求的保存目录。
 *
 * 返回值：
 *   无返回值；通过 socket 回复 OK/ERR。
 */
static void handle_save_command(int client_fd,
                                const struct latest_frame *frame,
                                const char *output_dir)
{
    char base_path[PATH_MAX];
    char path[PATH_MAX];

    if (output_dir == NULL || output_dir[0] == '\0') {
        send_control_reply(client_fd, "ERR", "保存目录为空");
        return;
    }

    if (!path_under_sdcard(output_dir)) {
        send_control_reply(client_fd, "ERR", "保存目录不在 /mnt/sdcard 下");
        return;
    }

    if (!mount_point_is_mounted(DEFAULT_SDCARD_MOUNT)) {
        send_control_reply(client_fd, "ERR", "/mnt/sdcard 未挂载");
        return;
    }

    if (mkdir_p(output_dir) != 0) {
        char detail[256];

        snprintf(detail, sizeof(detail), "创建目录失败: %s", strerror(errno));
        send_control_reply(client_fd, "ERR", detail);
        return;
    }

    if (build_snapshot_base_path(output_dir, frame, base_path, sizeof(base_path)) != 0 ||
        append_path_suffix(base_path, ".ppm", path, sizeof(path)) != 0) {
        send_control_reply(client_fd, "ERR", "生成图片路径失败");
        return;
    }

    if (write_latest_frame_as_ppm(frame, path) != 0) {
        char detail[256];

        snprintf(detail, sizeof(detail), "保存图片失败: %s", strerror(errno));
        send_control_reply(client_fd, "ERR", detail);
        return;
    }

    send_control_reply(client_fd, "OK", path);
}

/*
 * handle_save_dual_command 的作用：
 *   执行 SAVE_DUAL 请求，把当前显示帧保存为 JPG 和 PNG 两种浏览器可预览格式。
 *
 * 参数：
 *   client_fd 是 Qt 客户端连接。
 *   frame 是最新显示帧。
 *   output_dir 是 Qt 请求的保存目录。
 *
 * 返回值：
 *   无返回值；成功返回 "OK JPG <path> PNG <path>"，失败返回 "ERR <原因>"。
 */
static void handle_save_dual_command(int client_fd,
                                     const struct latest_frame *frame,
                                     const char *output_dir)
{
    char base_path[PATH_MAX];
    char jpg_path[PATH_MAX];
    char png_path[PATH_MAX];
    char reply[PATH_MAX * 2 + 32];
    int reply_len;

    if (output_dir == NULL || output_dir[0] == '\0') {
        send_control_reply(client_fd, "ERR", "保存目录为空");
        return;
    }

    if (!path_under_sdcard(output_dir)) {
        send_control_reply(client_fd, "ERR", "保存目录不在 /mnt/sdcard 下");
        return;
    }

    if (!mount_point_is_mounted(DEFAULT_SDCARD_MOUNT)) {
        send_control_reply(client_fd, "ERR", "/mnt/sdcard 未挂载");
        return;
    }

    if (mkdir_p(output_dir) != 0) {
        char detail[256];

        snprintf(detail, sizeof(detail), "创建目录失败: %s", strerror(errno));
        send_control_reply(client_fd, "ERR", detail);
        return;
    }

    if (build_snapshot_base_path(output_dir, frame, base_path, sizeof(base_path)) != 0 ||
        append_path_suffix(base_path, ".jpg", jpg_path, sizeof(jpg_path)) != 0 ||
        append_path_suffix(base_path, ".png", png_path, sizeof(png_path)) != 0) {
        send_control_reply(client_fd, "ERR", "生成图片路径失败");
        return;
    }

    if (write_latest_frame_as_jpeg_and_png(frame, jpg_path, png_path) != 0) {
        char detail[256];

        snprintf(detail, sizeof(detail), "保存 JPG/PNG 失败: %s", strerror(errno));
        send_control_reply(client_fd, "ERR", detail);
        return;
    }

    reply_len = snprintf(reply, sizeof(reply), "JPG %s PNG %s", jpg_path, png_path);
    if (reply_len < 0 || (size_t)reply_len >= sizeof(reply)) {
        send_control_reply(client_fd, "ERR", "保存结果路径过长");
        return;
    }

    send_control_reply(client_fd, "OK", reply);
}

/*
 * handle_save_detect_command 的作用：
 *   执行 SAVE_DETECT 请求，把当前显示帧保存成检测专用 JPG。
 *
 * 关键说明：
 *   SAVE_DETECT 只允许写入 /tmp/qt-defect-detect 或 /mnt/sdcard/images。
 *   正式 Qt 检测会写 SD 卡历史目录；SSH 临时调试仍可写 /tmp。
 *
 * 参数：
 *   client_fd 是 Qt 客户端连接。
 *   frame 是最新显示帧。
 *   output_dir 是 Qt 请求的临时保存目录。
 *
 * 返回值：
 *   无返回值；成功返回 "OK DETECT_JPG <path>"，失败返回 "ERR <原因>"。
 */
static void handle_save_detect_command(int client_fd,
                                       const struct latest_frame *frame,
                                       const char *output_dir)
{
    char base_path[PATH_MAX];
    char jpg_path[PATH_MAX];
    char reply[PATH_MAX + 32];
    int reply_len;

    if (output_dir == NULL || output_dir[0] == '\0') {
        send_control_reply(client_fd, "ERR", "检测保存目录为空");
        return;
    }

    if (!path_under_detect_output(output_dir)) {
        send_control_reply(client_fd, "ERR", "检测保存目录必须在 /tmp/qt-defect-detect 或 /mnt/sdcard/images 下");
        return;
    }

    if (mkdir_p(output_dir) != 0) {
        char detail[256];

        snprintf(detail, sizeof(detail), "创建检测目录失败: %s", strerror(errno));
        send_control_reply(client_fd, "ERR", detail);
        return;
    }

    if (build_snapshot_base_path(output_dir, frame, base_path, sizeof(base_path)) != 0 ||
        append_path_suffix(base_path, ".jpg", jpg_path, sizeof(jpg_path)) != 0) {
        send_control_reply(client_fd, "ERR", "生成检测图片路径失败");
        return;
    }

    if (write_latest_frame_as_jpeg(frame, jpg_path) != 0) {
        char detail[256];

        snprintf(detail, sizeof(detail), "保存检测 JPG 失败: %s", strerror(errno));
        send_control_reply(client_fd, "ERR", detail);
        return;
    }

    reply_len = snprintf(reply, sizeof(reply), "DETECT_JPG %s", jpg_path);
    if (reply_len < 0 || (size_t)reply_len >= sizeof(reply)) {
        send_control_reply(client_fd, "ERR", "检测结果路径过长");
        return;
    }

    send_control_reply(client_fd, "OK", reply);
}

/*
 * service_control_client 的作用：
 *   读取并执行一个 Qt 控制命令。
 *
 * 支持命令：
 *   SAVE /mnt/sdcard/images
 *   SAVE_DUAL /mnt/sdcard/images
 *   SAVE_DETECT /tmp/qt-defect-detect
 *   SAVE_DETECT /mnt/sdcard/images
 *   VISIBLE 0
 *   VISIBLE 1
 *   STATUS
 *   LOCATE
 *
 * 参数：
 *   client_fd 是客户端连接。
 *   frame 是最新显示帧。
 *
 * 返回值：
 *   无返回值。
 */
static void service_control_client(int client_fd, const struct latest_frame *frame)
{
    char command[PATH_MAX + 32];
    ssize_t nread;

    nread = read(client_fd, command, sizeof(command) - 1U);
    if (nread < 0) {
        send_control_reply(client_fd, "ERR", strerror(errno));
        return;
    }

    if (nread == 0) {
        send_control_reply(client_fd, "ERR", "空命令");
        return;
    }

    command[nread] = '\0';
    command[strcspn(command, "\r\n")] = '\0';

    if (strncmp(command, "SAVE_DUAL ", strlen("SAVE_DUAL ")) == 0) {
        handle_save_dual_command(client_fd, frame, command + strlen("SAVE_DUAL "));
        return;
    }

    if (strncmp(command, "SAVE_DETECT ", strlen("SAVE_DETECT ")) == 0) {
        handle_save_detect_command(client_fd, frame, command + strlen("SAVE_DETECT "));
        return;
    }

    if (strncmp(command, "VISIBLE ", strlen("VISIBLE ")) == 0) {
        handle_visible_command(client_fd, frame, command + strlen("VISIBLE "));
        return;
    }

    if (strcmp(command, "STATUS") == 0) {
        handle_status_command(client_fd, frame);
        return;
    }

    if (strcmp(command, "LOCATE") == 0) {
        handle_locate_command(client_fd, frame);
        return;
    }

    if (strncmp(command, "SAVE ", 5U) == 0) {
        handle_save_command(client_fd, frame, command + 5U);
        return;
    }

    send_control_reply(client_fd, "ERR", "未知命令");
}

/*
 * poll_control_server 的作用：
 *   在视频主循环中非阻塞处理所有待处理的 Qt 保存请求。
 *
 * 参数：
 *   server 是控制服务。
 *   frame 是最新显示帧。
 *
 * 返回值：
 *   无返回值；单个客户端错误不会影响视频显示主循环。
 */
static void poll_control_server(struct control_server *server, const struct latest_frame *frame)
{
    while (server->fd >= 0) {
        int client_fd = accept(server->fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }
            perror("接收控制 socket 连接失败");
            return;
        }

        service_control_client(client_fd, frame);
        close(client_fd);
    }
}

/*
 * open_camera 的作用：
 *   打开 V4L2 摄像头并验证它支持 streaming mmap。
 *
 * 参数：
 *   device 是摄像头节点路径。
 *   cam 是输出结构。
 *
 * 返回值：
 *   成功返回 0；失败返回 -1。
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

    if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0 ||
        (cap.capabilities & V4L2_CAP_STREAMING) == 0) {
        fprintf(stderr, "摄像头不支持 VIDEO_CAPTURE 或 STREAMING\n");
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    printf("camera: %s, driver=%s, card=%s\n", device, cap.driver, cap.card);
    return 0;
}

/*
 * setup_camera_format 的作用：
 *   请求摄像头输出指定尺寸的 YUYV 帧。
 *
 * 参数：
 *   cam 是已打开摄像头。
 *   width/height 是期望尺寸。
 *
 * 返回值：
 *   成功返回 0；驱动拒绝 YUYV 或返回其它格式时返回 -1。
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
        perror("设置摄像头格式失败");
        return -1;
    }

    cam->width = fmt.fmt.pix.width;
    cam->height = fmt.fmt.pix.height;
    cam->pixelformat = fmt.fmt.pix.pixelformat;

    printf("camera format: %ux%u %.4s bytesperline=%u\n",
           cam->width,
           cam->height,
           (const char *)&cam->pixelformat,
           fmt.fmt.pix.bytesperline);

    if (cam->pixelformat != V4L2_PIX_FMT_YUYV) {
        fprintf(stderr, "摄像头最终格式不是 YUYV，无法继续\n");
        return -1;
    }

    return 0;
}

/*
 * set_camera_fps 的作用：
 *   请求 UVC 摄像头输出指定帧率。
 *
 * 参数：
 *   cam 是已打开并设置格式的摄像头。
 *   fps 是目标帧率。
 *
 * 返回值：
 *   成功返回 0；失败返回 -1。
 */
static int set_camera_fps(struct camera_device *cam, unsigned int fps)
{
    struct v4l2_streamparm parm;

    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (xioctl(cam->fd, VIDIOC_G_PARM, &parm) < 0) {
        perror("读取摄像头帧率失败");
        return -1;
    }

    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps;

    if (xioctl(cam->fd, VIDIOC_S_PARM, &parm) < 0) {
        perror("设置摄像头帧率失败");
        return -1;
    }

    printf("camera fps: requested=%u actual=%u/%u\n",
           fps,
           parm.parm.capture.timeperframe.denominator,
           parm.parm.capture.timeperframe.numerator);
    return 0;
}

/*
 * init_camera_mmap 的作用：
 *   申请并映射 V4L2 mmap 缓冲区。
 *
 * 参数：
 *   cam 是已设置格式的摄像头。
 *
 * 返回值：
 *   成功返回 0；失败返回 -1。
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
        perror("申请摄像头缓冲区失败");
        return -1;
    }

    if (req.count < 2U) {
        fprintf(stderr, "摄像头缓冲区数量过少: %u\n", req.count);
        return -1;
    }

    cam->buffer_count = req.count > CAMERA_BUFFER_COUNT ? CAMERA_BUFFER_COUNT : req.count;

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
        cam->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, cam->fd, buf.m.offset);
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
 *   把所有 mmap 缓冲区入队并启动 V4L2 流。
 *
 * 参数：
 *   cam 是已完成 mmap 的摄像头。
 *
 * 返回值：
 *   成功返回 0；失败返回 -1。
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
        perror("启动摄像头流失败");
        return -1;
    }

    return 0;
}

/*
 * stop_camera_stream 的作用：
 *   停止 V4L2 采集流。
 *
 * 参数：
 *   cam 是摄像头结构。
 *
 * 返回值：
 *   无返回值；失败只打印错误，因为后续仍需释放资源。
 */
static void stop_camera_stream(struct camera_device *cam)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (cam->fd >= 0 && xioctl(cam->fd, VIDIOC_STREAMOFF, &type) < 0) {
        perror("停止摄像头流失败");
    }
}

/*
 * close_camera 的作用：
 *   释放摄像头 mmap 缓冲区并关闭设备。
 *
 * 参数：
 *   cam 是摄像头结构。
 *
 * 返回值：
 *   无返回值，可在部分初始化失败后调用。
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
 * find_connected_connector 的作用：
 *   在 DRM 资源中寻找第一个已连接且有模式的 connector。
 *
 * 参数：
 *   fd 是 DRM 设备文件描述符。
 *   res 是 drmModeGetResources 返回的资源对象。
 *
 * 返回值：
 *   成功返回 connector 指针，调用者负责 drmModeFreeConnector；
 *   找不到时返回 NULL。
 */
static drmModeConnector *find_connected_connector(int fd, drmModeRes *res)
{
    int i;

    for (i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
        if (conn == NULL) {
            continue;
        }

        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            return conn;
        }

        drmModeFreeConnector(conn);
    }

    return NULL;
}

/*
 * choose_crtc_id 的作用：
 *   为 connector 选择可用 CRTC。
 *
 * 关键逻辑：
 *   优先使用 connector 当前 encoder 绑定的 CRTC，避免扰动已有显示拓扑。
 *   如果没有当前 encoder，则退回资源列表中的第一个 CRTC。
 *
 * 参数：
 *   fd 是 DRM 设备文件描述符。
 *   res 是 DRM 资源。
 *   conn 是已连接 connector。
 *
 * 返回值：
 *   成功返回 crtc id；失败返回 0。
 */
static uint32_t choose_crtc_id(int fd, drmModeRes *res, drmModeConnector *conn)
{
    drmModeEncoder *enc = NULL;
    uint32_t crtc_id = 0;

    if (conn->encoder_id != 0) {
        enc = drmModeGetEncoder(fd, conn->encoder_id);
        if (enc != NULL) {
            crtc_id = enc->crtc_id;
            drmModeFreeEncoder(enc);
        }
    }

    if (crtc_id == 0 && res->count_crtcs > 0) {
        crtc_id = res->crtcs[0];
    }

    return crtc_id;
}

/*
 * create_dumb_fb 的作用：
 *   创建并 mmap 一个 DRM dumb framebuffer。
 *
 * 参数：
 *   kms 是已打开 DRM 设备并选好 mode 的结构。
 *   output_format 指定 framebuffer 像素格式。
 *
 * 返回值：
 *   成功返回 0；任一步失败返回 -1。
 */
static int create_dumb_fb(struct kms_device *kms, enum output_format output_format)
{
    struct drm_mode_create_dumb create_req;
    struct drm_mode_map_dumb map_req;
    uint32_t handles[4] = {0};
    uint32_t pitches[4] = {0};
    uint32_t offsets[4] = {0};
    uint32_t drm_format = DRM_FORMAT_XRGB8888;
    uint32_t depth = 24;
    uint32_t bpp = 32;
    int ret;

    if (output_format == OUTPUT_FORMAT_RGB565) {
        drm_format = DRM_FORMAT_RGB565;
        depth = 16;
        bpp = 16;
    } else if (output_format == OUTPUT_FORMAT_ARGB8888) {
        drm_format = DRM_FORMAT_ARGB8888;
        depth = 32;
        bpp = 32;
    }

    memset(&create_req, 0, sizeof(create_req));
    create_req.width = kms->fb_width;
    create_req.height = kms->fb_height;
    create_req.bpp = bpp;

    if (xioctl(kms->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0) {
        perror("创建 DRM dumb buffer 失败");
        return -1;
    }

    kms->dumb_handle = create_req.handle;
    kms->pitch = create_req.pitch;
    kms->size = create_req.size;

    handles[0] = kms->dumb_handle;
    pitches[0] = kms->pitch;
    offsets[0] = 0;

    ret = drmModeAddFB2(kms->fd,
                        kms->fb_width,
                        kms->fb_height,
                        drm_format,
                        handles,
                        pitches,
                        offsets,
                        &kms->fb_id,
                        0);
    if (ret != 0) {
        ret = drmModeAddFB(kms->fd,
                           kms->fb_width,
                           kms->fb_height,
                           depth,
                           bpp,
                           kms->pitch,
                           kms->dumb_handle,
                           &kms->fb_id);
        if (ret != 0) {
            perror("创建 DRM framebuffer 失败");
            return -1;
        }
    }

    memset(&map_req, 0, sizeof(map_req));
    map_req.handle = kms->dumb_handle;

    if (xioctl(kms->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
        perror("映射 DRM dumb buffer offset 失败");
        return -1;
    }

    kms->fb_map = mmap(NULL, kms->size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, kms->fd, map_req.offset);
    if (kms->fb_map == MAP_FAILED) {
        perror("mmap DRM dumb buffer 失败");
        kms->fb_map = NULL;
        return -1;
    }

    /*
     * 新建 dumb framebuffer 后先清成全 0。
     * 对 XRGB/RGB565 来说这是黑色；对 ARGB8888 来说 alpha 也是 0，
     * 即使某些启动瞬间 plane 被硬件短暂打开，也不会先露出摄像头首帧或未初始化数据。
     */
    memset(kms->fb_map, 0, kms->size);
    return 0;
}

/*
 * open_kms 的作用：
 *   打开 DRM 设备，选择 connector/CRTC/mode，并创建显示 framebuffer。
 *
 * 参数：
 *   device 是 DRM 节点路径。
 *   kms 是输出结构。
 *   output_format 指定 dumb framebuffer 的像素格式。
 *   initial_visible 表示 overlay plane 初始化后是否立即显示，0 表示先保持隐藏。
 *
 * 返回值：
 *   成功返回 0；失败返回 -1。
 */
static int open_kms(const char *device,
                    struct kms_device *kms,
                    enum output_format output_format,
                    int use_plane,
                    uint32_t plane_id,
                    unsigned int fb_width,
                    unsigned int fb_height,
                    int32_t dst_x,
                    int32_t dst_y,
                    uint32_t dst_w,
                    uint32_t dst_h,
                    int initial_visible)
{
    drmModeRes *res = NULL;
    drmModeConnector *conn = NULL;
    int i;

    memset(kms, 0, sizeof(*kms));
    kms->fd = -1;

    kms->fd = open(device, O_RDWR | O_CLOEXEC);
    if (kms->fd < 0) {
        perror("打开 DRM 设备失败");
        return -1;
    }

    res = drmModeGetResources(kms->fd);
    if (res == NULL) {
        perror("读取 DRM 资源失败");
        close(kms->fd);
        kms->fd = -1;
        return -1;
    }

    conn = find_connected_connector(kms->fd, res);
    if (conn == NULL) {
        fprintf(stderr, "没有找到已连接的 DRM connector\n");
        drmModeFreeResources(res);
        close(kms->fd);
        kms->fd = -1;
        return -1;
    }

    kms->connector_id = conn->connector_id;
    kms->crtc_id = choose_crtc_id(kms->fd, res, conn);
    if (kms->crtc_id == 0) {
        fprintf(stderr, "没有找到可用 CRTC\n");
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close(kms->fd);
        kms->fd = -1;
        return -1;
    }

    kms->mode = conn->modes[0];
    for (i = 0; i < conn->count_modes; i++) {
        if ((conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) != 0) {
            kms->mode = conn->modes[i];
            break;
        }
    }

    kms->use_plane = use_plane;
    kms->plane_id = plane_id;
    kms->fb_width = fb_width == 0U ? kms->mode.hdisplay : fb_width;
    kms->fb_height = fb_height == 0U ? kms->mode.vdisplay : fb_height;

    if (kms->fb_width == 0U || kms->fb_height == 0U) {
        fprintf(stderr, "KMS framebuffer 尺寸非法: %ux%u\n", kms->fb_width, kms->fb_height);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close(kms->fd);
        kms->fd = -1;
        return -1;
    }

    if (!use_plane) {
        kms->old_crtc = drmModeGetCrtc(kms->fd, kms->crtc_id);
    }

    if (create_dumb_fb(kms, output_format) != 0) {
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close(kms->fd);
        kms->fd = -1;
        return -1;
    }

    if (use_plane) {
        if (plane_id == 0U) {
            fprintf(stderr, "plane 模式需要有效 plane-id\n");
            drmModeFreeConnector(conn);
            drmModeFreeResources(res);
            return -1;
        }

        if (dst_w == 0U) {
            dst_w = kms->fb_width;
        }
        if (dst_h == 0U) {
            dst_h = kms->fb_height;
        }
        if (dst_x < 0) {
            dst_x = (int32_t)((kms->mode.hdisplay - dst_w) / 2U);
        }
        if (dst_y < 0) {
            dst_y = (int32_t)((kms->mode.vdisplay - dst_h) / 2U);
        }

        kms->dst_x = dst_x;
        kms->dst_y = dst_y;
        kms->dst_w = dst_w;
        kms->dst_h = dst_h;
        kms->plane_visible = initial_visible ? 1 : 0;

        if (initial_visible) {
            /*
             * 调试场景下保持原行为：overlay 初始化完成后立即把 framebuffer 放到指定 plane，
             * 这样单独运行 uvc_kms_overlay 时可以马上观察摄像头画面。
             */
            if (drmModeSetPlane(kms->fd,
                                plane_id,
                                kms->crtc_id,
                                kms->fb_id,
                                0,
                                dst_x,
                                dst_y,
                                dst_w,
                                dst_h,
                                0,
                                0,
                                kms->fb_width << 16,
                                kms->fb_height << 16) != 0) {
                perror("设置 DRM plane 失败");
                drmModeFreeConnector(conn);
                drmModeFreeResources(res);
                return -1;
            }
        } else {
            /*
             * 开机启动场景下先解除 plane 绑定：
             *   overlay 仍然会采集摄像头并写入 framebuffer，也会建立控制 socket；
             *   但视频层不会抢在 Qt 启动画面前盖住屏幕。
             * 如果 plane 本来就是关闭状态，部分驱动可能返回失败，这里只打印警告，不阻止采集链路继续运行。
             */
            if (drmModeSetPlane(kms->fd,
                                plane_id,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0) != 0) {
                fprintf(stderr, "警告：初始隐藏 DRM plane 失败: %s\n", strerror(errno));
            }
        }
    } else {
        kms->dst_x = 0;
        kms->dst_y = 0;
        kms->dst_w = kms->fb_width;
        kms->dst_h = kms->fb_height;
        kms->plane_visible = 1;

        if (drmModeSetCrtc(kms->fd,
                           kms->crtc_id,
                           kms->fb_id,
                           0,
                           0,
                           &kms->connector_id,
                           1,
                           &kms->mode) != 0) {
            perror("设置 DRM CRTC 失败");
            drmModeFreeConnector(conn);
            drmModeFreeResources(res);
            return -1;
        }
    }

    printf("kms: %s connector=%u crtc=%u mode=%ux%u fb-size=%ux%u pitch=%u fb=%u format=%s plane=%s%u initial-visible=%d\n",
           device,
           kms->connector_id,
           kms->crtc_id,
           kms->mode.hdisplay,
           kms->mode.vdisplay,
           kms->fb_width,
           kms->fb_height,
           kms->pitch,
           kms->fb_id,
           output_format_name(output_format),
           use_plane ? "" : "none/",
           use_plane ? plane_id : 0U,
           use_plane ? kms->plane_visible : 1);

    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return 0;
}

/*
 * close_kms 的作用：
 *   尽量恢复旧 CRTC，并释放 DRM framebuffer 和 dumb buffer。
 *
 * 参数：
 *   kms 是 open_kms 初始化过的结构。
 *
 * 返回值：
 *   无返回值；清理阶段失败只打印警告。
 */
static void close_kms(struct kms_device *kms)
{
    if (kms->use_plane && kms->plane_id != 0U && kms->fd >= 0) {
        drmModeSetPlane(kms->fd,
                        kms->plane_id,
                        0,
                        0,
                        0,
                        0,
                        0,
                        0,
                        0,
                        0,
                        0,
                        0,
                        0);
    }

    if (kms->old_crtc != NULL) {
        drmModeSetCrtc(kms->fd,
                       kms->old_crtc->crtc_id,
                       kms->old_crtc->buffer_id,
                       kms->old_crtc->x,
                       kms->old_crtc->y,
                       &kms->connector_id,
                       1,
                       &kms->old_crtc->mode);
        drmModeFreeCrtc(kms->old_crtc);
        kms->old_crtc = NULL;
    }

    if (kms->fb_map != NULL && kms->fb_map != MAP_FAILED) {
        munmap(kms->fb_map, kms->size);
        kms->fb_map = NULL;
    }

    if (kms->fb_id != 0) {
        drmModeRmFB(kms->fd, kms->fb_id);
        kms->fb_id = 0;
    }

    if (kms->dumb_handle != 0) {
        struct drm_mode_destroy_dumb destroy_req;

        memset(&destroy_req, 0, sizeof(destroy_req));
        destroy_req.handle = kms->dumb_handle;
        if (xioctl(kms->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req) < 0) {
            perror("销毁 DRM dumb buffer 失败");
        }
        kms->dumb_handle = 0;
    }

    if (kms->fd >= 0) {
        close(kms->fd);
        kms->fd = -1;
    }
}

/*
 * capture_loop 的作用：
 *   从摄像头取帧，转换并写入 KMS framebuffer。
 *
 * 参数：
 *   cam 是已启动采集流的摄像头。
 *   kms 是已设置 CRTC 的显示结构。
 *   frame_limit 控制最大显示帧数，0 表示持续运行。
 *   use_neon 为非 0 时使用 NEON 转换。
 *   use_staging 为非 0 时先写普通内存，再按行复制到 KMS framebuffer。
 *   output_format 指定写入 framebuffer 的像素格式。
 *
 * 返回值：
 *   正常结束返回 0；采集或转换失败返回 -1。
 */
static int capture_loop(struct camera_device *cam,
                        struct kms_device *kms,
                        struct control_server *control,
                        unsigned int frame_limit,
                        int use_neon,
                        int use_staging,
                        enum output_format output_format)
{
    unsigned int frames = 0;
    unsigned int bytes_per_pixel = bytes_per_pixel_for_format(output_format);
    uint8_t *staging = NULL;
    uint32_t staging_pitch = cam->width * bytes_per_pixel;
    struct latest_frame latest;
    int loop_ret = -1;

    memset(&latest, 0, sizeof(latest));
    latest.fb_map = kms->fb_map;
    latest.kms = kms;
    latest.pitch = kms->pitch;
    latest.fb_width = kms->fb_width;
    latest.fb_height = kms->fb_height;
    latest.frame_width = cam->width;
    latest.frame_height = cam->height;
    latest.bytes_per_pixel = bytes_per_pixel;
    latest.output_format = output_format;

    if (use_staging) {
        size_t staging_size = (size_t)staging_pitch * cam->height;

        staging = malloc(staging_size);
        if (staging == NULL) {
            fprintf(stderr, "申请 staging 缓冲失败: %zu bytes\n", staging_size);
            return -1;
        }
    }

    while (!g_stop && (frame_limit == 0 || frames < frame_limit)) {
        fd_set fds;
        struct timeval tv;
        struct v4l2_buffer buf;
        int ret;

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
            goto out;
        }

        if (ret == 0) {
            fprintf(stderr, "等待摄像头帧超时\n");
            goto out;
        }

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) {
                continue;
            }
            perror("取出摄像头缓冲区失败");
            goto out;
        }

        if (buf.index >= cam->buffer_count) {
            fprintf(stderr, "摄像头返回越界缓冲区索引: %u\n", buf.index);
            goto out;
        }

        latest.yuyv_map = (const uint8_t *)cam->buffers[buf.index].start;
        latest.yuyv_size = cam->buffers[buf.index].length;

        if (output_format == OUTPUT_FORMAT_RGB565) {
            ret = convert_yuyv_to_rgb565_center(use_staging ? staging : kms->fb_map,
                                                use_staging ? staging_pitch : kms->pitch,
                                                use_staging ? cam->width : kms->fb_width,
                                                use_staging ? cam->height : kms->fb_height,
                                                (const uint8_t *)cam->buffers[buf.index].start,
                                                cam->width,
                                                cam->height,
                                                use_neon);
        } else {
            ret = convert_yuyv_to_xrgb_center(use_staging ? staging : kms->fb_map,
                                              use_staging ? staging_pitch : kms->pitch,
                                              use_staging ? cam->width : kms->fb_width,
                                              use_staging ? cam->height : kms->fb_height,
                                              (const uint8_t *)cam->buffers[buf.index].start,
                                              cam->width,
                                              cam->height,
                                              use_neon);
        }

        if (ret != 0) {
            goto out;
        }

        if (use_staging &&
            copy_staging_to_center(kms->fb_map,
                                   kms->pitch,
                                   kms->fb_width,
                                   kms->fb_height,
                                   staging,
                                   staging_pitch,
                                   cam->width,
                                   cam->height,
                                    bytes_per_pixel) != 0) {
            goto out;
        }

        latest.serial = frames + 1U;
        latest.has_frame = 1;
        poll_control_server(control, &latest);

        if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("摄像头缓冲区重新入队失败");
            goto out;
        }

        frames++;
        if ((frames % 60U) == 0U) {
            printf("frames=%u\n", frames);
            fflush(stdout);
        }
    }

    poll_control_server(control, &latest);

    printf("capture finished, frames=%u\n", frames);
    loop_ret = 0;

out:
    free(staging);
    return loop_ret;
}

/*
 * main 的作用：
 *   串联参数解析、摄像头初始化、KMS 初始化、采集显示和资源清理。
 *
 * 参数：
 *   argc/argv 是命令行参数。
 *
 * 返回值：
 *   EXIT_SUCCESS 表示正常结束；EXIT_FAILURE 表示探测失败。
 */
int main(int argc, char **argv)
{
    struct app_config cfg;
    struct camera_device cam;
    struct kms_device kms;
    struct control_server control;
    int stream_started = 0;
    int control_started = 0;
    int ret = EXIT_FAILURE;

    if (parse_args(argc, argv, &cfg) != 0) {
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    memset(&cam, 0, sizeof(cam));
    cam.fd = -1;
    memset(&kms, 0, sizeof(kms));
    kms.fd = -1;
    memset(&control, 0, sizeof(control));
    control.fd = -1;

    if (open_camera(cfg.video_device, &cam) != 0) {
        goto out;
    }

    if (setup_camera_format(&cam, cfg.width, cfg.height) != 0) {
        goto out;
    }

    if (set_camera_fps(&cam, cfg.fps) != 0) {
        goto out;
    }

    if (init_camera_mmap(&cam) != 0) {
        goto out;
    }

    if (cfg.use_plane && cfg.output_format == OUTPUT_FORMAT_XRGB8888) {
        cfg.output_format = OUTPUT_FORMAT_ARGB8888;
    }

    if (open_kms(cfg.drm_device,
                 &kms,
                 cfg.output_format,
                 cfg.use_plane,
                 (uint32_t)cfg.plane_id,
                 cfg.use_plane ? cfg.width : 0U,
                 cfg.use_plane ? cfg.height : 0U,
                 cfg.dst_x,
                 cfg.dst_y,
                 cfg.dst_w,
                 cfg.dst_h,
                 cfg.initial_visible) != 0) {
        goto out;
    }

    if (start_camera_stream(&cam) != 0) {
        goto out;
    }
    stream_started = 1;

    printf("convert mode: %s\n", cfg.use_neon ? "neon" : "scalar");
    printf("output format: %s\n", output_format_name(cfg.output_format));
    printf("write strategy: %s\n", cfg.use_staging ? "staging" : "direct");
    printf("display target: %s%u\n", cfg.use_plane ? "plane " : "crtc ", cfg.use_plane ? cfg.plane_id : 0U);
    fflush(stdout);

    if (init_control_server(&control, cfg.control_socket_path) != 0) {
        goto out;
    }
    control_started = 1;

    if (capture_loop(&cam,
                     &kms,
                     &control,
                     cfg.frame_limit,
                     cfg.use_neon,
                     cfg.use_staging,
                     cfg.output_format) != 0) {
        goto out;
    }

    ret = EXIT_SUCCESS;

out:
    if (control_started) {
        close_control_server(&control);
    }
    if (stream_started) {
        stop_camera_stream(&cam);
    }
    close_camera(&cam);
    close_kms(&kms);
    return ret;
}
