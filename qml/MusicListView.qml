import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 假设您有一个名为 MusicModel 的 C++ QAbstractListModel
// 用于提供数据给 ListView
// import my.backend 1.0 // 假设导入了您的 C++ 模块

Rectangle {
    id:listViewRect
    // 列表的背景颜色可以根据您的需要调整
    color: "#2B2E33"
    width: 300
    height: 600

    // *** 1. 定义一个信号来向上级传播关闭请求 ***
    signal closeRequested

    property string iconFamily: ""

    MouseArea {
        anchors.fill: parent
        // 不写任何 onClicked 逻辑，它会自动消耗点击事件，防止穿透
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- 1. 播放列表表头 (Header) ---
        MusicListHeader {
            id: header
            iconFontFamily: listViewRect.iconFamily
            Layout.fillWidth: true
            Layout.preferredHeight: 50 // 表头高度
            // 假设您希望在表头显示的总时间
            totalDurationText: "剩余 101 小时 11 分钟" // TODO:这里需要把我们的列表播放剩余时间给过完

            onCloseRequested: {
                listViewRect.closeRequested() // 接收到内部信号后，立即转发出去
            }
        }

        // --- 2. 音乐列表视图 (ListView) ---
        ListView {
            id: musicListView
            Layout.fillWidth: true
            Layout.fillHeight: true // 列表占据剩余空间


            // 滚动条可见性 (QtQuick.Controls 2.15+)
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AlwaysOn
            }

            // 使用一个空的 ListModel 作为初始演示
            // 实际使用时请替换为您的 C++ Model -----------------后面主要是在这个地方看看怎么把我们的数据运送过来，
            // 还有一个比较实际的问题就是我的 Image 到时候怎么从内存里面传进来, 然后使用我们写好的控件来显示?


            // **** TODO: 使用 C++ 暴露的模型 ****
            // 这里到时候把音乐列表项的信息给过来就可以了,视图在append()之后会自动添加上去的
            model: musicListModel

            // 列表项的代理 (Delegate)
            delegate: MusicListItemDelegate {
                // 将 ListModel 中的数据映射到 Delegate 的属性
                itemImageSource: model.imageSource
                itemTitle: model.title
                itemArtist: model.artist
                isItemPlaying: model.isPlaying
                iconFontFamily: listViewRect.iconFamily
            }
        }
    }
}
