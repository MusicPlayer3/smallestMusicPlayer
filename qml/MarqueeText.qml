import QtQuick

Item {
    id: root

    // =========================
    // 公共属性 (对外接口)
    // =========================
    property string text: ""                // 要显示的文本
    property color color: "white"           // 文本颜色
    property alias font: measurer.font      // 字体设置
    property real spacing: 50               // 滚动时首尾相接的间距
    
    // 默认居中，但滚动时会强制靠左
    property int horizontalAlignment: Text.AlignHCenter 

    // 修复：必须提供隐式高度，否则在 Column 等布局中高度会是 0
    implicitHeight: measurer.implicitHeight 
    implicitWidth: 300 // 给一个默认宽度，防止在某些布局中塌缩

    clip: true

    // 1. 测量文本 (隐藏)
    Text {
        id: measurer
        visible: false
        text: root.text
        font: root.font // 绑定外部字体设置
    }

    // 判断文本是否溢出
    readonly property bool isOverflow: measurer.implicitWidth > root.width

    // 2. 内容容器
    Item {
        id: container
        anchors.fill: parent
        
        // A. 未溢出时的静态文本 (使用对齐属性)
        Text {
            visible: !root.isOverflow
            anchors.fill: parent
            text: root.text
            color: root.color
            font: root.font
            horizontalAlignment: root.horizontalAlignment
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideNone
        }

        // B. 溢出时的滚动层
        Row {
            id: scrollContent
            visible: root.isOverflow
            height: root.height
            spacing: root.spacing
            
            // 初始位置为 0 (靠左对齐)
            x: 0

            // 文本副本 1
            Text {
                text: root.text
                color: root.color
                font: root.font
                height: root.height
                verticalAlignment: Text.AlignVCenter
            }

            // 文本副本 2 (尾随)
            Text {
                text: root.text
                color: root.color
                font: root.font
                height: root.height
                verticalAlignment: Text.AlignVCenter
            }
        }
    }

    // 3. 动画逻辑 (针对 scrollContent)
    SequentialAnimation {
        id: marqueeAnim
        // 只有当溢出且组件可见时才运行，避免后台资源浪费
        running: root.isOverflow && root.visible 
        loops: Animation.Infinite // 无限循环

        // 步骤 0: 确保从 0 开始 (Text控件的最左侧对齐)
        PropertyAction { target: scrollContent; property: "x"; value: 0 }

        // 步骤 1: 初始停留 (可选，让用户先看清开头，这里设 1s 缓冲)
        PauseAnimation { duration: 1000 }

        // 步骤 2: 滚动动画 (先慢，再快，再慢)
        NumberAnimation {
            target: scrollContent
            property: "x"
            from: 0
            // 滚动距离：移动一个 "文本宽度 + 间距" 的距离
            // 这样副本2会正好移动到副本1原本的位置，实现无缝衔接
            to: -(measurer.implicitWidth + root.spacing)
            
            // 速度控制：根据文本长度动态计算时间，保证不同长度文本速度感知一致
            duration: measurer.implicitWidth * 20 
            
            // 核心要求：先慢，再快，再慢
            easing.type: Easing.InOutQuad 
        }

        // 步骤 3: 滚动结束后，瞬间重置回 0 
        // 此时视觉上副本2在最左边，和副本1在最左边是一样的，所以瞬间重置用户无感知
        PropertyAction { target: scrollContent; property: "x"; value: 0 }

        // 步骤 4: 回到原位后暂停 3 秒 (核心要求)
        PauseAnimation { duration: 3000 }
    }
}