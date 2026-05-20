/*
 * fb_boot_splash.c
 *
 * 作用：
 *   在 Qt Quick、eglfs、galcore GPU 和摄像头 overlay 进程启动之前，直接向
 *   Linux framebuffer `/dev/fb0` 绘制一张静态启动首帧。
 *
 * 主要流程：
 *   1. 解析命令行，确定要写入的 framebuffer 设备节点。
 *   2. 打开 framebuffer，读取固定信息、可变信息和像素位域。
 *   3. 优先读取 HTML 渲染生成的 RGB565 静态图资源，整张写入 framebuffer。
 *   4. 如果资源缺失或尺寸不匹配，再回退到 C 几何图元绘制，避免开机阶段黑屏。
 *   5. 调用 msync/fsync 尽快把静态首帧刷新到屏幕，然后释放映射并退出。
 *
 * 关键说明：
 *   - 本程序不依赖 Qt、OpenGL、DRM、图片解码库或字体库，适合放到 Buildroot SysV init
 *     的早期阶段运行，用来填补 Qt 启动画面出现前的黑屏时间。
 *   - 复杂中文、光晕和半透明层不再用 C 手工重画，而是由 generate_boot_splash_asset.py
 *     把 ai_boot_splash_preview.html 渲染为 boot_splash.rgb565；本程序只做资源 blit。
 *
 * 参数：
 *   -f /dev/fb0  指定 framebuffer 设备节点，默认 /dev/fb0。
 *   -a path      指定 RGB565 启动图资源，默认 /root/qt_camera_display/boot_splash.rgb565。
 *   -q           静默模式，只绘制不输出普通日志，适合 init 脚本调用。
 *   -h           打印帮助。
 *
 * 返回值：
 *   成功绘制返回 0；打开 framebuffer、查询信息、mmap 或像素格式不支持时返回非 0。
 */

#include <ctype.h>              /* toupper 用于把显示文本统一转换成内置点阵支持的大写字母。 */
#include <errno.h>              /* errno 保存系统调用失败原因，便于 perror 输出具体错误。 */
#include <fcntl.h>              /* open 使用 O_RDWR/O_CLOEXEC 打开 framebuffer 节点。 */
#include <linux/fb.h>           /* fb_fix_screeninfo、fb_var_screeninfo 和 FBIOGET_* ioctl 定义。 */
#include <stdint.h>             /* uint8_t/uint16_t/uint32_t 提供明确宽度的像素字段类型。 */
#include <stdio.h>              /* printf/fprintf/perror 用于输出帮助、状态和错误。 */
#include <stdlib.h>             /* EXIT_SUCCESS/EXIT_FAILURE 表示 main 返回语义。 */
#include <string.h>             /* memset/strcmp/strlen 用于初始化结构和解析参数。 */
#include <sys/ioctl.h>          /* ioctl 用于读取 framebuffer 固定信息和可变信息。 */
#include <sys/mman.h>           /* mmap/munmap/msync 用于映射并刷新 LCD 显存。 */
#include <sys/types.h>          /* POSIX 基础类型，配合 open/mmap/close 使用。 */
#include <unistd.h>             /* close/fsync 提供文件描述符资源释放和刷盘接口。 */

/* DEFAULT_FB_DEVICE 是正点原子 STM32MP157 RGB LCD 通常暴露的 framebuffer 设备节点。 */
#define DEFAULT_FB_DEVICE "/dev/fb0"

/* DEFAULT_SPLASH_ASSET 是部署脚本安装的 HTML 渲染版 RGB565 启动图资源路径。 */
#define DEFAULT_SPLASH_ASSET "/root/qt_camera_display/boot_splash.rgb565"

/* REF_WIDTH 是 QML 主界面的设计宽度，早期静态图按它做比例换算。 */
#define REF_WIDTH 1024

/* REF_HEIGHT 是 QML 主界面的设计高度，和 7 寸 RGB 屏 1024x600 对齐。 */
#define REF_HEIGHT 600

/* SPLASH_ASSET_BYTES 是 1024x600 RGB565 每像素 2 字节时必须满足的资源大小。 */
#define SPLASH_ASSET_BYTES ((size_t)REF_WIDTH * (size_t)REF_HEIGHT * 2U)

/* SPLASH_TITLE_CN 保留 HTML 预览稿中的中文主标语，日志和静态检查会用到它。 */
static const char *SPLASH_TITLE_CN = "AI赋能设计，设计点亮AI!";

/* SPLASH_CONTEST_CN 保留用户要求必须出现的赛事名称，便于确认真实启动图主题。 */
static const char *SPLASH_CONTEST_CN = "第九届嵌入式芯片与系统设计竞赛";

/* SPLASH_CONTEST_EN 是赛事名称的英文版本，日志和后续扩展字库时会用到它。 */
static const char *SPLASH_CONTEST_EN = "Embedded Chip & System Design Contest";

/* SPLASH_BRAND_EN 是主品牌标语的英文点阵版本，会绘制到白色 Logo 面板。 */
static const char *SPLASH_BRAND_EN = "AI for Design & Design for AI!";

/* SPLASH_SYSTEM_EN 是底部系统名称，用于连接启动图和后续 Qt 工业视觉界面。 */
static const char *SPLASH_SYSTEM_EN = "AI Vision Design Terminal";

/* SPLASH_BADGE_EDGE_AI 是底部第一枚技术铭牌的语义文本。 */
static const char *SPLASH_BADGE_EDGE_AI = "MP157 Edge AI";

/* SPLASH_BADGE_FRAMEBUFFER 是底部第二枚技术铭牌的语义文本。 */
static const char *SPLASH_BADGE_FRAMEBUFFER = "Framebuffer Splash";

/* SPLASH_BADGE_QT 是底部第三枚技术铭牌的语义文本。 */
static const char *SPLASH_BADGE_QT = "Qt Vision Ready";

/*
 * rgb_color 保存一个 8bit RGB 颜色。
 * r/g/b 分别是红、绿、蓝通道，写入 framebuffer 前会按屏幕位域转换成目标格式。
 */
struct rgb_color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

/*
 * framebuffer_device 保存一个已打开 framebuffer 的运行状态。
 * fd 是 `/dev/fb0` 文件描述符。
 * mem 是 mmap 后的显存起始地址。
 * mem_len 是映射长度，来自 smem_len 或 line_length * yres_virtual。
 * fix 保存一行字节数、显存长度等固定信息。
 * var 保存分辨率、位深和 RGB 位域等可变信息。
 */
struct framebuffer_device {
    int fd;
    uint8_t *mem;
    size_t mem_len;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
};

/*
 * app_config 保存命令行配置。
 * fb_device 是要写入的 framebuffer 节点。
 * asset_path 是 HTML 渲染生成的 RGB565 静态图资源路径。
 * quiet 为 1 时不输出普通日志，避免 init 脚本污染控制台。
 */
struct app_config {
    const char *fb_device;
    const char *asset_path;
    int quiet;
};

/*
 * print_usage 的作用：
 *   打印 fb_boot_splash 的命令行帮助。
 *
 * 参数：
 *   prog 是 argv[0]，用于在帮助中显示当前程序名。
 *
 * 返回值：
 *   无返回值，只向 stdout 输出帮助文本。
 */
static void print_usage(const char *prog)
{
    printf("用法: %s [-f /dev/fb0] [-a boot_splash.rgb565] [-q] [-h]\n", prog);
    printf("  -f  指定 framebuffer 设备，默认 %s\n", DEFAULT_FB_DEVICE);
    printf("  -a  指定 RGB565 启动图资源，默认 %s\n", DEFAULT_SPLASH_ASSET);
    printf("  -q  静默绘制，不输出普通日志，适合 init 脚本调用\n");
    printf("  -h  显示帮助\n");
}

/*
 * parse_args 的作用：
 *   解析命令行参数，得到 framebuffer 节点和日志模式。
 *
 * 主要流程：
 *   1. 设置默认 `/dev/fb0`。
 *   2. 逐个解析 `-f`、`-a`、`-q` 和 `-h`。
 *   3. 对缺少参数或未知参数返回错误，避免启动脚本误传值时静默失败。
 *
 * 参数：
 *   argc/argv 是 main 收到的命令行参数。
 *   cfg 保存解析结果。
 *
 * 返回值：
 *   0 表示可继续绘制；1 表示已经打印帮助并正常退出；-1 表示参数错误。
 */
static int parse_args(int argc, char *argv[], struct app_config *cfg)
{
    int i;

    cfg->fb_device = DEFAULT_FB_DEVICE;
    cfg->asset_path = DEFAULT_SPLASH_ASSET;
    cfg->quiet = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-f") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "错误：-f 缺少 framebuffer 设备路径\n");
                return -1;
            }
            cfg->fb_device = argv[++i];
        } else if (strcmp(argv[i], "-a") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "错误：-a 缺少 RGB565 启动图资源路径\n");
                return -1;
            }
            cfg->asset_path = argv[++i];
        } else if (strcmp(argv[i], "-q") == 0) {
            cfg->quiet = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        } else {
            fprintf(stderr, "错误：未知参数：%s\n", argv[i]);
            return -1;
        }
    }

    return 0;
}

/*
 * close_framebuffer 的作用：
 *   释放 framebuffer mmap 映射并关闭文件描述符。
 *
 * 参数：
 *   fb 是 open_framebuffer 初始化过的 framebuffer 状态结构。
 *
 * 返回值：
 *   无返回值；释放路径尽量完成所有资源清理。
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
 * open_framebuffer 的作用：
 *   打开并映射 framebuffer，为后续像素绘制做准备。
 *
 * 主要流程：
 *   1. 用读写方式打开设备节点。
 *   2. 通过 FBIOGET_FSCREENINFO 读取行跨度和显存长度。
 *   3. 通过 FBIOGET_VSCREENINFO 读取分辨率、位深和 RGB 位域。
 *   4. 检查当前位深是否是本程序支持的 16/24/32 bpp。
 *   5. mmap 映射显存，供 CPU 直接写像素。
 *
 * 参数：
 *   device 是 framebuffer 节点路径。
 *   fb 保存打开后的运行状态。
 *
 * 返回值：
 *   成功返回 0；任一步失败返回 -1。
 */
static int open_framebuffer(const char *device, struct framebuffer_device *fb)
{
    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;
    fb->mem = NULL;

    fb->fd = open(device, O_RDWR | O_CLOEXEC);
    if (fb->fd < 0) {
        perror("打开 framebuffer 失败");
        return -1;
    }

    if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->fix) < 0) {
        perror("读取 framebuffer 固定信息失败");
        close_framebuffer(fb);
        return -1;
    }

    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) < 0) {
        perror("读取 framebuffer 可变信息失败");
        close_framebuffer(fb);
        return -1;
    }

    if (fb->var.xres == 0 || fb->var.yres == 0 || fb->fix.line_length == 0) {
        fprintf(stderr, "错误：framebuffer 分辨率或 line_length 非法\n");
        close_framebuffer(fb);
        return -1;
    }

    if (fb->var.bits_per_pixel != 16 && fb->var.bits_per_pixel != 24 && fb->var.bits_per_pixel != 32) {
        fprintf(stderr, "错误：暂不支持 %u bpp framebuffer\n", fb->var.bits_per_pixel);
        close_framebuffer(fb);
        return -1;
    }

    fb->mem_len = fb->fix.smem_len;
    if (fb->mem_len == 0) {
        fb->mem_len = (size_t)fb->fix.line_length * fb->var.yres_virtual;
    }

    fb->mem = mmap(NULL, fb->mem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED) {
        perror("映射 framebuffer 显存失败");
        fb->mem = NULL;
        close_framebuffer(fb);
        return -1;
    }

    return 0;
}

/*
 * scale_ref_x 的作用：
 *   把 QML 设计稿中的横向坐标按当前 framebuffer 实际宽度换算。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   value 是以 1024 宽度为基准的设计坐标或尺寸。
 *
 * 返回值：
 *   返回按当前屏幕宽度缩放后的像素值。
 */
static int scale_ref_x(const struct framebuffer_device *fb, int value)
{
    return (int)((long long)value * (long long)fb->var.xres / REF_WIDTH);
}

/*
 * scale_ref_y 的作用：
 *   把 QML 设计稿中的纵向坐标按当前 framebuffer 实际高度换算。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   value 是以 600 高度为基准的设计坐标或尺寸。
 *
 * 返回值：
 *   返回按当前屏幕高度缩放后的像素值。
 */
static int scale_ref_y(const struct framebuffer_device *fb, int value)
{
    return (int)((long long)value * (long long)fb->var.yres / REF_HEIGHT);
}

/*
 * scale_ref_min 的作用：
 *   用横向和纵向缩放比例中的较小值换算线宽、圆角近似尺寸和字体倍数。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   value 是设计尺寸。
 *
 * 返回值：
 *   返回按较小比例缩放后的尺寸，最小为 1。
 */
static int scale_ref_min(const struct framebuffer_device *fb, int value)
{
    int sx = scale_ref_x(fb, value);
    int sy = scale_ref_y(fb, value);
    int scaled = sx < sy ? sx : sy;

    return scaled > 0 ? scaled : 1;
}

/*
 * expand_component 的作用：
 *   把 8bit 颜色分量压缩到 framebuffer 指定的位宽。
 *
 * 参数：
 *   value 是 0~255 的 RGB 分量。
 *   length 是 framebuffer 位域长度，例如 RGB565 的红色长度为 5。
 *
 * 返回值：
 *   返回已经缩放到目标位宽的整数值。
 */
static uint32_t expand_component(uint8_t value, uint32_t length)
{
    if (length == 0) {
        return 0;
    }

    if (length >= 8) {
        return (uint32_t)value << (length - 8);
    }

    return (uint32_t)value >> (8 - length);
}

/*
 * pack_color 的作用：
 *   按 framebuffer 的 RGB 位域把 8bit RGB 颜色打包成目标像素值。
 *
 * 参数：
 *   fb 是 framebuffer 状态，里面的 var.red/green/blue 描述位域。
 *   color 是待写入的 RGB 颜色。
 *
 * 返回值：
 *   返回可直接按小端字节顺序写入显存的像素值。
 */
static uint32_t pack_color(const struct framebuffer_device *fb, struct rgb_color color)
{
    uint32_t pixel = 0;

    pixel |= expand_component(color.r, fb->var.red.length) << fb->var.red.offset;
    pixel |= expand_component(color.g, fb->var.green.length) << fb->var.green.offset;
    pixel |= expand_component(color.b, fb->var.blue.length) << fb->var.blue.offset;

    if (fb->var.transp.length > 0) {
        uint32_t alpha_mask = 0xffffffffU;

        if (fb->var.transp.length < 32U) {
            alpha_mask = (1U << fb->var.transp.length) - 1U;
        }

        pixel |= alpha_mask << fb->var.transp.offset;
    }

    return pixel;
}

/*
 * put_pixel 的作用：
 *   在 framebuffer 指定坐标写入一个像素。
 *
 * 主要流程：
 *   1. 做边界检查，越界像素直接丢弃。
 *   2. 按 xoffset/yoffset 计算虚拟屏幕中的真实位置。
 *   3. 按 bits_per_pixel 写入 2、3 或 4 个字节。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   x/y 是目标坐标。
 *   color 是目标颜色。
 *
 * 返回值：
 *   无返回值；越界或地址超出映射长度时不写入。
 */
static void put_pixel(struct framebuffer_device *fb, int x, int y, struct rgb_color color)
{
    uint32_t pixel;
    unsigned int bytes_per_pixel;
    size_t offset;

    if (x < 0 || y < 0 || x >= (int)fb->var.xres || y >= (int)fb->var.yres) {
        return;
    }

    bytes_per_pixel = fb->var.bits_per_pixel / 8U;
    offset = (size_t)(y + (int)fb->var.yoffset) * fb->fix.line_length
           + (size_t)(x + (int)fb->var.xoffset) * bytes_per_pixel;

    if (offset + bytes_per_pixel > fb->mem_len) {
        return;
    }

    pixel = pack_color(fb, color);

    if (bytes_per_pixel == 2U) {
        fb->mem[offset + 0U] = (uint8_t)(pixel & 0xffU);
        fb->mem[offset + 1U] = (uint8_t)((pixel >> 8) & 0xffU);
    } else if (bytes_per_pixel == 3U) {
        fb->mem[offset + 0U] = (uint8_t)(pixel & 0xffU);
        fb->mem[offset + 1U] = (uint8_t)((pixel >> 8) & 0xffU);
        fb->mem[offset + 2U] = (uint8_t)((pixel >> 16) & 0xffU);
    } else {
        fb->mem[offset + 0U] = (uint8_t)(pixel & 0xffU);
        fb->mem[offset + 1U] = (uint8_t)((pixel >> 8) & 0xffU);
        fb->mem[offset + 2U] = (uint8_t)((pixel >> 16) & 0xffU);
        fb->mem[offset + 3U] = (uint8_t)((pixel >> 24) & 0xffU);
    }
}

/*
 * fill_rect 的作用：
 *   绘制一个实心矩形，是背景、面板、芯片图形和标签的基础绘制函数。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   x/y 是矩形左上角。
 *   w/h 是矩形宽高。
 *   color 是填充颜色。
 *
 * 返回值：
 *   无返回值；函数内部会裁剪屏幕外的区域。
 */
static void fill_rect(struct framebuffer_device *fb, int x, int y, int w, int h, struct rgb_color color)
{
    int row;
    int col;
    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;

    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > (int)fb->var.xres) {
        x1 = (int)fb->var.xres;
    }
    if (y1 > (int)fb->var.yres) {
        y1 = (int)fb->var.yres;
    }

    for (row = y0; row < y1; ++row) {
        for (col = x0; col < x1; ++col) {
            put_pixel(fb, col, row, color);
        }
    }
}

/*
 * draw_rect_border 的作用：
 *   绘制矩形边框，用于模拟启动图中的 Logo 面板、ROI 线稿和技术铭牌。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   x/y/w/h 描述矩形位置和大小。
 *   border 是边框颜色。
 *   thickness 是边框厚度，传入小于 1 时按 1 处理。
 *
 * 返回值：
 *   无返回值。
 */
static void draw_rect_border(struct framebuffer_device *fb,
                             int x,
                             int y,
                             int w,
                             int h,
                             struct rgb_color border,
                             int thickness)
{
    int t = thickness > 0 ? thickness : 1;

    fill_rect(fb, x, y, w, t, border);
    fill_rect(fb, x, y + h - t, w, t, border);
    fill_rect(fb, x, y, t, h, border);
    fill_rect(fb, x + w - t, y, t, h, border);
}

/*
 * glyph_for_char 的作用：
 *   返回 5x7 ASCII 点阵字模。
 *
 * 说明：
 *   每个字节使用低 5 位表示一行，从高位到低位对应从左到右 5 个像素。
 *   这里只内置启动首帧需要的英文、数字和少量符号，避免引入字体库。
 *
 * 参数：
 *   ch 是待绘制字符。
 *
 * 返回值：
 *   返回 7 行字模；未知字符返回问号字模。
 */
static const uint8_t *glyph_for_char(char ch)
{
    static const uint8_t space[7] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t question[7] = { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 };
    static const uint8_t minus[7] = { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 };
    static const uint8_t dot[7] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c };
    static const uint8_t colon[7] = { 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00 };
    static const uint8_t slash[7] = { 0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10 };
    static const uint8_t percent[7] = { 0x19, 0x1a, 0x02, 0x04, 0x08, 0x0b, 0x13 };
    static const uint8_t exclamation[7] = { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 };
    static const uint8_t ampersand[7] = { 0x0c, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0d };
    static const uint8_t comma[7] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x08 };

    static const uint8_t digits[10][7] = {
        { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e },
        { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e },
        { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f },
        { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e },
        { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 },
        { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e },
        { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e },
        { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
        { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e },
        { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c }
    };

    static const uint8_t letters[26][7] = {
        { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 },
        { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e },
        { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e },
        { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e },
        { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f },
        { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 },
        { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f },
        { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 },
        { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e },
        { 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e },
        { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
        { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f },
        { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 },
        { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },
        { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e },
        { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 },
        { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d },
        { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 },
        { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e },
        { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
        { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e },
        { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 },
        { 0x11, 0x11, 0x15, 0x15, 0x15, 0x1b, 0x11 },
        { 0x11, 0x0a, 0x04, 0x04, 0x0a, 0x11, 0x11 },
        { 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04, 0x04 },
        { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f }
    };

    unsigned char upper = (unsigned char)toupper((unsigned char)ch);

    if (upper == ' ') {
        return space;
    }
    if (upper >= '0' && upper <= '9') {
        return digits[upper - '0'];
    }
    if (upper >= 'A' && upper <= 'Z') {
        return letters[upper - 'A'];
    }
    if (upper == '-') {
        return minus;
    }
    if (upper == '.') {
        return dot;
    }
    if (upper == ':') {
        return colon;
    }
    if (upper == '/') {
        return slash;
    }
    if (upper == '%') {
        return percent;
    }
    if (upper == '!') {
        return exclamation;
    }
    if (upper == '&') {
        return ampersand;
    }
    if (upper == ',') {
        return comma;
    }

    return question;
}

/*
 * text_width 的作用：
 *   计算一行 ASCII 点阵文本的像素宽度，用于水平居中。
 *
 * 参数：
 *   text 是待绘制文本。
 *   scale 是点阵缩放倍数。
 *
 * 返回值：
 *   返回文本绘制宽度，单位像素。
 */
static int text_width(const char *text, int scale)
{
    size_t len = strlen(text);

    if (len == 0) {
        return 0;
    }

    return (int)len * (5 * scale) + (int)(len - 1U) * scale;
}

/*
 * draw_text 的作用：
 *   使用内置 5x7 点阵字体绘制一行 ASCII 文本。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   x/y 是文本左上角。
 *   text 是待绘制文本。
 *   scale 是每个点阵像素放大的倍数。
 *   color 是文本颜色。
 *
 * 返回值：
 *   无返回值；未知字符会显示成问号。
 */
static void draw_text(struct framebuffer_device *fb,
                      int x,
                      int y,
                      const char *text,
                      int scale,
                      struct rgb_color color)
{
    int cursor_x = x;
    size_t i;

    for (i = 0; text[i] != '\0'; ++i) {
        const uint8_t *glyph = glyph_for_char(text[i]);
        int row;
        int col;

        for (row = 0; row < 7; ++row) {
            for (col = 0; col < 5; ++col) {
                if (glyph[row] & (uint8_t)(1U << (4 - col))) {
                    fill_rect(fb,
                              cursor_x + col * scale,
                              y + row * scale,
                              scale,
                              scale,
                              color);
                }
            }
        }

        cursor_x += 6 * scale;
    }
}

/*
 * draw_text_center 的作用：
 *   按当前屏幕宽度把文本居中绘制。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   y 是文本顶边坐标。
 *   text 是待绘制文本。
 *   scale 是点阵缩放倍数。
 *   color 是文本颜色。
 *
 * 返回值：
 *   无返回值。
 */
static void draw_text_center(struct framebuffer_device *fb,
                             int y,
                             const char *text,
                             int scale,
                             struct rgb_color color)
{
    int width = text_width(text, scale);
    int x = ((int)fb->var.xres - width) / 2;

    draw_text(fb, x, y, text, scale, color);
}

/*
 * draw_grid 的作用：
 *   绘制低对比度科技背景网格，模拟 HTML 预览稿里的启动画布结构线。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   x/y/w/h 描述网格区域。
 *   columns/rows 描述网格列数和行数。
 *   color 是网格线颜色。
 *
 * 返回值：
 *   无返回值。
 */
static void draw_grid(struct framebuffer_device *fb,
                      int x,
                      int y,
                      int w,
                      int h,
                      int columns,
                      int rows,
                      struct rgb_color color)
{
    /* i 是网格线循环索引，先遍历竖线，再遍历横线。 */
    int i;
    /* line 是网格线宽，按屏幕缩放后至少保留 1 个像素。 */
    int line = scale_ref_min(fb, 1);

    for (i = 0; i <= columns; ++i) {
        /* gx 是当前竖向网格线的屏幕 x 坐标。 */
        int gx = x + i * w / columns;
        fill_rect(fb, gx, y, line, h, color);
    }

    for (i = 0; i <= rows; ++i) {
        /* gy 是当前横向网格线的屏幕 y 坐标。 */
        int gy = y + i * h / rows;
        fill_rect(fb, x, gy, w, line, color);
    }
}

/*
 * unpack_rgb565 的作用：
 *   把 RGB565 little-endian 像素转换成 8bit RGB，便于非 16bpp 或缩放路径复用 put_pixel。
 *
 * 主要流程：
 *   1. 从 raw 文件的低字节和高字节还原 16bit RGB565 值。
 *   2. 分别提取 R5、G6、B5 分量。
 *   3. 使用位扩展把低位补齐到 8bit，减少颜色发暗。
 *
 * 参数：
 *   lo 是 RGB565 低字节。
 *   hi 是 RGB565 高字节。
 *
 * 返回值：
 *   返回 8bit RGB 颜色结构。
 */
static struct rgb_color unpack_rgb565(uint8_t lo, uint8_t hi)
{
    /* value 是 little-endian 还原后的 RGB565 像素值。 */
    uint16_t value = (uint16_t)lo | (uint16_t)((uint16_t)hi << 8);
    /* r5/g6/b5 是从 RGB565 中拆出来的原始颜色分量。 */
    uint8_t r5 = (uint8_t)((value >> 11) & 0x1fU);
    uint8_t g6 = (uint8_t)((value >> 5) & 0x3fU);
    uint8_t b5 = (uint8_t)(value & 0x1fU);
    /* color 保存扩展到 8bit 后的 RGB 颜色。 */
    struct rgb_color color;

    color.r = (uint8_t)((r5 << 3) | (r5 >> 2));
    color.g = (uint8_t)((g6 << 2) | (g6 >> 4));
    color.b = (uint8_t)((b5 << 3) | (b5 >> 2));

    return color;
}

/*
 * read_exact_file 的作用：
 *   从启动图资源文件读取固定字节数，防止半截 raw 被误认为可用。
 *
 * 主要流程：
 *   1. 打开资源文件。
 *   2. 循环读取直到达到 expected_size。
 *   3. 再尝试多读 1 字节，确认文件没有额外尾巴。
 *   4. 所有路径都关闭文件描述符，避免 init 阶段泄漏资源。
 *
 * 参数：
 *   path 是资源文件路径。
 *   buffer 是接收数据的缓冲区。
 *   expected_size 是必须读取到的字节数。
 *
 * 返回值：
 *   成功返回 0；打开失败、长度不足或长度超出时返回 -1。
 */
static int read_exact_file(const char *path, uint8_t *buffer, size_t expected_size)
{
    /* fd 是资源文件描述符，函数结束前必须关闭。 */
    int fd;
    /* offset 是已经读入 buffer 的字节数。 */
    size_t offset = 0U;
    /* extra 用于确认文件正好结束，没有多余尾部。 */
    uint8_t extra;
    /* ret 保存最后一次 read 的返回值。 */
    ssize_t ret;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    while (offset < expected_size) {
        ret = read(fd, buffer + offset, expected_size - offset);
        if (ret < 0) {
            close(fd);
            return -1;
        }
        if (ret == 0) {
            close(fd);
            return -1;
        }
        offset += (size_t)ret;
    }

    ret = read(fd, &extra, 1U);
    close(fd);

    if (ret != 0) {
        return -1;
    }

    return 0;
}

/*
 * draw_splash_asset_fast_rgb565 的作用：
 *   在 1024x600 且 framebuffer 为 16bpp 时，把 RGB565 资源按行直接复制到显存。
 *
 * 主要流程：
 *   1. 遍历 600 行设计图。
 *   2. 按 framebuffer 的 line_length 找到目标行。
 *   3. 每行复制 1024*2 字节，保留 framebuffer 可能存在的行尾 padding。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   asset 是 boot_splash.rgb565 文件内容。
 *
 * 返回值：
 *   无返回值；调用方已确认分辨率、位深和映射长度合法。
 */
static void draw_splash_asset_fast_rgb565(struct framebuffer_device *fb, const uint8_t *asset)
{
    /* row 是当前复制的设计图行号。 */
    int row;
    /* row_bytes 是一行 1024 个 RGB565 像素占用的字节数。 */
    size_t row_bytes = (size_t)REF_WIDTH * 2U;

    for (row = 0; row < REF_HEIGHT; ++row) {
        /* dst_offset 是 framebuffer 中当前行的起始字节偏移。 */
        size_t dst_offset = (size_t)(row + (int)fb->var.yoffset) * fb->fix.line_length
                          + (size_t)fb->var.xoffset * 2U;
        /* src_offset 是 raw 资源中当前行的起始字节偏移。 */
        size_t src_offset = (size_t)row * row_bytes;

        if (dst_offset + row_bytes <= fb->mem_len) {
            memcpy(fb->mem + dst_offset, asset + src_offset, row_bytes);
        }
    }
}

/*
 * draw_splash_asset_scaled 的作用：
 *   在 framebuffer 不是 1024x600 RGB565 直拷场景时，对 raw 资源做最近邻缩放绘制。
 *
 * 主要流程：
 *   1. 遍历当前 framebuffer 的所有可见像素。
 *   2. 按比例映射到 1024x600 资源坐标。
 *   3. 把 RGB565 解包成 RGB888，再交给 put_pixel 按实际位域写屏。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   asset 是 boot_splash.rgb565 文件内容。
 *
 * 返回值：
 *   无返回值。
 */
static void draw_splash_asset_scaled(struct framebuffer_device *fb, const uint8_t *asset)
{
    /* y 是当前 framebuffer 输出行。 */
    int y;

    for (y = 0; y < (int)fb->var.yres; ++y) {
        /* src_y 是映射到资源图上的行号。 */
        int src_y = y * REF_HEIGHT / (int)fb->var.yres;
        /* x 是当前 framebuffer 输出列。 */
        int x;

        for (x = 0; x < (int)fb->var.xres; ++x) {
            /* src_x 是映射到资源图上的列号。 */
            int src_x = x * REF_WIDTH / (int)fb->var.xres;
            /* src_offset 是资源中对应 RGB565 像素的字节偏移。 */
            size_t src_offset = ((size_t)src_y * (size_t)REF_WIDTH + (size_t)src_x) * 2U;
            /* color 是解包后的 8bit RGB 颜色。 */
            struct rgb_color color = unpack_rgb565(asset[src_offset], asset[src_offset + 1U]);

            put_pixel(fb, x, y, color);
        }
    }
}

/*
 * framebuffer_is_rgb565 的作用：
 *   判断当前 framebuffer 是否是标准 RGB565 little-endian 布局，决定能否安全整行 memcpy。
 *
 * 主要流程：
 *   1. 检查位深必须是 16bpp。
 *   2. 检查红色位域为 offset=11、length=5。
 *   3. 检查绿色位域为 offset=5、length=6。
 *   4. 检查蓝色位域为 offset=0、length=5。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *
 * 返回值：
 *   是标准 RGB565 返回 1；否则返回 0，调用方应走 put_pixel 转换路径。
 */
static int framebuffer_is_rgb565(const struct framebuffer_device *fb)
{
    return fb->var.bits_per_pixel == 16U
        && fb->var.red.offset == 11U
        && fb->var.red.length == 5U
        && fb->var.green.offset == 5U
        && fb->var.green.length == 6U
        && fb->var.blue.offset == 0U
        && fb->var.blue.length == 5U;
}

/*
 * draw_splash_asset 的作用：
 *   优先显示 HTML 渲染生成的 RGB565 启动图，让板端首帧尽量和浏览器预览一致。
 *
 * 主要流程：
 *   1. 申请固定大小缓冲区并读取 boot_splash.rgb565。
 *   2. 当前屏幕为 1024x600 16bpp 时走按行 memcpy 的快速路径。
 *   3. 其它位深或分辨率走最近邻缩放路径，保证调试屏幕也能看到完整图。
 *   4. 释放缓冲区并返回是否成功。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   asset_path 是 RGB565 启动图资源路径。
 *
 * 返回值：
 *   成功绘制资源返回 0；资源不可用或内存不足返回 -1，由调用方决定是否 fallback。
 */
static int draw_splash_asset(struct framebuffer_device *fb, const char *asset_path)
{
    /* asset 保存完整 1024x600 RGB565 原始像素数据。 */
    uint8_t *asset = (uint8_t *)malloc(SPLASH_ASSET_BYTES);
    /* ret 保存资源读取和绘制流程的结果。 */
    int ret = 0;

    if (asset == NULL) {
        return -1;
    }

    if (read_exact_file(asset_path, asset, SPLASH_ASSET_BYTES) != 0) {
        free(asset);
        return -1;
    }

    if (fb->var.xres == REF_WIDTH && fb->var.yres == REF_HEIGHT && framebuffer_is_rgb565(fb)) {
        draw_splash_asset_fast_rgb565(fb, asset);
    } else {
        draw_splash_asset_scaled(fb, asset);
    }

    free(asset);
    return ret;
}

/*
 * draw_hline 的作用：
 *   绘制一条水平实线，用于背景电路、芯片连线和弱化扫描线稿。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   x/y 是线条起点。
 *   w 是线条长度。
 *   thickness 是线条厚度。
 *   color 是线条颜色。
 *
 * 返回值：
 *   无返回值；底层 fill_rect 会负责越界裁剪。
 */
static void draw_hline(struct framebuffer_device *fb,
                       int x,
                       int y,
                       int w,
                       int thickness,
                       struct rgb_color color)
{
    fill_rect(fb, x, y, w, thickness > 0 ? thickness : 1, color);
}

/*
 * draw_vline 的作用：
 *   绘制一条垂直实线，用于背景电路、芯片连线和弱化扫描线稿。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   x/y 是线条起点。
 *   h 是线条长度。
 *   thickness 是线条厚度。
 *   color 是线条颜色。
 *
 * 返回值：
 *   无返回值；底层 fill_rect 会负责越界裁剪。
 */
static void draw_vline(struct framebuffer_device *fb,
                       int x,
                       int y,
                       int h,
                       int thickness,
                       struct rgb_color color)
{
    fill_rect(fb, x, y, thickness > 0 ? thickness : 1, h, color);
}

/*
 * draw_corner_brackets 的作用：
 *   绘制四个 L 形角标，用于把普通矩形变成更像机器视觉 ROI 的线稿。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   x/y/w/h 描述角标所在矩形区域。
 *   length 是每个角标向内延伸的长度。
 *   thickness 是角标线宽。
 *   color 是角标颜色。
 *
 * 返回值：
 *   无返回值。
 */
static void draw_corner_brackets(struct framebuffer_device *fb,
                                 int x,
                                 int y,
                                 int w,
                                 int h,
                                 int length,
                                 int thickness,
                                 struct rgb_color color)
{
    draw_hline(fb, x, y, length, thickness, color);
    draw_vline(fb, x, y, length, thickness, color);
    draw_hline(fb, x + w - length, y, length, thickness, color);
    draw_vline(fb, x + w - thickness, y, length, thickness, color);
    draw_hline(fb, x, y + h - thickness, length, thickness, color);
    draw_vline(fb, x, y + h - length, length, thickness, color);
    draw_hline(fb, x + w - length, y + h - thickness, length, thickness, color);
    draw_vline(fb, x + w - thickness, y + h - length, length, thickness, color);
}

/*
 * draw_circuit_trace 的作用：
 *   绘制 HTML 预览稿背景里的电路线和端点节点，增强芯片启动图氛围。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   x/y 是电路线起点。
 *   horizontal_len 是水平线长度。
 *   vertical_len 是末端竖线长度，可正可负。
 *   color 是电路线颜色。
 *   node_color 是端点节点颜色。
 *
 * 返回值：
 *   无返回值。
 */
static void draw_circuit_trace(struct framebuffer_device *fb,
                               int x,
                               int y,
                               int horizontal_len,
                               int vertical_len,
                               struct rgb_color color,
                               struct rgb_color node_color)
{
    /* line 是电路线宽，适当比背景网格更粗，保证 LCD 上可见。 */
    int line = scale_ref_min(fb, 2);
    /* node 是电路线端点节点的方块尺寸，用于模拟 HTML 中的发光节点。 */
    int node = scale_ref_min(fb, 7);
    /* end_x 是水平电路线末端，也是竖向分支线所在的 x 坐标。 */
    int end_x = x + horizontal_len;
    /* vertical_y 是竖向分支的上边界，兼容向上和向下两种分支方向。 */
    int vertical_y = vertical_len >= 0 ? y : y + vertical_len;

    draw_hline(fb, x, y, horizontal_len, line, color);
    draw_vline(fb, end_x, vertical_y, vertical_len >= 0 ? vertical_len : -vertical_len, line, color);
    fill_rect(fb, end_x - node / 2, y - node / 2, node, node, node_color);
    fill_rect(fb,
              end_x - node / 2,
              y + vertical_len - node / 2,
              node,
              node,
              node_color);
}

/*
 * draw_chip_mark 的作用：
 *   用矩形、针脚和点阵文字绘制中央 9TH 芯片 Logo，对应 HTML 预览稿中的主视觉。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   x/y 是芯片主体左上角。
 *   size 是芯片主体边长。
 *   body 是芯片主体颜色。
 *   border 是芯片边框和针脚颜色。
 *   text_color 是 9TH 文本颜色。
 *
 * 返回值：
 *   无返回值。
 */
static void draw_chip_mark(struct framebuffer_device *fb,
                           int x,
                           int y,
                           int size,
                           struct rgb_color body,
                           struct rgb_color border,
                           struct rgb_color text_color)
{
    /* i 是针脚循环索引，按四边同步绘制芯片针脚。 */
    int i;
    /* pin_count 是每条边的针脚数量，控制芯片图形的复杂度和可读性。 */
    int pin_count = 5;
    /* pin_w 是针脚短边宽度，随屏幕缩放但至少 1 像素。 */
    int pin_w = scale_ref_min(fb, 5);
    /* pin_h 是针脚向外延伸长度，增强芯片轮廓识别度。 */
    int pin_h = scale_ref_min(fb, 13);
    /* gap 是相邻针脚中心点之间的间距，按芯片主体尺寸均匀分配。 */
    int gap = size / (pin_count + 1);
    /* inner 是芯片内部细边框缩进，避免主体看起来像普通色块。 */
    int inner = scale_ref_min(fb, 10);

    fill_rect(fb, x, y, size, size, body);
    draw_rect_border(fb, x, y, size, size, border, scale_ref_min(fb, 3));
    draw_rect_border(fb,
                     x + inner,
                     y + inner,
                     size - inner * 2,
                     size - inner * 2,
                     (struct rgb_color){ 116, 188, 213 },
                     scale_ref_min(fb, 1));

    for (i = 1; i <= pin_count; ++i) {
        /* px 是上下两边当前针脚的 x 坐标。 */
        int px = x + i * gap - pin_w / 2;
        /* py 是左右两边当前针脚的 y 坐标。 */
        int py = y + i * gap - pin_w / 2;

        fill_rect(fb, px, y - pin_h, pin_w, pin_h, border);
        fill_rect(fb, px, y + size, pin_w, pin_h, border);
        fill_rect(fb, x - pin_h, py, pin_h, pin_w, border);
        fill_rect(fb, x + size, py, pin_h, pin_w, border);
    }

    draw_text(fb,
              x + size / 2 - text_width("9TH", 5) / 2,
              y + size / 2 - scale_ref_y(fb, 18),
              "9TH",
              5,
              text_color);
}

/*
 * draw_badge 的作用：
 *   绘制底部技术铭牌，替代旧进度条，让启动图更像品牌静态首帧。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *   x/y/w/h 描述铭牌位置。
 *   text 是铭牌文本。
 *   border 是边框颜色。
 *   bg 是背景颜色。
 *   text_color 是文字颜色。
 *
 * 返回值：
 *   无返回值。
 */
static void draw_badge(struct framebuffer_device *fb,
                       int x,
                       int y,
                       int w,
                       int h,
                       const char *text,
                       struct rgb_color border,
                       struct rgb_color bg,
                       struct rgb_color text_color)
{
    fill_rect(fb, x, y, w, h, bg);
    draw_rect_border(fb, x, y, w, h, border, scale_ref_min(fb, 1));
    draw_text(fb,
              x + (w - text_width(text, 1)) / 2,
              y + (h - scale_ref_y(fb, 7)) / 2,
              text,
              1,
              text_color);
}

/*
 * draw_splash_fallback 的作用：
 *   在 RGB565 资源缺失时绘制一个低保真 AI 竞赛品牌静态画面，避免开机阶段黑屏。
 *
 * 主要流程：
 *   1. 绘制深蓝科技背景、电路线、网格和弱化芯片轮廓。
 *   2. 绘制顶部赛事条，使用英文点阵承载“第九届嵌入式芯片与系统设计竞赛”的屏幕可见版本。
 *   3. 绘制中央白色 Logo 面板、9TH 芯片图形、AI 设计英文标语和机器视觉 ROI 线稿。
 *   4. 绘制底部系统名称和三枚技术铭牌，替代旧版进度条，避免启动画面像加载控件。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *
 * 返回值：
 *   无返回值；绘制结果保留在 framebuffer 中，直到 Qt/DRM 后续接管显示。
 */
static void draw_splash_fallback(struct framebuffer_device *fb)
{
    /* 背景色采用深蓝黑，和 HTML 预览稿的科技底色保持一致。 */
    const struct rgb_color background = { 7, 14, 31 };
    /* 背景底部色块让画面有上下层次，避免纯色显得空。 */
    const struct rgb_color background_low = { 10, 25, 48 };
    /* 网格线使用低亮度蓝色，只提供结构感，不抢主 Logo。 */
    const struct rgb_color grid = { 22, 55, 82 };
    /* 主青色用于电路线、赛事栏边框和关键光带。 */
    const struct rgb_color cyan = { 70, 214, 255 };
    /* 次青色用于大面积背景线条，降低视觉干扰。 */
    const struct rgb_color cyan_soft = { 37, 131, 168 };
    /* 暖黄色用于届数和局部强调，呼应 HTML 预览稿的暖色点缀。 */
    const struct rgb_color warm = { 255, 210, 96 };
    /* 暖色弱化版本用于右上背景电路线。 */
    const struct rgb_color warm_soft = { 160, 132, 70 };
    /* 主面板底色是深蓝卡片，承托内部白色 Logo 区。 */
    const struct rgb_color hero_bg = { 11, 30, 58 };
    /* 主面板阴影用更深色模拟层叠关系。 */
    const struct rgb_color hero_shadow = { 3, 8, 20 };
    /* 白色 Logo 面板复刻 HTML 中参考图主体的白底视觉。 */
    const struct rgb_color logo_bg = { 243, 247, 250 };
    /* Logo 面板边框用浅蓝灰，避免纯白边缘贴背景。 */
    const struct rgb_color logo_border = { 166, 190, 207 };
    /* 深色文字用于白底面板上的品牌文案。 */
    const struct rgb_color ink = { 15, 44, 72 };
    /* 次级文字用于说明文案和顶部角标。 */
    const struct rgb_color muted = { 154, 178, 196 };
    /* 面板亮色文字用于深色背景上的标题。 */
    const struct rgb_color light_text = { 238, 247, 252 };
    /* 芯片主体蓝色对应 HTML 预览稿中 9th 芯片 Logo 的高饱和蓝。 */
    const struct rgb_color chip_body = { 18, 128, 189 };
    /* 技术铭牌背景保持深色，和底部状态区融为一体。 */
    const struct rgb_color badge_bg = { 13, 38, 67 };
    /* 技术铭牌边框用低亮青色，弱化控件感。 */
    const struct rgb_color badge_border = { 72, 150, 180 };
    /* 线宽按当前屏幕分辨率缩放，保证 1024x600 和其它尺寸都可读。 */
    const int line = scale_ref_min(fb, 1);
    /* 粗线用于主面板和 ROI 角标，让关键结构在 LCD 上更清楚。 */
    const int thick = scale_ref_min(fb, 2);

    /* 主面板位置参考 HTML 预览稿，给底部状态区留出空间。 */
    int hero_x = scale_ref_x(fb, 132);
    int hero_y = scale_ref_y(fb, 82);
    int hero_w = scale_ref_x(fb, 760);
    int hero_h = scale_ref_y(fb, 402);

    /* 顶部赛事条放在主面板上方区域，形成第一视觉锚点。 */
    int ribbon_x = hero_x + scale_ref_x(fb, 62);
    int ribbon_y = hero_y + scale_ref_y(fb, 28);
    int ribbon_w = hero_w - scale_ref_x(fb, 124);
    int ribbon_h = scale_ref_y(fb, 54);

    /* 白色 Logo 面板承载芯片图形和 AI 标语，是整张启动图的视觉中心。 */
    int logo_x = hero_x + scale_ref_x(fb, 86);
    int logo_y = hero_y + scale_ref_y(fb, 118);
    int logo_w = hero_w - scale_ref_x(fb, 172);
    int logo_h = scale_ref_y(fb, 178);

    /* 芯片 Logo 放在白色面板左侧，与右侧英文标语形成组合。 */
    int chip_size = scale_ref_min(fb, 98);
    int chip_x = logo_x + scale_ref_x(fb, 58);
    int chip_y = logo_y + (logo_h - chip_size) / 2;

    /* 右侧 ROI 线稿故意退到背景层，表达机器视觉主题但不干扰主 Logo。 */
    int roi_x = hero_x + hero_w - scale_ref_x(fb, 166);
    int roi_y = hero_y + scale_ref_y(fb, 214);
    int roi_w = scale_ref_x(fb, 122);
    int roi_h = scale_ref_y(fb, 86);

    /* 底部状态区承接 Qt 启动前的系统身份，不再显示加载进度。 */
    int status_y = scale_ref_y(fb, 510);
    int badge_y = scale_ref_y(fb, 510);
    int badge_h = scale_ref_y(fb, 30);
    int badge_gap = scale_ref_x(fb, 12);
    int badge1_w = scale_ref_x(fb, 122);
    int badge2_w = scale_ref_x(fb, 150);
    int badge3_w = scale_ref_x(fb, 136);
    int badge_x = (int)fb->var.xres - scale_ref_x(fb, 70) - badge1_w - badge2_w - badge3_w - badge_gap * 2;

    fill_rect(fb, 0, 0, (int)fb->var.xres, (int)fb->var.yres, background);
    fill_rect(fb, 0, scale_ref_y(fb, 392), (int)fb->var.xres, scale_ref_y(fb, 208), background_low);

    draw_grid(fb,
              scale_ref_x(fb, 54),
              scale_ref_y(fb, 76),
              scale_ref_x(fb, 916),
              scale_ref_y(fb, 420),
              12,
              6,
              grid);

    draw_circuit_trace(fb,
                       scale_ref_x(fb, 72),
                       scale_ref_y(fb, 104),
                       scale_ref_x(fb, 252),
                       scale_ref_y(fb, 78),
                       cyan_soft,
                       cyan);
    draw_circuit_trace(fb,
                       scale_ref_x(fb, 684),
                       scale_ref_y(fb, 138),
                       scale_ref_x(fb, 268),
                       scale_ref_y(fb, 96),
                       warm_soft,
                       warm);
    draw_circuit_trace(fb,
                       scale_ref_x(fb, 96),
                       scale_ref_y(fb, 468),
                       scale_ref_x(fb, 340),
                       -scale_ref_y(fb, 70),
                       cyan_soft,
                       cyan);

    draw_rect_border(fb,
                     scale_ref_x(fb, 72),
                     scale_ref_y(fb, 164),
                     scale_ref_x(fb, 112),
                     scale_ref_y(fb, 112),
                     (struct rgb_color){ 27, 83, 112 },
                     line);
    draw_rect_border(fb,
                     scale_ref_x(fb, 804),
                     scale_ref_y(fb, 300),
                     scale_ref_x(fb, 118),
                     scale_ref_y(fb, 104),
                     (struct rgb_color){ 40, 86, 98 },
                     line);

    fill_rect(fb, scale_ref_x(fb, 28), scale_ref_y(fb, 24), scale_ref_x(fb, 220), scale_ref_y(fb, 24), hero_shadow);
    draw_rect_border(fb,
                     scale_ref_x(fb, 28),
                     scale_ref_y(fb, 24),
                     scale_ref_x(fb, 220),
                     scale_ref_y(fb, 24),
                     (struct rgb_color){ 38, 93, 128 },
                     line);
    draw_text(fb,
              scale_ref_x(fb, 40),
              scale_ref_y(fb, 32),
              "STM32MP157 BOOT VISUAL PREVIEW",
              1,
              muted);

    fill_rect(fb, hero_x + scale_ref_x(fb, 8), hero_y + scale_ref_y(fb, 10), hero_w, hero_h, hero_shadow);
    fill_rect(fb, hero_x, hero_y, hero_w, hero_h, hero_bg);
    draw_rect_border(fb, hero_x, hero_y, hero_w, hero_h, (struct rgb_color){ 54, 118, 152 }, thick);
    draw_rect_border(fb,
                     hero_x + scale_ref_x(fb, 10),
                     hero_y + scale_ref_y(fb, 10),
                     hero_w - scale_ref_x(fb, 20),
                     hero_h - scale_ref_y(fb, 20),
                     (struct rgb_color){ 17, 58, 94 },
                     line);

    fill_rect(fb, ribbon_x, ribbon_y, ribbon_w, ribbon_h, (struct rgb_color){ 8, 48, 84 });
    draw_rect_border(fb, ribbon_x, ribbon_y, ribbon_w, ribbon_h, cyan, line);
    draw_hline(fb,
               ribbon_x + scale_ref_x(fb, 18),
               ribbon_y + ribbon_h - scale_ref_y(fb, 8),
               ribbon_w - scale_ref_x(fb, 36),
               scale_ref_min(fb, 2),
               warm);
    draw_text_center(fb,
                     ribbon_y + scale_ref_y(fb, 17),
                     "9TH EMBEDDED CHIP & SYSTEM DESIGN CONTEST",
                     2,
                     light_text);

    fill_rect(fb, logo_x, logo_y, logo_w, logo_h, logo_bg);
    draw_rect_border(fb, logo_x, logo_y, logo_w, logo_h, logo_border, thick);
    draw_hline(fb,
               logo_x + scale_ref_x(fb, 26),
               logo_y + scale_ref_y(fb, 20),
               logo_w - scale_ref_x(fb, 52),
               scale_ref_min(fb, 3),
               cyan);
    draw_chip_mark(fb, chip_x, chip_y, chip_size, chip_body, cyan, (struct rgb_color){ 255, 255, 255 });
    draw_text(fb,
              logo_x + scale_ref_x(fb, 210),
              logo_y + scale_ref_y(fb, 44),
              "AI DESIGN",
              4,
              ink);
    draw_text(fb,
              logo_x + scale_ref_x(fb, 212),
              logo_y + scale_ref_y(fb, 100),
              "AI FOR DESIGN &",
              2,
              (struct rgb_color){ 26, 104, 164 });
    draw_text(fb,
              logo_x + scale_ref_x(fb, 212),
              logo_y + scale_ref_y(fb, 130),
              "DESIGN FOR AI!",
              2,
              (struct rgb_color){ 26, 104, 164 });

    draw_corner_brackets(fb, roi_x, roi_y, roi_w, roi_h, scale_ref_x(fb, 30), thick, (struct rgb_color){ 68, 202, 226 });
    draw_hline(fb, roi_x + scale_ref_x(fb, 20), roi_y + roi_h / 2, roi_w - scale_ref_x(fb, 40), line, (struct rgb_color){ 51, 125, 150 });
    draw_vline(fb, roi_x + roi_w / 2, roi_y + scale_ref_y(fb, 16), roi_h - scale_ref_y(fb, 32), line, (struct rgb_color){ 51, 125, 150 });
    draw_text(fb, roi_x + scale_ref_x(fb, 18), roi_y + roi_h + scale_ref_y(fb, 12), "VISION ROI", 1, muted);

    draw_text(fb,
              scale_ref_x(fb, 78),
              status_y,
              SPLASH_SYSTEM_EN,
              2,
              light_text);
    draw_text(fb,
              scale_ref_x(fb, 80),
              status_y + scale_ref_y(fb, 34),
              "FRAMEBUFFER HANDOFF FOR QT DISPLAY SURFACE",
              1,
              muted);

    draw_badge(fb, badge_x, badge_y, badge1_w, badge_h, SPLASH_BADGE_EDGE_AI, badge_border, badge_bg, light_text);
    draw_badge(fb,
               badge_x + badge1_w + badge_gap,
               badge_y,
               badge2_w,
               badge_h,
               SPLASH_BADGE_FRAMEBUFFER,
               badge_border,
               badge_bg,
               light_text);
    draw_badge(fb,
               badge_x + badge1_w + badge2_w + badge_gap * 2,
               badge_y,
               badge3_w,
               badge_h,
               SPLASH_BADGE_QT,
               badge_border,
               badge_bg,
               light_text);
}

/*
 * main 的作用：
 *   程序入口，负责解析参数、打开 framebuffer、绘制静态启动首帧并释放资源。
 *
 * 参数：
 *   argc/argv 是命令行参数。
 *
 * 返回值：
 *   EXIT_SUCCESS 表示静态首帧已经写入 framebuffer；
 *   EXIT_FAILURE 表示参数、设备、映射或刷新过程失败。
 */
int main(int argc, char *argv[])
{
    struct app_config cfg;
    struct framebuffer_device fb;
    int parse_ret;
    int used_asset;

    parse_ret = parse_args(argc, argv, &cfg);
    if (parse_ret > 0) {
        return EXIT_SUCCESS;
    }
    if (parse_ret < 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (open_framebuffer(cfg.fb_device, &fb) != 0) {
        return EXIT_FAILURE;
    }

    used_asset = (draw_splash_asset(&fb, cfg.asset_path) == 0);
    if (!used_asset) {
        draw_splash_fallback(&fb);
    }

    if (msync(fb.mem, fb.mem_len, MS_SYNC) != 0) {
        perror("刷新 framebuffer 显存失败");
        close_framebuffer(&fb);
        return EXIT_FAILURE;
    }

    if (fsync(fb.fd) != 0) {
        perror("同步 framebuffer 文件描述符失败");
        close_framebuffer(&fb);
        return EXIT_FAILURE;
    }

    if (!cfg.quiet) {
        printf("fb_boot_splash: 已绘制 %s / %s / %s / %s 到 %s (%ux%u, %u bpp, %s: %s)\n",
               SPLASH_CONTEST_CN,
               SPLASH_CONTEST_EN,
               SPLASH_TITLE_CN,
               SPLASH_BRAND_EN,
               cfg.fb_device,
               fb.var.xres,
               fb.var.yres,
               fb.var.bits_per_pixel,
               used_asset ? "asset" : "fallback",
               used_asset ? cfg.asset_path : "draw_splash_fallback");
    }

    close_framebuffer(&fb);
    return EXIT_SUCCESS;
}
