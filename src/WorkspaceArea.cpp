#include "WorkspaceArea.h"

#include <QApplication>
#include <QBoxLayout>
#include <QDebug>
#include <QDir>
#include <QDockWidget>
#include <QEasingCurve>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHideEvent>
#include <QIcon>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPixmap>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QResizeEvent>
#include <QScroller>
#include <QScrollerProperties>
#include <QShortcut>
#include <QShowEvent>
#include <QPainter>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QWheelEvent>

namespace {
class ZeroTitleWidget : public QWidget {
public:
    explicit ZeroTitleWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedHeight(0);
    }
    QSize sizeHint() const override { return {0, 0}; }
    QSize minimumSizeHint() const override { return {0, 0}; }
};

// Overlay that "punches" antialiased rounded corners into the dock
// content. Sits above the plugin widget. In the 4 corner regions it
// paints the shell color, hiding the plugin's square corners; the
// interior is left transparent so the plugin shows through unmodified.
// This is why the rounded edges look smooth — QPainter antialiasing on
// the QPainterPath — instead of the staircase you get with setMask
// (which is 1-bit region clipping, no antialiasing).
//
// Transparent-for-mouse so clicks pass through to the plugin.
class CornerCutOverlay : public QWidget {
public:
    explicit CornerCutOverlay(QWidget* parent = nullptr,
                              QColor cutColor = QColor("#171717"),
                              qreal radius = 16.0)
        : QWidget(parent), m_cutColor(cutColor), m_radius(radius)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
        setAutoFillBackground(false);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Full rect minus rounded interior = 4 corner slivers.
        QPainterPath outer;
        outer.addRect(rect());
        QPainterPath inner;
        inner.addRoundedRect(rect(), m_radius, m_radius);

        p.fillPath(outer.subtracted(inner), m_cutColor);
    }

private:
    QColor m_cutColor;
    qreal  m_radius;
};

// Transparent wrapper around the plugin widget inside a dock. Gives us:
//   * 4px vertical gap between the tab bar and the plugin content —
//     via the layout's top contentsMargin.
//   * Antialiased 16px rounded corners — via CornerCutOverlay (see above).
//     Not using setMask because it's binary/staircase; not using
//     stylesheet border-radius because it doesn't clip children.
class DockCard : public QWidget {
public:
    explicit DockCard(QWidget* pluginWidget, QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("dockCard"));
        setAutoFillBackground(false);
        setAttribute(Qt::WA_TranslucentBackground);

        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 4, 0, 0);   // 4px gap under the tab strip
        lay->setSpacing(0);
        if (pluginWidget) lay->addWidget(pluginWidget);

        m_cornerOverlay = new CornerCutOverlay(this);
        m_cornerOverlay->raise();
    }

    QWidget* pluginWidget() const {
        if (!layout() || layout()->count() == 0) return nullptr;
        return layout()->itemAt(0)->widget();
    }

    // Detach the plugin widget so the caller (UIPluginManager) retains
    // ownership — mirrors the ownership contract WorkspaceArea gives
    // its consumers.
    QWidget* releasePluginWidget() {
        if (!layout() || layout()->count() == 0) return nullptr;
        QLayoutItem* item = layout()->takeAt(0);
        QWidget* w = item ? item->widget() : nullptr;
        delete item;
        if (w) w->setParent(nullptr);
        return w;
    }

protected:
    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e);
        if (m_cornerOverlay) {
            // Cover only the plugin area (below the 4px top gap). If
            // the overlay covered the full rect, its top corner slivers
            // would extend into the 4px gap AND clip the plugin's real
            // top edge with shell-colored fills.
            m_cornerOverlay->setGeometry(0, kTopGap, width(), height() - kTopGap);
            m_cornerOverlay->raise();   // stay on top of plugin
        }
    }

    static constexpr int kTopGap = 4;

private:
    CornerCutOverlay* m_cornerOverlay = nullptr;
};

// DEV_QML_PATH helpers — mirror the ones in MainContainer.cpp so
// WelcomePage.qml participates in the same live-edit flow. If we grow
// more shell-level QQuickWidget hosts, extract to a shared header.
QString devQmlRoot() {
    const QString dev = QString::fromUtf8(qgetenv("DEV_QML_PATH")).trimmed();
    if (dev.isEmpty()) return QString();
    if (!QFileInfo(dev).isDir()) return QString();
    return dev;
}
QUrl resolveQmlView(const QString& relPath, const QString& qrcFallback) {
    const QString root = devQmlRoot();
    if (root.isEmpty()) return QUrl(qrcFallback);
    const QString fullPath = QDir(root).absoluteFilePath(relPath);
    if (!QFile::exists(fullPath)) return QUrl(qrcFallback);
    return QUrl::fromLocalFile(fullPath);
}
void applyDevQmlImportPath(QQmlEngine* engine) {
    const QString root = devQmlRoot();
    if (!root.isEmpty()) engine->addImportPath(root);
}

constexpr int kTabBarInsetPx = 24;
}  // namespace

WorkspaceArea::WorkspaceArea(QObject* backend, QWidget* parent)
    : QMainWindow(parent)
{
    setWindowFlags(Qt::Widget);

    setDockOptions(QMainWindow::AllowNestedDocks
                   | QMainWindow::AllowTabbedDocks
                   | QMainWindow::AnimatedDocks
                   | QMainWindow::GroupedDragging);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);

    setAutoFillBackground(true);
    {
        QPalette pal = palette();
        pal.setColor(QPalette::Window, QColor("#171717"));
        setPalette(pal);
    }
    setContentsMargins(0, 0, 0, 0);

    if (backend) {
        m_welcomeWidget = new QQuickWidget(this);
        m_welcomeWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
        m_welcomeWidget->setClearColor(QColor("#171717"));
        m_welcomeWidget->setMinimumSize(300, 200);
        applyDevQmlImportPath(m_welcomeWidget->engine());
        m_welcomeWidget->rootContext()->setContextProperty("backend", backend);
        m_welcomeWidget->setSource(resolveQmlView(
            QStringLiteral("Basecamp/Shell/WelcomePage.qml"),
            QStringLiteral("qrc:/qt/qml/Basecamp/Shell/Basecamp/Shell/WelcomePage.qml")));

        if (m_welcomeWidget->status() == QQuickWidget::Error) {
            qWarning() << "WorkspaceArea: WelcomePage.qml failed to load:"
                       << m_welcomeWidget->errors();
        }

        setCentralWidget(m_welcomeWidget);

        // Forward the QML installClicked signal up to consumers.
        if (QObject* rootObj = m_welcomeWidget->rootObject()) {
            connect(rootObj, SIGNAL(installClicked()),
                    this,    SIGNAL(installClicked()));
        } else {
            qWarning() << "WorkspaceArea: WelcomePage.qml loaded but "
                          "rootObject is null — installClicked not wired.";
        }
    } else {
        auto* placeholder = new QWidget(this);
        placeholder->setMaximumSize(0, 0);
        setCentralWidget(placeholder);
    }
    installEventFilter(this);

    // DEV: Ctrl+Shift+L flips between tabbed and side-by-side. Hidden
    // toggle, no UI surface — wire a button to toggleLayoutModeForTesting()
    // once we're happy with the behavior.
    auto* sc = new QShortcut(QKeySequence("Ctrl+Shift+L"), this);
    connect(sc, &QShortcut::activated, this,
            &WorkspaceArea::toggleLayoutModeForTesting);
}

void WorkspaceArea::toggleLayoutModeForTesting()
{
    QTimer::singleShot(0, this, [this]() {
        if (m_dockOrder.size() < 2) return;
        m_sideBySide = !m_sideBySide;

        QMainWindow::DockOptions opts = QMainWindow::AllowNestedDocks
                                      | QMainWindow::AnimatedDocks
                                      | QMainWindow::GroupedDragging;
        if (!m_sideBySide) opts |= QMainWindow::AllowTabbedDocks;
        setDockOptions(opts);

        for (const QString& n : m_dockOrder)
            if (auto* d = m_docks.value(n)) removeDockWidget(d);

        QDockWidget* prev = nullptr;
        for (int i = 0; i < m_dockOrder.size(); ++i) {
            auto* d = m_docks.value(m_dockOrder.at(i));
            if (!d) continue;

            // Swap the title bar to match the new mode: Qt's default in
            // grid (built-in drag + close ×), ZeroTitleWidget in tabbed.
            if (auto* prevBar = d->titleBarWidget()) prevBar->deleteLater();
            d->setTitleBarWidget(m_sideBySide
                ? nullptr
                : static_cast<QWidget*>(new ZeroTitleWidget(d)));

            if (!prev) {
                addDockWidget(Qt::LeftDockWidgetArea, d);
            } else if (m_sideBySide) {
                placeDockInGrid(d, i);
            } else {
                tabifyDockWidget(prev, d);
            }
            d->setVisible(true);  // removeDockWidget hides as a side effect
            if (auto* w = d->widget()) w->show();
            prev = d;
        }
        m_firstDock = m_docks.value(m_dockOrder.first());

        if (m_sideBySide) {
            // Equalize row widths + row heights so each cell is roughly the
            // same size. Rows aren't uniform on remove (Qt collapses splits),
            // but on toggle we get a clean grid.
            const int rows = (m_dockOrder.size() + kColsPerRow - 1) / kColsPerRow;
            QList<QDockWidget*> firstOfEachRow;
            for (int r = 0; r < rows; ++r) {
                if (auto* d = m_docks.value(m_dockOrder.at(r * kColsPerRow)))
                    firstOfEachRow.append(d);
            }
            if (rows > 1) {
                const int rowH = qMax(1, height() / rows);
                QList<int> rowHeights(firstOfEachRow.size(), rowH);
                resizeDocks(firstOfEachRow, rowHeights, Qt::Vertical);
            }
            // Equalize columns within each row.
            for (int r = 0; r < rows; ++r) {
                QList<QDockWidget*> row;
                const int start = r * kColsPerRow;
                const int end   = qMin(start + kColsPerRow, int(m_dockOrder.size()));
                for (int i = start; i < end; ++i)
                    if (auto* d = m_docks.value(m_dockOrder.at(i)))
                        row.append(d);
                if (row.size() > 1) {
                    const int colW = qMax(1, width() / row.size());
                    QList<int> colWidths(row.size(), colW);
                    resizeDocks(row, colWidths, Qt::Horizontal);
                }
            }
        }

        QTimer::singleShot(0, this, [this]() { styleAllTabBars(); });
    });
}

WorkspaceArea::~WorkspaceArea() = default;

void WorkspaceArea::placeDockInGrid(QDockWidget* dock, int gridIndex)
{
    // Precondition: caller guarantees gridIndex >= 1 (index 0 is the
    // anchor and goes through addDockWidget). gridIndex is the target
    // position — i.e., where `dock` will land — not the current
    // m_dockOrder size at the callsite. When toggling from tabbed to
    // grid we iterate all existing docks and pass their new index.
    const int col = gridIndex % kColsPerRow;
    const int row = gridIndex / kColsPerRow;

    if (col == 0) {
        // Start of a new row — split vertically off the first dock
        // of the previous row so this row anchors below it.
        const int prevRowFirst = (row - 1) * kColsPerRow;
        if (auto* anchor = m_docks.value(m_dockOrder.value(prevRowFirst))) {
            splitDockWidget(anchor, dock, Qt::Vertical);
        } else {
            // Fallback: no previous row (shouldn't happen when
            // gridIndex >= kColsPerRow) — just addDockWidget so we
            // don't leave the dock unparented.
            addDockWidget(Qt::LeftDockWidgetArea, dock);
        }
    } else {
        // Continue the current row — horizontal split against the
        // previous dock in this row (its m_dockOrder index is
        // gridIndex - 1).
        if (auto* leftNeighbor = m_docks.value(m_dockOrder.value(gridIndex - 1))) {
            splitDockWidget(leftNeighbor, dock, Qt::Horizontal);
        } else {
            addDockWidget(Qt::LeftDockWidgetArea, dock);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void WorkspaceArea::ensurePhantomTab()
{
    if (m_sideBySide) return;      // no tab bar in grid mode
    if (m_phantomDock) return;     // already present
    if (!m_firstDock) return;      // no real dock to tabify with
    // Only meaningful when there's exactly one real dock — otherwise
    // Qt's real second tab already gives us a tab bar.
    if (m_dockOrder.size() != 1) return;

    m_phantomDock = new QDockWidget(this);
    m_phantomDock->setObjectName(QStringLiteral("__phantom_tab_placeholder__"));
    m_phantomDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    m_phantomDock->setTitleBarWidget(new ZeroTitleWidget(m_phantomDock));
    auto* placeholder = new QWidget;
    placeholder->setMaximumSize(0, 0);
    m_phantomDock->setWidget(placeholder);

    tabifyDockWidget(m_firstDock, m_phantomDock);
    m_firstDock->raise();  // keep the real dock's content shown
    m_firstDock->setFeatures(QDockWidget::DockWidgetClosable);
    setDockOptions(dockOptions() & ~QMainWindow::GroupedDragging);

    QTimer::singleShot(0, this, [this]() {
        if (!m_phantomDock) return;
        for (QTabBar* bar : findChildren<QTabBar*>()) {
            for (int i = 0; i < bar->count(); ++i) {
                if (bar->tabText(i).isEmpty()) {
                    bar->setTabVisible(i, false);
                    bar->setTabEnabled(i, false);   // block drag/click
                    return;
                }
            }
        }
    });
}

void WorkspaceArea::removePhantom()
{
    if (!m_phantomDock) return;
    QDockWidget* p = m_phantomDock;
    m_phantomDock.clear();   // null before destroying, so any concurrent
                             // deferred callback sees "no phantom".
    removeDockWidget(p);
    delete p;

    if (m_firstDock) {
        m_firstDock->setFeatures(QDockWidget::DockWidgetMovable
                                 | QDockWidget::DockWidgetClosable);
    }
    setDockOptions(dockOptions() | QMainWindow::GroupedDragging);
}

void WorkspaceArea::addPluginDock(QWidget* pluginWidget,
                                  const QString& moduleName,
                                  const QString& displayLabel)
{
    if (!pluginWidget || moduleName.isEmpty()) return;
    if (m_docks.contains(moduleName)) {
        activatePluginDock(moduleName);
        return;
    }

    const QString title = displayLabel.isEmpty() ? moduleName : displayLabel;
    auto* dock = new QDockWidget(title, this);
    dock->setObjectName(moduleName);
    dock->setWidget(new DockCard(pluginWidget, dock));
    dock->setAllowedAreas(Qt::AllDockWidgetAreas);
    dock->setFeatures(QDockWidget::DockWidgetMovable
                      | QDockWidget::DockWidgetClosable);
    if (m_sideBySide) {
        dock->setTitleBarWidget(nullptr);
    } else {
        dock->setTitleBarWidget(new ZeroTitleWidget(dock));
    }
    dock->installEventFilter(this);
    pluginWidget->installEventFilter(this);

    const QIcon icon = pluginWidget->windowIcon();
    if (!icon.isNull()) dock->setWindowIcon(icon);

    if (!m_firstDock) {
        addDockWidget(Qt::LeftDockWidgetArea, dock);
        m_firstDock = dock;
    } else if (m_sideBySide) {
        placeDockInGrid(dock, m_dockOrder.size());
    } else {
        removePhantom();
        tabifyDockWidget(m_firstDock, dock);
        dock->raise();
    }

    m_docks[moduleName] = dock;
    m_dockOrder.append(moduleName);

    QTimer::singleShot(0, this, [this, moduleName]() {
        styleAllTabBars();
        activatePluginDock(moduleName);
    });
    ensurePhantomTab();
    updateQmlPluginActiveStates();
    updateWelcomeVisibility();
}

void WorkspaceArea::removePluginDock(const QString& name)
{
    auto it = m_docks.find(name);
    if (it == m_docks.end()) return;

    QDockWidget* dock = it.value();
    m_docks.erase(it);
    m_dockOrder.removeAll(name);

    removeDockWidget(dock);
    dock->deleteLater();

    if (m_firstDock == dock) {
        m_firstDock = m_dockOrder.isEmpty() ? nullptr
                                            : m_docks.value(m_dockOrder.first());
    }

    QTimer::singleShot(0, this, [this]() { styleAllTabBars(); });
    if (m_dockOrder.isEmpty()) removePhantom();
    else ensurePhantomTab();
    updateQmlPluginActiveStates();
    updateWelcomeVisibility();
}

void WorkspaceArea::activatePluginDock(const QString& moduleName)
{
    QDockWidget* dock = m_docks.value(moduleName);
    if (!dock) return;

    dock->show();
    dock->raise();

    const QString tabText = dock->windowTitle();
    for (QTabBar* bar : findChildren<QTabBar*>()) {
        for (int i = 0; i < bar->count(); ++i) {
            if (bar->tabText(i) == tabText) {
                bar->setCurrentIndex(i);
                break;
            }
        }
    }
    updateQmlPluginActiveStates();
}

QString WorkspaceArea::moduleNameForTabText(const QString& tabText) const
{
    if (tabText.isEmpty()) return {};
    for (auto it = m_docks.cbegin(); it != m_docks.cend(); ++it) {
        if (it.value() && it.value()->windowTitle() == tabText) return it.key();
    }
    return {};
}

QString WorkspaceArea::nameForWidget(QWidget* w) const
{
    if (!w) return {};
    for (auto it = m_docks.cbegin(); it != m_docks.cend(); ++it) {
        QWidget* dockChild = it.value()->widget();
        if (dockChild == w) return it.key();
        if (auto* card = dynamic_cast<DockCard*>(dockChild)) {
            if (card->pluginWidget() == w) return it.key();
        }
    }
    return {};
}

void WorkspaceArea::removePluginDock(QWidget* pluginWidget)
{
    const QString name = nameForWidget(pluginWidget);
    if (!name.isEmpty()) removePluginDock(name);
}

void WorkspaceArea::activatePluginDock(QWidget* pluginWidget)
{
    const QString name = nameForWidget(pluginWidget);
    if (!name.isEmpty()) activatePluginDock(name);
}

// ---------------------------------------------------------------------------
// Tab-bar styling (ported from MdiView::customizeTabBarStyle +
// insetTabBarGeometry).
// ---------------------------------------------------------------------------

void WorkspaceArea::customizeTabBarStyle(QTabBar* tabBar)
{
    if (!tabBar) return;

    tabBar->setDocumentMode(true);
    tabBar->setDrawBase(false);
    tabBar->setAutoHide(false);
    tabBar->setAutoFillBackground(false);
    tabBar->setElideMode(Qt::ElideRight);
    tabBar->setUsesScrollButtons(false);
    tabBar->setExpanding(false);
    tabBar->setIconSize(QSize(15, 15));
    tabBar->setMovable(true);
    QScroller::grabGesture(tabBar, QScroller::TouchGesture);
    QScrollerProperties props = QScroller::scroller(tabBar)->scrollerProperties();
    props.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
    props.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
    props.setScrollMetric(QScrollerProperties::ScrollingCurve, QEasingCurve::OutCubic);
    QScroller::scroller(tabBar)->setScrollerProperties(props);

    tabBar->setStyleSheet(QStringLiteral(R"(
        QTabBar {
            background: #171717;
            border: none;
            qproperty-drawBase: false;
        }

        QTabBar::tab {
            background: #262626;
            color: #A4A4A4;

            padding: 0px 8px 0px 4px;
            margin-right: 10px;

            border-top-left-radius: 10px;
            border-top-right-radius: 10px;
            height: 20px;
            min-width: 120px;
        }

        QTabBar::tab:!selected {
            background: rgba(38, 38, 38, 0.6);
            color: #626262;
        }

        QTabBar::tab:hover {
            background: #262626;
        }
    )"));

    connect(tabBar, &QTabBar::tabCloseRequested, this,
            [this, tabBar](int index) {
                const QString moduleName = moduleNameForTabText(tabBar->tabText(index));
                if (moduleName.isEmpty()) return;
                emit pluginClosed(moduleName);
            });

    // Track tab-current changes so we can update plugin isActiveTab and
    // sync the sidebar's active-app highlight (backend.currentVisibleApp).
    connect(tabBar, &QTabBar::currentChanged, this,
            [this, tabBar](int index) {
                updateQmlPluginActiveStates();
                const QString name = index >= 0
                    ? moduleNameForTabText(tabBar->tabText(index))
                    : QString();
                emit activeAppChanged(name);
            });
}

void WorkspaceArea::installTabBarCloseButtons(QTabBar* tabBar)
{
    if (!tabBar) return;
    const QTabBar::ButtonPosition closeSide = QTabBar::LeftSide;
    for (int i = 0; i < tabBar->count(); ++i) {
        if (tabBar->tabText(i).isEmpty()) continue;
        QWidget* oldBtn = tabBar->tabButton(i, closeSide);
        if (oldBtn) {
            tabBar->setTabButton(i, closeSide, nullptr);
            oldBtn->deleteLater();
        }
        auto* btn = new QToolButton(tabBar);
        btn->setIcon(qApp->style()->standardIcon(QStyle::SP_TitleBarCloseButton));
        btn->setIconSize(QSize(12, 12));
        btn->setFixedSize(12, 12);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(QStringLiteral(R"(
            QToolButton { background: transparent; border: none; }
            QToolButton:hover { background: rgba(255,255,255,0.1); border-radius: 6px; }
        )"));
        connect(btn, &QToolButton::clicked, this, [tabBar, btn, closeSide]() {
            for (int j = 0; j < tabBar->count(); ++j) {
                if (tabBar->tabButton(j, closeSide) == btn) {
                    emit tabBar->tabCloseRequested(j);
                    break;
                }
            }
        });
        btn->setVisible(false);  // hover-show only
        btn->installEventFilter(this);
        tabBar->setTabButton(i, closeSide, btn);
    }
    tabBar->setMouseTracking(true);
}

void WorkspaceArea::insetTabBarGeometry(QTabBar* tabBar, int insetPx)
{
    if (!tabBar) return;
    QWidget* p = tabBar->parentWidget();
    if (!p) return;

    QRect g = tabBar->geometry();
    tabBar->setGeometry(insetPx, g.y(),
                        p->width() - insetPx - kTabBarInsetPx, g.height());
}

void WorkspaceArea::styleAllTabBars()
{
    static const char* kStyledMarker = "logosStyled";
    for (auto* tabBar : findChildren<QTabBar*>()) {
        if (!tabBar->property(kStyledMarker).toBool()) {
            tabBar->setProperty(kStyledMarker, true);
            customizeTabBarStyle(tabBar);
            tabBar->installEventFilter(this);
        }
        // Re-run close-button install in case Qt grew the tab count.
        installTabBarCloseButtons(tabBar);

        for (int i = 0; i < tabBar->count(); ++i) {
            const QString moduleName = moduleNameForTabText(tabBar->tabText(i));
            if (auto* dock = m_docks.value(moduleName)) {
                const QIcon icon = dock->windowIcon();
                if (!icon.isNull()) tabBar->setTabIcon(i, icon);
            }
        }

        if (tabBar->x() != kTabBarInsetPx)
            insetTabBarGeometry(tabBar, kTabBarInsetPx);
    }
}

// ---------------------------------------------------------------------------
// Plugin active-state propagation (port of MdiView::updateQmlPluginActiveStates)
// ---------------------------------------------------------------------------

void WorkspaceArea::updateWelcomeVisibility()
{
    if (!m_welcomeWidget) return;
    m_welcomeWidget->setVisible(m_docks.isEmpty());
}

QQuickWidget* WorkspaceArea::activeDockWidget() const
{
    for (auto it = m_docks.cbegin(); it != m_docks.cend(); ++it) {
        QDockWidget* dock = it.value();
        if (!dock || dock->visibleRegion().isEmpty()) continue;
        QWidget* dockChild = dock->widget();
        if (auto* card = dynamic_cast<DockCard*>(dockChild))
            dockChild = card->pluginWidget();
        if (auto* qw = qobject_cast<QQuickWidget*>(dockChild))
            return qw;
    }
    return nullptr;
}

void WorkspaceArea::updateQmlPluginActiveStates()
{
    const bool workspaceVisible = isVisible();
    for (auto it = m_docks.cbegin(); it != m_docks.cend(); ++it) {
        QDockWidget* dock = it.value();
        QWidget* dockChild = dock->widget();
        // Plugin widgets are wrapped in DockCard; unwrap before casting.
        if (auto* card = dynamic_cast<DockCard*>(dockChild))
            dockChild = card->pluginWidget();
        auto* qmlWidget = qobject_cast<QQuickWidget*>(dockChild);
        if (!qmlWidget) continue;
        const bool isActive = workspaceVisible
                              && !dock->visibleRegion().isEmpty();
        qmlWidget->rootContext()->setContextProperty(
            "isActiveTab", isActive);
    }
}

// ---------------------------------------------------------------------------
// Event filter — hover-show for close buttons + wheel-scroll between tabs.
// Ported from MdiView's eventFilter, minus the "+" add-button branch.
// ---------------------------------------------------------------------------

bool WorkspaceArea::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == this && event->type() == QEvent::ChildAdded) {
        QTimer::singleShot(0, this, [this]() { styleAllTabBars(); });
    }

    if (auto* tabBar = qobject_cast<QTabBar*>(watched)) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Move: {
            if (tabBar->x() != kTabBarInsetPx)
                insetTabBarGeometry(tabBar, kTabBarInsetPx);
            break;
        }
        case QEvent::MouseMove: {
            const QPoint pos = static_cast<QMouseEvent*>(event)->position().toPoint();
            for (int i = 0; i < tabBar->count(); ++i) {
                if (!tabBar->isTabVisible(i)) continue;  // skip phantom
                QWidget* closeBtn = tabBar->tabButton(i, QTabBar::LeftSide);
                if (closeBtn) {
                    const QRect tabRect = tabBar->tabRect(i);
                    const bool overTabOrButton = tabRect.contains(pos)
                        || closeBtn->geometry().contains(pos);
                    closeBtn->setVisible(overTabOrButton);
                }
            }
            break;
        }
        case QEvent::Leave: {
            for (int i = 0; i < tabBar->count(); ++i) {
                if (!tabBar->isTabVisible(i)) continue;  // skip phantom
                QWidget* closeBtn = tabBar->tabButton(i, QTabBar::LeftSide);
                if (closeBtn) closeBtn->setVisible(false);
            }
            break;
        }
        case QEvent::Wheel: {
            if (tabBar->count() <= 1) break;
            auto* wheelEvent = static_cast<QWheelEvent*>(event);
            int delta = 0;
            if (!wheelEvent->pixelDelta().isNull())
                delta = wheelEvent->pixelDelta().x();
            else if (!wheelEvent->angleDelta().isNull())
                delta = wheelEvent->angleDelta().x() / 2;
            if (delta != 0) {
                const int next = qBound(0,
                    tabBar->currentIndex() + (delta > 0 ? -1 : 1),
                    tabBar->count() - 1);
                if (next != tabBar->currentIndex()) {
                    tabBar->setCurrentIndex(next);
                    updateQmlPluginActiveStates();
                    return true;
                }
            }
            break;
        }
        default:
            break;
        }
    }

    // Close button hover persistence — show when pointer enters the
    // button itself, hide on leave.
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        if (auto* btn = qobject_cast<QToolButton*>(watched)) {
            if (qobject_cast<QTabBar*>(btn->parent())) {
                btn->setVisible(event->type() == QEvent::Enter);
                return false;
            }
        }
    }

    if (event->type() == QEvent::Close) {
        if (auto* dock = qobject_cast<QDockWidget*>(watched)) {
            const QString moduleName = m_docks.key(dock);
            if (!moduleName.isEmpty()) {
                emit pluginClosed(moduleName);
                event->ignore();
                return true;
            }
        }
    }

    // Plugin widget's windowIcon was refreshed
    if (event->type() == QEvent::WindowIconChange) {
        if (auto* widget = qobject_cast<QWidget*>(watched)) {
            for (auto it = m_docks.cbegin(); it != m_docks.cend(); ++it) {
                QWidget* dw = it.value()->widget();
                if (auto* card = dynamic_cast<DockCard*>(dw))
                    dw = card->pluginWidget();
                if (dw != widget) continue;

                const QIcon icon = widget->windowIcon();
                it.value()->setWindowIcon(icon);
                const QString& name = it.key();
                for (QTabBar* tabBar : findChildren<QTabBar*>()) {
                    for (int i = 0; i < tabBar->count(); ++i) {
                        if (moduleNameForTabText(tabBar->tabText(i)) == name)
                            tabBar->setTabIcon(i, icon);
                    }
                }
                break;
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void WorkspaceArea::hideEvent(QHideEvent* event)
{
    QMainWindow::hideEvent(event);
    updateQmlPluginActiveStates();
}

void WorkspaceArea::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    updateQmlPluginActiveStates();
}
