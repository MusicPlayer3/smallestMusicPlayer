import QtQuick
import QtQuick.Controls
import Qt5Compat.GraphicalEffects
import QtQuick.Effects
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Window

Item {
    id: root
    signal restorePlaylistRequested()
    signal addFolderRequested()

    MouseArea {
        anchors.fill: parent
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        onPressed: window.startSystemMove()
    }

    Row {
        id: titleBarbuttons
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 10
        anchors.rightMargin: 10
        spacing: 8
        z: 200
        Repeater {
            model: ["minimize", "fullscreen", "close"]
            delegate: Button {
                id: btn
                text: modelData
                width: 25
                height: 25
                font.family: materialFont.name
                property bool isMaxWindow: false
                background: Rectangle {
                    radius: 15
                    color: btn.pressed ? "#80FFFFFF" : (btn.hovered ? "#60FFFFFF" : "#40FFFFFF")
                    Behavior on color {
                        ColorAnimation {
                            duration: 150
                        }
                    }
                }
                contentItem: Text {
                    text: btn.text
                    color: "white"
                    font.pixelSize: 15
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.weight: Font.Black
                    font.family: materialFont.name
                }
                onClicked: {
                    switch (index) {
                    case 0:
                        window.showMinimized();
                        break;
                    case 1:
                        if (isMaxWindow)
                            window.showNormal();
                        else
                            window.showMaximized();
                        isMaxWindow = !isMaxWindow;
                        break;
                    case 2:
                        Qt.quit();
                        break;
                    }
                }
                SharedToolTip {
                    text: {
                        switch (index) {
                        case 0:
                            return "最小化";
                        case 1:
                            return isMaxWindow ? "还原" : "最大化";
                        case 2:
                            return "关闭";
                        }
                    }
                }
            }
        }
    }

    component SharedToolTip: ToolTip {
        id: toolTipControl
        property bool suppressed: false
        visible: parent.hovered && !suppressed
        delay: 500
        Connections {
            target: parent
            function onPressedChanged() {
                if (parent.pressed) {
                    toolTipControl.suppressed = true;
                }
            }
            function onHoveredChanged() {
                if (!parent.hovered) {
                    toolTipControl.suppressed = false;
                }
            }
        }
        contentItem: Text {
            text: toolTipControl.text
            color: "#FFFFFF"
            font.pixelSize: 12
            leftPadding: 4
            rightPadding: 4
        }
        background: Rectangle {
            color: "#1A1A1A"
            radius: 4
            opacity: 0.95
            border.color: "#333333"
            border.width: 1
        }
        enter: Transition {
            NumberAnimation {
                property: "opacity"
                from: 0.0
                to: 1.0
                duration: 200
            }
        }
        exit: Transition {
            NumberAnimation {
                property: "opacity"
                from: 1.0
                to: 0.0
                duration: 200
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#121212"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 28
        spacing: 18

        Item { Layout.fillHeight: true } // top spacer

        // Logo
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            width: 140
            height: 140
            radius: width / 2
            color: "#0E72C6"
            Rectangle {
                anchors.centerIn: parent
                width: 110
                height: 110
                radius: width / 2
                color: "white"
                Text {
                    anchors.centerIn: parent
                    text: "▶"
                    color: "#0E72C6"
                    font.pixelSize: 56
                }
            }
        }

        Text {
            Layout.alignment: Qt.AlignHCenter
            text: "MyGO!!!!!"
            color: "white"
            font.pixelSize: 34
            font.bold: true
        }

        Text {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: parent.width - 20
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: "选择恢复播放列表，或添加一个音乐文件夹。完成此页操作后才会进入主播放器。"
            color: "#BFFFFFFF"
            font.pixelSize: 14
        }

        Item { height: 12 }

        Button {
            id: restorePlaylistButton
            Layout.fillWidth: true
            text: "恢复播放列表"
            onClicked: root.restorePlaylistRequested()
        }

        Button {
            id: addFolderButton
            Layout.fillWidth: true
            text: "添加文件夹"
            onClicked: root.addFolderRequested()
        }

        Item { Layout.fillHeight: true } // bottom spacer
    }
}