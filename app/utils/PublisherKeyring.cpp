#include "PublisherKeyring.h"

#include "InstallTrust.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QPointer>

#include <algorithm>
#include <utility>

namespace logos::trust {

namespace {

QString didOf(const QVariantMap& key)
{
    return key.value(QStringLiteral("did")).toString();
}

QString why(const QString& error)
{
    return error.isEmpty() ? QStringLiteral("package_manager did not answer.") : error;
}

} // namespace

PublisherKeyring::PublisherKeyring(Keyring keyring, QObject* parent, LogosKeys logos)
    : QObject(parent)
    , m_keyring(std::move(keyring))
    , m_logos(std::move(logos))
{
}

bool PublisherKeyring::isLogos(const QString& did) const
{
    return m_logos.dids.contains(did) || m_logos.revoked.contains(did);
}

QVariantMap PublisherKeyring::logosKey() const
{
    return {{QStringLiteral("name"), logosKeyName()},
            {QStringLiteral("did"), m_logos.dids.value(0)},
            {QStringLiteral("displayName"), logosName()},
            {QStringLiteral("url"), logosUrl()}};
}

QString PublisherKeyring::labelOf(const QVariantMap& key) const
{
    // A name hidden for reading as Logos gives way to the key file's, under
    // the same rule, then to the key itself.
    QString label = publisherName(key.value(QStringLiteral("displayName")).toString(), didOf(key), m_logos);
    if (label.isEmpty())
        label = publisherName(key.value(QStringLiteral("name")).toString(), didOf(key), m_logos);
    return label.isEmpty() ? QStringLiteral("key ") + fingerprint(didOf(key)) : label;
}

QVariantList PublisherKeyring::keys() const
{
    return m_keys;
}

bool PublisherKeyring::readable() const
{
    return m_readable;
}

QVariantList PublisherKeyring::publishers() const
{
    QVariantList rows;
    for (const QVariant& v : m_keys) {
        const QVariantMap key = v.toMap();
        const QString name = key.value(QStringLiteral("name")).toString();
        const bool builtIn = isLogos(didOf(key));
        const QString did = builtIn ? m_logos.dids.value(0) : didOf(key);
        rows << QVariantMap{
            {QStringLiteral("name"), name},
            {QStringLiteral("did"), did},
            {QStringLiteral("fingerprint"), fingerprint(did)},
            {QStringLiteral("displayName"), labelOf(key)},
            {QStringLiteral("url"), publisherUrl(key.value(QStringLiteral("url")).toString(), didOf(key), m_logos)},
            {QStringLiteral("addedAt"), key.value(QStringLiteral("addedAt"))},
            {QStringLiteral("builtIn"), builtIn},
            {QStringLiteral("busy"), m_removing.contains(name)}};
    }
    // Logos first, then by name, so a row stays where it is.
    std::sort(rows.begin(), rows.end(), [](const QVariant& a, const QVariant& b) {
        const QVariantMap x = a.toMap();
        const QVariantMap y = b.toMap();
        const bool xLogos = x.value(QStringLiteral("builtIn")).toBool();
        if (xLogos != y.value(QStringLiteral("builtIn")).toBool()) return xLogos;
        const int byName = x.value(QStringLiteral("displayName")).toString().compare(
            y.value(QStringLiteral("displayName")).toString(), Qt::CaseInsensitive);
        return byName != 0 ? byName < 0
                           : x.value(QStringLiteral("name")).toString() < y.value(QStringLiteral("name")).toString();
    });
    return rows;
}

void PublisherKeyring::configure(const QString& folder)
{
    m_folder = folder;
    // Before the first keyring call, which makes the folder. No key to add
    // only if the list Basecamp ships is broken: see logosKeys().
    m_addLogos = !QFileInfo::exists(folder) && !m_logos.dids.isEmpty();
    refresh();
}

void PublisherKeyring::refresh(std::function<void()> then)
{
    if (!m_addLogos) {
        read(std::move(then));
        return;
    }
    // Until a read shows Logos there. The user can only delete it after that,
    // so this never brings back a Logos they deleted.
    QPointer<PublisherKeyring> self(this);
    m_keyring.add(logosKey(), [self, then](bool ok, const QString& error) {
        if (!ok)
            qWarning() << "PublisherKeyring: could not trust Logos:" << why(error);
        if (self) self->read(then);
    });
}

void PublisherKeyring::read(std::function<void()> then)
{
    QPointer<PublisherKeyring> self(this);
    m_keyring.list([self, then](bool answered, const QVariantList& keys) {
        if (!self) return;
        // package_manager lists what it could read, and says nothing of a key
        // file it skipped or a folder it couldn't open: fewer keys than files
        // is a keyring that can't be read either.
        const bool ok = answered
            && (self->m_folder.isEmpty()
                || keys.size() >= QDir(self->m_folder).entryList({QStringLiteral("*.json")},
                                                                 QDir::Files | QDir::Hidden).size());
        // What was read before may be out of date: until a read works, nobody is trusted.
        self->m_readable = ok;
        self->m_keys = ok ? keys : QVariantList{};
        if (std::any_of(self->m_keys.cbegin(), self->m_keys.cend(),
                        [&self](const QVariant& v) { return self->isLogos(didOf(v.toMap())); }))
            self->m_addLogos = false;
        if (then) then();
        if (!self) return;
        if (!ok) {
            qWarning() << "PublisherKeyring: could not read the keyring";
            emit self->changeFailed(QString(), tr("Couldn't read all of your trusted publishers, so "
                                                  "none is trusted until they can be read."));
        }
        emit self->changed();
    });
}

QVariantMap PublisherKeyring::named(const QString& keyName) const
{
    for (const QVariant& v : m_keys)
        if (v.toMap().value(QStringLiteral("name")).toString() == keyName)
            return v.toMap();
    return {};
}

void PublisherKeyring::remove(const QString& keyName)
{
    const QVariantMap key = named(keyName);
    if (key.isEmpty() || m_removing.contains(keyName)) return;
    const QString label = labelOf(key);
    m_removing.insert(keyName);
    emit changed();
    QPointer<PublisherKeyring> self(this);
    m_keyring.remove(keyName, [self, keyName, label](bool ok, const QString& error) {
        if (!self) return;
        // Busy until the keyring is read back, so the row never shows the old state.
        self->refresh([self, keyName, label, ok, error]() {
            if (!self) return;
            self->m_removing.remove(keyName);
            // A key gone by then is deleted, whatever the call said.
            if (ok || self->named(keyName).isEmpty()) return;
            const QString failure = tr("Couldn't delete “%1”: %2").arg(label, why(error));
            qWarning() << "PublisherKeyring:" << failure;
            emit self->changeFailed(keyName, failure);
        });
    });
}

void PublisherKeyring::trust(const QVariantMap& publisher,
                             std::function<void(const QString& error)> done)
{
    const QString did = didOf(publisher);
    // Logos's key now, under Basecamp's name for Logos, never one a package claimed.
    const QVariantMap key = m_logos.dids.contains(did)
        ? logosKey()
        : QVariantMap{{QStringLiteral("name"), keyNameFor(did)},
                      {QStringLiteral("did"), did},
                      {QStringLiteral("displayName"), publisher.value(QStringLiteral("name"))},
                      {QStringLiteral("url"), publisher.value(QStringLiteral("url"))}};
    QPointer<PublisherKeyring> self(this);
    m_keyring.add(key, [self, key, done](bool ok, const QString& error) {
        if (!self) return;
        const QString failure = ok ? QString()
                                   : tr("Couldn't trust “%1”: %2").arg(self->labelOf(key), why(error));
        // On only once the keyring has been read back.
        self->refresh([done, failure]() { done(failure); });
    });
}

} // namespace logos::trust
