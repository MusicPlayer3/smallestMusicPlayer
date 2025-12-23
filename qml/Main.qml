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
    width: 370
    height: 720
    minimumWidth: 370
    minimumHeight: 720
    title: "Music Player"
    color: "transparent"
    flags: Qt.FramelessWindowHint | Qt.Window

    // 辅助函数
    function formatTimeJS(microsecs) {
        if (microsecs < 0)
            microsecs = 0;
        var totalSecs = Math.floor(microsecs / 1000000);
        var m = Math.floor(totalSecs / 60);
        var s = totalSecs % 60;
        return (m < 10 ? "0" + m : m) + ":" + (s < 10 ? "0" + s : s);
    }

    readonly property int sidebarWidth: 350
    readonly property int playerMinWidth: 450
    readonly property bool isDockCapable: window.width >= (sidebarWidth + playerMinWidth)
    property bool isSidebarOpen: false
    property bool layoutAnimEnabled: true
    property bool isManualSidebarToggle: false

    Timer {
        id: animResetTimer
        interval: 50
        onTriggered: window.layoutAnimEnabled = true
    }
    Timer {
        id: manualAnimResetTimer
        interval: 310
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

    // ToolTip 组件
    component SharedToolTip: ToolTip {
        id: toolTipControl
        property bool suppressed: false
        visible: parent.hovered && !suppressed
        delay: 500
        Connections {
            target: parent
            function onPressedChanged() {
                if (parent.pressed) {
                    toolTipControl.suppressed = true;
                }
            }
            function onHoveredChanged() {
                if (!parent.hovered) {
                    toolTipControl.suppressed = false;
                }
            }
        }
        contentItem: Text {
            text: toolTipControl.text
            color: "#FFFFFF"
            font.pixelSize: 12
            leftPadding: 4
            rightPadding: 4
        }
        background: Rectangle {
            color: "#1A1A1A"
            radius: 4
            opacity: 0.95
            border.color: "#333333"
            border.width: 1
        }
        enter: Transition {
            NumberAnimation {
                property: "opacity"
                from: 0.0
                to: 1.0
                duration: 200
            }
        }
        exit: Transition {
            NumberAnimation {
                property: "opacity"
                from: 1.0
                to: 0.0
                duration: 200
            }
        }
    }

    MessageDialog {
        id: aboutDialog
        title: "关于"
        text: "Music Player v1.0\n基于 Qt 6.8 & QML 开发"
        buttons: MessageDialog.Ok
    }

    OutputSettingsWindow {
        id: outputSettingsWin
    }

    // =========================================================================
    // 圆角实现核心：Mask + Layer
    // =========================================================================

    // 2. 定义遮罩形状（不可见）
    Rectangle {
        id: rectMask
        anchors.fill: parent
        radius: window.visibility === Window.Maximized ? 0 : 24
        visible: false
    }

    // 3. 主内容容器
    Item {
        id: windowContent
        anchors.fill: parent

        // 关键点：开启图层并使用 OpacityMask
        // 这会强制将此 Item 内的所有内容（背景、列表、播放器）按照 rectMask 的形状进行裁剪
        layer.enabled: true
        layer.effect: OpacityMask {
            maskSource: rectMask
        }

        // 黑色底色背景 (用于防止透明时穿透)
        Rectangle {
            anchors.fill: parent
            color: "#000000"
        }

        StackLayout {
            id: pages
            anchors.fill: parent
            currentIndex: 0

            // -------------------------
            // Page 0: 启动页（窗体1）
            // -------------------------
            StartupPage {
                Layout.fillWidth: true
                Layout.fillHeight: true

                onRestorePlaylistRequested: {
                    // 这里以后接数据库恢复逻辑
                    playerController.UpdateLastFolder();
                    pages.currentIndex = 1;
                }
                onAddFolderRequested: {
                    pages.currentIndex = 1;
                    folderDialog.open();
                }
            }

            // -------------------------
            // Page 1: 主播放器页（窗体2）
            // -------------------------
            Item {
                id: playerPage
                Layout.fillWidth: true
                Layout.fillHeight: true
                // anchors.fill: parent

                // =========================================================================
                // 1. 全局背景 (渐变流光)
                // =========================================================================
                Background {
                    id: mainBackground
                    anchors.fill: parent
                    z: 0

                    color0: playerController.gradientColor1
                    color1: playerController.gradientColor2
                    color2: playerController.gradientColor3

                    MouseArea {
                        anchors.fill: parent
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        onPressed: window.startSystemMove()
                    }
                }

                // 菜单
                BubblePopup {
                    id: mainMenu
                    width: 170
                    onVisibleChanged: {
                        if (!visible) {
                            subMenu.close();
                        }
                    }
                    Column {
                        spacing: 0
                        Rectangle {
                            width: 162
                            height: 40
                            color: paramMouse.containsMouse ? "#50FFFFFF" : "transparent"
                            radius: 4
                            property bool isDirect: playerController.outputMode === 0
                            enabled: !isDirect
                            opacity: isDirect ? 0.3 : 1.0
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 12
                                Text {
                                    text: "tune"
                                    font.family: materialFont.name
                                    color: "white"
                                    font.pixelSize: 18
                                    verticalAlignment: Text.AlignVCenter
                                    Layout.fillHeight: true
                                }
                                Text {
                                    text: "输出参数设置"
                                    color: "white"
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    verticalAlignment: Text.AlignVCenter
                                    Layout.fillHeight: true
                                }
                            }
                            MouseArea {
                                id: paramMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    mainMenu.close();
                                    outputSettingsWin.openDialog();
                                }
                            }
                        }
                        Item {
                            width: 162
                            height: 9
                            Rectangle {
                                width: 150
                                height: 1
                                color: "#40FFFFFF"
                                anchors.centerIn: parent
                            }
                        }
                        Rectangle {
                            width: 162
                            height: 40
                            color: modeMouse.containsMouse ? "#50FFFFFF" : "transparent"
                            radius: 4
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 12
                                Text {
                                    text: "settings_input_component"
                                    font.family: materialFont.name
                                    color: "white"
                                    font.pixelSize: 18
                                    verticalAlignment: Text.AlignVCenter
                                    Layout.fillHeight: true
                                }
                                Text {
                                    text: "输出模式"
                                    color: "white"
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    verticalAlignment: Text.AlignVCenter
                                    Layout.fillHeight: true
                                }
                                Text {
                                    text: "chevron_right"
                                    font.family: materialFont.name
                                    color: "#AAAAAA"
                                    font.pixelSize: 20
                                    verticalAlignment: Text.AlignVCenter
                                    Layout.fillHeight: true
                                }
                            }
                            MouseArea {
                                id: modeMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    subMenu.transientParent = mainMenu;
                                    subMenu.toggle(modeMouse, 1);
                                }
                            }
                        }
                        Item {
                            width: 162
                            height: 9
                            Rectangle {
                                width: 150
                                height: 1
                                color: "#40FFFFFF"
                                anchors.centerIn: parent
                            }
                        }
                        Rectangle {
                            width: 162
                            height: 40
                            color: aboutMouse.containsMouse ? "#50FFFFFF" : "transparent"
                            radius: 4
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 12
                                Text {
                                    text: "info"
                                    font.family: materialFont.name
                                    color: "white"
                                    font.pixelSize: 18
                                    verticalAlignment: Text.AlignVCenter
                                    Layout.fillHeight: true
                                }
                                Text {
                                    text: "关于"
                                    color: "white"
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    verticalAlignment: Text.AlignVCenter
                                    Layout.fillHeight: true
                                }
                            }
                            MouseArea {
                                id: aboutMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    mainMenu.close();
                                    aboutDialog.open();
                                }
                            }
                        }
                        Rectangle {
                            width: 162
                            height: 40
                            color: quitMouse.containsMouse ? "#50FF5252" : "transparent"
                            radius: 4
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 12
                                Text {
                                    text: "power_settings_new"
                                    font.family: materialFont.name
                                    color: "#FF8A80"
                                    font.pixelSize: 18
                                    verticalAlignment: Text.AlignVCenter
                                    Layout.fillHeight: true
                                }
                                Text {
                                    text: "退出"
                                    color: "#FF8A80"
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    verticalAlignment: Text.AlignVCenter
                                    Layout.fillHeight: true
                                }
                            }
                            MouseArea {
                                id: quitMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    Qt.quit();
                                }
                            }
                        }
                    }
                }

                BubblePopup {
                    id: subMenu
                    width: 150
                    Column {
                        spacing: 0
                        Rectangle {
                            width: 130
                            height: 35
                            radius: 4
                            color: directMouse.containsMouse ? "#50FFFFFF" : "transparent"
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                spacing: 10
                                Text {
                                    text: playerController.outputMode === 0 ? "radio_button_checked" : "radio_button_unchecked"
                                    font.family: materialFont.name
                                    color: playerController.outputMode === 0 ? "#81C784" : "white"
                                    font.pixelSize: 18
                                    verticalAlignment: Text.AlignVCenter
                                    Layout.fillHeight: true
                                }
                                Text {
                                    text: "Direct (直接)"
                                    color: "white"
                                    font.pixelSize: 13
                                    verticalAlignment: Text.AlignVCenter
                                    Layout.fillHeight: true
                                }
                            }
                            MouseArea {
                                id: directMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    playerController.outputMode = 0;
                                    subMenu.close();
                                    mainMenu.close();
                                }
                            }
                        }
                        Rectangle {
                            width: 130
                            height: 35
                            radius: 4
                            color: mixMouse.containsMouse ? "#50FFFFFF" : "transparent"
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                spacing: 10
                                Text {
                                    text: playerController.outputMode === 1 ? "radio_button_checked" : "radio_button_unchecked"
                                    font.family: materialFont.name
                                    color: playerController.outputMode === 1 ? "#81C784" : "white"
                                    font.pixelSize: 18
                                    verticalAlignment: Text.AlignVCenter
                                    Layout.fillHeight: true
                                }
                                Text {
                                    text: "Mixing (混音)"
                                    color: "white"
                                    font.pixelSize: 13
                                    verticalAlignment: Text.AlignVCenter
                                    Layout.fillHeight: true
                                }
                            }
                            MouseArea {
                                id: mixMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    playerController.outputMode = 1;
                                    subMenu.close();
                                    mainMenu.close();
                                }
                            }
                        }
                    }
                }

                // =========================================================================
                // 侧边栏 (Sidebar)
                // =========================================================================
                Item {
                    id: sidebarContainer
                    width: sidebarWidth
                    height: parent.height
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
                        z: -1
                    }

                    // Sidebar 内容容器
                    Item {
                        id: contentRect
                        anchors.fill: parent
                        Background {
                            anchors.fill: parent
                            color0: playerController.gradientColor1
                            color1: playerController.gradientColor2
                            color2: playerController.gradientColor3
                        }

                        // 右边缘分割线
                        Rectangle {
                            anchors.right: parent.right
                            width: 1
                            height: parent.height
                            color: "#15FFFFFF"
                        }

                        MusicListView {
                            id: musicListView
                            iconFamily: materialFont.name
                            anchors.fill: parent
                            onCloseRequested: {
                                window.isManualSidebarToggle = true;
                                window.isSidebarOpen = false;
                                manualAnimResetTimer.restart();
                            }
                            onAddFolderRequested: addFolderDialog.open() // 打开已有的文件夹对话框
                            onAddFileRequested: fileDialog.open()     // 打开新添加的文件对话框
                        }
                    }
                }

                // 点击遮罩
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

                // 4. 播放器主界面
                Item {
                    id: playerContainer
                    height: parent.height
                    property bool isDocked: isDockCapable && isSidebarOpen
                    x: isDocked ? sidebarWidth : 0
                    width: isDocked ? (window.width - sidebarWidth) : window.width
                    Behavior on x {
                        enabled: window.layoutAnimEnabled
                        NumberAnimation {
                            duration: 300
                            easing.type: Easing.OutCubic
                        }
                    }
                    Behavior on width {
                        enabled: window.layoutAnimEnabled && window.isManualSidebarToggle
                        NumberAnimation {
                            duration: 300
                            easing.type: Easing.OutCubic
                        }
                    }

                    ColumnLayout {
                        id: mainColumnLayout
                        width: parent.width - 20
                        anchors.centerIn: parent
                        spacing: 0

                        // 1. 封面
                        Item {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 240
                            Layout.preferredHeight: 240

                            RectangularGlow {
                                anchors.fill: coverRect
                                glowRadius: 30
                                spread: 0.0
                                color: "#50000000"
                                cornerRadius: 12
                                z: -1
                            }

                            Rectangle {
                                id: coverRect
                                anchors.fill: parent
                                radius: 12
                                color: "#252525"
                                clip: true
                                Image {
                                    id: bigPNG
                                    anchors.fill: parent
                                    source: playerController.coverArtSource
                                    fillMode: Image.PreserveAspectCrop
                                    sourceSize: Qt.size(600, 600)
                                    smooth: true
                                    mipmap: true
                                    layer.enabled: true
                                    layer.effect: OpacityMask {
                                        anchors.fill: parent
                                        maskSource: Rectangle {
                                            width: bigPNG.width
                                            height: bigPNG.height
                                            radius: 12
                                        }
                                    }
                                }
                            }
                        }

                        Item {
                            Layout.preferredHeight: 30
                            Layout.preferredWidth: 1
                        }

                        // 2. 波形
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

                        // 3. 时间
                        Item {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 280
                            Layout.preferredHeight: 15
                            Text {
                                text: waveProgress.isHovering ? window.formatTimeJS(playerController.totalDurationMicrosec * waveProgress.hoverProgress) : playerController.currentPosText
                                color: waveProgress.isHovering ? "#E0E0E0" : "white"
                                font.pixelSize: 12
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                id: remainingDurationText
                                text: {
                                    if (waveProgress.isHovering) {
                                        var total = playerController.totalDurationMicrosec;
                                        var currentPreview = total * waveProgress.hoverProgress;
                                        return "-" + window.formatTimeJS(total - currentPreview);
                                    } else {
                                        return "-" + playerController.remainingTimeText;
                                    }
                                }
                                color: waveProgress.isHovering ? "#E0E0E0" : "white"
                                font.pixelSize: 12
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        Item {
                            Layout.preferredHeight: 10
                            Layout.preferredWidth: 1
                        }

                        // 4. 信息
                        Column {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.fillWidth: true
                            Layout.maximumWidth: 1000
                            spacing: 5
                            MarqueeText {
                                width: parent.width
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
                            Layout.preferredHeight: 15
                            Layout.preferredWidth: 1
                        }

                        // 5. 控制按钮
                        RowLayout {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 320
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
                                SharedToolTip {
                                    text: "上一首"
                                }
                            }
                            Item {
                                Layout.preferredWidth: 40
                            }
                            StyleButton {
                                Layout.preferredWidth: 60
                                Layout.preferredHeight: 60
                                buttonText: playerController.isPlaying ? "pause" : "play_arrow"
                                baseColor: "#40FFFFFF"
                                hoverColor: "#60FFFFFF"
                                pressedColor: "#90FFFFFF"
                                iconFontFamily: materialFont.name
                                textSize: 50
                                textColor: "white"
                                onClicked: playerController.playpluse()
                                SharedToolTip {
                                    text: playerController.isPlaying ? "暂停" : "播放"
                                }
                            }
                            Item {
                                Layout.preferredWidth: 40
                            }
                            StyleButton {
                                Layout.preferredWidth: 45
                                Layout.preferredHeight: 45
                                buttonText: "skip_next"
                                iconFontFamily: materialFont.name
                                textSize: 35
                                textColor: "white"
                                onClicked: playerController.next()
                                SharedToolTip {
                                    text: "下一首"
                                }
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                        }

                        Item {
                            Layout.preferredHeight: 10
                            Layout.preferredWidth: 1
                        }

                        // 6. 音量
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
                                SharedToolTip {
                                    text: "音量: " + Math.round(volumeSlider.value * 100) + "%"
                                }
                            }
                            Text {
                                text: "volume_up"
                                color: "#CCC"
                                font.pixelSize: 20
                                font.family: materialFont.name
                            }
                        }

                        Item {
                            Layout.preferredHeight: 15
                            Layout.preferredWidth: 1
                        }

                        // 7. 底部按钮
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
                                    SharedToolTip {
                                        text: isSidebarOpen ? "隐藏列表" : "显示列表"
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
                                    SharedToolTip {
                                        text: playerController.isShuffle ? "关闭乱序播放" : "启用乱序播放"
                                    }
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
                                    SharedToolTip {
                                        text: {
                                            switch (playerController.repeatMode) {
                                            case 1:
                                                return "列表循环";
                                            case 2:
                                                return "单曲循环";
                                            default:
                                                return "顺序播放";
                                            }
                                        }
                                    }
                                }

                                // [更多按钮]
                                StyleButton {
                                    id: moreBtn
                                    width: 40
                                    height: 40
                                    buttonText: "more_horiz"
                                    iconFontFamily: materialFont.name
                                    textSize: 20
                                    textColor: "white"
                                    onClicked: {
                                        mainMenu.toggle(moreBtn, 0);
                                    }
                                    SharedToolTip {
                                        text: "更多设置"
                                    }
                                }
                            }
                        }
                    }
                }
                // 8. 全局遮罩层
                MouseArea {
                    id: globalOverlay
                    anchors.fill: parent
                    visible: musicListView.sortPopup.visible || mainMenu.visible || subMenu.visible
                    z: 999
                    enabled: visible

                    onClicked: {
                        if (mainMenu.visible)
                            mainMenu.close();
                        if (subMenu.visible)
                            subMenu.close();
                        if (musicListView.sortPopup.visible)
                            musicListView.sortPopup.close();
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
                            SharedToolTip {
                                text: {
                                    switch (index) {
                                    case 0:
                                        return "最小化";
                                    case 1:
                                        return isMaxWindow ? "还原" : "最大化";
                                    case 2:
                                        return "关闭";
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    } // End of windowContent

    // ResizeArea (保留在最外层，不被裁剪)
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

    //Component.onCompleted: folderDialog.open()
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

    FolderDialog {
        id: addFolderDialog
        title: "新增音乐文件夹"
        onAccepted: {
            var urlObject = new URL(addFolderDialog.selectedFolder);
            var folderPath = urlObject.pathname;
            if (Qt.platform.os === "windows" && folderPath.startsWith("/"))
                folderPath = folderPath.substring(1);
            if (folderPath)
                musicListModel.ListViewAddNewFolder(folderPath);
        }
    }

    FileDialog {
        id: fileDialog
        title: "选择音乐文件"
        nameFilters: ["音频文件 (*.mp3 *.flac *.wav *.m4a *.aac *.ogg *.wma *.opus  *.mpc *.mp+ *.mpp *.ape *.aiff *.aif *.wv *.tta *.alac *.shn *.tak *.dsf *.dff *.dxd *.mka *.webm *.dts *.ac3 *.truehd)", "所有文件 (*)"]
        onAccepted: {
            var urlObject = new URL(fileDialog.selectedFile);
            var filePath = urlObject.pathname;

            if (Qt.platform.os === "windows" && filePath.startsWith("/"))
                filePath = filePath.substring(1);
            if (filePath)
                musicListModel.ListViewAddNewFile(filePath);
        }
    }
}
