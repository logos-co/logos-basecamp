#include "mdiview.h"
#include "mdichild.h"
#include <QApplication>
#include <QStyle>
#include <QDebug>
#include <QColor>
#include <QPainter>
#include <QPixmap>
#include <QTimer>
#include <QEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QScroller>
#include <QScrollerProperties>
#include <QEasingCurve>
#include <QQmlContext>
#include <QQuickWidget>

namespace {
// TODO: remove this when moving this over to qml
QIcon makePlusInCircleIcon(int size, QColor circleColor, QColor plusColor)
{
    const int dpr = 2;  // hidpi: render at 2× then let Qt scale
    QPixmap pm(size * dpr, size * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(circleColor);
    p.drawEllipse(0, 0, size, size);
    QPen plusPen(plusColor);
    plusPen.setWidthF(1.5);
    plusPen.setCapStyle(Qt::RoundCap);
    p.setPen(plusPen);
    const int margin = size / 4;
    const int mid = size / 2;
    p.drawLine(mid, margin, mid, size - margin);
    p.drawLine(margin, mid, size - margin, mid);
    return QIcon(pm);
}
}

MdiView::MdiView(QWidget *parent)
    : QWidget(parent)
    , windowCounter(0)
    , m_mdiAddBtn(nullptr)
{
    setupUi();
    addMdiWindow();
}

MdiView::~MdiView()
{
    // Disconnect destroyed-signal lambdas from all tracked subwindows
    // before member destructors run. Without this, ~QWidget::deleteChildren()
    // destroys QMdiSubWindows which emit destroyed(), invoking our lambda
    // that accesses m_pluginWindows/m_subWindowToWidget — already freed.
    for (const auto& conn : m_subWindowConnections) {
        QObject::disconnect(conn);
    }
    m_subWindowConnections.clear();
    m_pluginWindows.clear();
    m_subWindowToWidget.clear();
}

void MdiView::setupUi()
{
    mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    
    mdiArea = new QMdiArea(this);
    mdiArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    mdiArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    mdiArea->setViewMode(QMdiArea::SubWindowView);
    
    // TODO: this should probably be a qml file and should then use Logos.Theme instead
    mdiArea->setBackground(QColor("#171717"));
    
    mdiArea->setTabsClosable(true);
    
    connect(mdiArea, &QMdiArea::subWindowActivated, this,
            [this](QMdiSubWindow*) {
                updateTabCloseButtons();
                updateQmlPluginActiveStates();
            });
    
    mainLayout->addWidget(mdiArea);
    
    setLayout(mainLayout);
    mdiArea->setViewMode(QMdiArea::TabbedView);

    mdiArea->installEventFilter(this);

    // Ensure tab bar styling applies after the tab bar is created
    QTimer::singleShot(0, this, &MdiView::updateTabCloseButtons);
}

void MdiView::addMdiWindow()
{
    MdiChild *child = new MdiChild;
    windowCounter++;
    child->setWindowTitle(tr("MDI Window %1").arg(windowCounter));
    
    QMdiSubWindow *subWindow = mdiArea->addSubWindow(child);
    // Without this, closing only hides the subwindow and it lingers in
    // subWindowList(), leaking and skewing tab/layout logic. Matches addPluginWindow().
    subWindow->setAttribute(Qt::WA_DeleteOnClose);
    subWindow->setMinimumSize(200, 200);
    subWindow->show();
    
    connect(subWindow, &QMdiSubWindow::windowStateChanged, this, &MdiView::updateTabCloseButtons);
}

void MdiView::toggleViewMode()
{
    if (mdiArea->viewMode() == QMdiArea::SubWindowView) {
        mdiArea->setViewMode(QMdiArea::TabbedView);
        toggleButton->setText(tr("Switch to Windowed"));
        
        updateTabCloseButtons();
    } else {
        mdiArea->setViewMode(QMdiArea::SubWindowView);
        toggleButton->setText(tr("Switch to Tabbed"));
        if (m_mdiAddBtn) {
            m_mdiAddBtn->setVisible(false);
        }
    }
}

void MdiView::updateTabCloseButtons()
{
    if (mdiArea->viewMode() == QMdiArea::TabbedView) {
        QTabBar* tabBar = mdiArea->findChild<QTabBar*>();
        if (tabBar) {
            tabBar->setTabsClosable(false);
            disconnect(tabBar, &QTabBar::tabCloseRequested, nullptr, nullptr);
            QObject::disconnect(m_tabChangedConnection);
            m_tabChangedConnection = connect(tabBar, &QTabBar::currentChanged, this,
                    [this](int) { updateQmlPluginActiveStates(); });
            
            connect(tabBar, &QTabBar::tabCloseRequested, [this](int index) {
                QList<QMdiSubWindow*> windows = mdiArea->subWindowList();
                
                if (index >= 0 && index < windows.size()) {
                    windows.at(index)->close();
                    QTimer::singleShot(0, this, &MdiView::updateTabCloseButtons);
                    QTimer::singleShot(0, this, [this]() { updateQmlPluginActiveStates(); });
                }
            });

            customizeTabBarStyle(tabBar);
            ensureMdiAddButton(tabBar);

            QTimer::singleShot(0, this, [this, tabBar]() {
                insetTabBarGeometry(tabBar, 24);
                repositionMdiAddButton();
            });
        }
    }
}

void MdiView::updateQmlPluginActiveStates()
{
    const bool mdiVisible = isVisible();
    QMdiSubWindow* activeSubWindow = mdiVisible ? mdiArea->activeSubWindow() : nullptr;

    for (auto it = m_subWindowToWidget.cbegin(); it != m_subWindowToWidget.cend(); ++it) {
        auto* qmlWidget = qobject_cast<QQuickWidget*>(it.value());
        if (!qmlWidget) {
            continue;
        }
        const bool isActive = mdiVisible && (it.key() == activeSubWindow);
        qmlWidget->rootContext()->setContextProperty("isActiveTab", isActive);
    }
}

void MdiView::insetTabBarGeometry(QTabBar *tabBar, int insetPx)
{
    if (!tabBar) return;
    QWidget *p = tabBar->parentWidget();
    if (!p) return;

    QRect g = tabBar->geometry();
    const bool hasWindows = !mdiArea->subWindowList().isEmpty();
    if (!hasWindows) {
        // Reserve space on the left for the add button.
        const int addButtonReservedSpace = 42 + 10;
        const int leftInset = insetPx + addButtonReservedSpace;
        const int rightInset = 24;
        tabBar->setGeometry(leftInset, g.y(), p->width() - leftInset - rightInset, g.height());
        return;
    }

    // With windows: reserve right side for 10px gap + add button (42px) + right margin (24px).
    const int addButtonWidth = 42;
    const int gapBeforeAddButton = 10;
    const int rightMargin = 24;
    const int rightInset = gapBeforeAddButton + addButtonWidth + rightMargin;
    tabBar->setGeometry(insetPx, g.y(), p->width() - insetPx - rightInset, g.height());
}

void MdiView::customizeTabBarStyle(QTabBar* tabBar)
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
    installTabBarCloseButtons(tabBar);
}

void MdiView::installTabBarCloseButtons(QTabBar* tabBar)
{
    if (!tabBar) return;
    const QTabBar::ButtonPosition closeSide = QTabBar::LeftSide;
    for (int i = 0; i < tabBar->count(); ++i) {
        QWidget* oldBtn = tabBar->tabButton(i, closeSide);
        if (oldBtn) {
            tabBar->setTabButton(i, closeSide, nullptr);
            oldBtn->deleteLater();
        }
        QToolButton* btn = new QToolButton(tabBar);
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
                    tabBar->tabCloseRequested(j);
                    break;
                }
            }
        });
        btn->setVisible(false);  // show only on tab hover
        btn->installEventFilter(this);  // keep visible when hovering the button itself
        tabBar->setTabButton(i, closeSide, btn);
    }
    tabBar->setMouseTracking(true);  // needed for hover-to-show close buttons
}

void MdiView::ensureMdiAddButton(QTabBar* tabBar)
{
    if (!tabBar) {
        return;
    }

    // Create add button - parent it to the tab bar's parent so it's not clipped
    if (!m_mdiAddBtn) {
        m_mdiAddBtn = new QToolButton(tabBar->parentWidget());
        m_mdiAddBtn->setIcon(makePlusInCircleIcon(15,
                                                  QColor(255, 255, 255, 38),
                                                  QColor(255, 255, 255)));
        m_mdiAddBtn->setIconSize(QSize(15, 15));
        m_mdiAddBtn->setAutoRaise(true);
        m_mdiAddBtn->setCursor(Qt::PointingHandCursor);
        m_mdiAddBtn->setFixedSize(25, 19);
        m_mdiAddBtn->setStyleSheet(QStringLiteral(R"(
            QToolButton {
                background: #2A2A2A;
                color: #FFFFFF;
                border-top-left-radius: 8px;
                border-top-right-radius: 8px;
                padding-top: 1px;
            }
            QToolButton:hover {
                background: #262626;
            }
            QToolButton:pressed {
                background: #262626;
            }
        )"));
        connect(m_mdiAddBtn, &QToolButton::clicked, this, &MdiView::addMdiWindow);
        tabBar->installEventFilter(this);
    }

    m_mdiAddBtn->setVisible(true);
    repositionMdiAddButton();
}


void MdiView::repositionMdiAddButton()
{
    QTabBar* tabBar = mdiArea->findChild<QTabBar*>();
    if (!tabBar || !m_mdiAddBtn)
        return;

    QWidget* parent = tabBar->parentWidget();
    if (!parent) return;

    const int leftMargin = 24;
    const int rightMargin = 24;
    int x = leftMargin;

    const bool hasWindows = !mdiArea->subWindowList().isEmpty();
    if (hasWindows) {
        const int stickyX = parent->width() - m_mdiAddBtn->width() - rightMargin;
        x = stickyX;
        // Tab style already has margin-right: 10px, so position add button at last tab's right edge for 10px gap.
        if (tabBar->count() > 0) {
            const QRect lastRect = tabBar->tabRect(tabBar->count() - 1);
            if (lastRect.isValid()) {
                const int desiredX = tabBar->x() + lastRect.right();
                if (desiredX + m_mdiAddBtn->width() <= stickyX)
                    x = desiredX;
            }
        }
    }

    const int y = tabBar->geometry().bottom() - m_mdiAddBtn->height();
    m_mdiAddBtn->move(x, y);
    m_mdiAddBtn->raise();
}

bool MdiView::eventFilter(QObject* watched, QEvent* event)
{
    QTabBar* tabBar = mdiArea->findChild<QTabBar*>();
    if (tabBar && watched == tabBar) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
            repositionMdiAddButton();
        } else if (event->type() == QEvent::MouseMove) {
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
        } else if (event->type() == QEvent::Leave) {
            for (int i = 0; i < tabBar->count(); ++i) {
                QWidget* closeBtn = tabBar->tabButton(i, QTabBar::LeftSide);
                if (closeBtn)
                    closeBtn->setVisible(false);
            }
        } else if (event->type() == QEvent::Wheel && tabBar->count() > 1) {
            auto *wheelEvent = static_cast<QWheelEvent*>(event);
            int delta = 0;
            if (!wheelEvent->pixelDelta().isNull())
                delta = wheelEvent->pixelDelta().x();
            else if (!wheelEvent->angleDelta().isNull())
                delta = wheelEvent->angleDelta().x() / 2;
            if (delta != 0) {
                const int next = qBound(0, tabBar->currentIndex() + (delta > 0 ? -1 : 1), tabBar->count() - 1);
                if (next != tabBar->currentIndex()) {
                    tabBar->setCurrentIndex(next);
                    updateQmlPluginActiveStates();
                    return true;
                }
            }
        }
    }

    // Close button hover: show when pointer enters the button, hide on leave
    if (tabBar) {
        for (int i = 0; i < tabBar->count(); ++i) {
            if (tabBar->tabButton(i, QTabBar::LeftSide) == watched) {
                auto* w = static_cast<QWidget*>(watched);
                if (event->type() == QEvent::Enter)
                    w->setVisible(true);
                else if (event->type() == QEvent::Leave)
                    w->setVisible(false);
                return false;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

QMdiSubWindow* MdiView::addPluginWindow(QWidget* pluginWidget, const QString& title)
{
    if (!pluginWidget) {
        qDebug() << "Cannot add null plugin widget to MDI area";
        return nullptr;
    }
    
    QMdiSubWindow *subWindow = new QMdiSubWindow();
    subWindow->setWidget(pluginWidget);
    subWindow->setAttribute(Qt::WA_DeleteOnClose);
    subWindow->setWindowTitle(title);
    QIcon icon = pluginWidget->windowIcon();
    if (icon.isNull()) {
        icon = QApplication::windowIcon();
    }
    if (!icon.isNull()) {
        subWindow->setWindowIcon(icon);
    }
    
    subWindow->setMinimumSize(200, 200);
    
    QSize widgetSize = pluginWidget->sizeHint();
    if (widgetSize.isValid() && widgetSize.width() > 0 && widgetSize.height() > 0) {
        subWindow->resize(widgetSize);
    } else {
        subWindow->resize(800, 600);
    }
    
    mdiArea->addSubWindow(subWindow);
    
    subWindow->show();
    
    m_pluginWindows[pluginWidget] = subWindow;
    m_subWindowToWidget[subWindow] = pluginWidget;
    
    // Capture the title by value — the QMdiSubWindow is partially destroyed
    // by the time the destroyed signal fires (only ~QObject remains), so
    // calling subWindow->windowTitle() in the slot is undefined behavior.
    QString windowTitle = title;
    m_subWindowConnections[subWindow] =
        connect(subWindow, &QMdiSubWindow::destroyed, this, [this, pluginWidget, subWindow, windowTitle]() {
            if (!windowTitle.isEmpty()) {
                emit pluginWindowClosed(windowTitle);
            }
            m_pluginWindows.remove(pluginWidget);
            m_subWindowToWidget.remove(subWindow);
            m_subWindowConnections.remove(subWindow);
            updateQmlPluginActiveStates();
        });
    
    updateTabCloseButtons();
    updateQmlPluginActiveStates();
    
    return subWindow;
}

void MdiView::removePluginWindow(QWidget* pluginWidget)
{
    if (!pluginWidget || !m_pluginWindows.contains(pluginWidget)) {
        return;
    }

    QMdiSubWindow* subWindow = m_pluginWindows[pluginWidget];
    if (subWindow) {
        subWindow->setWidget(nullptr);
        
        if (m_subWindowToWidget.contains(subWindow)) {
            m_subWindowToWidget.remove(subWindow);
        }
        
        subWindow->close();
        
        m_pluginWindows.remove(pluginWidget);
    }
}

QWidget* MdiView::getWidgetForSubWindow(QMdiSubWindow* subWindow)
{
    if (subWindow && m_subWindowToWidget.contains(subWindow)) {
        return m_subWindowToWidget.value(subWindow);
    }
    return nullptr;
}

void MdiView::activatePluginWindow(QWidget* pluginWidget)
{
    if (!pluginWidget) {
        qDebug() << "MdiView::activatePluginWindow: pluginWidget is null";
        return;
    }
    
    if (!m_pluginWindows.contains(pluginWidget)) {
        qDebug() << "MdiView::activatePluginWindow: pluginWidget not found in map";
        return;
    }
    
    QMdiSubWindow* subWindow = m_pluginWindows[pluginWidget];
    if (subWindow) {
        if (subWindow->widget() == pluginWidget) {
            subWindow->raise();
            subWindow->activateWindow();
            mdiArea->setActiveSubWindow(subWindow);
            updateQmlPluginActiveStates();
        } else {
            qDebug() << "MdiView::activatePluginWindow: subwindow widget mismatch";
        }
    } else {
        qDebug() << "MdiView::activatePluginWindow: subWindow is null";
    }
}

void MdiView::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    updateQmlPluginActiveStates();
}

void MdiView::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    updateQmlPluginActiveStates();
}
