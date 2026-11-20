import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2
import QtQuick.Controls.Material 2.2

import ComputerManager 1.0
import AutoUpdateChecker 1.0
import UpdateInstaller 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import SdlGamepadKeyNavigation 1.0

ApplicationWindow {
    property bool pollingActive: false

    // Set by SettingsView to force the back operation to pop all
    // pages except the initial view. This is required when doing
    // a retranslate() because AppView breaks for some reason.
    property bool clearOnBack: false

    id: window
    width: 1280
    height: 600

    // This function runs prior to creation of the initial StackView item
    function doEarlyInit() {
        // Override the background color to Material 2 colors for Qt 6.5+
        // in order to improve contrast between GFE's placeholder box art
        // and the background of the app grid.
        if (SystemProperties.usesMaterial3Theme) {
            Material.background = "#15163A" // NyxPlay night surface
        }

        SdlGamepadKeyNavigation.enable()
    }

    Component.onCompleted: {
        // Show the window according to the user's preferences
        if (SystemProperties.hasDesktopEnvironment) {
            if (StreamingPreferences.uiDisplayMode == StreamingPreferences.UI_MAXIMIZED) {
                window.showMaximized()
            }
            else if (StreamingPreferences.uiDisplayMode == StreamingPreferences.UI_FULLSCREEN) {
                window.showFullScreen()
            }
            else {
                window.show()
            }
        } else {
            window.showFullScreen()
        }

        // Display any modal dialogs for configuration warnings
        if (isGuiLaunch) {
            if (SystemProperties.isWow64) {
                wow64Dialog.open()
            }

            // Hardware acceleration and unmapped gamepads are checked asynchronously
            SystemProperties.hasHardwareAccelerationChanged.connect(hasHardwareAccelerationChanged)
            SystemProperties.unmappedGamepadsChanged.connect(hasUnmappedGamepadsChanged)
            SystemProperties.startAsyncLoad()
        }
    }

    function hasHardwareAccelerationChanged() {
        if (!SystemProperties.hasHardwareAcceleration && StreamingPreferences.videoDecoderSelection !== StreamingPreferences.VDS_FORCE_SOFTWARE) {
            if (SystemProperties.isRunningXWayland) {
                xWaylandDialog.open()
            }
            else {
                noHwDecoderDialog.open()
            }
        }
    }

    function hasUnmappedGamepadsChanged() {
        if (SystemProperties.unmappedGamepads) {
            unmappedGamepadDialog.unmappedGamepads = SystemProperties.unmappedGamepads
            unmappedGamepadDialog.open()
        }
    }

    // It would be better to use TextMetrics here, but it always lays out
    // the text slightly more compactly than real Text does in ToolTip,
    // causing unexpected line breaks to be inserted
    Text {
        id: tooltipTextLayoutHelper
        visible: false
        font: ToolTip.toolTip.font
        text: ToolTip.toolTip.text
    }

    // This configures the maximum width of the singleton attached QML ToolTip. If left unconstrained,
    // it will never insert a line break and just extend on forever.
    ToolTip.toolTip.contentWidth: Math.min(tooltipTextLayoutHelper.width, 400)

    function goBack() {
        if (clearOnBack) {
            // Pop all items except the first one
            stackView.pop(null)
            clearOnBack = false
        }
        else {
            stackView.pop()
        }
    }

    StackView {
        id: stackView
        anchors.fill: parent
        focus: true

        Component.onCompleted: {
            // Perform our early initialization before constructing
            // the initial view and pushing it to the StackView
            doEarlyInit()
            push(initialView)
        }

        onCurrentItemChanged: {
            // Ensure focus travels to the next view when going back
            if (currentItem) {
                currentItem.forceActiveFocus()
            }
        }

        Keys.onEscapePressed: {
            if (depth > 1) {
                goBack()
            }
            else {
                quitConfirmationDialog.open()
            }
        }

        Keys.onBackPressed: {
            if (depth > 1) {
                goBack()
            }
            else {
                quitConfirmationDialog.open()
            }
        }

        Keys.onMenuPressed: {
            settingsButton.clicked()
        }

        // This is a keypress we've reserved for letting the
        // SdlGamepadKeyNavigation object tell us to show settings
        // when Menu is consumed by a focused control.
        Keys.onHangupPressed: {
            settingsButton.clicked()
        }
    }

    // This timer keeps us polling for 5 minutes of inactivity
    // to allow the user to work with Moonlight on a second display
    // while dealing with configuration issues. This will ensure
    // machines come online even if the input focus isn't on Moonlight.
    Timer {
        id: inactivityTimer
        interval: 5 * 60000
        onTriggered: {
            if (!active && pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
    }

    onVisibleChanged: {
        // Polling is only managed from the window for a normal GUI launch;
        // CLI commands (stream/quit/pair) manage it via the ComputerSeeker.
        if (isGuiLaunch) {
            // When we become invisible while streaming is going on,
            // stop polling immediately.
            if (!visible) {
                inactivityTimer.stop()

                if (pollingActive) {
                    ComputerManager.stopPollingAsync()
                    pollingActive = false
                }
            }
            else if (active) {
                // When we become visible and active again, start polling
                inactivityTimer.stop()

                // Restart polling if it was stopped
                if (!pollingActive) {
                    ComputerManager.startPolling()
                    pollingActive = true
                }
            }
        }

        // Poll for gamepad input only when the window is in focus
        SdlGamepadKeyNavigation.notifyWindowFocus(visible && active)
    }

    onActiveChanged: {
        // Polling is only managed from the window for a normal GUI launch;
        // CLI commands (stream/quit/pair) manage it via the ComputerSeeker.
        if (isGuiLaunch) {
            if (active) {
                // Stop the inactivity timer
                inactivityTimer.stop()

                // Restart polling if it was stopped
                if (!pollingActive) {
                    ComputerManager.startPolling()
                    pollingActive = true
                }
            }
            else {
                // Start the inactivity timer to stop polling
                // if focus does not return within a few minutes.
                inactivityTimer.restart()
            }
        }

        // Poll for gamepad input only when the window is in focus
        SdlGamepadKeyNavigation.notifyWindowFocus(visible && active)
    }

    function navigateTo(url, objectType)
    {
        var existingItem = stackView.find(function(item, index) {
            return item instanceof objectType
        })

        if (existingItem !== null) {
            // Pop to the existing item
            stackView.pop(existingItem)
        }
        else {
            // Create a new item
            stackView.push(url)
        }
    }

    header: ToolBar {
        id: toolBar
        height: 60
        anchors.topMargin: 5
        anchors.bottomMargin: 5

        Label {
            id: titleLabel
            visible: toolBar.width > 700
            anchors.fill: parent
            text: stackView.currentItem.objectName
            font.pointSize: 20
            elide: Label.ElideRight
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
        }

        RowLayout {
            spacing: 10
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            anchors.fill: parent

            NavigableToolButton {
                // Only make the button visible if the user has navigated somewhere.
                visible: stackView.depth > 1

                iconSource: "qrc:/res/arrow_left.svg"

                onClicked: goBack()

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            // This label will appear when the window gets too small and
            // we need to ensure the toolbar controls don't collide
            Label {
                id: titleRowLabel
                font.pointSize: titleLabel.font.pointSize
                elide: Label.ElideRight
                horizontalAlignment: Qt.AlignHCenter
                verticalAlignment: Qt.AlignVCenter
                Layout.fillWidth: true

                // We need this label to always be visible so it can occupy
                // the remaining space in the RowLayout. To "hide" it, we
                // just set the text to empty string.
                text: !titleLabel.visible ? stackView.currentItem.objectName : ""
            }

            Label {
                id: versionLabel
                visible: stackView.currentItem instanceof SettingsView
                text: qsTr("Version %1").arg(SystemProperties.versionString)
                font.pointSize: 12
                horizontalAlignment: Qt.AlignRight
                verticalAlignment: Qt.AlignVCenter
            }

            NavigableToolButton {
                id: addPcButton
                visible: stackView.currentItem instanceof PcView

                iconSource:  "qrc:/res/ic_add_to_queue_white_48px.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Add PC manually") + (newPcShortcut.nativeText ? (" ("+newPcShortcut.nativeText+")") : "")

                Shortcut {
                    id: newPcShortcut
                    sequence: StandardKey.New
                    onActivated: addPcButton.clicked()
                }

                onClicked: {
                    addPcDialog.open()
                }

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                property string browserUrl: ""

                id: updateButton

                iconSource: "qrc:/res/update.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered || visible

                // Invisible until we get a callback notifying us that
                // an update is available
                visible: false

                onClicked: {
                    if (UpdateInstaller.canInstall) {
                        updateMenu.open()
                    }
                    else if (SystemProperties.hasBrowser) {
                        Qt.openUrlExternally(browserUrl);
                    }
                }

                property string newVersion: ""

                function updateAvailable(version, url)
                {
                    ToolTip.text = qsTr("Update available for NyxPlay: Version %1").arg(version)
                    updateButton.newVersion = version
                    updateButton.browserUrl = url
                    updateButton.visible = true

                    // Announce each new version once, unless it was dismissed before
                    if (StreamingPreferences.dismissedUpdateVersion !== version) {
                        updateNotice.open()
                    }
                }

                Component.onCompleted: {
                    AutoUpdateChecker.onUpdateAvailable.connect(updateAvailable)
                    AutoUpdateChecker.start()
                }

                Keys.onDownPressed: {
                    if (updateNotice.opened) {
                        updateNoticeDownload.forceActiveFocus(Qt.TabFocus)
                    }
                    else {
                        stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                    }
                }
            }

            NavigableToolButton {
                id: helpButton
                visible: SystemProperties.hasBrowser

                iconSource: "qrc:/res/question_mark.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Help") + (helpShortcut.nativeText ? (" ("+helpShortcut.nativeText+")") : "")

                Shortcut {
                    id: helpShortcut
                    sequence: StandardKey.HelpContents
                    onActivated: helpButton.clicked()
                }

                // TODO need to make sure browser is brought to foreground.
                onClicked: Qt.openUrlExternally("https://github.com/siberanka/NyxPlay");

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                // TODO: Implement gamepad mapping then unhide this button
                visible: false

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Gamepad Mapper")

                iconSource: "qrc:/res/ic_videogame_asset_white_48px.svg"

                onClicked: navigateTo("qrc:/gui/GamepadMapper.qml", GamepadMapper)

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                id: settingsButton

                iconSource:  "qrc:/res/settings.svg"

                onClicked: navigateTo("qrc:/gui/SettingsView.qml", SettingsView)

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }

                Shortcut {
                    id: settingsShortcut
                    sequence: StandardKey.Preferences
                    onActivated: settingsButton.clicked()
                }

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Settings") + (settingsShortcut.nativeText ? (" ("+settingsShortcut.nativeText+")") : "")
            }
        }
    }

    ErrorMessageDialog {
        id: noHwDecoderDialog
        text: qsTr("No functioning hardware accelerated video decoder was detected by NyxPlay. " +
                   "Your streaming performance may be severely degraded in this configuration.")
        helpText: qsTr("Click the Help button for more information on solving this problem.")
        helpUrl: "https://github.com/siberanka/NyxPlay/issues"
    }

    ErrorMessageDialog {
        id: xWaylandDialog
        text: qsTr("Hardware acceleration doesn't work on XWayland. Continuing on XWayland may result in poor streaming performance. " +
                   "Try running with QT_QPA_PLATFORM=wayland or switch to X11.")
        helpText: qsTr("Click the Help button for more information.")
        helpUrl: "https://github.com/siberanka/NyxPlay/issues"
    }

    NavigableMessageDialog {
        id: wow64Dialog
        standardButtons: Dialog.Ok | Dialog.Cancel
        text: qsTr("This version of NyxPlay isn't optimized for your PC. Please download the '%1' version of NyxPlay for the best streaming performance.").arg(SystemProperties.friendlyNativeArchName)
        onAccepted: {
            Qt.openUrlExternally("https://github.com/siberanka/NyxPlay/releases");
        }
    }

    // Non-modal, dismissible notice shown once per new version. It never takes the focus by itself;
    // it is reachable with the Down key (or gamepad) from the update button in the toolbar.
    // It also shows the progress of an in-place update and asks before restarting.
    Popup {
        id: updateNotice
        x: window.width - width - 10
        y: 10
        width: Math.min(380, window.width - 20)
        modal: false
        focus: false
        closePolicy: Popup.NoAutoClose
        padding: 12

        property int installState: UpdateInstaller.state
        property bool busy: installState === UpdateInstaller.Downloading ||
                            installState === UpdateInstaller.Verifying ||
                            installState === UpdateInstaller.Extracting ||
                            installState === UpdateInstaller.Restarting
        property bool failed: installState === UpdateInstaller.Failed
        property bool folderProblem: failed && (UpdateInstaller.error === UpdateInstaller.NotWritable ||
                                                UpdateInstaller.error === UpdateInstaller.NotInstallable)

        function dismiss() {
            if (installState === UpdateInstaller.Available) {
                StreamingPreferences.dismissUpdateNotice(updateButton.newVersion)
            }
            else if (failed) {
                UpdateInstaller.dismissError()
            }
            close()
            updateButton.forceActiveFocus(Qt.TabFocus)
        }

        function errorText(code) {
            switch (code) {
            case UpdateInstaller.NotWritable:
                return qsTr("NyxPlay cannot update itself because its folder is not writable. Download the new version from the release page instead.")
            case UpdateInstaller.NoConnection:
                return qsTr("No connection. Check your network and try again.")
            case UpdateInstaller.DownloadFailed:
                return qsTr("The download failed.")
            case UpdateInstaller.VerifyFailed:
                return qsTr("The download could not be verified and was deleted.")
            case UpdateInstaller.NoSpace:
                return qsTr("Not enough disk space.")
            case UpdateInstaller.PackageInvalid:
                return qsTr("The update package is incomplete and was deleted.")
            case UpdateInstaller.LaunchFailed:
                return qsTr("The updater could not be started.")
            case UpdateInstaller.Busy:
                return qsTr("Another NyxPlay window is already installing an update.")
            default:
                return qsTr("This update cannot be installed from inside NyxPlay. Download it from the release page.")
            }
        }

        function bodyText() {
            switch (installState) {
            case UpdateInstaller.WaitingForStream:
                return qsTr("The update continues after the stream ends.")
            case UpdateInstaller.Downloading:
                return qsTr("Downloading… (%1 %)").arg(UpdateInstaller.progress)
            case UpdateInstaller.Verifying:
                return qsTr("Verifying…")
            case UpdateInstaller.Extracting:
                return qsTr("Preparing the update…")
            case UpdateInstaller.ReadyToRestart:
                return qsTr("Restart now to finish the update?")
            case UpdateInstaller.Restarting:
                return qsTr("Restarting…")
            case UpdateInstaller.Failed:
                return errorText(UpdateInstaller.error)
            default:
                return UpdateInstaller.canInstall
                        ? qsTr("A newer version of NyxPlay is available. Your settings and pairings are kept.")
                        : qsTr("A newer version of NyxPlay is available. You can download it from the releases page.")
            }
        }

        contentItem: ColumnLayout {
            spacing: 10

            Label {
                text: updateNotice.installState === UpdateInstaller.ReadyToRestart
                      ? qsTr("Update ready: NyxPlay %1").arg(updateButton.newVersion)
                      : qsTr("Update available for NyxPlay: Version %1").arg(updateButton.newVersion)
                font.bold: true
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            Label {
                text: updateNotice.bodyText()
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            ProgressBar {
                visible: updateNotice.installState === UpdateInstaller.Downloading
                value: UpdateInstaller.progress / 100
                indeterminate: UpdateInstaller.progress === 0
                Layout.fillWidth: true
            }

            RowLayout {
                spacing: 10
                Layout.alignment: Qt.AlignRight

                Button {
                    id: updateNoticeDismiss
                    text: updateNotice.installState === UpdateInstaller.ReadyToRestart ? qsTr("Later") : qsTr("Dismiss")
                    flat: true
                    activeFocusOnTab: true
                    visible: !updateNotice.busy && updateNotice.installState !== UpdateInstaller.WaitingForStream

                    onClicked: updateNotice.dismiss()
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                    Keys.onEscapePressed: clicked()
                    Keys.onRightPressed: updateNoticeDownload.forceActiveFocus(Qt.TabFocus)
                    Keys.onUpPressed: updateButton.forceActiveFocus(Qt.TabFocus)
                }

                Button {
                    id: updateNoticeCancel
                    text: qsTr("Cancel")
                    flat: true
                    activeFocusOnTab: true
                    visible: updateNotice.installState === UpdateInstaller.Downloading ||
                             updateNotice.installState === UpdateInstaller.WaitingForStream

                    onClicked: UpdateInstaller.cancel()
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                    Keys.onEscapePressed: clicked()
                    Keys.onUpPressed: updateButton.forceActiveFocus(Qt.TabFocus)
                }

                Button {
                    id: updateNoticeDownload
                    text: qsTr("Download")
                    activeFocusOnTab: true
                    visible: SystemProperties.hasBrowser && !updateNotice.busy &&
                             updateNotice.installState !== UpdateInstaller.WaitingForStream &&
                             updateNotice.installState !== UpdateInstaller.ReadyToRestart &&
                             (!UpdateInstaller.canInstall || updateNotice.folderProblem)

                    onClicked: {
                        Qt.openUrlExternally(updateButton.browserUrl)
                        updateNotice.dismiss()
                    }
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                    Keys.onEscapePressed: updateNoticeDismiss.clicked()
                    Keys.onLeftPressed: updateNoticeDismiss.forceActiveFocus(Qt.TabFocus)
                    Keys.onUpPressed: updateButton.forceActiveFocus(Qt.TabFocus)
                }

                Button {
                    id: updateNoticeInstall
                    text: updateNotice.failed ? qsTr("Try again") : qsTr("Install update")
                    activeFocusOnTab: true
                    visible: UpdateInstaller.canInstall && !updateNotice.folderProblem &&
                             (updateNotice.installState === UpdateInstaller.Available || updateNotice.failed)

                    onClicked: UpdateInstaller.install()
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                    Keys.onEscapePressed: updateNoticeDismiss.clicked()
                    Keys.onLeftPressed: updateNoticeDismiss.forceActiveFocus(Qt.TabFocus)
                    Keys.onUpPressed: updateButton.forceActiveFocus(Qt.TabFocus)
                }

                Button {
                    id: updateNoticeRestart
                    text: qsTr("Restart now")
                    activeFocusOnTab: true
                    visible: updateNotice.installState === UpdateInstaller.ReadyToRestart

                    onClicked: UpdateInstaller.restartNow()
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                    Keys.onEscapePressed: updateNoticeDismiss.clicked()
                    Keys.onLeftPressed: updateNoticeDismiss.forceActiveFocus(Qt.TabFocus)
                    Keys.onUpPressed: updateButton.forceActiveFocus(Qt.TabFocus)
                }
            }
        }
    }

    NavigableMessageDialog {
        id: updateInfoDialog
        standardButtons: Dialog.Ok
    }

    NavigableMenu {
        id: updateMenu
        initiator: updateButton
        y: updateButton.height

        NavigableMenuItem {
            text: qsTr("Install update %1").arg(updateButton.newVersion)
            visible: UpdateInstaller.canInstall &&
                     (UpdateInstaller.state === UpdateInstaller.Available || UpdateInstaller.state === UpdateInstaller.Failed)
            height: visible ? implicitHeight : 0
            onTriggered: {
                updateNotice.open()
                UpdateInstaller.install()
            }
        }
        NavigableMenuItem {
            text: qsTr("Restart now")
            visible: UpdateInstaller.state === UpdateInstaller.ReadyToRestart
            height: visible ? implicitHeight : 0
            onTriggered: updateNotice.open()
        }
        NavigableMenuItem {
            text: qsTr("Show details")
            visible: updateNotice.busy || UpdateInstaller.state === UpdateInstaller.WaitingForStream
            height: visible ? implicitHeight : 0
            onTriggered: updateNotice.open()
        }
        NavigableMenuItem {
            text: qsTr("Check for updates")
            onTriggered: UpdateInstaller.checkNow(false)
        }
        NavigableMenuItem {
            text: qsTr("Download")
            visible: SystemProperties.hasBrowser && updateButton.browserUrl !== ""
            height: visible ? implicitHeight : 0
            onTriggered: Qt.openUrlExternally(updateButton.browserUrl)
        }
    }

    Connections {
        target: UpdateInstaller

        function onStateChanged() {
            // Progress, errors and the restart question are always shown to the user
            if (UpdateInstaller.state === UpdateInstaller.ReadyToRestart ||
                    UpdateInstaller.state === UpdateInstaller.Failed ||
                    UpdateInstaller.state === UpdateInstaller.Downloading ||
                    UpdateInstaller.state === UpdateInstaller.WaitingForStream) {
                updateNotice.open()
            }
        }

        function onCheckReported(available, failed) {
            if (available) {
                updateNotice.open()
            }
            else {
                updateInfoDialog.text = failed ? qsTr("The update check failed. Check your network and try again.")
                                               : qsTr("NyxPlay is up to date.")
                updateInfoDialog.open()
            }
        }

        function onLastResultChanged() {
            var parts = UpdateInstaller.lastResult.split("|")
            if (parts[0] === "ok") {
                updateInfoDialog.text = qsTr("NyxPlay was updated to version %1.").arg(parts[1])
            }
            else if (parts[0] === "failed") {
                var reasons = {
                    "locked": qsTr("A file was in use."),
                    "write-failed": qsTr("A file could not be written."),
                    "stage-invalid": qsTr("The update package was incomplete."),
                    "wait-timeout": qsTr("NyxPlay did not close in time."),
                    "rollback-incomplete": qsTr("Some files could not be restored. Download NyxPlay again from the release page.")
                }
                updateInfoDialog.text = qsTr("The update could not be installed and the previous version was restored.") + "\n" +
                        (reasons[parts[1]] !== undefined ? reasons[parts[1]] : qsTr("The update failed."))
            }
            else {
                return
            }
            updateInfoDialog.open()
            UpdateInstaller.clearLastResult()
        }
    }

    ErrorMessageDialog {
        id: unmappedGamepadDialog
        property string unmappedGamepads : ""
        text: qsTr("NyxPlay detected gamepads without a mapping:") + "\n" + unmappedGamepads
        helpTextSeparator: "\n\n"
        helpText: qsTr("Click the Help button for information on how to map your gamepads.")
        helpUrl: "https://github.com/siberanka/NyxPlay/issues"
    }

    // This dialog appears when quitting via keyboard or gamepad button
    NavigableMessageDialog {
        id: quitConfirmationDialog
        standardButtons: Dialog.Yes | Dialog.No
        text: qsTr("Are you sure you want to quit?")
        // For keyboard/gamepad navigation
        onAccepted: Qt.quit()
    }

    // HACK: This belongs in StreamSegue but keeping a dialog around after the parent
    // dies can trigger bugs in Qt 5.12 that cause the app to crash. For now, we will
    // host this dialog in a QML component that is never destroyed.
    //
    // To repro: Start a stream, cut the network connection to trigger the "Connection
    // terminated" dialog, wait until the app grid times out back to the PC grid, then
    // try to dismiss the dialog.
    ErrorMessageDialog {
        id: streamSegueErrorDialog

        property bool quitAfter: false

        onClosed: {
            if (quitAfter) {
                Qt.quit()
            }

            // StreamSegue assumes its dialog will be re-created each time we
            // start streaming, so fake it by wiping out the text each time.
            text = ""
        }
    }

    NavigableDialog {
        id: addPcDialog
        property string label: qsTr("Enter the IP address of your host PC:")

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            editText.forceActiveFocus()
        }

        onClosed: {
            editText.clear()
        }

        onAccepted: {
            if (editText.text) {
                ComputerManager.addNewHostManually(editText.text.trim())
            }
        }

        ColumnLayout {
            Label {
                text: addPcDialog.label
                font.bold: true
            }

            TextField {
                id: editText
                Layout.fillWidth: true
                focus: true

                Keys.onReturnPressed: {
                    addPcDialog.accept()
                }

                Keys.onEnterPressed: {
                    addPcDialog.accept()
                }
            }
        }
    }
}
