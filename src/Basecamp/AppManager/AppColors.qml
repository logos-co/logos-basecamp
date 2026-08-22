pragma Singleton
import QtQuick
import Logos.Theme

QtObject {
    function hash32(key) {
        var h = 0x811c9dc5
        for (var i = 0; i < key.length; i++) {
            h ^= key.charCodeAt(i)
            h = Math.imul(h, 0x01000193) >>> 0
        }
        h = Math.imul(h ^ (h >>> 16), 0x85ebca6b) >>> 0
        h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35) >>> 0
        h = (h ^ (h >>> 16)) >>> 0
        return h
    }

    // Muted accent for the monogram fallback tile (single solid fill).
    //
    // Used only while an app is un-installed AND has no icon — a browsing
    // affordance that gives an otherwise-uniform grid of unknown packages
    // some scannability. Once installed, or once a real icon resolves, the
    // tile switches to the flat theme grey. See LogosTile in the design system.
    function colorForApp(appKey) {
        if (!appKey) return Theme.palette.surfaceRaised
        var h = hash32(appKey)
        var hue = ((h / 4294967296) * 0.618033988749895) % 1
        var sat   = 0.38
        var light = 0.22
        return Qt.hsla(hue, sat, light, 1.0)
    }

}
