#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqml.h>

// Which top-level section of the shell is in front.
class ShellSection : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Use ShellSection.Workspace etc.; not instantiable.")
public:
    enum Value {
        Workspace = 0,       // WorkspaceArea — plugin docks, or WelcomePage
        AppManager = 1,      // ContentViews.qml → app catalog / inspector
        PackageManager = 2,  // package_manager_ui, hoisted into the stack
        Settings = 3,        // ContentViews.qml → settings pages
    };
    Q_ENUM(Value)

    static QString toString(Value v) {
        switch (v) {
        case Workspace:      return QStringLiteral("workspace");
        case AppManager:     return QStringLiteral("app_manager");
        case PackageManager: return QStringLiteral("package_manager");
        case Settings:       return QStringLiteral("settings");
        }
        return QString();
    }
    
    static bool isValid(int v) {
        return v >= Workspace && v <= Settings;
    }
};
