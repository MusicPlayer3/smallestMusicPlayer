import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects

Window {
    id: infoWin
    width: 320
    height: 400
    visible: false
    title: "详细信息"
    flags: Qt.Dialog | Qt.WindowCloseButtonHint | Qt.CustomizeWindowHint
    modality: Qt.ApplicationModal
    color: "#2b2b2b"

    // 接收 C++ 返回的 Map 数据
    property var infoData: ({})

    // 存储当前操作的列表索引
    property int targetListIndex: -1

    // [修复逻辑] 内部星级交互状态
    property bool isStarHovering: false
    property int currentHoverRating: 0

    function showInfo(data, index) {
        infoData = data;

        if (infoData.rating === undefined) {
            infoData.rating = 0;
        }

        targetListIndex = index;

        // [关键修复] 每次打开窗口时，强制重置交互状态
        isStarHovering = false;
        currentHoverRating = 0;

        // 根据内容动态调整窗口高度
        if (data.isFolder === true)
            height = 380;
        else
            height = 480;

        show();
        requestActivate();
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 15

        // 1. 封面显示区域
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            width: 120
            height: 120
            color: "#333"
            radius: 8
            clip: true

            Image {
                anchors.fill: parent
                source: infoData.cover || ""
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                mipmap: true
            }
            border.color: "#444"
            border.width: 1
        }

        // 2. 分隔线
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: "#40FFFFFF"
        }

        // 3. 信息列表
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            rowSpacing: 8
            columnSpacing: 10

            // --- 通用信息 ---
            Text {
                text: "名称:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                text: infoData.title || "-"
                color: "white"
                font.pixelSize: 13
                Layout.fillWidth: true
                elide: Text.ElideRight
            }

            // --- 文件夹特有信息 ---
            Text {
                visible: infoData.isFolder === true
                text: "上级目录:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                visible: infoData.isFolder === true
                text: infoData.parentName || "-"
                color: "white"
                font.pixelSize: 13
                Layout.fillWidth: true
                elide: Text.ElideRight
            }

            Text {
                visible: infoData.isFolder === true
                text: "包含歌曲:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                visible: infoData.isFolder === true
                text: infoData.songCount || "0"
                color: "white"
                font.pixelSize: 13
            }

            Text {
                visible: infoData.isFolder === true
                text: "总时长:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                visible: infoData.isFolder === true
                text: infoData.totalDuration || "00:00"
                color: "white"
                font.pixelSize: 13
            }

            // --- 歌曲特有信息 ---
            Text {
                visible: infoData.isFolder === false
                text: "艺术家:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                visible: infoData.isFolder === false
                text: infoData.artist || "-"
                color: "white"
                font.pixelSize: 13
                Layout.fillWidth: true
                elide: Text.ElideRight
            }

            Text {
                visible: infoData.isFolder === false
                text: "专辑:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                visible: infoData.isFolder === false
                text: infoData.album || "-"
                color: "white"
                font.pixelSize: 13
                Layout.fillWidth: true
                elide: Text.ElideRight
            }

            Text {
                visible: infoData.isFolder === false
                text: "年份:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                visible: infoData.isFolder === false
                text: infoData.year || "-"
                color: "white"
                font.pixelSize: 13
            }

            Text {
                visible: infoData.isFolder === false
                text: "格式:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                visible: infoData.isFolder === false
                text: (infoData.format || "") + " (" + (infoData.bitDepth || "") + " / " + (infoData.sampleRate || "") + ")"
                color: "white"
                font.pixelSize: 13
            }

            Text {
                visible: infoData.isFolder === false
                text: "播放次数:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }
            Text {
                visible: infoData.isFolder === false
                text: infoData.playCount || "0"
                color: "white"
                font.pixelSize: 13
            }

            Text {
                visible: infoData.isFolder === false
                text: "评价星级:"
                color: "#AAAAAA"
                font.pixelSize: 13
            }

            // [核心修复] 星级交互区域
            // 使用 Item 包裹 Row，解决 anchors 报错问题
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                visible: infoData.isFolder === false

                // 星星排列
                Row {
                    id: starRow
                    spacing: 4
                    anchors.verticalCenter: parent.verticalCenter

                    Repeater {
                        model: 5
                        Text {
                            text: "star"
                            font.family: materialFont.name
                            font.pixelSize: 22

                            // 颜色逻辑：
                            // 1. 如果正在悬停(isStarHovering)，使用 currentHoverRating 判断
                            // 2. 否则使用真实的 infoData.rating 判断
                            color: {
                                var starLevel = index + 1;
                                var targetLevel = infoWin.isStarHovering ? infoWin.currentHoverRating : (infoData.rating || 0);
                                return starLevel <= targetLevel ? "#FFD700" : "#555";
                            }
                        }
                    }
                }

                // 单个 MouseArea 覆盖整个星星条，逻辑更稳定
                MouseArea {
                    id: starMouseArea
                    anchors.fill: starRow // 覆盖 Row 的大小
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor

                    // 计算鼠标当前在第几颗星
                    function calculateRating(mouseX) {
                        // 估算每颗星的宽度 (字体大小 + 间距)
                        // 为了更精确，这里简单的用宽度均分
                        var itemWidth = starRow.width / 5;
                        var r = Math.ceil(mouseX / itemWidth);
                        if (r < 0)
                            r = 0;
                        if (r > 5)
                            r = 5;
                        return r;
                    }

                    onPositionChanged: {
                        infoWin.isStarHovering = true;
                        infoWin.currentHoverRating = calculateRating(mouseX);
                    }

                    onExited: {
                        // [关键] 鼠标移出时，恢复原状
                        infoWin.isStarHovering = false;
                        infoWin.currentHoverRating = 0;
                    }

                    onClicked: {
                        var newRating = calculateRating(mouseX);

                        // 1. 更新 UI
                        infoData.rating = newRating;
                        infoData = infoData; // 触发刷新

                        // 点击后重置悬停状态，防止颜色卡住
                        // (或者保持悬停状态，看你喜欢哪种交互，这里选择保持悬停感)
                        infoWin.currentHoverRating = newRating;

                        // 2. 调用后端
                        if (infoWin.targetListIndex >= 0) {
                            musicListModel.setItemRating(infoWin.targetListIndex, newRating);
                        }
                    }
                }
            }
        }

        Item {
            Layout.fillHeight: true
        } // 弹簧

        Button {
            Layout.alignment: Qt.AlignHCenter
            text: "关闭"
            onClicked: infoWin.close()
            contentItem: Text {
                text: parent.text
                color: "white"
                horizontalAlignment: Text.AlignHCenter
            }
            background: Rectangle {
                color: parent.pressed ? "#50FFFFFF" : "#30FFFFFF"
                radius: 4
            }
        }
    }
}
