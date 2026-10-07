#ifndef SHELL_DEV_QML_H
#define SHELL_DEV_QML_H

#include <QString>
#include <QUrl>

#include <functional>

class QObject;
class QQuickWidget;

// Without DEV_QML_PATH, load() is setSource(). LOGOS_QML_HOT_RELOAD=0 keeps
// the source tree but stops watching.
namespace ShellDevQml {

void load(QQuickWidget* view, const QUrl& qrcUrl,
          std::function<void(QObject* root)> wire = {});

} // namespace ShellDevQml

#endif // SHELL_DEV_QML_H
