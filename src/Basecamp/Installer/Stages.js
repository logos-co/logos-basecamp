.pragma library
.import "Types.js" as T
.import "Downloads.js" as Downloads

// The words for what an install is doing and what a row's action does.

// %1, %2 … filled in one pass, so a value containing "%2" stays as it is.
function fill(text) {
    const args = arguments
    return text.replace(/%(\d)/g, function (m, n) {
        const i = Number(n)
        return i < args.length ? String(args[i]) : m
    })
}

// An InstallStage in words; "" for None.
function stageWord(stage) {
    switch (stage) {
    case T.InstallStage.Downloading: return qsTr("Downloading…")
    case T.InstallStage.Downloaded:  return qsTr("Downloaded")
    case T.InstallStage.Queued:      return qsTr("Queued")
    case T.InstallStage.Installing:  return qsTr("Installing…")
    case T.InstallStage.Installed:   return qsTr("Installed")
    case T.InstallStage.Failed:      return qsTr("Failed")
    }
    return ""
}

// Logos Storage with no byte yet: still finding peers that have the package.
function findingPeersWord() {
    return qsTr("Finding peers…")
}

// What an install in flight is doing, from what is known of its download:
// {queued, transport ("logos", "http" or "" before its download starts),
// received, total}. The bytes while they move; then the stage.
function progressWord(p) {
    const received = p.received || 0
    const total = p.total || 0
    if (Downloads.transferring(received, total))
        return Downloads.progressLabel(received, total)
    const done = total > 0 && received >= total
    if (p.queued || (!done && received <= 0 && !p.transport)) return qsTr("Queued")
    if (!done && Downloads.findingPeers(p.transport, received)) return findingPeersWord()
    if (!done && received <= 0 && p.transport === "http") return qsTr("Downloading…")
    return qsTr("Installing…")
}

// A RowAction in words: the verb, or "Installed" for NoOp.
function actionWord(action) {
    switch (action) {
    case T.RowAction.Install:      return qsTr("Install")
    case T.RowAction.Upgrade:      return qsTr("Upgrade")
    case T.RowAction.Downgrade:    return qsTr("Downgrade")
    case T.RowAction.Reinstall:    return qsTr("Reinstall")
    case T.RowAction.Retry:        return qsTr("Retry")
    case T.RowAction.NotAvailable: return qsTr("Not available")
    }
    return qsTr("Installed")
}

// What the action changes, in versions: on disk now → selected.
function transitionText(installedVersion, selectedVersion) {
    const from = installedVersion ? String(installedVersion) : ""
    const to = selectedVersion ? String(selectedVersion) : ""
    if (from === "")
        return to === "" ? "" : fill(qsTr("Installs v%1"), to)
    if (to === "" || to === from)
        return fill(qsTr("v%1 installed"), from)
    return fill(qsTr("v%1 → v%2"), from, to)
}
