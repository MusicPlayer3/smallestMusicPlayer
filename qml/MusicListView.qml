import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects

Rectangle {
    id: listViewRect
    color: "transparent"

    signal closeRequested
    signal addFolderRequested
    signal addFileRequested
    property string iconFamily: ""
    property alias sortPopup: header.sortPopup

    MouseArea {
        anchors.fill: parent
        // 拦截点击
    }

    // 1. 信息详情弹窗
    InfoDialog {
        id: infoDialog
    }

    // 2. 删除确认弹窗
    DeleteDialog {
        id: deleteDialog
        onConfirmDelete: (index, deletePhysicalFile) => {
            musicListModel.deleteItem(index, deletePhysicalFile);
        }
    }

    // 3. 右键菜单
    Menu {
        id: contextMenu
        property int targetIndex: -1
        property bool isTargetFolder: false
        property string targetTitle: ""

        background: Rectangle {
            implicitWidth: 160
            implicitHeight: 80
            color: "#2b2b2b"
            border.color: "#404040"
            radius: 4
        }

        MenuItem {
            text: contextMenu.isTargetFolder ? "文件夹信息" : "乐曲信息"
            contentItem: Text {
                text: parent.text
                color: "white"
                font.pixelSize: 14
                horizontalAlignment: Text.AlignLeft
                verticalAlignment: Text.AlignVCenter
                leftPadding: 12
            }
            background: Rectangle {
                color: parent.highlighted ? "#30FFFFFF" : "transparent"
                radius: 4
            }
            onTriggered: {
                var data = musicListModel.getDetailInfo(contextMenu.targetIndex);
                // 传入 index 以便回写
                infoDialog.showInfo(data, contextMenu.targetIndex);
            }
        }

        MenuItem {
            text: contextMenu.isTargetFolder ? "删除文件夹" : "删除歌曲"
            contentItem: Text {
                text: parent.text
                color: "#FF8A80"
                font.pixelSize: 14
                horizontalAlignment: Text.AlignLeft
                verticalAlignment: Text.AlignVCenter
                leftPadding: 12
            }
            background: Rectangle {
                color: parent.highlighted ? "#20FF5252" : "transparent"
                radius: 4
            }
            onTriggered: {
                deleteDialog.itemName = contextMenu.targetTitle;
                deleteDialog.isFolder = contextMenu.isTargetFolder;
                deleteDialog.targetIndex = contextMenu.targetIndex;
                deleteDialog.open();
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        MusicListHeader {
            id: header
            iconFontFamily: listViewRect.iconFamily
            Layout.fillWidth: true
            Layout.preferredHeight: height
            z: 2
            onCloseRequested: {
                listViewRect.closeRequested();
            }
            onAddFolderClicked: listViewRect.addFolderRequested()
            onAddFileClicked: listViewRect.addFileRequested()
        }

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

                onContextMenuRequested: {
                    contextMenu.targetIndex = index;
                    contextMenu.isTargetFolder = model.isFolder;
                    contextMenu.targetTitle = model.title;
                    contextMenu.popup();
                }
            }
        }
    }

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
