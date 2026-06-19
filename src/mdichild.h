#ifndef MDICHILD_H
#define MDICHILD_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

class MainUIBackend;

class MdiChild : public QWidget
{
    Q_OBJECT

public:
    MdiChild(MainUIBackend* backend = nullptr, QWidget *parent = nullptr);
    ~MdiChild();

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void refreshState();

private:
    MainUIBackend* m_backend;
    QLabel *titleLabel;
    QLabel *bodyLabel;
    QPushButton *installButton;
    QVBoxLayout *layout;
};

#endif // MDICHILD_H
