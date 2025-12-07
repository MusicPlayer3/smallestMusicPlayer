import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects
import QtQuick.Window // [新增]

Rectangle {
    id: musicListHeader
    color: '#003c3f46'
    
    height: isSearching ? 100 : 50
    Behavior on height { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }

    property string iconFontFamily: ""
    signal closeRequested 

    property bool isSearching: false

    layer.enabled: true
    // [优化]：修复 Header 遮罩边缘模糊
    layer.textureSize: Qt.size(width * Screen.devicePixelRatio, height * Screen.devicePixelRatio)

    layer.effect: OpacityMask {
        maskSource: Rectangle {
            width: musicListHeader.width
            height: musicListHeader.height
            color: "white"
        }
    }

    // ... 内部 Column 内容保持不变，因为我们只修改了上面的 layer 属性 ...
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
                            searchInput.text = "" 
                        }
                    }
                    width: 30; height: 30
                }
            }

            MarqueeText {
                anchors.centerIn: parent
                width: parent.width - 180 
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

        // --- 第二行：搜索框 ---
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