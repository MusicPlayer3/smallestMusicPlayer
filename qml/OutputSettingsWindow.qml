import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: settingsWin
    width: 300
    height: 320
    visible: false
    title: "Output Parameters"
    flags: Qt.Dialog | Qt.WindowCloseButtonHint | Qt.CustomizeWindowHint
    modality: Qt.ApplicationModal
    color: "#2b2b2b"

    property bool isApplying: false

    readonly property var sampleRates: [44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000]
    readonly property var formats: ["S16", "S32", "Float", "Double"]

    // 监听 C++ 反馈的信号 (500ms 后触发)
    Connections {
        target: playerController
        function onMixingParamsApplied(actualRate, actualFormatIndex) {
            // 如果窗口已经关闭了，就不做任何 UI 更新，防止报错
            if (!settingsWin.visible)
                return;

            // 回填实际参数
            updateComboSelection(rateCombo, actualRate);
            fmtCombo.currentIndex = actualFormatIndex;

            // 更新提示信息
            statusText.text = "Applied: " + actualRate + "Hz, " + formats[actualFormatIndex];
            statusText.color = "#4CAF50";

            // 启动 1s 后的自动关闭
            autoCloseTimer.restart();
        }
    }

    // 1s 后自动关闭的定时器
    Timer {
        id: autoCloseTimer
        interval: 1000
        repeat: false
        onTriggered: {
            settingsWin.close();
        }
    }

    // 窗口可见性改变处理 (处理用户手动关闭的情况)
    onVisibleChanged: {
        if (!visible) {
            // 窗口关闭时，重置所有状态
            isApplying = false;
            autoCloseTimer.stop();
        }
    }

    function openDialog() {
        // [逻辑更新] 打开时获取当前实际参数
        var currentParams = playerController.getCurrentDeviceParams();

        // 回填到 UI
        updateComboSelection(rateCombo, currentParams.sampleRate);
        if (currentParams.formatIndex >= 0 && currentParams.formatIndex < formats.length) {
            fmtCombo.currentIndex = currentParams.formatIndex;
        }

        // 重置 UI 状态
        statusText.text = "Please select parameters";
        statusText.color = "#AAAAAA";
        isApplying = false;

        settingsWin.show();
    }

    function updateComboSelection(combo, value) {
        for (var i = 0; i < combo.model.length; i++) {
            if (combo.model[i] === value) {
                combo.currentIndex = i;
                return;
            }
        }
        // 如果当前采样率不在列表中，默认选中一个（或者添加逻辑处理）
        if (combo.currentIndex === -1 && combo.count > 0)
            combo.currentIndex = 0;
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 15

        Text {
            text: "Sample Rate (Hz)"
            color: "white"
            font.pixelSize: 12
        }

        ComboBox {
            id: rateCombo
            Layout.fillWidth: true
            model: sampleRates
            // 在应用期间禁用交互
            enabled: !isApplying
        }

        Text {
            text: "Output Format"
            color: "white"
            font.pixelSize: 12
        }

        ComboBox {
            id: fmtCombo
            Layout.fillWidth: true
            model: formats
            // 在应用期间禁用交互
            enabled: !isApplying
        }

        Text {
            id: statusText
            text: "Please select parameters"
            color: "#AAAAAA"
            font.pixelSize: 12
            Layout.alignment: Qt.AlignHCenter
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 20

            Button {
                text: "Cancel"
                Layout.fillWidth: true
                // 应用期间禁用取消（或者允许取消但不触发后续逻辑，这里选禁用以防逻辑冲突）
                enabled: !isApplying
                onClicked: settingsWin.close()
            }

            Button {
                text: "Apply"
                Layout.fillWidth: true
                highlighted: true
                enabled: !isApplying
                onClicked: {
                    // 1. 锁定界面
                    settingsWin.isApplying = true;
                    statusText.text = "Applying...";
                    statusText.color = "white";

                    var rate = sampleRates[rateCombo.currentIndex];
                    var fmtIndex = fmtCombo.currentIndex;

                    // 2. 调用 C++ (C++ 会等待 500ms 后发射信号)
                    playerController.applyMixingParams(rate, fmtIndex);
                }
            }
        }
    }
}
