#include "MainContainer.h"
#include "MainUIBackend.h"
#include "UIPluginManager.h"

#include <QDebug>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWidget>
#include <QStringList>
#include <QUrl>

// Forward-declared: only used as a pointer in the pluginPublished
// signal signature.
class ViewModuleHost;

MainContainer::MainContainer(LogosAPI* logosAPI, QWidget* parent)
    : QWidget(parent)
    , m_backend(nullptr)
    , m_logosAPI(logosAPI)
{
    QQuickStyle::setStyle("Basic");

    m_backend = new MainUIBackend(m_logosAPI, this);

    // The entire UI lives in a single QML scene hosted by qmlHost. backend
    // is exposed via rootContext so QML can bind to its properties.
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* qmlHost = new QQuickWidget(this);
    qmlHost->setResizeMode(QQuickWidget::SizeRootObjectToView);
    qmlHost->rootContext()->setContextProperty("backend", m_backend);
    qmlHost->setSource(QUrl(QStringLiteral(
        "qrc:/qt/qml/Basecamp/Views/qml/views/MainContainer.qml")));
    layout->addWidget(qmlHost);

    // === PMUI ===
    if (auto* uipm = m_backend->uiPluginManager()) {
        connect(uipm, &UIPluginManager::pluginPublished, this,
            [this, qmlHost](const QString& name, const QUrl& viewUrl,
                            QObject* bridge, ViewModuleHost*) {
                if (name != QStringLiteral("package_manager_ui")) return;
                const QString viewPath = viewUrl.toLocalFile();
                const QString viewDir = QFileInfo(viewPath).absolutePath();
                const QString installDir = QFileInfo(viewDir).absolutePath();
                QStringList paths = qmlHost->engine()->importPathList();
                if (!paths.contains(viewDir))    paths.prepend(viewDir);
                if (!paths.contains(installDir)) paths.prepend(installDir);
                qmlHost->engine()->setImportPathList(paths);
                qmlHost->rootContext()->setContextProperty("logos", bridge);
                // Publish the URL only after the engine is wired so the
                // QML Loader sees a usable environment the moment its
                // source binding fires.
                m_backend->setPackageManagerViewUrl(viewUrl);
            });
    }

    // Lazy-load PMUI on first entry to section 2.
    connect(m_backend, &MainUIBackend::currentActiveSectionIndexChanged,
        this, [this]() {
            if (m_backend->currentActiveSectionIndex() == 2
                && m_backend->packageManagerViewUrl().isEmpty())
                m_backend->loadUiModule(QStringLiteral("package_manager_ui"));
        });
}

MainContainer::~MainContainer() = default;
