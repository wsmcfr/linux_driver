# STM32MP157 四分类模型量化替换设计

## 目标

将 `D:\model_picture\checkpoints_classify_4classes` 中的新 MobileNetV3-Small 四分类模型量化为适合 STM32MP157 CPU 推理的混合静态 INT8 ONNX，并替换板端原六分类模型。新模型只识别平垫圈和弹性垫圈的良品、坏品；现有 UNet 分割模型保持不变。

## 类别契约

| 输出索引 | 分类标签 | 零件类型 | 检测结果 |
|---:|---|---|---|
| 0 | `splitwasher_bad` | 弹性垫圈 | 坏品 |
| 1 | `splitwasher_good` | 弹性垫圈 | 良品 |
| 2 | `washer_bad` | 平垫圈 | 坏品 |
| 3 | `washer_good` | 平垫圈 | 良品 |

旧模型中的 `gasket_good/gasket_bad` 或 `wave_washer_good/wave_washer_bad` 不再由分类模型输出。云端兼容代码可以保留旧编码读取能力，但本轮检测不得再产生波形垫圈分类结果。

## 方案

1. 使用现有 `quantize_classify_int8.py --preset static_mixed` 量化新 FP32 ONNX，校准数据使用 `datasets_classify/val` 中四类共 160 张图片。
2. 量化前后都在完整验证集上执行批量推理，记录总准确率、逐类准确率和混淆情况。INT8 相对 FP32 准确率下降不得超过 1 个百分点；否则不部署并缩小量化节点范围。
3. 修改 `defect_classify.cpp`，从 ONNX 输出张量读取类别数，并要求该数量与 labels 中连续的 `idx_to_class` 数量完全一致。不得继续用固定的 6 类或直接改成固定 4 类。
4. 板端继续使用稳定文件名 `defect_classifier_static_mixed_int8.onnx` 和对应 labels 文件，使 Qt 主程序、检测配置与启动参数不需要迁移。
5. 部署脚本通过显式 `DEFECT_MODEL_SRC`、`DEFECT_LABELS_SRC` 指向新四分类产物。`DEFECT_UNET_MODEL_SRC` 仍指向现有分割模型，且部署前后校验分割模型 SHA256 不变。

## 数据流与校验边界

```text
四分类 FP32 ONNX
  -> 160 张四分类校准图片
  -> 混合静态 INT8 ONNX + labels
  -> defect-classify 校验输出维度与 labels 数量
  -> Qt 主程序解析 RESULT class/status/confidence
  -> QML 显示平垫圈或弹性垫圈
  -> 云端上传归一化 part_code=washer 或 splitwasher
```

| 边界 | 必须满足的契约 | 失败处理 |
|---|---|---|
| ONNX 输入 | 名称 `input`，类型 `float32`，形状 `N×3×224×224` | 停止量化或部署 |
| ONNX 输出 | 名称 `output`，形状 `N×4` | 停止部署 |
| labels | 索引 `0..3` 连续且与模型输出顺序一致 | 推理程序返回明确错误 |
| GOOD/BAD 汇总 | `_bad` 概率进入坏品总概率，`_good` 进入良品总概率 | 未知后缀视为标签契约错误 |
| 分割模型 | 文件路径、内容和 SHA256 均保持原值 | 发现变化则中止部署 |

## 部署与回滚

1. Windows 只同步本次相关源码、INT8 模型和 labels 到虚拟机，并在覆盖前备份虚拟机旧文件。
2. 在虚拟机加载 ST Qt/Wayland SDK，运行静态测试并交叉编译 `defect-classify` 与 Qt 主程序。
3. 完整部署优先使用 `deploy_qt_camera_display.sh`；部署前备份板端旧分类模型、labels、推理程序和 Qt 主程序。
4. 重启服务后对比虚拟机与板端 SHA256，执行单图分类和 Qt 检测自检。
5. 若推理失败、类别错误或精度不合格，恢复备份的旧分类模型、labels、推理程序和 Qt 主程序；分割模型无需回滚，因为本次不修改。

## 验收标准

| 测试目标 | 执行位置 | 成功标准 |
|---|---|---|
| FP32 基准 | Windows `D:\model_picture` | 完成 160 张四分类验证集评估并保存结果 |
| INT8 精度 | Windows `D:\model_picture` | 相对 FP32 总准确率下降不超过 1 个百分点，四类均有预测结果 |
| 模型体积 | Windows | INT8 文件小于 FP32 文件且 ONNX Runtime 可加载 |
| 推理契约 | 虚拟机 | 4 类模型与四项 labels 能推理；模型/labels 不一致时明确失败 |
| 静态回归 | 虚拟机 Qt 模块 | `test_qt_kms_overlay_assets.sh` 通过 |
| 交叉编译 | 虚拟机 | Qt 主程序和 `defect-classify` 均为 ARM 32 位可执行文件 |
| 板端替换 | STM32MP157 | 程序、分类模型和 labels 的 SHA256 与虚拟机一致 |
| 单图推理 | STM32MP157 | 只输出 `splitwasher_*` 或 `washer_*`，状态与后缀一致 |
| 双模型流程 | STM32MP157 | 分类使用新模型，分割仍使用原模型，Qt 自检能完成综合判定 |
