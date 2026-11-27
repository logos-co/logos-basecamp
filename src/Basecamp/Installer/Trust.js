.pragma library

// Who signed a package, in words and a mark, the same in Basecamp's install
// dialogs and the installer apps. A verdict is Trust.h's
// summary().byName[name]: {status, publisher, impersonates, installed,
// builtIn, reserved, backendTaken, replaces, replacesUnverified}.

// The verdict in words; "" without one.
function description(verdict) {
    if (!verdict || !verdict.status)
        return ""
    if (verdict.installed === true) {
        switch (verdict.status) {
        case "bundled":
            return qsTr("Ships with Basecamp")
        case "trusted":
            return qsTr("Already installed, signed by %1").arg(verdict.publisher || qsTr("a publisher you trust"))
        case "untrusted":
            return qsTr("Already installed, signed by a publisher you don't trust")
        case "unsigned":
            return qsTr("Already installed, not signed")
        }
        return qsTr("Already installed; couldn't check who signed it")
    }
    if (verdict.builtIn === true)
        return qsTr("Comes with Basecamp and updates only with it")
    if (verdict.reserved === true)
        return qsTr("Comes with Basecamp, so only Logos can update it")
    if (verdict.backendTaken === true)
        return qsTr("Another installed package has the name its backend needs")
    if (verdict.replaces)
        return qsTr("Not signed by %1, who signed the version you have").arg(verdict.replaces)
    if (verdict.replacesUnverified === true)
        return qsTr("The version you have isn't signed, so this may not be from the same publisher")
    switch (verdict.status) {
    case "bundled":
        return qsTr("Ships with Basecamp")
    case "trusted":
        return qsTr("Signed by %1").arg(verdict.publisher || qsTr("a publisher you trust"))
    case "untrusted":
        return verdict.impersonates
               ? qsTr("Claims to be %1, but isn't signed with its key").arg(verdict.impersonates)
               : qsTr("Signed by a publisher you don't trust yet")
    case "unsigned":
        return qsTr("Not signed: nobody vouches for it")
    case "invalid":
        return qsTr("Damaged, or its signature doesn't hold")
    }
    return qsTr("Couldn't check who signed it")
}

// How the mark is drawn: "fine", "caution" or "danger", or "muted" for what is
// installed already; each app's theme colours it. "" without a verdict.
function tone(verdict) {
    if (!verdict || !verdict.status)
        return ""
    if (verdict.installed === true)
        return "muted"
    if (verdict.builtIn === true || verdict.reserved === true || verdict.backendTaken === true
            || !!verdict.replaces || verdict.replacesUnverified === true)
        return "danger"
    switch (verdict.status) {
    case "bundled":
    case "trusted":
        return "fine"
    case "invalid":
        return "danger"
    case "untrusted":
        return verdict.impersonates ? "danger" : "caution"
    }
    return "caution"
}

// Whether the mark is a check rather than a warning: signed by someone the
// person trusts, by the same publisher as what it replaces.
function isCheck(verdict) {
    return !!verdict && (verdict.status === "trusted" || verdict.status === "bundled")
        && !verdict.replaces && verdict.replacesUnverified !== true
}
