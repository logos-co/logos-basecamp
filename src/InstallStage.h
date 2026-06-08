#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqml.h>

class InstallStage : public QObject {
    Q_OBJECT
public:
    enum Value {
        None = 0,
        Downloading,
        Queued,
        Installing,
        Done,
        Installed,
        Failed,
    };
    Q_ENUM(Value)

    static QString toString(Value v) {
        switch (v) {
        case Downloading: return QStringLiteral("downloading");
        case Queued:      return QStringLiteral("queued");
        case Installing:  return QStringLiteral("installing");
        case Done:        return QStringLiteral("done");
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
        if (s == QStringLiteral("done"))        return Done;
        if (s == QStringLiteral("installed"))   return Installed;
        if (s == QStringLiteral("failed"))      return Failed;
        return None;
    }
};
