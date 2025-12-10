import QtQuick
import QtQuick.Controls

Item {
    id: root

    // --- 公共属性 ---
    // 从 C++ 接收的原始数据
    property var waveformHeights: []
    property int barWidth: 4
    property int spacing: 2

    // 颜色配置
    property color playedColor: "white" // 正常播放时的颜色
    property color remainingColor: "#60FFFFFF"// 未播放部分的颜色

    // [新增] 悬浮时的预览颜色
    // 根据需求 "比已播放颜色更浅"。如果 playedColor 是纯白，这里可以用带一点透明度的白
    // 或者完全不透明的白，而将 playedColor 稍微调暗一点。
    // 这里默认设置为 #E0FFFFFF (稍微透明一点的白) 区分 #FFFFFF
    // 或者设置为 #AAFFFFFF，您可以根据实际视觉喜好调整这个值
    property color hoverColor: "#C0FFFFFF"

    // 进度 (0.0 - 1.0)
    property real progress: 0.0

    // [新增] 悬浮相关的内部属性
    property real hoverProgress: 0.0
    property bool isHovering: false

    signal seekRequested(real position)
    signal pressed
    signal released

    implicitWidth: 320
    implicitHeight: 60

    // --- 内部逻辑属性 ---

    property var displayedHeights: []
    property var pendingHeights: []
    property real globalMultiplier: 1.0

    // 监听 C++ 数据源变化
    onWaveformHeightsChanged: {
        pendingHeights = waveformHeights;

        if (growAnim.running) {
            growAnim.stop();
        }

        if (!shrinkAnim.running && globalMultiplier > 0.01) {
            shrinkAnim.start();
        } else if (globalMultiplier <= 0.01) {
            swapAndGrow();
        }
    }

    function swapAndGrow() {
        displayedHeights = pendingHeights;
        growAnim.start();
    }

    // --- 动画定义 ---

    NumberAnimation {
        id: shrinkAnim
        target: root
        property: "globalMultiplier"
        to: 0.0
        duration: 200
        easing.type: Easing.InQuad
        onFinished: {
            root.swapAndGrow();
        }
    }

    NumberAnimation {
        id: growAnim
        target: root
        property: "globalMultiplier"
        to: 1.0
        duration: 350
        easing.type: Easing.OutBack
        easing.overshoot: 0.8
    }

    // --- 布局计算 ---
    readonly property real contentRealWidth: (root.barWidth + root.spacing) * (root.displayedHeights.length > 0 ? root.displayedHeights.length : 60) - root.spacing
    property real contentScale: (contentRealWidth > width && width > 0) ? (width / contentRealWidth) : 1.0

    // 1. 波形渲染器
    Row {
        id: waveRow
        anchors.centerIn: parent
        spacing: root.spacing

        scale: root.contentScale
        transformOrigin: Item.Center

        Repeater {
            model: root.displayedHeights

            Rectangle {
                id: bar
                width: root.barWidth > 0 ? root.barWidth : 4

                height: Math.max(0, modelData * root.globalMultiplier)

                radius: width / 2

                anchors.verticalCenter: parent.verticalCenter

                // [核心修改] 颜色逻辑
                color: {
                    var idxPct = index / root.displayedHeights.length;

                    if (root.isHovering) {
                        // 悬浮状态：显示从头到鼠标位置的 hoverColor
                        // 这会覆盖当前的播放进度显示，给用户明确的“跳转预览”
                        if (idxPct <= root.hoverProgress)
                            return root.hoverColor;
                        return root.remainingColor;
                    } else {
                        // 普通状态：显示播放进度
                        if (idxPct <= root.progress)
                            return root.playedColor;
                        return root.remainingColor;
                    }
                }

                // 颜色变化仍然保留平滑过渡
                Behavior on color {
                    ColorAnimation {
                        duration: 100
                    }
                }
            }
        }
    }

    // 2. 交互区域
    MouseArea {
        id: mouseArea
        anchors.fill: parent
        anchors.margins: -10

        // [新增] 开启悬浮感应
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor

        // 提取计算逻辑
        function calculateP(mouseX) {
            var localPos = mapToItem(waveRow, mouseX, 0);
            var unscaledWidth = waveRow.width;
            var p = localPos.x / unscaledWidth;
            if (p < 0)
                p = 0;
            if (p > 1)
                p = 1;
            return p;
        }

        function updateSeek(mouseX) {
            var p = calculateP(mouseX);
            root.seekRequested(p);
        }

        // [新增] 悬浮事件处理
        onEntered: {
            root.isHovering = true;
        }

        onExited: {
            root.isHovering = false;
        }

        onPositionChanged: {
            // 实时更新悬浮进度
            root.hoverProgress = calculateP(mouseX);

            // 如果正在拖拽，同时也更新实际 Seek
            if (pressed) {
                updateSeek(mouseX);
            }
        }

        onPressed: {
            root.pressed();
            updateSeek(mouseX);
        }

        onReleased: {
            root.released();
        }
    }
}
