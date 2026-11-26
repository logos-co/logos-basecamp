.pragma library
.import "Types.js" as T

// Why a package can't be installed here, in words.

function notAvailableText(reason) {
    switch (reason) {
    case T.NotAvailableReason.NoVariantsPublished:
        return qsTr("No installable build is published for this package.")
    case T.NotAvailableReason.BuildFlavorMismatch:
        return qsTr("Not available for this build flavor (dev / portable / release).")
    case T.NotAvailableReason.PlatformMismatch:
        return qsTr("Not available for this platform.")
    case T.NotAvailableReason.NotOnLogosStorage:
        return qsTr("This version is not on Logos Storage, and the download source is set to Logos only.")
    case T.NotAvailableReason.NotOverHttp:
        return qsTr("This version has no HTTP download, and the download source is set to HTTP only.")
    }
    return qsTr("Not available")
}

// The empty state when every package is hidden as unavailable, as
// {title, subtitle}. `hostVariant` is this host's ("linux-x86_64", …) or "".
function nothingForThisPlatform(hostVariant) {
    return {
        title: qsTr("Nothing available for this platform"),
        subtitle: hostVariant
            ? qsTr("No configured repository publishes a build for %1.").arg(hostVariant)
            : qsTr("No configured repository publishes a build for this platform.")
    }
}
