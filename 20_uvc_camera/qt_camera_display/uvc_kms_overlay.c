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

/* 自动视觉定位的最小连通域面积，120px 允许铝色零件刚入画，同时过滤黑色传送带上的小反光点。 */
#define AUTO_LOCATE_MIN_COMPONENT_AREA 120U

/* 自动视觉定位的最大连通域面积比例分母，避免把整片背景误判成零件。 */
#define AUTO_LOCATE_MAX_COMPONENT_AREA_DIVISOR 2U

/* 自动视觉定位的最小外接框边长，太窄的亮线或暗线不作为完整零件。 */
#define AUTO_LOCATE_MIN_BBOX_SIDE 12U

/* 自动视觉定位的最大外接框边长，首版零件必须小于中心 ROI 的大部分区域。 */
#define AUTO_LOCATE_MAX_BBOX_SIDE 260U

/* 自动视觉定位的大候选局部收缩最小窗口边长；小于该尺寸不认为能稳定覆盖真实垫圈。 */
#define AUTO_LOCATE_OVERSIZE_REFINE_MIN_SIDE 80U

/* 自动视觉定位的大候选局部收缩最大窗口边长；保持小于 QML 45000px² 最大目标面积门槛。 */
#define AUTO_LOCATE_OVERSIZE_REFINE_MAX_SIDE 210U

/* 自动视觉定位的大候选环孔中心扫描步长，兼顾 10fps overlay 实时性和中心定位精度。 */
#define AUTO_LOCATE_OVERSIZE_REFINE_SCAN_STEP 6U

/* 自动视觉定位的大候选环孔支撑最小半径，用于过滤传送带小突起和局部高光。 */
#define AUTO_LOCATE_OVERSIZE_REFINE_MIN_RADIUS 26U

/* 自动视觉定位的大候选环孔支撑半径递增步长，避免每个像素半径都扫描导致 CPU 过高。 */
#define AUTO_LOCATE_OVERSIZE_REFINE_RADIUS_STEP 10U

/* 自动视觉定位的大候选环孔支撑最小厚度，防止支撑采样带太薄而被单行噪声影响。 */
#define AUTO_LOCATE_OVERSIZE_REFINE_MIN_SUPPORT_THICKNESS 4U

/* 自动视觉定位的最低实体密度百分比，过低通常是噪声边线，不是完整零件。 */
#define AUTO_LOCATE_MIN_SOLID_DENSITY_PERCENT 8U

/* 自动视觉定位的最高实体密度百分比，过高通常是黑色传送带或整片阴影，不是带孔垫圈。 */
#define AUTO_LOCATE_MAX_SOLID_DENSITY_PERCENT 78U

/* 自动视觉定位的中心孔采样分母，4 表示取 bbox 中央约 1/4 宽高区域判断垫圈中心孔。 */
#define AUTO_LOCATE_CENTER_HOLE_SAMPLE_DIVISOR 4U

/* 自动视觉定位的中心孔背景占比阈值，中央采样区至少 25% 不是候选亮度才认为像垫圈孔（放宽以提高银色零件识别率）。 */
#define AUTO_LOCATE_MIN_CENTER_HOLE_BACKGROUND_PERCENT 25U

/* 自动视觉定位的暗候选占比上限；灰色传送带暗纹理斑块可能低于 82%，降到 65% 更积极拒绝。 */
#define AUTO_LOCATE_MAX_DARK_FILL_PERCENT 65U

/* 自动视觉定位的暗色贴边判断边距；贴近搜索带边缘的大暗区通常是传送带背景。 */
#define AUTO_LOCATE_DARK_EDGE_MARGIN_PX 8U

/* 自动视觉定位的暗色面积上限；暗候选面积超过搜索区域 12% 时优先按背景误检处理。 */
#define AUTO_LOCATE_MAX_DARK_EDGE_AREA_PERCENT 12U

/* 自动视觉定位的中心孔背景最小对比度；孔区域与候选实体太接近时不认为是垫圈孔。 */
#define AUTO_LOCATE_MIN_RING_BACKGROUND_CONTRAST 10U

/* 自动视觉定位的中心孔四边主体支撑阈值；四边任一方向主体太少时，按白边夹黑带或皮带突起误检处理。 */
#define AUTO_LOCATE_MIN_RING_SIDE_BODY_PERCENT 18U

/* 自动视觉定位的 ring 结构最小 bbox 边长；过小候选即使中心发暗，也更像传送带突起而不是完整垫圈。 */
#define AUTO_LOCATE_MIN_RING_BBOX_SIDE 45U

/* 自动视觉定位的真实零件最小外接框面积，小于该面积的亮斑按传送带反光或凸起噪声处理。 */
#define AUTO_LOCATE_MIN_PART_BBOX_AREA 900U

/* 自动视觉定位的真实零件最小外接框边长，小于该边长的候选不足以作为稳定上料目标。 */
#define AUTO_LOCATE_MIN_PART_BBOX_SIDE 20U

/* 自动视觉定位的最低接收置信度，低置信候选即使满足面积也不能返回 has_target=1。 */
#define AUTO_LOCATE_MIN_ACCEPT_CONFIDENCE 52U

/* 自动视觉定位的无环孔候选最低置信度，无中心孔时必须更像完整零件而不是传送带高光。 */
#define AUTO_LOCATE_MIN_NON_RING_CONFIDENCE 72U

/* 自动视觉定位的无环孔候选最小边长，未看到中心孔时必须等零件主体足够大再进入跟踪。 */
#define AUTO_LOCATE_RING_REQUIRED_BBOX_SIDE 38U

/*
 * 自动视觉定位是否在 overlay 端强制要求候选具备垫圈中心孔结构。
 *
 * 现场当前目标是波形垫圈、平垫圈和弹性垫圈，三类零件都应呈现环形/孔洞结构。
 * 但是当前现场发现：同一只垫圈静止在绿色 ROI 内时，中心孔会被曝光、高光、阴影或模糊短暂压没。
 * 如果 overlay 端直接拒绝所有 no-ring 候选，目标一旦丢帧就可能持续 has_target=0。
 * 因此这里设为 0：overlay 允许尺寸和置信度足够高的 no-ring 候选回传；
 * QML 首次建链时让 ring=1 快速确认；ring=0 高置信候选走更长多帧确认，
 * 跟踪阶段再利用高置信候选提高稳定性。
 */
#define AUTO_LOCATE_REQUIRE_RING_HOLE_FOR_TARGET 0U

/*
 * 自动视觉定位的最小色度能量阈值。
 *
 * YUYV 中 U=128/V=128 表示消色差（灰色/黑色/白色）。
 * 蓝色金属零件的 |U-128| + |V-128| 通常 > 30。
 * 灰色传送带的 |U-128| + |V-128| 通常 < 10。
 * 设为 15 可有效拒绝传送带纹理误检，同时不误杀有颜色的零件。
 *
 * 注意：银色/铝色零件同样为消色差，此过滤器会误杀银色零件。
 * 当检测银色零件时应禁用此过滤器，改用圆度过滤。
 * 设为 0 表示禁用色度过滤。
 */
#define AUTO_LOCATE_MIN_CHROMA_ENERGY 0U

/*
 * 自动视觉定位的最小圆度阈值（百分比，0~100）。
 *
 * 圆度 = 4π × area / perimeter² × 100
 * 注意：环形零件（donut）包含外+内两条边界，数学圆度天然只有 15~33%，
 * 加上 BFS 边界计数膨胀，实测环形只有 10~20%。
 * 设为 0 禁用此过滤器，改用亮度主导+环孔检测来区分零件和传送带纹理。
 */
#define AUTO_LOCATE_MIN_CIRCULARITY_PERCENT 0U

/*
 * 自动视觉定位的最小亮像素占比百分比。
 *
 * 银白色零件在摄像头下呈现为亮候选（高于 bright_threshold）。
 * 当前 LOCATE 已改为“只用亮金属主体做连通域”，该阈值保留为防回退契约：
 * 如果后续又把暗像素加入候选，亮像素占比必须继续用于拒绝黑色传送带暗斑。
 * 设为 0 禁用此过滤器。
 */
#define AUTO_LOCATE_MIN_BRIGHT_FILL_PERCENT 40U

/*
 * 自动视觉定位的基础亮度差阈值。
 *
 * 铝色零件在黑色传送带上主要表现为“略亮于背景”的金属主体，
 * 若阈值过高，会造成“肉眼已经入画，但 LOCATE 间歇返回 has_target=0”的漏检。
 * 这里保持 12 作为最小差值，让低对比铝色边缘也能进入后续形状过滤。
 */
#define AUTO_LOCATE_MIN_LUMA_DELTA 12U

/*
 * 自动视觉定位的最大自适应亮度差。
 *
 * 现场画面里可能有白色支架、线缆或局部高光。若直接用 max-min 计算阈值，
 * 这些极端亮点会把 bright_threshold 拉得过高，导致铝色垫圈明明在黑色传送带上却识别不到。
 * 因此 LOCATE 使用亮度直方图的中位数和百分位差，并把单次阈值增量限制到 45。
 */
#define AUTO_LOCATE_MAX_LUMA_DELTA 45U

/* 自动视觉定位的金属主体最小亮度差，低于该差值会把传送带噪声并入零件主体。 */
#define AUTO_LOCATE_MIN_BODY_LUMA_DELTA 10U

/*
 * 自动视觉定位的金属主体扩张比例。
 *
 * bright_threshold 只作为“高可信种子”，body_threshold 用较低阈值把同一垫圈上
 * 被阴影压暗的银色环面并入连通域。65% 能连接真实垫圈亮弧，同时仍明显高于黑色皮带背景。
 */
#define AUTO_LOCATE_BODY_LUMA_DELTA_PERCENT 65U

/* 自动视觉定位亮度直方图桶数量；YUYV 的 Y 分量刚好是 0~255。 */
#define AUTO_LOCATE_LUMA_HISTOGRAM_BUCKETS 256U

/* 自动视觉定位查找黑色传送带时，每隔 4 个像素采样一次行亮度，降低 LOCATE 的 CPU 开销。 */
#define AUTO_LOCATE_BELT_ROW_SAMPLE_STEP 4U

/* 自动视觉定位查找黑色传送带时，使用行平均亮度的 35 分位作为暗背景基准，避免白色支架污染阈值。 */
#define AUTO_LOCATE_BELT_ROW_PERCENTILE 35U

/* 自动视觉定位查找黑色传送带时，允许行亮度比暗背景基准高 28，覆盖铝件入画后的局部增亮。 */
#define AUTO_LOCATE_BELT_ROW_LUMA_MARGIN 28U

/* 自动视觉定位查找黑色传送带时，行暗阈值最低保持 85，避免单个极暗噪声点把阈值压得过低。 */
#define AUTO_LOCATE_BELT_MIN_ROW_THRESHOLD 85U

/* 自动视觉定位查找黑色传送带时，采样点中至少 45% 为暗像素即可认为该行主要属于传送带。 */
#define AUTO_LOCATE_BELT_MIN_DARK_ROW_PERCENT 45U

/* 自动视觉定位查找黑色传送带时，允许最多 10 行被零件高光或支架短暂打断，避免传送带区域被切碎。 */
#define AUTO_LOCATE_BELT_MAX_GAP_ROWS 10U

/* 自动视觉定位查找黑色传送带时，候选纵向带至少 80 像素高，过滤零散黑线和阴影。 */
#define AUTO_LOCATE_BELT_MIN_HEIGHT_PX 80U

/* 自动视觉定位查找黑色传送带时，候选纵向带至少占画面高度的 1/5，避免误把小暗块当成传送带。 */
#define AUTO_LOCATE_BELT_MIN_HEIGHT_DIVISOR 5U

/* 自动视觉定位查找黑色传送带后，上下各扩 12 像素，避免刚入画零件被传送带边界裁掉。 */
#define AUTO_LOCATE_BELT_VERTICAL_PADDING_PX 12U

/* 自动视觉定位找不到黑色传送带时，只回退到中心 300px 高区域，不再回退到整帧高度。 */
#define AUTO_LOCATE_BELT_FALLBACK_HEIGHT DEFAULT_DETECT_ROI_SIZE

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
 * locate_diag_code 描述 LOCATE 没有放行候选时最值得排查的拒绝位置。
 *
 * 这些值只用于 MP157 现场调试回包，不进入 MP157-F4 二进制协议：
 *   0 表示没有候选或已经成功；
 *   1 表示画面亮度对比不足；
 *   2 表示候选面积不在允许范围；
 *   3 表示候选 bbox 尺寸不在允许范围；
 *   4 表示候选长宽比不像垫圈；
 *   5 表示候选贴住搜索带左右边界；
 *   6 表示候选实体密度不在允许范围；
 *   7 表示候选亮像素占比不足；
 *   8 表示候选置信度不足；
 *   9 表示候选没有通过中心孔/ring 结构检测。
 */
enum locate_diag_code {
    LOCATE_DIAG_NONE = 0U,
    LOCATE_DIAG_LOW_CONTRAST = 1U,
    LOCATE_DIAG_AREA = 2U,
    LOCATE_DIAG_BBOX = 3U,
    LOCATE_DIAG_ASPECT = 4U,
    LOCATE_DIAG_EDGE = 5U,
    LOCATE_DIAG_DENSITY = 6U,
    LOCATE_DIAG_BRIGHT_FILL = 7U,
    LOCATE_DIAG_CONFIDENCE = 8U,
    LOCATE_DIAG_RING = 9U
};

/*
 * locate_result 保存一次内存级零件定位的结果。
 * has_target 表示是否找到可信连通域；frame_id 对应 latest_frame.serial，便于 Qt 和 F4 对齐日志。
 * frame_width/frame_height 是原始摄像头尺寸；center/bbox 都使用原始 YUYV 帧坐标，不含 KMS 居中偏移。
 * confidence 是 0~100 的粗略置信度，供 MP157 下发给 F4 和现场调参时观察。
 * has_ring_hole 表示当前候选是否通过中心孔结构检测，便于 Qt 状态栏显示和现场排查误检来源。
 * diag_* 字段只用于 LOCATE 现场调试，帮助判断真实零件被哪道过滤条件拒绝。
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
    unsigned int has_ring_hole;
    unsigned int diag_code;
    unsigned int diag_roi_y;
    unsigned int diag_roi_h;
    unsigned int diag_median_luma;
    unsigned int diag_dark_threshold;
    unsigned int diag_body_threshold;
    unsigned int diag_bright_threshold;
    unsigned int diag_candidate_score;
    unsigned int diag_candidate_bbox_w;
    unsigned int diag_candidate_bbox_h;
    unsigned int diag_candidate_area;
    unsigned int diag_candidate_density;
    unsigned int diag_candidate_confidence;
    unsigned int diag_candidate_ring;
};

/*
 * auto_locate_refined_candidate 保存从过大粘连候选中收缩出来的局部候选统计值。
 *
 * 字段说明：
 *   min_x/max_x/min_y/max_y 是收缩后 bbox 在搜索 ROI 内的范围，通常围绕垫圈中心孔。
 *   area 是收缩窗口内达到 body_threshold 的金属主体像素数量。
 *   contrast_sum/luma_sum 用于回填原 LOCATE 评分、置信度和中心孔背景对比计算。
 *   dark_pixels/bright_pixels 保持与普通 BFS 候选相同的密度和亮度过滤输入。
 *   chroma_u_sum/chroma_v_sum/chroma_sample_count 用于沿用原来的色度能量过滤。
 *   perimeter_pixels 用于沿用原来的圆度过滤；当前圆度阈值为 0，但统计仍保持完整。
 *   has_ring 表示收缩窗口已经通过中心孔结构验证，避免无环孔大黑带被当作目标。
 */
struct auto_locate_refined_candidate {
    unsigned int min_x;
    unsigned int max_x;
    unsigned int min_y;
    unsigned int max_y;
    unsigned int area;
    unsigned int contrast_sum;
    uint64_t luma_sum;
    unsigned int dark_pixels;
    unsigned int bright_pixels;
    uint64_t chroma_u_sum;
    uint64_t chroma_v_sum;
    unsigned int chroma_sample_count;
    unsigned int perimeter_pixels;
    unsigned int has_ring;
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
 * yuyv_chroma_u_at 的作用：
 *   从 YUYV422 原始帧中读取指定像素位置的 U 色度分量。
 *   YUYV 每两个像素共享一组 U/V：字节序为 Y0 U Y1 V Y2 U Y3 V ...
 *   U 位于像素对起始位置 +1 字节处。
 */
static unsigned int yuyv_chroma_u_at(const struct latest_frame *frame,
                                     unsigned int x,
                                     unsigned int y)
{
    const uint8_t *line = frame->yuyv_map + (size_t)y * frame->frame_width * 2U;

    /* U 字节位于每对像素的第 2 个字节：(x & ~1) * 2 + 1 */
    return (unsigned int)line[(x & ~1U) * 2U + 1U];
}

/*
 * yuyv_chroma_v_at 的作用：
 *   从 YUYV422 原始帧中读取指定像素位置的 V 色度分量。
 *   V 位于像素对起始位置 +3 字节处。
 */
static unsigned int yuyv_chroma_v_at(const struct latest_frame *frame,
                                     unsigned int x,
                                     unsigned int y)
{
    const uint8_t *line = frame->yuyv_map + (size_t)y * frame->frame_width * 2U;

    /* V 字节位于每对像素的第 4 个字节：(x & ~1) * 2 + 3 */
    return (unsigned int)line[(x & ~1U) * 2U + 3U];
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
 * auto_locate_luma_percentile 的作用：
 *   从 0~255 亮度直方图中取指定百分位亮度，用于构造抗极端高光的自适应阈值。
 *
 * 主要流程：
 *   1. 把百分位限制到 0~100，避免调用者传入异常值导致目标计数越界。
 *   2. 使用向上取整的目标计数，保证 p95 代表“累计达到 95% 的第一个亮度桶”。
 *   3. 从低亮度到高亮度累加桶计数，首次达到目标计数时返回当前亮度。
 *
 * 参数：
 *   histogram 是 256 桶亮度直方图，桶下标就是 Y 分量亮度。
 *   total_pixels 是直方图对应的有效像素总数。
 *   percentile 是需要提取的百分位，例如 50 表示中位数，95 表示高亮侧百分位。
 *
 * 返回值：
 *   成功时返回 0~255 的亮度值；total_pixels 为 0 时返回 0。
 */
static unsigned int auto_locate_luma_percentile(const unsigned int histogram[AUTO_LOCATE_LUMA_HISTOGRAM_BUCKETS],
                                                unsigned int total_pixels,
                                                unsigned int percentile)
{
    uint64_t target_count;
    uint64_t cumulative_count = 0U;
    unsigned int bucket;

    if (total_pixels == 0U) {
        return 0U;
    }

    if (percentile > 100U) {
        percentile = 100U;
    }

    target_count = ((uint64_t)total_pixels * percentile + 99U) / 100U;
    if (target_count == 0U) {
        target_count = 1U;
    }

    for (bucket = 0U; bucket < AUTO_LOCATE_LUMA_HISTOGRAM_BUCKETS; bucket++) {
        cumulative_count += histogram[bucket];
        if (cumulative_count >= target_count) {
            return bucket;
        }
    }

    return 255U;
}

/*
 * auto_locate_is_bright_candidate_luma 的作用：
 *   判断一个像素是否属于铝色零件的亮金属主体候选。
 *
 * 关键说明：
 *   本项目当前识别对象是铝色垫圈/弹垫，黑色传送带只能作为背景或中心孔背景。
 *   如果把暗像素也加入连通域，传送带边缘、突起阴影和局部反光会与零件竞争，
 *   造成 has_target=0 或把传送带误报为零件。
 *
 * 参数：
 *   luma 是当前像素亮度。
 *   bright_threshold 是本帧根据亮度直方图得到的亮候选阈值。
 *
 * 返回值：
 *   明显亮于背景返回 1；否则返回 0。
 */
static int auto_locate_is_bright_candidate_luma(unsigned int luma,
                                                unsigned int bright_threshold)
{
    if (luma >= bright_threshold) {
        return 1;
    }

    return 0;
}

/*
 * auto_locate_body_threshold_from_delta 的作用：
 *   根据本帧黑色皮带背景和高亮种子差值，计算较低的金属主体扩张阈值。
 *
 * 主要流程：
 *   1. 先按 AUTO_LOCATE_BODY_LUMA_DELTA_PERCENT 从 luma_delta 取一部分，避免直接用高亮种子阈值。
 *   2. 再用 AUTO_LOCATE_MIN_BODY_LUMA_DELTA 保底，防止低对比画面把皮带噪声并入零件。
 *   3. 最后保证 body_delta 不超过 luma_delta，使 body_threshold 不会高于 bright_threshold。
 *
 * 参数：
 *   median_luma 是黑色传送带区域的亮度中位数。
 *   luma_delta 是用于 high-confidence 种子的亮度差。
 *   bright_threshold 是最终高亮种子阈值，用于上限保护。
 *
 * 返回值：
 *   返回 body_threshold；像素亮度达到该值时，只能作为已发现高亮种子的连通域扩张像素。
 */
static unsigned int auto_locate_body_threshold_from_delta(unsigned int median_luma,
                                                          unsigned int luma_delta,
                                                          unsigned int bright_threshold)
{
    unsigned int body_delta = (luma_delta * AUTO_LOCATE_BODY_LUMA_DELTA_PERCENT + 99U) / 100U;
    unsigned int body_threshold;

    if (body_delta < AUTO_LOCATE_MIN_BODY_LUMA_DELTA) {
        body_delta = AUTO_LOCATE_MIN_BODY_LUMA_DELTA;
    }
    if (body_delta > luma_delta) {
        body_delta = luma_delta;
    }

    body_threshold = clamp_luma_threshold((int)median_luma + (int)body_delta);
    if (body_threshold > bright_threshold) {
        body_threshold = bright_threshold;
    }

    return body_threshold;
}

/*
 * auto_locate_record_reject_candidate 的作用：
 *   记录当前被拒绝候选的关键诊断字段，供 LOCATE 回包输出。
 *
 * 主要流程：
 *   1. 用 candidate_score 代表候选的排查价值，较大的 bbox/面积/对比通常更接近真实零件。
 *   2. 只有新候选分数更高时才覆盖旧诊断，避免小噪声覆盖真正值得看的垫圈候选。
 *   3. 记录 bbox、area、density、confidence 和 ring，现场可以直接判断卡在哪道门槛。
 *
 * 参数：
 *   result 是本次 LOCATE 输出结构。
 *   diag_code 是拒绝原因，对应 enum locate_diag_code。
 *   candidate_score 是候选优先级分数。
 *   bbox_w/bbox_h/area/density/confidence/has_ring 是候选关键指标。
 *
 * 返回值：
 *   无返回值；函数只更新 result->diag_* 字段。
 */
static void auto_locate_record_reject_candidate(struct locate_result *result,
                                                unsigned int diag_code,
                                                unsigned int candidate_score,
                                                unsigned int bbox_w,
                                                unsigned int bbox_h,
                                                unsigned int area,
                                                unsigned int density,
                                                unsigned int confidence,
                                                unsigned int has_ring)
{
    if (result == NULL) {
        return;
    }

    if (candidate_score < result->diag_candidate_score) {
        return;
    }

    result->diag_code = diag_code;
    result->diag_candidate_score = candidate_score;
    result->diag_candidate_bbox_w = bbox_w;
    result->diag_candidate_bbox_h = bbox_h;
    result->diag_candidate_area = area;
    result->diag_candidate_density = density;
    result->diag_candidate_confidence = confidence;
    result->diag_candidate_ring = has_ring;
}

/*
 * auto_locate_measure_belt_row 的作用：
 *   在水平居中的定位搜索带内，抽样统计某一行的平均亮度和暗像素占比。
 *
 * 主要流程：
 *   1. 从 roi_x 开始，只扫描 roi_w 这段水平范围，保证只看传送带宽度附近的画面。
 *   2. 每隔 AUTO_LOCATE_BELT_ROW_SAMPLE_STEP 个像素取一个 Y 亮度，降低板端实时定位开销。
 *   3. 累加亮度得到该行平均值，并按 dark_threshold 统计该行有多少采样点属于暗背景。
 *
 * 参数：
 *   frame 是最新 YUYV 原始帧。
 *   roi_x 是水平搜索带在整帧中的起始 x 坐标。
 *   roi_w 是水平搜索带宽度，当前等于模型 ROI 宽度或整帧宽度中较小者。
 *   row_y 是要统计的整帧 y 坐标。
 *   dark_threshold 是判定“黑色传送带背景像素”的 Y 亮度阈值。
 *   dark_percent 是输出暗像素占比的指针，可以传 NULL 表示只需要平均亮度。
 *
 * 返回值：
 *   返回该行抽样点的平均 Y 亮度；roi_w 为 0 时返回 0。
 */
static unsigned int auto_locate_measure_belt_row(const struct latest_frame *frame,
                                                 unsigned int roi_x,
                                                 unsigned int roi_w,
                                                 unsigned int row_y,
                                                 unsigned int dark_threshold,
                                                 unsigned int *dark_percent)
{
    unsigned int x;
    unsigned int sample_count = 0U;
    unsigned int dark_count = 0U;
    uint64_t luma_sum = 0U;

    if (dark_percent != NULL) {
        *dark_percent = 0U;
    }

    if (roi_w == 0U) {
        return 0U;
    }

    for (x = 0U; x < roi_w; x += AUTO_LOCATE_BELT_ROW_SAMPLE_STEP) {
        unsigned int luma = yuyv_luma_at(frame, roi_x + x, row_y);

        luma_sum += luma;
        sample_count++;
        if (luma <= dark_threshold) {
            dark_count++;
        }
    }

    if (sample_count == 0U) {
        return 0U;
    }

    if (dark_percent != NULL) {
        *dark_percent = dark_count * 100U / sample_count;
    }

    return (unsigned int)(luma_sum / sample_count);
}

/*
 * auto_locate_select_fallback_center_band 的作用：
 *   当现场光照过亮或传送带暗区不足，暂时找不到可靠黑色传送带时，选择画面中心 300px 高区域作为保守回退。
 *
 * 关键说明：
 *   旧代码回退到整帧高度，会把传送带外的白色支架、金属边框和背景高光全部加入阈值统计。
 *   这里即使找不到暗带，也只允许回退到模型 ROI 同口径的中心高度，避免重新引入同类污染。
 *
 * 参数：
 *   frame_height 是当前摄像头帧高度。
 *   band_y0/band_y1 是输出的纵向搜索起止行，均为整帧坐标且包含端点。
 *
 * 返回值：
 *   无返回值；frame_height 为 0 时输出 0~0，调用者已在前面过滤非法帧。
 */
static void auto_locate_select_fallback_center_band(unsigned int frame_height,
                                                    unsigned int *band_y0,
                                                    unsigned int *band_y1)
{
    unsigned int fallback_h = frame_height < AUTO_LOCATE_BELT_FALLBACK_HEIGHT ?
                              frame_height : AUTO_LOCATE_BELT_FALLBACK_HEIGHT;

    if (fallback_h == 0U) {
        *band_y0 = 0U;
        *band_y1 = 0U;
        return;
    }

    *band_y0 = (frame_height - fallback_h) / 2U;
    *band_y1 = *band_y0 + fallback_h - 1U;
}

/*
 * auto_locate_expand_belt_band_to_model_roi 的作用：
 *   把黑色传送带暗带搜索结果扩展到至少覆盖中心模型 ROI 的纵向范围。
 *
 * 主要流程：
 *   1. 按 DEFAULT_DETECT_ROI_SIZE 计算当前帧中心模型 ROI 的上下边界。
 *   2. 保留黑色传送带暗带检测得到的原始范围，避免丢掉真实传送带区域。
 *   3. 用两者并集作为 LOCATE 纵向搜索带，保证垫圈进入绿色中心 ROI 后不会被只扫描上半截。
 *
 * 关键原因：
 *   现场强高光垫圈会把黑色传送带行检测切断，旧代码可能只得到 `roi_y=0 roi_h≈197`。
 *   此时垫圈真实中心在画面中部，但 LOCATE 只抓到上侧一条亮弧，bbox 变成约 `230x55`，
 *   随后被长宽比过滤成 `diag=4`。扩展到中心模型 ROI 后，完整垫圈主体会进入连通域计算。
 *
 * 参数：
 *   frame_height 是当前摄像头帧高度。
 *   band_y0/band_y1 是输入输出参数，保存纵向搜索起止行，均为整帧坐标且包含端点。
 *
 * 返回值：
 *   无返回值；参数非法或帧高为 0 时直接返回，保持调用方已有范围不变。
 */
static void auto_locate_expand_belt_band_to_model_roi(unsigned int frame_height,
                                                      unsigned int *band_y0,
                                                      unsigned int *band_y1)
{
    unsigned int model_roi_h;
    unsigned int model_y0;
    unsigned int model_y1;

    if (band_y0 == NULL || band_y1 == NULL || frame_height == 0U) {
        return;
    }

    model_roi_h = frame_height < DEFAULT_DETECT_ROI_SIZE ? frame_height : DEFAULT_DETECT_ROI_SIZE;
    if (model_roi_h == 0U) {
        return;
    }

    model_y0 = (frame_height - model_roi_h) / 2U;
    model_y1 = model_y0 + model_roi_h - 1U;

    if (*band_y0 > model_y0) {
        *band_y0 = model_y0;
    }

    if (*band_y1 < model_y1) {
        *band_y1 = model_y1;
    }

    if (*band_y1 >= frame_height) {
        *band_y1 = frame_height - 1U;
    }
}

/*
 * auto_locate_find_dark_belt_band 的作用：
 *   从水平居中的 300px 搜索带里，先找出连续的黑色传送带纵向区域，再交给零件定位逻辑使用。
 *
 * 主要流程：
 *   1. 对每一行采样平均亮度，建立“行平均亮度直方图”。
 *   2. 取行亮度低分位作为黑色传送带背景基准，再加固定余量得到暗行阈值。
 *   3. 第二遍扫描每一行，行平均亮度够低或暗像素比例够高时，认为该行属于传送带。
 *   4. 寻找最长连续暗行段，并允许少量行被铝件高光、皮带接缝或支架遮挡打断。
 *   5. 只有高度足够的暗行段才返回成功，避免把小阴影或黑线误认为传送带。
 *
 * 参数：
 *   frame 是最新 YUYV 原始帧。
 *   roi_x/roi_w 是水平搜索带范围。
 *   band_y0/band_y1 是输出的黑色传送带纵向起止行，均为整帧坐标且包含端点。
 *
 * 返回值：
 *   找到可靠黑色传送带返回 1；未找到返回 0，调用者会使用中心 300px 高度作为保守回退。
 */
static int auto_locate_find_dark_belt_band(const struct latest_frame *frame,
                                           unsigned int roi_x,
                                           unsigned int roi_w,
                                           unsigned int *band_y0,
                                           unsigned int *band_y1)
{
    unsigned int row_luma_histogram[AUTO_LOCATE_LUMA_HISTOGRAM_BUCKETS] = { 0U };
    unsigned int frame_height;
    unsigned int row_low_luma;
    unsigned int row_dark_threshold;
    unsigned int min_band_height;
    unsigned int best_start = 0U;
    unsigned int best_end = 0U;
    unsigned int best_length = 0U;
    unsigned int best_dark_rows = 0U;
    unsigned int current_start = 0U;
    unsigned int current_end = 0U;
    unsigned int current_dark_rows = 0U;
    unsigned int current_gap_rows = 0U;
    unsigned int y;
    int in_segment = 0;

    if (frame == NULL || band_y0 == NULL || band_y1 == NULL || roi_w == 0U) {
        return 0;
    }

    frame_height = frame->frame_height;
    if (frame_height == 0U) {
        return 0;
    }

    for (y = 0U; y < frame_height; y++) {
        unsigned int row_mean = auto_locate_measure_belt_row(frame,
                                                             roi_x,
                                                             roi_w,
                                                             y,
                                                             255U,
                                                             NULL);

        row_luma_histogram[row_mean]++;
    }

    row_low_luma = auto_locate_luma_percentile(row_luma_histogram,
                                               frame_height,
                                               AUTO_LOCATE_BELT_ROW_PERCENTILE);
    row_dark_threshold = clamp_luma_threshold((int)row_low_luma +
                                              (int)AUTO_LOCATE_BELT_ROW_LUMA_MARGIN);
    if (row_dark_threshold < AUTO_LOCATE_BELT_MIN_ROW_THRESHOLD) {
        row_dark_threshold = AUTO_LOCATE_BELT_MIN_ROW_THRESHOLD;
    }

    for (y = 0U; y < frame_height; y++) {
        unsigned int dark_percent = 0U;
        unsigned int row_mean = auto_locate_measure_belt_row(frame,
                                                             roi_x,
                                                             roi_w,
                                                             y,
                                                             row_dark_threshold,
                                                             &dark_percent);
        int is_belt_row = (row_mean <= row_dark_threshold ||
                           dark_percent >= AUTO_LOCATE_BELT_MIN_DARK_ROW_PERCENT);

        if (is_belt_row) {
            if (!in_segment) {
                current_start = y;
                current_dark_rows = 0U;
                current_gap_rows = 0U;
                in_segment = 1;
            }

            current_end = y;
            current_dark_rows++;
            current_gap_rows = 0U;
            continue;
        }

        if (!in_segment) {
            continue;
        }

        if (current_gap_rows < AUTO_LOCATE_BELT_MAX_GAP_ROWS) {
            current_gap_rows++;
            current_end = y;
            continue;
        }

        {
            unsigned int segment_end = current_end >= current_gap_rows ?
                                       current_end - current_gap_rows : current_start;
            unsigned int segment_length = segment_end >= current_start ?
                                          segment_end - current_start + 1U : 0U;

            if (segment_length > best_length ||
                (segment_length == best_length && current_dark_rows > best_dark_rows)) {
                best_start = current_start;
                best_end = segment_end;
                best_length = segment_length;
                best_dark_rows = current_dark_rows;
            }
        }

        in_segment = 0;
        current_gap_rows = 0U;
        current_dark_rows = 0U;
    }

    if (in_segment) {
        unsigned int segment_end = current_end >= current_gap_rows ?
                                   current_end - current_gap_rows : current_start;
        unsigned int segment_length = segment_end >= current_start ?
                                      segment_end - current_start + 1U : 0U;

        if (segment_length > best_length ||
            (segment_length == best_length && current_dark_rows > best_dark_rows)) {
            best_start = current_start;
            best_end = segment_end;
            best_length = segment_length;
            best_dark_rows = current_dark_rows;
        }
    }

    min_band_height = frame_height / AUTO_LOCATE_BELT_MIN_HEIGHT_DIVISOR;
    if (min_band_height < AUTO_LOCATE_BELT_MIN_HEIGHT_PX) {
        min_band_height = AUTO_LOCATE_BELT_MIN_HEIGHT_PX;
    }
    if (min_band_height > frame_height) {
        min_band_height = frame_height;
    }

    if (best_length < min_band_height) {
        return 0;
    }

    *band_y0 = best_start > AUTO_LOCATE_BELT_VERTICAL_PADDING_PX ?
               best_start - AUTO_LOCATE_BELT_VERTICAL_PADDING_PX : 0U;
    *band_y1 = best_end + AUTO_LOCATE_BELT_VERTICAL_PADDING_PX < frame_height ?
               best_end + AUTO_LOCATE_BELT_VERTICAL_PADDING_PX : frame_height - 1U;

    return 1;
}

/*
 * auto_locate_component_touches_search_edge 的作用：
 *   判断一个候选连通域是否贴近自动定位搜索带边缘。
 *
 * 主要流程：
 *   1. 使用 AUTO_LOCATE_DARK_EDGE_MARGIN_PX 作为边缘保护带。
 *   2. 候选 bbox 左、右、上、下任意一侧落入保护带，就认为它与搜索边缘相连。
 *   3. 该判断只作为“大暗区误检过滤”的条件，不会单独拒绝正常零件。
 *
 * 参数：
 *   min_x/max_x/min_y/max_y 是候选 bbox 在搜索 ROI 内的范围。
 *   roi_w/roi_h 是搜索 ROI 的宽度和高度。
 *
 * 返回值：
 *   贴近搜索边缘返回 1；否则返回 0。
 */
static int auto_locate_component_touches_search_edge(unsigned int min_x,
                                                     unsigned int max_x,
                                                     unsigned int min_y,
                                                     unsigned int max_y,
                                                     unsigned int roi_w,
                                                     unsigned int roi_h)
{
    if (min_x <= AUTO_LOCATE_DARK_EDGE_MARGIN_PX) {
        return 1;
    }
    if (min_y <= AUTO_LOCATE_DARK_EDGE_MARGIN_PX) {
        return 1;
    }
    if (max_x + AUTO_LOCATE_DARK_EDGE_MARGIN_PX + 1U >= roi_w) {
        return 1;
    }
    if (max_y + AUTO_LOCATE_DARK_EDGE_MARGIN_PX + 1U >= roi_h) {
        return 1;
    }

    return 0;
}

/*
 * auto_locate_component_has_ring_hole 的作用：
 *   判断候选连通域中心是否存在垫圈类零件常见的“孔/背景”区域。
 *
 * 主要流程：
 *   1. 在候选 bbox 中央取一个小窗口，窗口尺寸约为 bbox 的 1/4。
 *   2. 统计窗口内不属于候选亮度的像素占比。
 *   3. 统计孔背景与候选实体的亮度差，避免把黑色传送带纹理误判成中心孔。
 *   4. 继续检查中心暗窗上、下、左、右四个方向是否都有主体像素支撑。
 *   5. 背景占比、背景对比度和四边主体支撑都足够高时，才认为该连通域具有中心孔结构。
 *
 * 参数：
 *   frame 是原始 YUYV 帧。
 *   roi_x/roi_y 是搜索 ROI 在整帧中的起点。
 *   min_x/max_x/min_y/max_y 是候选 bbox 在 ROI 内的范围。
 *   bright_threshold 是本帧自适应亮金属候选阈值。
 *   component_mean_luma 是候选连通域平均亮度，用于判断孔背景与实体的对比。
 *
 * 返回值：
 *   像垫圈中心孔返回 1；否则返回 0。
 */
static int auto_locate_component_has_ring_hole(const struct latest_frame *frame,
                                               unsigned int roi_x,
                                               unsigned int roi_y,
                                               unsigned int min_x,
                                               unsigned int max_x,
                                               unsigned int min_y,
                                               unsigned int max_y,
                                               unsigned int bright_threshold,
                                               unsigned int component_mean_luma)
{
    unsigned int bbox_w = max_x - min_x + 1U;
    unsigned int bbox_h = max_y - min_y + 1U;
    unsigned int sample_w = bbox_w / AUTO_LOCATE_CENTER_HOLE_SAMPLE_DIVISOR;
    unsigned int sample_h = bbox_h / AUTO_LOCATE_CENTER_HOLE_SAMPLE_DIVISOR;
    unsigned int sample_x0;
    unsigned int sample_x1;
    unsigned int sample_y0;
    unsigned int sample_y1;
    unsigned int x;
    unsigned int y;
    unsigned int total = 0U;
    unsigned int background = 0U;
    unsigned int background_contrast_avg;
    uint64_t background_contrast_sum = 0U;
    unsigned int ring_top_total = 0U;
    unsigned int ring_top_body = 0U;
    unsigned int ring_bottom_total = 0U;
    unsigned int ring_bottom_body = 0U;
    unsigned int ring_left_total = 0U;
    unsigned int ring_left_body = 0U;
    unsigned int ring_right_total = 0U;
    unsigned int ring_right_body = 0U;

    /*
     * 黑色传送带上的局部突起或反光斑可能在小 bbox 中形成“亮边包暗心”的假 ring。
     * 真实垫圈在当前 640x480 现场完整入 ROI 时 bbox 约 173x173，因此首次承认 ring 前
     * 要求候选至少达到 45px 级别；这样只会让零件稍晚一点建链，不会挡住完整零件。
     */
    if (bbox_w < AUTO_LOCATE_MIN_RING_BBOX_SIDE ||
        bbox_h < AUTO_LOCATE_MIN_RING_BBOX_SIDE) {
        return 0;
    }

    if (sample_w < 1U) {
        sample_w = 1U;
    }
    if (sample_h < 1U) {
        sample_h = 1U;
    }

    sample_x0 = min_x + (bbox_w - sample_w) / 2U;
    sample_y0 = min_y + (bbox_h - sample_h) / 2U;
    sample_x1 = sample_x0 + sample_w - 1U;
    sample_y1 = sample_y0 + sample_h - 1U;

    for (y = 0; y < sample_h; y++) {
        for (x = 0; x < sample_w; x++) {
            unsigned int luma = yuyv_luma_at(frame,
                                             roi_x + sample_x0 + x,
                                             roi_y + sample_y0 + y);

            total++;
            if (!auto_locate_is_bright_candidate_luma(luma, bright_threshold)) {
                background++;
                background_contrast_sum += (unsigned int)abs((int)luma - (int)component_mean_luma);
            }
        }
    }

    if (total == 0U || background == 0U) {
        return 0;
    }

    background_contrast_avg = (unsigned int)(background_contrast_sum / background);
    if ((background * 100U / total) < AUTO_LOCATE_MIN_CENTER_HOLE_BACKGROUND_PERCENT) {
        return 0;
    }

    /*
     * 四边主体支撑检查：
     *   真实垫圈的中心孔四周应当都有金属环面；黑色传送带误检常见形态是左右有白边、
     *   中心是黑带，但上方和下方没有环面主体。这里分别统计暗窗上/下/左/右方向的
     *   body_threshold 像素比例，任一方向太低都不承认 ring=1。
     */
    for (y = min_y; y < sample_y0; y++) {
        for (x = sample_x0; x <= sample_x1; x++) {
            unsigned int luma = yuyv_luma_at(frame, roi_x + x, roi_y + y);
            ring_top_total++;
            if (auto_locate_is_bright_candidate_luma(luma, bright_threshold)) {
                ring_top_body++;
            }
        }
    }

    for (y = sample_y1 + 1U; y <= max_y; y++) {
        for (x = sample_x0; x <= sample_x1; x++) {
            unsigned int luma = yuyv_luma_at(frame, roi_x + x, roi_y + y);
            ring_bottom_total++;
            if (auto_locate_is_bright_candidate_luma(luma, bright_threshold)) {
                ring_bottom_body++;
            }
        }
    }

    for (y = sample_y0; y <= sample_y1; y++) {
        for (x = min_x; x < sample_x0; x++) {
            unsigned int luma = yuyv_luma_at(frame, roi_x + x, roi_y + y);
            ring_left_total++;
            if (auto_locate_is_bright_candidate_luma(luma, bright_threshold)) {
                ring_left_body++;
            }
        }
    }

    for (y = sample_y0; y <= sample_y1; y++) {
        for (x = sample_x1 + 1U; x <= max_x; x++) {
            unsigned int luma = yuyv_luma_at(frame, roi_x + x, roi_y + y);
            ring_right_total++;
            if (auto_locate_is_bright_candidate_luma(luma, bright_threshold)) {
                ring_right_body++;
            }
        }
    }

    if (ring_top_total == 0U || ring_bottom_total == 0U ||
        ring_left_total == 0U || ring_right_total == 0U) {
        return 0;
    }

    if ((ring_top_body * 100U / ring_top_total) < AUTO_LOCATE_MIN_RING_SIDE_BODY_PERCENT ||
        (ring_bottom_body * 100U / ring_bottom_total) < AUTO_LOCATE_MIN_RING_SIDE_BODY_PERCENT ||
        (ring_left_body * 100U / ring_left_total) < AUTO_LOCATE_MIN_RING_SIDE_BODY_PERCENT ||
        (ring_right_body * 100U / ring_right_total) < AUTO_LOCATE_MIN_RING_SIDE_BODY_PERCENT) {
        return 0;
    }

    return background_contrast_avg >= AUTO_LOCATE_MIN_RING_BACKGROUND_CONTRAST ? 1 : 0;
}

/*
 * auto_locate_body_support_percent_in_rect 的作用：
 *   统计指定矩形内达到 body_threshold 的金属主体像素比例。
 *
 * 主要流程：
 *   1. 遍历矩形内每个 YUYV 亮度像素。
 *   2. 使用 body_threshold 判断该像素是否属于垫圈主体支撑。
 *   3. 返回主体像素百分比，并可选输出主体像素数量用于评分。
 *
 * 参数：
 *   frame 是当前摄像头帧。
 *   roi_x/roi_y 是定位搜索 ROI 在整帧中的起点。
 *   min_x/max_x/min_y/max_y 是待统计矩形在 ROI 内的范围。
 *   body_threshold 是金属主体扩张阈值。
 *   body_count 是可选输出，保存主体像素数量。
 *
 * 返回值：
 *   返回主体像素百分比；矩形为空时返回 0。
 */
static unsigned int auto_locate_body_support_percent_in_rect(const struct latest_frame *frame,
                                                             unsigned int roi_x,
                                                             unsigned int roi_y,
                                                             unsigned int min_x,
                                                             unsigned int max_x,
                                                             unsigned int min_y,
                                                             unsigned int max_y,
                                                             unsigned int body_threshold,
                                                             unsigned int *body_count)
{
    unsigned int total = 0U;
    unsigned int body = 0U;
    unsigned int x;
    unsigned int y;

    if (body_count != NULL) {
        *body_count = 0U;
    }

    if (frame == NULL || min_x > max_x || min_y > max_y) {
        return 0U;
    }

    for (y = min_y; y <= max_y; y++) {
        for (x = min_x; x <= max_x; x++) {
            unsigned int luma = yuyv_luma_at(frame, roi_x + x, roi_y + y);

            total++;
            if (auto_locate_is_bright_candidate_luma(luma, body_threshold)) {
                body++;
            }
        }
    }

    if (body_count != NULL) {
        *body_count = body;
    }

    return total > 0U ? (body * 100U) / total : 0U;
}

/*
 * auto_locate_oversize_ring_support_score 的作用：
 *   判断一个暗中心点周围是否具备垫圈中心孔的四边主体支撑，并输出评分。
 *
 * 主要流程：
 *   1. 以 cx/cy 为疑似孔中心，按 radius 在上、下、左、右取四条主体采样带。
 *   2. 四条采样带都必须达到 AUTO_LOCATE_MIN_RING_SIDE_BODY_PERCENT。
 *   3. 把四边主体像素数量相加作为候选评分，供大候选收缩时选择最像垫圈孔的位置。
 *
 * 参数：
 *   min_x/max_x/min_y/max_y 是原始过大候选 bbox，用于限制支撑带仍在候选内部。
 *   cx/cy 是疑似中心孔坐标，单位为搜索 ROI 内像素。
 *   radius 是从中心孔到外侧主体采样带的大致半径。
 *   body_threshold 是金属主体阈值。
 *   score 是输出评分，只有返回 1 时有效。
 *
 * 返回值：
 *   四边都有主体支撑返回 1；否则返回 0。
 */
static int auto_locate_oversize_ring_support_score(const struct latest_frame *frame,
                                                   unsigned int roi_x,
                                                   unsigned int roi_y,
                                                   unsigned int min_x,
                                                   unsigned int max_x,
                                                   unsigned int min_y,
                                                   unsigned int max_y,
                                                   unsigned int cx,
                                                   unsigned int cy,
                                                   unsigned int radius,
                                                   unsigned int body_threshold,
                                                   unsigned int *score)
{
    unsigned int thickness = radius / 5U;
    unsigned int span = radius / 2U;
    unsigned int top_body = 0U;
    unsigned int bottom_body = 0U;
    unsigned int left_body = 0U;
    unsigned int right_body = 0U;
    unsigned int top_percent;
    unsigned int bottom_percent;
    unsigned int left_percent;
    unsigned int right_percent;

    if (score != NULL) {
        *score = 0U;
    }

    if (radius == 0U ||
        cx < min_x + radius ||
        cy < min_y + radius ||
        cx + radius > max_x ||
        cy + radius > max_y) {
        return 0;
    }

    if (thickness < AUTO_LOCATE_OVERSIZE_REFINE_MIN_SUPPORT_THICKNESS) {
        thickness = AUTO_LOCATE_OVERSIZE_REFINE_MIN_SUPPORT_THICKNESS;
    }
    if (thickness > radius / 2U) {
        thickness = radius / 2U;
    }
    if (span < AUTO_LOCATE_MIN_RING_BBOX_SIDE / 4U) {
        span = AUTO_LOCATE_MIN_RING_BBOX_SIDE / 4U;
    }
    if (span > radius) {
        span = radius;
    }

    top_percent = auto_locate_body_support_percent_in_rect(frame,
                                                           roi_x,
                                                           roi_y,
                                                           cx - span,
                                                           cx + span,
                                                           cy - radius,
                                                           cy - radius + thickness - 1U,
                                                           body_threshold,
                                                           &top_body);
    bottom_percent = auto_locate_body_support_percent_in_rect(frame,
                                                              roi_x,
                                                              roi_y,
                                                              cx - span,
                                                              cx + span,
                                                              cy + radius - thickness + 1U,
                                                              cy + radius,
                                                              body_threshold,
                                                              &bottom_body);
    left_percent = auto_locate_body_support_percent_in_rect(frame,
                                                            roi_x,
                                                            roi_y,
                                                            cx - radius,
                                                            cx - radius + thickness - 1U,
                                                            cy - span,
                                                            cy + span,
                                                            body_threshold,
                                                            &left_body);
    right_percent = auto_locate_body_support_percent_in_rect(frame,
                                                             roi_x,
                                                             roi_y,
                                                             cx + radius - thickness + 1U,
                                                             cx + radius,
                                                             cy - span,
                                                             cy + span,
                                                             body_threshold,
                                                             &right_body);

    if (top_percent < AUTO_LOCATE_MIN_RING_SIDE_BODY_PERCENT ||
        bottom_percent < AUTO_LOCATE_MIN_RING_SIDE_BODY_PERCENT ||
        left_percent < AUTO_LOCATE_MIN_RING_SIDE_BODY_PERCENT ||
        right_percent < AUTO_LOCATE_MIN_RING_SIDE_BODY_PERCENT) {
        return 0;
    }

    if (score != NULL) {
        *score = top_body + bottom_body + left_body + right_body;
    }

    return 1;
}

/*
 * auto_locate_measure_refined_ring_window 的作用：
 *   对大候选收缩后的局部窗口重新统计 LOCATE 后续过滤需要的指标。
 *
 * 主要流程：
 *   1. 把收缩窗口本身作为新的 bbox，保证中心点落在疑似垫圈孔附近。
 *   2. 只把达到 body_threshold 的像素计入主体面积、亮度均值、色度和边界统计。
 *   3. 用窗口内主体像素和周围背景重新生成 density/confidence 所需的输入。
 *
 * 参数：
 *   win_min_x/win_max_x/win_min_y/win_max_y 是收缩窗口在搜索 ROI 内的范围。
 *   median_luma/dark_threshold/body_threshold 是本帧 LOCATE 阈值。
 *   refined 是输出结构。
 *
 * 返回值：
 *   窗口内存在主体像素返回 1；否则返回 0。
 */
static int auto_locate_measure_refined_ring_window(const struct latest_frame *frame,
                                                   unsigned int roi_x,
                                                   unsigned int roi_y,
                                                   unsigned int win_min_x,
                                                   unsigned int win_max_x,
                                                   unsigned int win_min_y,
                                                   unsigned int win_max_y,
                                                   unsigned int median_luma,
                                                   unsigned int dark_threshold,
                                                   unsigned int body_threshold,
                                                   struct auto_locate_refined_candidate *refined)
{
    static const int neighbor_dx[4] = { -1, 1, 0, 0 };
    static const int neighbor_dy[4] = { 0, 0, -1, 1 };
    unsigned int x;
    unsigned int y;

    if (frame == NULL || refined == NULL ||
        win_min_x > win_max_x ||
        win_min_y > win_max_y) {
        return 0;
    }

    memset(refined, 0, sizeof(*refined));
    refined->min_x = win_min_x;
    refined->max_x = win_max_x;
    refined->min_y = win_min_y;
    refined->max_y = win_max_y;

    for (y = win_min_y; y <= win_max_y; y++) {
        for (x = win_min_x; x <= win_max_x; x++) {
            unsigned int luma = yuyv_luma_at(frame, roi_x + x, roi_y + y);
            unsigned int i;

            if (!auto_locate_is_bright_candidate_luma(luma, body_threshold)) {
                continue;
            }

            refined->area++;
            refined->contrast_sum += (unsigned int)abs((int)luma - (int)median_luma);
            refined->luma_sum += luma;

            if (luma <= dark_threshold) {
                refined->dark_pixels++;
            } else if (luma >= body_threshold) {
                refined->bright_pixels++;
            }

            if ((refined->area & 3U) == 0U) {
                refined->chroma_u_sum += yuyv_chroma_u_at(frame, roi_x + x, roi_y + y);
                refined->chroma_v_sum += yuyv_chroma_v_at(frame, roi_x + x, roi_y + y);
                refined->chroma_sample_count++;
            }

            for (i = 0U; i < 4U; i++) {
                int next_x = (int)x + neighbor_dx[i];
                int next_y = (int)y + neighbor_dy[i];
                unsigned int next_luma;

                if (next_x < (int)win_min_x || next_y < (int)win_min_y ||
                    next_x > (int)win_max_x || next_y > (int)win_max_y) {
                    refined->perimeter_pixels++;
                    continue;
                }

                next_luma = yuyv_luma_at(frame,
                                         roi_x + (unsigned int)next_x,
                                         roi_y + (unsigned int)next_y);
                if (!auto_locate_is_bright_candidate_luma(next_luma, body_threshold)) {
                    refined->perimeter_pixels++;
                }
            }
        }
    }

    return refined->area > 0U ? 1 : 0;
}

/*
 * auto_locate_refine_oversized_ring_candidate 的作用：
 *   当连通域 bbox 因垫圈、白色支架或高光粘连而过大时，从内部收缩出真实环孔局部 bbox。
 *
 * 主要流程：
 *   1. 只处理超过 AUTO_LOCATE_MAX_BBOX_SIDE 的过大候选，普通候选继续走原过滤链。
 *   2. 在过大候选内部扫描暗中心点，暗中心点必须不是金属主体像素。
 *   3. 对每个暗中心点尝试多个半径，要求上、下、左、右都有金属主体支撑。
 *   4. 选出评分最高的中心孔位置，并生成不超过 210x210 的局部窗口。
 *   5. 对局部窗口重新统计面积、密度、置信度输入，并再次调用 ring 检测确认中心孔结构。
 *
 * 参数：
 *   min_x/max_x/min_y/max_y 是原始过大候选 bbox。
 *   median_luma/dark_threshold/body_threshold 是本帧定位阈值。
 *   component_mean_luma 是原始大候选平均亮度，用于给暗中心对比度评分。
 *   refined 是输出收缩候选。
 *
 * 返回值：
 *   成功收缩出带中心孔的局部候选返回 1；否则返回 0，让外层继续按 diag=3 拒绝。
 */
static int auto_locate_refine_oversized_ring_candidate(const struct latest_frame *frame,
                                                       unsigned int roi_x,
                                                       unsigned int roi_y,
                                                       unsigned int roi_w,
                                                       unsigned int roi_h,
                                                       unsigned int min_x,
                                                       unsigned int max_x,
                                                       unsigned int min_y,
                                                       unsigned int max_y,
                                                       unsigned int median_luma,
                                                       unsigned int dark_threshold,
                                                       unsigned int body_threshold,
                                                       unsigned int component_mean_luma,
                                                       struct auto_locate_refined_candidate *refined)
{
    unsigned int bbox_w;
    unsigned int bbox_h;
    unsigned int max_radius;
    unsigned int best_cx = 0U;
    unsigned int best_cy = 0U;
    unsigned int best_radius = 0U;
    unsigned int best_score = 0U;
    unsigned int cy;
    unsigned int side;
    unsigned int half_side;
    unsigned int win_min_x;
    unsigned int win_min_y;
    unsigned int win_max_x;
    unsigned int win_max_y;
    unsigned int refined_mean_luma;

    if (frame == NULL || refined == NULL ||
        min_x > max_x || min_y > max_y ||
        roi_w == 0U || roi_h == 0U) {
        return 0;
    }

    bbox_w = max_x - min_x + 1U;
    bbox_h = max_y - min_y + 1U;
    if (bbox_w <= AUTO_LOCATE_MAX_BBOX_SIDE &&
        bbox_h <= AUTO_LOCATE_MAX_BBOX_SIDE) {
        return 0;
    }

    if (bbox_w < AUTO_LOCATE_OVERSIZE_REFINE_MIN_SIDE ||
        bbox_h < AUTO_LOCATE_OVERSIZE_REFINE_MIN_SIDE ||
        min_x + AUTO_LOCATE_OVERSIZE_REFINE_MIN_RADIUS > max_x ||
        min_y + AUTO_LOCATE_OVERSIZE_REFINE_MIN_RADIUS > max_y) {
        return 0;
    }

    max_radius = AUTO_LOCATE_OVERSIZE_REFINE_MAX_SIDE / 2U;
    if (max_radius > bbox_w / 2U) {
        max_radius = bbox_w / 2U;
    }
    if (max_radius > bbox_h / 2U) {
        max_radius = bbox_h / 2U;
    }
    if (max_radius < AUTO_LOCATE_OVERSIZE_REFINE_MIN_RADIUS) {
        return 0;
    }

    for (cy = min_y + AUTO_LOCATE_OVERSIZE_REFINE_MIN_RADIUS;
         cy <= max_y - AUTO_LOCATE_OVERSIZE_REFINE_MIN_RADIUS;
         cy += AUTO_LOCATE_OVERSIZE_REFINE_SCAN_STEP) {
        unsigned int cx;

        for (cx = min_x + AUTO_LOCATE_OVERSIZE_REFINE_MIN_RADIUS;
             cx <= max_x - AUTO_LOCATE_OVERSIZE_REFINE_MIN_RADIUS;
             cx += AUTO_LOCATE_OVERSIZE_REFINE_SCAN_STEP) {
            unsigned int center_luma = yuyv_luma_at(frame, roi_x + cx, roi_y + cy);
            unsigned int center_contrast;
            unsigned int radius;

            if (auto_locate_is_bright_candidate_luma(center_luma, body_threshold)) {
                continue;
            }

            center_contrast = (unsigned int)abs((int)center_luma - (int)component_mean_luma);

            for (radius = AUTO_LOCATE_OVERSIZE_REFINE_MIN_RADIUS;
                 radius <= max_radius;
                 radius += AUTO_LOCATE_OVERSIZE_REFINE_RADIUS_STEP) {
                unsigned int support_score = 0U;
                unsigned int candidate_score;

                if (!auto_locate_oversize_ring_support_score(frame,
                                                             roi_x,
                                                             roi_y,
                                                             min_x,
                                                             max_x,
                                                             min_y,
                                                             max_y,
                                                             cx,
                                                             cy,
                                                             radius,
                                                             body_threshold,
                                                             &support_score)) {
                    continue;
                }

                candidate_score = support_score + center_contrast * 20U + radius * 3U;
                if (candidate_score > best_score) {
                    best_score = candidate_score;
                    best_cx = cx;
                    best_cy = cy;
                    best_radius = radius;
                }
            }
        }
    }

    if (best_score == 0U || best_radius == 0U) {
        return 0;
    }

    side = best_radius * 2U + best_radius / 2U;
    if (side < AUTO_LOCATE_OVERSIZE_REFINE_MIN_SIDE) {
        side = AUTO_LOCATE_OVERSIZE_REFINE_MIN_SIDE;
    }
    if (side > AUTO_LOCATE_OVERSIZE_REFINE_MAX_SIDE) {
        side = AUTO_LOCATE_OVERSIZE_REFINE_MAX_SIDE;
    }
    if (side > roi_w) {
        side = roi_w;
    }
    if (side > roi_h) {
        side = roi_h;
    }
    if (side == 0U) {
        return 0;
    }

    half_side = side / 2U;
    win_min_x = best_cx > half_side ? best_cx - half_side : 0U;
    win_min_y = best_cy > half_side ? best_cy - half_side : 0U;
    if (win_min_x + side > roi_w) {
        win_min_x = roi_w > side ? roi_w - side : 0U;
    }
    if (win_min_y + side > roi_h) {
        win_min_y = roi_h > side ? roi_h - side : 0U;
    }
    win_max_x = win_min_x + side - 1U;
    win_max_y = win_min_y + side - 1U;

    if (!auto_locate_measure_refined_ring_window(frame,
                                                 roi_x,
                                                 roi_y,
                                                 win_min_x,
                                                 win_max_x,
                                                 win_min_y,
                                                 win_max_y,
                                                 median_luma,
                                                 dark_threshold,
                                                 body_threshold,
                                                 refined)) {
        return 0;
    }

    refined_mean_luma = (unsigned int)(refined->luma_sum / refined->area);
    if (!auto_locate_component_has_ring_hole(frame,
                                             roi_x,
                                             roi_y,
                                             refined->min_x,
                                             refined->max_x,
                                             refined->min_y,
                                             refined->max_y,
                                             body_threshold,
                                             refined_mean_luma)) {
        return 0;
    }

    refined->has_ring = 1U;
    return 1;
}

/*
 * locate_part_in_yuyv_frame 的作用：
 *   在最新 YUYV 原始帧的中心 ROI 内定位传送带上的零件。
 *
 * 主要流程：
 *   1. 初始化输出结果，把 frame_id 和图像尺寸先写入 result，保证无目标时也能回传上下文。
 *   2. 先在水平居中的 300px 搜索带里查找黑色传送带纵向区域，排除传送带外白色支架和背景。
 *   3. 再把纵向搜索范围扩展到至少覆盖中心 300px 模型 ROI，避免强高光垫圈把暗带切断后漏掉完整主体。
 *   4. 只统计最终搜索区域内的亮度直方图、最暗值和最亮值，用 p50/p95 得到当前背景的自适应阈值。
 *   5. 只对明显亮于传送带背景的金属主体像素做四邻域连通域搜索，暗像素只作为背景/孔洞证据。
 *   6. 拒绝贴边、窄长、过密或过稀的连通域，避免把传送带高光斑误识别成零件。
 *   7. 选择面积、外接框和长宽比都合理的最佳连通域，输出中心点、bbox 和置信度。
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
    unsigned int belt_y0;
    unsigned int belt_y1;
    unsigned int pixel_count;
    unsigned int luma_histogram[AUTO_LOCATE_LUMA_HISTOGRAM_BUCKETS] = { 0U };
    unsigned int min_luma = 255U;
    unsigned int max_luma = 0U;
    unsigned int median_luma;
    unsigned int upper_luma;
    unsigned int contrast_span;
    unsigned int luma_delta;
    unsigned int bright_threshold;
    unsigned int body_threshold;
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
    roi_x = (frame->frame_width - roi_w) / 2U;

    if (!auto_locate_find_dark_belt_band(frame, roi_x, roi_w, &belt_y0, &belt_y1)) {
        auto_locate_select_fallback_center_band(frame->frame_height, &belt_y0, &belt_y1);
    }
    auto_locate_expand_belt_band_to_model_roi(frame->frame_height, &belt_y0, &belt_y1);

    roi_y = belt_y0;
    roi_h = belt_y1 >= belt_y0 ? belt_y1 - belt_y0 + 1U : 0U;
    pixel_count = roi_w * roi_h;
    result->diag_roi_y = roi_y;
    result->diag_roi_h = roi_h;

    if (pixel_count == 0U) {
        errno = EINVAL;
        return -1;
    }

    for (y = 0; y < roi_h; y++) {
        unsigned int x;

        for (x = 0; x < roi_w; x++) {
            unsigned int luma = yuyv_luma_at(frame, roi_x + x, roi_y + y);

            luma_histogram[luma]++;
            if (luma < min_luma) {
                min_luma = luma;
            }
            if (luma > max_luma) {
                max_luma = luma;
            }
        }
    }

    median_luma = auto_locate_luma_percentile(luma_histogram, pixel_count, 50U);
    upper_luma = auto_locate_luma_percentile(luma_histogram, pixel_count, 95U);
    contrast_span = max_luma > min_luma ? max_luma - min_luma : 0U;
    result->diag_median_luma = median_luma;
    if (contrast_span < AUTO_LOCATE_MIN_LUMA_DELTA) {
        result->diag_code = LOCATE_DIAG_LOW_CONTRAST;
        return 0;
    }

    luma_delta = upper_luma > median_luma ? upper_luma - median_luma : 0U;
    if (luma_delta < AUTO_LOCATE_MIN_LUMA_DELTA) {
        luma_delta = AUTO_LOCATE_MIN_LUMA_DELTA;
    }
    if (luma_delta > AUTO_LOCATE_MAX_LUMA_DELTA) {
        luma_delta = AUTO_LOCATE_MAX_LUMA_DELTA;
    }

    bright_threshold = clamp_luma_threshold((int)median_luma + (int)luma_delta);
    body_threshold = auto_locate_body_threshold_from_delta(median_luma,
                                                           luma_delta,
                                                           bright_threshold);
    dark_threshold = clamp_luma_threshold((int)median_luma - (int)luma_delta);
    result->diag_dark_threshold = dark_threshold;
    result->diag_body_threshold = body_threshold;
    result->diag_bright_threshold = bright_threshold;

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
            uint64_t component_luma_sum = 0U;
            unsigned int dark_pixels = 0U;
            unsigned int bright_pixels = 0U;
            /* 色度累加器：用于计算连通域平均 U/V，判断消色差传送带 */
            uint64_t chroma_u_sum = 0U;
            uint64_t chroma_v_sum = 0U;
            unsigned int chroma_sample_count = 0U;
            /* 边界像素计数器：用于计算圆度 = 4π×area/perimeter² */
            unsigned int perimeter_pixels = 0U;
            unsigned int min_x = x;
            unsigned int max_x = x;
            unsigned int min_y = y;
            unsigned int max_y = y;
            unsigned int bbox_w;
            unsigned int bbox_h;
            unsigned int bbox_area;
            unsigned int contrast_avg;
            unsigned int component_mean_luma;
            unsigned int dark_fill_percent;
            unsigned int dark_area_percent;
            unsigned int density;
            unsigned int score;
            unsigned int confidence;
            unsigned int has_ring;
            struct auto_locate_refined_candidate refined_candidate;
            int oversized_refined = 0;

            if (visited[start_index]) {
                continue;
            }

            start_luma = yuyv_luma_at(frame, roi_x + x, roi_y + y);
            if (!auto_locate_is_bright_candidate_luma(start_luma, bright_threshold)) {
                /*
                 * 高亮种子仍然使用 bright_threshold，防止灰色皮带纹理自己启动连通域。
                 * 但达到 body_threshold 的银色阴影像素不能提前标记 visited；
                 * 它们需要保留给后续相邻高亮种子扩张，否则垫圈亮环会被切成几个小弧段。
                 */
                if (!auto_locate_is_bright_candidate_luma(start_luma, body_threshold)) {
                    visited[start_index] = 1U;
                }
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
                contrast_sum += (unsigned int)abs((int)luma - (int)median_luma);
                component_luma_sum += luma;

                /* 每隔 4 个像素采样一次色度，降低计算开销同时保持统计精度 */
                if ((area & 3U) == 0U) {
                    chroma_u_sum += yuyv_chroma_u_at(frame, roi_x + local_x, roi_y + local_y);
                    chroma_v_sum += yuyv_chroma_v_at(frame, roi_x + local_x, roi_y + local_y);
                    chroma_sample_count++;
                }

                if (luma <= dark_threshold) {
                    dark_pixels++;
                } else if (luma >= body_threshold) {
                    bright_pixels++;
                }

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
                        /* 邻居越界 = 当前像素在连通域边缘 */
                        perimeter_pixels++;
                        continue;
                    }

                    neighbor_index = (unsigned int)next_y * roi_w + (unsigned int)next_x;
                    if (visited[neighbor_index]) {
                        continue;
                    }

                    next_luma = yuyv_luma_at(frame,
                                             roi_x + (unsigned int)next_x,
                                             roi_y + (unsigned int)next_y);
                    if (!auto_locate_is_bright_candidate_luma(next_luma, body_threshold)) {
                        visited[neighbor_index] = 1U;
                        /* 邻居不是候选 = 当前像素是边界像素 */
                        perimeter_pixels++;
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
                auto_locate_record_reject_candidate(result,
                                                    LOCATE_DIAG_AREA,
                                                    bbox_area + area,
                                                    bbox_w,
                                                    bbox_h,
                                                    area,
                                                    0U,
                                                    0U,
                                                    0U);
                continue;
            }

            component_mean_luma = (unsigned int)(component_luma_sum / area);

            if (bbox_w < AUTO_LOCATE_MIN_BBOX_SIDE ||
                bbox_h < AUTO_LOCATE_MIN_BBOX_SIDE) {
                auto_locate_record_reject_candidate(result,
                                                    LOCATE_DIAG_BBOX,
                                                    bbox_area + area,
                                                    bbox_w,
                                                    bbox_h,
                                                    area,
                                                    0U,
                                                    0U,
                                                    0U);
                continue;
            }

            /*
             * 过大 bbox 先尝试环孔局部收缩：
             *   现场 `cand_box=300x261` 这类候选通常不是“没有看到零件”，
             *   而是垫圈亮边与白色支架、强反光或背景粘成了一个过大连通域。
             *   如果这里在 ring_hole 前直接 diag=3，模型入口会显示“当前 ROI 未识别到零件”。
             *   因此先在大候选内部寻找暗中心和四边主体支撑，把候选收缩为局部垫圈 bbox；
             *   收缩失败时才继续按 bbox 过大拒绝，避免放行整片白色支架或黑色传送带。
             */
            if (bbox_w > AUTO_LOCATE_MAX_BBOX_SIDE ||
                bbox_h > AUTO_LOCATE_MAX_BBOX_SIDE) {
                oversized_refined = auto_locate_refine_oversized_ring_candidate(frame,
                                                                                roi_x,
                                                                                roi_y,
                                                                                roi_w,
                                                                                roi_h,
                                                                                min_x,
                                                                                max_x,
                                                                                min_y,
                                                                                max_y,
                                                                                median_luma,
                                                                                dark_threshold,
                                                                                body_threshold,
                                                                                component_mean_luma,
                                                                                &refined_candidate);
                if (!oversized_refined) {
                    auto_locate_record_reject_candidate(result,
                                                        LOCATE_DIAG_BBOX,
                                                        bbox_area + area,
                                                        bbox_w,
                                                        bbox_h,
                                                        area,
                                                        0U,
                                                        0U,
                                                        0U);
                    continue;
                }

                min_x = refined_candidate.min_x;
                max_x = refined_candidate.max_x;
                min_y = refined_candidate.min_y;
                max_y = refined_candidate.max_y;
                area = refined_candidate.area;
                contrast_sum = refined_candidate.contrast_sum;
                component_luma_sum = refined_candidate.luma_sum;
                dark_pixels = refined_candidate.dark_pixels;
                bright_pixels = refined_candidate.bright_pixels;
                chroma_u_sum = refined_candidate.chroma_u_sum;
                chroma_v_sum = refined_candidate.chroma_v_sum;
                chroma_sample_count = refined_candidate.chroma_sample_count;
                perimeter_pixels = refined_candidate.perimeter_pixels;
                bbox_w = max_x - min_x + 1U;
                bbox_h = max_y - min_y + 1U;
                bbox_area = bbox_w * bbox_h;

                if (area < AUTO_LOCATE_MIN_COMPONENT_AREA || area > max_component_area) {
                    auto_locate_record_reject_candidate(result,
                                                        LOCATE_DIAG_AREA,
                                                        bbox_area + area,
                                                        bbox_w,
                                                        bbox_h,
                                                        area,
                                                        0U,
                                                        0U,
                                                        0U);
                    continue;
                }
            }

            if (bbox_w * 100U < bbox_h * 25U ||
                bbox_h * 100U < bbox_w * 25U) {
                auto_locate_record_reject_candidate(result,
                                                    LOCATE_DIAG_ASPECT,
                                                    bbox_area + area,
                                                    bbox_w,
                                                    bbox_h,
                                                    area,
                                                    0U,
                                                    0U,
                                                    0U);
                continue;
            }

            /*
             * 收紧长宽比：环形零件近似正方形外接框（长宽比 > 0.5）。
             * 传送带边缘/框架阴影通常是窄长条（长宽比 < 0.35）。
             * 比原来的 25% 更严格，用 40% 过滤长条形误检。
             */
            if (bbox_w * 100U < bbox_h * 40U ||
                bbox_h * 100U < bbox_w * 40U) {
                auto_locate_record_reject_candidate(result,
                                                    LOCATE_DIAG_ASPECT,
                                                    bbox_area + area,
                                                    bbox_w,
                                                    bbox_h,
                                                    area,
                                                    0U,
                                                    0U,
                                                    0U);
                continue;
            }

            contrast_avg = contrast_sum / area;
            component_mean_luma = (unsigned int)(component_luma_sum / area);
            dark_fill_percent = area > 0U ? (dark_pixels * 100U) / area : 0U;
            dark_area_percent = pixel_count > 0U ? (dark_pixels * 100U) / pixel_count : 0U;
            density = bbox_area > 0U ? (area * 100U) / bbox_area : 0U;

            if (density < AUTO_LOCATE_MIN_SOLID_DENSITY_PERCENT ||
                density > AUTO_LOCATE_MAX_SOLID_DENSITY_PERCENT) {
                auto_locate_record_reject_candidate(result,
                                                    LOCATE_DIAG_DENSITY,
                                                    bbox_area + area,
                                                    bbox_w,
                                                    bbox_h,
                                                    area,
                                                    density,
                                                    0U,
                                                    0U);
                continue;
            }

            /*
             * 色度能量过滤：当 AUTO_LOCATE_MIN_CHROMA_ENERGY > 0 时启用。
             * 银色/铝色零件为消色差，与灰色传送带无法用色度区分，此时应设为 0 禁用。
             */
            {
                /*
                 * 使用局部变量承接宏值，避免宏为 0 时编译器把
                 * `chroma_energy < 0U` 报成 -Wtype-limits。
                 */
                const unsigned int min_chroma_energy = AUTO_LOCATE_MIN_CHROMA_ENERGY;

                if (min_chroma_energy > 0U && chroma_sample_count > 0U) {
                    unsigned int avg_u = (unsigned int)(chroma_u_sum / chroma_sample_count);
                    unsigned int avg_v = (unsigned int)(chroma_v_sum / chroma_sample_count);
                    unsigned int chroma_energy = (unsigned int)(abs((int)avg_u - 128)
                                                                + abs((int)avg_v - 128));
                    if (chroma_energy < min_chroma_energy) {
                        continue;
                    }
                }
            }

            /*
             * 圆度过滤：真实零件（环形/圆形）有规则的圆形轮廓，圆度高；
             * 传送带纹理/反光斑块边界不规则，圆度低。
             * circularity = 4π × area / perimeter² (×100 转整数百分比)
             */
            {
                /*
                 * 使用局部变量承接宏值，避免宏为 0 时编译器把
                 * `circularity_percent < 0U` 报成 -Wtype-limits。
                 */
                const unsigned int min_circularity_percent = AUTO_LOCATE_MIN_CIRCULARITY_PERCENT;

                if (perimeter_pixels > 0U && min_circularity_percent > 0U) {
                    /* 用 1257 / 100 近似 4π ≈ 12.566 */
                    unsigned int circularity_percent = (unsigned int)(
                        (uint64_t)area * 1257U / ((uint64_t)perimeter_pixels * perimeter_pixels / 100U + 1U));
                    if (circularity_percent > 100U) {
                        circularity_percent = 100U;
                    }
                    if (circularity_percent < min_circularity_percent) {
                        continue;
                    }
                }
            }

            /*
             * 暗候选回退保护：
             * 当前 BFS 只接受亮金属主体，正常情况下 dark_pixels 应接近 0。
             * 保留该判断是为了防止后续维护时又把暗像素并回候选后，
             * 仍能拒绝贴边大暗区，避免黑色传送带背景重新被当成零件。
             */
            if (dark_pixels > bright_pixels &&
                dark_fill_percent >= AUTO_LOCATE_MAX_DARK_FILL_PERCENT &&
                (dark_area_percent >= AUTO_LOCATE_MAX_DARK_EDGE_AREA_PERCENT ||
                 auto_locate_component_touches_search_edge(min_x,
                                                           max_x,
                                                           min_y,
                                                           max_y,
                                                           roi_w,
                                                           roi_h))) {
                continue;
            }

            /*
             * 亮度主导过滤：银白色零件在摄像头下是亮候选为主。
             * 如果亮像素占比过低，说明更可能是传送带暗纹理/阴影而非金属零件。
             * 设为 0 禁用此过滤器。
             */
            if (AUTO_LOCATE_MIN_BRIGHT_FILL_PERCENT > 0U) {
                unsigned int bright_fill_percent = area > 0U ? (bright_pixels * 100U) / area : 0U;
                if (bright_fill_percent < AUTO_LOCATE_MIN_BRIGHT_FILL_PERCENT) {
                    auto_locate_record_reject_candidate(result,
                                                        LOCATE_DIAG_BRIGHT_FILL,
                                                        bbox_area + area,
                                                        bbox_w,
                                                        bbox_h,
                                                        area,
                                                        density,
                                                        0U,
                                                        0U);
                    continue;
                }
            }

            /*
             * ring_hole 检测作为强结构证据：
             * - 有环孔的候选按垫圈类零件处理，允许较低置信度进入后续多帧确认。
             * - 没有环孔的候选必须同时满足更大的 bbox 和更高置信度，避免黑色传送带凸起反光被当成零件。
             * - 零件刚从上方进入时如果只露出很小亮边，宁可继续等待下一帧，也不要提前把传送带反光报成 has_target=1。
             */
            {
                has_ring = auto_locate_component_has_ring_hole(frame,
                                                               roi_x,
                                                               roi_y,
                                                               min_x,
                                                               max_x,
                                                               min_y,
                                                               max_y,
                                                               body_threshold,
                                                               component_mean_luma);

                score = area + bbox_area / 4U + contrast_avg * 8U;
                if (has_ring) {
                    score = score + score / 2U; /* 有环孔加 50% 得分 */
                }

                if (score <= best_score) {
                    continue;
                }

                confidence = contrast_avg * 2U + density / 2U + area / 20U;
                if (has_ring) {
                    confidence += 15U; /* 有环孔加 15 置信度 */
                }
                if (confidence > 100U) {
                    confidence = 100U;
                }

                /*
                 * 贴边候选延后拒绝：
                 *   旧逻辑在 ring_hole 检测前，只要候选触及左右搜索边界就直接拒绝。
                 *   现场垫圈强高光会把右侧亮边短暂并入同一连通域，候选虽然贴边，
                 *   但中心区域仍可能有真实垫圈孔；因此这里先看 has_ring。
                 *   没有中心孔结构证据的贴边候选继续按背景/支架误检拒绝。
                 */
                if (!has_ring && (min_x == 0U || max_x >= roi_w - 1U)) {
                    auto_locate_record_reject_candidate(result,
                                                        LOCATE_DIAG_EDGE,
                                                        score,
                                                        bbox_w,
                                                        bbox_h,
                                                        area,
                                                        density,
                                                        confidence,
                                                        has_ring);
                    continue;
                }

                /*
                 * 真实零件尺寸硬门槛：
                 * 黑色传送带表面的亮点、接缝和局部凸起通常会形成较小 bbox，
                 * 即使连续几帧存在，也不能作为自动流程的上料目标。
                 */
                if (bbox_area < AUTO_LOCATE_MIN_PART_BBOX_AREA ||
                    bbox_w < AUTO_LOCATE_MIN_PART_BBOX_SIDE ||
                    bbox_h < AUTO_LOCATE_MIN_PART_BBOX_SIDE) {
                    continue;
                }

                /*
                 * 低置信拒绝：
                 * 只要 LOCATE 返回 has_target=1，QML 就可能下发视觉坐标给 F4，
                 * 因此在 overlay 端先挡住低对比、低密度或面积不足的候选。
                 */
                if (confidence < AUTO_LOCATE_MIN_ACCEPT_CONFIDENCE) {
                    auto_locate_record_reject_candidate(result,
                                                        LOCATE_DIAG_CONFIDENCE,
                                                        score,
                                                        bbox_w,
                                                        bbox_h,
                                                        area,
                                                        density,
                                                        confidence,
                                                        has_ring);
                    continue;
                }

                /*
                 * 无环孔候选加严：
                 * 铝色平垫/弹垫在完整进入 ROI 后应能看到中心孔或较完整的环形主体。
                 * 如果暂时没有检测到环孔，就必须等 bbox 更大且置信度更高再放行，
                 * 这样空传送带上的稳定反光不会因为两帧确认而触发自动检测。
                 */
                if (!has_ring &&
                    (bbox_w < AUTO_LOCATE_RING_REQUIRED_BBOX_SIDE ||
                     bbox_h < AUTO_LOCATE_RING_REQUIRED_BBOX_SIDE ||
                     confidence < AUTO_LOCATE_MIN_NON_RING_CONFIDENCE)) {
                    auto_locate_record_reject_candidate(result,
                                                        LOCATE_DIAG_RING,
                                                        score,
                                                        bbox_w,
                                                        bbox_h,
                                                        area,
                                                        density,
                                                        confidence,
                                                        has_ring);
                    continue;
                }
            }

            best_score = score;
            result->diag_code = LOCATE_DIAG_NONE;
            result->has_target = 1;
            result->center_x = (int)(roi_x + (min_x + max_x) / 2U);
            result->center_y = (int)(roi_y + (min_y + max_y) / 2U);
            result->bbox_x = (int)(roi_x + min_x);
            result->bbox_y = (int)(roi_y + min_y);
            result->bbox_w = (int)bbox_w;
            result->bbox_h = (int)bbox_h;
            result->confidence = confidence;
            result->has_ring_hole = has_ring;
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
    char detail[768];
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
                   "bbox_w=%d bbox_h=%d confidence=%u ring=%u "
                   "diag=%u roi_y=%u roi_h=%u thr=%u,%u,%u "
                   "cand_box=%dx%d cand_area=%u cand_density=%u cand_conf=%u cand_ring=%u",
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
                   result.confidence,
                   result.has_ring_hole,
                   result.diag_code,
                   result.diag_roi_y,
                   result.diag_roi_h,
                   result.diag_dark_threshold,
                   result.diag_body_threshold,
                   result.diag_bright_threshold,
                   result.diag_candidate_bbox_w,
                   result.diag_candidate_bbox_h,
                   result.diag_candidate_area,
                   result.diag_candidate_density,
                   result.diag_candidate_confidence,
                   result.diag_candidate_ring);
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
