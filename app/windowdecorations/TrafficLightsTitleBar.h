#ifndef WINDOWDECORATIONS_TRAFFICLIGHTSTITLEBAR_H
#define WINDOWDECORATIONS_TRAFFICLIGHTSTITLEBAR_H

#include <QWidget>
#include <QPoint>
#include <QEvent>

class QPushButton;
class QMouseEvent;

class TrafficLightsTitleBar : public QWidget
{
    Q_OBJECT

public:
    static const int kTitleBarHeight = 28;
    static const int kButtonSize = 12;
    static const int kButtonSpacing = 6;
    static const int kLeftMargin = 10;
    static const int kTopMargin = 4;

    explicit TrafficLightsTitleBar(QWidget* parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool isOverButton(const QPoint& pos) const;
    void forwardEventToCentralWidget(QMouseEvent* e);
    void setButtonIcon(QPushButton* btn, const QString& iconPath);
    void setAllButtonIcons();
    void clearAllButtonIcons();

    QPushButton* m_closeBtn = nullptr;
    QPushButton* m_minBtn = nullptr;
    QPushButton* m_zoomBtn = nullptr;

    bool m_dragActive = false;
    QPoint m_dragStartPos;
    static const int kDragThresholdPx = 4;
};

#endif // WINDOWDECORATIONS_TRAFFICLIGHTSTITLEBAR_H
