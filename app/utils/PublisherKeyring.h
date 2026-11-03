#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "InstallTrust.h"

#include <functional>

namespace logos::trust {

// The publishers the user trusts: package_manager's keyring. Trusting adds a
// key, deleting removes it, and only a new keyring gets Logos, so a Logos the
// user deleted stays deleted. A Logos key of any age in it is Logos (see
// trustedDids()). A change that fails is reported, and a keyring that can't
// be read trusts nobody.
class PublisherKeyring : public QObject
{
    Q_OBJECT
public:
    using Done = std::function<void(bool ok, const QString& error)>;
    // package_manager's listTrustedKeys, addTrustedKey and removeTrustedKey.
    struct Keyring {
        std::function<void(std::function<void(bool ok, const QVariantList& keys)>)> list;
        std::function<void(const QVariantMap& key, Done)> add;  // {name, did, displayName, url}
        std::function<void(const QString& name, Done)> remove;
    };

    explicit PublisherKeyring(Keyring keyring, QObject* parent = nullptr, LogosKeys logos = logosKeys());

    // The keys trusted now, as listTrustedKeys() has them. Empty until the
    // keyring is read, and while it can't be.
    QVariantList keys() const;
    // Whether the last read worked: if not, no signer can be checked.
    bool readable() const;
    // A row per key, Logos first, then by name: {name, did, fingerprint,
    // displayName, url, addedAt, builtIn, busy}. `busy` while being deleted.
    // A Logos row shows the key Logos signs with now.
    QVariantList publishers() const;

    // Reads the keyring in `folder`. A new one (no folder yet) gets Logos,
    // tried again on each read until a read shows it there.
    void configure(const QString& folder);
    void refresh(std::function<void()> then = {});
    void remove(const QString& keyName);
    // Trusts `publisher` ({did, name, url}), then reads the keyring back.
    // `done` gets an empty string, or why it could not.
    void trust(const QVariantMap& publisher, std::function<void(const QString& error)> done);

signals:
    void changed();
    // A delete that failed, or, with no key name, a keyring that can't be read.
    void changeFailed(const QString& keyName, const QString& message);

private:
    void read(std::function<void()> then);
    QVariantMap named(const QString& keyName) const;
    bool isLogos(const QString& did) const;   // any Logos key, even revoked
    QVariantMap logosKey() const;             // the one it signs with now
    QString labelOf(const QVariantMap& key) const;

    const Keyring m_keyring;
    const LogosKeys m_logos;
    QString m_folder;          // the keyring's, once configured
    QVariantList m_keys;
    bool m_readable = false;
    bool m_addLogos = false;   // a new keyring, Logos not read in it yet
    QSet<QString> m_removing;  // the key names being deleted
};

} // namespace logos::trust
