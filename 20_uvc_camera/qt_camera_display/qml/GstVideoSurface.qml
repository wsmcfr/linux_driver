/*
 * GstVideoSurface.qml
 *
 * 作用：
 *   为 GStreamer qmlglsink 提供一个可绑定的 Qt Quick 视频表面。
 *   main.cpp 会在 QML 加载完成后查找 objectName 为 gstVideoItem 的对象，
 *   并把它设置到 qmlglsink 的 widget 属性上。
 */

import QtQuick 2.12
import org.freedesktop.gstreamer.GLVideoItem 1.0

GstGLVideoItem {
    id: gstVideoSurface

    /* objectName 是 C++ findChild 的稳定锚点，不能随意改名。 */
    objectName: "gstVideoItem"

    /* anchors.fill 由 Loader 的父项提供，保证视频铺满实时画面区。 */
    anchors.fill: parent
}
