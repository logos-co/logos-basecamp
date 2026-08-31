#include "FixtureModels.h"

FixtureListModel::FixtureListModel(QHash<int, QByteArray> roles, QObject* parent)
    : QAbstractListModel(parent), m_roles(std::move(roles)) {}

void FixtureListModel::setRows(const QVariantList& rows)
{
    beginResetModel();
    m_rows = rows;
    endResetModel();
}

int FixtureListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant FixtureListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) return {};
    const auto it = m_roles.constFind(role);
    if (it == m_roles.cend()) return {};
    return m_rows.at(index.row()).toMap().value(QString::fromUtf8(it.value()));
}

QHash<int, QByteArray> appsRoleNames()
{
    using R = AppsModelRoles;
    return {
        {R::NameRole, "name"}, {R::RepositoryUrlRole, "repositoryUrl"},
        {R::DisplayNameRole, "displayName"}, {R::DescriptionRole, "description"},
        {R::CategoryRole, "category"}, {R::TypeRole, "type"},
        {R::IconUrlRole, "iconUrl"}, {R::SupportsFullBleedIconRole, "supportsFullBleedIcon"},
        {R::VersionsRole, "versions"}, {R::DependenciesRole, "dependencies"},
        {R::InstalledVersionRole, "installedVersion"}, {R::LatestVersionRole, "latestVersion"},
        {R::HasUpdateRole, "hasUpdate"}, {R::IsInstalledRole, "isInstalled"},
        {R::MissingDepsRole, "missingDeps"}, {R::InstallStatusRole, "installStatus"},
        {R::InstallTypeRole, "installType"}, {R::ActionRole, "action"},
        {R::ToVersionRole, "toVersion"}, {R::IsTopLevelRole, "isTopLevel"},
        {R::ResolverErrorRole, "resolverError"}, {R::InstallStageRole, "installStage"},
        {R::InstallErrorRole, "installError"},
    };
}

QHash<int, QByteArray> moduleInstanceRoleNames()
{
    using R = ModuleInstanceRoles;
    return {
        {R::NameRole, "name"}, {R::LabelRole, "label"},
        {R::DescriptionRole, "description"}, {R::CategoryRole, "category"},
        {R::TypeRole, "type"}, {R::VersionRole, "version"},
        {R::IconPathRole, "iconPath"}, {R::InstallTypeRole, "installType"},
        {R::IsLoadedRole, "isLoaded"}, {R::IsMainUiRole, "isMainUi"},
        {R::HasMissingDepsRole, "hasMissingDeps"}, {R::StatusTextRole, "statusText"},
        {R::CpuRole, "cpu"}, {R::MemoryRole, "memory"},
    };
}
