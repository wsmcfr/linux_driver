# Qt 统计分析界面设计

## 目标

| 项目 | 设计结论 |
|---|---|
| 页面名称 | `统计分析` |
| 目标屏幕 | STM32MP157 7 寸 RGB LCD，固定 1024x600 |
| 数据来源 | 本地 `uploadHistory` 模型，对应 `/mnt/sdcard/images/upload_history.json` |
| 当前边界 | 统计保存/上传历史，不声称已经接入真实缺陷类型模型 |
| 交互目标 | 现场人员能快速看到保存数量、良品/待复核、上传成功率、文件状态，并能从最近记录跳转历史详情 |

## 设计原则

| 原则 | 说明 |
|---|---|
| 本地优先 | 统计页不依赖实时云端查询，网络断开时仍能查看 SD 卡中已有证据。 |
| 数据密集 | 1024x600 屏幕空间有限，采用 KPI + 条形图 + 最近记录表，而不是大面积装饰。 |
| 轻量绘制 | 使用 QML `Rectangle` 绘制柱状图和分布条，不新增 Qt Charts 依赖。 |
| 复用详情 | 最近记录表点击后进入已有历史详情页，避免在统计页重复图片轮播和长路径展示。 |
| Overlay 边界 | 统计页打开时隐藏 KMS overlay 视频 plane，返回首页时恢复。 |

## 页面结构

| 区域 | 内容 |
|---|---|
| 顶部标题 | `统计分析`、样本数量、返回首页按钮 |
| KPI 行 | 总记录、良品、待复核、上传成功率、图片总量 |
| 最近保存趋势 | 最近 8 条记录的轻量柱状图，绿色表示云端登记成功，黄色表示本地保存但上传待排查 |
| 分布概览 | 良品、待复核、上传成功、上传失败四条水平分布条 |
| 最近记录 | 最新 5 条记录的时间、结果、云端编号、图片数和状态 |
| 云端与文件状态 | 最近时间、最近编号、云端状态、平均文件大小、最大批次大小 |

## 数据契约

| 字段 | 来源 | 用途 |
|---|---|---|
| `uploadHistory.count` | `UploadHistoryModel` | 总记录和空状态判断 |
| `resultText` | `upload_history.json` | `良品` 计入良品，其它计入待复核 |
| `recordId` / `recordNo` | 上传脚本结果 | 判断云端登记是否成功 |
| `uploadStatus` | 上传脚本摘要 | 展示云端状态、兼容上传失败记录 |
| `jpgSizeBytes` / `pngSizeBytes` | 本地文件大小 | 计算图片总大小、平均文件和最大批次 |
| `imageCount` | JPG/PNG 路径数量 | 统计图片总量和最近记录图片数 |

## 验收方式

| 测试目标 | 执行位置 | 命令/动作 | 预期输出/现象 | 失败时排查 |
|---|---|---|---|---|
| 静态契约 | 本地仓库 | `./test_qt_kms_overlay_assets.sh` | 输出 `PASS: Qt KMS overlay assets contract` | 检查 `statsPageVisible`、`statsSummary`、`statsPage` 等标记 |
| 历史数据 | 开发板 | `test -s /mnt/sdcard/images/upload_history.json; grep -c '"upload_time"' /mnt/sdcard/images/upload_history.json` | JSON 非空且输出记录数 | 先点击首页 `保存图片` 生成记录 |
| 页面入口 | 开发板屏幕 | 点击左侧 `统计分析` | 页面显示 KPI、趋势、分布、最近记录和云端状态 | 查 `switchPage("stats")` 和触摸输入 |
| 详情跳转 | 开发板屏幕 | 点击统计页最近记录行 | 进入对应历史详情页，可查看 JPG/PNG | 查 `openHistoryDetailFromStats()` 和 `showHistoryDetail()` |
| Overlay 隐藏 | 开发板屏幕 | 进入统计页再返回首页 | 统计页不被视频覆盖，返回首页后视频恢复 | 查 `setOverlayVisible(pageName === "home")` 和 overlay `VISIBLE` 命令 |

