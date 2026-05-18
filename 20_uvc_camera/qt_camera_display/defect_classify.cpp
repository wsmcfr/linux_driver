/*
 * defect_classify.cpp
 *
 * 作用：
 *   STM32MP157 板端 MobileNetV3-Small 缺陷分类命令行推理程序。
 *   Qt 首页的“检测”按钮会先让 KMS overlay 保存当前帧 JPG，再调用本程序对该图片做一次推理。
 *
 * 主要流程：
 *   1. 解析模型、标签和图片路径参数。
 *   2. 用 libjpeg 读取 JPG 图片，中心裁剪 300x300 ROI，并缩放到 224x224。
 *   3. 按训练脚本一致的 RGB、ImageNet mean/std 和 NCHW float32 规则生成输入张量。
 *   4. 使用 ONNX Runtime CPUExecutionProvider 执行 INT8 ONNX 图。
 *   5. 对输出 logits 做 softmax，按 bad 类别总概率阈值给出 GOOD/BAD 结果。
 *
 * 返回值：
 *   0 表示推理成功；非 0 表示参数、图片、模型或运行时错误。
 */

#include <onnxruntime_cxx_api.h> /* ONNX Runtime C++ API 提供 Session、Tensor 和 CPU 推理能力。 */

#include <jpeglib.h>             /* libjpeg 用于在板端读取 overlay 保存出来的 JPG 当前帧。 */
#include <setjmp.h>              /* setjmp/longjmp 用于把 libjpeg 的错误回调转换成 C++ 可控失败。 */

#include <algorithm>             /* std::max_element/std::min 用于 softmax 和 ROI 边界计算。 */
#include <array>                 /* std::array 用于声明 ONNX Runtime 输入张量形状。 */
#include <chrono>                /* std::chrono 用于统计单次模型推理耗时。 */
#include <cmath>                 /* std::exp 用于 softmax 概率计算。 */
#include <cstdint>               /* uint8_t 保存 RGB 像素。 */
#include <cstdio>                /* FILE/fopen/fclose 用于读取 JPG 文件。 */
#include <cstdlib>               /* EXIT_SUCCESS/EXIT_FAILURE 表示命令行程序返回语义。 */
#include <cstring>               /* std::strlen/std::strcmp 用于参数解析和 libjpeg 错误处理。 */
#include <fstream>               /* std::ifstream 用于读取 labels JSON 文本。 */
#include <iostream>              /* std::cout/std::cerr 输出 Qt 可解析的一行检测结果。 */
#include <limits>                /* std::numeric_limits 用于初始化概率比较值。 */
#include <memory>                /* std::unique_ptr 管理 JPG 解码缓冲。 */
#include <sstream>               /* std::ostringstream 组装错误信息。 */
#include <stdexcept>             /* std::runtime_error 抛出明确失败原因。 */
#include <string>                /* std::string 保存路径、标签和输出文本。 */
#include <vector>                /* std::vector 保存图片、输入张量、输出概率和标签列表。 */

/* 默认模型路径：deploy_qt_camera_display.sh 会把 INT8 ONNX 复制到这个位置。 */
static const char *DEFAULT_MODEL_PATH =
    "/root/qt_camera_display/models/defect_classifier_static_mixed_int8.onnx";

/* 默认标签路径：必须和 ONNX 同前缀，保证 6 类输出顺序不靠猜。 */
static const char *DEFAULT_LABELS_PATH =
    "/root/qt_camera_display/models/defect_classifier_static_mixed_int8_labels.json";

/* 当前模型训练数据实际使用 300x300 中心 ROI，板端测试必须与训练输入保持一致。 */
static const int DEFAULT_ROI_SIZE = 300;

/* 模型输入宽度：export_classify_onnx.py 固定导出 224x224。 */
static const int MODEL_INPUT_WIDTH = 224;

/* 模型输入高度：export_classify_onnx.py 固定导出 224x224。 */
static const int MODEL_INPUT_HEIGHT = 224;

/* 当前模型类别数：gasket/splitwasher/washer 各 good/bad，共 6 类。 */
static const int MODEL_CLASS_COUNT = 6;

/* bad 总概率超过该阈值时判为 BAD，和 PC 端 infer_classify.py 的 BAD_THRESHOLD 保持一致。 */
static const float BAD_THRESHOLD = 0.5f;

/* ImageNet RGB 均值：训练、导出、PC 推理和板端推理必须一致。 */
static const float IMAGENET_MEAN[3] = {0.485f, 0.456f, 0.406f};

/* ImageNet RGB 标准差：训练、导出、PC 推理和板端推理必须一致。 */
static const float IMAGENET_STD[3] = {0.229f, 0.224f, 0.225f};

/*
 * ImageBuffer 的作用：
 *   保存 libjpeg 解码后的 RGB888 图片。
 *
 * 字段说明：
 *   width/height 是图片尺寸。
 *   rgb 是按 HWC 排列的 RGB888 像素，长度为 width * height * 3。
 */
struct ImageBuffer {
    int width;
    int height;
    std::vector<uint8_t> rgb;
};

/*
 * ProgramOptions 的作用：
 *   保存命令行参数解析结果。
 *
 * 字段说明：
 *   model_path 是 ONNX 模型路径。
 *   labels_path 是 labels JSON 路径。
 *   image_path 是待检测 JPG 图片路径。
 *   roi_size 是中心裁剪边长，0 表示不裁剪整图缩放。
 */
struct ProgramOptions {
    std::string model_path = DEFAULT_MODEL_PATH;
    std::string labels_path = DEFAULT_LABELS_PATH;
    std::string image_path;
    int roi_size = DEFAULT_ROI_SIZE;
};

/*
 * JpegErrorManager 的作用：
 *   扩展 libjpeg 错误管理器，把错误信息保存到缓冲区并跳回调用点。
 */
struct JpegErrorManager {
    jpeg_error_mgr pub;       /* pub 是 libjpeg 要求的标准错误管理器字段。 */
    jmp_buf jump_buffer;      /* jump_buffer 保存错误发生时要跳回的位置。 */
    char message[JMSG_LENGTH_MAX]; /* message 保存 libjpeg 格式化后的错误文本。 */
};

/*
 * print_usage 的作用：
 *   打印 defect-classify 命令行用法，便于 SSH 和 Qt 日志排查参数问题。
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
        << "用法: " << program << " --image <jpg> [--model <onnx>] [--labels <json>] [--roi 300]\n"
        << "输出: RESULT status=GOOD|BAD class=<label> confidence=<0-1> bad_total=<0-1> good_total=<0-1> time_ms=<ms>\n";
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
    char *end = nullptr;
    long value = std::strtol(text, &end, 10);

    if (text == nullptr || *text == '\0' || end == text || *end != '\0') {
        throw std::runtime_error(std::string("参数不是整数: ") + name);
    }

    if (value < 0 || value > 4096) {
        throw std::runtime_error(std::string("参数超出范围: ") + name);
    }

    return static_cast<int>(value);
}

/*
 * parse_args 的作用：
 *   解析命令行参数并返回 ProgramOptions。
 *
 * 主要流程：
 *   1. 支持 --image、--model、--labels、--roi 和 --help。
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
    ProgramOptions options;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

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

        if (std::strcmp(arg, "--labels") == 0) {
            if (++i >= argc) {
                throw std::runtime_error("--labels 缺少标签路径");
            }
            options.labels_path = argv[i];
            continue;
        }

        if (std::strcmp(arg, "--roi") == 0) {
            if (++i >= argc) {
                throw std::runtime_error("--roi 缺少数值");
            }
            options.roi_size = parse_int(argv[i], "--roi");
            continue;
        }

        throw std::runtime_error(std::string("未知参数: ") + arg);
    }

    if (options.image_path.empty()) {
        throw std::runtime_error("必须指定 --image <jpg>");
    }

    return options;
}

/*
 * jpeg_error_exit 的作用：
 *   libjpeg 发生致命错误时保存错误文本并跳回 read_jpeg_rgb()。
 *
 * 参数：
 *   cinfo 是 libjpeg 解码对象。
 *
 * 返回值：
 *   不直接返回，通过 longjmp 回到 setjmp 位置。
 */
static void jpeg_error_exit(j_common_ptr cinfo)
{
    JpegErrorManager *err = reinterpret_cast<JpegErrorManager *>(cinfo->err);

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
    FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        throw std::runtime_error("无法打开图片: " + path);
    }

    jpeg_decompress_struct cinfo;
    JpegErrorManager jerr;
    ImageBuffer image;

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
            + static_cast<size_t>(cinfo.output_scanline) * image.width * 3U;
        JSAMPROW row_pointer[1] = {row};

        jpeg_read_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    std::fclose(file);

    return image;
}

/*
 * pixel_at_clamped 的作用：
 *   读取图片中指定坐标的 RGB 像素，调用者保证坐标在有效范围内。
 *
 * 参数：
 *   image 是 RGB 图片。
 *   x/y 是像素坐标。
 *   channel 是 RGB 通道索引，0=R、1=G、2=B。
 *
 * 返回值：
 *   返回 0~255 的像素值。
 */
static uint8_t pixel_at(const ImageBuffer &image, int x, int y, int channel)
{
    const size_t index = (static_cast<size_t>(y) * image.width + x) * 3U
        + static_cast<size_t>(channel);
    return image.rgb[index];
}

/*
 * preprocess_image 的作用：
 *   把 RGB 图片转换成模型输入 tensor。
 *
 * 主要流程：
 *   1. 根据 roi_size 计算中心 ROI；roi_size=0 时使用完整图片。
 *   2. 使用双线性插值把 ROI resize 到 224x224。
 *   3. 按 RGB 通道分别执行 ImageNet 标准化。
 *   4. 输出 NCHW float32，形状为 [1,3,224,224]。
 *
 * 参数：
 *   image 是 libjpeg 解码出来的 RGB 图片。
 *   roi_size 是中心 ROI 边长。
 *
 * 返回值：
 *   返回长度 1*3*224*224 的 float 向量。
 */
static std::vector<float> preprocess_image(const ImageBuffer &image, int roi_size)
{
    if (image.width <= 0 || image.height <= 0 || image.rgb.empty()) {
        throw std::runtime_error("输入图片为空");
    }

    int crop_w = image.width;
    int crop_h = image.height;
    int crop_x = 0;
    int crop_y = 0;

    if (roi_size > 0) {
        const int size = std::min(roi_size, std::min(image.width, image.height));
        crop_w = size;
        crop_h = size;
        crop_x = (image.width - size) / 2;
        crop_y = (image.height - size) / 2;
    }

    std::vector<float> tensor(static_cast<size_t>(3 * MODEL_INPUT_WIDTH * MODEL_INPUT_HEIGHT), 0.0f);

    for (int oy = 0; oy < MODEL_INPUT_HEIGHT; oy++) {
        const float src_y = crop_y + (static_cast<float>(oy) + 0.5f) * crop_h / MODEL_INPUT_HEIGHT - 0.5f;
        const int y0 = std::max(0, std::min(image.height - 1, static_cast<int>(std::floor(src_y))));
        const int y1 = std::max(0, std::min(image.height - 1, y0 + 1));
        const float wy = src_y - y0;

        for (int ox = 0; ox < MODEL_INPUT_WIDTH; ox++) {
            const float src_x = crop_x + (static_cast<float>(ox) + 0.5f) * crop_w / MODEL_INPUT_WIDTH - 0.5f;
            const int x0 = std::max(0, std::min(image.width - 1, static_cast<int>(std::floor(src_x))));
            const int x1 = std::max(0, std::min(image.width - 1, x0 + 1));
            const float wx = src_x - x0;

            for (int channel = 0; channel < 3; channel++) {
                const float p00 = static_cast<float>(pixel_at(image, x0, y0, channel));
                const float p01 = static_cast<float>(pixel_at(image, x1, y0, channel));
                const float p10 = static_cast<float>(pixel_at(image, x0, y1, channel));
                const float p11 = static_cast<float>(pixel_at(image, x1, y1, channel));
                const float top = p00 + (p01 - p00) * wx;
                const float bottom = p10 + (p11 - p10) * wx;
                const float value01 = (top + (bottom - top) * wy) / 255.0f;
                const float normalized = (value01 - IMAGENET_MEAN[channel]) / IMAGENET_STD[channel];
                const size_t tensor_index =
                    static_cast<size_t>(channel) * MODEL_INPUT_WIDTH * MODEL_INPUT_HEIGHT
                    + static_cast<size_t>(oy) * MODEL_INPUT_WIDTH
                    + static_cast<size_t>(ox);

                tensor[tensor_index] = normalized;
            }
        }
    }

    return tensor;
}

/*
 * extract_json_object_value 的作用：
 *   从简单 JSON 文本中提取指定对象字段的原始内容。
 *
 * 参数：
 *   json 是完整 JSON 文本。
 *   key 是字段名，例如 idx_to_class。
 *
 * 返回值：
 *   返回对象花括号内文本，不包含最外层花括号。
 */
static std::string extract_json_object_value(const std::string &json, const std::string &key)
{
    const std::string marker = "\"" + key + "\"";
    const size_t key_pos = json.find(marker);
    if (key_pos == std::string::npos) {
        return std::string();
    }

    const size_t open_pos = json.find('{', key_pos + marker.size());
    if (open_pos == std::string::npos) {
        return std::string();
    }

    int depth = 0;
    for (size_t pos = open_pos; pos < json.size(); pos++) {
        if (json[pos] == '{') {
            depth++;
        } else if (json[pos] == '}') {
            depth--;
            if (depth == 0) {
                return json.substr(open_pos + 1, pos - open_pos - 1);
            }
        }
    }

    return std::string();
}

/*
 * trim_json_string 的作用：
 *   去掉 JSON 字符串外层引号和首尾空白，当前标签文件只含 ASCII 类别名，不需要完整 JSON 反转义器。
 *
 * 参数：
 *   text 是 JSON key 或 value 文本片段。
 *
 * 返回值：
 *   返回普通字符串。
 */
static std::string trim_json_string(std::string text)
{
    const char *spaces = " \t\r\n";
    const size_t begin = text.find_first_not_of(spaces);
    const size_t end = text.find_last_not_of(spaces);

    if (begin == std::string::npos || end == std::string::npos) {
        return std::string();
    }

    text = text.substr(begin, end - begin + 1);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
    }

    return text;
}

/*
 * load_labels 的作用：
 *   读取 export/quantize 脚本生成的 idx_to_class 类别顺序。
 *
 * 参数：
 *   labels_path 是 *_labels.json 文件路径。
 *
 * 返回值：
 *   返回长度为 6 的类别名数组，索引必须对应 ONNX 输出。
 */
static std::vector<std::string> load_labels(const std::string &labels_path)
{
    std::ifstream input(labels_path.c_str());
    if (!input) {
        throw std::runtime_error("无法打开标签文件: " + labels_path);
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string json = buffer.str();
    const std::string object = extract_json_object_value(json, "idx_to_class");

    if (object.empty()) {
        throw std::runtime_error("标签文件缺少 idx_to_class: " + labels_path);
    }

    std::vector<std::string> labels(MODEL_CLASS_COUNT);
    size_t pos = 0;

    while (pos < object.size()) {
        const size_t key_start = object.find('"', pos);
        if (key_start == std::string::npos) {
            break;
        }
        const size_t key_end = object.find('"', key_start + 1);
        const size_t colon = object.find(':', key_end + 1);
        const size_t value_start = object.find('"', colon + 1);
        const size_t value_end = object.find('"', value_start + 1);

        if (key_end == std::string::npos || colon == std::string::npos
                || value_start == std::string::npos || value_end == std::string::npos) {
            break;
        }

        const int index = parse_int(object.substr(key_start + 1, key_end - key_start - 1).c_str(), "label index");
        const std::string value = trim_json_string(object.substr(value_start, value_end - value_start + 1));

        if (index >= 0 && index < MODEL_CLASS_COUNT) {
            labels[static_cast<size_t>(index)] = value;
        }

        pos = value_end + 1;
    }

    for (int i = 0; i < MODEL_CLASS_COUNT; i++) {
        if (labels[static_cast<size_t>(i)].empty()) {
            throw std::runtime_error("标签文件类别数量或索引不完整: " + labels_path);
        }
    }

    return labels;
}

/*
 * class_name_is_group 的作用：
 *   判断类别名是否包含 good 或 bad token。
 *
 * 参数：
 *   name 是类别名，例如 washer_bad。
 *   group 是 "good" 或 "bad"。
 *
 * 返回值：
 *   true 表示类别属于该 good/bad 分组。
 */
static bool class_name_is_group(const std::string &name, const std::string &group)
{
    size_t start = 0;

    while (start <= name.size()) {
        size_t end = name.find('_', start);
        if (end == std::string::npos) {
            end = name.size();
        }

        if (name.substr(start, end - start) == group) {
            return true;
        }

        if (end == name.size()) {
            break;
        }
        start = end + 1;
    }

    return false;
}

/*
 * softmax 的作用：
 *   把 ONNX 输出 logits 转换成概率。
 *
 * 参数：
 *   logits 是模型输出的一维数组。
 *
 * 返回值：
 *   返回和 logits 等长的概率数组，和为 1。
 */
static std::vector<float> softmax(const float *logits, size_t count)
{
    std::vector<float> probs(count);
    const float max_logit = *std::max_element(logits, logits + count);
    float sum = 0.0f;

    for (size_t i = 0; i < count; i++) {
        probs[i] = std::exp(logits[i] - max_logit);
        sum += probs[i];
    }

    for (float &prob : probs) {
        prob /= sum;
    }

    return probs;
}

/*
 * main 的作用：
 *   程序入口，执行一次图片分类并输出 Qt 可解析的一行 RESULT。
 */
int main(int argc, char **argv)
{
    try {
        const ProgramOptions options = parse_args(argc, argv);
        const std::vector<std::string> labels = load_labels(options.labels_path);
        const ImageBuffer image = read_jpeg_rgb(options.image_path);
        std::vector<float> input_tensor = preprocess_image(image, options.roi_size);

        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "defect-classify");
        Ort::SessionOptions session_options;
        session_options.SetIntraOpNumThreads(2);
        session_options.SetInterOpNumThreads(1);
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

        Ort::Session session(env, options.model_path.c_str(), session_options);
        Ort::AllocatorWithDefaultOptions allocator;
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

        float *logits = outputs.front().GetTensorMutableData<float>();
        std::vector<float> probs = softmax(logits, MODEL_CLASS_COUNT);

        float bad_total = 0.0f;
        float good_total = 0.0f;
        int best_bad_index = -1;
        float best_bad_prob = -std::numeric_limits<float>::infinity();
        int argmax_index = 0;

        for (int i = 0; i < MODEL_CLASS_COUNT; i++) {
            const std::string &name = labels[static_cast<size_t>(i)];
            const float prob = probs[static_cast<size_t>(i)];

            if (class_name_is_group(name, "bad")) {
                bad_total += prob;
                if (prob > best_bad_prob) {
                    best_bad_prob = prob;
                    best_bad_index = i;
                }
            }

            if (class_name_is_group(name, "good")) {
                good_total += prob;
            }

            if (prob > probs[static_cast<size_t>(argmax_index)]) {
                argmax_index = i;
            }
        }

        int pred_index = argmax_index;
        if (best_bad_index >= 0 && bad_total >= BAD_THRESHOLD) {
            pred_index = best_bad_index;
        }

        const bool is_bad = class_name_is_group(labels[static_cast<size_t>(pred_index)], "bad");
        const float confidence = probs[static_cast<size_t>(pred_index)];
        const double time_ms = std::chrono::duration<double, std::milli>(infer_end - infer_start).count();

        std::cout.setf(std::ios::fixed);
        std::cout.precision(4);
        std::cout
            << "RESULT"
            << " status=" << (is_bad ? "BAD" : "GOOD")
            << " class=" << labels[static_cast<size_t>(pred_index)]
            << " confidence=" << confidence
            << " bad_total=" << bad_total
            << " good_total=" << good_total
            << " time_ms=" << time_ms
            << " image=" << options.image_path
            << '\n';

        return EXIT_SUCCESS;
    } catch (const std::exception &ex) {
        std::cerr << "ERROR " << ex.what() << '\n';
        return EXIT_FAILURE;
    }
}
