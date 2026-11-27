#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqml.h>

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
