import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects
import QtQuick.Window

Rectangle {
    id: delegateRoot
    width: ListView.view ? ListView.view.width : 400
    // 保持之前修改后的高度，容纳三行信息
    height: 75

    color: mouseArea.pressed ? "#50555B" : (mouseArea.containsMouse ? "#40444A" : "transparent")

    property url itemImageSource: ""
    property string itemTitle: ""
    property string itemArtist: ""
    property string itemAlbumName: ""
    property string itemExtraInfo: ""
    property string itemParentDir: ""
    property bool isItemPlaying: false
    property string iconFontFamily: ""
    property bool isFolder: false

    // [新增] 信号：当用户点击右键时发出通知
    signal contextMenuRequested

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true

        // [修改] 同时接受左键(点击播放/进入)和右键(上下文菜单)
        acceptedButtons: Qt.LeftButton | Qt.RightButton

        onClicked: mouse => {
            if (mouse.button === Qt.LeftButton) {
                // 左键逻辑：原有的播放或进入文件夹
                musicListModel.handleClick(index);
            } else if (mouse.button === Qt.RightButton) {
                // 右键逻辑：发出信号，由外层 ListView 处理菜单弹出
                delegateRoot.contextMenuRequested();
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 12
        layoutDirection: Qt.LeftToRight

        // 1. 封面 / 图标区域
        Rectangle {
            id: albumRect
            Layout.preferredWidth: 50
            Layout.preferredHeight: 50
            Layout.alignment: Qt.AlignVCenter
            radius: 6
            color: "#2d2d2d"
            clip: true

            Image {
                id: albumCover
                anchors.fill: parent
                source: itemImageSource
                fillMode: Image.PreserveAspectCrop

                sourceSize: Qt.size(60 * Screen.devicePixelRatio, 60 * Screen.devicePixelRatio)

                asynchronous: true
                smooth: true
                mipmap: true

                layer.enabled: true
                layer.textureSize: Qt.size(width * Screen.devicePixelRatio, height * Screen.devicePixelRatio)

                layer.effect: OpacityMask {
                    anchors.fill: parent
                    maskSource: Rectangle {
                        width: albumCover.width
                        height: albumCover.height
                        radius: 6
                        color: "white"
                    }
                }
            }

            // 如果图片加载中或没有封面，显示默认图标
            Text {
                visible: albumCover.status !== Image.Ready
                anchors.centerIn: parent
                text: isFolder ? "folder" : "music_note"
                font.family: iconFontFamily
                color: "#888"
                font.pixelSize: 24
            }
        }

        // 2. 文本信息列
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 2

            // 第一行：标题 / 文件夹名
            Text {
                Layout.fillWidth: true
                text: itemTitle
                font.pixelSize: 15
                font.bold: true
                color: isItemPlaying ? "#66CCFF" : "white"
                horizontalAlignment: Text.AlignLeft

                // [修改]：不再滚动，而是过长显示省略号
                elide: Text.ElideRight
                maximumLineCount: 1
            }

            // 第二行：文件夹(上级目录) | 歌曲(歌手 - 专辑)
            Text {
                Layout.fillWidth: true
                text: isFolder ? itemParentDir : (itemArtist + (itemAlbumName ? (" - " + itemAlbumName) : ""))
                color: "#AAAAAA"
                font.pixelSize: 12
                horizontalAlignment: Text.AlignLeft

                // [修改]：省略号
                elide: Text.ElideRight
                maximumLineCount: 1
            }

            // 第三行：扩展信息 (时长、格式、数量等)
            Text {
                Layout.fillWidth: true
                text: itemExtraInfo
                color: "#777777"
                font.pixelSize: 11
                horizontalAlignment: Text.AlignLeft

                // [修改]：省略号
                elide: Text.ElideRight
                maximumLineCount: 1
            }
        }

        // 3. 正在播放指示器
        Text {
            visible: isItemPlaying
            text: "audiotrack"
            font.pixelSize: 20
            color: "#66CCFF"
            Layout.preferredWidth: 20
            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
            font.family: iconFontFamily
        }
    }
}
