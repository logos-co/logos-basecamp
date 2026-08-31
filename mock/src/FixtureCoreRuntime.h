#pragma once

#include "ICoreRuntime.h"

#include <QJsonArray>
#include <QSet>
#include <QString>

// FixtureCoreRuntime — ICoreRuntime answered from a JSON fixture.
//
// Loads nothing, spawns nothing, executes nothing. Module lists, dependency
// edges and stats come from the `modules` section of LOGOS_MOCK_FIXTURE.
class FixtureCoreRuntime final : public ICoreRuntime {
public:
    // `fixturePath` is a fully-resolved fixture file. Placeholder expansion is
    // the caller's job (MockBackendFixture) — this class only reads.
    explicit FixtureCoreRuntime(const QString& fixturePath);

    void         start() override;
    QStringList  knownModules() const override;
    QStringList  loadedModules() const override;
    bool         loadModule(const QString& name, bool withDependencies) override;
    bool         unloadModule(const QString& name, bool withDependents) override;
    void         refreshModules() override;
    QVariantList allStats() const override;

private:
    QJsonArray  m_modules;          // immutable, from the fixture
    QSet<QString> m_loaded;         // mutable runtime state

    QJsonArray  moduleByName(const QString& name) const;
    QStringList directDependencies(const QString& name) const;
    QStringList transitiveDependencies(const QString& name) const;
    QStringList directDependents(const QString& name) const;
    QStringList transitiveDependents(const QString& name) const;
    bool        isKnown(const QString& name) const;
    // Fixture declaration order, not QSet order — otherwise the Modules tab
    // reshuffles between runs for no reason.
    QStringList inFixtureOrder(const QSet<QString>& names) const;
};
