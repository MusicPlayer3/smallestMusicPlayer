import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects

Rectangle {
    id: listViewRect
    
    // (修复 1 & 2) 重新使用 Gradient，保证侧边栏是不透明的 (Opaque)
    // 这样在悬浮模式下，它会遮挡住下面的播放器。
    // 同时通过 Behavior 保证颜色变化与主界面同步。
    gradient: Gradient {
        GradientStop { 
            position: 0.0; 
            color: playerController.gradientColor1 
            Behavior on color { ColorAnimation { duration: 500; easing.type: Easing.Linear } }
        } 
        GradientStop { 
            position: 0.5; 
            color: playerController.gradientColor2 
            Behavior on color { ColorAnimation { duration: 500; easing.type: Easing.Linear } }
        } 
        GradientStop { 
            position: 1.0; 
            color: playerController.gradientColor3 
            Behavior on color { ColorAnimation { duration: 500; easing.type: Easing.Linear } }
        } 
    }
    
    // 背景光晕
    RectangularGlow {
        anchors.fill: listViewRect
        glowRadius: 10
        spread: 0.1
        color: "#80000000"
        cornerRadius: 20
        z:-1
    }

    signal closeRequested
    property string iconFamily: ""

    MouseArea {
        anchors.fill: parent
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- Header ---
        MusicListHeader {
            id: header
            iconFontFamily: listViewRect.iconFamily
            Layout.fillWidth: true
            
            Layout.preferredHeight: height 
            z: 2 

            onCloseRequested: {
                listViewRect.closeRequested() 
            }
        }

        // --- ListView ---
        ListView {
            id: musicListView
            Layout.fillWidth: true
            Layout.fillHeight: true 
            
            clip: true 
            z: 1

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }

            model: musicListModel

            delegate: MusicListItemDelegate {
                itemImageSource: model.imageSource
                itemTitle: model.title
                itemArtist: model.artist
                isItemPlaying: model.isPlaying
                iconFontFamily: listViewRect.iconFamily
                isFolder: model.isFolder 
            }
        }
    }
}