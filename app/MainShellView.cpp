#include "MainShellView.h"

#include "MainContainer.h"
#include "IShellHost.h"

#include <QWidget>

MainShellView::MainShellView(QObject* parent)
    : QObject(parent)
{
}

MainShellView::~MainShellView()
{
    // If Window already destroyed the central widget, m_shell is null and this
    // is a no-op; otherwise this is the last chance to detach the observer.
    destroyShell(m_shell.data());
}

QWidget* MainShellView::createShell(IShellHost* host)
{
    // Not a degraded mode: the shell reaches every host operation through this
    // pointer, so a null one would surface later as a null dereference in a
    // click handler rather than here.
    if (!host) {
        qFatal("MainShellView::createShell requires an IShellHost");
    }

    if (!m_shell) {
        m_shell = new MainContainer(host);
    }
    return m_shell.data();
}

void MainShellView::destroyShell(QWidget* widget)
{
    if (!widget) {
        return;
    }

    // Detach before deleting so no in-flight host callback reaches a
    // half-destroyed observer. MainContainer also does this in its own
    // destructor, for the path where Qt's parent-child teardown gets there
    // first; both are idempotent.
    if (widget == m_shell) {
        m_shell->detachFromHost();
        m_shell = nullptr;
    }

    delete widget;
}

int MainShellView::hostAbiVersion() const
{
    return IShellHost_abi;
}
