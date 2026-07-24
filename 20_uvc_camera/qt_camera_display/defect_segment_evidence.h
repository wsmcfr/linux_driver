#ifndef DEFECT_SEGMENT_EVIDENCE_H
#define DEFECT_SEGMENT_EVIDENCE_H

/*
 * defect_segment_evidence.h
 *
 * 作用：
 *   提供不依赖 ONNX Runtime 和 Qt 的 UNet 缺陷 mask 连通域统计与三级证据判定。
 *   defect-segment 板端程序和 Windows 主机回归测试共用本实现，避免测试复制生产算法。
 */

#include <algorithm> /* std::max 用于维护最大连通域面积。 */
#include <cstddef>   /* std::size_t 用于安全表示 mask 下标和元素数量。 */
#include <limits>    /* std::numeric_limits 用于检查 width*height 是否可能溢出。 */
#include <stdexcept> /* std::invalid_argument 用于拒绝非法尺寸和阈值组合。 */
#include <vector>    /* std::vector 保存 mask、访问标记和待遍历像素栈。 */

namespace defect_segment_evidence {

/* 默认小连通域过滤阈值：小于 20 像素的孤立区域按反光或量化噪点处理。 */
static constexpr int kDefaultMinComponentPixels = 20;

/* 默认待复核阈值：过滤后缺陷总面积达到 80 像素时进入 WEAK。 */
static constexpr int kDefaultReviewPixels = 80;

/* 默认明确坏品阈值：过滤后缺陷总面积达到 300 像素时满足 STRONG 面积条件。 */
static constexpr int kDefaultBadPixels = 300;

/* 默认强连通域阈值：最大连续缺陷达到 120 像素时满足 STRONG 连续性条件。 */
static constexpr int kDefaultStrongComponentPixels = 120;

/* SegmentEvidenceLevel 表示 UNet 缺陷 mask 对最终综合判定提供的证据强度。 */
enum class SegmentEvidenceLevel {
    Clear,  /* CLEAR 表示过滤小噪点后缺陷面积不足复核线，可以参与良品判定。 */
    Weak,   /* WEAK 表示存在可疑区域但证据不足以直接判坏，最终进入待复核。 */
    Strong  /* STRONG 表示总面积和最大连续区域同时过线，可以直接判坏。 */
};

/*
 * SegmentEvidenceSettings 的作用：
 *   保存一次 mask 分析使用的四个板端可调像素阈值。
 *
 * 字段说明：
 *   minComponentPixels 是单个连通域保留所需的最小面积。
 *   reviewPixels 是过滤后总面积进入 WEAK 的下限。
 *   badPixels 是过滤后总面积进入 STRONG 的下限。
 *   strongComponentPixels 是最大连通域进入 STRONG 的下限。
 */
struct SegmentEvidenceSettings {
    int minComponentPixels;
    int reviewPixels;
    int badPixels;
    int strongComponentPixels;
};

/*
 * SegmentEvidenceStats 的作用：
 *   保存连通域遍历得到的诊断统计和最终证据等级。
 *
 * 字段说明：
 *   rawDefectPixels 是原始 mask 中所有非 0 像素数量。
 *   filteredDefectPixels 是删除小连通域后保留的缺陷像素数量。
 *   largestComponentPixels 是原始 mask 中最大连通域面积。
 *   componentCount 是原始非背景连通域总数。
 *   retainedComponentCount 是达到 minComponentPixels 的连通域数量。
 *   level 是 CLEAR、WEAK 或 STRONG 最终证据等级。
 */
struct SegmentEvidenceStats {
    int rawDefectPixels = 0;
    int filteredDefectPixels = 0;
    int largestComponentPixels = 0;
    int componentCount = 0;
    int retainedComponentCount = 0;
    SegmentEvidenceLevel level = SegmentEvidenceLevel::Clear;
};

/*
 * validate_segment_evidence_settings 的作用：
 *   校验四个阈值是否满足板端配置约束，防止无效参数产生不可解释的判定区间。
 *
 * 参数：
 *   settings 是待校验的阈值集合。
 *
 * 返回值：
 *   无返回值；参数非法时抛出 std::invalid_argument。
 */
inline void validate_segment_evidence_settings(const SegmentEvidenceSettings &settings)
{
    if (settings.minComponentPixels < 1) {
        throw std::invalid_argument("UNet 小连通域阈值必须大于等于 1");
    }
    if (settings.reviewPixels < settings.minComponentPixels) {
        throw std::invalid_argument("UNet 复核像素阈值不能小于小连通域阈值");
    }
    if (settings.badPixels < settings.reviewPixels) {
        throw std::invalid_argument("UNet 坏品像素阈值不能小于复核像素阈值");
    }
    if (settings.strongComponentPixels < settings.minComponentPixels
        || settings.strongComponentPixels > settings.badPixels) {
        throw std::invalid_argument("UNet 强连通域阈值必须位于小连通域阈值和坏品像素阈值之间");
    }
}

/*
 * segment_evidence_level_from_stats 的作用：
 *   根据过滤后缺陷总面积和最大连通域面积计算 CLEAR/WEAK/STRONG。
 *   defect-segment 生产判定和 Qt RESULT_SEG 二次校验共用本函数，避免两边公式漂移。
 *
 * 参数：
 *   filteredDefectPixels 是小连通域过滤后的缺陷总像素数。
 *   largestComponentPixels 是原始 mask 中最大 8 邻域连通域面积。
 *   settings 是本轮四个板端阈值快照。
 *
 * 返回值：
 *   返回对应证据等级；统计值或阈值非法时抛出 std::invalid_argument。
 */
inline SegmentEvidenceLevel segment_evidence_level_from_stats(
    int filteredDefectPixels,
    int largestComponentPixels,
    const SegmentEvidenceSettings &settings)
{
    validate_segment_evidence_settings(settings);
    if (filteredDefectPixels < 0 || largestComponentPixels < 0) {
        throw std::invalid_argument("UNet 连通域统计值不能为负数");
    }

    if (filteredDefectPixels < settings.reviewPixels) {
        return SegmentEvidenceLevel::Clear;
    }
    if (filteredDefectPixels >= settings.badPixels
            && largestComponentPixels >= settings.strongComponentPixels) {
        return SegmentEvidenceLevel::Strong;
    }
    return SegmentEvidenceLevel::Weak;
}

/*
 * segment_evidence_level_name 的作用：
 *   把证据枚举转换成 RESULT_SEG 使用的稳定 ASCII 字符串。
 *
 * 参数：
 *   level 是待转换的证据等级。
 *
 * 返回值：
 *   返回 CLEAR、WEAK 或 STRONG；枚举值异常时返回 UNKNOWN。
 */
inline const char *segment_evidence_level_name(SegmentEvidenceLevel level)
{
    switch (level) {
    case SegmentEvidenceLevel::Clear:
        return "CLEAR";
    case SegmentEvidenceLevel::Weak:
        return "WEAK";
    case SegmentEvidenceLevel::Strong:
        return "STRONG";
    }

    return "UNKNOWN";
}

/*
 * analyze_segment_evidence 的作用：
 *   对单通道类别 mask 执行 8 邻域连通域遍历，过滤小区域并生成三级证据。
 *
 * 主要流程：
 *   1. 校验 mask 尺寸和阈值顺序。
 *   2. 把所有非 0 类别视为缺陷，使用显式像素栈遍历每个 8 邻域连通域。
 *   3. 统计原始像素、过滤后像素、最大区域和连通域数量。
 *   4. 按复核线、坏品线和强连通域线输出 CLEAR、WEAK 或 STRONG。
 *
 * 参数：
 *   mask 是逐像素类别 mask，0 为背景，非 0 为缺陷类别。
 *   width/height 是 mask 的像素宽高。
 *   settings 是本轮板端阈值快照。
 *
 * 返回值：
 *   返回完整统计；尺寸或阈值非法时抛出 std::invalid_argument。
 */
inline SegmentEvidenceStats analyze_segment_evidence(const std::vector<unsigned char> &mask,
                                                      int width,
                                                      int height,
                                                      const SegmentEvidenceSettings &settings)
{
    validate_segment_evidence_settings(settings);

    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("UNet mask 宽高必须大于 0");
    }
    if (static_cast<std::size_t>(width) > std::numeric_limits<std::size_t>::max()
            / static_cast<std::size_t>(height)) {
        throw std::invalid_argument("UNet mask 宽高乘积溢出");
    }

    const std::size_t expectedSize = static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height); /* expectedSize 是 width*height 应有元素数量。 */
    if (mask.size() != expectedSize) {
        throw std::invalid_argument("UNet mask 元素数量与宽高不一致");
    }

    SegmentEvidenceStats stats;                         /* stats 累计本次完整诊断结果。 */
    std::vector<unsigned char> visited(expectedSize, 0U); /* visited 标记像素是否已归入某个连通域。 */
    std::vector<std::size_t> pending;                    /* pending 作为显式栈保存当前连通域待访问像素。 */
    pending.reserve(expectedSize);

    for (std::size_t start = 0U; start < expectedSize; ++start) {
        if (mask[start] == 0U || visited[start] != 0U) {
            continue;
        }

        int componentPixels = 0; /* componentPixels 统计当前 8 邻域区域面积。 */
        stats.componentCount++;
        pending.clear();
        pending.push_back(start);
        visited[start] = 1U;

        while (!pending.empty()) {
            const std::size_t current = pending.back(); /* current 是本轮弹出的像素一维下标。 */
            pending.pop_back();
            componentPixels++;

            const int currentX = static_cast<int>(current % static_cast<std::size_t>(width));
            const int currentY = static_cast<int>(current / static_cast<std::size_t>(width));

            for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    if (offsetX == 0 && offsetY == 0) {
                        continue;
                    }

                    const int nextX = currentX + offsetX; /* nextX/nextY 是候选 8 邻域坐标。 */
                    const int nextY = currentY + offsetY;
                    if (nextX < 0 || nextX >= width || nextY < 0 || nextY >= height) {
                        continue;
                    }

                    const std::size_t next = static_cast<std::size_t>(nextY * width + nextX);
                    if (mask[next] == 0U || visited[next] != 0U) {
                        continue;
                    }

                    visited[next] = 1U;
                    pending.push_back(next);
                }
            }
        }

        stats.rawDefectPixels += componentPixels;
        stats.largestComponentPixels = std::max(stats.largestComponentPixels, componentPixels);
        if (componentPixels >= settings.minComponentPixels) {
            stats.filteredDefectPixels += componentPixels;
            stats.retainedComponentCount++;
        }
    }

    stats.level = segment_evidence_level_from_stats(stats.filteredDefectPixels,
                                                    stats.largestComponentPixels,
                                                    settings);

    return stats;
}

} /* namespace defect_segment_evidence */

#endif /* DEFECT_SEGMENT_EVIDENCE_H */
