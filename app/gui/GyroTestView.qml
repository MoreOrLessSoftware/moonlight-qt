import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import GamepadSettingsModel 1.0
import GyroTester 1.0
import SdlGamepadKeyNavigation 1.0

// A gamepad's gyro settings with a live test: a crosshair moved by the filtered gyro, like a game
// camera, and raw vs filtered readings for each axis
Item {
    id: gyroPage
    objectName: qsTr("Gyro Test")

    // The gamepad to select first (an index into the model's gamepads)
    property int initialIndex: 0

    readonly property var axisColors: ["#e57373", "#81c784", "#64b5f6"]
    readonly property var axisNames: [qsTr("Pitch (X)"), qsTr("Yaw (Y)"), qsTr("Roll (Z)")]
    readonly property real graphRangeDps: 20

    StackView.onActivated: {
        SdlGamepadKeyNavigation.setUiNavMode(true)
        gamepadModel.refresh()
        tester.active = true
    }

    StackView.onDeactivating: {
        tester.active = false
        SdlGamepadKeyNavigation.setUiNavMode(false)
    }

    GamepadSettingsModel {
        id: gamepadModel

        Component.onCompleted: {
            if (gyroPage.initialIndex >= 0 && gyroPage.initialIndex < gamepadNames.length) {
                currentIndex = gyroPage.initialIndex
            }
        }

        onGyroSettingsChanged: tester.reloadSettings()
    }

    GyroTester {
        id: tester
        deviceKey: gamepadModel.hasGyro ? gamepadModel.currentKey : ""

        onUpdated: graph.requestPaint()
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 30

        ScrollView {
            Layout.preferredWidth: 380
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            Column {
                width: parent.width
                spacing: 10

                ComboBox {
                    width: parent.width
                    model: gamepadModel.gamepadNames
                    currentIndex: gamepadModel.currentIndex
                    visible: gamepadModel.gamepadNames.length > 1
                    onCurrentIndexChanged: gamepadModel.currentIndex = currentIndex
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    font.pointSize: 12
                    text: gamepadModel.gamepadNames.length === 0 ? qsTr("Connect a gamepad with a gyro to test it.") :
                          !gamepadModel.hasGyro ? qsTr("%1 has no gyro.").arg(gamepadModel.currentName) :
                          gamepadModel.currentName
                }

                GyroSettingsControls {
                    width: parent.width
                    gamepadModel: gamepadModel
                    visible: gamepadModel.hasGyro
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    visible: gamepadModel.hasGyro
                    text: qsTr("Hold the gamepad still to see how much it jitters at rest, then move it to feel the aim. Changes are saved as you make them and apply from the next stream.")
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10
            visible: gamepadModel.hasGyro

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pointSize: 12
                visible: tester.receiving
                text: qsTr("Largest reading over the last second: %1 °/s filtered, %2 °/s raw")
                      .arg(tester.restFilteredDps.toFixed(2)).arg(tester.restRawDps.toFixed(2))
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pointSize: 12
                visible: !tester.receiving
                color: "#ffb74d"
                text: qsTr("Waiting for gyro readings… (they only arrive while this window has focus)")
            }

            GridLayout {
                columns: 2
                columnSpacing: 10
                rowSpacing: 2

                Label { text: qsTr("Drift removed:") }
                Label { text: tester.offsetText }
                Label { text: qsTr("Accelerometer:") }
                Label { text: tester.accelText }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 20

                // The crosshair: where the filtered gyro would have turned a camera
                Rectangle {
                    id: crosshairArea
                    Layout.preferredWidth: 220
                    Layout.preferredHeight: 220
                    color: "#202020"
                    border.color: "#606060"
                    clip: true

                    Rectangle { x: parent.width / 2; width: 1; height: parent.height; color: "#404040" }
                    Rectangle { y: parent.height / 2; height: 1; width: parent.width; color: "#404040" }

                    Item {
                        width: 24
                        height: 24
                        x: crosshairArea.width / 2 + tester.crosshairX / tester.crosshairRange * (crosshairArea.width / 2) - width / 2
                        y: crosshairArea.height / 2 + tester.crosshairY / tester.crosshairRange * (crosshairArea.height / 2) - height / 2

                        Rectangle { anchors.centerIn: parent; width: parent.width; height: 2; color: "#ffffff" }
                        Rectangle { anchors.centerIn: parent; width: 2; height: parent.height; color: "#ffffff" }
                    }
                }

                Column {
                    spacing: 10

                    Label {
                        width: 200
                        wrapMode: Text.Wrap
                        text: qsTr("The crosshair moves the way a camera would, %1° from the center to each edge.").arg(tester.crosshairRange)
                    }

                    Button {
                        text: qsTr("Recenter")
                        onClicked: tester.recenter()
                    }
                }
            }

            // Raw (grey) and filtered (colored) readings for each axis over the last couple of seconds
            Canvas {
                id: graph
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 240

                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    ctx.fillStyle = "#202020"
                    ctx.fillRect(0, 0, width, height)

                    var rowHeight = height / 3
                    var length = tester.traceLength
                    for (var axis = 0; axis < 3; axis++) {
                        var top = axis * rowHeight
                        var middle = top + rowHeight / 2

                        // Zero line and the row's edge
                        ctx.strokeStyle = "#404040"
                        ctx.lineWidth = 1
                        ctx.beginPath()
                        ctx.moveTo(0, middle)
                        ctx.lineTo(width, middle)
                        ctx.moveTo(0, top + rowHeight)
                        ctx.lineTo(width, top + rowHeight)
                        ctx.stroke()

                        var series = [tester.trace(axis, false), tester.trace(axis, true)]
                        for (var s = 0; s < 2; s++) {
                            var values = series[s]
                            ctx.strokeStyle = s === 0 ? "#808080" : gyroPage.axisColors[axis]
                            ctx.lineWidth = s === 0 ? 1 : 2
                            ctx.beginPath()
                            for (var i = 0; i < length; i++) {
                                var v = Math.max(-graphRangeDps, Math.min(graphRangeDps, values[i]))
                                var x = i * width / (length - 1)
                                var y = middle - v / graphRangeDps * (rowHeight / 2 - 2)
                                if (i === 0) {
                                    ctx.moveTo(x, y)
                                }
                                else {
                                    ctx.lineTo(x, y)
                                }
                            }
                            ctx.stroke()
                        }

                        ctx.fillStyle = gyroPage.axisColors[axis]
                        ctx.font = "12px sans-serif"
                        ctx.fillText(gyroPage.axisNames[axis] + "  ±" + graphRangeDps + " °/s", 6, top + 14)
                    }
                }
            }
        }
    }
}
