import QtQuick
import QtQuick.Dialogs

// Picks an image to use as an icon. Loaded on its own (see OverlayMenuCommandsView.qml), since
// QtQuick.Dialogs' FileDialog is only in Qt 6.
Item {
    signal chosen(url file)

    function open() {
        dialog.open()
    }

    FileDialog {
        id: dialog
        title: qsTr("Choose an image")
        nameFilters: [ qsTr("Images (*.svg *.png *.jpg *.jpeg *.webp *.bmp *.gif *.ico)") ]
        onAccepted: chosen(selectedFile)
    }
}
