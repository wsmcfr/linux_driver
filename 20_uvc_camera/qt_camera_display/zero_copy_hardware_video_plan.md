# STM32MP157 UVC 零拷贝/硬件视频显示路线记录

## 当前状态

| 项目 | 结论 |
|---|---|
| Qt Quick 界面 | 已通过 `eglfs + galcore + OpenGL ES` 正常显示 |
| UVC 摄像头 | 已通过自定义 `V4L2VideoItem` 正常显示 |
| 当前低 CPU 版本 | `320x240@10fps`，5 秒平均 CPU 实测 `5.9%` |
| 当前高清尝试 | `640x480@15fps`，5 秒平均 CPU 实测 `42.0%` |
| 旧纯 CPU 版本 | `/root/uvc_fb_preview` 约 `44%~49% CPU` |
| Qt 内嵌 GL 桥接 | `VIDEO_BACKEND=qt-gst` 已能显示 Qt UI + 摄像头画面，默认 `mmap` 路线 CPU 约 `36.2%` |
| 当前可见低 CPU 基线 | `v4l2src mmap ! videoconvert fast(n-threads=1) ! BGRA ! kmssink` 在 `640x480@10fps` 可见，30 秒平均 CPU 约 `11.3%` |
| 当前正式 overlay 集成 | `run_qt_kms_overlay_display.sh restart` 已用项目内 `uvc_kms_overlay` 启动 Qt eglfs UI + KMS overlay plane `36` 视频，`640x480@10fps`，项目二进制运行到 `frames=5700`；10 秒样本 overlay `13.5%/13.6%`、Qt `2.3%/2.5%` |
| 当前主要问题 | 独立 `dmabuf ! glupload ! glimagesink` 低 CPU但后台/SSH 启动时 LCD 黑屏；`dmabuf ! qmlglsink` 在 Vivante 用户态崩溃；`mmap ! qmlglsink` 可显示但 CPU 仍高 |

当前版本是“安全预览版本”，用于证明 Qt 界面和 UVC 摄像头可以稳定跑通。它不是最终性能方案。

## 已验证不能继续走的路线

| 路线 | 结果 | 原因 |
|---|---|---|
| QML `Camera + VideoOutput` | 不可作为当前稳定方案 | 会加载 QtMultimedia/Vivante 视频节点 |
| `libimx6vivantevideonode.so` 零拷贝视频节点 | 触发内核 Oops | 栈落在 `glTexDirectVIVMap -> galcore _UserMemoryAttach -> dma_map_sg` |
| 自定义 V4L2 + `glTexImage2D` | 可稳定显示，但高清 CPU 高 | 每帧仍有 V4L2 拷贝和纹理上传 |
| `qmlglsink + UVC DMABUF` | 用户态段错误 rc=`139`，内核无 Oops | core/gdb 指向 `libGAL.so:gcoTEXTURE_GetMipMap()`，Qt scene graph 绘制该类纹理不稳定 |
| 后台/SSH `glimagesink` 直显 | 进程 PLAYING、CPU 约 `1.9%~2.1%`，但 LCD 黑屏 | 不能把“低 CPU + 进程存活”当作“可见显示成功” |
| `glupload/glcolorconvert/gldownload -> kmssink` | 部分组合黑屏，强制 BGRA 组合触发用户态 `SIGSEGV` | 当前 Vivante/GStreamer GL download 路径不可作为主线 |
| MJPEG 解码到 KMS | 可运行但 `640x480@15fps` CPU 约 `31.9%` | 软件 JPEG 解码比 YUYV KMS 基线更重 |

关键内核崩溃栈：

```text
QSGRenderThread
galcore _UserMemoryAttach
gckOS_WrapMemory
gckVIDMEM_NODE_WrapUserMemory
dma_cache_maint_page
Internal error: Oops: 17
```

这个栈说明问题不是普通应用崩溃，而是用户态视频帧被 Vivante GPU 驱动包装为可直接访问内存时触发了内核空指针。继续强行使用 QtMultimedia 的 Vivante 视频节点风险很高。

## 最终目标

| 指标 | 目标 |
|---|---|
| 画质 | 至少 `640x480@15fps`，优先 `640x480@30fps` |
| CPU | 显著低于旧 CPU 预览，目标小于 `15%` |
| 稳定性 | 连续运行不触发 `galcore` Oops |
| UI | Qt Quick 工业检测界面继续保留 |
| 视频路径 | 摄像头帧尽量不经过 CPU 像素转换和整帧纹理上传 |

## 推荐路线

### 本轮路线收敛结论

| 路线 | 当前静态证据 | 本轮动作 |
|---|---|---|
| GStreamer GL sink | rootfs 存在 `glupload`、`glimagesink` 和 DMABUF/EGLImage 相关符号；独立 `glimagesink` 已通过 10 分钟板端长测 | 当前已打通独立显示入口：`VIDEO_BACKEND=gst-gl /root/qt_camera_display/run_qt_camera_display.sh` |
| DRM/KMS `kmssink` | rootfs 存在 `libgstkms.so`，内核配置包含 `CONFIG_DRM=y`、`CONFIG_DRM_STM=y`、`CONFIG_DRM_FBDEV_EMULATION=y`，UVC 队列启用 `VB2_DMABUF` | 已验证 YUYV 直连 `kmssink` 会 `not-negotiated`；当前可见基线是 fast `videoconvert ! BGRA ! kmssink` |
| Wayland `waylandsink` | rootfs 存在 `libgstwaylandsink.so`、Wayland client 库和 Qt wayland-egl 插件 | 当前 rootfs 未发现 `weston` 可执行文件或 `drm-backend.so`，需要先补 compositor |
| Qt Quick `qmlglsink` | `libgstqmlgl.so` 已手工交叉编译并部署，`videotestsrc ! glupload ! qmlglsink` 最小示例可运行 | 普通 GL 纹理可嵌入；UVC DMABUF 纹理路径不可作为稳定主线 |
| 内核可开启项 | `CONFIG_DMA_ENGINE/STM32_DMA/STM32_MDMA/CMA` 已启用；`ltdc.c` 只映射 RGB 类 DRM 格式，STM32 media 目录只有 DCMI/CEC，没有 STM32MP1 DMA2D/Chrom-ART V4L2 mem2mem 转色驱动 | 目前没有“打开一个内核开关就让 LTDC 吃 YUYV”的路线；后续要么走 Wayland/GL shader，要么做专用 NEON/libyuv 转换 |
| Buildroot 可补项 | `waylandsink`、Wayland client 已有，`weston` 未启用；`gst1-plugins-base` 当前 Buildroot 配置里 OpenGL 仍是 disabled；`libyuv` 包存在但未启用 | 下一次 rootfs 正规化优先评估 Weston/GL 依赖和 `BR2_PACKAGE_LIBYUV`，不要继续只调 `videoconvert` 参数 |

板端验证入口：

```bash
/root/qt_camera_display/probe_zero_copy_video_path.sh
```

该脚本默认测试 `640x480@15fps`，记录 V4L2/KMS/Wayland/GStreamer 能力、短时 CPU、`dmesg` 中的 `Oops/galcore/dma_map_sg` 关键日志。

直接看 GL 硬件显示效果的启动入口：

```bash
VIDEO_BACKEND=gst-gl /root/qt_camera_display/run_qt_camera_display.sh
```

该模式只显示摄像头硬件视频画面，不显示 Qt 工业检测 UI；它用于确认画质、流畅度和 CPU，再进入 `qmlglsink` 的 Qt Quick 内嵌阶段。

### 2026-04-29 板端实测结论

| 路线 | 结果 | 结论 |
|---|---|---|
| `v4l2src io-mode=dmabuf ! fakesink` | `640x480@15fps` 可持续运行，CPU 约 `0.1%~0.3%` | UVC/V4L2 DMABUF 捕获链路可用 |
| `v4l2src io-mode=dmabuf ! kmssink` | 指定 `driver-name=stm` 后能打开 KMS，但 `not-negotiated` | STM32MP157 当前 KMS plane 不支持摄像头 YUYV 直扫 |
| `modetest -M stm` | primary plane: `AR24/XR24/RG24/RG16/...`，overlay plane: `AR24/RG24/RG16/...` | KMS plane 只支持 RGB 类格式，不支持 YUY2/NV12 |
| `v4l2src io-mode=dmabuf ! videoconvert ! BGRA ! kmssink driver-name=stm` | 可显示，CPU 约 `21.5%~21.6%` | KMS 显示可用，但 CPU 转色仍明显，不是最终零拷贝 |
| `v4l2src io-mode=dmabuf ! glupload ! glimagesink` | 可显示，短测 CPU 约 `1.9%`；10 分钟长测 CPU 约 `2.0%`，无 `Oops/galcore/dma_map_sg` | 当前最有希望的稳定硬件视频路线 |
| `qmlglsink` 插件 | 已部署到板端 `/usr/lib/gstreamer-1.0/libgstqmlgl.so`，可 `gst-inspect-1.0 qmlglsink` | 插件问题已解决 |
| `videotestsrc ! glupload ! qmlglsink` | GStreamer 自带 Qt 示例 10 秒存活 | qmlglsink 可显示普通 GL 纹理 |
| `v4l2src mmap ! videoconvert ! glupload ! glcolorconvert ! gleffects_identity ! qmlglsink` | 已嵌入 Qt Quick 工业界面，640x480@15fps，CPU 约 `36.2%`，无 Oops | 当前可看效果路线，但不是最终低 CPU 零拷贝 |
| `v4l2src dmabuf ! glupload ! qmlglsink` | rc=`139`；加 `glcolorconvert/gleffects_identity` 仍 rc=`139` | core/gdb 指向 `libGAL.so:gcoTEXTURE_GetMipMap()`，不要反复作为主线 |
| Qt UI + 外部 `glimagesink render-rectangle` | Qt 和 `gst-launch` 可同时存活 | framebuffer 抓图未显示视频叠层，eglfs 叠放不可作为已验证嵌入 |

### 2026-04-30 追加板端实测结论

| 路线 | 结果 | 结论 |
|---|---|---|
| 后台 `glimagesink` | `v4l2src dmabuf ! glupload ! glimagesink` 协商到 `GLMemory`，CPU 约 `1.9%~2.1%`，但 LCD 黑屏 | 只能作为低 CPU 处理基准，不能作为当前可见显示路线 |
| KMS BGRA 基线 | `v4l2src dmabuf ! videoconvert ! BGRA ! kmssink driver-name=stm` 可见，CPU 约 `21.6%` | 稳定可见恢复路线 |
| fast KMS 15fps | `videoconvert dither=none chroma-mode=none matrix-mode=input-only chroma-resampler=nearest n-threads=2` 后 `640x480@15fps` 可见，CPU 约 `18.0%` | 比默认 KMS 低，但仍高于 `<15%` 目标 |
| fast KMS 10fps | 同一 fast KMS 路线改为 `640x480@10fps` 可见，30 秒平均 CPU 约 `11.9%` | 当前可用低 CPU 可见基线 |
| mmap KMS 10fps | `v4l2src io-mode=mmap ! fast videoconvert n-threads=1 ! BGRA ! kmssink`，30 秒 CPU 约 `11.3%` | 当前更低的可见低 CPU 基线 |
| mmap KMS 5fps | 同一 mmap KMS 路线改为 `640x480@5fps`，CPU 约 `5.5%~5.7%` | 极限低 CPU 备选，但流畅度下降 |
| 低分辨率 KMS 预览 | `320x240` YUYV 采集，经 fast `videoconvert n-threads=1` 转 `BGRA`，再用 `kmssink render-rectangle=<0,0,1024,600>` 请求显示矩形；全线程 CPU 口径：`5fps` 约 `3.3%`，`10fps` 30 秒约 `6.5%`、复测约 `6.4%`，`15fps` 约 `9.9%` | 当前最低 CPU 的正确彩色可见候选；画面细节低于 `640x480`，实际显示尺寸需肉眼确认，适合作为预览/演示保底，不满足最终高清检测目标 |
| 低分辨率参数微调 | 同为 `320x240@10fps` 全屏 KMS：`mmap + BGRA + n-threads=1` 最低约 `6.4%~6.5%`；`n-threads=2` 约 `6.6%`；`dmabuf + BGRA` 约 `6.8%`；`BGRx` 报 `not-negotiated` | 保持 `mmap + BGRA + n-threads=1`，不要为了 dmabuf 或 BGRx 牺牲稳定性 |
| 640x480 KMS 全线程复测 | `640x480@10fps` 正确彩色 `mmap + fast videoconvert + BGRA + kmssink` 约 `22.4%~23.4%`；`dmabuf + BGRA` 约 `23.4%`；`YUY2` 直连仍 `not-negotiated`；`RGBA` 不协商；`BGRx + plane-id=33` 约 `22.7%` | 纯 KMS 参数微调无法让 `640x480@10fps` 稳定低于 `<15%` |
| 640x480 预转换前丢帧 | 摄像头按 `640x480@10fps` 采集，在 `videoconvert` 前用 `videorate drop-only=true max-rate=6` 输出 `6fps`，再转 `BGRA` 到 KMS；全线程 CPU 观测为 30 秒约 `11.2%`，后续 20 秒复查约 `13.8%` | 当前推荐的 `640x480` 低 CPU 可见路线，代价是运动流畅度约 `6fps` |
| 640x480 8fps 尝试 | `10fps -> videorate -> 8fps -> videoconvert -> kmssink` 短测约 `14.5%`，但 30 秒复测约 `18.4%` | 不能宣称稳定低于 `<15%` |
| 640x480 5fps 对比 | 直接 `640x480@5fps` KMS 30 秒约 `11.1%~11.2%`；`10fps -> drop 5fps` 约 `11.5%~11.9%` | 如果接受低帧率，直接 `5fps` 与 `drop 6fps` 都可用；`drop 6fps` 更顺一点 |
| 640x480 更低显示帧率 | 摄像头仍按 `640x480@10fps` 输入，在 `videoconvert` 前丢帧：`drop 5fps` 15 秒样本约 `11.83%`，`drop 4fps` 约 `9.39%`，`drop 3fps` 约 `7.21%`；测试结束已恢复 `drop 6fps`，恢复样本约 `11.18%` | 这是保持 640 输入细节的低 CPU 档位，但运动流畅度依次下降；`drop 4fps` 可作为低于 10% 的实验档，不能当 15fps 目标达成 |
| mmap KMS 15fps | `n-threads=3` 短测出现 `14.5%`，但 30 秒长测约 `17.2%` | 不能宣称稳定低于 `<15%` |
| 捕获方式对比 | `mmap 10fps` 约 `11.3%`，`dmabuf 10fps` 约 `11.8%~11.9%`，`userptr 10fps` 约 `11.7%`，`rw` 不支持 | 当前 CPU 转 RGB 到 KMS 时，`mmap` 略优 |
| `matrix-mode=none` KMS 15fps | `640x480@15fps` CPU 约 `7.0%`，但用户肉眼确认颜色异常 | 不能作为正常彩色视频路线，只能作为灰度/伪彩实验线索 |
| `matrix-mode=output-only` KMS 15fps | CPU 约 `35.1%`，用户肉眼确认颜色异常 | 既不低 CPU，也不正确显示，淘汰 |
| explicit primary plane | `plane-id=33 + BGRx` 可跑但 CPU 约 `17.8%`，`plane-id=33 + BGRA` 约 `17.4%`；`RGBx` 和 `plane-id=36 + BGRx` 不协商 | 强制 plane 没有突破 `<15%`，正确彩色 KMS 仍卡在约 `17%~18%` |
| 通用灰度链路 | `YUYV -> GRAY8 -> BGRA -> kmssink` CPU 约 `21.6%~22.2%` | 两段 `videoconvert` 不适合低 CPU 灰度；若要灰度应写专用 Y 分量路径 |
| 原始尺寸 framebuffer 原型 | `/root/uvc_fb_preview_native -w 640 -h 480 -r 15 -p` 避免全屏缩放，CPU 约 `21.6%`；查表优化尝试约 `22.6%~22.7%` | 不如 fast KMS，说明 framebuffer 写回/CPU 转色仍是瓶颈 |
| GL download 到 KMS | `glupload ! glcolorconvert ! gldownload ! kmssink` 低 CPU但黑屏；强制 `BGRA` 组合触发 `SIGSEGV` | 暂停作为主线 |
| GL window 后端复测 | 默认 `glimagesink` 和 `GST_GL_WINDOW=gbm` 组合均失败，日志为 `Failed to bind OpenGL|ES API: EGL_BAD_PARAMETER`；`libgstopengl.so` 可见 X11/Wayland 字符串，未见可用 GBM 后端 | 当前 rootfs 缺少可用 GL 窗口/合成环境，仍需补 Weston/Wayland 后再谈 GL 可见显示 |
| Weston DRM 补测 | 板端原本无 `weston`；用 ST SDK sysroot 的 `libexec_weston.so` 编译极小 `weston-test` wrapper，并临时补入 `libweston-8`、`drm-backend.so`、`gl-renderer.so`、`xkeyboard-config`、`libVSC.so` 后，`weston-test --help` 可运行；`drm-backend` 能打开 `/dev/dri/card0`，识别 universal planes、atomic modesetting 和 Vivante EGL 1.5 | Wayland compositor 主体可推进到 DRM/EGL 初始化阶段，但 GL renderer 创建 context 失败：`EGL_BAD_CONFIG`，尚不能进入 `waylandsink` 视频验证 |
| Weston pixman/fbdev 对照 | `weston-test --backend=drm-backend.so --use-pixman` 卡在 `failed to create input devices`，板端有 `/dev/input/event*` 但缺 `udevadm`/udev seat 运行态；`fbdev-backend.so` 可短暂创建 `wayland-0` socket，但不是低 CPU 目标，且受 tty/logind 状态影响退出 | 当前 Wayland 双 surface 路线不是缺 `waylandsink`，而是缺可稳定启动的 Weston DRM+GL compositor；下一步应优先解决 Vivante EGL config / Weston 构建配置和 udev seat，而不是直接跑 UVC `waylandsink` |
| MJPEG 到 KMS | `MJPG -> jpegdec idct-method=ifast -> videoconvert -> kmssink` 可跑；全线程 CPU 口径下 `320x240@10fps` 约 `13.7%`，`320x240@15fps` 约 `20.9%`，`640x480@10fps` 约 `50.4%` | 软件 JPEG 解码太贵，淘汰 |
| KMS 格式尝试 | `BGRx/RGBx/RGB/RGB16` 不协商，`BGR` 可跑但 CPU 约 `23.7%` | `BGRA` 仍是当前最稳 KMS 输出格式 |
| RGB565 插件映射实验 | 在 Buildroot build tree 的 `gstkmsutils.c` 加入 `DRM_FORMAT_RGB565 -> GST_VIDEO_FORMAT_RGB16` 后，板端 `gst-inspect-1.0 kmssink` 已显示 `RGB16`；但实测 `640x480@10fps RGB16` 约 `24.5%~25.0%`，`15fps` 约 `36.8%`，`10fps -> drop 6fps RGB16` 约 `14.7%` | RGB565 减少输出字节数，但转换路径不省 CPU；不作为推荐彩色路线 |
| ORC 版 `videoconvert` | 已启用 `BR2_PACKAGE_ORC=y`，`gst1-plugins-base` meson 日志为 `-Dorc=enabled`，板端部署 ORC 版 `libgstvideo/libgstvideoconvert/liborc`；复测 `640x480@10fps BGRA` 约 `23.0%~23.1%`，`15fps` 约 `34.6%`，`10fps -> drop 6fps` 约 `13.9%` | ORC 没有带来决定性收益，保留为构建状态记录即可 |
| LTDC 矩形/缩放源码结论 | `ltdc_plane_atomic_check()` 只检查目标矩形不小于源矩形，`ltdc_plane_atomic_update()` 按源矩形写窗口寄存器，未看到 scaler 配置；`to_ltdc_pixelformat()` 不支持 YUYV/NV12 | 不要把 `render-rectangle=<0,0,1024,600>` 解释成已验证硬件缩放或硬件转色；画面大小仍需肉眼确认 |
| Wayland 依赖 | `waylandsink` 和 Qt `libqwayland-egl.so` 存在；原始 rootfs 未发现 `weston` 可执行文件和 rootfs 内 `drm-backend.so`；临时补 ST SDK Weston 组件后，`drm-backend` 能进入 Vivante EGL，但失败在 `EGL_BAD_CONFIG`；pixman 对照失败在 udev/input seat | 下一主线需正规化 Weston/Gcnano EGL provider 和 udev seat 配置；在 compositor 未稳定前，不要直接把 `waylandsink` 失败误判为视频格式问题 |
| 摄像头输出格式约束 | 当前 UVC 只支持 `YUYV` 和 `MJPEG` | 由于 STM32MP157 当前 KMS plane 只有 RGB 类格式，彩色视频必须在进入 KMS 前完成 YUYV->RGB 或 MJPEG 解码 |
| CPU 统计口径提醒 | 旧记录中部分数字只读取 `/proc/$pid/stat` 主线程；本轮新增记录累加 `/proc/$pid/task/*/stat` 的所有 GStreamer 线程，并按双核归一化成单核百分比 | 后续横向比较必须注明口径；同一路线用同一口径复测后再下结论 |
| framebuffer 原型复测 | `/root/uvc_fb_preview_native -w 640 -h 480 -r 10 -p` 约 `28.2%`，`-r 10` 全屏缩放约 `45.0%`，`-r 5 -p` 约 `14.1%` | 不如 KMS `10fps -> 6fps` 观测范围 `11.2%~13.8%`，继续保留为实验代码而非推荐路线 |
| Direct KMS NEON XRGB 探针 | 临时 `/tmp/uvc_kms_probe` / `/tmp/uvc_kms_probe_stage` 使用 V4L2 mmap、DRM dumb XRGB8888 framebuffer、手写 NEON `YUYV -> XRGB8888`；`640x480@10fps` 保持实际 `10/1`，历史 30 秒样本最低 `11.2%`，同场复测 `13.3%`；2026-05-01 用户确认颜色正常，10 分钟以上候选运行到 `frames=6477`，CPU 样本约 `13.4%~13.7%`，无新增 crash 关键字 | 当前最好的 10fps direct KMS 可见候选，运动流畅度优于 `drop6`；仍需确认位置/大小和决定是否接受无 Qt UI 的全屏 direct KMS 形态；`15fps` 仍约 `20.9%`，未达标 |
| Direct KMS overlay plane 正式链路 | 临时 `/tmp/uvc_kms_probe_overlay_rect` 已整理为 `uvc_kms_overlay.c`、`build_uvc_kms_overlay.sh` 和 `run_qt_kms_overlay_display.sh`；正式默认命令等价于 `uvc_kms_overlay -d /dev/video0 -w 640 -h 480 -r 10 -m neon -F argb8888 -P 36 -x 177 -y 73 -W 640 -H 480`，仍不调用 `drmModeSetCrtc` | Qt primary/eglfs 与视频 overlay plane `36` 已共存；用户确认画面融合正常。项目二进制运行到 `frames=5700`；两次 10 秒样本 overlay `13.5%/13.6%`、Qt `2.3%/2.5%`，无新增 `Oops/galcore/dma_map_sg/segfault/gcoTEXTURE`；这是当前推荐的集成型 10fps 路线 |
| Direct KMS RGB565/staging 对照 | `DRM_FORMAT_RGB565` dumb buffer 可创建，pitch 为 `2048`，但 `640x480@10fps` direct RGB565 30 秒 CPU `13.7%`；`XRGB8888 + staging` 为 `15.2%`，`RGB565 + staging` 为 `14.4%` | RGB565 和 cached staging+memcpy 都没有优于 direct XRGB8888；后续不要把它们当下一主线，除非转换核心或 framebuffer 内存属性发生实质变化 |

### 路线 A：Wayland compositor + 硬件视频 surface

| 项目 | 内容 |
|---|---|
| 核心思路 | Weston/Wayland 负责合成，Qt 作为 Wayland 客户端，GStreamer 视频作为另一个 Wayland surface |
| 视频链路 | `v4l2src io-mode=dmabuf` → `waylandsink` 或支持 DMABUF/EGLImage 的 sink |
| UI 链路 | Qt Quick 使用 `QT_QPA_PLATFORM=wayland-egl` |
| 优点 | 避免 QtMultimedia 的 Vivante 视频节点；视频可由 compositor/GPU/KMS 合成 |
| 风险 | 当前 rootfs 需要补齐 Weston/Wayland sink；需要验证 sink 是否真的使用 DMABUF/零拷贝 |

验证命令：

```bash
ls -l /dev/dri/card0 /dev/galcore /dev/video0
gst-inspect-1.0 waylandsink
gst-inspect-1.0 v4l2src
```

初步管线：

```bash
gst-launch-1.0 v4l2src device=/dev/video0 io-mode=dmabuf ! \
  video/x-raw,format=YUY2,width=640,height=480,framerate=30/1 ! \
  waylandsink sync=false
```

若 `io-mode=dmabuf` 不支持，先用 `io-mode=mmap` 验证显示，再继续查内核和插件的 DMABUF 能力。

### 路线 B：DRM/KMS overlay plane + Qt EGLFS UI

| 项目 | 内容 |
|---|---|
| 核心思路 | Qt EGLFS 继续显示 UI，摄像头画面由 KMS plane 或 `kmssink` 显示到底层视频平面 |
| 视频链路 | `uvc_kms_overlay`: V4L2 `mmap` → NEON `YUYV -> ARGB8888` → `drmModeSetPlane` |
| UI 链路 | Qt Quick 继续 `eglfs` |
| 优点 | 视频不必变成 Qt 纹理，减少 CPU 拷贝 |
| 风险 | 当前仍需 CPU 转色，且 `15fps` 尚未达标；overlay 矩形变化要同步 QML 预留区域并重新肉眼确认 |

当前正式入口：

```bash
/root/qt_camera_display/run_qt_kms_overlay_display.sh restart
/root/qt_camera_display/run_qt_kms_overlay_display.sh status
```

默认使用 `640x480@10fps`、`ARGB8888`、overlay plane `36`、矩形 `177,73,640,480`。该路线已经验证 Qt UI 和视频 plane 共存，但仍需要 CPU 做 `YUYV -> ARGB8888`，所以不是纯零拷贝；它解决的是“低 CPU 视频不进入 Qt scene graph 也能和 UI 同屏”的集成问题。

验证命令：

```bash
ls -l /dev/dri/card0
gst-inspect-1.0 kmssink
```

初步管线：

```bash
gst-launch-1.0 v4l2src device=/dev/video0 io-mode=dmabuf ! \
  video/x-raw,format=YUY2,width=640,height=480,framerate=30/1 ! \
  kmssink sync=false
```

如果没有 `/dev/dri/card0` 或 `kmssink` 不能控制当前 RGB LCD，这条路线要先补 DRM/KMS 显示栈。

### 路线 C：GStreamer GL sink 嵌入 Qt Quick

| 项目 | 内容 |
|---|---|
| 核心思路 | 使用 `qmlglsink`、`glupload`、`GstGLMemory` 一类插件，把视频帧以 GL 纹理形式交给 Qt Quick |
| 优点 | 可以保留单一 Qt 进程和 QML 层级 |
| 当前结论 | `mmap` 可嵌入但 CPU 高；`dmabuf` 会在 Vivante `libGAL` 用户态段错误 |
| 风险 | 不能把 `qmlglsink + UVC DMABUF` 当稳定零拷贝路线；必须换合成方式或自研更可控的 Qt GL 渲染路径 |

验证命令：

```bash
gst-inspect-1.0 qmlglsink
gst-inspect-1.0 glupload
gst-inspect-1.0 glimagesink
```

当前实测已经证明 `glupload ! glimagesink` 可以稳定低 CPU 显示，也证明 `qmlglsink` 可以嵌入普通 GL 纹理。真正缺口变成：UVC DMABUF 导入纹理交给 Qt scene graph 后会触发 Vivante 用户态段错误。因此 `qmlglsink` 只保留 `mmap` 稳定桥接；最终低 CPU 路线应转向 Wayland surface 合成、KMS plane 合成，或自研 Qt GL Item 在 Qt 上下文内更可控地导入/绘制 DMABUF。

## 下一阶段实施顺序

| 步骤 | 目标 | 验证标准 |
|---|---|---|
| 1 | 保留并复测已打通 GL 路线 | `VIDEO_BACKEND=gst-gl /root/qt_camera_display/run_qt_camera_display.sh` 能显示 `640x480@15fps`，CPU 低于 `15%` |
| 2 | 保留 `qt-gst` mmap 桥接效果 | `VIDEO_BACKEND=qt-gst` 能显示 Qt UI 与摄像头画面，明确标记 CPU 约 `36.2%` |
| 3 | 关闭 `qmlglsink + dmabuf` 主线 | 记录 rc=`139`、core/gdb `libGAL.so:gcoTEXTURE_GetMipMap()`，后续只做受控复现实验 |
| 4 | 补 Wayland/Weston 合成环境 | Qt UI 和视频 sink 分离为两个 surface，避免 Qt scene graph 直接绘制 UVC DMABUF 纹理；注意当前 Buildroot Weston DRM 依赖 Mesa EGL，可能需要 ST OpenSTLinux Weston 包或 Buildroot provider 配置 |
| 5 | 固化 KMS overlay plane + direct NEON | 项目内 `uvc_kms_overlay` + `run_qt_kms_overlay_display.sh` 已证明 Qt primary/eglfs 与视频 overlay plane `36` 能共存；后续重点转为长稳测试、stop/start 可运维性和转换核心优化 |
| 6 | 评估专用 NEON/libyuv 转换 | direct KMS `XRGB8888/ARGB8888 + NEON` 已成为当前 10fps 候选；RGB565 dumb buffer 与 cached staging+memcpy 已无收益，后续只尝试真正不同的 NEON/LUT/libyuv 转换核心 |

## 当前安全预览版本保留策略

| 项目 | 说明 |
|---|---|
| 保留原因 | 它能稳定显示摄像头，是后续调 UI 和检测叠加的备用路径 |
| 默认参数 | `320x240@10fps`，CPU 约 `5.9%` |
| 高清参数 | `CAMERA_WIDTH=640 CAMERA_HEIGHT=480 CAMERA_FPS=15`，CPU 约 `42.0%` |
| 不满足点 | 画质不够，且高清时 CPU 仍高 |
| 当前独立低 CPU 预览 | 保底：`gst-kms-320x240-10fps-n1-full`，即 `320x240@10fps` YUYV 转 `BGRA` 后由 KMS 显示，30 秒全线程 CPU 约 `6.4%~6.5%`；当前 `640x480` 推荐线：`10fps -> videorate drop 6fps -> BGRA -> kmssink`，观测范围约 `11.2%~13.9%`；若只追求更低 CPU，可临时测 `drop 4fps` 约 `9.39%` |

启动命令：

```bash
/root/qt_camera_display/run_qt_camera_display.sh
```

高清测试命令：

```bash
CAMERA_WIDTH=640 CAMERA_HEIGHT=480 CAMERA_FPS=15 /root/qt_camera_display/run_qt_camera_display.sh
```

## CPU 测试命令

```bash
pid=$(pidof qt_camera_display | awk '{print $1}')
ncpu=$(grep -c '^cpu[0-9]' /proc/stat)
t1=$(awk '/^cpu /{s=0; for(i=2;i<=NF;i++)s+=$i; print s}' /proc/stat)
p1=$(awk '{s+=$14+$15} END{print s}' /proc/$pid/task/*/stat)
sleep 5
t2=$(awk '/^cpu /{s=0; for(i=2;i<=NF;i++)s+=$i; print s}' /proc/stat)
p2=$(awk '{s+=$14+$15} END{print s}' /proc/$pid/task/*/stat)
awk -v p1=$p1 -v p2=$p2 -v t1=$t1 -v t2=$t2 -v n=$ncpu 'BEGIN{printf("qt_camera_display 全线程平均CPU: %.1f%%\n", (p2-p1)*100*n/(t2-t1));}'
```

## 明确结论

当前已经完成“Qt 工业检测界面 + UVC 摄像头稳定显示”的第一阶段打通。

下一阶段不能只靠降低分辨率和帧率优化 CPU。`320x240@10fps + KMS` 已经可以把独立预览压到约 `6.4%~6.5%`，是牺牲输入细节得到的保底路线；`640x480` 当前可见低 CPU 路线是 `10fps` 采集后在 `videoconvert` 前丢到 `6fps`，观测约 `11.2%~13.9%`，但运动流畅度下降。进一步降到 `drop 4fps` 可到约 `9.39%`，但这只是低帧率预览档，不是高清流畅方案。内核源码已经确认 LTDC 不支持 YUYV/NV12 直扫，也没有现成 STM32MP1 硬件 mem2mem 转色开关；最终仍必须验证并切换到稳定的零拷贝/硬件视频显示链路，优先顺序为：

1. 保留 `VIDEO_BACKEND=qt-gst` 默认 mmap 桥接：它已经把视频嵌回 Qt Quick，可用于看界面效果和继续 UI 联调，但 CPU 约 `36.2%`。
2. GStreamer 独立 GL sink：`v4l2src io-mode=dmabuf ! glupload ! glimagesink` 仍是当前最低 CPU 证据，继续作为画质/性能基准。
3. Wayland compositor + `waylandsink`/DMABUF：优先补 compositor，把 Qt UI 和视频 surface 分开合成，绕开 `qmlglsink + dmabuf`。
4. 专用 NEON/libyuv 转换：在 KMS 仍只能吃 RGB 的约束下，用更专门的 YUYV->BGRA 转换替代通用 `videoconvert`。
5. DRM/KMS plane + `kmssink`/DMABUF：当前 YUYV/NV12 直扫不通，仅作为 RGB 转换基线或后续格式支持改造参考。

QtMultimedia `Camera + VideoOutput` 在当前 `galcore` 驱动组合下已经触发内核 Oops，除非后续升级或修复 GPU 驱动，否则不作为稳定路线。
