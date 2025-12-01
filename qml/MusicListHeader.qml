import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {

    id:musicListHeader
    // 表头样式，与图片保持一致的深灰色
    color: "#3C3F46"
    width: parent.width // 确保宽度跟随父级 (ListView)
    height: 50 // 设定一个高度
    // 允许外部设置总时间文本
    property alias totalDurationText: durationText.text

    property string iconFontFamily: ""

    signal closeRequested // 定义一个信号，表示请求关闭列表

    // 左右边距常量
    readonly property int margin: 10

    // 使用 RowLayout 实现水平布局
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 10

        // --- 1. 左侧按钮 (放大镜/搜索) ---
        Button {
            id: searchButton
            // 按钮透明
            background: Rectangle { color: "transparent" }
            // 按钮图标 (假设有一个 search.png 或使用 Font Awesome/Material Icons)
            // 这里我们用一个 Text 来模拟放大镜图标
            Text {
                text: "search"
                font.pixelSize: 20
                color: "#CCCCCC"
                anchors.centerIn: parent
                font.family:musicListHeader.iconFontFamily
            }
            Layout.preferredWidth: 30
            Layout.preferredHeight: 30
        }

        // **[关键修改 1]：左侧填充 Item**
        // 使用 Item 填充左侧空间，将中间内容推向右侧
        Item {
            Layout.fillWidth: true
        }

        // --- 2. 中间标题和时间 ---
        ColumnLayout {
            //Layout.fillWidth: true
            spacing: 0

            // 标题
            Text {
                text: "播放列表"
                font.pixelSize: 16
                color: "white"
                font.bold: true
                Layout.alignment: Qt.AlignHCenter // 居中显示
            }

            // 时间信息 (模仿图中的样式)
            Text {
                id: durationText
                text: "剩余 101 小时 11 分钟 分钟" // 默认值
                font.pixelSize: 12
                color: "#AAAAAA"
                Layout.alignment: Qt.AlignHCenter
            }
        }

        // **[关键修改 3]：右侧填充 Item**
        // 使用 Item 填充右侧空间，平衡左侧的 Item，从而使中间内容绝对居中
        Item {
            Layout.fillWidth: true
        }

        // --- 3. 右侧按钮 (打钩/完成) ---
        Button {
            id: doneButton
            background: Rectangle { color: "transparent" }
            // 模仿打钩图标
            Text {
                text: "done"
                font.pixelSize: 20
                color: "#CCCCCC"
                anchors.centerIn: parent
                font.family:musicListHeader.iconFontFamily
            }
            Layout.preferredWidth: 30
            Layout.preferredHeight: 30
            onClicked: {
                closeRequested()
            }
        }
    }

}
