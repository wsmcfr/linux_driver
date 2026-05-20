# 作用：
#   这是 STM32MP157 Qt Quick 摄像头显示界面的 qmake 工程文件。
#   工程使用 Qt Quick 绘制 1024x600 工业检测界面，使用自定义 V4L2VideoItem
#   从 UVC 摄像头取 YUYV 预览流，并通过 OpenGL ES shader 显示到界面。

# QT 指定本程序依赖的 Qt 模块：
# quick/qml 负责 QML 界面和 Qt Quick Scene Graph；
# gui 提供 OpenGL ES/FBO/shader 相关类型；
# network 提供云端复核结果回写时使用的 QTcpServer/QTcpSocket。
QT += quick qml gui network

# QT_CONFIG -= no-pkg-config 避免 qmake 禁用 pkg-config，后续要通过它寻找 GStreamer。
QT_CONFIG -= no-pkg-config

# CONFIG 使用 C++11 和 release 构建，减少板端运行开销；
# link_pkgconfig 让 qmake 自动加入 GStreamer 头文件和链接参数。
CONFIG += c++11 release link_pkgconfig

# PKGCONFIG 列出 C++ 入口直接使用的 GStreamer 开发包。
# gstreamer-video-1.0 不是当前代码直接包含的头文件，但 qmlglsink/视频 caps 调试常与它一起部署。
PKGCONFIG += \
    gstreamer-1.0 \
    gstreamer-video-1.0

# TARGET 是交叉编译后拷贝到开发板运行的可执行文件名。
TARGET = qt_camera_display

# TEMPLATE=app 表示生成普通 Linux 用户态应用程序。
TEMPLATE = app

# SOURCES 列出 C++ 源文件，QML 文件通过 qml.qrc 编译进资源。
SOURCES += \
    main.cpp \
    v4l2_video_item.cpp

# HEADERS 列出自定义 QML Item 的头文件，方便 qmake/moc 生成元对象代码。
HEADERS += \
    v4l2_video_item.h

# RESOURCES 把 QML 主界面打包进二进制，部署时只需要复制一个程序和运行脚本。
RESOURCES += qml.qrc

# target.path 是执行 make install 时的目标安装目录，部署脚本也使用同样约定。
target.path = /root/qt_camera_display

# INSTALLS 让 qmake 知道 make install 需要安装目标程序。
INSTALLS += target
