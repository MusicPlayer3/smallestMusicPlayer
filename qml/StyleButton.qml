import QtQuick
import QtQuick.Controls

// 根对象：RoundButton
RoundButton {
    id: control

    // ------------------------------------------------
    //  修正后的属性定义，作为组件的外部接口
    // ------------------------------------------------
    property color baseColor: "#20FFFFFF" // 默认颜色（低透明度白）
    property color hoverColor: "#30FFFFFF" // 悬停时的颜色
    property color pressedColor: "#40FFFFFF" // 按下时的颜色

    // 允许外部定义按钮的宽度和高度
    property alias buttonWidth: control.width
    property alias buttonHeight: control.height

    property color checkedColor: "#60FFFFFF" // 选中(常亮)时的颜色

    // 用于接收字体家族名称
    property string iconFontFamily: ""

    // 允许外部定义按钮的文本（或图标）
    property alias buttonText: contentText.text

    // 允许外部设置圆角半径 (使用 Math.min 确保是圆形)
    property int buttonRadius: Math.min(control.width, control.height) / 2

    // 暴露 Text 的属性，这样外部就可以直接修改，而不需要覆盖 contentItem
    property alias textColor: contentText.color
    property alias textSize: contentText.font.pixelSize

    checkable: false // 使按钮在按下后保持在 "checked" 状态，直到再次按下, 如果你想用这个功能就true

    // ------------------------------------------------
    // 按钮的背景和高亮逻辑
    // ------------------------------------------------
    background: Rectangle {
        id: bgRect
        radius: control.buttonRadius
        width: control.width
        height: control.height
        //anchors.fill: parent

        // 动态颜色绑定到组件的属性
        color: control.baseColor

        // 使用 Behavior on color 实现平滑的高亮过渡
        Behavior on color {
            ColorAnimation { duration: 150 }
        }

        // 根据状态更新颜色
        states: [
            State {
                name: "checkedState"
                when: control.checked // 按钮处于“选中”状态时
                PropertyChanges {
                    target: bgRect;
                    color: control.checkedColor // 使用常亮色
                }
            },
            State {
                name: "hovered"
                when: control.hovered && !control.pressed && !control.checked
                PropertyChanges { target: bgRect; color: control.hoverColor }
            },
            State {
                name: "pressed"
                when: control.pressed && !control.checked
                PropertyChanges { target: bgRect; color: control.pressedColor }
            }
        ]
    }

    // ------------------------------------------------
    // 按钮内容（用于显示图标或文字）
    // ------------------------------------------------
    contentItem: Text {
        id: contentText
        text: control.buttonText
        color: "white"
        // 播放按钮需要更大的字体，这里设置一个默认值，可以在主文件中覆盖
        font.pixelSize: 20
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        anchors.centerIn: parent
        font.family: control.iconFontFamily
    }
}
