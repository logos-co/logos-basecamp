import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import Logos.Icons
import Logos.Theme

// UpdateCheckState / UpdateDownloadStage.
import Basecamp.Backend 1.0

Item {
    id: root

    LogosScrollView {
        id: scroll
        anchors.fill: parent
        anchors.margins: 40
        clip: true

        ColumnLayout {
            width: scroll.availableWidth
            spacing: 20

            LogosText {
                text: "Dashboard"
                font.pixelSize: 24
                font.weight: Font.Bold
                color: "#ffffff"
            }

            // Build summary: version (release only) + build type.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6

                // "Version" stays a plain label so a drag-select copies the
                // bare version string, not the prefix.
                RowLayout {
                    Layout.fillWidth: true
                    visible: backend.buildVersion.length > 0
                    spacing: 6

                    LogosText {
                        text: "Version"
                        font.pixelSize: 18
                        color: "#ffffff"
                    }

                    LogosSelectableText {
                        Layout.fillWidth: true
                        text: backend.buildVersion
                        font.pixelSize: 18
                        color: "#ffffff"
                        wrapMode: TextEdit.WrapAnywhere
                    }
                }

                LogosText {
                    text: backend.isPortableBuild ? "Portable build" : "Dev build"
                    font.pixelSize: 14
                    color: "#a0a0a0"
                }
            }

            // Host-app update checker. The whole section collapses on builds
            // where the check never runs (dev / pre-release / non-nix), which
            // report Skipped — those users must see nothing at all here, not
            // an "unknown" row.
            ColumnLayout {
                id: updateSection

                objectName: "dashboard.updateSection"

                // Cached so the nested stage rows read as a state machine
                // rather than a wall of repeated backend lookups.
                readonly property int checkState: backend.updateCheckState
                readonly property int stage: backend.updateDownloadStage
                // A stage still parked at Idle on a platform with no matching
                // release asset is presented as Unsupported, not as a download
                // offer that would fail the moment it is clicked.
                readonly property bool offersDownload: stage === UpdateDownloadStage.Idle
                                                       && backend.downloadSupported

                Layout.fillWidth: true
                Layout.topMargin: 10
                spacing: 8
                visible: checkState !== UpdateCheckState.Skipped

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    LogosText {
                        text: "Updates"
                        font.pixelSize: 18
                        font.weight: Font.Bold
                        color: "#ffffff"
                    }
                }

                // ── Checking ──────────────────────────────────────────────
                RowLayout {
                    id: updateCheckingRow

                    objectName: "dashboard.updateChecking"
                    Layout.fillWidth: true
                    spacing: 10
                    visible: updateSection.checkState === UpdateCheckState.Checking

                    LogosSpinner {
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        running: updateCheckingRow.visible
                    }

                    LogosText {
                        Layout.fillWidth: true
                        text: "Checking for updates…"
                        font.pixelSize: 14
                        color: "#a0a0a0"
                    }
                }

                // ── Up to date ────────────────────────────────────────────
                RowLayout {
                    objectName: "dashboard.updateUpToDate"
                    Layout.fillWidth: true
                    spacing: 16
                    visible: updateSection.checkState === UpdateCheckState.UpToDate

                    LogosText {
                        text: "You're on the latest version."
                        font.pixelSize: 14
                        color: "#a0a0a0"
                    }

                    LogosButton {
                        objectName: "dashboard.updateCheckButton"
                        text: "Check for updates"
                        leadingIcon.source: LogosIcons.refresh
                        leadingIcon.brightness: 1.0
                        onClicked: backend.checkForUpdates()
                    }

                    Item { Layout.fillWidth: true }
                }

                // ── Check failed ──────────────────────────────────────────
                // Deliberately distinct from "up to date": a failed check must
                // never read as a clean bill of health.
                ColumnLayout {
                    objectName: "dashboard.updateCheckFailed"
                    Layout.fillWidth: true
                    spacing: 8
                    visible: updateSection.checkState === UpdateCheckState.Failed

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        LogosIcon {
                            source: LogosIcons.warning
                            color: Theme.palette.warning
                            Layout.preferredWidth: 16
                            Layout.preferredHeight: 16
                            Layout.alignment: Qt.AlignVCenter
                        }

                        LogosText {
                            Layout.fillWidth: true
                            text: "Couldn't check for updates."
                            font.pixelSize: 14
                            color: "#a0a0a0"
                            wrapMode: Text.WordWrap
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16

                        LogosButton {
                            objectName: "dashboard.updateRetryCheckButton"
                            text: "Check for updates"
                            leadingIcon.source: LogosIcons.refresh
                            leadingIcon.brightness: 1.0
                            onClicked: backend.checkForUpdates()
                        }

                        LogosLink {
                            objectName: "dashboard.updateCheckFailedLink"
                            text: "View releases"
                            href: backend.releaseUrl
                            onActivated: backend.openReleasePage()
                        }

                        Item { Layout.fillWidth: true }
                    }
                }

                // ── Update available ──────────────────────────────────────
                //
                // The only actionable thing on an otherwise read-only page, so
                // it gets a card while the flat "Commits" list stays flat. That
                // contrast is the point: without it the notice reads as one more
                // paragraph of build metadata and is easy to scroll past — which
                // is the exact failure (#319) this feature exists to fix.
                Rectangle {
                    objectName: "dashboard.updateAvailable"
                    Layout.fillWidth: true
                    Layout.bottomMargin: 8
                    visible: updateSection.checkState === UpdateCheckState.UpdateAvailable
                    // Hug the content rather than reserving room for every state.
                    implicitHeight: updateCard.implicitHeight + 2 * updateCard.anchors.margins
                    color: Theme.palette.surfaceRaised
                    border.color: Theme.palette.borderSubtle
                    border.width: 1
                    radius: 8

                ColumnLayout {
                    id: updateCard

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 16
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        // Leads with the version rather than a bare "Update
                        // available" + badge: the number is the information the
                        // user actually needs, and the old phrasing buried it in
                        // a chip beside the label.
                        LogosText {
                            text: "Basecamp " + backend.latestVersion + " is available"
                            font.pixelSize: 15
                            font.weight: Font.Bold
                            color: "#ffffff"
                        }

                        Item { Layout.fillWidth: true }
                    }

                    LogosText {
                        Layout.fillWidth: true
                        visible: backend.buildVersion.length > 0
                        text: "You're on " + backend.buildVersion
                        font.pixelSize: 13
                        color: "#a0a0a0"
                    }

                    // ── Download: idle ────────────────────────────────────
                    RowLayout {
                        objectName: "dashboard.updateDownloadIdle"
                        Layout.fillWidth: true
                        spacing: 16
                        visible: updateSection.offersDownload

                        LogosButton {
                            objectName: "dashboard.updateDownloadButton"
                            text: "Download update"
                            variant: LogosButton.Variant.Primary
                            leadingIcon.source: LogosIcons.install
                            // LogosIcon tints via MultiEffect colorization, which
                            // leaves a dark source silhouette essentially untouched —
                            // the glyph renders as its raw asset color instead of the
                            // button's foreground. brightness 1.0 normalizes it first.
                            // Same reason as ConfirmationDialog.qml's warning icon.
                            leadingIcon.brightness: 1.0
                            onClicked: backend.startUpdateDownload()
                        }

                        LogosLink {
                            objectName: "dashboard.updateReleaseLink"
                            text: "View release notes"
                            href: backend.releaseUrl
                            onActivated: backend.openReleasePage()
                        }

                        Item { Layout.fillWidth: true }
                    }

                    // ── Download: unsupported on this platform ────────────
                    ColumnLayout {
                        objectName: "dashboard.updateDownloadUnsupported"
                        Layout.fillWidth: true
                        spacing: 6
                        visible: updateSection.stage === UpdateDownloadStage.Unsupported
                                 || (updateSection.stage === UpdateDownloadStage.Idle
                                     && !backend.downloadSupported)

                        LogosLink {
                            objectName: "dashboard.updateUnsupportedLink"
                            text: "View release notes"
                            href: backend.releaseUrl
                            onActivated: backend.openReleasePage()
                        }

                        LogosText {
                            Layout.fillWidth: true
                            text: "No automatic download for this platform."
                            font.pixelSize: 13
                            color: "#a0a0a0"
                            wrapMode: Text.WordWrap
                        }
                    }

                    // ── Download: starting ────────────────────────────────
                    RowLayout {
                        id: updateStartingRow

                        objectName: "dashboard.updateStarting"
                        Layout.fillWidth: true
                        spacing: 16
                        visible: updateSection.stage === UpdateDownloadStage.Starting

                        LogosSpinner {
                            Layout.preferredWidth: 16
                            Layout.preferredHeight: 16
                            running: updateStartingRow.visible
                        }

                        LogosText {
                            text: "Starting download…"
                            font.pixelSize: 13
                            color: "#a0a0a0"
                        }

                        LogosButton {
                            objectName: "dashboard.updateCancelStartButton"
                            text: "Cancel"
                            onClicked: backend.cancelUpdateDownload()
                        }

                        Item { Layout.fillWidth: true }
                    }

                    // ── Download: in progress ─────────────────────────────
                    ColumnLayout {
                        objectName: "dashboard.updateDownloading"
                        Layout.fillWidth: true
                        spacing: 8
                        visible: updateSection.stage === UpdateDownloadStage.Downloading

                        LogosProgressBar {
                            objectName: "dashboard.updateProgressBar"
                            Layout.fillWidth: true
                            from: 0
                            to: 1
                            value: backend.downloadProgress
                            // -1 means the server sent no Content-Length.
                            indeterminate: backend.downloadProgress < 0
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16

                            LogosText {
                                objectName: "dashboard.updateProgressText"
                                Layout.fillWidth: true
                                text: backend.downloadProgressText
                                font.pixelSize: 13
                                color: "#a0a0a0"
                            }

                            LogosButton {
                                objectName: "dashboard.updateCancelButton"
                                text: "Cancel"
                                onClicked: backend.cancelUpdateDownload()
                            }
                        }
                    }

                    // ── Download: verifying ───────────────────────────────
                    RowLayout {
                        id: updateVerifyingRow

                        objectName: "dashboard.updateVerifying"
                        Layout.fillWidth: true
                        spacing: 10
                        visible: updateSection.stage === UpdateDownloadStage.Verifying

                        LogosSpinner {
                            Layout.preferredWidth: 16
                            Layout.preferredHeight: 16
                            running: updateVerifyingRow.visible
                        }

                        LogosText {
                            Layout.fillWidth: true
                            text: "Checking download…"
                            font.pixelSize: 13
                            color: "#a0a0a0"
                        }
                    }

                    // ── Download: done ────────────────────────────────────
                    ColumnLayout {
                        objectName: "dashboard.updateDownloadDone"
                        Layout.fillWidth: true
                        spacing: 8
                        visible: updateSection.stage === UpdateDownloadStage.Done

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            LogosIcon {
                                source: LogosIcons.check
                                color: Theme.palette.success
                                Layout.preferredWidth: 16
                                Layout.preferredHeight: 16
                                Layout.alignment: Qt.AlignTop
                            }

                            // "Saved to" stays a plain label so a drag-select
                            // copies the bare path, not the prefix.
                            LogosText {
                                text: "Saved to"
                                font.pixelSize: 13
                                color: "#a0a0a0"
                                Layout.alignment: Qt.AlignTop
                            }

                            LogosSelectableText {
                                objectName: "dashboard.updateDownloadPath"
                                Layout.fillWidth: true
                                text: backend.downloadedFilePath
                                font.pixelSize: 13
                                color: "#d0d0d0"
                                wrapMode: TextEdit.WrapAnywhere
                            }
                        }

                        LogosText {
                            objectName: "dashboard.updateInstallHint"
                            Layout.fillWidth: true
                            visible: backend.updateInstallHint.length > 0
                            text: backend.updateInstallHint
                            font.pixelSize: 13
                            color: "#a0a0a0"
                            wrapMode: Text.WordWrap
                        }

                        LogosButton {
                            objectName: "dashboard.updateRevealButton"
                            text: Qt.platform.os === "osx" ? "Show in Finder"
                                                           : "Show in Folder"
                            onClicked: backend.revealUpdateDownload()
                        }
                    }

                    // ── Download: failed ──────────────────────────────────
                    ColumnLayout {
                        objectName: "dashboard.updateDownloadFailed"
                        Layout.fillWidth: true
                        spacing: 8
                        visible: updateSection.stage === UpdateDownloadStage.Failed

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            LogosIcon {
                                source: LogosIcons.warning
                                color: Theme.palette.error
                                Layout.preferredWidth: 16
                                Layout.preferredHeight: 16
                                Layout.alignment: Qt.AlignTop
                            }

                            LogosText {
                                objectName: "dashboard.updateDownloadError"
                                Layout.fillWidth: true
                                text: backend.downloadError.length > 0
                                    ? backend.downloadError
                                    : "Download failed."
                                font.pixelSize: 13
                                color: "#a0a0a0"
                                wrapMode: Text.WordWrap
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16

                            LogosButton {
                                objectName: "dashboard.updateRetryDownloadButton"
                                text: "Retry"
                                leadingIcon.source: LogosIcons.refresh
                                leadingIcon.brightness: 1.0
                                onClicked: backend.startUpdateDownload()
                            }

                            LogosLink {
                                objectName: "dashboard.updateDownloadFailedLink"
                                text: "View release notes"
                                href: backend.releaseUrl
                                onActivated: backend.openReleasePage()
                            }

                            Item { Layout.fillWidth: true }
                        }
                    }
                }
                }  // update-available card
            }

            // Commit hashes for basecamp + all flake dependencies.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: 10
                spacing: 4

                LogosText {
                    text: "Commits"
                    font.pixelSize: 16
                    font.weight: Font.Bold
                    color: "#ffffff"
                }

                Repeater {
                    model: backend.buildCommits
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        // Module name and commit hash are both copy targets, so
                        // they wrap instead of eliding — an elided hash cannot
                        // be selected in full.
                        LogosSelectableText {
                            text: modelData.name
                            color: "#a0a0a0"
                            font.pixelSize: 13
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignTop
                            wrapMode: TextEdit.WrapAnywhere
                        }
                        LogosSelectableText {
                            text: modelData.commit
                            color: "#d0d0d0"
                            font.family: "monospace"
                            font.pixelSize: 13
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            wrapMode: TextEdit.WrapAnywhere
                        }
                    }
                }
            }

            Item { Layout.fillHeight: true }
        }
    }
}
