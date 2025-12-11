import QtQuick
import QtQuick.Controls

// 独立窗口气泡
Window {
    id: root

    // Qt.Popup: 弹出窗口特性
    // Qt.WindowStaysOnTopHint: 确保在最上层
    flags: Qt.Popup | Qt.FramelessWindowHint | Qt.NoDropShadowWindowHint | Qt.WindowStaysOnTopHint

    color: "transparent"
    visible: false

    default property alias content: contentStack.children

    // 0: 下方, 1: 右侧
    property int arrowDirection: 0

    // [修改] 背景颜色改浅，使其不那么突兀
    property color backgroundColor: "#454545"
    property int borderRadius: 8

    readonly property int arrowSize: 12
    readonly property int arrowOffset: 12
    // [修改] 减小内边距，让内容更贴边 (从 8 改为 4)
    readonly property int contentPadding: 4

    // --- 背景绘制 ---
    Item {
        id: bgContainer
        anchors.fill: parent

        // 1. 箭头
        Rectangle {
            id: arrowRect
            rotation: 45
            width: root.arrowSize
            height: root.arrowSize
            color: root.backgroundColor

            x: root.arrowDirection === 0 ? (bgBody.x + bgBody.width / 2 - width / 2) : (bgBody.x - width / 2 + 1)
            y: root.arrowDirection === 0 ? (bgBody.y + bgBody.height - height / 2 + 1) : (bgBody.y + 25)
        }

        // 2. 主体背景
        Rectangle {
            id: bgBody
            anchors.fill: parent
            anchors.bottomMargin: root.arrowDirection === 0 ? root.arrowOffset : 0
            anchors.leftMargin: root.arrowDirection === 1 ? root.arrowOffset : 0

            color: root.backgroundColor
            radius: root.borderRadius
        }
    }

    // --- 内容容器 ---
    width: contentContainer.width
    height: contentContainer.height

    Item {
        id: contentContainer
        // [修改] 宽度计算：内容宽 + 箭头(如果有) + 左右极窄边距
        width: contentStack.implicitWidth + (root.arrowDirection === 1 ? root.arrowOffset : 0) + (root.contentPadding * 2)
        height: contentStack.implicitHeight + (root.arrowDirection === 0 ? root.arrowOffset : 0) + (root.contentPadding * 2)

        Column {
            id: contentStack
            // 偏移内容
            x: (root.arrowDirection === 1 ? root.arrowOffset : 0) + root.contentPadding
            y: root.contentPadding

            // 宽度自适应或限制
            width: (root.width > 0 && root.width !== contentContainer.width) ? (root.width - (root.arrowDirection === 1 ? root.arrowOffset : 0) - (root.contentPadding * 2)) : undefined

            spacing: 0
        }
    }

    // 动画
    OpacityAnimator {
        target: root
        from: 0
        to: 1
        duration: 150
        running: root.visible
    }

    // --- 定位逻辑 ---
    function showAtTarget(targetItem, direction) {
        if (direction === undefined)
            direction = 0;
        root.arrowDirection = direction;

        var globalPos = targetItem.mapToGlobal(0, 0);
        if (globalPos.x === 0 && globalPos.y === 0 && targetItem.width === 0)
            return;

        var tx = globalPos.x;
        var ty = globalPos.y;
        var tw = targetItem.width;

        if (direction === 0) {
            root.x = tx + (tw - root.width) / 2;
            root.y = ty - root.height + 5;
        } else {
            root.x = tx + tw - 5;
            root.y = ty - 20;
        }

        root.show();
        root.requestActivate();
    }

    function close() {
        root.visible = false;
    }

    function toggle(targetItem, direction) {
        if (root.visible) {
            root.close();
        } else {
            showAtTarget(targetItem, direction);
        }
    }

    onActiveChanged: {
        if (!active && visible)
            close();
    }
}
