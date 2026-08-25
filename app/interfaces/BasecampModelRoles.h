#pragma once

// qnamespace.h, not QtGlobal: the role bases are Qt::UserRole, and QtGlobal
// does not declare the Qt namespace. This header is included FIRST by
// AppsModel.h, so there is nothing ahead of it to pull the declaration in.
#include <QtCore/qnamespace.h>

// ─────────────────────────────────────────────────────────────────────────────
// BasecampModelRoles.h — the model role contract, shared across the boundary.
//
// AppsModel and ModuleInstanceModel live host-side; the filter proxies over
// them live in the shell and need the role NUMBERS and no symbol of any kind.
// The models inherit these structs rather than redeclaring the enum, so
// `AppsModel::NameRole` resolves host-side and the shell spells the same
// constant `AppsModelRoles::NameRole` — one definition either way.
//
// Adding a role is a boundary change: append at the END. Renumbering shifts
// values the shell was compiled against, and the failure is a proxy silently
// reading the wrong role rather than anything that fails to build.
// ─────────────────────────────────────────────────────────────────────────────

struct AppsModelRoles {
    enum Roles {
        NameRole = Qt::UserRole + 1,
        RepositoryUrlRole,
        DisplayNameRole,
        DescriptionRole,
        CategoryRole,
        TypeRole,                // "ui_qml" | "core" 
        IconUrlRole,             // resolved icon URL; "" → monogram fallback
        SupportsFullBleedIconRole, // manifest >= 0.4.0: icon is a validated
                                   // 256x256 asset, safe to render edge-to-edge
        VersionsRole,            // QVariantList — all known catalog versions
        DependenciesRole,        // QVariantList — direct deps of versions[0]'s manifest,
                                 //   normalized to [{name, version}, ...]. version "" = no constraint.
        InstalledVersionRole,    // "" when not installed
        LatestVersionRole,       // versions[0].version
        HasUpdateRole,           // installedVersion != "" && installed != latest
        IsInstalledRole,         // installedVersion != ""
        MissingDepsRole,         // QStringList of dep names whose LGX isn't on
                                 //   disk (sourced from
                                 //   PackageCoordinator::m_missingDepsByModule).
                                 //   Empty when the install is complete.
        InstallStatusRole,       // InstallStatus enum — per-row catalog-vs-disk
                                 //   state (Install/Launch/Upgrade/Downgrade/
                                 //   Reinstall in QML terms). PMUI mirror.
        InstallTypeRole,         // "embedded" | "user" | ""
        ActionRole,            
        ToVersionRole,
        IsTopLevelRole,
        ResolverErrorRole,
        InstallStageRole,        // InstallStage::Value (int) — see InstallEnums.h
        InstallErrorRole,        // failure message when InstallStage == Failed
        ProvidesRole,            // QStringList — intents this package can service
    };
};

struct ModuleInstanceRoles {
    enum Roles {
        NameRole = Qt::UserRole + 1,
        LabelRole,             // displayName if set, else name
        DescriptionRole,
        CategoryRole,
        TypeRole,              // "ui_qml" | "core" | ""
        VersionRole,
        IconPathRole,
        InstallTypeRole,       // "user" | "embedded" | ""
        IsLoadedRole,
        IsMainUiRole,
        HasMissingDepsRole,
        StatusTextRole,        // derived: Main UI / Missing deps / Loaded / Not loaded
        CpuRole,               // core modules only; 0 when unknown
        MemoryRole,            // core modules only; 0 when unknown
    };
};
