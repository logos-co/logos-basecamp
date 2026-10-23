pragma Singleton
import QtQuick
import Logos.Icons

// Shared byte formatting for the install surfaces that show live download
// progress: the Applications list and grid delegates, and the dependency
// dialog's package rows. One copy so all three describe the same transfer
// the same way.
QtObject {
    function bytes(n) {
        if (!isFinite(n) || n <= 0) return "0 B"
        if (n < 1024) return Math.round(n) + " B"
        if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB"
        if (n < 1024 * 1024 * 1024) return (n / (1024 * 1024)).toFixed(1) + " MB"
        return (n / (1024 * 1024 * 1024)).toFixed(1) + " GB"
    }

    // "12.3 / 45.2 MB" when the size is known, "12.3 MB" when it isn't.
    // Both sides scaled to the TOTAL's unit, so the unit prints once and the
    // numbers are directly comparable: "12.3 / 45.2 MB", not
    // "12.3 MB / 45.2 MB". Also the narrower worst case, which is what the
    // badge's fixed install width is sized against.
    function label(received, total) {
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

    // Clamped 0..1 fill fraction. Returns 0 for an unknown total rather
    // than NaN, so a binding on it never produces an invalid width.
    function fraction(received, total) {
        if (!(total > 0)) return 0
        return Math.max(0, Math.min(1, received / total))
    }

    // "45%" — for the grid tile, whose badge is too narrow for two byte
    // counts side by side.
    function percent(received, total) {
        return Math.round(fraction(received, total) * 100) + "%"
    }

    // Where a download's bytes come from, for the line under its name.
    // `transport` is "logos", "http" or "" (unknown: no caption).
    function transportCaption(transport, fellBack) {
        if (transport === "logos")
            return qsTr("via Logos Storage · peer-to-peer, may be slower")
        if (transport === "http")
            return fellBack ? qsTr("via GitHub · Logos Storage didn't have it")
                            : qsTr("via GitHub")
        return ""
    }

    // The icon at the start of a download's progress badge: where its bytes
    // come from. Empty when not known.
    function transportIcon(transport) {
        if (transport === "logos") return LogosIcons.logosStorage
        if (transport === "http") return LogosIcons.github
        return ""
    }

    // What a badge's icon adds to its width: 12px image and 4px spacing.
    readonly property int badgeIconWidth: 16

    // Logos Storage with no byte yet is still finding peers that have the
    // package: where a slow download spends its time. The progress badge says
    // so instead of "0 B".
    function findingPeers(transport, received) {
        return transport === "logos" && !(received > 0)
    }
}
