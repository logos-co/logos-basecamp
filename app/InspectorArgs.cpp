#include "InspectorArgs.h"
#include "ModuleCallFailure.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace {

const QSet<QString> kSigned = {
    QStringLiteral("int"), QStringLiteral("qint32"), QStringLiteral("long"),
    QStringLiteral("qint64"), QStringLiteral("qlonglong"),
};
const QSet<QString> kUnsigned = {
    QStringLiteral("uint"), QStringLiteral("quint32"), QStringLiteral("ulong"),
    QStringLiteral("quint64"), QStringLiteral("qulonglong"),
};
const QSet<QString> kLists = {
    QStringLiteral("QVariantList"), QStringLiteral("QJsonArray"),
};
const QSet<QString> kObjects = {
    QStringLiteral("QVariantMap"), QStringLiteral("QJsonObject"),
};

bool readJson(const QString& text, QJsonDocument* doc)
{
    QJsonParseError err;
    *doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    return err.error == QJsonParseError::NoError;
}

// One parameter's value; `*error` says why when it could not be read.
QVariant read(const QString& type, const QString& text, QString* error)
{
    const QString t = text.trimmed();
    if (kSigned.contains(type)) {
        bool ok = false;
        const qlonglong v = t.toLongLong(&ok);
        if (!ok) *error = QStringLiteral("expected a whole number");
        return v;
    }
    if (kUnsigned.contains(type)) {
        bool ok = false;
        const qulonglong v = t.toULongLong(&ok);
        if (!ok) *error = QStringLiteral("expected a whole number, 0 or more");
        return v;
    }
    if (type == QLatin1String("double") || type == QLatin1String("float")) {
        bool ok = false;
        const double v = t.toDouble(&ok);
        if (!ok) *error = QStringLiteral("expected a number");
        return v;
    }
    if (type == QLatin1String("bool")) {
        if (t.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0) return true;
        if (t.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0) return false;
        *error = QStringLiteral("expected true or false");
        return {};
    }
    if (type == QLatin1String("QStringList")) {
        QJsonDocument doc;
        if (!readJson(t, &doc) || !doc.isArray()) {
            *error = QStringLiteral("expected a JSON list of strings, like [\"a\", \"b\"]");
            return {};
        }
        QStringList list;
        for (const QJsonValue& v : doc.array()) {
            if (!v.isString()) {
                *error = QStringLiteral("expected a JSON list of strings, like [\"a\", \"b\"]");
                return {};
            }
            list.append(v.toString());
        }
        return list;
    }
    if (kLists.contains(type) || type.startsWith(QLatin1String("QList<"))) {
        QJsonDocument doc;
        if (!readJson(t, &doc) || !doc.isArray()) {
            *error = QStringLiteral("expected a JSON list, like [1, 2]");
            return {};
        }
        return doc.array().toVariantList();
    }
    if (kObjects.contains(type) || type.startsWith(QLatin1String("QMap<"))) {
        QJsonDocument doc;
        if (!readJson(t, &doc) || !doc.isObject()) {
            *error = QStringLiteral("expected a JSON object, like {\"key\": 1}");
            return {};
        }
        return doc.object().toVariantMap();
    }
    if (type == QLatin1String("QByteArray")) return text.toUtf8();
    if (type == QLatin1String("QVariant")) {
        // Any JSON value as itself; anything else as the text.
        QJsonDocument doc;
        if (readJson(QStringLiteral("[") + t + QStringLiteral("]"), &doc) && doc.array().size() == 1)
            return doc.array().first().toVariant();
        return text;
    }
    // QString, and any type not known here: as typed.
    return text;
}

} // namespace

InspectorArgs inspectorArgs(const QJsonArray& parameters, const QStringList& texts)
{
    InspectorArgs out;
    if (texts.size() != parameters.size()) {
        out.error = QStringLiteral("expected %1 arguments, got %2")
                        .arg(parameters.size()).arg(texts.size());
        return out;
    }
    for (int i = 0; i < parameters.size(); ++i) {
        const QJsonObject p = parameters.at(i).toObject();
        const QString type = p.value(QStringLiteral("type")).toString();
        QString name = p.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) name = QStringLiteral("argument %1").arg(i + 1);
        QString error;
        const QVariant v = read(type, texts.at(i), &error);
        if (!error.isEmpty()) {
            out.values.clear();
            out.error = QStringLiteral("%1: %2").arg(name, error);
            return out;
        }
        out.values.append(v);
    }
    return out;
}

QString inspectorCall(const QString& module, const QString& parametersJson,
                      const QString& argsJson, int timeoutMs, const InspectorInvoke& invoke)
{
    auto payload = [](const char* key, const QJsonValue& value) {
        return QString::fromUtf8(QJsonDocument(QJsonObject{{key, value}})
                                     .toJson(QJsonDocument::Compact));
    };

    QStringList texts;
    for (const QJsonValue& v : QJsonDocument::fromJson(argsJson.toUtf8()).array())
        texts.append(v.toString());
    const InspectorArgs args = inspectorArgs(
        QJsonDocument::fromJson(parametersJson.toUtf8()).array(), texts);
    if (!args.error.isEmpty()) return payload("error", args.error);

    const InspectorReply reply = invoke(args.values);
    if (!reply.code.empty())
        return payload("error", moduleCallFailure(module, reply.code, reply.message, timeoutMs));
    return payload("result", reply.result);
}
