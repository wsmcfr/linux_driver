/*
 * test_defect_segment_evidence.cpp
 *
 * 作用：
 *   在 Windows 主机上验证 UNet 缺陷 mask 的连通域过滤和三级证据判定。
 *   测试只依赖标准 C++14，不加载 ONNX Runtime，便于修改板端阈值算法后快速回归。
 *
 * 主要流程：
 *   1. 构造孤立噪点、小型连续缺陷、大型连续缺陷和斜向缺陷 mask。
 *   2. 调用生产代码 analyze_segment_evidence() 计算连通域统计。
 *   3. 检查 CLEAR、WEAK、STRONG 三种等级和非法尺寸错误处理。
 *
 * 返回值：
 *   所有断言通过时返回 0；任一断言失败时由 assert 终止并返回非 0。
 */

#include "defect_segment_evidence.h" /* 引入生产环境使用的连通域证据算法，确保测试不复制实现。 */

#include <cassert>   /* assert 用于验证每个 mask 的统计结果和证据等级。 */
#include <iostream>  /* std::cout 用于输出可被验证脚本识别的 PASS 标记。 */
#include <stdexcept> /* std::invalid_argument 用于验证非法 mask 尺寸被明确拒绝。 */
#include <vector>    /* std::vector 保存测试构造的单通道类别 mask。 */

using defect_segment_evidence::SegmentEvidenceLevel;
using defect_segment_evidence::SegmentEvidenceSettings;
using defect_segment_evidence::SegmentEvidenceStats;
using defect_segment_evidence::analyze_segment_evidence;

/*
 * make_mask 的作用：
 *   创建指定宽高且默认全部为背景 0 的单通道 mask。
 *
 * 参数：
 *   width/height 是测试 mask 的像素宽高。
 *
 * 返回值：
 *   返回长度为 width*height 的全 0 mask。
 */
static std::vector<unsigned char> make_mask(int width, int height)
{
    return std::vector<unsigned char>(static_cast<std::size_t>(width * height), 0U);
}

/*
 * fill_rectangle 的作用：
 *   在测试 mask 中写入一个值为 1 的连续矩形缺陷区域。
 *
 * 参数：
 *   mask 是待修改的单通道 mask。
 *   width 是 mask 每行像素数。
 *   x/y 是矩形左上角坐标，rectWidth/rectHeight 是矩形尺寸。
 *
 * 返回值：
 *   无返回值；函数直接修改 mask。
 */
static void fill_rectangle(std::vector<unsigned char> &mask,
                           int width,
                           int x,
                           int y,
                           int rectWidth,
                           int rectHeight)
{
    for (int row = y; row < y + rectHeight; ++row) {
        for (int column = x; column < x + rectWidth; ++column) {
            mask[static_cast<std::size_t>(row * width + column)] = 1U;
        }
    }
}

/*
 * test_isolated_noise_is_clear 的作用：
 *   验证多个小于 20 像素的孤立反光点会被过滤，不会进入复核或坏品等级。
 */
static void test_isolated_noise_is_clear()
{
    const int width = 16;  /* width 保存测试 mask 宽度。 */
    const int height = 16; /* height 保存测试 mask 高度。 */
    std::vector<unsigned char> mask = make_mask(width, height); /* mask 保存四个互不相连的噪点。 */
    const SegmentEvidenceSettings settings{20, 80, 300, 120};   /* settings 使用已确认的板端首轮默认阈值。 */

    mask[1U] = 1U;
    mask[40U] = 1U;
    mask[130U] = 1U;
    mask[250U] = 1U;

    const SegmentEvidenceStats stats = analyze_segment_evidence(mask, width, height, settings);

    assert(stats.rawDefectPixels == 4);
    assert(stats.filteredDefectPixels == 0);
    assert(stats.componentCount == 4);
    assert(stats.retainedComponentCount == 0);
    assert(stats.level == SegmentEvidenceLevel::Clear);
}

/*
 * test_component_filter_boundary_is_inclusive 的作用：
 *   验证 19px 连通域会被过滤，而恰好 20px 的连通域会被保留。
 */
static void test_component_filter_boundary_is_inclusive()
{
    const int width = 24;
    const int height = 4;
    const SegmentEvidenceSettings settings{20, 80, 300, 120};
    std::vector<unsigned char> belowMask = make_mask(width, height); /* belowMask 保存 19px 连续区域。 */
    std::vector<unsigned char> exactMask = make_mask(width, height); /* exactMask 保存 20px 连续区域。 */

    fill_rectangle(belowMask, width, 1, 1, 19, 1);
    fill_rectangle(exactMask, width, 1, 1, 20, 1);

    const SegmentEvidenceStats belowStats = analyze_segment_evidence(belowMask, width, height, settings);
    const SegmentEvidenceStats exactStats = analyze_segment_evidence(exactMask, width, height, settings);

    assert(belowStats.filteredDefectPixels == 0);
    assert(belowStats.retainedComponentCount == 0);
    assert(exactStats.filteredDefectPixels == 20);
    assert(exactStats.retainedComponentCount == 1);
}

/*
 * test_review_boundary_is_inclusive 的作用：
 *   验证过滤后 79px 仍为 CLEAR，而恰好 80px 已进入 WEAK 待复核。
 */
static void test_review_boundary_is_inclusive()
{
    const int width = 20;
    const int height = 12;
    const SegmentEvidenceSettings settings{20, 80, 300, 120};
    std::vector<unsigned char> belowMask = make_mask(width, height); /* belowMask 从 8x10 区域删去一个角点得到 79px。 */
    std::vector<unsigned char> exactMask = make_mask(width, height); /* exactMask 保存完整 8x10 区域。 */

    fill_rectangle(belowMask, width, 1, 1, 8, 10);
    belowMask[static_cast<std::size_t>(10 * width + 8)] = 0U;
    fill_rectangle(exactMask, width, 1, 1, 8, 10);

    const SegmentEvidenceStats belowStats = analyze_segment_evidence(belowMask, width, height, settings);
    const SegmentEvidenceStats exactStats = analyze_segment_evidence(exactMask, width, height, settings);

    assert(belowStats.filteredDefectPixels == 79);
    assert(belowStats.level == SegmentEvidenceLevel::Clear);
    assert(exactStats.filteredDefectPixels == 80);
    assert(exactStats.level == SegmentEvidenceLevel::Weak);
}

/*
 * test_medium_component_is_weak 的作用：
 *   验证通过噪点过滤但尚未达到强缺陷面积的连续区域会进入 WEAK 待复核等级。
 */
static void test_medium_component_is_weak()
{
    const int width = 20;
    const int height = 20;
    std::vector<unsigned char> mask = make_mask(width, height); /* mask 保存一个 10x10 的连续缺陷区域。 */
    const SegmentEvidenceSettings settings{20, 80, 300, 120};

    fill_rectangle(mask, width, 2, 2, 10, 10);
    const SegmentEvidenceStats stats = analyze_segment_evidence(mask, width, height, settings);

    assert(stats.rawDefectPixels == 100);
    assert(stats.filteredDefectPixels == 100);
    assert(stats.largestComponentPixels == 100);
    assert(stats.retainedComponentCount == 1);
    assert(stats.level == SegmentEvidenceLevel::Weak);
}

/*
 * test_large_component_is_strong 的作用：
 *   验证总面积和最大连通域同时过线时输出 STRONG 明确缺陷等级。
 */
static void test_large_component_is_strong()
{
    const int width = 24;
    const int height = 24;
    std::vector<unsigned char> mask = make_mask(width, height); /* mask 保存一个 20x20 的大连续缺陷区域。 */
    const SegmentEvidenceSettings settings{20, 80, 300, 120};

    fill_rectangle(mask, width, 2, 2, 20, 20);
    const SegmentEvidenceStats stats = analyze_segment_evidence(mask, width, height, settings);

    assert(stats.filteredDefectPixels == 400);
    assert(stats.largestComponentPixels == 400);
    assert(stats.level == SegmentEvidenceLevel::Strong);
}

/*
 * test_fragmented_large_area_stays_weak 的作用：
 *   验证过滤后总面积超过坏品线、但没有单个连续区域达到强连通域线时仍输出 WEAK。
 *   该用例锁定平衡策略的核心边界，避免大量分散反光点仅凭总面积把良品直接判坏。
 */
static void test_fragmented_large_area_stays_weak()
{
    const int width = 40;
    const int height = 40;
    std::vector<unsigned char> mask = make_mask(width, height); /* mask 保存四个互不接触的 9x9 区域，总面积 324。 */
    const SegmentEvidenceSettings settings{20, 80, 300, 120};

    fill_rectangle(mask, width, 2, 2, 9, 9);
    fill_rectangle(mask, width, 20, 2, 9, 9);
    fill_rectangle(mask, width, 2, 20, 9, 9);
    fill_rectangle(mask, width, 20, 20, 9, 9);

    const SegmentEvidenceStats stats = analyze_segment_evidence(mask, width, height, settings);

    assert(stats.filteredDefectPixels == 324);
    assert(stats.largestComponentPixels == 81);
    assert(stats.componentCount == 4);
    assert(stats.retainedComponentCount == 4);
    assert(stats.level == SegmentEvidenceLevel::Weak);
}

/*
 * test_exact_strong_thresholds_are_inclusive 的作用：
 *   验证过滤后总面积恰好 300px、最大连通域恰好 120px 时已经达到 STRONG。
 *   该边界用例防止后续误把生产规则中的 >= 改成 >，造成阈值显示与实际判定不一致。
 */
static void test_exact_strong_thresholds_are_inclusive()
{
    const int width = 50;
    const int height = 20;
    std::vector<unsigned char> mask = make_mask(width, height); /* 三个分离区域面积依次为 120、90、90。 */
    const SegmentEvidenceSettings settings{20, 80, 300, 120};

    fill_rectangle(mask, width, 1, 2, 12, 10);
    fill_rectangle(mask, width, 20, 2, 9, 10);
    fill_rectangle(mask, width, 36, 2, 9, 10);
    mask[static_cast<std::size_t>(11 * width + 44)] = 0U;

    const SegmentEvidenceStats belowStats = analyze_segment_evidence(mask, width, height, settings);

    assert(belowStats.filteredDefectPixels == 299);
    assert(belowStats.largestComponentPixels == 120);
    assert(belowStats.level == SegmentEvidenceLevel::Weak);

    mask[static_cast<std::size_t>(11 * width + 44)] = 1U;

    const SegmentEvidenceStats stats = analyze_segment_evidence(mask, width, height, settings);

    assert(stats.filteredDefectPixels == 300);
    assert(stats.largestComponentPixels == 120);
    assert(stats.level == SegmentEvidenceLevel::Strong);
}

/*
 * test_diagonal_pixels_use_eight_neighbors 的作用：
 *   验证斜向相邻像素按 8 邻域视为一个连续缺陷，避免细划痕被拆成多个孤立点。
 */
static void test_diagonal_pixels_use_eight_neighbors()
{
    const int width = 8;
    const int height = 8;
    std::vector<unsigned char> mask = make_mask(width, height); /* mask 保存五个斜向连续像素。 */
    const SegmentEvidenceSettings settings{1, 5, 5, 5};

    for (int index = 0; index < 5; ++index) {
        mask[static_cast<std::size_t>(index * width + index)] = 1U;
    }

    const SegmentEvidenceStats stats = analyze_segment_evidence(mask, width, height, settings);

    assert(stats.componentCount == 1);
    assert(stats.largestComponentPixels == 5);
    assert(stats.level == SegmentEvidenceLevel::Strong);
}

/*
 * test_invalid_mask_size_is_rejected 的作用：
 *   验证 mask 元素数量与宽高不一致时抛出明确异常，防止生产代码越界访问。
 */
static void test_invalid_mask_size_is_rejected()
{
    const SegmentEvidenceSettings settings{20, 80, 300, 120};
    bool rejected = false; /* rejected 标记是否捕获到预期的参数异常。 */

    try {
        const std::vector<unsigned char> invalidMask(10U, 0U);
        (void)analyze_segment_evidence(invalidMask, 4, 4, settings);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }

    assert(rejected);
}

/*
 * test_invalid_threshold_orders_are_rejected 的作用：
 *   验证过滤线、复核线、坏品线和强连通域线的四类非法顺序都会被生产校验拒绝。
 */
static void test_invalid_threshold_orders_are_rejected()
{
    const int width = 2;
    const int height = 2;
    const std::vector<unsigned char> mask = make_mask(width, height);
    const SegmentEvidenceSettings invalidSettings[] = {
        {0, 80, 300, 120},   /* 过滤阈值不能小于 1。 */
        {20, 19, 300, 120},  /* 复核阈值不能低于过滤阈值。 */
        {20, 80, 79, 40},    /* 坏品阈值不能低于复核阈值。 */
        {20, 80, 300, 301}   /* 强连通域阈值不能高于坏品总面积阈值。 */
    };

    for (const SegmentEvidenceSettings &settings : invalidSettings) {
        bool rejected = false; /* rejected 标记当前非法组合是否抛出预期异常。 */

        try {
            (void)analyze_segment_evidence(mask, width, height, settings);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }

        assert(rejected);
    }
}

/*
 * main 的作用：
 *   顺序执行全部连通域证据回归用例，并输出统一 PASS 标记。
 *
 * 返回值：
 *   全部断言通过返回 0；断言失败时进程异常结束并返回非 0。
 */
int main()
{
    test_isolated_noise_is_clear();
    test_component_filter_boundary_is_inclusive();
    test_review_boundary_is_inclusive();
    test_medium_component_is_weak();
    test_large_component_is_strong();
    test_fragmented_large_area_stays_weak();
    test_exact_strong_thresholds_are_inclusive();
    test_diagonal_pixels_use_eight_neighbors();
    test_invalid_mask_size_is_rejected();
    test_invalid_threshold_orders_are_rejected();

    std::cout << "PASS: defect segment evidence" << std::endl;
    return 0;
}
