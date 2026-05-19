# Qt Dual Model Fusion Design

| 项目 | 内容 |
|---|---|
| 目标 | 让 STM32MP157 Qt 检测链路等待分类模型和 UNet 分割模型都完成后，再按两个模型的详细结果生成最终良品/坏品/待复核判断。 |
| 背景 | 当前检测链路已经串行运行 `defect-classify` 和 `defect-segment`，但云端 `CLOUD_RESULT` 和历史主结果仍主要按分类模型 `GOOD/BAD` 决定，导致“分类为 GOOD 但 UNet 检出划痕/缺陷”的样本可能被登记为良品。 |
| 设计原则 | 任一模型发现缺陷时，不允许最终判为良品；分类和分割结果冲突时进入保守坏品/待复核路径；云端上传、历史记录、首页综合状态必须使用同一个融合函数。 |

## 判定矩阵

| 分类模型结果 | UNet 分割结果 | 最终云端结果 | 本地历史结果 | 首页综合状态 |
|---|---|---|---|---|
| `GOOD` | `OK` 且 `defect_pixels=0` | `good` | `良品` | `GOOD` |
| `BAD` | 任意 | `bad` | `待复核` | `BAD` |
| `GOOD` | `NG` 或 `defect_pixels>0` | `bad` | `待复核` | `BAD` |
| 未知/缺失 | 任意 | `review` | `待复核` | `REVIEW` |

## 数据流

| 阶段 | 输入 | 输出 | 责任 |
|---|---|---|---|
| 当前帧保存 | KMS overlay 原始 YUYV 帧 | `sourcePath` JPG | `uvc_kms_overlay` 保存模型输入图，不包含显示 ROI 框。 |
| 分类模型 | `sourcePath` | `RESULT status=GOOD/BAD class=... confidence=...` | `defect-classify` 输出零件类别、分类状态和概率。 |
| UNet 模型 | `sourcePath` | `RESULT_SEG status=OK/NG defect_pixels=...` | `defect-segment` 输出缺陷区域和结果图。 |
| 融合判定 | 两个模型输出 | `fused_status/fused_result/fused_reason` | Qt C++ 统一生成最终结果，供 UI、历史和云端共用。 |
| 上传历史 | source + annotated + 融合结果 | 云端记录和 `upload_history.json` | 不再用单个分类模型结果决定最终 good/bad。 |

## 错误处理

| 情况 | 处理 |
|---|---|
| 分类模型失败 | 保持现有失败返回，不继续上传，避免缺少主分类上下文。 |
| UNet 模型失败 | 保持现有失败返回，不写入成功历史，避免缺陷依据缺失却误判良品。 |
| 旧历史缺少分割结果 | 重发时使用 `review`，避免旧记录被误传为良品。 |
| 结果字段未知 | 统一落到 `review/待复核`，不静默默认 `good`。 |
