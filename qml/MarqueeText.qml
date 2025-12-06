import QtQuick

Item {
    id: root

    // =========================
    // 公共属性 (对外接口)
    // =========================
    property string text: ""                // 要显示的文本
    property color color: "white"           // 文本颜色
    property alias font: measurer.font      // 字体设置 (大小、粗细等)
    property real speed: 30                 // 滚动速度 (ms/px)，越小越快
    property real spacing: 50               // 滚动时首尾相接的间距

    width: 380 //这个是默认尺寸外面可以去改的
    height: 30
    clip: true

    // 1. 隐藏的测量文本 (用于计算实际宽度和承载字体设置)
    Text {
        id: measurer
        visible: false
        text: root.text
        // 这里的 font 已通过 alias 暴露给外部
        font.bold: true
        wrapMode: Text.NoWrap
    }


    Item {
        id: isolationLayer
        height: root.height

        // ⚡ 关键调整 1: 确保隔离层水平居中于根控件
        anchors.horizontalCenter: parent.horizontalCenter

        // ⚡ 关键调整 2: 隔离层的宽度必须绑定到内容的实际宽度或 viewPort 宽度
        // 当不滚动时，宽度 = 文本实际宽度；当滚动时，宽度 = viewPort 宽度
        width: measurer.implicitWidth > root.width ? root.width : measurer.implicitWidth

        // 2. 滚动容器
        Row {
            id: scrollContent
            height: root.height
            spacing: root.spacing

            // 这里的 x 由 updateState() 函数完全接管，不使用 anchors

            // --- 文本副本 1 ---
            Text {
                text: root.text
                color: root.color
                font: root.font
                height: root.height
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.NoWrap
            }

            // --- 文本副本 2 (仅滚动时显示) ---
            Text {
                text: root.text
                color: root.color
                font: root.font
                height: root.height
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.NoWrap
                // 只有当动画运行时才需要显示这个影子文本
                visible: measurer.implicitWidth > root.width
            }
        }
    }


    // 3. 动画控制
    PropertyAnimation {
        id: marqueeAnim
        target: scrollContent
        property: "x"
        easing.type: Easing.Linear

        onRunningChanged: {
            // 循环逻辑：动画停止且处于滚动状态时，重置并重启
            if (!running && scrollContent.x !== 0 && to !== 0) {
                 if (measurer.implicitWidth > root.width) {
                     scrollContent.x = 0;
                     marqueeAnim.start();
                 }
            }
        }
    }

    // 4. 状态更新函数 (核心逻辑)
    function updateState() {
        marqueeAnim.stop();

        var textWidth = measurer.implicitWidth;
        var viewWidth = root.width;

        if (textWidth > viewWidth) {

            scrollContent.x = 0;

            // 1. 设置动画起点：让文本从居中对齐的位置开始
            marqueeAnim.from = 0; // 滚动起点从 viewPort 的左边缘开始
            marqueeAnim.to = -(textWidth + root.spacing);

            // 3. 调整 duration
            marqueeAnim.duration = measurer.implicitWidth > 0 ? measurer.implicitWidth * 30 : 0;

            //scrollContent.children[1].visible = true; // 显示第二个副本
            marqueeAnim.start();

        } /*else {
            var centerX = (viewWidth - textWidth) / 2;

            // 2. 直接赋值
            scrollContent.x = centerX;
        }*/
    }

    // 5. 触发更新的信号
    Component.onCompleted: updateState()
    onWidthChanged: updateState() // 应对 viewPort 宽度变化
    Connections {
        target: playerController
        function onSongTitleChanged() {
            root.updateState();
        }
    }
}
