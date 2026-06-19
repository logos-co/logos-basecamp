pragma Singleton
import QtQuick

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

    function colorForApp(appKey) {
        if (!appKey) return "#404040"
        var h = hash32(appKey)
        // Golden-ratio hue: maximally spreads similar hashes around the wheel.
        var hue = ((h / 4294967296) * 0.618033988749895) % 1
        var sat   = 0.68
        var light = 0.48
        return Qt.hsla(hue, sat, light, 1.0)
    }
}
