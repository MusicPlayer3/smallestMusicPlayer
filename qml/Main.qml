import QtQuick
import QtQuick.Controls
import Qt5Compat.GraphicalEffects
import QtQuick.Effects
import QtQuick.Layouts
import QtQuick.Dialogs

import "./"


ApplicationWindow {
    id:window
    visible: true
    width: 400
    height: 750
    title: "Music Player"
    color: "transparent" // 为了圆角窗口或自定义背景
    flags: Qt.FramelessWindowHint | Qt.Window // 无边框窗口

    // 1. 加载字体
    FontLoader {
        id: materialFont
        source: "qrc:/MaterialIcons-Regular.ttf"
    }

    // 背景（模拟图中的渐变背景） 这个地方到时候
    Rectangle {
        id: background
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: playerController.gradientColor1 } // 上部浅色深色
            GradientStop { position: 0.5; color: playerController.gradientColor2 } // 中部更浅
            GradientStop { position: 1.0; color: playerController.gradientColor3 } // 下部更深
        }
        radius: 10 // 窗口圆角
    }

    // 这个是可拖动的窗口的区域
    MouseArea{
        // 锚定到父元素的顶部和左右两侧
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right

        height: 30

        property point clickPos: "0,0"
        onPressed: function(mouse){
            clickPos = Qt.point(mouse.x,mouse.y)
        }
        onPositionChanged: function(mouse){
            let delta = Qt.point(mouse.x - clickPos.x ,mouse.y - clickPos.y)
            window.x += delta.x
            window.y += delta.y
        }
    }

    // 这个让我的文件选择窗口自动在一开始就打开
    Component.onCompleted: {
        console.log("Application loaded. Automatically opening folder dialog.")
        // 1. 自动打开对话框
        folderDialog.open()
    }

    // 文件选择窗口
    FolderDialog {
        id: folderDialog
        title: "选择音乐文件夹"

        // 绑定 C++ 提供的默认路径 (CONSTANT 属性)
        //folder: playerController.defaultMusicPath

        // 当用户点击“确定”选择文件夹后触发
        onAccepted: {
            console.log("folderDialog.folder 的原始值:", folderDialog.selectedFolder);

            // 将获取到的文件路径转成合适的url
            var urlObject = new URL(folderDialog.selectedFolder);
            var folderPath = urlObject.pathname;

            if (Qt.platform.os === "windows" && folderPath.startsWith("/")) {
                folderPath = folderPath.substring(1);
            }

            // 调用 C++ 方法开始扫描
            if(folderPath === null){
                console.log("You not select a folder")
            }
            else{
                playerController.startMediaScan(folderPath)
            }
        }
        onRejected: {
            console.log("Folder selection cancelled.")
        }
    }

    MouseArea {
        z: -1
        anchors.fill: parent
        onClicked: {
            musicListView.isOpen = false
        }

    }

    Text {
        id: qqqtest
        text: "home"
        color: "white"
        anchors.top: parent.top
        anchors.left: parent.left
        font.weight: Font.Black
        font.family: materialFont.name
        width: 30
    }

    //--- 歌单程序布局 ---

    RowLayout {
        //width: window.width
        height: window.height
        width: 400
        //height: 750
        id: musicListView
        z : 2

        // 初始放在屏幕左侧外面
        x: isOpen ? 0 : -width

        // 为 x 属性添加动画
        Behavior on x {
            NumberAnimation {
                duration: 250
                easing.type: Easing.InOutQuad
            }
        }

        // 控制开关
        property bool isOpen: false

        // --- 2. 右侧内容区域，放置 MusicListView ---
        Rectangle {
            color: "#2B2E33" // 列表背景色
            Layout.fillWidth: true
            Layout.fillHeight: true

            // **** 实例化您的自定义 MusicListView 组件 ****
            // QML 引擎会通过文件名找到并加载它
            MusicListView {

                iconFamily: materialFont.name
                // 确保列表视图占据父容器的全部空间
                anchors.fill: parent
                // 可以设置内边距，让列表与边缘保持一定距离
                anchors.margins: 10
                // 连接 MusicListView 传出的信号 ***
                onCloseRequested: {
                    musicListView.isOpen = false // 执行关闭操作
                }

                // 注意：由于 MusicListView.qml 内部已经定义了 width/height，
                // 您可能需要在 MusicListView 内部将这些固定尺寸改为
                // Layout.preferredWidth/Layout.preferredHeight 或删除，
                // 确保它能适应父容器的大小。
                //
                // 在本例中，因为您在 MusicListView 内部使用了 anchors.fill: parent，
                // 它会覆盖组件内部设置的固定 width/height。
            }
        }

    }

    // ==========================================
    // 1. 顶部三个排版按钮 (最小化, 最大化, 关闭)
    // ==========================================
    Row {
        id:titleBarbuttons
        anchors.top: parent.top
        anchors.right: parent.right

        // **【新】添加边距，使其不紧贴边缘**
        anchors.topMargin: 10 // 顶部边距
        anchors.rightMargin: 10 // 右侧边距
        spacing: 8

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
                    id: bg
                    radius: 15

                    // hover / press 高亮逻辑
                    color: btn.pressed
                           ? "#80FFFFFF"
                           : (btn.hovered ? "#60FFFFFF" : "#40FFFFFF")

                    // 动画过渡
                    Behavior on color {
                        ColorAnimation { duration: 150 }
                    }
                }

                contentItem: Text {
                    id: brightText
                    text: btn.text
                    color: "white"
                    font.pixelSize: 15
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.weight: Font.Black
                    font.family: materialFont.name
                    // DropShadow {
                    //     // 设置锚定到文本，使阴影与文本重合
                    //     anchors.fill: brightText

                    //     // 阴影颜色设置为与文本相同或更亮的颜色
                    //     color: brightText.color

                    //     // 减小偏移量，让阴影围绕文本扩散
                    //     horizontalOffset: 0
                    //     verticalOffset: 0

                    //     // 增加模糊半径，创建柔和的辉光效果，使文本看起来更亮
                    //     radius: 3.0

                    //     // source 必须是你要加特效的元素
                    //     source: brightText
                    // }
                }

                onClicked: {
                    switch(index){
                        case 0:
                            window.showMinimized();
                            break;
                        case 1:
                            if(isMaxWindow){
                                window.showNormal();
                            }else{
                                window.showMaximized();
                            }
                            isMaxWindow = !isMaxWindow
                            break;
                        case 2:
                            Qt.quit();
                            break;
                    }
                    //if (index === 2) Qt.quit();   // index 2 = “×”
                }
            }
        }
    }


    // 主布局容器
    ColumnLayout {
        id: mainColumnLayout
        width: 380
        height: 750
        anchors.centerIn: parent
        anchors.margins: 10
        spacing: 10

        Item {
            // 按钮高 30，下方间隔 Item 高 20，共 50
            Layout.preferredHeight: 40 // 占据原有的空间高度
            Layout.fillWidth: true // 保持宽度不变
        }

        Item { Layout.fillHeight: true; Layout.preferredHeight: 20 } // 间隔

        // ==========================================
        // 2. 封面控件 (居中 + 阴影)
        // ==========================================
        Item {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 256
            Layout.preferredHeight: 256

            //阴影源
            RectangularGlow {
                id: effect
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

                // 这里放你的 Image 控件
                Image {
                    id: bigPNG
                    anchors.fill: parent
                    // 后端来了，采用的是当 C++ 检测到指针变化并 emit 信号时，这里会自动刷新
                    source: playerController.coverArtSource 
                    fillMode: Image.PreserveAspectCrop

                    layer.enabled: true
                    layer.effect: OpacityMask{
                        anchors.fill: parent
                        maskSource: Rectangle {
                            width: bigPNG.width
                            height: bigPNG.height
                            radius: 20      // 使用同样的圆角
                            color: "white"  // white = 不透明区域
                        }
                    }
                }

            }
        }

        Item { Layout.fillHeight: true; Layout.preferredHeight: 30 } // 间隔

        // ==========================================
        // 3. 进度条区域 (时间 - 进度条 - 时间)
        // ==========================================
        

        ColumnLayout{
            Layout.fillWidth: true
            Layout.leftMargin: 18  
            Layout.rightMargin: 18
            spacing: 10
            Slider {
                id: progressSlider
                Layout.fillWidth: true
                Layout.leftMargin: 2  
                Layout.rightMargin: 2
                from: 0
                to: playerController.totalDurationMicrosec 

                //正式修复滑动条人机打架的问题
                Binding {
                    target: progressSlider
                    property: "value"
                    value: playerController.currentPosMicrosec
                    when: !progressSlider.pressed
                }
                onPressedChanged: {
                    // 1. 检查：是否刚刚松手 (pressed 变为 false)
                    if (!pressed) {

                        // 2. 只有松手后，才提交 Seek 命令
                        playerController.seek(value);
                        // 同时释放锁
                        playerController.setIsSeeking(false)
                    }
                    else{// 3. 你没松手,一直按,前端获取前端控制权
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
                // 隐藏默认的圆球把手
                handle: Item {}
            }

            RowLayout{
                Layout.fillWidth: true // 确保文本容器占满宽度
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
                    font.pixelSize: 12 // 推荐字体略小
                }
            }
        }

        Item { Layout.fillHeight: true; Layout.preferredHeight: 10 } // 间隔

        // ==========================================
        // 4. 文本信息区域 (标题, 演唱者, 专辑)
        // ==========================================
        Column {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            spacing: 5 // 相邻紧凑一点

            Text {
                text: playerController.songTitle
                color: "white"
                font.pixelSize: 22
                font.bold: true
                anchors.horizontalCenter: parent.horizontalCenter
            }

            Text {
                text: playerController.artistName
                color: "#DDDDDD"
                font.pixelSize: 16
                anchors.horizontalCenter: parent.horizontalCenter
            }

            Text {
                text: playerController.albumName
                color: "#AAAAAA"
                font.pixelSize: 14
                anchors.horizontalCenter: parent.horizontalCenter
                elide: Text.ElideRight
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
            }
        }

        Item { Layout.fillHeight: true; Layout.preferredHeight: 20 } // 间隔

        // ==========================================
        // 5. 播放控制 (上一首, 播放, 下一首)
        // 使用封装的 StyledButton
        // ==========================================
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 30

            // 上一首
            StyleButton {
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
                buttonText: "skip_previous"
                // 内部的 contentItem 默认字体为 20，这里覆盖一下
                iconFontFamily: materialFont.name
                textSize: 30
                textColor: "white"
                onClicked: playerController.prev()
            }

            // 播放/暂停 (稍大) 
            StyleButton {
                Layout.preferredWidth: 60
                Layout.preferredHeight: 60
                buttonText: playerController.isPlaying ? "pause" : "play_arrow" 

                // 设置更高的透明度作为基础颜色，使其更亮
                baseColor: "#40FFFFFF"
                hoverColor: "#60FFFFFF"
                pressedColor: "#90FFFFFF"

                // 设置更大的字体
                iconFontFamily: materialFont.name
                textSize: 40
                textColor: "white"
                onClicked: {
                    playerController.playpluse()
                }
            }

            // 下一首
            StyleButton {
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
                buttonText: "skip_next"
                iconFontFamily: materialFont.name
                textSize: 30
                textColor: "white"
                onClicked: playerController.next()
            }
        }

        Item { Layout.fillHeight: true; Layout.preferredHeight: 1 } // 间隔

        // ==========================================
        // 6. 音量控制区
        // ==========================================
        RowLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            spacing: 1

            // 左边小喇叭
            Text { text: "volume_mute"; color: "#CCC"; font.pixelSize: 18 ; font.family: materialFont.name}

            // 音量条：圆头矩形，无标头，显示占比
            Slider {
                id: volumeSlider
                Layout.fillWidth: true
                Layout.maximumWidth: 200 // 限制一下最大宽度，不要太长
                from: 0.0
                to: 1.0

                value: playerController.volume
    
                // 【绑定 2：控制】当用户拖动滑块时，立即调用 C++ 方法设置音量
                onValueChanged: {
                    // 如果音量值改变，立即设置后端音量
                    // 确保 value 在 0.0 到 1.0 之间
                    playerController.setVolume(value)
                }

                background: Rectangle {
                    x: volumeSlider.leftPadding
                    y: volumeSlider.topPadding + volumeSlider.availableHeight / 2 - height / 2
                    width: volumeSlider.availableWidth
                    height: 12 // 稍粗一点的矩形
                    radius: 6 // 圆角等于高度的一半，形成圆头
                    color: "#40000000" // 底色

                    Rectangle {
                        width: volumeSlider.visualPosition * parent.width
                        height: parent.height
                        color: "#C0FFFFFF" // 填充色
                        radius: 6
                    }
                }
                // 完全移除 Handle
                handle: Item {}
            }

            // 右边大喇叭
            Text { text: "volume_up"; color: "#CCC"; font.pixelSize: 18 ; font.family: materialFont.name}
        }

        Item { Layout.fillHeight: true; Layout.preferredHeight: 1 } // 间隔

        // ==========================================
        // 7. 底部按钮组 (左2，右2，居中分开)
        // ==========================================
        // 这里有个知识点，在普通布局下是用的width ，但是带Layout的就必须使用这个了Layout.preferredWidth
        RowLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            spacing: 100 // <-- 设置左右两组之间的固定距离

            // 左边一组
            Row {
                spacing: 15
                StyleButton {
                    width: 40;
                    height: 40
                    buttonText: "view_sidebar"
                    iconFontFamily: materialFont.name
                    textSize: 18
                    textColor: "white"
                    onClicked: {
                        console.log("Lyrics/List Clicked")
                        musicListView.isOpen = !musicListView.isOpen
                    }
                }
                StyleButton {
                    width: 40;
                    height: 40
                    buttonText: "shuffle"
                    iconFontFamily: materialFont.name
                    textSize: 18
                    textColor: "white"

                    onClicked: {
                        // 切换逻辑：调用 C++ Setter 来改变后端状态
                        // 注意：我们传入 checked 的相反值，以切换后端状态。
                        playerController.setShuffle(!playerController.isShuffle)
                    }
                    checkable: true
                    checked: playerController.isShuffle
                }
            }

            // 中间撑开距离
            //Item { Layout.fillWidth: true }

            // 右边一组
            Row {
                spacing: 15
                StyleButton {
                    id: stylePlayBtn
                    property int playMode : 1 // 0 是列表顺序播放，1 是单曲循环
                    buttonText: "repeat" // 初始设置为 "repeat
                    width: 40
                    height: 40
                    iconFontFamily: materialFont.name
                    textSize: 18
                    textColor: "white"
                    onClicked:{
                       console.log("Repeat Clicked") // TODO: 这里改变播放状态
                       playMode ++;
                       switch(playMode){
                           case 1:
                               // 列表顺序播放
                               stylePlayBtn.buttonText = "repeat"
                               break;
                           case 2:
                               // 单曲循环
                               stylePlayBtn.buttonText = "repeat_one"
                               playMode = 0;
                               break;
                       }
                   }
                }

                StyleButton {
                    width: 40;
                    height: 40
                    buttonText: "more_vert" // TODO: 更多菜单
                    iconFontFamily: materialFont.name
                    textSize: 18
                    textColor: "white"
                    onClicked: console.log("More Clicked") // TODO: 留好给定义功能的位置
                }
            }
        }

        // 底部留白
        Item { Layout.preferredHeight: 40 }
    }
}
