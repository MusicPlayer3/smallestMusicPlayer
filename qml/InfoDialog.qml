import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects

Window {
    id: infoWin
    width: 320
    height: 400
    visible: false
    title: "详细信息"
    flags: Qt.Dialog | Qt.WindowCloseButtonHint | Qt.CustomizeWindowHint
    modality: Qt.ApplicationModal
    color: "#2b2b2b"

    property var infoData: ({}) // 接收 C++ 返回的 Map

    function showInfo(data) {
        infoData = data;
        // 根据内容调整高度
        if (data.isFolder)
            height = 320;
        else
            height = 480;

        show();
        requestActivate();
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 15

        // 1. 封面
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            width: 120
            height: 120
            color: "#333"
            radius: 8
            clip: true

            Image {
                anchors.fill: parent
                source: infoData.cover || ""
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                mipmap: true
            }
            border.color: "#444"
            border.width: 1
        }

        // 2. 分隔线
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: "#40FFFFFF"
        }

        // 3. 信息列表
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            rowSpacing: 8
            columnSpacing: 10

            // 通用：名称
            Text {
                text: "名称:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                text: infoData.title || "-"
                color: "white"
                font.pixelSize: 13
                Layout.fillWidth: true
                elide: Text.ElideRight
            }

            // --- 文件夹特有 (使用 === true 避免 undefined 错误) ---
            Text {
                visible: infoData.isFolder === true
                text: "上级目录:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                visible: infoData.isFolder === true
                text: infoData.parentName || "-"
                color: "white"
                font.pixelSize: 13
                Layout.fillWidth: true
                elide: Text.ElideRight
            }

            Text {
                visible: infoData.isFolder === true
                text: "包含歌曲:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                visible: infoData.isFolder === true
                text: infoData.songCount || "0"
                color: "white"
                font.pixelSize: 13
            }

            Text {
                visible: infoData.isFolder === true
                text: "总时长:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                visible: infoData.isFolder === true
                text: infoData.totalDuration || "00:00"
                color: "white"
                font.pixelSize: 13
            }

            // --- 歌曲特有 (使用 === false 避免 undefined 错误) ---
            Text {
                visible: infoData.isFolder === false
                text: "艺术家:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                visible: infoData.isFolder === false
                text: infoData.artist || "-"
                color: "white"
                font.pixelSize: 13
                Layout.fillWidth: true
                elide: Text.ElideRight
            }

            Text {
                visible: infoData.isFolder === false
                text: "专辑:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                visible: infoData.isFolder === false
                text: infoData.album || "-"
                color: "white"
                font.pixelSize: 13
                Layout.fillWidth: true
                elide: Text.ElideRight
            }

            Text {
                visible: infoData.isFolder === false
                text: "年份:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                visible: infoData.isFolder === false
                text: infoData.year || "-"
                color: "white"
                font.pixelSize: 13
            }

            Text {
                visible: infoData.isFolder === false
                text: "格式:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                visible: infoData.isFolder === false
                text: (infoData.format || "") + " (" + (infoData.bitDepth || "") + " / " + (infoData.sampleRate || "") + ")"
                color: "white"
                font.pixelSize: 13
            }

            Text {
                visible: infoData.isFolder === false
                text: "播放次数:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                visible: infoData.isFolder === false
                text: infoData.playCount || "0"
                color: "white"
                font.pixelSize: 13
            }

            Text {
                visible: infoData.isFolder === false
                text: "评价星级:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Row {
                visible: infoData.isFolder === false
                spacing: 2
                Repeater {
                    model: 5
                    Text {
                        text: "star"
                        font.family: materialFont.name
                        // 这里也要防范 undefined
                        color: index < (infoData.rating || 0) ? "#FFD700" : "#555"
                        font.pixelSize: 14
                    }
                }
            }
        }

        Item {
            Layout.fillHeight: true
        } // 弹簧

        Button {
            Layout.alignment: Qt.AlignHCenter
            text: "关闭"
            onClicked: infoWin.close()
            contentItem: Text {
                text: parent.text
                color: "white"
                horizontalAlignment: Text.AlignHCenter
            }
            background: Rectangle {
                color: parent.pressed ? "#50FFFFFF" : "#30FFFFFF"
                radius: 4
            }
        }
    }
}
