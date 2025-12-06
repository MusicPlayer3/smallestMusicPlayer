import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects

Rectangle {
    id: musicListHeader
    color: '#003c3f46'
    
    // 动态高度控制
    height: isSearching ? 100 : 50
    Behavior on height { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }

    property string iconFontFamily: ""
    signal closeRequested 

    property bool isSearching: false

    layer.enabled: true
    layer.effect: OpacityMask {
        maskSource: Rectangle {
            width: musicListHeader.width
            height: musicListHeader.height
            color: "white"
        }
    }

    // 使用 Column 确保内部布局稳定
    Column {
        anchors.fill: parent

        // --- 第一行：标题栏 (高度固定 50) ---
        Item {
            width: parent.width
            height: 50
            
            // 左侧按钮
            Row {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                Button {
                    background: Rectangle { color: "transparent" }
                    Text {
                        text: "undo" 
                        font.pixelSize: 20
                        color: "#CCCCCC"
                        anchors.centerIn: parent
                        font.family: musicListHeader.iconFontFamily
                    }
                    onClicked: musicListModel.goBack()
                    width: 40; height: 40
                }

                Button {
                    id: searchButton
                    background: Rectangle { color: "transparent" }
                    Text {
                        text: isSearching ? "expand_less" : "search"
                        font.pixelSize: 20
                        color: isSearching ? "#66CCFF" : "#CCCCCC"
                        anchors.centerIn: parent
                        font.family: musicListHeader.iconFontFamily
                    }
                    onClicked: {
                        musicListHeader.isSearching = !musicListHeader.isSearching
                        if (musicListHeader.isSearching) {
                            searchInput.forceActiveFocus() 
                        } else {
                            // 收起时，先清空输入框，再触发空搜索以恢复列表
                            searchInput.text = "" 
                        }
                    }
                    width: 30; height: 40
                }
            }

            // 中间标题
            MarqueeText {
                anchors.centerIn: parent
                width: parent.width - 180 
                text: musicListModel.currentDirName
                color: "white"
                font.pixelSize: 16
                font.bold: true
                horizontalAlignment: Text.AlignHCenter 
            }

            // 右侧按钮
            Row {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10
                Button {
                    background: Rectangle { color: "transparent" }
                    Text {
                        text: "close"
                        font.pixelSize: 20
                        color: "#CCCCCC"
                        anchors.centerIn: parent
                        font.family: musicListHeader.iconFontFamily
                    }
                    width: 30; height: 30
                    onClicked: closeRequested()
                }
            }
        }

        // --- 第二行：搜索框 (高度固定 50) ---
        Item {
            width: parent.width
            height: 50
            visible: opacity > 0
            opacity: isSearching ? 1.0 : 0.0
            Behavior on opacity { NumberAnimation { duration: 200 } }
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
                            musicListModel.search(text)
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
                                searchInput.text = "" 
                            }
                        }
                    }
                }
            }
        }
    }
}