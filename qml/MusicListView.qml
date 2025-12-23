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
        // 拦截点击，防止穿透
    }

    // ==========================================
    // [新增] 弹窗与菜单组件
    // ==========================================

    // 1. 信息详情弹窗
    InfoDialog {
        id: infoDialog
    }

    // 2. 删除确认弹窗
    DeleteDialog {
        id: deleteDialog
        // 当用户点击弹窗中的"删除"时触发
        onConfirmDelete: (index, deletePhysicalFile) => {
            // 调用 C++ Model 接口执行删除
            musicListModel.deleteItem(index, deletePhysicalFile);
        }
    }

    // 3. 右键菜单
    Menu {
        id: contextMenu
        // 临时存储当前右键选中的条目信息
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

        // 菜单项 1: 信息
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
                // 调用 C++ 获取详细信息 Map
                var data = musicListModel.getDetailInfo(contextMenu.targetIndex);
                // 显示弹窗
                infoDialog.showInfo(data);
            }
        }

        // 菜单项 2: 删除
        MenuItem {
            text: contextMenu.isTargetFolder ? "删除文件夹" : "删除歌曲"
            contentItem: Text {
                text: parent.text
                // 红色字体示警
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
                // 设置删除弹窗的属性并打开
                deleteDialog.itemName = contextMenu.targetTitle;
                deleteDialog.isFolder = contextMenu.isTargetFolder;
                deleteDialog.targetIndex = contextMenu.targetIndex;
                deleteDialog.open();
            }
        }
    }

    // ==========================================
    // 主布局
    // ==========================================

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

                // [新增] 处理 Delegate 发出的右键信号
                onContextMenuRequested: {
                    // 1. 记录被点击项的信息到菜单属性中
                    contextMenu.targetIndex = index;
                    contextMenu.isTargetFolder = model.isFolder;
                    contextMenu.targetTitle = model.title;

                    // 2. 在鼠标位置弹出菜单
                    contextMenu.popup();
                }
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
