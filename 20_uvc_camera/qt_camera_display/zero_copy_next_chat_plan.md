# 下一轮任务计划：STM32MP157 稳定零拷贝/硬件视频显示链路

## 新对话启动提示

如果重新开启一个对话，请先把下面这段话发给 Codex：

```text
我要继续 STM32MP157 工业缺陷检测 Qt 界面的 UVC 摄像头优化。当前 Qt Quick/eglfs/GPU UI 已经跑通，自定义 V4L2 安全预览也能显示摄像头；320x240@10fps CPU 约 5.9%，但画质差；640x480@15fps CPU 约 42%，接近旧 CPU framebuffer 预览。QtMultimedia Camera+VideoOutput 已触发 galcore _UserMemoryAttach/dma_map_sg 内核 Oops，不能作为稳定方案。板端曾验证 `v4l2src io-mode=dmabuf ! glupload ! glimagesink` 低 CPU，但当前后台/SSH GL 启动要么 LCD 黑屏，要么 `EGL_BAD_PARAMETER`，不能继续当作可见显示成功。当前真正可见的低 CPU KMS 路线是 `v4l2src mmap ! videoconvert dither=none chroma-mode=none matrix-mode=input-only chroma-resampler=nearest n-threads=1 ! BGRA ! kmssink driver-name=stm`：`320x240@10fps + render-rectangle=<0,0,1024,600>` 请求显示矩形保底约 6.4%~6.5%，实际显示大小需肉眼确认；当前后台保留 `640x480` 测试线，即摄像头 `640x480@10fps` 采集，在 `videoconvert` 前 `videorate drop-only=true max-rate=6` 丢到 `6fps`，再转 `BGRA` 到 KMS，全线程 CPU 观测约 11.2%~13.8%。`640x480@10fps` 不丢帧的正确彩色 KMS 约 22.4%~23.4%，`640x480@15fps` 全线程约 35.5%，`10fps -> 8fps` 短测 14.5% 但 30 秒复测 18.4%，不能宣称稳定低于 <15%。`qmlglsink` 插件已部署，`VIDEO_BACKEND=qt-gst /root/qt_camera_display/run_qt_camera_display.sh` 已能把视频嵌回 Qt Quick 画面区，但当前稳定默认路线是 `v4l2src io-mode=mmap ! videoconvert ! glupload ! glcolorconvert ! gleffects_identity ! qmlglsink`，640x480@15fps CPU 约 36.2%。`GST_IO_MODE=dmabuf VIDEO_BACKEND=qt-gst` 会在用户态 rc=139，core/gdb 指向 Vivante `libGAL.so:gcoTEXTURE_GetMipMap()`，内核无 Oops。后续所有长时间显示管线必须 `nohup ... >/tmp/<route>.log 2>&1 < /dev/null &` 后台运行。请阅读 20_uvc_camera/qt_camera_display/zero_copy_next_chat_plan.md 和 zero_copy_hardware_video_plan.md，下一步不要重复 qmlglsink+dmabuf 直接嵌入，优先补 Weston/Wayland 或做专用 NEON/GL 转换，目标是在保持 `640x480` 更高帧率时继续压 CPU。
补充：RGB565 KMS 插件映射和 ORC 版 videoconvert 已实测，无明显降 CPU；direct KMS 探针里的 RGB565 dumb buffer 与 cached staging+memcpy 也已实测无收益：direct `XRGB8888` 同场 `13.3%`，direct `RGB565` `13.7%`，`XRGB8888 + staging` `15.2%`，`RGB565 + staging` `14.4%`。direct KMS `XRGB8888 + NEON` 仍是当前最好的 10fps 候选，历史最低 30 秒 `11.2%`；2026-05-01 用户确认颜色正常，10 分钟以上候选运行到 `frames=6477`，CPU 样本约 `13.4%~13.7%`，无新增 crash 关键字。新的集成路线已从 `/tmp/uvc_kms_probe_overlay_rect` 正规化为项目内 `uvc_kms_overlay` 和 `/root/qt_camera_display/run_qt_kms_overlay_display.sh`：默认使用 STM overlay plane `36`，不调用 `drmModeSetCrtc`，`640x480@10fps`、`ARGB8888`、矩形 `177,73,640,480`；用户已确认画面融合正常，项目二进制运行到 `frames=5700`，两次 10 秒样本 overlay `13.5%/13.6%`、Qt `2.3%/2.5%`，无新增 crash 关键字。内核 LTDC 不支持 YUYV/NV12 直扫，也没有现成 STM32MP1 硬件 mem2mem 转色开关。`640x480@10fps -> drop 5/4/3fps` 分别约 `11.83%/9.39%/7.21%`，但只是降低显示帧率；当前恢复线仍用 `drop 6fps`，约 `11.2%~14.2%`。
```

## 本轮真正目标

| 项目 | 目标 |
|---|---|
| 不是要做什么 | 不是继续靠 `320x240@10fps` 降低 CPU |
| 真正要做什么 | 找到稳定的零拷贝或硬件视频显示链路 |
| UI 要求 | 保留 Qt Quick 工业检测界面 |
| 视频要求 | 至少 `640x480@15fps`，优先 `640x480@30fps` |
| CPU 目标 | 高清预览时显著低于旧 CPU 预览，目标小于 `15%` |
| 稳定性目标 | 连续运行不出现 `galcore` Oops、不死机、不占死 `/dev/video0` |

## 当前可看效果入口

| 项目 | 命令或结论 |
|---|---|
| 当前可见低 CPU 基线 | `nohup gst-launch-1.0 -q v4l2src device=/dev/video0 io-mode=mmap ! 'video/x-raw,format=YUY2,width=640,height=480,framerate=10/1' ! videorate drop-only=true max-rate=6 skip-to-first=true silent=true ! 'video/x-raw,format=YUY2,width=640,height=480,framerate=6/1' ! videoconvert dither=none chroma-mode=none matrix-mode=input-only chroma-resampler=nearest alpha-mode=set alpha-value=1 n-threads=1 qos=false ! 'video/x-raw,format=BGRA,width=640,height=480,framerate=6/1' ! kmssink driver-name=stm sync=false async=false enable-last-sample=false qos=false show-preroll-frame=false processing-deadline=0 max-lateness=-1 'render-rectangle=<0,0,1024,600>' >/tmp/gst640-src10-drop6-bgra-fullrect.log 2>&1 < /dev/null &` |
| 当前基线定位 | LCD 可见，`640x480@10fps` 输入在转色前丢到 `6fps`，全线程 CPU 约 `11.2%~14.2%`，用于测试失败后的恢复和对比 |
| GL 视频路线状态 | `v4l2src io-mode=dmabuf ! glupload ! glimagesink sync=false` 后台/SSH 启动 CPU 约 `1.9%~2.1%`，但 LCD 黑屏 |
| GL 路线定位 | 只能作为低 CPU 处理基准，不能当作当前可见显示入口 |
| Qt 内嵌可看效果 | `VIDEO_BACKEND=qt-gst /root/qt_camera_display/run_qt_camera_display.sh` |
| Qt 内嵌当前定位 | 已显示 Qt 工业检测 UI + 摄像头画面，默认走 `mmap` 稳定路线，CPU 约 `36.2%`，不是最终低 CPU 零拷贝 |
| Qt + KMS overlay 正式入口 | `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart`，Qt eglfs UI + 项目内 `uvc_kms_overlay` 视频 plane `36`，默认 `640x480@10fps`、矩形 `177,73,640,480` |
| 下一步集成方向 | overlay+Qt 链路已打通并正规化；下一步围绕长稳、启动/停止可运维性和 `15fps` 转换核心优化继续推进 |

## 当前已知事实

| 事实 | 证据 |
|---|---|
| 旧 CPU framebuffer 预览 CPU 高 | `/root/uvc_fb_preview` 约 `44%~49% CPU` |
| Qt Quick/eglfs/GPU UI 已通 | 界面、字体、叠加框、按钮和状态栏已显示 |
| 当前安全预览已通 | 自定义 `V4L2VideoItem` 能显示 UVC |
| 低 CPU 版本画质不足 | `320x240@10fps` CPU 约 `5.9%`，但画质差 |
| 高清安全预览 CPU 仍高 | `640x480@15fps` CPU 约 `42.0%` |
| QtMultimedia 路线不稳定 | `Camera + VideoOutput` 触发 `galcore _UserMemoryAttach -> dma_map_sg` 内核 Oops |
| 当前可执行程序不再依赖 QtMultimedia | `readelf -d qt_camera_display` 只需要 QtQuick/QtGui/QtQml/QtCore |

## 本轮 VM/rootfs 静态探测补充

| 项目 | 当前证据 | 对路线的影响 |
|---|---|---|
| GStreamer KMS 插件 | NFS rootfs 存在 `/usr/lib/gstreamer-1.0/libgstkms.so`，依赖 `libdrm.so.2` 和 `libgstallocators-1.0.so.0` 均存在 | KMS/DRM route 可以优先做板端独立验证 |
| GStreamer Wayland 插件 | NFS rootfs 存在 `libgstwaylandsink.so`、`libwayland-client.so.0` 和 Qt wayland-egl 插件 | sink 侧具备，但还缺 compositor |
| Weston compositor | NFS rootfs 未发现 `weston`、`drm-backend.so` 或 `libweston*` | Wayland route 需要先补 rootfs 包或另行确认已有 compositor |
| GStreamer GL 插件 | rootfs 存在 `libgstopengl.so`，`glupload ! glimagesink` 可协商 `GLMemory` 且 CPU 约 `1.9%~2.1%` | 当前后台/SSH 启动 LCD 黑屏；只能保留为低 CPU处理基准，不能作为可见路线 |
| qmlglsink | 已部署 `/usr/lib/gstreamer-1.0/libgstqmlgl.so`，`gst-inspect-1.0 qmlglsink` 可识别元素 | 插件可用，但 UVC DMABUF 嵌入 Qt scene graph 会用户态段错误 |
| 内核 DRM/KMS | kernel `.config` 和 `stm32mp1_atk_defconfig` 均有 `CONFIG_DRM=y`、`CONFIG_DRM_STM=y`、`CONFIG_DRM_KMS_HELPER=y`、`CONFIG_DRM_FBDEV_EMULATION=y` | 具备 `/dev/dri/card0` 出现的内核基础 |
| UVC DMABUF 能力 | `drivers/media/usb/uvc/uvc_queue.c` 中采集队列启用 `VB2_DMABUF`，`vb2_vmalloc_memops` 实现 `get_dmabuf/attach_dmabuf` | 可以测试 `v4l2src io-mode=dmabuf`，但最终是否能被 KMS import 仍需实机验证 |
| 内核硬件转色能力 | `drivers/gpu/drm/stm/ltdc.c` 只把 RGB 类 DRM 格式映射到 LTDC，未支持 YUYV/NV12；STM32 media 目录只有 DCMI/CEC，未发现 STM32MP1 DMA2D/Chrom-ART V4L2 mem2mem 转色驱动 | 没有可直接开启的内核 YUYV->RGB 硬件转换开关；DMA/MDMA 只能搬运，不会自动做色彩空间转换 |
| Buildroot 优化余地 | `BR2_PACKAGE_ORC=y` 已验证但收益不明显；`BR2_PACKAGE_LIBYUV` 未启用；`waylandsink` 已有但 Weston 未启用，且标准 Weston DRM backend 依赖 Mesa EGL | 后续真正值得做的是 Weston/Wayland 依赖闭环，或专用 libyuv/NEON 转换实验 |
| 板端 SSH | VM 能看到 `192.168.1.250:22`；已把 VM 公钥写入 NFS rootfs `/root/.ssh/authorized_keys` 后成功登录 | 后续可直接远程执行板端只读验证和短时管线测试 |

## 本轮板端实测结果

| 路线 | 板端结果 | 下一步判断 |
|---|---|---|
| SSH 访问 | 已把 VM 公钥写入 NFS rootfs `/root/.ssh/authorized_keys`，可从 VM 登录 `root@192.168.1.250` | 后续可直接远程跑板端验证 |
| UVC 格式 | `/dev/video0` 支持 MJPG 和 YUYV；YUYV `640x480@30fps` 可用 | 当前测试继续使用 `640x480@15fps` 保守验证 |
| DMABUF 捕获 | `v4l2src device=/dev/video0 io-mode=dmabuf ! ... ! fakesink` 运行成功，CPU 约 `0.1%~0.3%` | UVC DMABUF 捕获不是瓶颈 |
| KMS 原始直连 | `kmssink driver-name=stm` 能打开 KMS，但 `YUY2 -> kmssink` 报 `not-negotiated` | STM32MP157 KMS plane 不支持 YUYV/NV12，不能走 YUYV 直扫 |
| KMS 转 RGB 基线 | `videoconvert ! BGRA ! kmssink driver-name=stm` 可显示，CPU 约 `21.5%~21.6%` | 比 42% fallback 低，但仍不是最终零拷贝 |
| GL 硬件路线 | `v4l2src io-mode=dmabuf ! glupload ! glimagesink` 可显示，短测 CPU 约 `1.9%`，10 分钟长测 CPU 约 `2.0%`，无 `Oops/galcore/dma_map_sg` | 当前低 CPU 画质基准；`qmlglsink` 已完成 mmap 嵌回但 DMABUF 会崩，下一步转 Wayland surface、KMS plane 或自研 Qt GL Item |
| qmlglsink 包 | Buildroot `output-uvc/.config` 和 `configs/stm32mp1_atk_defconfig` 已加入 `BR2_PACKAGE_GST1_PLUGINS_GOOD_PLUGIN_QMLGL=y`；因 DNS 卡住，当前板端插件由 ST SDK 手工编译部署 | 后续 rootfs 正规重建恢复后应把插件纳入 Buildroot 产物 |
| qmlglsink 最小示例 | GStreamer 自带 `videotestsrc ! glupload ! qmlglsink` 示例在板端 10 秒存活 | `qmlglsink` 本身能显示普通 GL 纹理 |
| Qt 内嵌 mmap | `VIDEO_BACKEND=qt-gst` 默认 `mmap`，实际链路 `v4l2src mmap ! videoconvert ! glupload ! glcolorconvert ! gleffects_identity ! qmlglsink`，640x480@15fps 可嵌入 UI，5 秒 CPU 约 `36.2%`，无 Oops | 已打通“嵌回界面”的可看效果，但 CPU 仍接近旧预览，只能作为稳定桥接路线 |
| Qt 内嵌 dmabuf | `GST_IO_MODE=dmabuf VIDEO_BACKEND=qt-gst` 即使加 `glcolorconvert/gleffects_identity` 仍 rc=`139` | core/gdb 指向 `libGAL.so:gcoTEXTURE_GetMipMap()`；不要把它当稳定路线反复跑 |
| 外部 glimagesink + Qt | Qt UI 进程与外部 `gst-launch ... glimagesink render-rectangle=<176,72,560,330>` 可同时存活 | framebuffer 抓图只看到 Qt 黑色视频区，eglfs 叠放/透明洞不可靠，暂不作为已嵌入方案 |
| 后台 GL sink 可见性复测 | `v4l2src dmabuf ! glupload ! glimagesink` 进程 PLAYING，CPU 约 `1.9%~2.1%`，但 LCD 黑屏 | 低 CPU 不等于显示成功，必须以 LCD 人工确认作为可见判据 |
| KMS BGRA 默认基线 | `v4l2src dmabuf ! videoconvert ! BGRA ! kmssink driver-name=stm` 可见，CPU 约 `21.6%` | 作为稳定可见恢复路线 |
| KMS fast 15fps | `videoconvert dither=none chroma-mode=none matrix-mode=input-only chroma-resampler=nearest n-threads=2` 后 `640x480@15fps` 可见，CPU 约 `18.0%` | 比默认 KMS 低，但仍高于 `<15%` 目标 |
| KMS fast 10fps | 同一 fast KMS 路线改为 `640x480@10fps` 可见，30 秒平均 CPU 约 `11.9%` | 当前可用低 CPU 可见基线，测试失败后优先恢复 |
| mmap KMS 10fps | `v4l2src io-mode=mmap ! fast videoconvert n-threads=1 ! BGRA ! kmssink`，30 秒 CPU 约 `11.3%` | 当前更低的可见低 CPU 基线 |
| mmap KMS 5fps | 同一 mmap KMS 路线改为 `640x480@5fps`，CPU 约 `5.5%~5.7%` | 极限低 CPU 备选，但流畅度下降 |
| 低分辨率 KMS 预览 | `320x240` YUYV 采集，fast `videoconvert n-threads=1` 转 `BGRA`，`kmssink render-rectangle=<0,0,1024,600>` 请求显示矩形；全线程 CPU：`5fps` 约 `3.3%`，`10fps` 30 秒约 `6.5%`、复测约 `6.4%`，`15fps` 约 `9.9%` | 当前最低 CPU 正常彩色候选；保留 `10fps` 后台运行，但画质细节低于 `640x480`，实际显示大小需肉眼确认 |
| 低分辨率参数微调 | 同为 `320x240@10fps` 全屏：`mmap + BGRA + n-threads=1` 最低约 `6.4%~6.5%`；`n-threads=2` 约 `6.6%`；`dmabuf + BGRA` 约 `6.8%`；`BGRx` 报 `not-negotiated` | 后续恢复/保底继续用 `mmap + BGRA + n-threads=1` |
| 640x480 当前后台线 | `640x480@10fps` 采集，`videorate drop-only=true max-rate=6` 在 `videoconvert` 前丢到 `6fps`，再 `BGRA -> kmssink` | 全线程 CPU 观测约 `11.2%~14.2%`，当前推荐 `640x480` 低 CPU 可见线 |
| Direct KMS NEON 10fps 候选 | 临时 `/tmp/uvc_kms_probe` / `/tmp/uvc_kms_probe_stage`，V4L2 mmap + DRM dumb XRGB8888 framebuffer + 手写 NEON `YUYV -> XRGB8888`；`640x480@10fps` 实际保持 `10/1` | 历史 30 秒最低 `11.2%`，同场复测 `13.3%`；2026-05-01 用户确认颜色正常，10 分钟以上候选运行到 `frames=6477`，CPU 样本约 `13.4%~13.7%`，无新增 crash 关键字。它比当前 `drop6` 更流畅，但仍需确认位置/大小和是否接受无 Qt UI 的 direct KMS 形态 |
| Direct KMS overlay plane 正式链路 | 项目内 `uvc_kms_overlay -d /dev/video0 -w 640 -h 480 -r 10 -m neon -F argb8888 -P 36 -x 177 -y 73 -W 640 -H 480`，由 `run_qt_kms_overlay_display.sh` 统一 start/stop/status | Qt primary/eglfs 与视频 overlay plane `36` 已共存，用户确认画面融合正常；项目二进制运行到 `frames=5700`，两次 10 秒样本 overlay `13.5%/13.6%`、Qt `2.3%/2.5%`，无新增 crash 关键字。它是当前推荐的集成型 10fps 正式路线 |
| Direct KMS RGB565/staging 对照 | `DRM_FORMAT_RGB565` 可创建，pitch `2048`，但 direct RGB565 `13.7%`；cached staging+memcpy：XRGB `15.2%`、RGB565 `14.4%` | RGB565 和 staging 都没有优于 direct XRGB8888，后续不要作为主线重复 |
| 640x480 更低显示帧率 | 保持 `640x480@10fps` 输入，在转色前分别丢到 `5/4/3fps`：15 秒样本约 `11.83% / 9.39% / 7.21%`；测试后恢复 `drop 6fps`，恢复样本约 `11.18%` | `drop 4fps` 可低于 10%，但肉眼会更卡；这是低帧率预览档，不是最终 `640x480@15fps` 目标 |
| 640x480 不丢帧 KMS | `640x480@10fps` 正常彩色约 `22.4%~23.4%`，`640x480@15fps` 约 `35.5%` | 仅靠 KMS/videoconvert 参数不能达到 `<15%` |
| 640x480 8fps 尝试 | `10fps -> videorate -> 8fps` 短测 `14.5%`，30 秒复测 `18.4%` | 不稳定达标，不能作为推荐线 |
| GL 后端复测 | 默认 `glimagesink` 与 `GST_GL_WINDOW=gbm` 均失败，`EGL_BAD_PARAMETER`；当前 OpenGL 插件更像需要 X11/Wayland 窗口环境 | 继续 GL/Wayland 前先补 Weston/compositor |
| Weston 临时补测 | 原 rootfs 无 `weston`；用 ST SDK sysroot 的 `libexec_weston.so` 编译 `weston-test` wrapper，并临时补入 `libweston-8`、`drm-backend.so`、`gl-renderer.so`、`xkeyboard-config`、`libVSC.so` 后，`weston-test --help` 可运行；`drm-backend` 能打开 `/dev/dri/card0`，识别 universal planes、atomic modesetting 和 Vivante EGL 1.5 | Compositor 进入 DRM/EGL 初始化后卡在 `failed to create context` / `EGL_BAD_CONFIG`，尚不能跑 UVC `waylandsink`；这不是 `waylandsink` caps 问题 |
| Weston pixman/fbdev 对照 | `--use-pixman` 的 DRM backend 卡在 `failed to create input devices`；板端有 `/dev/input/event*`，但缺 `udevadm`/udev seat 运行态；fbdev backend 曾短暂创建 `wayland-0` socket，但不是低 CPU 路线且受 tty/logind 状态影响退出 | 下一步若继续 Wayland，应先解决 Weston DRM+GL 的 Vivante EGL config 和 udev seat，而不是直接反复跑 `v4l2src ! waylandsink` |
| Weston 测试后恢复 | 测试结束已杀掉 Weston/GStreamer 残留并恢复 `640x480@10fps -> drop6 -> BGRA -> kmssink`；恢复后 PID `5569`，10 秒全线程 CPU `13.9%`，`dmesg` crash 关键字只见 Galcore 版本行 | 当前板端已回到推荐可见线；后续失败后仍恢复这条线 |
| 正式 overlay 测试后状态 | 已通过 `/root/qt_camera_display/run_qt_kms_overlay_display.sh restart` 从临时探针切到项目内 `uvc_kms_overlay`；当前应看到 Qt PID 与 `uvc_kms_overlay` PID，无 `gst-launch-1.0` 和无 `/tmp/uvc_kms_probe_overlay_rect` | 新会话仍必须重新 `pidof gst-launch-1.0`，并用 `run_qt_kms_overlay_display.sh status` 确认当前 PID |
| framebuffer 原型复测 | `/root/uvc_fb_preview_native -w 640 -h 480 -r 10 -p` 约 `28.2%`；`-r 5 -p` 约 `14.1%`；全屏缩放约 `45.0%` | 不如当前 KMS `drop 6fps` 路线 |
| mmap KMS 15fps | `n-threads=3` 短测出现 `14.5%`，但 30 秒长测约 `17.2%` | 不能宣称稳定低于 `<15%` |
| 捕获方式对比 | `mmap 10fps` 约 `11.3%`，`dmabuf 10fps` 约 `11.8%~11.9%`，`userptr 10fps` 约 `11.7%`，`rw` 不支持 | 当前 CPU 转 RGB 到 KMS 时，`mmap` 略优 |
| `matrix-mode=none` KMS 15fps | `640x480@15fps` CPU 约 `7.0%`，但用户确认颜色异常 | 淘汰为正常彩色视频路线，只能保留为灰度/伪彩实验线索 |
| `matrix-mode=output-only` KMS 15fps | CPU 约 `35.1%`，用户确认颜色异常 | 既不低 CPU，也不正确显示，淘汰 |
| explicit primary plane | `plane-id=33 + BGRx` 可跑但 CPU 约 `17.8%`，`plane-id=33 + BGRA` 约 `17.4%`；`RGBx` 和 `plane-id=36 + BGRx` 不协商 | 强制 plane 没有突破 `<15%`，正确彩色 KMS 仍卡在约 `17%~18%` |
| 通用灰度链路 | `YUYV -> GRAY8 -> BGRA -> kmssink` CPU 约 `21.6%~22.2%` | 两段 `videoconvert` 不适合低 CPU 灰度；若要灰度应写专用 Y 分量路径 |
| 原始尺寸 framebuffer 原型 | `/root/uvc_fb_preview_native -w 640 -h 480 -r 15 -p` 避免全屏缩放，CPU 约 `21.6%`；查表优化尝试约 `22.6%~22.7%` | 不如 fast KMS，说明 framebuffer 写回/CPU 转色仍是瓶颈 |
| GL download 到 KMS | `glupload ! glcolorconvert ! gldownload ! kmssink` 低 CPU但黑屏；强制 `BGRA` 组合触发用户态 `SIGSEGV` | 暂停作为主线 |
| MJPEG 到 KMS | `MJPG -> jpegdec idct-method=ifast -> videoconvert -> kmssink` 可跑；全线程 CPU：`320x240@10fps` 约 `13.7%`，`320x240@15fps` 约 `20.9%`，`640x480@10fps` 约 `50.4%` | 软件 JPEG 解码路线淘汰 |
| KMS 输出格式 | `BGRx/RGBx/RGB/RGB16` 不协商，`BGR` 可跑但 CPU 约 `23.7%` | `BGRA` 仍是当前最稳 KMS 输出格式 |
| RGB565/ORC/direct staging 实验 | KMS 插件补 `RGB16` 后 `gst-inspect` 能看到 RGB16，但 `640x480@10fps RGB16` 约 `24.5%~25.0%`，`drop6 RGB16` 约 `14.7%`；ORC 版 `videoconvert` 部署后 `10fps BGRA` 仍约 `23%`，`drop6` 约 `13.9%`；direct KMS 探针里 RGB565 `13.7%`、XRGB staging `15.2%`、RGB565 staging `14.4%` | RGB565、ORC、cached staging 都没有突破瓶颈，后续不要在这三处反复消耗时间 |
| 摄像头输出格式约束 | 当前 UVC 只支持 `YUYV` 和 `MJPEG` | KMS plane 只支持 RGB 类格式，所以彩色视频不能纯零拷贝直扫；必须做 YUYV->RGB 或 MJPEG 解码 |

## 当前代码和文档位置

| 路径 | 用途 |
|---|---|
| `20_uvc_camera/qt_camera_display/main.cpp` | Qt 程序入口，注册 `V4L2VideoItem`，传入摄像头参数 |
| `20_uvc_camera/qt_camera_display/v4l2_video_item.h` | 自定义 V4L2 QML Item 声明 |
| `20_uvc_camera/qt_camera_display/v4l2_video_item.cpp` | V4L2 mmap 采集 + OpenGL ES YUYV shader 显示 |
| `20_uvc_camera/qt_camera_display/qml/Main.qml` | 工业检测 QML 界面 |
| `20_uvc_camera/qt_camera_display/run_qt_camera_display.sh` | 板端启动脚本，默认 `320x240@10fps` |
| `20_uvc_camera/qt_camera_display/uvc_kms_overlay.c` | 正式 KMS overlay 辅助进程源码，V4L2 mmap + NEON `YUYV -> ARGB8888` + plane `36` 显示 |
| `20_uvc_camera/qt_camera_display/build_uvc_kms_overlay.sh` | VM 交叉编译 `uvc_kms_overlay` |
| `20_uvc_camera/qt_camera_display/run_qt_kms_overlay_display.sh` | 板端正式 overlay+Qt 链路控制脚本，支持 `start/stop/restart/status/restore-fallback` |
| `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh` | 静态检查 KMS overlay 正式交付物的关键契约 |
| `20_uvc_camera/qt_camera_display/probe_zero_copy_video_path.sh` | 板端零拷贝/硬件视频链路探测脚本，覆盖 DMABUF 捕获、KMS 基线和 GL sink 路线 |
| `20_uvc_camera/qt_camera_display/zero_copy_hardware_video_plan.md` | 已有路线记录和背景 |
| `20_uvc_camera/qt_camera_display/zero_copy_next_chat_plan.md` | 本文件，下一轮执行计划 |
| `/home/cfr/linux/nfs/rootfs/root/qt_camera_display` | 开发板 NFS rootfs 中的 Qt 程序部署位置 |

## 禁止重复踩坑

| 禁止项 | 原因 |
|---|---|
| 不要把 `320x240@10fps` 当最终方案 | CPU 低是因为数据量小，画质不满足项目 |
| 不要混用 CPU 统计口径 | 旧记录部分只读 `/proc/$pid/stat`，新记录累加 `/proc/$pid/task/*/stat` 所有线程；比较前必须说明口径 |
| 不要直接恢复 QML `Camera + VideoOutput` | 当前板端已经触发过 `galcore` 内核 Oops |
| 不要只看 Qt 进程 CPU 就宣称优化完成 | 必须同时说明分辨率、帧率、显示链路和稳定性 |
| 不要先改大工程 | 先用独立命令验证 Wayland/KMS/DMABUF 能力，再集成 Qt |
| 不要在 Oops 后继续反复运行同一路径 | 需要重启板子，避免内核状态污染测试结果 |
| 不要继续把 `qmlglsink + dmabuf` 直接当主线 | 当前已经有 core 证据指向 Vivante 用户态 `gcoTEXTURE_GetMipMap()`，短测必崩 |
| 不要把 `qt-gst` 默认 mmap 结果叫零拷贝 | 它通过 `videoconvert` 和系统内存保证稳定，CPU 约 `36.2%` |
| 不要前台运行长时间显示管线 | GStreamer 日志会刷到 LCD 控制台并干扰画面判断，必须用 `nohup ... >/tmp/<route>.log 2>&1 < /dev/null &` |
| 不要把 `glimagesink` 低 CPU 当作可见成功 | 本轮已出现进程 PLAYING 但 LCD 黑屏，必须有人眼确认画面 |
| 不要把 `matrix-mode=none` 当彩色优化 | 它能把 CPU 压到约 `7.0%`，但颜色异常，说明没有做正确 YUV->RGB 转换 |
| 不要把 `matrix-mode=output-only` 当折中 | 实测 CPU 约 `35.1%` 且颜色异常，比 fast KMS 正常彩色路线更差 |
| 不要期待内核开关直接解决 YUYV->RGB | 本地内核没有 STM32MP1 硬件 mem2mem 转色驱动；`VIDEO_VIM2M`、`VIDEO_MEM2MEM_DEINTERLACE`、DMA/MDMA 都不是这块板的硬件彩色转换方案 |
| 不要把 `render-rectangle` 当硬件缩放证据 | `ltdc_plane_atomic_update()` 未看到 scaler 配置，画面大小/位置必须肉眼确认，文档中不要把它描述为已证明的硬件缩放 |
| 不要把 Weston 已补齐等同于 Wayland 路线成功 | 临时 Weston wrapper 已能进入 DRM/EGL 初始化，但 GL renderer 失败在 `EGL_BAD_CONFIG`，pixman 又被 udev/input seat 卡住；必须先解决 compositor，再测 `waylandsink` 视频 surface |

## 下一轮总路线

| 优先级 | 路线 | 目标 |
|---|---|---|
| 保底 | 保留 `qt-gst` mmap 内嵌桥接 | 已能看到 Qt UI + 摄像头画面；用于现场演示和 UI 联调，但记录为高 CPU 稳定桥接，不作为零拷贝主线 |
| 1 | Wayland compositor + `waylandsink`/DMABUF | 避开 Qt scene graph 直接绘制 UVC DMABUF 纹理，由 compositor 负责 surface 合成；需要补 Weston/compositor |
| 2 | DRM/KMS overlay plane + direct NEON | 项目内 `uvc_kms_overlay` 已用 overlay plane `36` 与 Qt primary/eglfs 共存，用户确认画面融合正常；下一步做更长稳定性、启动/停止可运维验证和 `15fps` 转换核心优化 |
| 3 | 专用 NEON/libyuv 转换 | direct KMS 手写 NEON 已证明 10fps 候选成立；后续不再重复 RGB565/staging，而是尝试真正不同的 NEON/LUT/libyuv 转换核心 |
| 4 | 自研 Qt GL Item 导入 DMABUF | 风险高；必须在 Qt GL 上下文内自己 EGLImage import + shader 绘制，避免 qmlglsink 的跨上下文纹理路径 |
| 5 | 优化当前安全路径 | 只作为过渡，例如动态帧率和低帧率预览档，不作为最终零拷贝 |

## 下一步主线执行计划

### 主线 1：Wayland Surface 合成

| 步骤 | 操作 | 验证标准 | 停止条件 |
|---|---|---|---|
| 1 | 在 Buildroot/rootfs 中确认或补齐 Weston、`waylandsink`、Qt wayland-egl 运行依赖 | `weston --backend=drm-backend.so --idle-time=0` 能启动，`gst-inspect-1.0 waylandsink` 成功 | Weston 或 `waylandsink` 缺依赖且短期无法补齐 |
| 2 | 独立启动 Wayland 视频 surface | `v4l2src io-mode=dmabuf ! ... ! waylandsink sync=false` 可显示 `640x480@15fps` | `waylandsink` 退回 CPU 转换、黑屏、或报 DMABUF/format 不支持 |
| 3 | Qt 以 Wayland 客户端启动 | `QT_QPA_PLATFORM=wayland-egl /root/qt_camera_display/qt_camera_display ...` 能显示工业界面 | Qt wayland-egl 缺插件或 compositor 不能合成 Qt surface |
| 4 | 双 surface 合成定位 | Qt UI 与视频 surface 同屏，视频落在预览区或可由 compositor 规则定位 | 只能全屏覆盖或无法控制层级/位置 |
| 5 | 10 分钟稳定性和 CPU 测试 | `640x480@15fps` CPU 低于 `15%`，无 `Oops`、无 `SIGSEGV`、无 `/dev/video0` 占死 | CPU 高于目标或出现内核/用户态崩溃 |

### 主线 2：KMS Plane 合成

| 步骤 | 操作 | 验证标准 | 停止条件 |
|---|---|---|---|
| 1 | 记录 DRM 资源 | `modetest -M stm` 输出 connector、CRTC、primary/overlay plane 和格式列表 | 没有 `/dev/dri/card0` 或 `stm` DRM 不可用 |
| 2 | 复测已知基线 | `videoconvert ! BGRA ! kmssink driver-name=stm` 可显示，CPU 约 `21%` 量级 | 基线也不能显示，先回到 DRM/rootfs 配置 |
| 3 | 查 plane 格式与摄像头格式是否可桥接 | 明确是否存在 YUYV/NV12/RGB DMABUF 可 import 的 plane | plane 仅支持 RGB 且必须 CPU 转色，则不作为零拷贝主线 |
| 4 | 尝试指定 plane/rectangle | `kmssink plane-id=<id> render-rectangle=<x,y,w,h>` 或等效属性能控制视频位置 | Qt EGLFS 与 `kmssink` 抢占同一 DRM master，无法共存 |
| 5 | 10 分钟稳定性和 CPU 测试 | Qt UI 与视频 plane 同屏，CPU 低于 `15%`，无 Oops/段错误 | 只能全屏、只能 CPU RGB 转换、或 UI 被覆盖 |

### 主线 3：自研 Qt GL Item 可控导入 DMABUF

| 步骤 | 操作 | 验证标准 | 停止条件 |
|---|---|---|---|
| 1 | 做最小实验，不直接改工业界面 | 单独 Qt Quick 测试程序创建一个 GL Item，只画固定纹理/FBO | 最小 GL Item 在板端 eglfs 都不稳定 |
| 2 | 在 Qt 渲染线程内完成 EGLImage/DMABUF import | import、shader 绘制、fence/sync 都发生在 Qt GL 上下文可控位置 | 复用 QtMultimedia 或 qmlglsink 的 DMABUF 纹理路径 |
| 3 | 接入 V4L2 DMABUF 生命周期 | buffer queue/dequeue、EGLImage 创建/销毁、纹理释放顺序明确 | `/dev/video0` 停止后无法释放，或重复启动泄漏 |
| 4 | 嵌入现有 QML 预览区 | 替换 `GstVideoSurface.qml` 或新增独立 Item，叠加 ROI/状态层仍正常 | QML 叠加层无法合成或触发 `libGAL` 段错误 |
| 5 | 10 分钟稳定性和 CPU 测试 | `640x480@15fps` CPU 低于 `15%`，无 `gcoTEXTURE_GetMipMap`、无 Oops | 任何 Vivante 用户态段错误，立即回退，不继续硬闯 |

### 主线优先级判断

| 判断项 | 优先选择 |
|---|---|
| 能快速补齐 compositor | 先走 Wayland surface 合成 |
| DRM plane 能明确支持目标格式和矩形叠层 | 走 KMS plane 合成 |
| Wayland/KMS 都被 rootfs 或硬件格式卡住 | 再做自研 Qt GL Item |
| 需要现场演示界面效果 | 使用 `VIDEO_BACKEND=qt-gst` 默认 mmap 桥接 |
| 需要低 CPU 处理基准 | 使用独立 `glupload ! glimagesink`，但必须标注“当前后台启动黑屏，非可见成功” |
| 测试失败后需要恢复可见画面 | 使用 `gst-fast-kms-10fps` 后台管线 |

## 第一轮已完成，以下命令作为复测清单

这些信息已经在本轮板端实测中收集过；如果换摄像头、换 rootfs、换内核或重启后怀疑环境变化，再执行以下命令复测：

```bash
ls -l /dev/video* /dev/fb0 /dev/dri/card0 /dev/galcore 2>&1
uname -a
cat /proc/cmdline
lsmod
v4l2-ctl -d /dev/video0 --list-formats-ext
gst-inspect-1.0 v4l2src | grep -E "io-mode|dmabuf|Device|Pad Templates" -A20
gst-inspect-1.0 kmssink
gst-inspect-1.0 waylandsink
gst-inspect-1.0 qmlglsink
gst-inspect-1.0 glupload
gst-inspect-1.0 glimagesink
```

如果某个命令不存在，把错误原文记录下来，不要猜。

也可以直接运行仓库提供的探测脚本，它会执行上述大部分只读采集，并短时验证 DMABUF/KMS 管线：

```bash
/root/qt_camera_display/probe_zero_copy_video_path.sh
```

如果本轮目标是恢复当前已确认可见的 `640x480` 低 CPU 画面，运行：

```bash
nohup gst-launch-1.0 -v \
  v4l2src device=/dev/video0 io-mode=mmap ! \
  'video/x-raw,format=YUY2,width=640,height=480,framerate=10/1' ! \
  videorate drop-only=true max-rate=6 skip-to-first=true silent=true ! \
  'video/x-raw,format=YUY2,width=640,height=480,framerate=6/1' ! \
  videoconvert dither=none chroma-mode=none matrix-mode=input-only chroma-resampler=nearest alpha-mode=set alpha-value=1 n-threads=1 qos=false ! \
  'video/x-raw,format=BGRA,width=640,height=480,framerate=6/1' ! \
  kmssink driver-name=stm sync=false async=false enable-last-sample=false qos=false show-preroll-frame=false processing-deadline=0 max-lateness=-1 render-rectangle='<0,0,1024,600>' \
  >/tmp/gst640-src10-drop6-bgra-fullrect.log 2>&1 < /dev/null &
```

这个模式保持 `640x480` 输入细节，但显示帧率约 `6fps`，全线程 CPU 观测约 `11.2%~13.8%`。如果需要恢复最低 CPU 的 `320x240` 保底画面，运行：

```bash
nohup gst-launch-1.0 -v \
  v4l2src device=/dev/video0 io-mode=mmap ! \
  'video/x-raw,format=YUY2,width=320,height=240,framerate=10/1' ! \
  videoconvert dither=none chroma-mode=none matrix-mode=input-only chroma-resampler=nearest n-threads=1 ! \
  'video/x-raw,format=BGRA,width=320,height=240,framerate=10/1' ! \
  kmssink driver-name=stm sync=false render-rectangle='<0,0,1024,600>' \
  >/tmp/gst-kms-320x240-10fps-n1-full.log 2>&1 < /dev/null &
```

这个模式只显示 `v4l2src mmap ! fast videoconvert ! BGRA ! kmssink` 的视频画面，不显示 Qt UI；它用 KMS 把 `320x240` 画面拉伸到 `1024x600` 全屏，CPU 低但细节少。

## 第二轮：独立验证视频 sink

### A. 验证 V4L2 DMABUF 能力

```bash
nohup gst-launch-1.0 -v v4l2src device=/dev/video0 io-mode=dmabuf ! \
  video/x-raw,format=YUY2,width=640,height=480,framerate=15/1 ! \
  fakesink sync=false \
  >/tmp/gst-dmabuf-fakesink.log 2>&1 < /dev/null &
```

| 结果 | 下一步 |
|---|---|
| 能跑 | 继续接 `waylandsink` 或 `kmssink` |
| 报 `io-mode` 不支持 | 改查 UVC/V4L2 驱动是否支持 DMABUF export/import |
| 报格式不支持 | 根据 `v4l2-ctl --list-formats-ext` 改 caps |

### B. 验证 KMS 视频显示

```bash
nohup gst-launch-1.0 -v v4l2src device=/dev/video0 io-mode=dmabuf ! \
  video/x-raw,format=YUY2,width=640,height=480,framerate=15/1 ! \
  kmssink sync=false \
  >/tmp/gst-kms-yuy2-direct.log 2>&1 < /dev/null &
```

如果 `kmssink` 抢占 LCD 或与 Qt EGLFS 冲突，先只验证单独视频显示是否 CPU 低，再决定是否做 plane 分层。

### C. 验证 Wayland 视频显示

需要先确认 Weston 或其它 compositor 能启动：

```bash
nohup weston --tty=1 --backend=drm-backend.so --idle-time=0 \
  >/tmp/weston-drm.log 2>&1 < /dev/null &
```

再测试：

```bash
export XDG_RUNTIME_DIR=/tmp/runtime-root
export WAYLAND_DISPLAY=wayland-0
nohup gst-launch-1.0 -v v4l2src device=/dev/video0 io-mode=dmabuf ! \
  video/x-raw,format=YUY2,width=640,height=480,framerate=15/1 ! \
  waylandsink sync=false \
  >/tmp/gst-waylandsink.log 2>&1 < /dev/null &
```

## 第三轮：测 CPU

每次测试都必须记录以下字段：

| 字段 | 示例 |
|---|---|
| 路线 | `v4l2src io-mode=dmabuf ! kmssink` |
| 分辨率 | `640x480` |
| 帧率 | `15fps` |
| CPU | `xx.x%` |
| 稳定性 | `运行 10 分钟，无 Oops` |
| 画质 | `可看清工件边缘/不能看清` |

CPU 测试命令：

```bash
pid=$(pidof gst-launch-1.0 | awk '{print $1}')
ncpu=$(grep -c '^cpu[0-9]' /proc/stat)
t1=$(awk '/^cpu /{s=0; for(i=2;i<=NF;i++)s+=$i; print s}' /proc/stat)
p1=$(awk '{s+=$14+$15} END{print s}' /proc/$pid/task/*/stat)
sleep 5
t2=$(awk '/^cpu /{s=0; for(i=2;i<=NF;i++)s+=$i; print s}' /proc/stat)
p2=$(awk '{s+=$14+$15} END{print s}' /proc/$pid/task/*/stat)
awk -v p1=$p1 -v p2=$p2 -v t1=$t1 -v t2=$t2 -v n=$ncpu 'BEGIN{printf("全线程平均CPU: %.1f%%\n", (p2-p1)*100*n/(t2-t1));}'
```

## 第四轮：与 Qt UI 集成

只有当独立视频 sink 达到目标后，再做 Qt 集成。

| 集成方式 | 条件 | 说明 |
|---|---|---|
| Wayland 双 surface | `waylandsink` 工作且 CPU 低 | Qt 用 `QT_QPA_PLATFORM=wayland-egl`，视频用 `waylandsink` |
| KMS plane | `kmssink` 能选择 plane 且不破坏 UI | Qt EGLFS 负责 UI，视频 plane 负责摄像头 |
| QML GL sink mmap | `VIDEO_BACKEND=qt-gst` 默认可显示 | 单进程集成已通，但 CPU 高 |
| QML GL sink dmabuf | 当前不可作为稳定方案 | 用户态段错误，core 指向 Vivante `libGAL` |

## 成功标准

| 标准 | 通过条件 |
|---|---|
| 画质 | `640x480@15fps` 或更高，明显好于 `320x240` fallback |
| CPU | 视频显示链路低于 `15%`，且系统整体仍流畅 |
| 稳定性 | 连续运行 10 分钟无 `galcore` Oops |
| 可集成性 | Qt UI 和视频能同时显示 |
| 可维护性 | 启动脚本、Buildroot/rootfs 依赖和文档同步更新 |

## 如果路线失败怎么判断

| 失败现象 | 判断 |
|---|---|
| 没有 `/dev/dri/card0` | KMS 路线暂不可用，需补 DRM/KMS 显示栈 |
| 没有 `waylandsink`/Weston | Wayland 路线需先补 rootfs 包 |
| `v4l2src io-mode=dmabuf` 不支持 | UVC 驱动或插件不支持 DMABUF，需考虑 mmap + 硬件 sink 或内核能力 |
| 再次出现 `galcore _UserMemoryAttach` Oops | 该路线仍进入 Vivante wrap user memory，停止使用 |
| CPU 仍约 `40%` | 该路线仍有 CPU 拷贝/转换，不满足最终目标 |

## 下一轮交付物

| 交付物 | 内容 |
|---|---|
| 测试记录表 | 每条管线的命令、CPU、画质、稳定性 |
| 路线结论 | 选择 Wayland、KMS、GL sink 或暂时回退 |
| rootfs 依赖清单 | 需要补哪些 GStreamer/Weston/DRM 库和插件 |
| 集成方案 | 如何让 Qt UI 和硬件视频显示共存 |
| 文档更新 | 更新本文件和 `zero_copy_hardware_video_plan.md` |

## 最重要的一句话

下一轮的目标不是“让摄像头能显示”，这个已经完成；下一轮的目标是“在不牺牲画质的情况下，把摄像头显示从 CPU 拷贝/纹理上传切换到稳定的零拷贝或硬件视频显示链路”。
