import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects

Rectangle {

    id: musicListHeader
    color: '#003c3f46'
    height: 50
    
    property string iconFontFamily: ""
    signal closeRequested 

    // --- 1. 左侧按钮组 ---
    Row {
        id: leftButtons
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        spacing: 5
        property color baseColor: "#20FFFFFF" // 默认颜色（低透明度白）
        property color hoverColor: "#30FFFFFF" // 悬停时的颜色
        property color pressedColor: "#40FFFFFF" // 按下时的颜色

        Button {
            id: backButton
            background: Rectangle { color: "transparent" }
            Text {
                text: "undo" 
                font.pixelSize: 20
                color: "#CCCCCC"
                anchors.centerIn: parent
                font.family: musicListHeader.iconFontFamily
            }
            onClicked: {
                musicListModel.goBack(); 
                console.log("Back to parent");
            }
            width: 25
            height: 45
        }

        Button {
            id: searchButton
            background: Rectangle { color: "transparent" }
            Text {
                text: "search"
                font.pixelSize: 20
                color: "#CCCCCC"
                anchors.centerIn: parent
                font.family:musicListHeader.iconFontFamily
            }
            width: 30
            height: 45
        }
    }

    // --- 2. 中间标题 (绝对居中 + 宽度自适应) ---
    // 修复：不使用 RowLayout 挤压，而是直接锚定居中，并计算最大宽度
    MarqueeText {
        id: titleText
        anchors.centerIn: parent
        
        // 宽度逻辑：总宽度减去左右两侧按钮占据的空间（预留 ample space 防止覆盖）
        // 160 是估算的左右两边按钮加边距的总宽度 (40+30+10+margin) * 2
        width: parent.width - 180 
        
        text: musicListModel.currentDirName
        color: "white"
        font.pixelSize: 16
        font.bold: true
        
        // 居中对齐设置
        horizontalAlignment: Text.AlignHCenter 
    }

    // --- 3. 右侧按钮组 ---
    Row {
        id: rightButtons
        anchors.right: parent.right
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10

        Button {
            id: doneButton
            background: Rectangle { color: "transparent" }
            Text {
                text: "close"
                font.pixelSize: 20
                color: "#CCCCCC"
                anchors.centerIn: parent
                font.family:musicListHeader.iconFontFamily
            }
            width: 30
            height: 30
            onClicked: {
                closeRequested()
            }
        }
    }
}