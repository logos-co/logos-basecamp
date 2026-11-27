.pragma library

// How a download is described: its bytes, where they come from, and the notice
// at the top of a screen that downloads packages.

function bytes(n) {
    if (!isFinite(n) || n <= 0) return "0 B"
    if (n < 1024) return Math.round(n) + " B"
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB"
    if (n < 1024 * 1024 * 1024) return (n / (1024 * 1024)).toFixed(1) + " MB"
    return (n / (1024 * 1024 * 1024)).toFixed(1) + " GB"
}

// "12.3 / 45.2 MB" when the size is known, "12.3 MB" when it isn't. Both sides
// in the total's unit, so the unit prints once.
function progressLabel(received, total) {
    if (!(total > 0)) return bytes(received)
    const k = 1024
    let unit = "B", div = 1
    if (total >= k * k * k)  { unit = "GB"; div = k * k * k }
    else if (total >= k * k) { unit = "MB"; div = k * k }
    else if (total >= k)     { unit = "KB"; div = k }
    const f = function (n) {
        return div === 1 ? String(Math.round(n)) : (n / div).toFixed(1)
    }
    return f(received) + " / " + f(total) + " " + unit
}

// The widest progressLabel, for sizing a badge that shows it.
var widestProgressLabel = "999.9 / 999.9 MB"

// 0..1, and 0 for an unknown total, so a binding never gets NaN.
function fraction(received, total) {
    if (!(total > 0)) return 0
    return Math.max(0, Math.min(1, received / total))
}

function percent(received, total) {
    return Math.round(fraction(received, total) * 100) + "%"
}

// The bytes are moving: some arrived, not all.
function transferring(received, total) {
    return total > 0 && received > 0 && received < total
}

// Where a download's bytes come from, for the line under its name.
// `transport` is "logos", "http" or "" (not known yet: no caption).
function transportCaption(transport, fellBack) {
    if (transport === "logos")
        return qsTr("via Logos Storage · peer-to-peer, may be slower")
    if (transport === "http")
        return fellBack ? qsTr("via GitHub · Logos Storage didn't have it")
                        : qsTr("via GitHub")
    return ""
}

// The design system's icon for the transport ("logosStorage", "github"), or
// "" when not known.
function transportIconName(transport) {
    if (transport === "logos") return "logosStorage"
    if (transport === "http") return "github"
    return ""
}

// Logos Storage with no byte yet is still finding peers that have the package.
function findingPeers(transport, received) {
    return transport === "logos" && !(received > 0)
}

// Why downloads may not work right now, as {severity, title, message}, or null.
// `source` is the download source ("any", "logos", "http"); `storageState`
// package_downloader's ("off", "waiting", "idle", "starting", "running",
// "failed", or "" not known; "idle" says nothing, the next download starts the
// node). `showsFallback`: also say when Logos Storage is down and GitHub covers
// for it, only where each download's source is shown. Offline says nothing
// here: Basecamp says so over every view.
function downloadNotice(online, source, storageState, showsFallback) {
    if (!online)
        return null

    const storageDown = storageState === "failed" || storageState === "off"
    const storageStarting = storageState === "waiting" || storageState === "starting"

    if (source === "logos" && storageDown)
        return { severity: "error", title: qsTr("Logos Storage isn't running"),
                 message: qsTr("Downloads are set to Logos Storage only, so apps can't be "
                               + "downloaded. You can enable GitHub downloads in Settings → "
                               + "Repositories.") }

    if (source === "logos" && storageStarting)
        return { severity: "info", title: qsTr("Starting Logos Storage…"),
                 message: qsTr("Downloads begin once it's running.") }

    if (source === "any" && storageDown && showsFallback)
        return { severity: "info", title: qsTr("Logos Storage isn't running"),
                 message: qsTr("Apps download from GitHub meanwhile. Basecamp tries Logos "
                               + "Storage again with the next download.") }

    return null
}
