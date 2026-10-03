import QtQuick 2.9
import QtQuick.Controls 2.2

import StreamingPreferences 1.0

// Dropdown of the saved video presets. The first entry, "Current settings", stands for
// settings that don't match any preset. Choosing a preset applies it to StreamingPreferences
// and emits presetApplied() so the owner can refresh whatever shows those settings.
//
// Call initialize() once the control is created.
AutoResizingComboBox {
    id: presetComboBox
    textRole: "text"
    model: ListModel {
        id: presetListModel
    }

    // Set while a preset is being applied, so the half-applied
    // values don't bounce the selection back to "Current settings"
    property bool applying: false

    // Label the "Current settings" entry with what the settings are, like
    // "Current: 1440p @ 120 FPS / HEVC / 200 Mbps / HDR", instead of a generic name
    property bool showCurrentName: false

    // Emitted after a preset has been applied, while the selection is still being held
    signal presetApplied()

    function currentEntryText() {
        return showCurrentName ? qsTr("Current: %1").arg(StreamingPreferences.suggestedVideoPresetName())
                               : qsTr("Current settings")
    }

    function selectedPresetName() {
        return currentIndex > 0 ? presetListModel.get(currentIndex).name : ""
    }

    // Rebuilds the list from the saved presets and selects selectName,
    // or whichever preset matches the current settings if it's empty
    function reload(selectName) {
        presetListModel.clear()
        presetListModel.append({ "text": currentEntryText(), "name": "" })

        var names = StreamingPreferences.videoPresetNames()
        for (var i = 0; i < names.length; i++) {
            presetListModel.append({ "text": names[i], "name": names[i] })
        }

        recalculateWidth()
        updateSelection(selectName)
    }

    function updateSelection(preferredName) {
        if (applying) {
            return
        }

        // The label follows the settings, and the control resizes to fit it
        if (showCurrentName && presetListModel.count > 0) {
            presetListModel.setProperty(0, "text", currentEntryText())
            recalculateWidth()
        }

        var wanted = preferredName ? preferredName : StreamingPreferences.matchingVideoPreset()
        currentIndex = 0
        for (var i = 1; i < presetListModel.count; i++) {
            if (presetListModel.get(i).name === wanted) {
                currentIndex = i
                break
            }
        }
    }

    function initialize() {
        reload("")

        // Fall back to "Current settings" as soon as a setting no
        // longer matches the selected preset
        StreamingPreferences.displayModeChanged.connect(updateSelection)
        StreamingPreferences.bitrateChanged.connect(updateSelection)
        StreamingPreferences.unlockBitrateChanged.connect(updateSelection)
        StreamingPreferences.enableVsyncChanged.connect(updateSelection)
        StreamingPreferences.framePacingChanged.connect(updateSelection)
        StreamingPreferences.videoCodecConfigChanged.connect(updateSelection)
        StreamingPreferences.enableHdrChanged.connect(updateSelection)
        StreamingPreferences.enableYUV444Changed.connect(updateSelection)
        StreamingPreferences.windowModeChanged.connect(updateSelection)
    }

    // ::onActivated must be used, as it only listens for when the index is changed by a human
    onActivated: {
        var name = selectedPresetName()
        if (name) {
            applying = true
            StreamingPreferences.applyVideoPreset(name)
            presetApplied()
            applying = false
        }

        // Settle on the preset (or "Current settings") that matches what is now set
        updateSelection(name)
        recalculateWidth()
    }

    // The gamepad's A button sends Return outside of UI navigation mode (like on the
    // apps screen), but a ComboBox only opens its popup on Space. A ComboBox also toggles
    // its popup closed when Return is released while the popup is showing, and the gamepad
    // sends the release right after the press, so opening on the press would close it again
    // immediately. Open on the release instead, but only if the matching press found the
    // popup closed, so the release that follows picking an entry doesn't reopen it.
    property bool returnArmed: false

    Keys.onReturnPressed: {
        returnArmed = !popup.visible
        event.accepted = returnArmed
    }

    Keys.onEnterPressed: {
        returnArmed = !popup.visible
        event.accepted = returnArmed
    }

    Keys.onReleased: {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            if (returnArmed && !event.isAutoRepeat) {
                popup.open()
                event.accepted = true
            }
            returnArmed = false
        }
    }

    hoverEnabled: true
    ToolTip.delay: 1000
    ToolTip.timeout: 5000
    ToolTip.visible: hovered
    // Show the full name of the selected preset, since long names get cut off
    ToolTip.text: currentIndex > 0 ?
                      selectedPresetName()
                    :
                      qsTr("Saved groups of video settings. Choosing one applies its resolution, frame rate, codec, bitrate, display mode, V-Sync, frame pacing, HDR, and YUV 4:4:4 settings.")
}
