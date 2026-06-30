#include "WorkspaceArea.h"

#include <QApplication>
#include <QDebug>
#include <QDockWidget>
#include <QEasingCurve>
#include <QEvent>
#include <QHideEvent>
#include <QIcon>
#include <QKeySequence>
#include <QMouseEvent>
#include <QQmlContext>
#include <QQuickWidget>
#include <QScroller>
#include <QScrollerProperties>
#include <QShortcut>
#include <QShowEvent>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
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
}  // namespace

WorkspaceArea::WorkspaceArea(QWidget* parent)
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
        pal.setColor(QPalette::Window, QColor("#0a0a0a"));
        setPalette(pal);
    }
    setContentsMargins(10, 10, 10, 10);

    setStyleSheet(
        "QDockWidget { background: #171717; border: none; border-radius: 8px; }"
        "QDockWidget::title {"
        "  background: #171717;"
        "  padding: 6px 10px;"
        "  border-top-left-radius: 8px;"
        "  border-top-right-radius: 8px;"
        "  color: #cfcfcf;"
        "}"
        "QMainWindow::separator {"
        "  background: transparent;"
        "  width: 12px;"
        "  height: 12px;"
        "}");

    auto* placeholder = new QWidget(this);
    placeholder->setMaximumSize(0, 0);
    setCentralWidget(placeholder);
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
        for (const QString& n : m_dockOrder) {
            auto* d = m_docks.value(n);
            if (!d) continue;

            if (m_sideBySide) {
                if (auto* t = d->titleBarWidget()) {
                    d->setTitleBarWidget(nullptr);
                    t->deleteLater();
                }
            } else if (!d->titleBarWidget()) {
                d->setTitleBarWidget(new ZeroTitleWidget(d));
            }

            if (!prev) {
                addDockWidget(Qt::LeftDockWidgetArea, d);
            } else if (m_sideBySide) {
                splitDockWidget(prev, d, Qt::Horizontal);
            } else {
                tabifyDockWidget(prev, d);
            }
            d->setVisible(true);  // removeDockWidget hides as a side effect
            if (auto* w = d->widget()) w->show();
            prev = d;
        }
        m_firstDock = m_docks.value(m_dockOrder.first());

        if (m_sideBySide) {
            QList<QDockWidget*> docks;
            QList<int> sizes;
            const int each = qMax(1, width() / int(m_dockOrder.size()));
            for (const QString& n : m_dockOrder) {
                if (auto* d = m_docks.value(n)) {
                    docks.append(d);
                    sizes.append(each);
                }
            }
            resizeDocks(docks, sizes, Qt::Horizontal);
        }

        QTimer::singleShot(0, this, [this]() { styleAllTabBars(); });
    });
}

WorkspaceArea::~WorkspaceArea() = default;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void WorkspaceArea::addPluginDock(QWidget* pluginWidget, const QString& name)
{
    if (!pluginWidget || name.isEmpty()) return;
    if (m_docks.contains(name)) {
        activatePluginDock(name);
        return;
    }

    auto* dock = new QDockWidget(name, this);
    dock->setObjectName(name);    // required for saveState/restoreState
    dock->setWidget(pluginWidget);
    dock->setAllowedAreas(Qt::AllDockWidgetAreas);
    dock->setFeatures(QDockWidget::DockWidgetMovable
                      | QDockWidget::DockWidgetClosable);
    dock->setTitleBarWidget(new ZeroTitleWidget(dock));

    const QIcon icon = pluginWidget->windowIcon();
    if (!icon.isNull()) dock->setWindowIcon(icon);

    if (!m_firstDock) {
        addDockWidget(Qt::LeftDockWidgetArea, dock);
        m_firstDock = dock;
    } else if (m_sideBySide) {
        QDockWidget* last = m_docks.value(m_dockOrder.last());
        splitDockWidget(last, dock, Qt::Horizontal);
    } else {
        tabifyDockWidget(m_firstDock, dock);
        dock->raise();
    }

    m_docks[name] = dock;
    m_dockOrder.append(name);

    QTimer::singleShot(0, this, [this]() { styleAllTabBars(); });
    updateQmlPluginActiveStates();
}

void WorkspaceArea::removePluginDock(const QString& name)
{
    auto it = m_docks.find(name);
    if (it == m_docks.end()) return;

    QDockWidget* dock = it.value();
    m_docks.erase(it);
    m_dockOrder.removeAll(name);

    // Detach the plugin widget BEFORE deleting the dock so the widget
    // is owned by whoever passed it in (UIPluginManager), not torn down
    // with the dock.
    dock->setWidget(nullptr);
    removeDockWidget(dock);
    dock->deleteLater();

    if (m_firstDock == dock) {
        m_firstDock = m_dockOrder.isEmpty() ? nullptr
                                            : m_docks.value(m_dockOrder.first());
    }

    QTimer::singleShot(0, this, [this]() { styleAllTabBars(); });
    updateQmlPluginActiveStates();
}

void WorkspaceArea::activatePluginDock(const QString& name)
{
    if (auto* dock = m_docks.value(name)) {
        dock->raise();
        dock->show();
        updateQmlPluginActiveStates();
    }
}

QString WorkspaceArea::nameForWidget(QWidget* w) const
{
    if (!w) return {};
    for (auto it = m_docks.cbegin(); it != m_docks.cend(); ++it) {
        if (it.value()->widget() == w) return it.key();
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
// installTabBarCloseButtons + insetTabBarGeometry).
// ---------------------------------------------------------------------------

void WorkspaceArea::customizeTabBarStyle(QTabBar* tabBar)
{
    if (!tabBar) return;

    tabBar->setDocumentMode(true);
    tabBar->setDrawBase(false);
    tabBar->setAutoFillBackground(false);
    tabBar->setElideMode(Qt::ElideRight);
    tabBar->setUsesScrollButtons(false);
    tabBar->setExpanding(false);
    tabBar->setIconSize(QSize(15, 15));
    QScroller::grabGesture(tabBar, QScroller::LeftMouseButtonGesture);
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

    // Wheel-scrolling + tab-close-by-X. Forward the close request to
    // removePluginDock + emit pluginClosed.
    connect(tabBar, &QTabBar::tabCloseRequested, this,
            [this, tabBar](int index) {
                const QString name = tabBar->tabText(index);
                if (name.isEmpty()) return;
                emit pluginClosed(name);
                removePluginDock(name);
            });

    // Track tab-current changes so we can update plugin isActiveTab.
    connect(tabBar, &QTabBar::currentChanged, this,
            [this]() { updateQmlPluginActiveStates(); });
}

void WorkspaceArea::installTabBarCloseButtons(QTabBar* tabBar)
{
    if (!tabBar) return;
    const QTabBar::ButtonPosition closeSide = QTabBar::LeftSide;
    for (int i = 0; i < tabBar->count(); ++i) {
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
    const int rightInset = 24;
    tabBar->setGeometry(insetPx, g.y(),
                        p->width() - insetPx - rightInset, g.height());
}

void WorkspaceArea::styleAllTabBars()
{
    for (auto* tabBar : findChildren<QTabBar*>()) {
        if (!m_styledTabBars.contains(tabBar)) {
            m_styledTabBars.insert(tabBar);
            customizeTabBarStyle(tabBar);
            tabBar->installEventFilter(this);
        }
        // Re-run close-button install in case Qt grew the tab count.
        installTabBarCloseButtons(tabBar);
        QTimer::singleShot(0, this, [this, tabBar]() {
            insetTabBarGeometry(tabBar, 24);
        });
    }
}

// ---------------------------------------------------------------------------
// Plugin active-state propagation (port of MdiView::updateQmlPluginActiveStates)
// ---------------------------------------------------------------------------

void WorkspaceArea::updateQmlPluginActiveStates()
{
    const bool workspaceVisible = isVisible();
    for (auto it = m_docks.cbegin(); it != m_docks.cend(); ++it) {
        QDockWidget* dock = it.value();
        auto* qmlWidget = qobject_cast<QQuickWidget*>(dock->widget());
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
        case QEvent::MouseMove: {
            const QPoint pos = static_cast<QMouseEvent*>(event)->position().toPoint();
            for (int i = 0; i < tabBar->count(); ++i) {
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
