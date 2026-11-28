#pragma once

#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <QVariantList>

// The Module Inspector's arguments: what was typed for each parameter, read as
// that parameter's type. `parameters` is the method's list from
// getPluginMethods ({name, type} each).
struct InspectorArgs {
    QVariantList values;
    QString error;   // empty when every text was read
};

InspectorArgs inspectorArgs(const QJsonArray& parameters, const QStringList& texts);
