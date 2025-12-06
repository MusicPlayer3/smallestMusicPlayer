import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects

Rectangle {
    id: listViewRect
    gradient: Gradient {
        GradientStop { position: 0.0; color: playerController.gradientColor1 } 
        GradientStop { position: 0.5; color: playerController.gradientColor2 } 
        GradientStop { position: 1.0; color: playerController.gradientColor3 } 
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
            
            // 绑定高度，确保 List 被挤压
            Layout.preferredHeight: height 
            
            // 提高 Z 值防止遮挡
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