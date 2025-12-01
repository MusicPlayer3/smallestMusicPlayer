import QtQuick
import QtQuick.Controls
import Qt5Compat.GraphicalEffects
import QtQuick.Effects
import QtQuick.Layouts

Window {
    width: 400
    height: 600
    visible: true
    title: "Music List Demo"

    // 1. 模拟数据模型 (Model)
    // 在实际项目中，这里通常推荐用 C++ (QAbstractListModel)
    // 但为了演示，我们用 ListModel 动态生成 2000 条数据
    ListModel {
        id: musicModel
        Component.onCompleted: {
            for (var i = 0; i < 2000; i++) {
                append({
                    "songTitle": "歌曲名称 " + (i + 1),
                    "artistName": "歌手 " + (i + 1),
                    // 使用一个占位图，实际换成你的图片路径 (file://...)
                    "coverUrl": "https://placehold.co/100x100/orange/white?text=" + (i + 1)
                })
            }
        }
    }

    // 2. 定义组件 (Delegate) - 每一行的外观
    Component {
        id: musicDelegate

        Rectangle {
            id: wrapper
            width: ListView.view.width // 跟随列表宽度
            height: 70
            color: ListView.isCurrentItem ? "#eee" : "white" // 选中高亮

            // 鼠标点击交互
            MouseArea {
                anchors.fill: parent
                onClicked: wrapper.ListView.view.currentIndex = index
            }

            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 15

                // [封面图片] - 关键优化区
                Image {
                    Layout.preferredWidth: 50
                    Layout.preferredHeight: 50
                    source: model.coverUrl

                    // 核心优化 1: 异步加载，防止滚动卡顿
                    asynchronous: true

                    // 核心优化 2: 限制缓存大小，防止大图撑爆内存
                    // 即使原图是 4000x4000，这里也只按 50x50 占用内存
                    sourceSize: Qt.size(50, 50)

                    fillMode: Image.PreserveAspectCrop

                    // 加个圆角遮罩 (可选，增加美观)
                    layer.enabled: true
                    layer.effect: OpacityMask {
                        maskSource: Rectangle {
                            width: 50; height: 50; radius: 5
                        }
                    }
                }

                // [文字信息]
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Text {
                        text: model.songTitle
                        font.pixelSize: 16
                        font.bold: true
                        elide: Text.ElideRight // 文字太长自动省略号
                        Layout.fillWidth: true
                    }

                    Text {
                        text: model.artistName
                        font.pixelSize: 12
                        color: "gray"
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }
            }

            // 分割线
            Rectangle {
                width: parent.width - 20
                height: 1
                color: "#f0f0f0"
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
            }
        }
    }

    // 3. 列表视图 (ListView)
    ListView {
        id: listView
        anchors.fill: parent

        // 绑定模型和组件
        model: musicModel
        delegate: musicDelegate

        // 性能优化参数
        clip: true // 必须为 true，否则滚动时超出区域的内容仍会渲染

        // 预加载缓冲区：在屏幕可视区域外多渲染多少像素
        // 设得太大内存高，设得太小滚动快了会看到白块。默认通常够用。
        cacheBuffer: 200

        // 滚动条 (QtQuick.Controls 2)
        ScrollBar.vertical: ScrollBar { }
    }
}
