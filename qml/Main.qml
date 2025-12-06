import QtQuick
import QtQuick.Controls
import Qt5Compat.GraphicalEffects
import QtQuick.Effects
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Window

import "./"

ApplicationWindow {
    id: window
    visible: true
    width: 400
    height: 700
    minimumWidth: 400 
    minimumHeight: 700 
    title: "Music Player"
    color: "transparent"
    flags: Qt.FramelessWindowHint | Qt.Window

    // =========================================================================
    // 逻辑控制属性
    // =========================================================================
    
    readonly property int sidebarWidth: 350
    readonly property int playerMinWidth: 450
    
    // 判断当前窗口宽度是否足以容纳分屏
    readonly property bool isDockCapable: window.width >= (sidebarWidth + playerMinWidth)

    // 控制侧边栏是否展开
    property bool isSidebarOpen: false

    // 监听分屏能力变化，实现“自动弹出”
    onIsDockCapableChanged: {
        // 当窗口拉大到足以分屏时，自动展开播放列表
        if (isDockCapable) {
            isSidebarOpen = true
        }
        // 当窗口缩小到不足以分屏时，您可以选择自动收起，或者保持原状（变成悬浮遮挡模式）
        // 这里选择保持原状，由用户决定是否关闭，或者如下行代码自动关闭：
        // else { isSidebarOpen = false } 
    }

    Connections {
        target: playerController
        function onScanCompleted() {
            console.log("C++ Scan Completed, loading root data.");
            musicListModel.loadRoot(); 
        }
        function onSongTitleChanged() {
            musicListModel.refreshPlayingState()
        }
    }

    FontLoader {
        id: materialFont
        source: "qrc:/MaterialIcons-Regular.ttf"
    }

    // =========================================================================
    // 1. 全局背景
    // =========================================================================
    Rectangle {
        id: background
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: playerController.gradientColor1 } 
            GradientStop { position: 0.5; color: playerController.gradientColor2 } 
            GradientStop { position: 1.0; color: playerController.gradientColor3 } 
        }
        radius: isMaximized ? 0 : 10
        property bool isMaximized: window.visibility === Window.Maximized

        // 顶部拖拽区域
        MouseArea {
            height: 50
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            onPressed: window.startSystemMove()
        }
    }

    // =========================================================================
    // 2. 窗口调整大小手柄 (修复 Bug 1)
    // =========================================================================
    Item {
        anchors.fill: parent
        visible: window.visibility !== Window.Maximized
        z: 1000 // 确保在最上层

        component ResizeArea : MouseArea {
            property int edgeFlag
            cursorShape: {
                switch(edgeFlag) {
                    case (Qt.TopEdge | Qt.LeftEdge): return Qt.SizeFDiagCursor;
                    case (Qt.BottomEdge | Qt.RightEdge): return Qt.SizeFDiagCursor;
                    case (Qt.TopEdge | Qt.RightEdge): return Qt.SizeBDiagCursor;
                    case (Qt.BottomEdge | Qt.LeftEdge): return Qt.SizeBDiagCursor;
                    case Qt.TopEdge: return Qt.SizeVerCursor;
                    case Qt.BottomEdge: return Qt.SizeVerCursor;
                    case Qt.LeftEdge: return Qt.SizeHorCursor;
                    case Qt.RightEdge: return Qt.SizeHorCursor;
                    default: return Qt.ArrowCursor;
                }
            }
            onPressed: window.startSystemResize(edgeFlag)
        }

        // 边缘 (5px)
        ResizeArea { anchors { left: parent.left; top: parent.top; bottom: parent.bottom; topMargin: 10; bottomMargin: 10 } width: 5; edgeFlag: Qt.LeftEdge }
        ResizeArea { anchors { right: parent.right; top: parent.top; bottom: parent.bottom; topMargin: 10; bottomMargin: 10 } width: 5; edgeFlag: Qt.RightEdge }
        ResizeArea { anchors { top: parent.top; left: parent.left; right: parent.right; leftMargin: 10; rightMargin: 10 } height: 5; edgeFlag: Qt.TopEdge }
        ResizeArea { anchors { bottom: parent.bottom; left: parent.left; right: parent.right; leftMargin: 10; rightMargin: 10 } height: 5; edgeFlag: Qt.BottomEdge }

        // 角落 (10x10) - 使用位运算组合 Edge
        ResizeArea { anchors { top: parent.top; left: parent.left } width: 10; height: 10; edgeFlag: Qt.TopEdge | Qt.LeftEdge }
        ResizeArea { anchors { top: parent.top; right: parent.right } width: 10; height: 10; edgeFlag: Qt.TopEdge | Qt.RightEdge }
        ResizeArea { anchors { bottom: parent.bottom; left: parent.left } width: 10; height: 10; edgeFlag: Qt.BottomEdge | Qt.LeftEdge }
        ResizeArea { anchors { bottom: parent.bottom; right: parent.right } width: 10; height: 10; edgeFlag: Qt.BottomEdge | Qt.RightEdge }
    }

    Component.onCompleted: folderDialog.open()
    
    FolderDialog {
        id: folderDialog
        title: "选择音乐文件夹"
        onAccepted: {
            var urlObject = new URL(folderDialog.selectedFolder);
            var folderPath = urlObject.pathname;
            if (Qt.platform.os === "windows" && folderPath.startsWith("/")) {
                folderPath = folderPath.substring(1);
            }
            if(folderPath) playerController.startMediaScan(folderPath)
        }
    }

    // =========================================================================
    // 3. 侧边栏 (播放列表) - 修复 Bug 4 (层级) & Bug 5 (收起)
    // =========================================================================
    Item {
        id: sidebarContainer
        width: sidebarWidth
        height: window.height
        
        // 修复 Bug 4: 设置 Z 轴为 500，确保高于窗口右上角的控制按钮 (Z=200)
        // 这样当侧边栏弹出时，会盖住右上角的按钮，避免点击冲突
        z: 500 
        
        // 位置逻辑：由 isSidebarOpen 唯一决定
        x: isSidebarOpen ? 0 : -width

        Behavior on x {
            NumberAnimation { duration: 300; easing.type: Easing.OutCubic }
        }

        // 侧边栏背景阴影 (仅在悬浮覆盖模式下显示，即开启但非分屏状态)
        RectangularGlow {
            anchors.fill: contentRect
            glowRadius: 10
            spread: 0.2
            color: "#80000000"
            // 当不是分屏模式(悬浮)且打开时显示阴影
            visible: !isDockCapable && isSidebarOpen
        }

        Rectangle {
            id: contentRect
            anchors.fill: parent
            color: "#2B2E33" 
            
            MusicListView {
                iconFamily: materialFont.name
                anchors.fill: parent
                onCloseRequested: {
                    window.isSidebarOpen = false 
                }
            }
        }
    }
    
    // 点击遮罩 (仅在悬浮模式有效)
    MouseArea {
        anchors.fill: parent
        z: 499 // 比侧边栏低一点，但比播放器高
        // 只有在悬浮模式(非 Dock 模式)且侧边栏打开时，才启用遮罩，点击空白处关闭
        visible: !isDockCapable && isSidebarOpen
        onClicked: window.isSidebarOpen = false
    }

    // =========================================================================
    // 4. 播放器主界面
    // =========================================================================
    Item {
        id: playerContainer
        height: window.height
        
        // 只有当具备分屏能力 且 侧边栏打开时，才进行位置偏移
        // 否则(即使侧边栏打开但窗口很小)，播放器保持全屏(被遮挡)，x=0
        property bool isDocked: isDockCapable && isSidebarOpen

        x: isDocked ? sidebarWidth : 0
        width: isDocked ? (window.width - sidebarWidth) : window.width

        Behavior on x { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
        Behavior on width { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

        ColumnLayout {
            id: mainColumnLayout
            
            width: Math.min(parent.width - 40, 400) 
            height: parent.height - 40 // 留出上下边距
            
            anchors.centerIn: parent 
            spacing: 0 // 手动控制 spacing

            // --- 顶部弹性填充 (修复 Bug 3: 确保垂直居中) ---
            Item { Layout.fillHeight: true }

            // --- 2. 封面控件 ---
            Item {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: Math.min(256, parent.width * 0.7)
                Layout.preferredHeight: Layout.preferredWidth 

                RectangularGlow {
                    anchors.fill: coverRect
                    glowRadius: 10
                    spread: 0.1
                    color: "#80000000"
                    cornerRadius: 20
                    z:-1
                }

                Rectangle {
                    id: coverRect
                    anchors.fill: parent
                    radius: 20
                    color: "#dddddd"
                    clip: true

                    Image {
                        id: bigPNG
                        anchors.fill: parent
                        source: playerController.coverArtSource 
                        fillMode: Image.PreserveAspectCrop
                        sourceSize: Qt.size(800, 800)
                        smooth: true 
                        mipmap: true 
                        layer.enabled: true
                        layer.textureSize: Qt.size(width * Math.max(2, Screen.devicePixelRatio), height * Math.max(2, Screen.devicePixelRatio))
                        layer.samples: 4 
                        layer.effect: OpacityMask{
                            anchors.fill: parent
                            maskSource: Rectangle {
                                width: bigPNG.width
                                height: bigPNG.height
                                radius: 20      
                                color: "white"  
                            }
                        }
                    }
                }
            }

            // --- 间隔 (修复 Bug 2: 增加封面与进度条距离) ---
            Item { 
                Layout.fillWidth: true
                Layout.preferredHeight: 40 // 强制留出 40px 的空间
            } 

            // --- 3. 进度条区域 ---
            ColumnLayout{
                Layout.fillWidth: true
                Layout.leftMargin: 10
                Layout.rightMargin: 10
                spacing: 5
                
                Slider {
                    id: progressSlider
                    Layout.fillWidth: true
                    from: 0
                    to: playerController.totalDurationMicrosec 
                    Binding {
                        target: progressSlider
                        property: "value"
                        value: playerController.currentPosMicrosec
                        when: !progressSlider.pressed
                    }
                    onPressedChanged: {
                        if (!pressed) {
                            playerController.seek(value);
                            playerController.setIsSeeking(false)
                        } else {
                            playerController.setIsSeeking(true)
                        }
                    }                
                    background: Rectangle {
                        x: progressSlider.leftPadding
                        y: progressSlider.topPadding + progressSlider.availableHeight / 2 - height / 2
                        width: progressSlider.availableWidth
                        height: 4
                        radius: 2
                        color: "#50FFFFFF"
                        Rectangle {
                            width: progressSlider.visualPosition * parent.width
                            height: parent.height
                            color: "white"
                            radius: 2
                        }
                    }
                    handle: Item {}
                }

                RowLayout{
                    Layout.fillWidth: true 
                    Text {
                        text: playerController.currentPosText
                        color: "white"
                        font.pixelSize: 12
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        id: remainingDurationText
                        text: playerController.remainingTimeText 
                        color: "white"
                        font.pixelSize: 12 
                    }
                }
            }

            Item { Layout.fillWidth: true; Layout.preferredHeight: 15 } // 间隔

            // --- 4. 文本信息区域 ---
            Column {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignHCenter
                spacing: 5

                MarqueeText {
                    width: parent.width * 0.95
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: playerController.songTitle
                    color: "white"
                    font.pixelSize: 22
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter 
                }

                MarqueeText {
                    width: parent.width * 0.95 
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: playerController.artistName
                    color: "#DDDDDD"
                    font.pixelSize: 16
                    horizontalAlignment: Text.AlignHCenter
                }

                MarqueeText {
                    width: parent.width * 0.95
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: playerController.albumName
                    color: "#AAAAAA"
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                }
            }

            Item { Layout.fillWidth: true; Layout.preferredHeight: 25 } // 间隔

            // --- 5. 播放控制 ---
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 30
                StyleButton {
                    Layout.preferredWidth: 40; Layout.preferredHeight: 40
                    buttonText: "skip_previous"
                    iconFontFamily: materialFont.name
                    textSize: 30; textColor: "white"
                    onClicked: playerController.prev()
                }
                StyleButton {
                    Layout.preferredWidth: 60; Layout.preferredHeight: 60
                    buttonText: playerController.isPlaying ? "pause" : "play_arrow" 
                    baseColor: "#40FFFFFF"; hoverColor: "#60FFFFFF"; pressedColor: "#90FFFFFF"
                    iconFontFamily: materialFont.name
                    textSize: 40; textColor: "white"
                    onClicked: playerController.playpluse()
                }
                StyleButton {
                    Layout.preferredWidth: 40; Layout.preferredHeight: 40
                    buttonText: "skip_next"
                    iconFontFamily: materialFont.name
                    textSize: 30; textColor: "white"
                    onClicked: playerController.next()
                }
            }

            Item { Layout.fillWidth: true; Layout.preferredHeight: 15 } // 间隔

            // --- 6. 音量控制 ---
            RowLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignHCenter
                spacing: 5
                Text { text: "volume_mute"; color: "#CCC"; font.pixelSize: 18 ; font.family: materialFont.name}
                Slider {
                    id: volumeSlider
                    Layout.fillWidth: true
                    Layout.maximumWidth: 200 
                    from: 0.0; to: 1.0
                    value: playerController.volume
                    onValueChanged: playerController.setVolume(value)
                    background: Rectangle {
                        x: volumeSlider.leftPadding
                        y: volumeSlider.topPadding + volumeSlider.availableHeight / 2 - height / 2
                        width: volumeSlider.availableWidth
                        height: 12; radius: 6; color: "#40000000" 
                        Rectangle {
                            width: volumeSlider.visualPosition * parent.width
                            height: parent.height; color: "#C0FFFFFF"; radius: 6
                        }
                    }
                    handle: Item {}
                }
                Text { text: "volume_up"; color: "#CCC"; font.pixelSize: 18 ; font.family: materialFont.name}
            }

            Item { Layout.fillWidth: true; Layout.preferredHeight: 30 } // 间隔

            // --- 7. 底部按钮组 ---
            RowLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignHCenter
                spacing: 40 
                
                Row {
                    spacing: 15
                    StyleButton {
                        width: 40; height: 40
                        buttonText: "view_sidebar"
                        iconFontFamily: materialFont.name
                        textSize: 18; textColor: "white"
                        
                        // 逻辑修改：纯粹作为一个 Toggle 开关
                        // 无论是在 Dock 模式还是悬浮模式，都可以通过这个按钮收起/展开列表
                        checkable: true
                        checked: isSidebarOpen
                        onClicked: {
                            window.isSidebarOpen = !window.isSidebarOpen
                        }
                    }
                    StyleButton {
                        width: 40; height: 40
                        buttonText: "shuffle"
                        iconFontFamily: materialFont.name
                        textSize: 18; textColor: "white"
                        onClicked: playerController.setShuffle(!playerController.isShuffle)
                        checkable: true
                        checked: playerController.isShuffle
                    }
                }

                Item { Layout.fillWidth: true; Layout.maximumWidth: 100 }

                Row {
                    spacing: 15
                    StyleButton {
                        id: stylePlayBtn
                        buttonText: playerController.repeatMode === 1 ? "repeat" : 
                                    playerController.repeatMode === 2 ? "repeat_one" : 
                                    "import_export" 
                        width: 40; height: 40
                        iconFontFamily: materialFont.name
                        textSize: 18; textColor: "white"
                        onClicked: playerController.toggleRepeatMode();
                    }
                    StyleButton {
                        width: 40; height: 40
                        buttonText: "more_vert" 
                        iconFontFamily: materialFont.name
                        textSize: 18; textColor: "white"
                        onClicked: console.log("More Clicked") 
                    }
                }
            }
            
            // --- 底部弹性填充 (确保垂直居中) ---
            Item { Layout.fillHeight: true }
        }
    }

    // =========================================================================
    // 5. 顶部标题栏按钮
    // =========================================================================
    Row {
        id: titleBarbuttons
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 10 
        anchors.rightMargin: 10 
        spacing: 8
        // Z轴 200。侧边栏弹出时 Z=500 会覆盖此区域
        z: 200 

        Repeater {
            model: ["minimize", "fullscreen", "close"]
            delegate: Button {
                id: btn
                text: modelData
                width: 30
                height: 30
                font.family: materialFont.name
                property bool isMaxWindow: false
                background: Rectangle {
                    radius: 15
                    color: btn.pressed ? "#80FFFFFF" : (btn.hovered ? "#60FFFFFF" : "#40FFFFFF")
                    Behavior on color { ColorAnimation { duration: 150 } }
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
                   switch(index){
                        case 0: window.showMinimized(); break;
                        case 1: 
                            if(isMaxWindow) window.showNormal(); 
                            else window.showMaximized();
                            isMaxWindow = !isMaxWindow
                            break;
                        case 2: Qt.quit(); break;
                    }
                }
            }
        }
    }
}