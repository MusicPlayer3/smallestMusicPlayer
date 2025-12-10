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
    height: 750
    minimumWidth: 400
    minimumHeight: 750
    title: "Music Player"
    color: "transparent"
    flags: Qt.FramelessWindowHint | Qt.Window

    // =========================================================================
    // 逻辑控制属性
    // =========================================================================

    readonly property int sidebarWidth: 350
    readonly property int playerMinWidth: 450
    readonly property bool isDockCapable: window.width >= (sidebarWidth + playerMinWidth)
    property bool isSidebarOpen: false

    // [控制 1] 全局布局动画开关
    property bool layoutAnimEnabled: true

    // [控制 2] 手动切换标识
    property bool isManualSidebarToggle: false

    // 窗口尺寸变化后的动画恢复定时器
    Timer {
        id: animResetTimer
        interval: 50
        onTriggered: window.layoutAnimEnabled = true
    }

    // 手动动画状态重置定时器
    Timer {
        id: manualAnimResetTimer
        interval: 310 // 比动画时间稍长
        onTriggered: window.isManualSidebarToggle = false
    }

    onIsDockCapableChanged: {
        layoutAnimEnabled = false;
        isManualSidebarToggle = false;

        if (isDockCapable)
            isSidebarOpen = true;
        else
            isSidebarOpen = false;

        animResetTimer.restart();
    }

    Connections {
        target: playerController
        function onScanCompleted() {
            musicListModel.loadRoot();
        }
        function onSongTitleChanged() {
            musicListModel.refreshPlayingState();
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
            GradientStop {
                position: 0.0
                color: playerController.gradientColor1
                Behavior on color {
                    ColorAnimation {
                        duration: 500
                    }
                }
            }
            GradientStop {
                position: 0.5
                color: playerController.gradientColor2
                Behavior on color {
                    ColorAnimation {
                        duration: 500
                    }
                }
            }
            GradientStop {
                position: 1.0
                color: playerController.gradientColor3
                Behavior on color {
                    ColorAnimation {
                        duration: 500
                    }
                }
            }
        }
        radius: isMaximized ? 0 : 10
        property bool isMaximized: window.visibility === Window.Maximized

        MouseArea {
            anchors.fill: parent
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            onPressed: window.startSystemMove()
        }
    }

    // =========================================================================
    // 2. 窗口调整大小手柄
    // =========================================================================
    Item {
        anchors.fill: parent
        visible: window.visibility !== Window.Maximized
        z: 1000

        ResizeArea {
            anchors {
                left: parent.left
                top: parent.top
                bottom: parent.bottom
                topMargin: 10
                bottomMargin: 10
            }
            width: 5
            edgeFlag: Qt.LeftEdge
        }
        ResizeArea {
            anchors {
                right: parent.right
                top: parent.top
                bottom: parent.bottom
                topMargin: 10
                bottomMargin: 10
            }
            width: 5
            edgeFlag: Qt.RightEdge
        }
        ResizeArea {
            anchors {
                top: parent.top
                left: parent.left
                right: parent.right
                leftMargin: 10
                rightMargin: 10
            }
            height: 5
            edgeFlag: Qt.TopEdge
        }
        ResizeArea {
            anchors {
                bottom: parent.bottom
                left: parent.left
                right: parent.right
                leftMargin: 10
                rightMargin: 10
            }
            height: 5
            edgeFlag: Qt.BottomEdge
        }
        ResizeArea {
            anchors {
                top: parent.top
                left: parent.left
            }
            width: 10
            height: 10
            edgeFlag: Qt.TopEdge | Qt.LeftEdge
        }
        ResizeArea {
            anchors {
                top: parent.top
                right: parent.right
            }
            width: 10
            height: 10
            edgeFlag: Qt.TopEdge | Qt.RightEdge
        }
        ResizeArea {
            anchors {
                bottom: parent.bottom
                left: parent.left
            }
            width: 10
            height: 10
            edgeFlag: Qt.BottomEdge | Qt.LeftEdge
        }
        ResizeArea {
            anchors {
                bottom: parent.bottom
                right: parent.right
            }
            width: 10
            height: 10
            edgeFlag: Qt.BottomEdge | Qt.RightEdge
        }
    }
    component ResizeArea: MouseArea {
        property int edgeFlag
        cursorShape: {
            switch (edgeFlag) {
            case (Qt.TopEdge | Qt.LeftEdge):
                return Qt.SizeFDiagCursor;
            case (Qt.BottomEdge | Qt.RightEdge):
                return Qt.SizeFDiagCursor;
            case (Qt.TopEdge | Qt.RightEdge):
                return Qt.SizeBDiagCursor;
            case (Qt.BottomEdge | Qt.LeftEdge):
                return Qt.SizeBDiagCursor;
            case Qt.TopEdge:
                return Qt.SizeVerCursor;
            case Qt.BottomEdge:
                return Qt.SizeVerCursor;
            case Qt.LeftEdge:
                return Qt.SizeHorCursor;
            case Qt.RightEdge:
                return Qt.SizeHorCursor;
            default:
                return Qt.ArrowCursor;
            }
        }
        onPressed: window.startSystemResize(edgeFlag)
    }

    Component.onCompleted: folderDialog.open()
    FolderDialog {
        id: folderDialog
        title: "选择音乐文件夹"
        onAccepted: {
            var urlObject = new URL(folderDialog.selectedFolder);
            var folderPath = urlObject.pathname;
            if (Qt.platform.os === "windows" && folderPath.startsWith("/"))
                folderPath = folderPath.substring(1);
            if (folderPath)
                playerController.startMediaScan(folderPath);
        }
    }

    // =========================================================================
    // 3. 侧边栏 (播放列表)
    // =========================================================================
    Item {
        id: sidebarContainer
        width: sidebarWidth
        height: window.height
        z: 500
        x: isSidebarOpen ? 0 : -width

        Behavior on x {
            enabled: window.layoutAnimEnabled
            NumberAnimation {
                duration: 300
                easing.type: Easing.OutCubic
            }
        }

        RectangularGlow {
            anchors.fill: contentRect
            glowRadius: 10
            spread: 0.2
            color: "#80000000"
            visible: !isDockCapable && isSidebarOpen
        }

        Rectangle {
            id: contentRect
            anchors.fill: parent
            gradient: Gradient {
                GradientStop {
                    position: 0.0
                    color: playerController.gradientColor1
                    Behavior on color {
                        ColorAnimation {
                            duration: 500
                        }
                    }
                }
                GradientStop {
                    position: 0.5
                    color: playerController.gradientColor2
                    Behavior on color {
                        ColorAnimation {
                            duration: 500
                        }
                    }
                }
                GradientStop {
                    position: 1.0
                    color: playerController.gradientColor3
                    Behavior on color {
                        ColorAnimation {
                            duration: 500
                        }
                    }
                }
            }

            MusicListView {
                iconFamily: materialFont.name
                anchors.fill: parent
                onCloseRequested: {
                    window.isManualSidebarToggle = true;
                    window.isSidebarOpen = false;
                    manualAnimResetTimer.restart();
                }
            }
        }
    }
    MouseArea {
        anchors.fill: parent
        z: 499
        visible: !isDockCapable && isSidebarOpen
        onClicked: {
            window.isManualSidebarToggle = true;
            window.isSidebarOpen = false;
            manualAnimResetTimer.restart();
        }
    }

    // =========================================================================
    // 4. 播放器主界面
    // =========================================================================
    Item {
        id: playerContainer
        height: window.height

        property bool isDocked: isDockCapable && isSidebarOpen
        x: isDocked ? sidebarWidth : 0
        width: isDocked ? (window.width - sidebarWidth) : window.width

        // X 轴位移动画
        Behavior on x {
            enabled: window.layoutAnimEnabled
            NumberAnimation {
                duration: 300
                easing.type: Easing.OutCubic
            }
        }

        // 宽度动画
        Behavior on width {
            enabled: window.layoutAnimEnabled && window.isManualSidebarToggle
            NumberAnimation {
                duration: 300
                easing.type: Easing.OutCubic
            }
        }

        // ---------- 新增：显示用的“虚拟时间”与格式化函数 ----------
        function microsecToText(us) {
            // us: microseconds
            if (!isFinite(us) || us <= 0)
                return "0:00";
            var s = Math.floor(us / 1000000);
            var m = Math.floor(s / 60);
            var sec = s % 60;
            return m + ":" + (sec < 10 ? "0" + sec : sec);
        }

        // displayedProgress 优先级：
        // 1) 拖拽中 -> waveProgress.dragProgress
        // 2) 悬浮预览 -> waveProgress.hoverProgress
        // 3) 默认 -> 实际播放进度（playerController.currentPosMicrosec / total）
        property real displayedProgress: (waveProgress.dragProgress >= 0) ? waveProgress.dragProgress : (waveProgress.isHovering ? waveProgress.hoverProgress : (playerController.totalDurationMicrosec > 0 ? playerController.currentPosMicrosec / playerController.totalDurationMicrosec : 0))

        property real displayedCurrentMicrosec: (playerController.totalDurationMicrosec > 0) ? (playerController.totalDurationMicrosec * displayedProgress) : 0
        property real displayedRemainingMicrosec: Math.max(0, (playerController.totalDurationMicrosec - displayedCurrentMicrosec))

        property string displayedCurrentText: microsecToText(displayedCurrentMicrosec)
        property string displayedRemainingText: "-" + microsecToText(displayedRemainingMicrosec)
        // ------------------------------------------------------------------

        ColumnLayout {
            id: mainColumnLayout

            // [布局逻辑]
            // 宽度跟随窗口变化，留出 60px 边距。
            // 这让内部设置为 Layout.fillWidth 的控件（如歌名）可以随窗口拉伸。
            width: parent.width - 20

            anchors.centerIn: parent

            spacing: 0

            // --- 1. 封面控件 (固定 240x240) ---
            Item {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 240
                Layout.preferredHeight: 240

                RectangularGlow {
                    anchors.fill: coverRect
                    glowRadius: 10
                    spread: 0.1
                    color: "#80000000"
                    cornerRadius: 20
                    z: -1
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
                        sourceSize: Qt.size(1200, 1200)
                        smooth: true
                        antialiasing: true
                        mipmap: true
                        layer.enabled: true
                        layer.textureSize: Qt.size(width * Screen.devicePixelRatio * 2, height * Screen.devicePixelRatio * 2)
                        layer.samples: 8
                        layer.effect: OpacityMask {
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

            Item {
                Layout.preferredHeight: 30
                Layout.preferredWidth: 1
            }

            // --- 2. 波形进度条 (固定宽度 320) ---
            WaveformProgressBar {
                id: waveProgress
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 320
                Layout.preferredHeight: 60

                waveformHeights: playerController.waveformHeights
                barWidth: playerController.waveformBarWidth

                progress: playerController.totalDurationMicrosec > 0 ? (playerController.currentPosMicrosec / playerController.totalDurationMicrosec) : 0

                onSeekRequested: function (pos) {
                    playerController.isSeeking = true;
                    var targetTime = pos * playerController.totalDurationMicrosec;
                    playerController.seek(targetTime);
                }
                onReleased: {
                    playerController.isSeeking = false;
                }
            }

            Item {
                Layout.preferredHeight: 5
                Layout.preferredWidth: 1
            }

            // --- 3. 时间显示 (固定宽度 320) ---
            Item {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 320
                Layout.preferredHeight: 15

                Text {
                    // 这里绑定到 playerContainer 的 displayedCurrentText，
                    // 会在拖拽时跟随鼠标进度显示（但不实际 seek）
                    text: playerContainer.displayedCurrentText
                    color: "white"
                    font.pixelSize: 12
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    id: remainingDurationText
                    // 同样绑定到 displayedRemainingText（带 '-' 前缀）
                    text: playerContainer.displayedRemainingText
                    color: "white"
                    font.pixelSize: 12
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Item {
                Layout.preferredHeight: 15
                Layout.preferredWidth: 1
            }

            // --- 4. 文本信息 (可变宽度) ---
            // 这里使用 Layout.fillWidth: true，使其跟随 mainColumnLayout 的宽度变化
            // 但内部的 MarqueeText 会根据内容自动判断是居中还是滚动
            Column {
                Layout.alignment: Qt.AlignHCenter

                // [关键] 填满父容器宽度 (即 parent.width - 60)
                Layout.fillWidth: true

                // [可选] 限制最大宽度，避免太宽
                Layout.maximumWidth: 1000

                spacing: 5

                MarqueeText {
                    width: parent.width // 跟随 Column 宽度
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: playerController.songTitle
                    color: "white"
                    font.pixelSize: 24
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                }

                MarqueeText {
                    width: parent.width
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: playerController.artistName
                    color: "#DDDDDD"
                    font.pixelSize: 16
                    horizontalAlignment: Text.AlignHCenter
                }
                MarqueeText {
                    width: parent.width
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: playerController.albumName
                    color: "#AAAAAA"
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                }
            }

            Item {
                Layout.preferredHeight: 25
                Layout.preferredWidth: 1
            }

            // --- 5. 播放控制 (固定宽度 320) ---
            // 尽管父容器变宽了，但我们给这里设置了 preferredWidth，它会保持固定大小并居中
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 320

                // 使用弹簧 Item 将按钮居中分布
                Item {
                    Layout.fillWidth: true
                }

                StyleButton {
                    Layout.preferredWidth: 45
                    Layout.preferredHeight: 45
                    buttonText: "skip_previous"
                    iconFontFamily: materialFont.name
                    textSize: 35
                    textColor: "white"
                    onClicked: playerController.prev()
                }

                Item {
                    Layout.preferredWidth: 40
                } // 按钮间距

                StyleButton {
                    Layout.preferredWidth: 70
                    Layout.preferredHeight: 70
                    buttonText: playerController.isPlaying ? "pause" : "play_arrow"
                    baseColor: "#40FFFFFF"
                    hoverColor: "#60FFFFFF"
                    pressedColor: "#90FFFFFF"
                    iconFontFamily: materialFont.name
                    textSize: 50
                    textColor: "white"
                    onClicked: playerController.playpluse()
                }

                Item {
                    Layout.preferredWidth: 40
                } // 按钮间距

                StyleButton {
                    Layout.preferredWidth: 45
                    Layout.preferredHeight: 45
                    buttonText: "skip_next"
                    iconFontFamily: materialFont.name
                    textSize: 35
                    textColor: "white"
                    onClicked: playerController.next()
                }

                Item {
                    Layout.fillWidth: true
                }
            }

            Item {
                Layout.preferredHeight: 20
                Layout.preferredWidth: 1
            }

            // --- 6. 音量控制 (固定宽度 320) ---
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 320
                spacing: 15

                Text {
                    text: "volume_down"
                    color: "#CCC"
                    font.pixelSize: 20
                    font.family: materialFont.name
                }

                Slider {
                    id: volumeSlider
                    Layout.fillWidth: true
                    Layout.maximumWidth: 240
                    from: 0.0
                    to: 1.0
                    value: playerController.volume
                    onValueChanged: playerController.setVolume(value)

                    background: Rectangle {
                        x: volumeSlider.leftPadding
                        y: volumeSlider.topPadding + volumeSlider.availableHeight / 2 - height / 2
                        width: volumeSlider.availableWidth
                        height: 12
                        radius: 6
                        color: "#50FFFFFF"
                        Rectangle {
                            width: volumeSlider.visualPosition * parent.width
                            height: parent.height
                            color: "#C0FFFFFF"
                            radius: 6
                        }
                    }
                    handle: Item {}
                }

                Text {
                    text: "volume_up"
                    color: "#CCC"
                    font.pixelSize: 20
                    font.family: materialFont.name
                }
            }

            Item {
                Layout.preferredHeight: 20
                Layout.preferredWidth: 1
            }

            // --- 7. 底部按钮 (固定宽度 320) ---
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 320

                Row {
                    spacing: 20
                    StyleButton {
                        width: 40
                        height: 40
                        buttonText: "view_sidebar"
                        iconFontFamily: materialFont.name
                        textSize: 20
                        textColor: "white"
                        checkable: true
                        checked: isSidebarOpen
                        onClicked: {
                            window.isManualSidebarToggle = true;
                            window.isSidebarOpen = !window.isSidebarOpen;
                            manualAnimResetTimer.restart();
                        }
                    }
                    StyleButton {
                        width: 40
                        height: 40
                        buttonText: "shuffle"
                        iconFontFamily: materialFont.name
                        textSize: 20
                        textColor: "white"
                        onClicked: playerController.setShuffle(!playerController.isShuffle)
                        checkable: true
                        checked: playerController.isShuffle
                    }
                }

                Item {
                    Layout.minimumWidth: 100
                    Layout.maximumWidth: 100
                }

                Row {
                    spacing: 20
                    StyleButton {
                        id: stylePlayBtn
                        buttonText: playerController.repeatMode === 1 ? "repeat" : playerController.repeatMode === 2 ? "repeat_one" : "import_export"
                        width: 40
                        height: 40
                        iconFontFamily: materialFont.name
                        textSize: 20
                        textColor: "white"
                        onClicked: playerController.toggleRepeatMode()
                    }
                    StyleButton {
                        width: 40
                        height: 40
                        buttonText: "more_horiz"
                        iconFontFamily: materialFont.name
                        textSize: 20
                        textColor: "white"
                        onClicked: console.log("More Clicked")
                    }
                }
            }

            // Item { Layout.preferredHeight: 1 }
        }
    }

    // 顶部按钮
    Row {
        id: titleBarbuttons
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 10
        anchors.rightMargin: 10
        spacing: 8
        z: 200
        Repeater {
            model: ["minimize", "fullscreen", "close"]
            delegate: Button {
                id: btn
                text: modelData
                width: 25
                height: 25
                font.family: materialFont.name
                property bool isMaxWindow: false
                background: Rectangle {
                    radius: 15
                    color: btn.pressed ? "#80FFFFFF" : (btn.hovered ? "#60FFFFFF" : "#40FFFFFF")
                    Behavior on color {
                        ColorAnimation {
                            duration: 150
                        }
                    }
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
                    switch (index) {
                    case 0:
                        window.showMinimized();
                        break;
                    case 1:
                        if (isMaxWindow)
                            window.showNormal();
                        else
                            window.showMaximized();
                        isMaxWindow = !isMaxWindow;
                        break;
                    case 2:
                        Qt.quit();
                        break;
                    }
                }
            }
        }
    }
}
