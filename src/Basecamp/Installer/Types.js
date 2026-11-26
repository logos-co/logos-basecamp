.pragma library

// The values of include/logos/installer/Types.h, for code that has no
// generated enums at hand. tests/cpp/tst_types.cpp keeps the two equal.

var InstallStatus = {
    NotInstalled: 0,
    Installing: 1,
    Installed: 2,
    Failed: 3,
    UpgradeAvailable: 4,
    DowngradeAvailable: 5,
    DifferentHash: 6
}

var InstallStage = {
    None: 0,
    Downloading: 1,
    Queued: 2,
    Installing: 3,
    Installed: 4,
    Failed: 5,
    Downloaded: 6
}

var RowAction = {
    Install: 0,
    Upgrade: 1,
    Downgrade: 2,
    Reinstall: 3,
    Retry: 4,
    NotAvailable: 5,
    NoOp: 6
}

var NotAvailableReason = {
    Available: 0,
    NoVariantsPublished: 1,
    BuildFlavorMismatch: 2,
    PlatformMismatch: 3,
    NotOnLogosStorage: 4,
    NotOverHttp: 5
}

var DownloadSource = {
    NoSource: 0,
    Builtin: 1,
    GitHub: 2,
    Storage: 3,
    LocalFile: 4
}
