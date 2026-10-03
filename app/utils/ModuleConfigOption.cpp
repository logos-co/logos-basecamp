#include "ModuleConfigOption.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>

namespace LogosBasecamp {

ModuleConfigResolution resolveModuleConfig(const QString& arg)
{
    ModuleConfigResolution out;
    const QString trimmed = arg.trimmed();
    if (trimmed.isEmpty())
        return out;

    QString content;
    QString source;
    if (trimmed.startsWith(QLatin1Char('{'))) {
        content = trimmed;
        source = QStringLiteral("inline --module-config JSON");
    } else {
        QFile f(trimmed);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            out.ok = false;
            out.error = QStringLiteral("--module-config file '%1' could not be opened: %2")
                            .arg(trimmed, f.errorString());
            return out;
        }
        content = QString::fromUtf8(f.readAll());
        source = QStringLiteral("--module-config file '%1'").arg(trimmed);
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        out.ok = false;
        out.error = QStringLiteral("%1 is not valid JSON: %2 (at offset %3)")
                        .arg(source, err.errorString())
                        .arg(err.offset);
        return out;
    }
    if (!doc.isObject()) {
        out.ok = false;
        out.error = QStringLiteral("%1 must map module names to their configuration").arg(source);
        return out;
    }
    // Module names as the runtime takes them; it refuses the rest at start.
    static const QRegularExpression moduleName(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    for (const QString& name : doc.object().keys()) {
        if (!moduleName.match(name).hasMatch()) {
            out.ok = false;
            out.error = QStringLiteral("%1: '%2' is not a module name").arg(source, name);
            return out;
        }
    }
    out.configJson = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
    return out;
}

} // namespace LogosBasecamp
