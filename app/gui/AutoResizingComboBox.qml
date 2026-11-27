import QtQuick 2.9
import QtQuick.Controls 2.2

import SdlGamepadKeyNavigation 1.0
import SystemProperties 1.0

// https://stackoverflow.com/questions/45029968/how-do-i-set-the-combobox-width-to-fit-the-largest-item
ComboBox {
    property int textWidth
    property int desiredWidth : leftPadding + textWidth + indicator.width + rightPadding
    property int maximumWidth : parent.width

    // Left and right never change the selection (that's done in the popup). By
    // default they're left to the page, like settings, which moves between its
    // sections. This makes them move to the neighboring controls instead, like
    // in the toolbar.
    property bool arrowKeysMoveFocus: false

    // The UI navigation mode to restore when the popup closes. Pages that use
    // Tab-based navigation (like settings) want it on, the rest want it off.
    property bool uiNavModeOnClose: true

    implicitWidth: desiredWidth < maximumWidth ? desiredWidth : maximumWidth

    TextMetrics {
        id: popupMetrics
    }

    TextMetrics {
        id: textMetrics
    }

    function recalculateWidth() {
        textMetrics.font = font
        popupMetrics.font = popup.font
        textWidth = 0
        for (var i = 0; i < count; i++){
            textMetrics.text = textAt(i)
            popupMetrics.text = textAt(i)
            textWidth = Math.max(textMetrics.width, textWidth)
            textWidth = Math.max(popupMetrics.width, textWidth)
        }
    }

    // We call this every time the options change (and init)
    // so we can adjust the combo box width here too
    onActivated: recalculateWidth()

    popup.onAboutToShow: {
        // Switch to normal navigation for combo boxes
        SdlGamepadKeyNavigation.setUiNavMode(false)

        // Override the popup color to improve contrast with the overridden
        // Material 2 background color set in main.qml.
        if (SystemProperties.usesMaterial3Theme) {
            popup.background.color = "#424242"
        }
    }

    popup.onAboutToHide: {
        SdlGamepadKeyNavigation.setUiNavMode(uiNavModeOnClose)
    }

    // Keeps the keys while the popup is open, so focus can't leave it
    Keys.onLeftPressed: function(event) {
        if (popup.visible) {
            return
        }
        if (arrowKeysMoveFocus) {
            nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocus)
        }
        else {
            event.accepted = false
        }
    }

    Keys.onRightPressed: function(event) {
        if (popup.visible) {
            return
        }
        if (arrowKeysMoveFocus) {
            nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocus)
        }
        else {
            event.accepted = false
        }
    }
}
