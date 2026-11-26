.pragma library

// An app's fallback tile colour, so the same app has the same colour wherever
// it is shown: a muted hue from its name. null for no name; the caller then
// uses its theme's surface colour.

function hash32(key) {
    var h = 0x811c9dc5
    for (var i = 0; i < key.length; i++) {
        h ^= key.charCodeAt(i)
        h = Math.imul(h, 0x01000193) >>> 0
    }
    h = Math.imul(h ^ (h >>> 16), 0x85ebca6b) >>> 0
    h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35) >>> 0
    return (h ^ (h >>> 16)) >>> 0
}

function colorForApp(appKey) {
    if (!appKey) return null
    var hue = ((hash32(appKey) / 4294967296) * 0.618033988749895) % 1
    return Qt.hsla(hue, 0.38, 0.22, 1.0)
}
