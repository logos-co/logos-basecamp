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

    // Muted accent for dark-theme app tiles (single solid fill).
    function colorForApp(appKey) {
        if (!appKey) return Theme.palette.surfaceRaised
        var h = hash32(appKey)
        var hue = ((h / 4294967296) * 0.618033988749895) % 1
        var sat   = 0.38
        var light = 0.22
        return Qt.hsla(hue, sat, light, 1.0)
    }

    function accentForDarkTheme(color) {
        var c = Qt.color(color)
        var hue = c.hsvHue
        if (hue < 0)
            hue = (hash32(color) / 4294967296) % 1
        var sat = Math.max(0.30, Math.min(c.hsvSaturation > 0 ? c.hsvSaturation : 0.38, 0.50))
        return Qt.hsva(hue, sat, 0.28, 1.0)
    }

    function tileColor(packageColor, appKey) {
        if (packageColor && packageColor.length > 0)
            return accentForDarkTheme(packageColor)
        return colorForApp(appKey)
    }
}
