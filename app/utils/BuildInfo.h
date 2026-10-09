#pragma once

// Build info for its two consumers:
//   * app/main.cpp       — startup banner in the per-session log.
//   * app/MainUIBackend  — Q_PROPERTYs for the Dashboard view.
//
// Read at runtime from ../buildinfo/build-info.json next to the executable,
// which nix/app.nix stages (not compiled in, so input bumps do not recompile
// the app). Without that file (non-nix builds) accessors return empty values
// so callers can still render / log something sane.

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

namespace LogosBasecampBuildInfo {

// The parsed build-info.json, read once; empty when there is none. Callers
// run after QApplication exists, which applicationDirPath() needs.
inline const QJsonObject& data() {
    static const QJsonObject obj = [] {
        QFile f(QDir::cleanPath(QCoreApplication::applicationDirPath()
                                + "/../buildinfo/build-info.json"));
        if (!f.open(QIODevice::ReadOnly))
            return QJsonObject();
        return QJsonDocument::fromJson(f.readAll()).object();
    }();
    return obj;
}

// VERSION file contents at build time. Empty on dev branches (VERSION is only
// checked in on release branches) and in non-nix builds.
inline QString version() {
    return data().value("version").toString();
}

// True for distributed / portable builds (AppImage, DMG) — driven by the
// LOGOS_PORTABLE_BUILD compile define set by nix/app.nix.
inline bool isPortableBuild() {
#ifdef LOGOS_PORTABLE_BUILD
    return true;
#else
    return false;
#endif
}

// List of {name, commit} entries for logos-basecamp + each flake input.
// Empty in non-nix builds.
inline QVariantList commits() {
    QVariantList out;
    for (const QJsonValue& v : data().value("commits").toArray()) {
        const QJsonObject obj = v.toObject();
        QVariantMap entry;
        entry["name"] = obj.value("name").toString();
        entry["commit"] = obj.value("commit").toString();
        out.append(entry);
    }
    return out;
}

// Prints version + dev/portable marker + commit hashes at startup so the
// per-session log captures exactly which sources produced this binary.
inline void logStartupBanner() {
    const char* buildType = isPortableBuild() ? "portable" : "dev";
    if (data().isEmpty()) {
        qInfo().noquote() << QString("LogosBasecamp (%1 build, no build info)")
                                 .arg(QString::fromUtf8(buildType));
        return;
    }
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
}

} // namespace LogosBasecampBuildInfo
