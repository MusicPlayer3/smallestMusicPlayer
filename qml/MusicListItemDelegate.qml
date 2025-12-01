import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects

Rectangle {
    width: ListView.view ? ListView.view.width : 400
    // 列表项背景色，默认透明
    color: "transparent"
    // 高度，用于列表视图计算
    height: 60

    // 自定义属性，用于接收 Model 数据
    property url itemImageSource: ""
    property string itemTitle: ""
    property string itemArtist: ""
    // 新增属性：标记是否正在播放，用于显示播放图标
    property bool isItemPlaying: false

    property string iconFontFamily: ""

    // 鼠标区域用于处理点击事件和悬停效果
    MouseArea {
        anchors.fill: parent
        // 鼠标悬停时变色
        onEntered: parent.color = "#40444A"
        onExited: parent.color = "transparent"
        onClicked: {
            console.log("Clicked:", itemTitle)
            // 实际应用中：在这里调用 C++ 后端的方法来播放此歌曲
        }
    }



    RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10 // 内部控件之间的间距
        layoutDirection: Qt.LeftToRight
        //rowWrapPolicy: RowLayout.NoWrap

        //--- 1. 左侧图片 ---
        /*Image {
            id: albumCover
            source: itemImageSource
            Layout.preferredWidth: 40
            Layout.preferredHeight: 40
            // 保持图片的宽高比
            fillMode: Image.PreserveAspectCrop
            // 让图片看起来像一个圆角/方形
            clip: true
            layer.enabled: true
            layer.effect: OpacityMask{
                anchors.fill: parent
                maskSource: Rectangle {
                    width: albumCover.width
                    height: albumCover.height
                    radius: 20      // 使用同样的圆角
                    color: "white"  // white = 不透明区域
                }
            }
        }*/

        Rectangle {
            id: albumRect
            // width: 50
            // height: 50
            Layout.preferredWidth: 40
            Layout.preferredHeight: 40

            radius: 10
            color: "#dddddd"
            clip: true

            // 这里放你的 Image 控件
            Image {
                id: albumCover
                anchors.fill: parent
                source: itemImageSource // 替换为后端提供的封面 URL, 记得到时候把下面的false给取消了, 还有下面那个Text
                fillMode: Image.PreserveAspectCrop

                layer.enabled: true
                layer.effect: OpacityMask{
                    anchors.fill: parent
                    maskSource: Rectangle {
                        width: albumCover.width
                        height: albumCover.height
                        radius: 10      // 使用同样的圆角
                        color: "white"  // white = 不透明区域
                    }
                }
            }
        }


        // --- 2. 中间标题和艺术家 (上下堆叠) ---
        ColumnLayout {
            Layout.fillWidth: true
            Layout.preferredWidth: 0   // 关键：允许它压缩但也能被拉伸
            Layout.preferredHeight: parent.height

            // 垂直居中对齐
            Layout.alignment: Qt.AlignVCenter
            spacing: 2 // 上下 Text 之间的微小间距

            // 歌曲标题
            Text {
                text: itemTitle
                font.pixelSize: 14
                color: "white" // 白色标题
                elide: Text.ElideRight // 文本过长时显示省略号
                maximumLineCount: 1

                Layout.fillWidth: true       // ← 关键
                horizontalAlignment: Text.AlignLeft   // ← 关键
            }

            // 艺术家/作者
            Text {
                text: itemArtist
                font.pixelSize: 12
                color: "#AAAAAA" // 浅灰色作者
                elide: Text.ElideRight
                maximumLineCount: 1

                Layout.fillWidth: true       // ← 关键
                horizontalAlignment: Text.AlignLeft   // ← 关键
            }
        }

        // --- 3. 右侧正在播放图标 (可选) ---
        // 模仿 SARD UNDERGROUND 那一行右边的播放图标
        Text {
            visible: isItemPlaying // 只有当 isItemPlaying 为 true 时才显示
            text: "audiotrack" // 用一个简单的符号模拟播放图标
            font.pixelSize: 16
            color: "#66CCFF"

            Layout.preferredWidth: 20
            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
            font.family: iconFontFamily
        }
    }
}
