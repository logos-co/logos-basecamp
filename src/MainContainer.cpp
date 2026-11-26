#include "MainContainer.h"
#include "DefaultApps.h"
#include "ShellSections.h"
#include "ShellDevQml.h"
#include "ThemeColours.h"
#include "ShortcutBridge.h"
#include <ThemeManager.h>
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
#include <QVariantMap>
#include <QColor>
#include <QPalette>
#include <QEvent>
#include <QtMath>
#include <QTimer>

namespace {
constexpr int kAppsStackIndex     = 0;  // WorkspaceArea (QDockWidget-based)
constexpr int kContentStackIndex  = 1;  // ContentViews.qml (Settings)
// Then a page per section app (DefaultApps.h), from 2: the Package Manager's,
// then App Manager's.

// A section app's page before its widget arrives -- and again after it goes
// away. Built in two places, so it lives here: the stack keeps every page at
// all times, because every section switch below indexes into it.
QWidget* makeSectionPlaceholder(int section, QWidget* parent)
{
    QWidget* ph = new QWidget(parent);
    ph->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QVBoxLayout* phLayout = new QVBoxLayout(ph);
    phLayout->setAlignment(Qt::AlignCenter);
    QLabel* loadingLabel = new QLabel(section == ShellSection::AppManager
                                          ? QStringLiteral("Loading App Manager…")
                                          : QStringLiteral("Loading Package Manager…"),
                                      ph);
    loadingLabel->setAlignment(Qt::AlignCenter);
    ThemeColours::bind(loadingLabel, [loadingLabel] {
        loadingLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 14px;")
            .arg(ThemeColours::css("textSecondary")));
    });
    phLayout->addWidget(loadingLabel);
    return ph;
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

    // Before any QML loads, so the saved theme applies from the first frame.
    m_themeManager = new Logos::ThemeManager(m_host->themesDirectory(), this);

    setupUi();

    // Provider lets the bridge scan the front-most dock's shortcuts
    // when Workspace is the current section.
    m_shortcutBridge = new ShortcutBridge(this, m_contentStack, [this]() -> QQuickWidget* {
        if (!m_workspaceArea) return nullptr;
        if (QQuickWidget* dock = m_workspaceArea->activeDockWidget()) return dock;
        return m_workspaceArea->welcomePageWidget();
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

    // WelcomePage's two navigation blocks → the views that own those flows.
    connect(m_workspaceArea, &WorkspaceArea::discoverApplicationsClicked, this, [this]() {
        m_host->setCurrentSectionIndex(ShellSection::AppManager);
    });
    connect(m_workspaceArea, &WorkspaceArea::managePackagesClicked, this, [this]() {
        m_host->setCurrentSectionIndex(ShellSection::PackageManager);
    });
    connect(m_workspaceArea, SIGNAL(reopenAppRequested(QString)),
            m_host->backendObject(), SLOT(onAppLauncherClicked(QString)),
            Qt::QueuedConnection);

    connect(m_workspaceArea, &WorkspaceArea::appActivated, this,
            [this](const QString& name, const QString& repositoryUrl) {
        invokeOpenApp(name, repositoryUrl);
    });
    connect(m_workspaceArea, &WorkspaceArea::packageActivated, this,
            [this](const QString& name) {
        QMetaObject::invokeMethod(m_host->backendObject(),
                                  "showPackageDetails",
                                  Q_ARG(QString, name));
    });
    connect(m_workspaceArea, &WorkspaceArea::packageInstallRequested, this,
            [this](const QString& name) {
        QMetaObject::invokeMethod(m_host->backendObject(),
                                  "requestPackageInstall",
                                  Q_ARG(QString, name));
    });
    connect(m_workspaceArea, &WorkspaceArea::showAllResultsRequested, this,
            [this](const QString& typeValue, const QString& query) {
        // Apps: App Manager, asked to search for them.
        if (typeValue != QStringLiteral("core")
            && QMetaObject::invokeMethod(m_host->backendObject(), "searchApps", Q_ARG(QString, query)))
            return;
        m_host->setCurrentSectionIndex(typeValue == QStringLiteral("core")
                                           ? ShellSection::PackageManager
                                           : ShellSection::AppManager);
    });

    qDebug() << "MainContainer created";
}

// Connects to SidebarPanel's signals; runs again after each shell hot reload.
void MainContainer::wireSidebar(QObject* sidebarRoot)
{
    connect(sidebarRoot, SIGNAL(launchUIModule(QString)),
            m_host->backendObject(), SLOT(onAppLauncherClicked(QString)),
            Qt::QueuedConnection);
    connect(sidebarRoot, SIGNAL(updateLauncherIndex(int)),
            m_host->backendObject(), SLOT(setCurrentActiveSectionIndex(int)));
    connect(sidebarRoot, SIGNAL(tooltipRequested(QString, qreal)),
            this, SLOT(onSidebarTooltipRequested(QString, qreal)));
}

void MainContainer::invokeOpenApp(const QString& name,
                                  const QString& repositoryUrl)
{
    if (!QMetaObject::invokeMethod(m_host->backendObject(), "openApp",
                                   Q_ARG(QString, name),
                                   Q_ARG(QString, repositoryUrl),
                                   Q_ARG(QVariantMap, QVariantMap()),
                                   // allowFastLaunch: launch when installed,
                                   // App Manager on it if not.
                                   Q_ARG(bool, true))) {
        qCritical() << "openApp(QString,QString,QVariantMap,bool) not found on"
                    << "the backend — welcome-page app tiles will do nothing.";
    }
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
    ThemeColours::fillWithBackground(this);

    // Create main horizontal layout
    m_mainLayout = new QHBoxLayout(this);
    m_mainLayout->setSpacing(0);
    // Even margin around the sidebar so it doesn't touch the title bar.
    m_mainLayout->setContentsMargins(8, 8, 4, 8);

    // === SIDEBAR (QML) ===
    m_sidebarWidget = new QQuickWidget(this);
    m_sidebarWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_sidebarWidget->rootContext()->setContextProperty("backend", m_host->backendObject());
    ShellDevQml::load(m_sidebarWidget,
        QUrl(QStringLiteral("qrc:/qt/qml/Basecamp/Sidebar/Basecamp/Sidebar/SidebarPanel.qml")),
        [this](QObject* root) { wireSidebar(root); });
    m_sidebarWidget->setMinimumWidth(80);
    m_sidebarWidget->setMaximumWidth(80);
    // set clear color to sidebar so that rounded corners don't show white
    ThemeColours::clearToBackground(m_sidebarWidget);

    // === CONTENT AREA (vertical layout with stack + app launcher) ===
    QWidget* contentArea = new QWidget(this);
    QVBoxLayout* contentLayout = new QVBoxLayout(contentArea);
    contentLayout->setSpacing(0);
    // Top 1 + the main layout's 8 keeps the content where it was (9).
    contentLayout->setContentsMargins(4, 1, 4, 0);
    // Create content stack
    m_contentStack = new QStackedWidget(contentArea);
    m_contentStack->setObjectName(QStringLiteral("contentStack"));  // UI tests
    m_contentStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    
    // Index 0: WorkspaceArea (QDockWidget-based)
    m_workspaceArea = new WorkspaceArea(m_host->backendObject(), m_contentStack);
    m_contentStack->addWidget(m_workspaceArea);
    
    // Index 1: QML content views (Dashboard, Modules, PackageManager, Settings)
    m_contentWidget = new QQuickWidget(m_contentStack);
    m_contentWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    ThemeColours::clearToBackground(m_contentWidget);
    m_contentWidget->rootContext()->setContextProperty("backend", m_host->backendObject());
    m_contentWidget->rootContext()->setContextProperty("themeManager", m_themeManager);
    ShellDevQml::load(m_contentWidget,
        QUrl(QStringLiteral("qrc:/qt/qml/Basecamp/Shell/Basecamp/Shell/ContentViews.qml")),
        [](QObject* root) {
            Logos::ThemeManager::followViewWidth(qobject_cast<QQuickItem*>(root));
        });
    m_contentStack->addWidget(m_contentWidget);

    // From index 2: a page per section app, a centered "Loading…" label
    // until its QQuickWidget arrives via the pluginWindowRequested intercept.
    for (const int section : {int(ShellSection::PackageManager), int(ShellSection::AppManager)}) {
        const auto* app = logos::defaultapps::appInSection(section);
        if (!app) continue;
        m_sectionPages.append({section, m_contentStack->count(), app->app, {}, {}});
        m_contentStack->addWidget(makeSectionPlaceholder(section, m_contentStack));
    }

    // === OFFLINE TOAST (QML) ===
    // Over the content, outside the layout so nothing moves; only its own
    // rectangle takes input.
    m_offlineToast = new QQuickWidget(this);
    m_offlineToast->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_offlineToast->setAttribute(Qt::WA_AlwaysStackOnTop);
    m_offlineToast->setAttribute(Qt::WA_TranslucentBackground);
    m_offlineToast->setClearColor(Qt::transparent);
    m_offlineToast->setVisible(false);
    ShellDevQml::load(m_offlineToast,
        QUrl(QStringLiteral("qrc:/qt/qml/Basecamp/Shell/Basecamp/Shell/OfflineToast.qml")),
        [this](QObject* root) {
            connect(root, SIGNAL(toastRectChanged()), this, SLOT(maskOfflineToast()));
            connect(root, SIGNAL(dismissed()), this, SLOT(closeOfflineToast()));
        });
    m_offlineToastReturn = new QTimer(this);
    m_offlineToastReturn->setSingleShot(true);
    m_offlineToastReturn->setInterval(5 * 60 * 1000);
    connect(m_offlineToastReturn, &QTimer::timeout, this, [this]() {
        m_offlineToastClosed = false;
        updateOfflineToast();
    });
    // A backend without `online` (shell-preview) never shows it.
    if (m_host->backendObject()->metaObject()->indexOfSignal("onlineChanged()") >= 0) {
        connect(m_host->backendObject(), SIGNAL(onlineChanged()),
                this, SLOT(onOnlineChanged()));
    }
    // It follows the content area, which the layout sizes after the window.
    m_contentStack->installEventFilter(this);
    updateOfflineToast();

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
    m_overlayWidget->rootContext()->setContextProperty("backend", m_host->backendObject());
    ShellDevQml::load(m_overlayWidget,
        QUrl(QStringLiteral("qrc:/qt/qml/Basecamp/Shell/Basecamp/Shell/OverlayDialogs.qml")),
        [this](QObject* root) {
            Logos::ThemeManager::followViewWidth(qobject_cast<QQuickItem*>(root));
            connect(root, SIGNAL(overlayActiveChanged(bool)),
                    this, SLOT(onOverlayActiveChanged(bool)));
        });
    m_overlayWidget->setVisible(false);

    // === FIRST-RUN ONBOARDING (QML) ===
    // Until the user has chosen how apps download, the backend holds back the
    // downloader and with it the storage node. A backend without the property
    // (shell-preview) never shows it.
    if (m_host->backendObject()->property("onboardingRequired").toBool()) {
        m_onboardingWidget = new QQuickWidget(this);
        m_onboardingWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
        ThemeColours::clearToBackground(m_onboardingWidget);
        m_onboardingWidget->rootContext()->setContextProperty("backend", m_host->backendObject());
        ShellDevQml::load(m_onboardingWidget,
            QUrl(QStringLiteral("qrc:/qt/qml/Basecamp/Shell/Basecamp/Shell/OnboardingShell.qml")),
            [this](QObject* root) {
                Logos::ThemeManager::followViewWidth(qobject_cast<QQuickItem*>(root));
                connect(root, SIGNAL(finished()), this, SLOT(onOnboardingFinished()));
            });
        m_onboardingWidget->raise();
    }

    // Watch for the mouse leaving the sidebar so we can hide the tooltip
    // overlay.
    if (m_sidebarWidget) m_sidebarWidget->installEventFilter(this);

    // Set initial state — Apps section (workspace) visible by default.
    m_contentStack->setCurrentIndex(kAppsStackIndex);

    // As narrow as a phone, as Status's desktop window: the views follow
    // their width, down to the phone layouts of the design system.
    setMinimumSize(360, 480);
}

void MainContainer::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_onboardingWidget) {
        m_onboardingWidget->setGeometry(0, 0, width(), height());
        m_onboardingWidget->raise();
    }
    if (m_offlineToast && !m_offlineToast->isHidden()) placeOfflineToast();
    if (m_overlayWidget) {
        m_overlayWidget->setGeometry(0, 0, width(), height());
        // Qt re-stacks siblings on resize in some cases; keep the
        // overlay on top explicitly.
        m_overlayWidget->raise();
    }
}

bool MainContainer::isOffline() const
{
    const QVariant online = m_host->backendObject()->property("online");
    return online.isValid() && !online.toBool();
}

void MainContainer::onOnlineChanged()
{
    // Each time the connection goes, the toast comes back.
    if (!isOffline()) {
        m_offlineToastClosed = false;
        m_offlineToastReturn->stop();
    }
    updateOfflineToast();
}

void MainContainer::closeOfflineToast()
{
    m_offlineToastClosed = true;
    m_offlineToastReturn->start();
    updateOfflineToast();
}

void MainContainer::updateOfflineToast()
{
    if (!m_offlineToast) return;
    const bool show = isOffline() && !m_offlineToastClosed;
    m_offlineToast->setVisible(show);
    // Once shown: while hidden, the widget does not pass a new size on.
    if (show) placeOfflineToast();
}

// Over the content area, under dialogs and onboarding.
void MainContainer::placeOfflineToast()
{
    if (!m_offlineToast || !m_contentStack) return;
    m_offlineToast->setGeometry(QRect(m_contentStack->mapTo(this, QPoint(0, 0)),
                                      m_contentStack->size()));
    maskOfflineToast();
    m_offlineToast->raise();
    if (m_overlayWidget && m_overlayWidget->isVisible()) m_overlayWidget->raise();
    if (m_onboardingWidget) m_onboardingWidget->raise();
}

// Only the notice draws and takes clicks; the content around it stays usable.
void MainContainer::maskOfflineToast()
{
    if (!m_offlineToast || !m_offlineToast->rootObject()) return;
    const QRectF r = m_offlineToast->rootObject()->property("toastRect").toRectF();
    if (r.isEmpty()) return;
    m_offlineToast->setMask(QRegion(r.toAlignedRect()));
}

void MainContainer::onOnboardingFinished()
{
    if (!m_onboardingWidget) return;
    m_onboardingWidget->hide();
    m_onboardingWidget->deleteLater();
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
    if (watched == m_contentStack && event->type() == QEvent::Resize
        && m_offlineToast && !m_offlineToast->isHidden()) {
        placeOfflineToast();
    }
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

// A backend without availableSections (shell-preview) offers every section.
bool MainContainer::offersSection(int section) const
{
    if (QObject* backend = m_host->backendObject()) {
        const QVariant available = backend->property("availableSections");
        if (available.isValid())
            return available.toList().contains(QVariant(section));
    }
    return true;
}

void MainContainer::onSectionIndexChanged(int index)
{
    if (!offersSection(index)) {
        m_host->setCurrentSectionIndex(ShellSection::Workspace);
        return;
    }

    const int sectionIndex = index;

    qDebug() << "MainContainer: Active section index changed to" << sectionIndex;

    //   0 (Workspace)        → WorkspaceArea (QDockWidget-based)
    //   1 (Applications)     → app_manager_ui, loaded on first visit
    //   2 (Package Manager)  → package_manager_ui, loaded on first visit
    //   3 (Settings)         → ContentViews.qml
    switch (sectionIndex) {
    case ShellSection::Workspace:
        m_contentStack->setCurrentIndex(kAppsStackIndex);
        break;
    case ShellSection::Settings:
        m_contentStack->setCurrentIndex(kContentStackIndex);
        break;
    default:
        if (SectionPage* page = pageForSection(sectionIndex)) {
            if (!page->widget) m_host->loadUiModule(page->app);
            m_contentStack->setCurrentIndex(page->stackIndex);
        }
        break;
    }
}

MainContainer::SectionPage* MainContainer::pageForSection(int section)
{
    for (SectionPage& page : m_sectionPages) {
        if (page.section == section) return &page;
    }
    return nullptr;
}

MainContainer::SectionPage* MainContainer::pageForApp(const QString& app)
{
    for (SectionPage& page : m_sectionPages) {
        if (page.app == app) return &page;
    }
    return nullptr;
}

MainContainer::SectionPage* MainContainer::pageForWidget(const QWidget* widget)
{
    for (SectionPage& page : m_sectionPages) {
        if (widget && page.widget == widget) return &page;
    }
    return nullptr;
}

void MainContainer::onNavigateToApps()
{
    // Suppressed exactly once after a section app is folded into the content
    // stack — that path installs the pane itself and must not also bounce the
    // user to the Apps view.
    if (m_suppressNextNavToApps) {
        m_suppressNextNavToApps = false;
        return;
    }

    // This is called when an app is loaded and we need to switch to Apps view
    m_host->setCurrentSectionIndex(ShellSection::Workspace);
}

void MainContainer::onPluginWindowRequested(QWidget* widget, const QString& title)
{
    // A section app is not a dock: it becomes its section's page of the
    // content stack, replacing the placeholder that sits there at startup.
    // Only where that section exists — a host without it (standalone mode)
    // docks it like any other app.
    SectionPage* page = pageForApp(title);
    if (page && !page->widget && offersSection(page->section)) {
        page->widget = widget;
        QWidget* placeholder = m_contentStack->widget(page->stackIndex);
        const bool showing = placeholder && m_contentStack->currentWidget() == placeholder;
        widget->setParent(m_contentStack);
        m_contentStack->insertWidget(page->stackIndex, widget);
        if (showing) m_contentStack->setCurrentWidget(widget);
        if (placeholder) {
            m_contentStack->removeWidget(placeholder);
            placeholder->deleteLater();
        }
        if (m_workspaceArea) m_workspaceArea->followAppViewWidth(widget);
        // A new pane's QML just materialised — ask the bridge to pick up any
        // Shortcut { } blocks inside it. Deferred, so the QML tree has time to
        // instantiate.
        if (m_shortcutBridge) m_shortcutBridge->rebindDeferred();
        m_suppressNextNavToApps = true;
        // Standalone mode opening it as the app under development: show it.
        if (QObject* backend = m_host->backendObject();
            backend && backend->property("currentVisibleApp").toString() == title)
            m_host->setCurrentSectionIndex(page->section);
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
    if (SectionPage* page = pageForWidget(widget)) {
        // Removing the page on screen lets the stack show whichever is next;
        // keep the section's page on screen across a reload (Reopen).
        QWidget* current = m_contentStack->currentWidget();
        const bool showing = current == widget
            || (page->stoppedPanel && current == page->stoppedPanel);
        if (page->stoppedPanel) {
            m_contentStack->removeWidget(page->stoppedPanel);
            page->stoppedPanel->deleteLater();
            page->stoppedPanel = nullptr;
        }
        // A section app is its section's PAGE, not a dock, so it must not
        // reach removePluginDock() -- but it cannot simply be left in the stack
        // either: the host deleteLater()s it the moment this returns. Taking a
        // page out without putting one back shifts every page after it, and
        // `setCurrentIndex(page->stackIndex)` then shows the wrong one or
        // indexes past the end: the section is dead for the rest of the session.
        m_contentStack->removeWidget(widget);
        QWidget* placeholder = makeSectionPlaceholder(page->section, m_contentStack);
        m_contentStack->insertWidget(page->stackIndex, placeholder);
        if (showing) m_contentStack->setCurrentWidget(placeholder);
        page->widget = nullptr;
        return;
    }
    if (m_workspaceArea && widget)
        m_workspaceArea->removePluginDock(widget);
}

QQuickWidget* MainContainer::makeAppStoppedPanel(const QString& name)
{
    auto* panel = new QQuickWidget;
    panel->setResizeMode(QQuickWidget::SizeRootObjectToView);
    ThemeColours::clearToBackground(panel);
    panel->setSource(QUrl(QStringLiteral(
        "qrc:/qt/qml/Basecamp/Shell/Basecamp/Shell/AppStoppedPanel.qml")));
    QObject* root = panel->rootObject();
    if (!root) {
        qWarning() << "MainContainer: AppStoppedPanel.qml failed to load:" << panel->errors();
        return panel;
    }
    const QString label = m_host->displayNameFor(name);
    root->setProperty("appId", name);
    root->setProperty("appName", label.isEmpty() ? name : label);
    // A section's page cannot be closed, only reloaded.
    root->setProperty("closable", !pageForApp(name));
    connect(root, SIGNAL(reopenRequested(QString)), this, SLOT(onStoppedAppReopenRequested(QString)));
    connect(root, SIGNAL(closeRequested(QString)), this, SLOT(onStoppedAppCloseRequested(QString)));
    return panel;
}

void MainContainer::onAppStopped(QWidget* widget, const QString& name)
{
    if (!widget) {
        qWarning() << "MainContainer: no widget for stopped app" << name;
        return;
    }
    QQuickWidget* panel = makeAppStoppedPanel(name);

    if (SectionPage* page = pageForWidget(widget)) {
        const int index = m_contentStack->indexOf(widget);
        if (index < 0 || page->stoppedPanel) {
            panel->deleteLater();
            return;
        }
        const bool showing = m_contentStack->currentWidget() == widget;
        panel->setParent(m_contentStack);
        m_contentStack->insertWidget(index, panel);
        m_contentStack->removeWidget(widget);
        if (showing) m_contentStack->setCurrentWidget(panel);
        page->stoppedPanel = panel;
        return;
    }

    if (!m_workspaceArea || !m_workspaceArea->showStoppedPanel(widget, panel)) {
        qWarning() << "MainContainer: no tab holds" << name << "- stopped panel not shown";
        panel->deleteLater();
        return;
    }
    qInfo() << "MainContainer: showing the stopped panel for" << name;
}

void MainContainer::onStoppedAppReopenRequested(const QString& name)
{
    m_host->restartUiModule(name);
}

void MainContainer::onStoppedAppCloseRequested(const QString& name)
{
    m_host->unloadUiModule(name);
}

void MainContainer::onPresentAppRequested(QWidget* widget)
{
    if (!widget) return;

    // Hoisted into the content stack, not docked, so "to the front" means
    // selecting its section rather than raising a tab.
    if (SectionPage* page = pageForWidget(widget)) {
        m_host->setCurrentSectionIndex(page->section);
        return;
    }

    if (!m_workspaceArea) return;

    // Switch section before raising the tab, or the tab becomes current behind
    // whatever the user is actually looking at.
    onNavigateToApps();
    m_workspaceArea->activatePluginDock(widget);
}


