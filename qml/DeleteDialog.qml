import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    title: "删除确认"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 350

    // 自定义背景
    background: Rectangle {
        color: "#2b2b2b"
        radius: 8
        border.color: "#404040"
        border.width: 1
    }

    // 外部传入
    property string itemName: ""
    property bool isFolder: false
    property int targetIndex: -1

    // 信号
    signal confirmDelete(int index, bool deleteFile)

    // [修改] 这里将 Text 改为 Label，因为 Text 组件没有 background 属性
    header: Label {
        text: root.title
        color: "white"
        font.pixelSize: 18
        font.bold: true
        padding: 15
        horizontalAlignment: Text.AlignHCenter
        background: Rectangle {
            color: "transparent"
        } // 占位
    }

    contentItem: ColumnLayout {
        spacing: 15

        // 上部提示
        Text {
            Layout.fillWidth: true
            text: "是否一同删除文件？"
            color: "white"
            font.pixelSize: 16
            wrapMode: Text.Wrap
        }

        // 中部解释
        Text {
            Layout.fillWidth: true
            text: root.isFolder ? "警告：勾选下方选项将永久删除物理存储中的文件夹 \"" + root.itemName + "\" 及其所有内容！此操作不可恢复。" : "警告：勾选下方选项将永久删除物理存储中的文件 \"" + root.itemName + "\"！此操作不可恢复。"
            color: "#AAAAAA"
            font.pixelSize: 13
            wrapMode: Text.Wrap
        }

        // 复选框
        CheckBox {
            id: physicalCheck
            text: "同时删除物理文件"
            checked: false

            contentItem: Text {
                text: physicalCheck.text
                font: physicalCheck.font
                color: "white" // 强制文字白色
                verticalAlignment: Text.AlignVCenter
                leftPadding: physicalCheck.indicator.width + physicalCheck.spacing
            }
        }
    }

    footer: DialogButtonBox {
        background: Rectangle {
            color: "transparent"
        }

        Button {
            text: "取消"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            flat: true
            contentItem: Text {
                text: parent.text
                color: "white"
                horizontalAlignment: Text.AlignHCenter
            }
            background: Rectangle {
                color: parent.hovered ? "#30FFFFFF" : "transparent"
                radius: 4
            }
        }
        Button {
            text: "删除"
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            flat: true
            contentItem: Text {
                text: parent.text
                color: "#FF5252"
                horizontalAlignment: Text.AlignHCenter
                font.bold: true
            }
            background: Rectangle {
                color: parent.hovered ? "#20FF5252" : "transparent"
                radius: 4
            }
        }

        onAccepted: {
            root.confirmDelete(root.targetIndex, physicalCheck.checked);
            physicalCheck.checked = false; // 重置
        }
    }
}
