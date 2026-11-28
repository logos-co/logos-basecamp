#pragma once

#include <QJsonArray>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <functional>
#include <string>

// The Module Inspector's arguments: what was typed for each parameter, read as
// that parameter's type. `parameters` is the method's list from
// getPluginMethods ({name, type} each).
struct InspectorArgs {
    QVariantList values;
    QString error;   // empty when every text was read
};

InspectorArgs inspectorArgs(const QJsonArray& parameters, const QStringList& texts);

// What a call returned: its result as JSON, or a logos::CallError's code and
// message.
struct InspectorReply {
    QJsonValue result;
    std::string code;   // empty = no error
    std::string message;
};
using InspectorInvoke = std::function<InspectorReply(const QVariantList& args)>;

// The Module Inspector's call, as QML shows it: {"result": ...}, or {"error": ...}
// when the arguments can't be read (nothing is called) or the call failed.
// `argsJson` is a JSON array of the typed texts.
QString inspectorCall(const QString& module, const QString& parametersJson,
                      const QString& argsJson, int timeoutMs, const InspectorInvoke& invoke);
