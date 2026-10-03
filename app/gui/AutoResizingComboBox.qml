import QtQuick 2.9
import QtQuick.Controls 2.2

import SdlGamepadKeyNavigation 1.0
import SystemProperties 1.0

// https://stackoverflow.com/questions/45029968/how-do-i-set-the-combobox-width-to-fit-the-largest-item
ComboBox {
    property int textWidth
    property int desiredWidth : leftPadding + textWidth + indicator.width + rightPadding
    property int maximumWidth : parent.width

    // Left and right change the selection by default. Turn this off where they
    // need to move focus between neighboring controls instead, like the toolbar.
    property bool arrowKeysChangeSelection: true

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

    Keys.onLeftPressed: {
        if (arrowKeysChangeSelection) {
            decrementCurrentIndex()
        }
        else if (!popup.visible) {
            // Leave the keys alone while the popup is open, so focus can't leave it
            nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocus)
        }
    }

    Keys.onRightPressed: {
        if (arrowKeysChangeSelection) {
            incrementCurrentIndex()
        }
        else if (!popup.visible) {
            nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocus)
        }
    }
}
