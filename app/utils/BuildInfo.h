#pragma once

// Single place that pulls the nix-generated logos_build_info.h and exposes
// the values to the two consumers:
//   * app/main.cpp       — logs a banner to the per-session log on startup.
//   * src/MainUIBackend  — exposes Q_PROPERTYs for the Dashboard view.
//
// Header-only because the generated header lands in different include dirs
// for each CMake target (app/generated/ vs src/generated_code/), so every
// translation unit must do its own __has_include check. Keeping everything
// inline avoids having to wire a separate .cpp into both targets.
//
// Non-nix builds see no logos_build_info.h; accessors return empty values
// so callers can still render / log something sane.

#include <QByteArray>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#if __has_include("logos_build_info.h")
#  include "logos_build_info.h"
#  define LOGOS_BASECAMP_HAS_BUILD_INFO 1
#else
#  define LOGOS_BASECAMP_HAS_BUILD_INFO 0
#endif

namespace LogosBasecampBuildInfo {

// VERSION file contents, baked in at build time. Empty on dev branches
// (VERSION is only checked in on release branches) and in non-nix builds.
inline QString version() {
#if LOGOS_BASECAMP_HAS_BUILD_INFO
    return QStringLiteral(LOGOS_BASECAMP_VERSION);
#else
    return {};
#endif
}

// True for distributed / portable builds (AppImage, DMG) — driven by the
// LOGOS_PORTABLE_BUILD compile define set by nix/app.nix and nix/main-ui.nix.
// Each translation unit sees its own define, so this resolves correctly in
// both the app binary and the main_ui plugin.
inline bool isPortableBuild() {
#ifdef LOGOS_PORTABLE_BUILD
    return true;
#else
    return false;
#endif
}

// List of {name, commit} entries for logos-basecamp + each flake input,
// populated from the nix-generated JSON blob. Empty in non-nix builds.
inline QVariantList commits() {
    QVariantList out;
#if LOGOS_BASECAMP_HAS_BUILD_INFO
    const auto doc = QJsonDocument::fromJson(
        QByteArray::fromRawData(logos_basecamp_build_info::kCommitsJson,
                                 qstrlen(logos_basecamp_build_info::kCommitsJson)));
    if (doc.isArray()) {
        for (const QJsonValue& v : doc.array()) {
            const QJsonObject obj = v.toObject();
            QVariantMap entry;
            entry["name"] = obj.value("name").toString();
            entry["commit"] = obj.value("commit").toString();
            out.append(entry);
        }
    }
#endif
    return out;
}

// Prints version + dev/portable marker + commit hashes at startup so the
// per-session log captures exactly which sources produced this binary.
inline void logStartupBanner() {
    const char* buildType = isPortableBuild() ? "portable" : "dev";
#if LOGOS_BASECAMP_HAS_BUILD_INFO
    const QString v = version();
    if (!v.isEmpty()) {
        qInfo().noquote() << QString("LogosBasecamp version %1 (%2 build)")
                                 .arg(v, QString::fromUtf8(buildType));
    } else {
        qInfo().noquote() << QString("LogosBasecamp (%1 build, unreleased)")
                                 .arg(QString::fromUtf8(buildType));
    }
    const QVariantList cs = commits();
    if (!cs.isEmpty()) {
        qInfo().noquote() << "Build commits:";
        for (const QVariant& c : cs) {
            const QVariantMap m = c.toMap();
            qInfo().noquote() << QString("  - %1 %2")
                                     .arg(m.value("name").toString(),
                                          m.value("commit").toString());
        }
    }
#else
    qInfo().noquote() << QString("LogosBasecamp (%1 build, no build info)")
                             .arg(QString::fromUtf8(buildType));
#endif
}

} // namespace LogosBasecampBuildInfo
