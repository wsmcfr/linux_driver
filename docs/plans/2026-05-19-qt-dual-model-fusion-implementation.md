# Qt Dual Model Fusion Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 把 Qt 双模型检测链路改成分类模型和 UNet 分割模型共同决定最终良品/坏品/待复核结果。

**Architecture:** 在 `CameraStorageController` 内新增统一融合函数，输入为 `classificationResult` 和 `segmentationResult`，输出云端结果、本地历史结果、首页综合状态和原因字段。检测完成、COS 上传、历史追加、历史重发和 QML 首页显示全部复用同一融合结果。

**Tech Stack:** Qt/C++、QML、Shell 静态契约测试、Markdown 模块文档。

---

### Task 1: 写失败的静态契约测试

| 文件 | 动作 |
|---|---|
| `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh` | 要求存在 `fusedResultFromModelResults`、`cloudResultFromFusedResult`、`historyTextFromFusedResult`、`uiStatusFromFusedResult`，并要求 `detectCurrentFrameOnce()` 和 `retryUploadRecord()` 不再调用旧的 `cloudResultFromClassificationResult()`。 |

运行：

```bash
bash 20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh
```

预期：在实现前失败，提示缺少融合函数或仍使用分类模型上传。

### Task 2: 实现 C++ 融合判定

| 文件 | 动作 |
|---|---|
| `20_uvc_camera/qt_camera_display/main.cpp` | 新增 `FusedDetectResult` 结构体和融合函数；`buildDetectModelResultLine()` 追加 `fused_status/fused_result/fused_reason`；`detectCurrentFrameOnce()` 上传时使用融合云端结果；`appendDetectHistoryRecord()` 使用融合本地结果；`retryUploadRecord()` 读取分割结果并按融合结果重发。 |

验证：

```bash
rg -n "fusedResultFromModelResults|cloudResultFromFusedResult|historyTextFromFusedResult" 20_uvc_camera/qt_camera_display/main.cpp
```

### Task 3: 更新 QML 首页和历史说明

| 文件 | 动作 |
|---|---|
| `20_uvc_camera/qt_camera_display/qml/Main.qml` | 双模型完成后优先读取 `fused_status` 更新首页综合状态；历史可信度说明和缺陷提示保留分类/UNet 细节，检测结论说明强调综合判定。 |

验证：

```bash
rg -n "fused_status|综合判定|综合检测" 20_uvc_camera/qt_camera_display/qml/Main.qml
```

### Task 4: 更新模块文档

| 文件 | 动作 |
|---|---|
| `20_uvc_camera/qt_camera_display/README.md` | 更新当前检测逻辑、检测历史路线、检测按钮与双模型部署、测试矩阵，说明“任一模型发现缺陷不得判良品”。 |

### Task 5: 运行验证

| 命令 | 预期 |
|---|---|
| `bash 20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh` | 输出 `PASS: Qt KMS overlay assets contract`。 |
| `rg -n "cloudResultFromClassificationResult\\(" 20_uvc_camera/qt_camera_display/main.cpp` | 不能再出现生产路径调用旧函数。 |
| `git diff --check` | 无空白错误。 |
