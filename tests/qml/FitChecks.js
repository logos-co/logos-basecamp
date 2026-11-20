.pragma library

// What reaches past `root`'s sides: the design system's PhoneChecks.js check,
// but through clipping items too. A page's scroll view clips whatever runs off
// it, and that is what this looks for. Of parts that reach past, only the
// outermost is listed; a line of text that spills out of its own box counts.

// overflow(root) -> [items]
function overflow(root) {
    const found = []
    walk(root, root, found)
    return found
}

function walk(root, item, found) {
    for (let i = 0; i < item.children.length; ++i) {
        const child = item.children[i]
        if (!child.visible || child.opacity === 0 || drawsNothing(child))
            continue
        if (child.width > 0 && child.height > 0) {
            const span = extent(child, root)
            if (span.left < -0.5 || span.right > root.width + 0.5) {
                found.push(child)
                continue
            }
        }
        walk(root, child, found)
    }
}

// Holds nothing and paints nothing itself, whatever its size: an unloaded
// Loader, which keeps its last size, or a list's default highlight, an empty
// Item that follows the current row's size with an animation.
function drawsNothing(item) {
    return item.children.length === 0
           && ("sourceComponent" in item || !("color" in item || "text" in item || "source" in item))
}

// Where a part is painted, across: its box, or its text where an unwrapped,
// unelided line is wider than the box.
function extent(item, root) {
    let left = item.mapToItem(root, 0, 0).x
    let right = left + item.width
    const spill = "contentWidth" in item && "wrapMode" in item && item.wrapMode === 0
                  && (item.elide === undefined || item.elide === 3)   // NoWrap; ElideNone
                  ? item.contentWidth - item.width : 0
    if (spill > 0.5) {
        if (item.horizontalAlignment === 2)          // AlignRight
            left -= spill
        else if (item.horizontalAlignment === 4) {   // AlignHCenter
            left -= spill / 2
            right += spill / 2
        } else
            right += spill
    }
    return { left: left, right: right }
}

// "LogosText "Docked layout" 220×22 at 16,410", for a failure message.
function describe(item, root) {
    const type = String(item).replace(/_QMLTYPE_\d+|_QML_\d+|\(0x[0-9a-f]+\)/g, "").trim()
    const name = item.objectName ? " " + item.objectName : ""
    const text = typeof item.text === "string" && item.text !== "" ? " \"" + item.text.slice(0, 40) + "\"" : ""
    const span = extent(item, root)
    return type + name + text + " " + Math.round(span.right - span.left) + "×" + Math.round(item.height)
           + " at " + Math.round(span.left) + "," + Math.round(item.mapToItem(root, 0, 0).y)
}
