/*
 * defect_segment.cpp
 *
 * 作用：
 *   STM32MP157 板端 UNet 缺陷分割命令行推理程序。
 *   Qt 首页“检测”按钮会在 MobileNetV3-Small 分类结束后，再调用本程序对同一张当前帧 JPG 做分割。
 *
 * 主要流程：
 *   1. 解析图片、模型、输出目录、ROI 和叠加透明度等命令行参数。
 *   2. 使用 libjpeg 解码 overlay 保存出来的当前帧 JPG。
 *   3. 裁剪中心 ROI，并按训练脚本一致的 RGB、224x224、ImageNet mean/std 生成 NCHW float32 输入。
 *   4. 使用 ONNX Runtime CPUExecutionProvider 执行 UNet INT8 ONNX 图。
 *   5. 对 [1,C,H,W] logits 做逐像素 argmax，得到单通道类别 mask。
 *   6. 写出 ROI 原图 raw.jpg、分割叠加图 overlay.jpg、彩色 mask.png，并输出 RESULT_SEG 一行。
 *
 * 返回值：
 *   0 表示分割成功；非 0 表示参数、图片、模型、输出文件或运行时错误。
 */

#include <onnxruntime_cxx_api.h> /* ONNX Runtime C++ API 提供 Session、Tensor 和 CPU 推理能力。 */

#include "defect_segment_evidence.h" /* 共用 8 邻域连通域统计和 CLEAR/WEAK/STRONG 证据判定。 */

#include <jpeglib.h>             /* libjpeg 用于读取当前帧 JPG，也用于写出 raw/overlay JPG 结果图。 */
#include <png.h>                 /* libpng 用于写出彩色 mask PNG，方便历史页和云端直接预览。 */
#include <setjmp.h>              /* setjmp/longjmp 用于把 libjpeg/libpng 的错误回调转为可控失败。 */
#include <sys/stat.h>            /* stat/mkdir 用于检查输出目录和创建目录。 */
#include <sys/types.h>           /* mode_t 等 POSIX 类型由 mkdir/stat 使用。 */
#include <unistd.h>              /* fsync 用于确保图片文件落盘后再返回成功。 */

#include <algorithm>             /* std::max/std::min/std::fill 用于边界裁剪和像素混合。 */
#include <array>                 /* std::array 用于声明 ONNX Runtime 输入张量形状。 */
#include <chrono>                /* std::chrono 用于统计单次 UNet 推理耗时。 */
#include <cmath>                 /* std::floor/std::round 用于双线性缩放和透明叠加。 */
#include <cstdint>               /* uint8_t 保存 RGB 像素和类别 mask。 */
#include <cstdio>                /* FILE/fopen/fclose 用于读写图片文件。 */
#include <cstdlib>               /* EXIT_SUCCESS/EXIT_FAILURE 表示命令行程序返回语义。 */
#include <cstring>               /* std::strcmp/std::strerror 用于参数解析和错误文本。 */
#include <ctime>                 /* localtime_r/strftime 用于生成输出文件名时间戳。 */
#include <cerrno>                /* errno 用于输出目录和文件写入失败原因。 */
#include <iostream>              /* std::cout/std::cerr 输出 Qt 可解析的一行分割结果。 */
#include <limits>                /* std::numeric_limits 用于初始化 logits 比较值。 */
#include <sstream>               /* std::ostringstream 组装路径和错误信息。 */
#include <stdexcept>             /* std::runtime_error 抛出明确失败原因。 */
#include <string>                /* std::string 保存路径、模型名和输出文本。 */
#include <vector>                /* std::vector 保存图片、输入张量、输出 mask 和颜色表。 */

/* 默认 UNet INT8 模型路径：部署脚本会把用户的新模型复制到这个位置。 */
static const char *DEFAULT_MODEL_PATH =
    "/root/qt_camera_display/models/defect_unet_test_decoder_head_int8.onnx";

/* 默认输出目录：检测历史图片放在 SD 卡 images 目录，重启后历史页仍能预览。 */
static const char *DEFAULT_OUTPUT_DIR = "/mnt/sdcard/images";

/* 默认中心 ROI 边长：与分类模型和 PC 端 infer_camera_onnx.py 的当前测试口径一致。 */
static const int DEFAULT_ROI_SIZE = 300;

/* UNet 输入宽度：当前模型固定导出为 224x224。 */
static const int MODEL_INPUT_WIDTH = 224;

/* UNet 输入高度：当前模型固定导出为 224x224。 */
static const int MODEL_INPUT_HEIGHT = 224;

/* 默认叠加透明度：与 PC 端脚本 alpha=0.45 保持一致。 */
static const float DEFAULT_OVERLAY_ALPHA = 0.45f;

/* ImageNet RGB 均值：训练、PC 推理和板端推理必须一致。 */
static const float IMAGENET_MEAN[3] = {0.485f, 0.456f, 0.406f};

/* ImageNet RGB 标准差：训练、PC 推理和板端推理必须一致。 */
static const float IMAGENET_STD[3] = {0.229f, 0.224f, 0.225f};

/*
 * ImageBuffer 的作用：
 *   保存 libjpeg 解码或程序生成的 RGB888 图片。
 *
 * 字段说明：
 *   width/height 是图片尺寸。
 *   rgb 是按 HWC 排列的 RGB888 像素，长度必须等于 width * height * 3。
 */
struct ImageBuffer {
    int width = 0;              /* width 保存图像宽度，单位像素。 */
    int height = 0;             /* height 保存图像高度，单位像素。 */
    std::vector<uint8_t> rgb;   /* rgb 保存连续 RGB888 数据。 */
};

/*
 * ProgramOptions 的作用：
 *   保存命令行参数解析后的配置。
 *
 * 字段说明：
 *   image_path 是 overlay 保存出来的当前帧 JPG。
 *   model_path 是 UNet INT8 ONNX 模型路径。
 *   output_dir 是 raw/overlay/mask 三张结果图输出目录。
 *   roi_size 是中心裁剪边长，0 表示不裁剪整图缩放。
 *   alpha 是 overlay 中缺陷颜色的叠加强度。
 *   min_component_pixels 是过滤孤立小连通域所需的最小面积。
 *   review_defect_pixels 是进入 WEAK 待复核区的过滤后总像素下限。
 *   bad_defect_pixels 是进入 STRONG 的过滤后总像素下限。
 *   strong_component_pixels 是进入 STRONG 的最大连通域面积下限。
 */
struct ProgramOptions {
    std::string image_path;                         /* 待分割输入图片路径，必须由 --image 提供。 */
    std::string model_path = DEFAULT_MODEL_PATH;    /* UNet 模型路径，默认使用板端部署路径。 */
    std::string output_dir = DEFAULT_OUTPUT_DIR;    /* 输出目录，默认写入 SD 卡历史图片目录。 */
    int roi_size = DEFAULT_ROI_SIZE;                /* ROI 边长，默认 300。 */
    float alpha = DEFAULT_OVERLAY_ALPHA;            /* 叠加透明度，默认 0.45。 */
    int min_component_pixels = defect_segment_evidence::kDefaultMinComponentPixels; /* 小连通域过滤阈值。 */
    int review_defect_pixels = defect_segment_evidence::kDefaultReviewPixels;        /* 待复核总像素阈值。 */
    int bad_defect_pixels = defect_segment_evidence::kDefaultBadPixels;              /* 明确坏品总像素阈值。 */
    int strong_component_pixels = defect_segment_evidence::kDefaultStrongComponentPixels; /* 强缺陷最大连通域阈值。 */
};

/*
 * JpegErrorManager 的作用：
 *   扩展 libjpeg 错误管理器，把错误信息保存到缓冲区并跳回调用点。
 */
struct JpegErrorManager {
    jpeg_error_mgr pub;             /* pub 是 libjpeg 要求放在首位的标准错误管理器。 */
    jmp_buf jump_buffer;            /* jump_buffer 保存 libjpeg 错误发生后要返回的位置。 */
    char message[JMSG_LENGTH_MAX];  /* message 保存 libjpeg 格式化后的错误文本。 */
};

/*
 * PngErrorManager 的作用：
 *   保存 libpng 写文件时的 longjmp 目标。
 */
struct PngErrorManager {
    jmp_buf jump_buffer;            /* jump_buffer 保存 libpng 错误发生后要返回的位置。 */
};

/*
 * print_usage 的作用：
 *   打印 defect-segment 命令行用法，便于 SSH 和 Qt 日志排查参数问题。
 *
 * 参数：
 *   program 是 argv[0] 程序名。
 *
 * 返回值：
 *   无返回值，只输出到 stdout。
 */
static void print_usage(const char *program)
{
    std::cout
        << "用法: " << program
        << " --image <jpg> [--model <onnx>] [--output-dir <dir>] [--roi 300] [--alpha 0.45]"
        << " [--min-component-pixels 20] [--review-defect-pixels 80]"
        << " [--bad-defect-pixels 300] [--strong-component-pixels 120]\n"
        << "输出: RESULT_SEG status=OK|NG evidence=CLEAR|WEAK|STRONG"
        << " raw_defect_pixels=<n> filtered_defect_pixels=<n> largest_component_pixels=<n>"
        << " component_count=<n> retained_component_count=<n> time_ms=<ms>"
        << " raw_path=<jpg> overlay_path=<jpg> mask_path=<png>\n";
}

/*
 * parse_int 的作用：
 *   把命令行字符串解析为 int，并在失败时抛出可读错误。
 *
 * 参数：
 *   text 是待解析字符串。
 *   name 是参数名，用于错误信息。
 *
 * 返回值：
 *   返回解析出的整数。
 */
static int parse_int(const char *text, const char *name)
{
    char *end = nullptr;                  /* end 保存 strtol 停止解析的位置。 */
    long value = std::strtol(text, &end, 10); /* value 保存解析出的长整数，稍后做范围检查。 */

    if (text == nullptr || *text == '\0' || end == text || *end != '\0') {
        throw std::runtime_error(std::string("参数不是整数: ") + name);
    }

    if (value < 0 || value > 4096) {
        throw std::runtime_error(std::string("参数超出范围: ") + name);
    }

    return static_cast<int>(value);
}

/*
 * parse_pixel_count 的作用：
 *   解析板端 UNet 像素阈值，并允许覆盖完整 224x224 mask 的像素数量范围。
 *
 * 参数：
 *   text 是待解析的十进制字符串。
 *   name 是参数名，用于输出明确错误。
 *
 * 返回值：
 *   返回 1~50000 范围内的像素阈值。
 */
static int parse_pixel_count(const char *text, const char *name)
{
    char *end = nullptr;                       /* end 保存 strtol 停止解析的位置。 */
    const long value = std::strtol(text, &end, 10); /* value 保存解析出的长整数。 */

    if (text == nullptr || *text == '\0' || end == text || *end != '\0') {
        throw std::runtime_error(std::string("参数不是整数: ") + name);
    }
    if (value < 1 || value > 50000) {
        throw std::runtime_error(std::string("像素阈值必须在 1~50000: ") + name);
    }

    return static_cast<int>(value);
}

/*
 * parse_float 的作用：
 *   把命令行字符串解析为 float，并在失败或越界时抛出明确错误。
 *
 * 参数：
 *   text 是待解析字符串。
 *   name 是参数名，用于错误信息。
 *
 * 返回值：
 *   返回 0~1 范围内的 float。
 */
static float parse_float(const char *text, const char *name)
{
    char *end = nullptr;                       /* end 保存 strtof 停止解析的位置。 */
    const float value = std::strtof(text, &end); /* value 保存解析出的浮点数。 */

    if (text == nullptr || *text == '\0' || end == text || *end != '\0') {
        throw std::runtime_error(std::string("参数不是小数: ") + name);
    }

    if (value < 0.0f || value > 1.0f) {
        throw std::runtime_error(std::string("参数必须在 0~1: ") + name);
    }

    return value;
}

/*
 * parse_args 的作用：
 *   解析命令行参数并返回 ProgramOptions。
 *
 * 主要流程：
 *   1. 支持图片、模型、输出目录、ROI、透明度和四个连通域证据阈值。
 *   2. 对缺少参数值、未知参数和缺少 --image 做明确报错。
 *
 * 参数：
 *   argc/argv 是 main 收到的命令行参数。
 *
 * 返回值：
 *   返回填好的 ProgramOptions。
 */
static ProgramOptions parse_args(int argc, char **argv)
{
    ProgramOptions options; /* options 保存逐项解析出来的配置。 */

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i]; /* arg 保存当前命令行参数名。 */

        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }

        if (std::strcmp(arg, "--image") == 0) {
            if (++i >= argc) {
                throw std::runtime_error("--image 缺少图片路径");
            }
            options.image_path = argv[i];
            continue;
        }

        if (std::strcmp(arg, "--model") == 0) {
            if (++i >= argc) {
                throw std::runtime_error("--model 缺少模型路径");
            }
            options.model_path = argv[i];
            continue;
        }

        if (std::strcmp(arg, "--output-dir") == 0) {
            if (++i >= argc) {
                throw std::runtime_error("--output-dir 缺少目录路径");
            }
            options.output_dir = argv[i];
            continue;
        }

        if (std::strcmp(arg, "--roi") == 0) {
            if (++i >= argc) {
                throw std::runtime_error("--roi 缺少数值");
            }
            options.roi_size = parse_int(argv[i], "--roi");
            continue;
        }

        if (std::strcmp(arg, "--alpha") == 0) {
            if (++i >= argc) {
                throw std::runtime_error("--alpha 缺少数值");
            }
            options.alpha = parse_float(argv[i], "--alpha");
            continue;
        }

        if (std::strcmp(arg, "--min-component-pixels") == 0) {
            if (++i >= argc) {
                throw std::runtime_error("--min-component-pixels 缺少数值");
            }
            options.min_component_pixels = parse_pixel_count(argv[i], "--min-component-pixels");
            continue;
        }

        if (std::strcmp(arg, "--review-defect-pixels") == 0) {
            if (++i >= argc) {
                throw std::runtime_error("--review-defect-pixels 缺少数值");
            }
            options.review_defect_pixels = parse_pixel_count(argv[i], "--review-defect-pixels");
            continue;
        }

        if (std::strcmp(arg, "--bad-defect-pixels") == 0) {
            if (++i >= argc) {
                throw std::runtime_error("--bad-defect-pixels 缺少数值");
            }
            options.bad_defect_pixels = parse_pixel_count(argv[i], "--bad-defect-pixels");
            continue;
        }

        if (std::strcmp(arg, "--strong-component-pixels") == 0) {
            if (++i >= argc) {
                throw std::runtime_error("--strong-component-pixels 缺少数值");
            }
            options.strong_component_pixels = parse_pixel_count(argv[i], "--strong-component-pixels");
            continue;
        }

        throw std::runtime_error(std::string("未知参数: ") + arg);
    }

    if (options.image_path.empty()) {
        throw std::runtime_error("必须指定 --image <jpg>");
    }

    const defect_segment_evidence::SegmentEvidenceSettings evidenceSettings{
        options.min_component_pixels,
        options.review_defect_pixels,
        options.bad_defect_pixels,
        options.strong_component_pixels
    }; /* evidenceSettings 复用生产算法校验入口，保证 CLI 和 mask 判定接受同一组参数。 */
    defect_segment_evidence::validate_segment_evidence_settings(evidenceSettings);

    return options;
}

/*
 * jpeg_error_exit 的作用：
 *   libjpeg 发生致命错误时保存错误文本并跳回调用点。
 *
 * 参数：
 *   cinfo 是 libjpeg 解码或压缩对象。
 *
 * 返回值：
 *   不直接返回，通过 longjmp 回到 setjmp 位置。
 */
static void jpeg_error_exit(j_common_ptr cinfo)
{
    JpegErrorManager *err = reinterpret_cast<JpegErrorManager *>(cinfo->err); /* err 是调用侧设置的错误上下文。 */

    (*cinfo->err->format_message)(cinfo, err->message);
    longjmp(err->jump_buffer, 1);
}

/*
 * read_jpeg_rgb 的作用：
 *   使用 libjpeg 把 JPG 图片解码成 RGB888。
 *
 * 参数：
 *   path 是 JPG 文件路径。
 *
 * 返回值：
 *   返回 ImageBuffer，像素格式固定为 RGB888。
 */
static ImageBuffer read_jpeg_rgb(const std::string &path)
{
    FILE *file = std::fopen(path.c_str(), "rb"); /* file 保存输入 JPG 文件句柄。 */
    if (file == nullptr) {
        throw std::runtime_error("无法打开图片: " + path);
    }

    jpeg_decompress_struct cinfo; /* cinfo 保存 libjpeg 解码状态。 */
    JpegErrorManager jerr;        /* jerr 保存错误回跳现场和错误文本。 */
    ImageBuffer image;            /* image 保存最终 RGB 解码结果。 */

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    jerr.message[0] = '\0';

    if (setjmp(jerr.jump_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        std::fclose(file);
        throw std::runtime_error(std::string("JPG 解码失败: ") + jerr.message);
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, file);
    jpeg_read_header(&cinfo, TRUE);

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    image.width = static_cast<int>(cinfo.output_width);
    image.height = static_cast<int>(cinfo.output_height);
    image.rgb.resize(static_cast<size_t>(image.width) * image.height * 3U);

    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t *row = image.rgb.data()
            + static_cast<size_t>(cinfo.output_scanline) * image.width * 3U; /* row 指向当前输出行首地址。 */
        JSAMPROW row_pointer[1] = {row};                                    /* row_pointer 是 libjpeg 要求的行指针数组。 */

        jpeg_read_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    std::fclose(file);

    return image;
}

/*
 * pixel_at 的作用：
 *   读取图片中指定坐标的 RGB 像素。
 *
 * 参数：
 *   image 是 RGB 图片。
 *   x/y 是像素坐标，调用者保证已经裁剪到合法范围。
 *   channel 是 RGB 通道索引，0=R、1=G、2=B。
 *
 * 返回值：
 *   返回 0~255 的像素值。
 */
static uint8_t pixel_at(const ImageBuffer &image, int x, int y, int channel)
{
    const size_t index = (static_cast<size_t>(y) * image.width + x) * 3U
        + static_cast<size_t>(channel); /* index 是 HWC RGB 中的线性下标。 */
    return image.rgb[index];
}

/*
 * crop_center_roi 的作用：
 *   从输入图片中裁剪中心 ROI，供结果 raw.jpg 和 overlay.jpg 使用。
 *
 * 参数：
 *   image 是完整当前帧 RGB 图片。
 *   roi_size 是中心 ROI 边长，0 表示返回整图。
 *
 * 返回值：
 *   返回裁剪后的 RGB 图片。
 */
static ImageBuffer crop_center_roi(const ImageBuffer &image, int roi_size)
{
    if (image.width <= 0 || image.height <= 0 || image.rgb.empty()) {
        throw std::runtime_error("输入图片为空");
    }

    int crop_w = image.width;  /* crop_w 保存实际裁剪宽度。 */
    int crop_h = image.height; /* crop_h 保存实际裁剪高度。 */
    int crop_x = 0;            /* crop_x 保存裁剪区域左上角 x。 */
    int crop_y = 0;            /* crop_y 保存裁剪区域左上角 y。 */

    if (roi_size > 0) {
        const int size = std::min(roi_size, std::min(image.width, image.height)); /* size 避免 ROI 大于输入图片。 */
        crop_w = size;
        crop_h = size;
        crop_x = (image.width - size) / 2;
        crop_y = (image.height - size) / 2;
    }

    ImageBuffer roi; /* roi 保存裁剪后的结果图。 */
    roi.width = crop_w;
    roi.height = crop_h;
    roi.rgb.resize(static_cast<size_t>(crop_w) * crop_h * 3U);

    for (int y = 0; y < crop_h; y++) {
        const uint8_t *src = image.rgb.data()
            + (static_cast<size_t>(crop_y + y) * image.width + crop_x) * 3U; /* src 指向原图对应行的 ROI 起点。 */
        uint8_t *dst = roi.rgb.data() + static_cast<size_t>(y) * crop_w * 3U; /* dst 指向 ROI 输出行。 */
        std::copy(src, src + static_cast<size_t>(crop_w) * 3U, dst);
    }

    return roi;
}

/*
 * resize_rgb 的作用：
 *   使用双线性插值把 RGB 图片缩放到指定尺寸。
 *
 * 参数：
 *   image 是输入 RGB 图片。
 *   out_w/out_h 是输出尺寸。
 *
 * 返回值：
 *   返回缩放后的 RGB 图片。
 */
static ImageBuffer resize_rgb(const ImageBuffer &image, int out_w, int out_h)
{
    ImageBuffer output; /* output 保存缩放后的 RGB 图。 */
    output.width = out_w;
    output.height = out_h;
    output.rgb.resize(static_cast<size_t>(out_w) * out_h * 3U);

    for (int oy = 0; oy < out_h; oy++) {
        const float src_y = (static_cast<float>(oy) + 0.5f) * image.height / out_h - 0.5f; /* src_y 是输出像素中心对应的输入 y。 */
        const int y0 = std::max(0, std::min(image.height - 1, static_cast<int>(std::floor(src_y)))); /* y0 是上方采样行。 */
        const int y1 = std::max(0, std::min(image.height - 1, y0 + 1));                                /* y1 是下方采样行。 */
        const float wy = src_y - y0;                                                                  /* wy 是 y 方向插值权重。 */

        for (int ox = 0; ox < out_w; ox++) {
            const float src_x = (static_cast<float>(ox) + 0.5f) * image.width / out_w - 0.5f; /* src_x 是输出像素中心对应的输入 x。 */
            const int x0 = std::max(0, std::min(image.width - 1, static_cast<int>(std::floor(src_x)))); /* x0 是左侧采样列。 */
            const int x1 = std::max(0, std::min(image.width - 1, x0 + 1));                               /* x1 是右侧采样列。 */
            const float wx = src_x - x0;                                                               /* wx 是 x 方向插值权重。 */

            for (int channel = 0; channel < 3; channel++) {
                const float p00 = static_cast<float>(pixel_at(image, x0, y0, channel)); /* p00 左上角像素。 */
                const float p01 = static_cast<float>(pixel_at(image, x1, y0, channel)); /* p01 右上角像素。 */
                const float p10 = static_cast<float>(pixel_at(image, x0, y1, channel)); /* p10 左下角像素。 */
                const float p11 = static_cast<float>(pixel_at(image, x1, y1, channel)); /* p11 右下角像素。 */
                const float top = p00 + (p01 - p00) * wx;                               /* top 是上边插值结果。 */
                const float bottom = p10 + (p11 - p10) * wx;                            /* bottom 是下边插值结果。 */
                const float value = top + (bottom - top) * wy;                          /* value 是最终双线性结果。 */
                const size_t out_index =
                    (static_cast<size_t>(oy) * out_w + ox) * 3U + static_cast<size_t>(channel);

                output.rgb[out_index] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, std::round(value))));
            }
        }
    }

    return output;
}

/*
 * preprocess_image 的作用：
 *   把 ROI RGB 图片转换成 UNet 输入 tensor。
 *
 * 主要流程：
 *   1. 把 ROI 双线性缩放到 224x224。
 *   2. 像素缩放到 0~1。
 *   3. 按 RGB 通道分别执行 ImageNet 标准化。
 *   4. 输出 NCHW float32，形状为 [1,3,224,224]。
 *
 * 参数：
 *   roi 是中心 ROI RGB 图片。
 *
 * 返回值：
 *   返回长度 1*3*224*224 的 float 向量。
 */
static std::vector<float> preprocess_image(const ImageBuffer &roi)
{
    const ImageBuffer resized = resize_rgb(roi, MODEL_INPUT_WIDTH, MODEL_INPUT_HEIGHT); /* resized 是模型尺寸 RGB 图。 */
    std::vector<float> tensor(static_cast<size_t>(3 * MODEL_INPUT_WIDTH * MODEL_INPUT_HEIGHT), 0.0f);

    for (int y = 0; y < MODEL_INPUT_HEIGHT; y++) {
        for (int x = 0; x < MODEL_INPUT_WIDTH; x++) {
            for (int channel = 0; channel < 3; channel++) {
                const float value01 = static_cast<float>(pixel_at(resized, x, y, channel)) / 255.0f; /* value01 是 0~1 像素值。 */
                const float normalized = (value01 - IMAGENET_MEAN[channel]) / IMAGENET_STD[channel]; /* normalized 是标准化结果。 */
                const size_t tensor_index =
                    static_cast<size_t>(channel) * MODEL_INPUT_WIDTH * MODEL_INPUT_HEIGHT
                    + static_cast<size_t>(y) * MODEL_INPUT_WIDTH
                    + static_cast<size_t>(x);

                tensor[tensor_index] = normalized;
            }
        }
    }

    return tensor;
}

/*
 * build_palette 的作用：
 *   生成 RGB 颜色表，用于把 mask 类别 ID 转成彩色结果图。
 *
 * 关键说明：
 *   PC 端脚本使用 BGR 表；这里全程序使用 RGB，因此颜色视觉效果保持一致但通道顺序已经换成 RGB。
 *
 * 参数：
 *   class_count 是从 ONNX 输出形状读取的实际类别数。
 *
 * 返回值：
 *   返回 class_count * 3 长度的 RGB 颜色表。
 */
static std::vector<uint8_t> build_palette(size_t class_count)
{
    const uint8_t fixed_palette[][3] = {
        {0, 0, 0},       /* 0 背景：黑色。 */
        {255, 0, 0},     /* 1 缺陷类：红色。 */
        {255, 165, 0},   /* 2 缺陷类：橙色。 */
        {0, 0, 255},     /* 3 缺陷类：蓝色。 */
        {255, 0, 255},   /* 4 缺陷类：紫色。 */
        {255, 255, 0},   /* 5 缺陷类：黄色。 */
    };
    const size_t fixed_color_count = sizeof(fixed_palette) / sizeof(fixed_palette[0]); /* fixed_color_count 保存内置颜色数量。 */
    std::vector<uint8_t> palette(class_count * 3U); /* palette 按模型真实类别数分配，兼容两类和旧六类模型。 */

    for (size_t class_id = 0; class_id < class_count; class_id++) {
        /* 超过内置颜色数量时循环使用五种缺陷色，但类别 0 始终保持黑色背景。 */
        const size_t color_index = class_id < fixed_color_count
            ? class_id
            : 1U + ((class_id - 1U) % (fixed_color_count - 1U));
        for (int channel = 0; channel < 3; channel++) {
            palette[class_id * 3U + static_cast<size_t>(channel)] =
                fixed_palette[color_index][channel];
        }
    }

    return palette;
}

/*
 * mask_to_color 的作用：
 *   把单通道类别 mask 转成 RGB 彩色 mask。
 *
 * 参数：
 *   mask 是类别 ID 数组，尺寸 mask_w * mask_h。
 *   mask_w/mask_h 是 mask 尺寸。
 *   palette 是 RGB 颜色表。
 *   class_count 是模型实际类别数，用于校验类别下标不越界。
 *
 * 返回值：
 *   返回 RGB 彩色 mask 图片。
 */
static ImageBuffer mask_to_color(const std::vector<uint8_t> &mask,
                                 int mask_w,
                                 int mask_h,
                                 const std::vector<uint8_t> &palette,
                                 size_t class_count)
{
    ImageBuffer color; /* color 保存彩色 mask 输出图。 */
    color.width = mask_w;
    color.height = mask_h;
    color.rgb.resize(static_cast<size_t>(mask_w) * mask_h * 3U);

    for (int y = 0; y < mask_h; y++) {
        for (int x = 0; x < mask_w; x++) {
            const size_t mask_index = static_cast<size_t>(y) * mask_w + x; /* mask_index 是当前像素类别下标。 */
            const size_t class_id = std::min<size_t>(mask[mask_index], class_count - 1U); /* class_id 防止异常类别越界。 */

            for (int channel = 0; channel < 3; channel++) {
                color.rgb[mask_index * 3U + static_cast<size_t>(channel)] =
                    palette[static_cast<size_t>(class_id) * 3U + static_cast<size_t>(channel)];
            }
        }
    }

    return color;
}

/*
 * resize_mask_nearest 的作用：
 *   用最近邻插值把 224x224 类别 mask 放大到 ROI 原图尺寸。
 *
 * 参数：
 *   mask 是输入类别 mask。
 *   in_w/in_h 是输入 mask 尺寸。
 *   out_w/out_h 是目标 ROI 尺寸。
 *
 * 返回值：
 *   返回目标尺寸的类别 mask。
 */
static std::vector<uint8_t> resize_mask_nearest(const std::vector<uint8_t> &mask,
                                                int in_w,
                                                int in_h,
                                                int out_w,
                                                int out_h)
{
    std::vector<uint8_t> output(static_cast<size_t>(out_w) * out_h, 0U); /* output 保存放大后的类别 mask。 */

    for (int y = 0; y < out_h; y++) {
        const int src_y = std::max(0, std::min(in_h - 1, y * in_h / out_h)); /* src_y 是最近邻源行。 */
        for (int x = 0; x < out_w; x++) {
            const int src_x = std::max(0, std::min(in_w - 1, x * in_w / out_w)); /* src_x 是最近邻源列。 */
            output[static_cast<size_t>(y) * out_w + x] =
                mask[static_cast<size_t>(src_y) * in_w + src_x];
        }
    }

    return output;
}

/*
 * overlay_mask 的作用：
 *   把类别 mask 的非背景区域叠加到 ROI 原图上。
 *
 * 参数：
 *   roi 是中心 ROI 原图。
 *   mask 是模型实际输出尺寸的类别 mask。
 *   mask_w/mask_h 是模型输出 mask 的实际宽高。
 *   palette 是 RGB 颜色表。
 *   class_count 是模型实际类别数。
 *   alpha 是缺陷颜色叠加强度。
 *
 * 返回值：
 *   返回和 ROI 同尺寸的 overlay RGB 图。
 */
static ImageBuffer overlay_mask(const ImageBuffer &roi,
                                const std::vector<uint8_t> &mask,
                                int mask_w,
                                int mask_h,
                                const std::vector<uint8_t> &palette,
                                size_t class_count,
                                float alpha)
{
    ImageBuffer output = roi; /* output 从原图开始，只改非背景缺陷区域。 */
    const std::vector<uint8_t> resized_mask =
        resize_mask_nearest(mask, mask_w, mask_h, roi.width, roi.height);

    for (int y = 0; y < roi.height; y++) {
        for (int x = 0; x < roi.width; x++) {
            const size_t pixel_index = static_cast<size_t>(y) * roi.width + x; /* pixel_index 是当前 ROI 像素下标。 */
            const size_t class_id = std::min<size_t>(resized_mask[pixel_index], class_count - 1U); /* class_id 使用动态类别上限。 */

            if (class_id == 0U) {
                continue;
            }

            for (int channel = 0; channel < 3; channel++) {
                const float base = static_cast<float>(roi.rgb[pixel_index * 3U + static_cast<size_t>(channel)]);
                const float color = static_cast<float>(palette[static_cast<size_t>(class_id) * 3U + static_cast<size_t>(channel)]);
                const float blended = base * (1.0f - alpha) + color * alpha;

                output.rgb[pixel_index * 3U + static_cast<size_t>(channel)] =
                    static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, std::round(blended))));
            }
        }
    }

    return output;
}

/*
 * mkdir_p 的作用：
 *   递归创建输出目录，等价于简化版 `mkdir -p`。
 *
 * 参数：
 *   path 是要创建的目录路径。
 *
 * 返回值：
 *   成功返回 true；失败抛出异常。
 */
static void mkdir_p(const std::string &path)
{
    if (path.empty()) {
        throw std::runtime_error("输出目录为空");
    }

    std::string current; /* current 保存逐级创建时的路径前缀。 */
    size_t pos = 0;      /* pos 保存当前扫描到的位置。 */

    if (path[0] == '/') {
        current = "/";
        pos = 1;
    }

    while (pos <= path.size()) {
        const size_t next = path.find('/', pos); /* next 是下一个路径分隔符位置。 */
        const std::string part = path.substr(pos, next == std::string::npos ? std::string::npos : next - pos);

        if (!part.empty()) {
            if (!current.empty() && current.back() != '/') {
                current += "/";
            }
            current += part;

            struct stat st; /* st 保存现有路径状态。 */
            if (stat(current.c_str(), &st) != 0) {
                if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                    throw std::runtime_error("创建输出目录失败: " + current + " " + std::strerror(errno));
                }
            } else if (!S_ISDIR(st.st_mode)) {
                throw std::runtime_error("输出路径不是目录: " + current);
            }
        }

        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }
}

/*
 * current_timestamp_stem 的作用：
 *   生成用于结果图文件名的时间戳 stem。
 *
 * 返回值：
 *   返回形如 segment_20260518_132447_634 的文件名前缀。
 */
static std::string current_timestamp_stem()
{
    struct timespec ts;       /* ts 保存秒和纳秒，用于生成毫秒级文件名。 */
    struct tm local_tm;       /* local_tm 保存本地时间拆分结果。 */
    char buffer[64];          /* buffer 保存 strftime 生成的日期时间。 */
    std::ostringstream stream; /* stream 拼出最终 stem。 */

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        throw std::runtime_error(std::string("读取系统时间失败: ") + std::strerror(errno));
    }

    if (localtime_r(&ts.tv_sec, &local_tm) == nullptr) {
        throw std::runtime_error("转换本地时间失败");
    }

    if (std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &local_tm) == 0) {
        throw std::runtime_error("格式化时间失败");
    }

    stream << "segment_" << buffer << "_" << (ts.tv_nsec / 1000000L);
    return stream.str();
}

/*
 * join_path 的作用：
 *   拼接目录和文件名，避免调用者手写斜杠判断。
 *
 * 参数：
 *   dir 是目录。
 *   name 是文件名。
 *
 * 返回值：
 *   返回完整路径字符串。
 */
static std::string join_path(const std::string &dir, const std::string &name)
{
    if (dir.empty()) {
        return name;
    }

    if (dir.back() == '/') {
        return dir + name;
    }

    return dir + "/" + name;
}

/*
 * fsync_file_stream 的作用：
 *   把 FILE* 缓冲和内核文件缓存刷到存储设备。
 *
 * 参数：
 *   file 是已经打开的文件。
 *
 * 返回值：
 *   成功返回 true；失败返回 false。
 */
static bool fsync_file_stream(FILE *file)
{
    if (std::fflush(file) != 0) {
        return false;
    }

    if (::fsync(fileno(file)) != 0) {
        return false;
    }

    return true;
}

/*
 * write_rgb_as_jpeg 的作用：
 *   把 RGB888 图片编码成 JPEG 文件，并在成功返回前 fsync。
 *
 * 参数：
 *   image 是待写出的 RGB 图片。
 *   path 是输出 JPG 路径。
 *   quality 是 JPEG 质量，取值 1~100。
 *
 * 返回值：
 *   无返回值；失败抛出异常。
 */
static void write_rgb_as_jpeg(const ImageBuffer &image, const std::string &path, int quality)
{
    FILE *file = std::fopen(path.c_str(), "wb"); /* file 保存输出 JPG 文件句柄。 */
    if (file == nullptr) {
        throw std::runtime_error("无法创建 JPG: " + path + " " + std::strerror(errno));
    }

    jpeg_compress_struct cinfo; /* cinfo 保存 libjpeg 压缩状态。 */
    JpegErrorManager jerr;      /* jerr 保存错误回跳现场和错误文本。 */

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    jerr.message[0] = '\0';

    if (setjmp(jerr.jump_buffer)) {
        jpeg_destroy_compress(&cinfo);
        std::fclose(file);
        throw std::runtime_error(std::string("JPG 编码失败: ") + jerr.message);
    }

    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, file);

    cinfo.image_width = static_cast<JDIMENSION>(image.width);
    cinfo.image_height = static_cast<JDIMENSION>(image.height);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row_pointer[1]; /* row_pointer 是 libjpeg 要求的行指针数组。 */
        row_pointer[0] = const_cast<JSAMPROW>(
            image.rgb.data() + static_cast<size_t>(cinfo.next_scanline) * image.width * 3U);
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);

    if (!fsync_file_stream(file)) {
        jpeg_destroy_compress(&cinfo);
        std::fclose(file);
        throw std::runtime_error("JPG fsync 失败: " + path + " " + std::strerror(errno));
    }

    jpeg_destroy_compress(&cinfo);
    std::fclose(file);
}

/*
 * png_error_callback 的作用：
 *   libpng 写入失败时跳回 write_rgb_as_png() 的错误处理分支。
 *
 * 参数：
 *   png_ptr 是 libpng 写上下文。
 *   message 是 libpng 提供的错误文本。
 *
 * 返回值：
 *   不直接返回，通过 longjmp 回到调用点。
 */
static void png_error_callback(png_structp png_ptr, png_const_charp message)
{
    PngErrorManager *err = reinterpret_cast<PngErrorManager *>(png_get_error_ptr(png_ptr)); /* err 是调用侧错误上下文。 */

    (void)message;
    longjmp(err->jump_buffer, 1);
}

/*
 * png_warning_callback 的作用：
 *   接收 libpng 警告但不终止流程，避免轻微元数据问题影响检测。
 *
 * 参数：
 *   png_ptr 是 libpng 写上下文。
 *   message 是警告文本。
 *
 * 返回值：
 *   无返回值。
 */
static void png_warning_callback(png_structp png_ptr, png_const_charp message)
{
    (void)png_ptr;
    (void)message;
}

/*
 * write_rgb_as_png 的作用：
 *   把 RGB888 图片写成 PNG 文件，并在成功返回前 fsync。
 *
 * 参数：
 *   image 是待写出的 RGB 图片。
 *   path 是输出 PNG 路径。
 *
 * 返回值：
 *   无返回值；失败抛出异常。
 */
static void write_rgb_as_png(const ImageBuffer &image, const std::string &path)
{
    FILE *file = std::fopen(path.c_str(), "wb"); /* file 保存输出 PNG 文件句柄。 */
    if (file == nullptr) {
        throw std::runtime_error("无法创建 PNG: " + path + " " + std::strerror(errno));
    }

    PngErrorManager err; /* err 保存 libpng 错误回跳现场。 */
    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                                  &err,
                                                  png_error_callback,
                                                  png_warning_callback);
    if (png_ptr == nullptr) {
        std::fclose(file);
        throw std::runtime_error("创建 PNG 写上下文失败");
    }

    png_infop info_ptr = png_create_info_struct(png_ptr); /* info_ptr 保存 PNG 头信息。 */
    if (info_ptr == nullptr) {
        png_destroy_write_struct(&png_ptr, nullptr);
        std::fclose(file);
        throw std::runtime_error("创建 PNG 信息上下文失败");
    }

    if (setjmp(err.jump_buffer)) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        std::fclose(file);
        throw std::runtime_error("PNG 编码失败: " + path);
    }

    std::vector<png_bytep> rows(static_cast<size_t>(image.height)); /* rows 保存每一行 RGB 数据指针。 */
    for (int y = 0; y < image.height; y++) {
        rows[static_cast<size_t>(y)] = const_cast<png_bytep>(
            image.rgb.data() + static_cast<size_t>(y) * image.width * 3U);
    }

    png_init_io(png_ptr, file);
    png_set_IHDR(png_ptr,
                 info_ptr,
                 static_cast<png_uint_32>(image.width),
                 static_cast<png_uint_32>(image.height),
                 8,
                 PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);
    png_write_image(png_ptr, rows.data());
    png_write_end(png_ptr, info_ptr);

    if (!fsync_file_stream(file)) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        std::fclose(file);
        throw std::runtime_error("PNG fsync 失败: " + path + " " + std::strerror(errno));
    }

    png_destroy_write_struct(&png_ptr, &info_ptr);
    std::fclose(file);
}

/*
 * logits_to_mask 的作用：
 *   把 ONNX 输出 logits 转换成逐像素类别 mask。
 *
 * 参数：
 *   logits 是输出张量首地址。
 *   count 是输出张量元素数量。
 *   class_count 是模型输出类别数。
 *   output_width/output_height 是模型输出 mask 的宽高。
 *
 * 返回值：
 *   返回模型实际输出尺寸的类别 mask。
 */
static std::vector<uint8_t> logits_to_mask(const float *logits,
                                           size_t count,
                                           size_t class_count,
                                           int output_width,
                                           int output_height)
{
    const size_t output_pixels = static_cast<size_t>(output_width) * static_cast<size_t>(output_height); /* output_pixels 保存单通道像素数。 */
    const size_t expected = class_count * output_pixels; /* expected 保存单批次 NCHW 输出应有的精确元素数。 */
    if (count != expected) {
        throw std::runtime_error(
            "UNet 输出元素数量与模型元数据不一致: elements="
            + std::to_string(count)
            + " expected=" + std::to_string(expected));
    }

    std::vector<uint8_t> mask(output_pixels, 0U); /* mask 保存每个输出像素的 argmax 类别。 */

    for (int y = 0; y < output_height; y++) {
        for (int x = 0; x < output_width; x++) {
            int best_class = 0;                                            /* best_class 保存当前像素最大 logits 类别。 */
            float best_value = -std::numeric_limits<float>::infinity();    /* best_value 保存当前最大 logits。 */

            for (size_t class_id = 0; class_id < class_count; class_id++) {
                const size_t offset =
                    class_id * output_pixels
                    + static_cast<size_t>(y) * static_cast<size_t>(output_width)
                    + static_cast<size_t>(x);
                const float value = logits[offset];

                if (value > best_value) {
                    best_value = value;
                    best_class = static_cast<int>(class_id);
                }
            }

            mask[static_cast<size_t>(y) * static_cast<size_t>(output_width) + static_cast<size_t>(x)] =
                static_cast<uint8_t>(best_class);
        }
    }

    return mask;
}

/*
 * tensor_element_count 的作用：
 *   计算 ONNX Runtime 输出张量的元素数量。
 *
 * 参数：
 *   value 是输出张量。
 *
 * 返回值：
 *   返回所有维度相乘的元素个数。
 */
static size_t tensor_element_count(const Ort::Value &value)
{
    Ort::TensorTypeAndShapeInfo info = value.GetTensorTypeAndShapeInfo(); /* info 保存输出张量形状信息。 */
    std::vector<int64_t> shape = info.GetShape();                         /* shape 保存每个维度长度。 */
    size_t count = 1U;                                                     /* count 保存维度乘积。 */

    for (int64_t dim : shape) {
        if (dim <= 0) {
            throw std::runtime_error("UNet 输出张量维度非法");
        }
        count *= static_cast<size_t>(dim);
    }

    return count;
}

/*
 * main 的作用：
 *   程序入口，执行一次 UNet 分割并输出 Qt 可解析的一行 RESULT_SEG。
 *
 * 主要流程：
 *   1. 解析命令行。
 *   2. 读取当前帧 JPG 并裁剪中心 ROI。
 *   3. 运行 ONNX Runtime 推理。
 *   4. 生成 raw/overlay/mask 三张结果图。
 *   5. 输出 RESULT_SEG，供 Qt 后台线程解析并写入历史记录。
 *
 * 返回值：
 *   EXIT_SUCCESS 表示成功；EXIT_FAILURE 表示失败。
 */
int main(int argc, char **argv)
{
    try {
        const ProgramOptions options = parse_args(argc, argv); /* options 保存命令行配置。 */
        mkdir_p(options.output_dir);

        const ImageBuffer image = read_jpeg_rgb(options.image_path);       /* image 保存完整当前帧。 */
        const ImageBuffer roi = crop_center_roi(image, options.roi_size);  /* roi 保存中心检测区域原图。 */
        std::vector<float> input_tensor = preprocess_image(roi);           /* input_tensor 保存 NCHW float32 输入。 */

        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "defect-segment");        /* env 保存 ONNX Runtime 全局环境。 */
        Ort::SessionOptions session_options;                              /* session_options 保存 CPU 推理配置。 */
        session_options.SetIntraOpNumThreads(2);
        session_options.SetInterOpNumThreads(1);
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

        Ort::Session session(env, options.model_path.c_str(), session_options); /* session 加载 UNet INT8 模型。 */

        /* 当前辅助程序只支持一个 NCHW 分割输出，多输出模型无法确定哪一个张量是像素 logits。 */
        if (session.GetOutputCount() != 1U) {
            throw std::runtime_error("UNet 模型必须且只能包含一个输出张量");
        }

        /* 从模型元数据读取真实输出形状，确保两类新模型和旧六类模型都不会发生越界读取。 */
        const Ort::TypeInfo output_type_info = session.GetOutputTypeInfo(0);
        const auto output_tensor_info = output_type_info.GetTensorTypeAndShapeInfo();
        /* 后处理会把输出缓冲区解释为 float，因此必须先拒绝其他元素类型，避免错误指针解释导致越界或错误结果。 */
        if (output_tensor_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            throw std::runtime_error("UNet 输出元素类型必须为 float32");
        }
        const std::vector<int64_t> output_shape = output_tensor_info.GetShape();
        if (output_shape.size() != 4U) {
            throw std::runtime_error("UNet 输出形状必须为 [batch, classes, height, width]");
        }
        if (output_shape[0] != 1 || output_shape[1] <= 1 || output_shape[1] > 256) {
            throw std::runtime_error("UNet 输出 batch 必须为 1，classes 必须在 2 到 256 之间");
        }
        if (output_shape[2] != MODEL_INPUT_HEIGHT || output_shape[3] != MODEL_INPUT_WIDTH) {
            throw std::runtime_error("UNet 输出高宽必须与 224x224 输入一致");
        }

        const size_t model_class_count = static_cast<size_t>(output_shape[1]); /* model_class_count 保存模型实际输出通道数。 */
        const int model_output_height = static_cast<int>(output_shape[2]);     /* model_output_height 保存输出 mask 高度。 */
        const int model_output_width = static_cast<int>(output_shape[3]);      /* model_output_width 保存输出 mask 宽度。 */
        Ort::AllocatorWithDefaultOptions allocator;                            /* allocator 用于读取输入输出节点名。 */
        Ort::AllocatedStringPtr input_name = session.GetInputNameAllocated(0, allocator);
        Ort::AllocatedStringPtr output_name = session.GetOutputNameAllocated(0, allocator);
        const char *input_names[] = {input_name.get()};
        const char *output_names[] = {output_name.get()};
        std::array<int64_t, 4> input_shape = {1, 3, MODEL_INPUT_HEIGHT, MODEL_INPUT_WIDTH};
        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input_value = Ort::Value::CreateTensor<float>(
            memory_info,
            input_tensor.data(),
            input_tensor.size(),
            input_shape.data(),
            input_shape.size());

        const auto infer_start = std::chrono::steady_clock::now();
        std::vector<Ort::Value> outputs = session.Run(
            Ort::RunOptions{nullptr},
            input_names,
            &input_value,
            1,
            output_names,
            1);
        const auto infer_end = std::chrono::steady_clock::now();

        if (outputs.size() != 1U || !outputs.front().IsTensor()) {
            throw std::runtime_error("ONNX Runtime 未返回唯一的 UNet 输出张量");
        }

        float *logits = outputs.front().GetTensorMutableData<float>();        /* logits 指向模型实际 NCHW 输出。 */
        const size_t logits_count = tensor_element_count(outputs.front());     /* logits_count 保存输出元素总数。 */
        const std::vector<uint8_t> mask = logits_to_mask(
            logits,
            logits_count,
            model_class_count,
            model_output_width,
            model_output_height);
        const defect_segment_evidence::SegmentEvidenceSettings evidenceSettings{
            options.min_component_pixels,
            options.review_defect_pixels,
            options.bad_defect_pixels,
            options.strong_component_pixels
        }; /* evidenceSettings 是本次进程启动时固定的板端阈值快照。 */
        const defect_segment_evidence::SegmentEvidenceStats evidence =
            defect_segment_evidence::analyze_segment_evidence(
                mask,
                model_output_width,
                model_output_height,
                evidenceSettings); /* evidence 保存 8 邻域过滤后的完整诊断和三级结论。 */
        const bool is_ng = evidence.level != defect_segment_evidence::SegmentEvidenceLevel::Clear;
        const std::vector<uint8_t> palette = build_palette(model_class_count);
        const ImageBuffer color_mask = mask_to_color(
            mask,
            model_output_width,
            model_output_height,
            palette,
            model_class_count);
        const ImageBuffer overlay = overlay_mask(
            roi,
            mask,
            model_output_width,
            model_output_height,
            palette,
            model_class_count,
            options.alpha);
        const std::string stem = current_timestamp_stem();
        const std::string raw_path = join_path(options.output_dir, stem + "_raw.jpg");
        const std::string overlay_path = join_path(options.output_dir, stem + "_overlay.jpg");
        const std::string mask_path = join_path(options.output_dir, stem + "_mask.png");

        write_rgb_as_jpeg(roi, raw_path, 95);
        write_rgb_as_jpeg(overlay, overlay_path, 95);
        write_rgb_as_png(color_mask, mask_path);

        const double time_ms = std::chrono::duration<double, std::milli>(infer_end - infer_start).count();

        std::cout.setf(std::ios::fixed);
        std::cout.precision(4);
        std::cout
            << "RESULT_SEG"
            << " status=" << (is_ng ? "NG" : "OK")
            << " evidence=" << defect_segment_evidence::segment_evidence_level_name(evidence.level)
            << " defect_pixels=" << evidence.rawDefectPixels
            << " raw_defect_pixels=" << evidence.rawDefectPixels
            << " filtered_defect_pixels=" << evidence.filteredDefectPixels
            << " largest_component_pixels=" << evidence.largestComponentPixels
            << " component_count=" << evidence.componentCount
            << " retained_component_count=" << evidence.retainedComponentCount
            << " min_component_pixels=" << options.min_component_pixels
            << " review_defect_pixels=" << options.review_defect_pixels
            << " bad_defect_pixels=" << options.bad_defect_pixels
            << " strong_component_pixels=" << options.strong_component_pixels
            << " classes=" << model_class_count
            << " time_ms=" << time_ms
            << " raw_path=" << raw_path
            << " overlay_path=" << overlay_path
            << " mask_path=" << mask_path
            << " image=" << options.image_path
            << '\n';

        return EXIT_SUCCESS;
    } catch (const std::exception &ex) {
        std::cerr << "ERROR " << ex.what() << '\n';
        return EXIT_FAILURE;
    }
}
