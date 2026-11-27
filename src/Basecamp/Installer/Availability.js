.pragma library
.import "Types.js" as T

// Why a package can't be installed here, in words.

// A dev build installs only "-dev" variants; published packages are portable.
var portableBuildsUrl = "https://github.com/logos-co/logos-basecamp/#portable-builds"

// Whether `hostVariant` ("linux-x86_64-dev", …) is a dev build's.
function isDevBuild(hostVariant) {
    return (hostVariant || "").endsWith("-dev")
}

function notAvailableText(reason, hostVariant) {
    switch (reason) {
    case T.NotAvailableReason.NoVariantsPublished:
        return qsTr("No installable build is published for this package.")
    case T.NotAvailableReason.BuildFlavorMismatch:
        return isDevBuild(hostVariant)
            ? qsTr("Needs a portable build of Basecamp.")
            : qsTr("Not available for this build flavor (dev / portable / release).")
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
    if (isDevBuild(hostVariant)) {
        return {
            title: qsTr("Nothing installable on a dev build"),
            subtitle: qsTr("Published packages need a portable build of Basecamp.")
        }
    }
    return {
        title: qsTr("Nothing available for this platform"),
        subtitle: hostVariant
            ? qsTr("No configured repository publishes a build for %1.").arg(hostVariant)
            : qsTr("No configured repository publishes a build for this platform.")
    }
}

// The notice above the list on a dev build, as {severity, title, message}, or
// null. The link is text: sandboxed apps can't open URLs.
function devBuildNotice(hostVariant) {
    if (!isDevBuild(hostVariant)) return null
    return {
        severity: "info",
        title: qsTr("This is a dev build of Basecamp"),
        message: qsTr("It installs only dev packages, and published packages are portable. "
                      + "To install them, use a portable build: %1").arg(portableBuildsUrl)
    }
}
