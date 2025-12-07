import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects
import QtQuick.Window // [新增]

Rectangle {
    id: delegateRoot
    width: ListView.view ? ListView.view.width : 400
    height: 60

    color: mouseArea.pressed ? "#50555B" : (mouseArea.containsMouse ? "#40444A" : "transparent")

    property url itemImageSource: ""
    property string itemTitle: ""
    property string itemArtist: ""
    property bool isItemPlaying: false
    property string iconFontFamily: ""
    property bool isFolder: false

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true 
        onClicked: {
            musicListModel.handleClick(index)
        }
    }

    RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10 
        layoutDirection: Qt.LeftToRight

        Rectangle {
            id: albumRect
            Layout.preferredWidth: 40
            Layout.preferredHeight: 40
            radius: 4
            color: "#dddddd"
            clip: true

            Image {
                id: albumCover
                anchors.fill: parent
                source: itemImageSource 
                fillMode: Image.PreserveAspectCrop
                
                // [优化]：列表图片源尺寸根据屏幕密度缩放，防止加载过小的图导致放大模糊
                sourceSize: Qt.size(50 * Screen.devicePixelRatio, 50 * Screen.devicePixelRatio) 
                
                asynchronous: true
                smooth: true 
                mipmap: true // 列表小图开启 mipmap 有助于平滑

                layer.enabled: true
                
                // [优化]：设置纹理大小适配屏幕缩放 (1:1 物理像素)
                layer.textureSize: Qt.size(width * Screen.devicePixelRatio, height * Screen.devicePixelRatio)
                
                layer.effect: OpacityMask{
                    anchors.fill: parent
                    maskSource: Rectangle {
                        width: albumCover.width
                        height: albumCover.height
                        radius: 4      
                        color: "white" 
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.preferredWidth: 0
            Layout.preferredHeight: parent.height
            Layout.alignment: Qt.AlignVCenter
            spacing: 2 

            Text {
                text: itemTitle
                font.pixelSize: 14
                color: "white" 
                elide: Text.ElideRight 
                maximumLineCount: 1
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignLeft
            }

            Text {
                text: itemArtist
                font.pixelSize: 12
                color: "#AAAAAA" 
                elide: Text.ElideRight
                maximumLineCount: 1
                visible: !isFolder
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignLeft
            }
        }

        Text {
            visible: isItemPlaying 
            text: "audiotrack" 
            font.pixelSize: 16
            color: "#66CCFF"
            Layout.preferredWidth: 20
            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
            font.family: iconFontFamily
        }
    }
}