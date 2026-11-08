import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import OverlayMenuCommandModel 1.0
import KeyRecorder 1.0
import SdlGamepadKeyNavigation 1.0

// The in-stream menu's custom commands: a list to add to, edit, reorder and delete from
Item {
    id: commandsPage
    objectName: qsTr("Custom Commands")

    readonly property string iconFont: commandModel.iconFontFamily()

    function glyph(name) {
        return commandModel.iconGlyph(name)
    }

    function postActionText(action) {
        switch (action) {
        case 1: return qsTr("Closes the menu")
        case 2: return qsTr("Then disconnects")
        case 3: return qsTr("Then quits the session")
        default: return qsTr("Menu stays open")
        }
    }

    StackView.onActivated: {
        SdlGamepadKeyNavigation.setUiNavMode(true)
    }

    StackView.onDeactivating: {
        recorder.stop()
        SdlGamepadKeyNavigation.setUiNavMode(false)
    }

    OverlayMenuCommandModel {
        id: commandModel
    }

    KeyRecorder {
        id: recorder

        onChordRecorded: function(keys) {
            // A new array, so bindings to the steps see the change
            commandEditor.steps = commandEditor.steps.concat([keys])
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 10

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.pointSize: 12
            text: qsTr("Custom commands run along the bottom of the in-stream menu. Each one presses keys on the host, like a shortcut or a few in a row.")
        }

        ListView {
            id: commandList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 4
            model: commandModel

            ScrollBar.vertical: ScrollBar {}

            delegate: ItemDelegate {
                width: commandList.width
                height: 64

                onClicked: commandEditor.openFor(index)

                contentItem: RowLayout {
                    spacing: 12

                    Image {
                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32
                        fillMode: Image.PreserveAspectFit
                        source: model.icon !== "" ? commandModel.iconPreview(model.icon, "#ffffff", 64) : ""
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            Layout.fillWidth: true
                            text: model.label
                            font.pointSize: 13
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            text: model.keys + "  ·  " + postActionText(model.postAction)
                            font.pointSize: 10
                            opacity: 0.7
                            elide: Text.ElideRight
                        }
                    }

                    ToolButton {
                        text: glyph("arrow_upward")
                        font.family: iconFont
                        font.pixelSize: 22
                        enabled: index > 0
                        onClicked: commandModel.move(index, index - 1)

                        ToolTip.delay: 1000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Move left in the menu")
                    }

                    ToolButton {
                        text: glyph("arrow_downward")
                        font.family: iconFont
                        font.pixelSize: 22
                        enabled: index < commandList.count - 1
                        onClicked: commandModel.move(index, index + 1)

                        ToolTip.delay: 1000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Move right in the menu")
                    }

                    ToolButton {
                        text: glyph("delete")
                        font.family: iconFont
                        font.pixelSize: 22
                        onClicked: commandModel.remove(index)

                        ToolTip.delay: 1000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Delete")
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: commandList.count === 0
            wrapMode: Text.Wrap
            font.pointSize: 12
            opacity: 0.7
            text: qsTr("No custom commands yet.")
        }

        Button {
            text: qsTr("Add Command")
            onClicked: commandEditor.openFor(-1)
        }
    }

    NavigableDialog {
        id: commandEditor

        // The command's row, or -1 for a new one
        property int row: -1
        property string icon: ""
        property var steps: []

        title: row < 0 ? qsTr("Add Command") : qsTr("Edit Command")
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: Math.min(commandsPage.width - 40, 560)

        // Escape is a key to record while recording
        closePolicy: recorder.recording ? Popup.NoAutoClose : (Popup.CloseOnEscape | Popup.CloseOnPressOutside)

        function openFor(index) {
            row = index

            var command = index >= 0 ? commandModel.get(index) : { name: "", icon: "", postAction: 0, steps: [] }
            nameField.text = command.name
            icon = command.icon
            steps = command.steps
            postActionBox.currentIndex = command.postAction
            open()
        }

        function updateOkButton() {
            // A command needs keys to send
            if (standardButton) {
                standardButton(Dialog.Ok).enabled = steps.length > 0
            }
        }

        onStepsChanged: updateOkButton()
        onOpened: updateOkButton()
        onAboutToHide: recorder.stop()

        onAccepted: {
            var command = {
                name: nameField.text,
                icon: icon,
                postAction: postActionBox.currentIndex,
                steps: steps
            }

            if (row < 0) {
                commandModel.add(command)
            }
            else {
                commandModel.update(row, command)
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            Label {
                text: qsTr("Keys")
                font.bold: true
            }

            Flow {
                Layout.fillWidth: true
                spacing: 6
                visible: commandEditor.steps.length > 0

                Repeater {
                    model: commandEditor.steps

                    delegate: Rectangle {
                        height: 32
                        width: chipRow.implicitWidth + 20
                        radius: 16
                        color: "#33ffffff"

                        RowLayout {
                            id: chipRow
                            anchors.centerIn: parent
                            spacing: 6

                            Label {
                                text: commandModel.describeKeys([modelData])
                            }

                            Label {
                                text: glyph("close")
                                font.family: iconFont
                                font.pixelSize: 16

                                MouseArea {
                                    anchors.fill: parent
                                    anchors.margins: -6
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        var steps = commandEditor.steps.slice()
                                        steps.splice(index, 1)
                                        commandEditor.steps = steps
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: commandEditor.steps.length === 0 && !recorder.recording
                wrapMode: Text.Wrap
                opacity: 0.7
                text: qsTr("No keys yet. Press Record, then press each shortcut to send, one after another.")
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Button {
                    text: recorder.recording ? qsTr("Stop Recording") : qsTr("Record")
                    highlighted: recorder.recording
                    onClicked: {
                        if (recorder.recording) {
                            recorder.stop()
                        }
                        else {
                            recorder.start()

                            // For keys that come through QML rather than the system's hook
                            keyCatcher.forceActiveFocus()
                        }
                    }
                }

                Button {
                    text: qsTr("Clear")
                    enabled: commandEditor.steps.length > 0
                    onClicked: commandEditor.steps = []
                }

                Label {
                    Layout.fillWidth: true
                    visible: recorder.recording
                    elide: Text.ElideRight
                    text: recorder.pressedKeys !== "" ? recorder.pressedKeys : qsTr("Press keys…")
                }
            }

            Label {
                text: qsTr("Name")
                font.bold: true
            }

            TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: commandEditor.steps.length > 0 ? commandModel.describeKeys(commandEditor.steps) : qsTr("Shown in the menu")
            }

            Label {
                text: qsTr("Icon")
                font.bold: true
            }

            RowLayout {
                spacing: 10

                Image {
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32
                    fillMode: Image.PreserveAspectFit
                    visible: commandEditor.icon !== ""
                    source: commandEditor.icon !== "" ? commandModel.iconPreview(commandEditor.icon, "#ffffff", 64) : ""
                }

                Button {
                    text: commandEditor.icon !== "" ? qsTr("Change…") : qsTr("Choose…")
                    onClicked: iconChooser.open()
                }

                Button {
                    text: qsTr("None")
                    visible: commandEditor.icon !== ""
                    onClicked: commandEditor.icon = ""
                }
            }

            Label {
                text: qsTr("After sending the keys")
                font.bold: true
            }

            ComboBox {
                id: postActionBox
                Layout.fillWidth: true
                model: [
                    qsTr("Keep the menu open"),
                    qsTr("Close the menu first"),
                    qsTr("Disconnect"),
                    qsTr("Quit the session")
                ]
            }

            // Takes the keys while recording, where the system's hook doesn't
            Item {
                id: keyCatcher
                width: 0
                height: 0

                Keys.onPressed: function(event) {
                    if (recorder.recording) {
                        recorder.keyEvent(event.key, event.nativeVirtualKey, true, event.isAutoRepeat)
                        event.accepted = true
                    }
                }

                Keys.onReleased: function(event) {
                    if (recorder.recording) {
                        recorder.keyEvent(event.key, event.nativeVirtualKey, false, event.isAutoRepeat)
                        event.accepted = true
                    }
                }
            }
        }
    }

    NavigableDialog {
        id: iconChooser

        property string importError: ""

        title: qsTr("Choose an Icon")
        standardButtons: Dialog.Cancel
        width: Math.min(commandsPage.width - 40, 760)
        height: Math.min(commandsPage.height - 40, 640)

        onOpened: {
            importError = ""
            searchField.text = ""
            searchField.forceActiveFocus()
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                TextField {
                    id: searchField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Search icons")
                }

                Button {
                    text: qsTr("Use My Own Image…")
                    visible: fileDialogLoader.status === Loader.Ready
                    onClicked: fileDialogLoader.item.open()
                }

                CheckBox {
                    id: keepColorsCheck
                    text: qsTr("Keep its colours")
                    visible: fileDialogLoader.status === Loader.Ready

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Otherwise the image's shape is drawn in the menu's colours, like the other icons")
                }
            }

            Label {
                Layout.fillWidth: true
                visible: iconChooser.importError !== ""
                wrapMode: Text.Wrap
                color: "#ff8a80"
                text: iconChooser.importError
            }

            GridView {
                id: iconGrid
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                cellWidth: 64
                cellHeight: 64
                model: commandModel.searchIcons(searchField.text)

                ScrollBar.vertical: ScrollBar {}

                delegate: ItemDelegate {
                    width: 64
                    height: 64

                    contentItem: Text {
                        text: glyph(modelData)
                        font.family: iconFont
                        font.pixelSize: 32
                        color: "white"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    ToolTip.delay: 500
                    ToolTip.visible: hovered
                    ToolTip.text: modelData.replace(/_/g, " ")

                    onClicked: {
                        commandEditor.icon = "material:" + modelData
                        iconChooser.close()
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: iconGrid.count === 0
                opacity: 0.7
                text: qsTr("No icons match.")
            }
        }
    }

    // QtQuick.Dialogs' file dialog, where there is one (Qt 6)
    Loader {
        id: fileDialogLoader
        source: commandModel.canImportIcons() ? "qrc:/gui/IconFileDialog.qml" : ""
    }

    Connections {
        target: fileDialogLoader.item
        ignoreUnknownSignals: true

        function onChosen(file) {
            var icon = commandModel.importIcon(file, keepColorsCheck.checked)
            if (icon !== "") {
                commandEditor.icon = icon
                iconChooser.close()
            }
            else {
                iconChooser.importError = qsTr("That file couldn't be used as an icon. Choose an SVG, PNG or other image of up to 4 MB.")
            }
        }
    }
}
