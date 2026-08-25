#include "AccessPolicyOption.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>

namespace LogosBasecamp {

AccessPolicyResolution resolveAccessPolicy(const QString& arg)
{
    AccessPolicyResolution out;

    const QString trimmed = arg.trimmed();
    if (trimmed.isEmpty())
        return out;  // ok, no policy — the default

    // Checked before the file branch, so `--access-policy enforce` never gets
    // read as a relative path named "enforce" (which would make arming
    // enforcement depend on the working directory the app was launched from).
    if (trimmed == QLatin1String(kAccessPolicyEnforceAlias)) {
        out.policyJson = QString::fromUtf8(kAccessPolicyEnforceEnvelope);
        return out;
    }

    QString content;
    QString source;
    if (trimmed.startsWith(QLatin1Char('{'))) {
        content = trimmed;
        source = QStringLiteral("inline --access-policy JSON");
    } else {
        QFile f(trimmed);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            out.ok = false;
            out.error = QStringLiteral("--access-policy file '%1' could not be opened: %2")
                            .arg(trimmed, f.errorString());
            return out;
        }
        content = QString::fromUtf8(f.readAll());
        source = QStringLiteral("--access-policy file '%1'").arg(trimmed);
    }

    QJsonParseError err{};
    QJsonDocument::fromJson(content.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        out.ok = false;
        out.error = QStringLiteral("%1 is not valid JSON: %2 (at offset %3)")
                        .arg(source, err.errorString())
                        .arg(err.offset);
        return out;
    }

    out.policyJson = content;
    return out;
}

} // namespace LogosBasecamp
