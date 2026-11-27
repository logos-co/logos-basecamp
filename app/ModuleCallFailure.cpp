#include "ModuleCallFailure.h"

QString moduleCallFailure(const QString& module, const std::string& code,
                          const std::string& message, int timeoutMs)
{
    if (code == "timeout") {
        const QString within = timeoutMs % (60 * 1000) != 0
            ? QStringLiteral("%1 s").arg(timeoutMs / 1000)
            : QStringLiteral("%1 min").arg(timeoutMs / (60 * 1000));
        return QStringLiteral("%1 did not answer within %2.").arg(module, within);
    }
    if (!message.empty()) return QString::fromStdString(message);
    return QStringLiteral("%1 did not reply.").arg(module);
}

QString moduleChangeFailure(const QString& module, const std::string& code,
                            const std::string& message, int timeoutMs)
{
    const QString failure = moduleCallFailure(module, code, message, timeoutMs);
    if (code != "timeout") return failure;
    return failure + QStringLiteral(" The change may still be made; the list updates when it is.");
}
