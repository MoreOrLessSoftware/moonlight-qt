import QtQuick 2.9
import QtQuick.Controls 2.2

// The gyro settings of the gamepad selected in a GamepadSettingsModel. Used by the gamepad
// settings on the Input tab and by the gyro test page.
Column {
    id: controls

    // A GamepadSettingsModel
    property var gamepadModel

    spacing: 5

    CheckBox {
        id: filterCheck
        width: parent.width
        text: qsTr("Filter gyro and accelerometer readings")
        font.pointSize: 12
        checked: gamepadModel.gyroFilterEnabled
        onClicked: gamepadModel.gyroFilterEnabled = checked

        ToolTip.delay: 1000
        ToolTip.timeout: 5000
        ToolTip.visible: hovered
        ToolTip.text: qsTr("For gamepads whose gyro drifts or jitters at rest. Off sends the readings to the host as they are.")
    }

    Column {
        width: parent.width
        spacing: 5
        enabled: gamepadModel.gyroFilterEnabled
        leftPadding: 20

        CheckBox {
            width: parent.width - parent.leftPadding
            text: qsTr("Remove drift")
            font.pointSize: 12
            checked: gamepadModel.gyroRemoveOffset
            onClicked: gamepadModel.gyroRemoveOffset = checked

            ToolTip.delay: 1000
            ToolTip.timeout: 5000
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Measures what the gyro reads while the gamepad is held still, and subtracts it from every reading")
        }

        Label {
            width: parent.width - parent.leftPadding
            text: gamepadModel.gyroSmoothSamples > 1 && gamepadModel.gyroSmoothBelowDps > 0 ?
                      qsTr("Smoothing: average of %1 readings below %2 °/s").arg(gamepadModel.gyroSmoothSamples).arg(gamepadModel.gyroSmoothBelowDps) :
                      qsTr("Smoothing: off")
            font.pointSize: 12
            wrapMode: Text.Wrap
        }

        Row {
            spacing: 10

            Label {
                text: qsTr("Readings")
                width: 90
                anchors.verticalCenter: parent.verticalCenter
            }

            Slider {
                width: 200
                from: 1
                to: gamepadModel.gyroMaxSmoothSamples
                stepSize: 1
                snapMode: Slider.SnapAlways
                value: gamepadModel.gyroSmoothSamples
                onMoved: gamepadModel.gyroSmoothSamples = value

                ToolTip.delay: 1000
                ToolTip.timeout: 5000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("More readings means less jitter but more delay on slow movement")
            }
        }

        Row {
            spacing: 10

            Label {
                text: qsTr("Below °/s")
                width: 90
                anchors.verticalCenter: parent.verticalCenter
            }

            Slider {
                width: 200
                from: 0
                to: 20
                stepSize: 1
                snapMode: Slider.SnapAlways
                value: gamepadModel.gyroSmoothBelowDps
                onMoved: gamepadModel.gyroSmoothBelowDps = value

                ToolTip.delay: 1000
                ToolTip.timeout: 5000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Movement slower than this is smoothed; from twice this speed it isn't smoothed at all")
            }
        }

        Label {
            width: parent.width - parent.leftPadding
            text: gamepadModel.gyroDeadzoneDps > 0 ?
                      qsTr("Deadzone: %1 °/s").arg(gamepadModel.gyroDeadzoneDps) :
                      qsTr("Deadzone: off")
            font.pointSize: 12
        }

        Row {
            spacing: 10

            Label {
                text: qsTr("°/s")
                width: 90
                anchors.verticalCenter: parent.verticalCenter
            }

            Slider {
                width: 200
                from: 0
                to: 15
                stepSize: 1
                snapMode: Slider.SnapAlways
                value: gamepadModel.gyroDeadzoneDps
                onMoved: gamepadModel.gyroDeadzoneDps = value

                ToolTip.delay: 1000
                ToolTip.timeout: 5000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Movement slower than this is scaled down rather than cut off, so slow aiming still moves")
            }
        }

        CheckBox {
            width: parent.width - parent.leftPadding
            text: qsTr("Correct the accelerometer")
            font.pointSize: 12
            checked: gamepadModel.gyroCorrectAccel
            onClicked: gamepadModel.gyroCorrectAccel = checked

            ToolTip.delay: 1000
            ToolTip.timeout: 5000
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Scales it to read exactly 1 g while held still, and smooths its jitter")
        }

        Button {
            text: qsTr("Reset to Defaults")
            onClicked: gamepadModel.resetGyroSettings()
        }
    }
}
