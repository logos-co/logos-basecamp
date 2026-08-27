#include "MainContainer.h"
#include "AppsFilterProxy.h"
#include "InstallEnums.h"
#include "ShortcutBridge.h"
#include "WorkspaceArea.h"

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
#include <QFileInfo>
#include <QQuickItem>
#include <QColor>
#include <QPalette>
#include <QEvent>

namespace {
constexpr int kAppsStackIndex     = 0;  // WorkspaceArea (QDockWidget-based)
constexpr int kContentStackIndex  = 1;  // ContentViews.qml (App Manager + Settings)
constexpr int kModulesStackIndex  = 2;  // package_manager_ui (sandboxed QQuickWidget)

// The Package Manager page before package_manager_ui arrives -- and again after
// it goes away. Built in two places, so it lives here: the stack must keep all
// three pages at all times, because every section switch below indexes into it
// by constant.
QWidget* makePmuiPlaceholder(QWidget* parent)
{
    QWidget* ph = new QWidget(parent);
    ph->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QVBoxLayout* phLayout = new QVBoxLayout(ph);
    phLayout->setAlignment(Qt::AlignCenter);
    QLabel* loadingLabel = new QLabel(QStringLiteral("Loading Package Manager…"), ph);
    loadingLabel->setAlignment(Qt::AlignCenter);
    loadingLabel->setStyleSheet(QStringLiteral("color: #a0a0a0; font-size: 14px;"));
    phLayout->addWidget(loadingLabel);
    return ph;
}

// DEV_QML_PATH: when set, load QML view entry files from the filesystem source
// tree instead of the embedded qrc resource
QString devQmlRoot() {
    const QString dev = QString::fromUtf8(qgetenv("DEV_QML_PATH")).trimmed();
    if (dev.isEmpty()) return QString();
    if (!QFileInfo(dev).isDir()) {
        qWarning().noquote() << "DEV_QML_PATH is not a directory:" << dev
                             << "- using embedded QML";
        return QString();
    }
    return dev;
}

QUrl resolveQmlView(const QString& relPath, const QString& qrcFallback) {
    const QString root = devQmlRoot();
    if (root.isEmpty()) return QUrl(qrcFallback);
    const QString fullPath = QDir(root).absoluteFilePath(relPath);
    if (!QFile::exists(fullPath)) {
        qWarning().noquote() << "DEV_QML_PATH set but" << relPath
                             << "not found at" << fullPath
                             << "- using embedded QML";
        return QUrl(qrcFallback);
    }
    qInfo().noquote() << "DEV_QML_PATH override active:" << fullPath;
    return QUrl::fromLocalFile(fullPath);
}

void applyDevQmlImportPath(QQmlEngine* engine) {
    const QString root = devQmlRoot();
    if (!root.isEmpty()) engine->addImportPath(root);
}
} // namespace

MainContainer::MainContainer(IShellHost* host, QWidget* parent)
    : QWidget(parent)
    , m_host(host)
    , m_sidebarWidget(nullptr)
    , m_contentStack(nullptr)
    , m_workspaceArea(nullptr)
    , m_contentWidget(nullptr)
    , m_overlayWidget(nullptr)
{
    // Not a degraded mode — every host operation below goes through this
    // pointer, so a null one would surface as a null dereference in a click
    // handler rather than here.
    if (!m_host) {
        qFatal("MainContainer requires an IShellHost");
    }

    // Set QML style
    QQuickStyle::setStyle("Basic");

    setupUi();

    // Provider lets the bridge scan the front-most dock's shortcuts
    // when Workspace is the current section.
    m_shortcutBridge = new ShortcutBridge(this, m_contentStack, [this]() {
        return m_workspaceArea ? m_workspaceArea->activeDockWidget()
                               : nullptr;
    });
    // Tab-switching inside the workspace changes the dock without
    // touching m_contentStack — force a rebind so the new dock's
    // shortcuts become live.
    if (m_workspaceArea) {
        connect(m_workspaceArea, &WorkspaceArea::activeAppChanged,
                m_shortcutBridge,
                [this](const QString&) { m_shortcutBridge->rebindDeferred(); });
    }

    // Subscribe to the host. These five arrive as IShellObserver virtual calls
    // rather than signals: a signal/slot connection would make both sides agree
    // on a metaobject, which is the coupling this boundary exists to avoid.
    // Detached again in detachFromHost().
    m_host->setObserver(this);

    // When user closes a plugin tab (× button), notify backend to unload.
    connect(m_workspaceArea, &WorkspaceArea::pluginClosed,
            this, [this](const QString& moduleName) {
        m_host->unloadUiModule(moduleName);
    });

    // Keep the sidebar's active-app highlight in sync with the front-most
    // dock — QDockWidget tab clicks bypass onAppLauncherClicked, so without
    // this the sidebar stays stuck on whichever app was last launched.
    connect(m_workspaceArea, &WorkspaceArea::activeAppChanged,
            this, [this](const QString& moduleName) {
        m_host->setCurrentVisibleApp(moduleName);
    });

    // WelcomePage "Install now" CTA → jump to Applications view.
    connect(m_workspaceArea, &WorkspaceArea::installClicked, this, [this]() {
        m_host->setCurrentSectionIndex(1);
    });

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
        // String-based on purpose: these resolve through the backend's
        // metaobject, so the shell never names its C++ type. The
        // QueuedConnection above is load-bearing and must stay.
        connect(sidebarRoot, SIGNAL(launchUIModule(QString)),
                m_host->backendObject(), SLOT(onAppLauncherClicked(QString)),
                Qt::QueuedConnection);
        connect(sidebarRoot, SIGNAL(updateLauncherIndex(int)),
                m_host->backendObject(), SLOT(setCurrentActiveSectionIndex(int)));
        connect(sidebarRoot, SIGNAL(tooltipRequested(QString, qreal)),
                this, SLOT(onSidebarTooltipRequested(QString, qreal)));
    }

    qDebug() << "MainContainer created";
}

MainContainer::~MainContainer()
{
    // Belt-and-braces: MainShellView::destroyShell() detaches before deleting,
    // but this covers the path where Qt's parent-child teardown destroys the
    // central widget first. Idempotent.
    detachFromHost();
    qDebug() << "MainContainer destroyed";
}

void MainContainer::detachFromHost()
{
    if (!m_host) {
        return;
    }
    m_host->setObserver(nullptr);
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

    // === SIDEBAR (QML) ===
    m_sidebarWidget = new QQuickWidget(this);
    m_sidebarWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    applyDevQmlImportPath(m_sidebarWidget->engine());
    m_sidebarWidget->rootContext()->setContextProperty("backend", m_host->backendObject());
    m_sidebarWidget->setSource(resolveQmlView(
        QStringLiteral("Basecamp/Sidebar/SidebarPanel.qml"),
        QStringLiteral("qrc:/qt/qml/Basecamp/Sidebar/Basecamp/Sidebar/SidebarPanel.qml")));
    m_sidebarWidget->setMinimumWidth(80);
    m_sidebarWidget->setMaximumWidth(80);
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
    
    // Index 0: WorkspaceArea (QDockWidget-based)
    m_workspaceArea = new WorkspaceArea(m_host->backendObject(), m_contentStack);
    m_contentStack->addWidget(m_workspaceArea);
    
    // Index 1: QML content views (Dashboard, Modules, PackageManager, Settings)
    m_contentWidget = new QQuickWidget(m_contentStack);
    m_contentWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_contentWidget->setClearColor(bgColor);
    applyDevQmlImportPath(m_contentWidget->engine());
    m_contentWidget->rootContext()->setContextProperty("backend", m_host->backendObject());
    m_contentWidget->setSource(resolveQmlView(
        QStringLiteral("Basecamp/Shell/ContentViews.qml"),
        QStringLiteral("qrc:/qt/qml/Basecamp/Shell/Basecamp/Shell/ContentViews.qml")));
    m_contentStack->addWidget(m_contentWidget);

    // Index 2: placeholder for package_manager_ui — shows a centered
    // "Loading…" label until PMUI's QQuickWidget arrives via the
    // pluginWindowRequested intercept
    m_contentStack->addWidget(makePmuiPlaceholder(m_contentStack));

    // Content stack fills the content area — the version footer that
    // used to live in a QML bottom toolbar is now inside SidebarPanel.qml.
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
    applyDevQmlImportPath(m_overlayWidget->engine());
    m_overlayWidget->rootContext()->setContextProperty("backend", m_host->backendObject());
    m_overlayWidget->setSource(resolveQmlView(
        QStringLiteral("Basecamp/Shell/OverlayDialogs.qml"),
        QStringLiteral("qrc:/qt/qml/Basecamp/Shell/Basecamp/Shell/OverlayDialogs.qml")));

    // Hook up the QML signal that tracks "any dialog visible" so we can
    // toggle mouse-passthrough on the overlay QQuickWidget.
    if (QObject* overlayRoot = m_overlayWidget->rootObject()) {
        connect(overlayRoot, SIGNAL(overlayActiveChanged(bool)),
                this, SLOT(onOverlayActiveChanged(bool)));
    }
    m_overlayWidget->setVisible(false);

    // Watch for the mouse leaving the sidebar so we can hide the tooltip
    // overlay.
    if (m_sidebarWidget) m_sidebarWidget->installEventFilter(this);

    // Set initial state — Apps section (workspace) visible by default.
    m_contentStack->setCurrentIndex(kAppsStackIndex);

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
    m_overlayWidget->setVisible(active);
    if (active) m_overlayWidget->raise();
}

void MainContainer::onSidebarTooltipRequested(const QString& text, qreal y)
{
    if (!m_overlayWidget) return;
    QObject* overlayRoot = m_overlayWidget->rootObject();
    if (!overlayRoot) return;
    overlayRoot->setProperty("sidebarTooltipText", text);
    overlayRoot->setProperty("sidebarTooltipY", y);
    if (!m_overlayWidget->isVisible()) {
        m_overlayWidget->setVisible(true);
        m_overlayWidget->raise();
    }
}

bool MainContainer::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_sidebarWidget && event->type() == QEvent::Leave) {
        if (m_overlayWidget) {
            QObject* overlayRoot = m_overlayWidget->rootObject();
            if (overlayRoot) {
                overlayRoot->setProperty("sidebarTooltipText", QString());
                const bool dialogUp = overlayRoot->property("anyDialogOpen").toBool();
                if (!dialogUp) m_overlayWidget->setVisible(false);
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MainContainer::onSectionIndexChanged(int index)
{
    const int sectionIndex = index;

    qDebug() << "MainContainer: Active section index changed to" << sectionIndex;

    //   0 (Workspace)        → WorkspaceArea (QDockWidget-based)
    //   1 (Applications)     → ContentViews.qml (App Manager view)
    //   2 (Package Manager)  → package_manager_ui (preloaded in background)
    //   3 (Settings)         → ContentViews.qml (StackLayout picks the page)
    switch (sectionIndex) {
    case 0: m_contentStack->setCurrentIndex(kAppsStackIndex);    break;
    case 1: m_contentStack->setCurrentIndex(kContentStackIndex); break;
    case 2:
        if (!m_pmuiWidget) {
            m_host->loadUiModule(QStringLiteral("package_manager_ui"));
        }
        m_contentStack->setCurrentIndex(kModulesStackIndex);
        break;
    case 3: m_contentStack->setCurrentIndex(kContentStackIndex); break;
    default: break;
    }
}

void MainContainer::onNavigateToApps()
{
    // Suppressed exactly once after package_manager_ui is folded into the
    // content stack — that path installs the pane itself and must not also
    // bounce the user to the Apps view.
    if (m_suppressNextNavToApps) {
        m_suppressNextNavToApps = false;
        return;
    }

    // This is called when an app is loaded and we need to switch to Apps view
    m_host->setCurrentSectionIndex(0);
}

void MainContainer::onPluginWindowRequested(QWidget* widget, const QString& title)
{
    // package_manager_ui is not a dock: it becomes the Package Manager page of
    // the content stack, replacing the placeholder that sits there at startup.
    if (title == QStringLiteral("package_manager_ui") && !m_pmuiWidget) {
        m_pmuiWidget = widget;
        QWidget* placeholder = m_contentStack->widget(kModulesStackIndex);
        widget->setParent(m_contentStack);
        m_contentStack->insertWidget(kModulesStackIndex, widget);
        if (placeholder) {
            m_contentStack->removeWidget(placeholder);
            placeholder->deleteLater();
        }
        // A new pane's QML just materialised — ask the bridge to pick up any
        // Shortcut { } blocks inside it. Deferred, so the QML tree has time to
        // instantiate.
        if (m_shortcutBridge) m_shortcutBridge->rebindDeferred();
        m_suppressNextNavToApps = true;
        return;
    }

    if (m_workspaceArea && widget) {
        const QString resolved = m_host->displayNameFor(title);
        const QString label = resolved.isEmpty() ? title : resolved;
        m_workspaceArea->addPluginDock(widget, title, label);
        qDebug() << "MainContainer: Added plugin dock to WorkspaceArea:"
                 << label << "(module:" << title << ")";
    }
}

void MainContainer::onPluginWindowRemoveRequested(QWidget* widget)
{
    if (widget && widget == m_pmuiWidget) {
        // package_manager_ui is the Package Manager PAGE, not a dock, so it must
        // not reach removePluginDock() -- but it cannot simply be left in the
        // stack either: the host deleteLater()s it the moment this returns.
        // Taking a page out without putting one back leaves a two-page stack
        // that every `setCurrentIndex(kModulesStackIndex)` then indexes past the
        // end of, and the section is dead for the rest of the session.
        m_contentStack->removeWidget(widget);
        m_contentStack->insertWidget(kModulesStackIndex,
                                     makePmuiPlaceholder(m_contentStack));
        m_pmuiWidget = nullptr;
        return;
    }
    if (m_workspaceArea && widget)
        m_workspaceArea->removePluginDock(widget);
}

void MainContainer::onPluginWindowActivateRequested(QWidget* widget)
{
    if (widget && widget == m_pmuiWidget) return;
    if (m_workspaceArea && widget)
        m_workspaceArea->activatePluginDock(widget);
}

