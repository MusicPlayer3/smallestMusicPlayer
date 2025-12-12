import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects
import QtQuick.Window

Rectangle {
    id: musicListHeader
    color: '#003c3f46'
    property alias sortPopup: sortPopup // 添加这行
    property bool isSortPopupVisible: sortPopup.visible // 添加这行


    height: isSearching ? 100 : 50
    Behavior on height {
        NumberAnimation {
            duration: 250
            easing.type: Easing.OutCubic
        }
    }

    property string iconFontFamily: ""
    signal closeRequested

    property bool isSearching: false

    layer.enabled: true
    layer.textureSize: Qt.size(width * Screen.devicePixelRatio, height * Screen.devicePixelRatio)

    layer.effect: OpacityMask {
        maskSource: Rectangle {
            width: musicListHeader.width
            height: musicListHeader.height
            color: "white"
        }
    }

    Column {
        anchors.fill: parent

        // --- 第一行：标题栏 ---
        Item {
            width: parent.width
            height: 50

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                Button {
                    background: Rectangle {
                        color: "transparent"
                    }
                    Text {
                        text: "undo"
                        font.pixelSize: 20
                        color: "#CCCCCC"
                        anchors.centerIn: parent
                        font.family: musicListHeader.iconFontFamily
                    }
                    onClicked: musicListModel.goBack()
                    width: 20
                    height: 40
                }

                Button {
                    id: searchButton
                    background: Rectangle {
                        color: "transparent"
                    }
                    Text {
                        text: isSearching ? "expand_less" : "search"
                        font.pixelSize: 20
                        color: isSearching ? "#66CCFF" : "#CCCCCC"
                        anchors.centerIn: parent
                        font.family: musicListHeader.iconFontFamily
                    }
                    onClicked: {
                        musicListHeader.isSearching = !musicListHeader.isSearching;
                        if (musicListHeader.isSearching) {
                            searchInput.forceActiveFocus();
                        } else {
                            searchInput.text = "";
                        }
                    }
                    width: 20
                    height: 40
                }
            }

            MarqueeText {
                anchors.centerIn: parent
                // 减小宽度，避免遮挡右侧按钮
                width: parent.width - 140
                text: musicListModel.currentDirName
                color: "white"
                font.pixelSize: 16
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 5 // 间距微调

                // [新增] 更多选项按钮
                Button {
                    id: moreBtn
                    background: Rectangle {
                        color: "transparent"
                    }
                    Text {
                        text: "more_vert" // 请确保你的字体库包含此图标，或换成 menu
                        font.pixelSize: 20
                        color: "#CCCCCC"
                        anchors.centerIn: parent
                        font.family: musicListHeader.iconFontFamily
                    }
                    width: 20
                    height: 30
                    onClicked: {
                        sortPopup.toggle(moreBtn, 1); // 1表示箭头在右侧
                    }
                }

                Button {
                    background: Rectangle {
                        color: "transparent"
                    }
                    Text {
                        text: "close"
                        font.pixelSize: 20
                        color: "#CCCCCC"
                        anchors.centerIn: parent
                        font.family: musicListHeader.iconFontFamily
                    }
                    width: 30
                    height: 30
                    onClicked: closeRequested()
                }
            }
        }

        // --- 第二行：搜索框 ---
        Item {
            width: parent.width
            height: 50
            visible: opacity > 0
            opacity: isSearching ? 1.0 : 0.0
            Behavior on opacity {
                NumberAnimation {
                    duration: 200
                }
            }
            z: 10

            Rectangle {
                anchors.centerIn: parent
                width: parent.width - 20
                height: 36
                color: "#20FFFFFF"
                radius: 18

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10

                    Text {
                        text: "search"
                        font.family: musicListHeader.iconFontFamily
                        color: "#AAAAAA"
                        font.pixelSize: 18
                    }

                    TextField {
                        id: searchInput
                        Layout.fillWidth: true
                        background: null
                        color: "white"
                        font.pixelSize: 14
                        placeholderText: "搜索当前文件夹及子目录..."
                        placeholderTextColor: "#60FFFFFF"
                        selectByMouse: true
                        verticalAlignment: Text.AlignVCenter

                        onTextChanged: {
                            musicListModel.search(text);
                        }
                    }

                    Text {
                        text: "close"
                        font.family: musicListHeader.iconFontFamily
                        color: "#AAAAAA"
                        visible: searchInput.text.length > 0
                        font.pixelSize: 16
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                searchInput.text = "";
                            }
                        }
                    }
                }
            }
        }
    }

    // [新增] 排序气泡菜单
    BubblePopup {
        id: sortPopup
        width: 180
        // 使用 StackLayout 实现二级菜单
        content: StackLayout {
            id: menuStack
            currentIndex: 0
            width: 170

            // 1. 一级菜单
            Column {
                spacing: 0
                Repeater {
                    model: ["排序"]
                    delegate: Rectangle {
                        width: 170
                        height: 40
                        color: "transparent"
                        Text {
                            text: modelData
                            color: "white"
                            font.pixelSize: 14
                            anchors.left: parent.left
                            anchors.leftMargin: 15
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: "chevron_right"
                            font.family: musicListHeader.iconFontFamily
                            color: "#AAAAAA"
                            font.pixelSize: 18
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            onEntered: parent.color = "#30FFFFFF"
                            onExited: parent.color = "transparent"
                            onClicked: menuStack.currentIndex = 1 // 进入二级菜单
                        }
                    }
                }
            }

            // 2. 二级排序菜单
            Column {
                spacing: 0
                // 顶部返回条
                Rectangle {
                    width: 170
                    height: 35
                    color: "#20FFFFFF"
                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 5
                        spacing: 5
                        Text {
                            text: "arrow_back"
                            font.family: musicListHeader.iconFontFamily
                            color: "white"
                            font.pixelSize: 16
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: "返回"
                            color: "white"
                            font.pixelSize: 12
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: menuStack.currentIndex = 0
                    }
                }

                // 排序选项
                Repeater {
                    model: [
                        {
                            name: "根据标题",
                            type: 0
                        },
                        {
                            name: "根据文件名",
                            type: 1
                        },
                        {
                            name: "根据路径",
                            type: 2
                        },
                        {
                            name: "根据艺术家",
                            type: 3
                        },
                        {
                            name: "根据专辑",
                            type: 4
                        },
                        {
                            name: "根据年份",
                            type: 5
                        },
                        {
                            name: "根据持续时间",
                            type: 6
                        },
                        {
                            name: "根据日期",
                            type: 7
                        }
                    ]
                    delegate: Rectangle {
                        width: 170
                        height: 35
                        color: "transparent"
                        Text {
                            text: modelData.name
                            color: (musicListModel.sortType === modelData.type) ? "#66CCFF" : "white"
                            font.pixelSize: 13
                            anchors.left: parent.left
                            anchors.leftMargin: 15
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                musicListModel.setSortMode(modelData.type, musicListModel.sortReverse);
                                sortPopup.close();
                                // 重置回一级菜单，方便下次打开
                                menuStack.currentIndex = 0;
                            }
                        }
                    }
                }

                // 分割线
                Rectangle {
                    width: 170
                    height: 1
                    color: "#50FFFFFF"
                }

                // 倒序复选框
                Rectangle {
                    width: 170
                    height: 35
                    color: "transparent"
                    Row {
                        anchors.left: parent.left
                        anchors.leftMargin: 15
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 10
                        Text {
                            text: musicListModel.sortReverse ? "check_box" : "check_box_outline_blank"
                            font.family: musicListHeader.iconFontFamily
                            color: "white"
                            font.pixelSize: 16
                        }
                        Text {
                            text: "倒序"
                            color: "white"
                            font.pixelSize: 13
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            musicListModel.setSortMode(musicListModel.sortType, !musicListModel.sortReverse);
                            sortPopup.close();
                            menuStack.currentIndex = 0;
                        }
                    }
                }
            }
        }
    }
}
