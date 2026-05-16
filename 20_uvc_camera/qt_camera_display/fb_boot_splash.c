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
 *   3. mmap 映射 LCD 显存，按当前像素格式写入背景、检测窗口、ROI、扫描线和进度条。
 *   4. 调用 msync/fsync 尽快把静态首帧刷新到屏幕，然后释放映射并退出。
 *
 * 关键说明：
 *   - 本程序不依赖 Qt、OpenGL、DRM、图片解码库或字体库，适合放到 Buildroot SysV init
 *     的早期阶段运行，用来填补 Qt 启动画面出现前的黑屏时间。
 *   - 早期 framebuffer 阶段只内置 5x7 ASCII 点阵字体，因此首帧用英文标题表达
 *     “工业缺陷检测系统”；Qt 真正启动后，QML splashOverlay 会继续显示中文标题
 *     “工业缺陷检测系统”和完整动画。
 *
 * 参数：
 *   -f /dev/fb0  指定 framebuffer 设备节点，默认 /dev/fb0。
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

/* REF_WIDTH 是 QML 主界面的设计宽度，早期静态图按它做比例换算。 */
#define REF_WIDTH 1024

/* REF_HEIGHT 是 QML 主界面的设计高度，和 7 寸 RGB 屏 1024x600 对齐。 */
#define REF_HEIGHT 600

/* SPLASH_TITLE_CN 保留 QML 启动画面的中文标题语义，日志和静态检查会用到它。 */
static const char *SPLASH_TITLE_CN = "工业缺陷检测系统";

/* SPLASH_SUBTITLE_EN 与 QML splashOverlay 的英文副标题保持一致，便于视觉衔接。 */
static const char *SPLASH_SUBTITLE_EN = "STM32MP157 Vision Inspection Terminal";

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
 * quiet 为 1 时不输出普通日志，避免 init 脚本污染控制台。
 */
struct app_config {
    const char *fb_device;
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
    printf("用法: %s [-f /dev/fb0] [-q] [-h]\n", prog);
    printf("  -f  指定 framebuffer 设备，默认 %s\n", DEFAULT_FB_DEVICE);
    printf("  -q  静默绘制，不输出普通日志，适合 init 脚本调用\n");
    printf("  -h  显示帮助\n");
}

/*
 * parse_args 的作用：
 *   解析命令行参数，得到 framebuffer 节点和日志模式。
 *
 * 主要流程：
 *   1. 设置默认 `/dev/fb0`。
 *   2. 逐个解析 `-f`、`-q` 和 `-h`。
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
    cfg->quiet = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-f") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "错误：-f 缺少 framebuffer 设备路径\n");
                return -1;
            }
            cfg->fb_device = argv[++i];
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
 *   绘制一个实心矩形，是背景、面板、进度条和标签的基础绘制函数。
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
 *   绘制矩形边框，用于模拟 QML 启动画面的检测窗口、ROI 框和进度条轨道。
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
 *   绘制低对比度工业坐标网格，模拟 QML splashOverlay 中的 subtleGrid。
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
    int i;
    int line = scale_ref_min(fb, 1);

    for (i = 0; i <= columns; ++i) {
        int gx = x + i * w / columns;
        fill_rect(fb, gx, y, line, h, color);
    }

    for (i = 0; i <= rows; ++i) {
        int gy = y + i * h / rows;
        fill_rect(fb, x, gy, w, line, color);
    }
}

/*
 * draw_splash 的作用：
 *   绘制与 QML 启动动画第一帧风格一致的静态画面。
 *
 * 主要流程：
 *   1. 绘制深色背景和低对比度网格。
 *   2. 绘制系统标题、副标题和居中的检测窗口。
 *   3. 绘制 ROI 框、扫描线、SELF CHECK 标识和首阶段进度条。
 *   4. 使用 18% 初始进度，对应 QML splashStageModel 第一阶段“加载相机”。
 *
 * 参数：
 *   fb 是 framebuffer 状态。
 *
 * 返回值：
 *   无返回值；绘制结果保留在 framebuffer 中，直到 Qt/DRM 后续接管显示。
 */
static void draw_splash(struct framebuffer_device *fb)
{
    const struct rgb_color background = { 11, 13, 15 };
    const struct rgb_color grid = { 38, 48, 57 };
    const struct rgb_color title = { 244, 247, 248 };
    const struct rgb_color subtitle = { 148, 163, 173 };
    const struct rgb_color panel = { 5, 6, 6 };
    const struct rgb_color panel_border = { 52, 64, 71 };
    const struct rgb_color inner_border = { 32, 39, 44 };
    const struct rgb_color green = { 53, 208, 127 };
    const struct rgb_color muted = { 154, 166, 173 };
    const struct rgb_color track = { 26, 32, 36 };
    const struct rgb_color track_border = { 48, 57, 64 };
    const struct rgb_color label_bg = { 16, 22, 25 };
    const struct rgb_color check_bg = { 19, 33, 25 };
    const int line = scale_ref_min(fb, 1);
    const int thick = scale_ref_min(fb, 2);

    int grid_x = scale_ref_x(fb, 72);
    int grid_y = scale_ref_y(fb, 90);
    int grid_w = scale_ref_x(fb, 880);
    int grid_h = scale_ref_y(fb, 420);

    int viewport_w = scale_ref_x(fb, 430);
    int viewport_h = scale_ref_y(fb, 232);
    int viewport_x = ((int)fb->var.xres - viewport_w) / 2;
    int viewport_y = scale_ref_y(fb, 158);

    int roi_w = scale_ref_x(fb, 244);
    int roi_h = scale_ref_y(fb, 126);
    int roi_x = viewport_x + (viewport_w - roi_w) / 2;
    int roi_y = viewport_y + (viewport_h - roi_h) / 2;

    int progress_w = scale_ref_x(fb, 520);
    int progress_h = scale_ref_y(fb, 12);
    int progress_x = ((int)fb->var.xres - progress_w) / 2;
    int progress_y = scale_ref_y(fb, 486);

    fill_rect(fb, 0, 0, (int)fb->var.xres, (int)fb->var.yres, background);

    draw_grid(fb, grid_x, grid_y, grid_w, grid_h, 11, 6, grid);

    draw_text_center(fb, scale_ref_y(fb, 64), "VISION INSPECTION SYSTEM", 3, title);
    draw_text_center(fb, scale_ref_y(fb, 112), "STM32MP157 VISION INSPECTION TERMINAL", 2, subtitle);

    fill_rect(fb, viewport_x, viewport_y, viewport_w, viewport_h, panel);
    draw_rect_border(fb, viewport_x, viewport_y, viewport_w, viewport_h, panel_border, line);
    draw_rect_border(fb,
                     viewport_x + scale_ref_x(fb, 12),
                     viewport_y + scale_ref_y(fb, 12),
                     viewport_w - scale_ref_x(fb, 24),
                     viewport_h - scale_ref_y(fb, 24),
                     inner_border,
                     line);

    fill_rect(fb,
              viewport_x + scale_ref_x(fb, 26),
              viewport_y + scale_ref_y(fb, 24),
              viewport_w - scale_ref_x(fb, 52),
              scale_ref_y(fb, 3),
              green);

    draw_rect_border(fb, roi_x, roi_y, roi_w, roi_h, green, thick);

    fill_rect(fb,
              viewport_x + scale_ref_x(fb, 28),
              viewport_y + viewport_h - scale_ref_y(fb, 38),
              scale_ref_x(fb, 132),
              scale_ref_y(fb, 18),
              label_bg);
    draw_rect_border(fb,
                     viewport_x + scale_ref_x(fb, 28),
                     viewport_y + viewport_h - scale_ref_y(fb, 38),
                     scale_ref_x(fb, 132),
                     scale_ref_y(fb, 18),
                     panel_border,
                     line);
    draw_text(fb,
              viewport_x + scale_ref_x(fb, 44),
              viewport_y + viewport_h - scale_ref_y(fb, 34),
              "ROI 320X180",
              1,
              muted);

    fill_rect(fb,
              viewport_x + viewport_w - scale_ref_x(fb, 134),
              viewport_y + viewport_h - scale_ref_y(fb, 38),
              scale_ref_x(fb, 106),
              scale_ref_y(fb, 18),
              check_bg);
    draw_rect_border(fb,
                     viewport_x + viewport_w - scale_ref_x(fb, 134),
                     viewport_y + viewport_h - scale_ref_y(fb, 38),
                     scale_ref_x(fb, 106),
                     scale_ref_y(fb, 18),
                     green,
                     line);
    draw_text(fb,
              viewport_x + viewport_w - scale_ref_x(fb, 120),
              viewport_y + viewport_h - scale_ref_y(fb, 34),
              "SELF CHECK",
              1,
              title);

    draw_text_center(fb, scale_ref_y(fb, 420), "LOADING CAMERA", 3, green);
    draw_text_center(fb,
                     scale_ref_y(fb, 456),
                     "INITIALIZING UVC CAPTURE AND KMS DISPLAY PATH",
                     1,
                     subtitle);

    fill_rect(fb, progress_x, progress_y, progress_w, progress_h, track);
    draw_rect_border(fb, progress_x, progress_y, progress_w, progress_h, track_border, line);
    fill_rect(fb, progress_x, progress_y, progress_w * 18 / 100, progress_h, green);

    draw_text(fb, progress_x, progress_y + scale_ref_y(fb, 22), "BOOT SELF CHECK", 1, subtitle);
    draw_text(fb,
              progress_x + progress_w - text_width("18%", 1),
              progress_y + scale_ref_y(fb, 22),
              "18%",
              1,
              title);
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

    draw_splash(&fb);

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
        printf("fb_boot_splash: 已绘制 %s / %s 到 %s (%ux%u, %u bpp)\n",
               SPLASH_TITLE_CN,
               SPLASH_SUBTITLE_EN,
               cfg.fb_device,
               fb.var.xres,
               fb.var.yres,
               fb.var.bits_per_pixel);
    }

    close_framebuffer(&fb);
    return EXIT_SUCCESS;
}
