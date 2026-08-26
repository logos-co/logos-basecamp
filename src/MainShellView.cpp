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
    // Null if Window already destroyed the central widget; otherwise this is
    // the last chance to detach the observer.
    destroyShell(m_shell.data());
}

QWidget* MainShellView::createShell(IShellHost* host)
{
    // Every host operation goes through this pointer; a null one would surface
    // later as a null dereference inside a click handler instead of here.
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
    // half-destroyed observer. MainContainer's destructor does the same when
    // Qt's teardown gets there first; both are idempotent.
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
