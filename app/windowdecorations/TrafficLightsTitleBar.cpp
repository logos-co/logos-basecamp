#include "TrafficLightsTitleBar.h"
#include <QPushButton>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QWindow>
#include <QIcon>
#include <QApplication>
#include <QMainWindow>

QPushButton* makeTrafficButton(const QString& colorHex, const QString& borderHex, int size, QWidget* parent) {
    auto* btn = new QPushButton(parent);
    btn->setFixedSize(size, size);
    btn->setCursor(Qt::ArrowCursor);
    btn->setFlat(true);
    btn->setStyleSheet(
        QString("QPushButton {"
                "  background-color: %1;"
                "  border: 0.5px solid %2;"
                "  border-radius: %3px;"
                "}"
                "QPushButton:hover { background-color: %1; }"
                "QPushButton:pressed { background-color: %1; }")
            .arg(colorHex, borderHex)
            .arg(size / 2));
    return btn;
}

TrafficLightsTitleBar::TrafficLightsTitleBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(kTitleBarHeight);
    setCursor(Qt::ArrowCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
    
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(kLeftMargin, kTopMargin, 0, 0);
    layout->setSpacing(kButtonSpacing);

    m_closeBtn = makeTrafficButton("#FF5F57", "#E0443E", kButtonSize, this);
    layout->addWidget(m_closeBtn);

    m_minBtn = makeTrafficButton("#FEBC2E", "#DE9E24", kButtonSize, this);
    layout->addWidget(m_minBtn);

    m_zoomBtn = makeTrafficButton("#28C840", "#1AAC37", kButtonSize, this);
    layout->addWidget(m_zoomBtn);

    layout->addStretch(1);

    connect(m_closeBtn, &QPushButton::clicked, this, [this]() { window()->hide(); });
    connect(m_minBtn, &QPushButton::clicked, this, [this]() { window()->showMinimized(); });
    connect(m_zoomBtn, &QPushButton::clicked, this, [this]() {
        QWidget* w = window();
        if (w->windowState() & Qt::WindowFullScreen)
            w->setWindowState(Qt::WindowNoState);
        else
            w->setWindowState(Qt::WindowFullScreen);
    });

    m_closeBtn->installEventFilter(this);
    m_minBtn->installEventFilter(this);
    m_zoomBtn->installEventFilter(this);
}

bool TrafficLightsTitleBar::isOverButton(const QPoint& pos) const {
    if (m_closeBtn && m_closeBtn->isVisible() && m_closeBtn->geometry().contains(pos)) {
        return true;
    }
    if (m_minBtn && m_minBtn->isVisible() && m_minBtn->geometry().contains(pos)) {
        return true;
    }
    if (m_zoomBtn && m_zoomBtn->isVisible() && m_zoomBtn->geometry().contains(pos)) {
        return true;
    }
    return false;
}

void TrafficLightsTitleBar::setButtonIcon(QPushButton* btn, const QString& iconPath) {
    if (!btn) return;
    const QSize iconSize(6, 6);
    btn->setIcon(QIcon(iconPath));
    btn->setIconSize(iconSize);
}

void TrafficLightsTitleBar::setAllButtonIcons() {
    setButtonIcon(m_closeBtn, ":/icons/trafficlights/close.png");
    setButtonIcon(m_minBtn, ":/icons/trafficlights/minimise.png");
    setButtonIcon(m_zoomBtn, ":/icons/trafficlights/maximize.png");
}

void TrafficLightsTitleBar::clearAllButtonIcons() {
    if (m_closeBtn) m_closeBtn->setIcon(QIcon());
    if (m_minBtn) m_minBtn->setIcon(QIcon());
    if (m_zoomBtn) m_zoomBtn->setIcon(QIcon());
}

void TrafficLightsTitleBar::leaveEvent(QEvent* e) {
    QWidget::leaveEvent(e);
    clearAllButtonIcons();
}

bool TrafficLightsTitleBar::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Enter) {
        if (watched == m_closeBtn || watched == m_minBtn || watched == m_zoomBtn) {
            setAllButtonIcons();
        }
    } else if (event->type() == QEvent::Leave) {
        if (watched == m_closeBtn || watched == m_minBtn || watched == m_zoomBtn) {
            clearAllButtonIcons();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TrafficLightsTitleBar::forwardEventToCentralWidget(QMouseEvent* e) {
    QMainWindow* mainWindow = qobject_cast<QMainWindow*>(window());
    if (!mainWindow) return;
    
    QWidget* centralWidget = mainWindow->centralWidget();
    if (!centralWidget) return;
    
    // Map coordinates from title bar space to central widget space
    QPoint globalPos = mapToGlobal(e->pos());
    QPoint centralPos = centralWidget->mapFromGlobal(globalPos);
    
    // Check if the position is within central widget bounds
    if (!centralWidget->rect().contains(centralPos)) return;
    
    // Find the actual child widget at this position
    QWidget* targetWidget = centralWidget->childAt(centralPos);
    if (!targetWidget) {
        // No specific child at this position, send to central widget itself
        targetWidget = centralWidget;
    }
    
    // Map coordinates to target widget's space
    QPoint targetPos = targetWidget->mapFromGlobal(globalPos);
    
    // Create a new event with mapped coordinates
    QMouseEvent mappedEvent(
        e->type(),
        targetPos,
        e->globalPosition(),
        e->button(),
        e->buttons(),
        e->modifiers()
    );
    
    // Send the event to the target widget
    QApplication::sendEvent(targetWidget, &mappedEvent);
}

void TrafficLightsTitleBar::mousePressEvent(QMouseEvent* e) {
    if (isOverButton(e->pos())) {
        QWidget::mousePressEvent(e);
        return;
    }
    
    // Only handle left button for potential dragging
    // Forward all other buttons (right-click, middle-click, etc.) to central widget
    if (e->button() != Qt::LeftButton) {
        forwardEventToCentralWidget(e);
        e->ignore();
        return;
    }
    
    // Left button: prepare for potential drag
    m_dragActive = false;
    m_dragStartPos = e->pos();
    e->accept();
}

void TrafficLightsTitleBar::mouseMoveEvent(QMouseEvent* e) {
    if (isOverButton(e->pos())) {
        QWidget::mouseMoveEvent(e);
        return;
    }
    
    // If left button is pressed and we haven't started dragging yet
    if (!m_dragActive && (e->buttons() & Qt::LeftButton)) {
        // Check if we've moved enough to start a drag
        int distance = (e->pos() - m_dragStartPos).manhattanLength();
        if (distance >= kDragThresholdPx) {
            if (QWindow* w = window()->windowHandle()) {
                w->startSystemMove();
                m_dragActive = true;
            }
        }
    } else if (!(e->buttons() & Qt::LeftButton)) {
        // No left button - forward hover events to central widget
        forwardEventToCentralWidget(e);
    }
    
    QWidget::mouseMoveEvent(e);
}

void TrafficLightsTitleBar::mouseReleaseEvent(QMouseEvent* e) {
    if (isOverButton(e->pos())) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    
    // If this was a left button release and we never started dragging,
    // it was a click - forward it to the central widget
    if (e->button() == Qt::LeftButton) {
        bool wasClick = !m_dragActive;
        m_dragActive = false;
        
        if (wasClick) {
            // This was a click, not a drag - forward the release to central widget
            // Also synthesize a press event so the central widget gets a complete click
            QMainWindow* mainWindow = qobject_cast<QMainWindow*>(window());
            if (mainWindow) {
                QWidget* centralWidget = mainWindow->centralWidget();
                if (centralWidget) {
                    QPoint globalPos = mapToGlobal(e->pos());
                    QPoint centralPos = centralWidget->mapFromGlobal(globalPos);
                    
                    if (centralWidget->rect().contains(centralPos)) {
                        // Find the actual child widget at this position
                        QWidget* targetWidget = centralWidget->childAt(centralPos);
                        if (!targetWidget) {
                            targetWidget = centralWidget;
                        }
                        
                        // Map coordinates to target widget's space
                        QPoint targetPos = targetWidget->mapFromGlobal(globalPos);
                        
                        // Send press event
                        QMouseEvent pressEvent(
                            QEvent::MouseButtonPress,
                            targetPos,
                            e->globalPosition(),
                            e->button(),
                            e->button(),
                            e->modifiers()
                        );
                        QApplication::sendEvent(targetWidget, &pressEvent);
                        
                        forwardEventToCentralWidget(e);
                    }
                }
            }
        }
    } else {
        // Non-left button release - forward to central widget
        forwardEventToCentralWidget(e);
    }
}

void TrafficLightsTitleBar::mouseDoubleClickEvent(QMouseEvent* e) {
    if (isOverButton(e->pos())) {
        QWidget::mouseDoubleClickEvent(e);
        return;
    }
    
    forwardEventToCentralWidget(e);
    e->ignore();
}