import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects

Rectangle {
    id: listViewRect
    color: "transparent"

    signal closeRequested
    signal addFolderRequested()
    signal addFileRequested()
    property string iconFamily: ""
    property alias sortPopup: header.sortPopup

    MouseArea {
        anchors.fill: parent
        // 拦截点击，防止穿透
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- Header ---
        MusicListHeader {
            id: header
            iconFontFamily: listViewRect.iconFamily
            Layout.fillWidth: true
            Layout.preferredHeight: height
            z: 2
            onCloseRequested: {
                listViewRect.closeRequested();
            }
            // 2. 接收 Header 信号并向上传递
            onAddFolderClicked: listViewRect.addFolderRequested()
            onAddFileClicked: listViewRect.addFileRequested()
        }

        // --- ListView ---
        ListView {
            id: musicListView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            z: 1
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }
            model: musicListModel
            Connections {
                target: musicListModel
                function onRequestScrollTo(index) {
                    musicListView.positionViewAtIndex(index, ListView.Beginning);
                }
            }
            delegate: MusicListItemDelegate {
                itemImageSource: model.imageSource
                itemTitle: model.title
                itemArtist: model.artist
                itemAlbumName: model.albumName
                itemExtraInfo: model.extraInfo
                itemParentDir: model.parentDirName
                isItemPlaying: model.isPlaying
                iconFontFamily: listViewRect.iconFamily
                isFolder: model.isFolder
            }
        }
    }

    // 定位按钮
    Button {
        id: locateFab
        width: 44
        height: 44
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 20
        z: 100
        background: Rectangle {
            color: "#30FFFFFF"
            radius: 22
            border.color: "#50FFFFFF"
            border.width: 1
            Rectangle {
                anchors.fill: parent
                radius: 22
                color: parent.parent.hovered ? "#30FFFFFF" : "transparent"
            }
        }
        contentItem: Text {
            text: "my_location"
            font.family: listViewRect.iconFamily
            color: "white"
            font.pixelSize: 22
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        ToolTip.visible: hovered
        ToolTip.text: "定位当前播放"
        ToolTip.delay: 500
        onClicked: {
            musicListModel.locateCurrentPlaying();
        }
    }
}
