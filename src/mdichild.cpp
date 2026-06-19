#include "mdichild.h"
#include "MainUIBackend.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>

MdiChild::MdiChild(MainUIBackend* backend, QWidget *parent)
    : QWidget(parent)
    , m_backend(backend)
{
    setAutoFillBackground(true);
    QPalette p = palette();
    p.setColor(QPalette::Window, QColor("#171717"));
    setPalette(p);

    setAttribute(Qt::WA_StyledBackground, false);

    layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(8);
    layout->addStretch();

    titleLabel = new QLabel(this);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet("color: #FFFFFF; font-size: 24px; font-weight: 700;");

    bodyLabel = new QLabel(this);
    bodyLabel->setAlignment(Qt::AlignCenter);
    bodyLabel->setStyleSheet("color: #A4A4A4; font-size: 14px; font-weight: 400;");
    bodyLabel->setWordWrap(true);

    QHBoxLayout* buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 16, 0, 0);
    installButton = new QPushButton(tr("Install now"), this);
    installButton->setCursor(Qt::PointingHandCursor);
    installButton->setFixedSize(200, 50);
    installButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #262626;"
        "  color: #FFFFFF;"
        "  border: 1px solid #434343;"
        "  border-radius: 16px;"
        "  font-size: 12px;"
        "  font-weight: 500;"
        "  padding: 0 20px;"
        "}"
        "QPushButton:hover, QPushButton:pressed {"
        "  border: 1px solid rgba(255, 136, 0, 0.30);"
        "}");
    buttonRow->addStretch();
    buttonRow->addWidget(installButton);
    buttonRow->addStretch();

    layout->addWidget(titleLabel);
    layout->addWidget(bodyLabel);
    layout->addLayout(buttonRow);
    layout->addStretch();

    setLayout(layout);
    setMinimumSize(300, 200);

    if (m_backend) {
        connect(installButton, &QPushButton::clicked, m_backend, [this]() {
            m_backend->setCurrentActiveSectionIndex(1);
        });
        connect(m_backend, &MainUIBackend::launcherAppsChanged,
                this, &MdiChild::refreshState);
    } else {
        installButton->setEnabled(false);
    }

    refreshState();
}

MdiChild::~MdiChild()
{
}

void MdiChild::refreshState()
{
    const bool hasApps = m_backend && !m_backend->launcherApps().isEmpty();

    if (hasApps) {
        titleLabel->setText(tr("Welcome back"));
        bodyLabel->setText(tr("Launch an installed app from the sidebar to open it here."));
        installButton->setVisible(false);
    } else {
        titleLabel->setText(tr("Welcome to Basecamp!"));
        bodyLabel->setText(tr("Browse the catalog and install your first app to get started."));
        installButton->setVisible(true);
    }
}

void MdiChild::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    const QColor backgroundColor("#171717");
    const QColor borderColor("#434343");
    const qreal radius = 16.0;
    const qreal penWidth = 2.0;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QRectF rect = this->rect().adjusted(penWidth / 2.0, penWidth / 2.0,
                                        -penWidth / 2.0, -penWidth / 2.0);

    painter.setPen(Qt::NoPen);
    painter.setBrush(backgroundColor);
    painter.drawRoundedRect(rect, radius, radius);

    QPen pen(borderColor, penWidth);
    pen.setCosmetic(true);
    pen.setStyle(Qt::DashLine);
    pen.setDashPattern({4.0, 4.0});
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect, radius, radius);
}
