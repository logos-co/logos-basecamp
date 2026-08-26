#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqml.h>

// Transient: what the install pipeline is doing to a row right now.
class InstallStage : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Use InstallStage.Downloading etc.; not instantiable.")
public:
    enum Value {
        None = 0,
        Downloading,
        Queued,
        Installing,
        Installed,
        Failed,
    };
    Q_ENUM(Value)

    static QString toString(Value v) {
        switch (v) {
        case Downloading: return QStringLiteral("downloading");
        case Queued:      return QStringLiteral("queued");
        case Installing:  return QStringLiteral("installing");
        case Installed:   return QStringLiteral("installed");
        case Failed:      return QStringLiteral("failed");
        case None:        return QString();
        }
        return QString();
    }

    static Value fromString(const QString& s) {
        if (s == QStringLiteral("downloading")) return Downloading;
        if (s == QStringLiteral("queued"))      return Queued;
        if (s == QStringLiteral("installing"))  return Installing;
        if (s == QStringLiteral("installed"))   return Installed;
        if (s == QStringLiteral("failed"))      return Failed;
        return None;
    }
};

// Steady-state: catalog row vs disk comparison. Mirrors PMUI's
// PackageTypes::InstallStatus
class InstallStatus : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Use InstallStatus.Installed etc.; not instantiable.")
public:
    enum Value {
        NotInstalled = 0,
        Installed,            // installed AND this repo's rootHash matches disk
        UpgradeAvailable,     // installed at older version than this repo offers
        DowngradeAvailable,   // installed at newer version than this repo offers
        DifferentHash,        // installed at SAME version, different rootHash → Reinstall
    };
    Q_ENUM(Value)
};
