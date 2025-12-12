import QtQuick
import Qt5Compat.GraphicalEffects

Item {
    id: root
    anchors.fill: parent

    // 接收三个主色调
    property color color0: "#2d2d2d"
    property color color1: "#2d2d2d"
    property color color2: "#2d2d2d"

    // 1. 基础底色 (不透明，确保遮挡下方内容)
    Rectangle {
        anchors.fill: parent
        color: "#1e1e1e" // 深灰色底板
    }

    // 2. 第一层渐变 (127deg: 左上 -> 右下)
    LinearGradient {
        anchors.fill: parent
        start: Qt.point(0, 0)
        end: Qt.point(width, height)
        gradient: Gradient {
            GradientStop {
                position: 0.0
                // 55% 不透明度的颜色
                color: Qt.rgba(root.color0.r, root.color0.g, root.color0.b, 0.55)
            }
            GradientStop {
                position: 0.7071 // 约 70% 处完全透明
                color: "transparent"
            }
        }
    }

    // 3. 第二层渐变 (217deg: 右上 -> 左下)
    LinearGradient {
        anchors.fill: parent
        start: Qt.point(width, 0)
        end: Qt.point(0, height)
        gradient: Gradient {
            GradientStop {
                position: 0.0
                color: Qt.rgba(root.color1.r, root.color1.g, root.color1.b, 0.55)
            }
            GradientStop {
                position: 0.7071
                color: "transparent"
            }
        }
    }

    // 4. 第三层渐变 (336deg: 左下 -> 右上)
    LinearGradient {
        anchors.fill: parent
        start: Qt.point(0, height)
        end: Qt.point(width, 0)
        gradient: Gradient {
            GradientStop {
                position: 0.0
                color: Qt.rgba(root.color2.r, root.color2.g, root.color2.b, 0.55)
            }
            GradientStop {
                position: 0.7071
                color: "transparent"
            }
        }
    }

    // 颜色变化动画
    Behavior on color0 {
        ColorAnimation {
            duration: 500
        }
    }
    Behavior on color1 {
        ColorAnimation {
            duration: 500
        }
    }
    Behavior on color2 {
        ColorAnimation {
            duration: 500
        }
    }
}
