#include "FixtureShellHost.h"

#include <QDebug>

FixtureShellHost::FixtureShellHost(const QJsonObject& fixture)
    : m_backend(fixture) {}

QObject* FixtureShellHost::backendObject() { return &m_backend; }

int  FixtureShellHost::currentSectionIndex() const { return m_backend.currentActiveSectionIndex(); }

void FixtureShellHost::setCurrentSectionIndex(int index)
{
    m_backend.setCurrentActiveSectionIndex(index);
    if (m_observer) m_observer->onSectionIndexChanged(index);
}

void FixtureShellHost::loadUiModule(const QString& name)
{
    qInfo() << "FixtureShellHost: loadUiModule" << name
            << "— no plugin loading in the preview host";
}

void FixtureShellHost::unloadUiModule(const QString& name)
{
    qInfo() << "FixtureShellHost: unloadUiModule" << name;
}

void FixtureShellHost::setCurrentVisibleApp(const QString& name)
{
    m_backend.setCurrentVisibleApp(name);
}

QString FixtureShellHost::displayNameFor(const QString& name) const
{
    return m_backend.displayNameFor(name);
}

void FixtureShellHost::setObserver(IShellObserver* observer) { m_observer = observer; }
