#include "MainContainer.h"
#include "InstallStage.h"
#include "MainUIBackend.h"
#include "mdiview.h"

#include <QQuickWidget>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <qqml.h>
#include <QVBoxLayout>
#include <QLabel>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QQuickItem>
#include <QProcessEnvironment>
#include <QColor>
#include <QPalette>
#include <QTimer>

namespace {
constexpr int kAppsStackIndex     = 0;  // MdiView (C++ widget)
constexpr int kContentStackIndex  = 1;  // ContentViews.qml (App Manager + Settings)
constexpr int kModulesStackIndex  = 2;  // package_manager_ui (sandboxed QQuickWidget)
}

MainContainer::MainContainer(LogosAPI* logosAPI, QWidget* parent)
    : QWidget(parent)
    , m_logosAPI(logosAPI)
    , m_backend(nullptr)
    , m_sidebarWidget(nullptr)
    , m_contentStack(nullptr)
    , m_mdiView(nullptr)
    , m_contentWidget(nullptr)
    , m_overlayWidget(nullptr)
{
    // Set QML style
    QQuickStyle::setStyle("Basic");

    qmlRegisterUncreatableType<InstallStage>("Basecamp.Backend", 1, 0,
        "InstallStage",
        QStringLiteral("Use InstallStage.Downloading etc.; not instantiable."));
    
    // Create backend
    m_backend = new MainUIBackend(m_logosAPI, this);
    
    setupUi();
    
    // Connect section index changes
    connect(m_backend, &MainUIBackend::currentActiveSectionIndexChanged, 
            this, &MainContainer::onViewIndexChanged);
    connect(m_backend, &MainUIBackend::navigateToApps, this, [this]() {
        if (m_suppressNextNavToApps) {
            m_suppressNextNavToApps = false;
            return;
        }
        onNavigateToApps();
    });

    connect(m_backend, &MainUIBackend::pluginWindowRequested, this,
        [this](QWidget* widget, const QString& title) {
            if (title == QStringLiteral("package_manager_ui") && !m_pmuiWidget) {
                m_pmuiWidget = widget;
                QWidget* placeholder = m_contentStack->widget(kModulesStackIndex);
                widget->setParent(m_contentStack);
                m_contentStack->insertWidget(kModulesStackIndex, widget);
                if (placeholder) {
                    m_contentStack->removeWidget(placeholder);
                    placeholder->deleteLater();
                }
                m_suppressNextNavToApps = true;
                return;
            }
            onPluginWindowRequested(widget, title);
        });
    connect(m_backend, &MainUIBackend::pluginWindowRemoveRequested,
            this, &MainContainer::onPluginWindowRemoveRequested);
    connect(m_backend, &MainUIBackend::pluginWindowActivateRequested,
            this, &MainContainer::onPluginWindowActivateRequested);

    // When user closes a plugin window (tab/window X), notify backend to unload
    connect(m_mdiView, &MdiView::pluginWindowClosed,
            m_backend, &MainUIBackend::onPluginWindowClosed);

    // Connect to QML signals from SidebarPanel.
    //
    // launchUIModule uses QueuedConnection — the signal is emitted from a
    // SidebarAppDelegate.onClicked handler inside a Repeater delegate.
    // onAppLauncherClicked calls setCurrentVisibleApp which synchronously
    // emits launcherAppsChanged, causing both sidebar Repeaters to reset
    // their models. If the connection were direct the Repeater would call
    // setParentItem(nullptr) on the clicked delegate while its click handler
    // is still on the call stack, leading to a null deref in
    // QQuickItemPrivate::derefWindow. Queuing the call lets the click handler
    // return before any Repeater model update fires.
    QObject* sidebarRoot = m_sidebarWidget->rootObject();
    if (sidebarRoot) {
        connect(sidebarRoot, SIGNAL(launchUIModule(QString)),
                m_backend, SLOT(onAppLauncherClicked(QString)),
                Qt::QueuedConnection);
        connect(sidebarRoot, SIGNAL(updateLauncherIndex(int)),
                m_backend, SLOT(setCurrentActiveSectionIndex(int)));
    }

    qDebug() << "MainContainer created";
}

MainContainer::~MainContainer()
{
    qDebug() << "MainContainer destroyed";
}

// Using this function to load qml files from local path instead of qrc
QUrl MainContainer::resolveQmlUrl(const QString& qmlFile)
{
    QString qmlUiPath =  QProcessEnvironment::systemEnvironment().value("QML_UI", "");

    if (!qmlUiPath.isEmpty()) {
        QDir qmlDir(qmlUiPath);
        QString fullPath = qmlDir.absoluteFilePath(qmlFile);

        if (QFile::exists(fullPath)) {
            qDebug() << "Loading from filesystem " << fullPath;
            return QUrl::fromLocalFile(fullPath);
        }
    }

    qDebug() << "Loading from resources " << qmlFile;
    QString resourcePath = "qrc:/" + qmlFile;
    return QUrl(resourcePath);
}

void MainContainer::setupUi()
{
    // We would likely move this to qml and use Logos.Theme instead
    QColor bgColor("#171717");
    // set background color
    setAutoFillBackground(true);
    QPalette p = palette();
    p.setColor(QPalette::Window, bgColor);
    setPalette(p);

    // Create main horizontal layout
    m_mainLayout = new QHBoxLayout(this);
    m_mainLayout->setSpacing(0);
    m_mainLayout->setContentsMargins(4, 0, 4, 2);
    // When QML_UI is set, add it to each QML engine's import path so nested
    // components (e.g. SidebarIconButton) load from disk — no rebuild for UI changes.
    QString qmlUiPath = QProcessEnvironment::systemEnvironment().value("QML_UI", "");

    // === SIDEBAR (QML) ===
    m_sidebarWidget = new QQuickWidget(this);
    m_sidebarWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    if (!qmlUiPath.isEmpty()) {
        QString absPath = QDir(qmlUiPath).absolutePath();
        m_sidebarWidget->engine()->addImportPath(absPath + "/qml");
        m_sidebarWidget->engine()->addImportPath(absPath);
        qDebug() << "DEV MODE: Added QML import paths:" << absPath + "/qml" << absPath;
    } else {
        m_sidebarWidget->engine()->addImportPath("qrc:/qml");
    }
    qDebug() << "Sidebar engine import paths:" << m_sidebarWidget->engine()->importPathList();
    m_sidebarWidget->rootContext()->setContextProperty("backend", m_backend);
    m_sidebarWidget->setSource(resolveQmlUrl("qml/panels/SidebarPanel.qml"));
    m_sidebarWidget->setMinimumWidth(60);
    m_sidebarWidget->setMaximumWidth(60);
    // set clear color to sidebar so that rounded corners don't show white
    m_sidebarWidget->setClearColor(bgColor);

    // === CONTENT AREA (vertical layout with stack + app launcher) ===
    QWidget* contentArea = new QWidget(this);
    QVBoxLayout* contentLayout = new QVBoxLayout(contentArea);
    contentLayout->setSpacing(0);
    contentLayout->setContentsMargins(4, 9, 4, 4);
    // Create content stack
    m_contentStack = new QStackedWidget(contentArea);
    m_contentStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    
    // Index 0: MdiView (C++ widget)
    m_mdiView = new MdiView(m_contentStack);
    m_contentStack->addWidget(m_mdiView);
    
    // Index 1: QML content views (Dashboard, Modules, PackageManager, Settings)
    m_contentWidget = new QQuickWidget(m_contentStack);
    m_contentWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    if (!qmlUiPath.isEmpty()) {
        QString absPath = QDir(qmlUiPath).absolutePath();
        m_contentWidget->engine()->addImportPath(absPath + "/qml");
        m_contentWidget->engine()->addImportPath(absPath);
    } else {
        m_contentWidget->engine()->addImportPath("qrc:/qml");
    }
    m_contentWidget->rootContext()->setContextProperty("backend", m_backend);
    m_contentWidget->setSource(resolveQmlUrl("qml/views/ContentViews.qml"));
    m_contentStack->addWidget(m_contentWidget);

    // Index 2: placeholder for package_manager_ui — shows a centered
    // "Loading…" label until PMUI's QQuickWidget arrives via the
    // pluginWindowRequested intercept
    QWidget* pmuiPlaceholder = new QWidget(m_contentStack);
    pmuiPlaceholder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    {
        QVBoxLayout* phLayout = new QVBoxLayout(pmuiPlaceholder);
        phLayout->setAlignment(Qt::AlignCenter);
        QLabel* loadingLabel = new QLabel(QStringLiteral("Loading Package Manager…"),
                                          pmuiPlaceholder);
        loadingLabel->setAlignment(Qt::AlignCenter);
        loadingLabel->setStyleSheet(QStringLiteral(
            "color: #a0a0a0; font-size: 14px;"));
        phLayout->addWidget(loadingLabel);
    }
    m_contentStack->addWidget(pmuiPlaceholder);

    // Add widgets to content layout
    contentLayout->addWidget(m_contentStack, 1);

    // Add widgets to main layout
    m_mainLayout->addWidget(m_sidebarWidget);
    m_mainLayout->addWidget(contentArea, 1);

    // === OVERLAY DIALOGS (QML) ===
    // Child of `this` but deliberately NOT added to m_mainLayout — we
    // want it to float across the whole window, overlapping sidebar +
    // content. resizeEvent keeps its geometry in sync with the parent.
    m_overlayWidget = new QQuickWidget(this);
    m_overlayWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    // Transparent clear so the sidebar + content stay visible through
    // the overlay. The dialog itself paints its own opaque background.
    m_overlayWidget->setAttribute(Qt::WA_AlwaysStackOnTop);
    m_overlayWidget->setAttribute(Qt::WA_TranslucentBackground);
    m_overlayWidget->setClearColor(Qt::transparent);
    // Start transparent-to-input so the user can interact with the
    // normal UI; flipped off in onOverlayActiveChanged while a dialog
    // is visible so the dialog itself can receive clicks.
    m_overlayWidget->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    if (!qmlUiPath.isEmpty()) {
        QString absPath = QDir(qmlUiPath).absolutePath();
        m_overlayWidget->engine()->addImportPath(absPath + "/qml");
        m_overlayWidget->engine()->addImportPath(absPath);
    } else {
        m_overlayWidget->engine()->addImportPath("qrc:/qml");
    }
    m_overlayWidget->rootContext()->setContextProperty("backend", m_backend);
    m_overlayWidget->setSource(resolveQmlUrl("qml/views/OverlayDialogs.qml"));

    // Hook up the QML signal that tracks "any dialog visible" so we can
    // toggle mouse-passthrough on the overlay QQuickWidget.
    if (QObject* overlayRoot = m_overlayWidget->rootObject()) {
        connect(overlayRoot, SIGNAL(overlayActiveChanged(bool)),
                this, SLOT(onOverlayActiveChanged(bool)));
    }

    // Set initial state
    m_contentStack->setCurrentIndex(kAppsStackIndex); // Show MdiView by default

    // Set reasonable minimum size
    setMinimumSize(800, 600);
}

void MainContainer::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_overlayWidget) {
        m_overlayWidget->setGeometry(0, 0, width(), height());
        // Qt re-stacks siblings on resize in some cases; keep the
        // overlay on top explicitly.
        m_overlayWidget->raise();
    }
}

void MainContainer::onOverlayActiveChanged(bool active)
{
    if (!m_overlayWidget) return;
    // When a dialog is open, the overlay must catch the click on the
    // Cancel/Continue buttons — so make it opaque to input. When no
    // dialog is showing, pass every click through to the sidebar /
    // content behind it.
    m_overlayWidget->setAttribute(Qt::WA_TransparentForMouseEvents, !active);
    if (active) m_overlayWidget->raise();
}

void MainContainer::onViewIndexChanged()
{
    int sectionIndex = m_backend->currentActiveSectionIndex();
    
    qDebug() << "MainContainer: Active section index changed to" << sectionIndex;

    //   0 (Workspace)        → MdiView
    //   1 (Applications)     → ContentViews.qml (App Manager view)
    //   2 (Package Manager)  → package_manager_ui (preloaded in background)
    //   3 (Settings)         → ContentViews.qml (StackLayout picks the page)
    switch (sectionIndex) {
    case 0: m_contentStack->setCurrentIndex(kAppsStackIndex);    break;
    case 1: m_contentStack->setCurrentIndex(kContentStackIndex); break;
    case 2:
        if (!m_pmuiWidget) {
            m_backend->loadUiModule(QStringLiteral("package_manager_ui"));
        }
        m_contentStack->setCurrentIndex(kModulesStackIndex);
        break;
    case 3: m_contentStack->setCurrentIndex(kContentStackIndex); break;
    default: break;
    }
}

void MainContainer::onNavigateToApps()
{
    // This is called when an app is loaded and we need to switch to Apps view
    m_backend->setCurrentActiveSectionIndex(0);
}

void MainContainer::onPluginWindowRequested(QWidget* widget, const QString& title)
{
    if (m_mdiView && widget) {
        m_mdiView->addPluginWindow(widget, title);
        qDebug() << "MainContainer: Added plugin window to MdiView:" << title;
    }
}

void MainContainer::onPluginWindowRemoveRequested(QWidget* widget)
{
    if (widget && widget == m_pmuiWidget) return;
    if (m_mdiView && widget) {
        m_mdiView->removePluginWindow(widget);
        qDebug() << "MainContainer: Removed plugin window from MdiView";
    }
}

void MainContainer::onPluginWindowActivateRequested(QWidget* widget)
{
    if (widget && widget == m_pmuiWidget) return;
    if (m_mdiView && widget) {
        m_mdiView->activatePluginWindow(widget);
        qDebug() << "MainContainer: Activated plugin window in MdiView";
    }
}

