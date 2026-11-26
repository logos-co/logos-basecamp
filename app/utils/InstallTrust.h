#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

namespace logos::trust {

// Logos's signing keys, as this Basecamp ships them.
struct LogosKeys {
    QStringList dids;     // the key Logos signs with now, then older ones still good
    QStringList revoked;  // never to be trusted again
};

// Basecamp's list. A new key goes first, and the one it replaces stays while
// what it signed should install without asking; a leaked key moves to
// `revoked`. None is ever just dropped. Checked before every release: see
// the README, "Cutting a release".
LogosKeys logosKeys();
QString logosDid();       // the key Logos signs with now
QString logosKeyName();
QString logosName();
QString logosUrl();

enum class Status {
    Trusted,    // signed by a trusted publisher
    Untrusted,  // signed by a publisher not trusted yet
    Unsigned,   // nobody signed it
    Unknown,    // who signed it could not be checked
    Invalid,    // damaged, or a signature that does not hold
    Bundled,    // already there, shipped with Basecamp itself
};

struct Package {
    QString name;
    QString version;
    Status status = Status::Unsigned;
    QString did;
    QString signerName;  // the package's own claim: shown, never trusted
    QString signerUrl;
    QString sha256;      // the catalogue's digest of the whole file, its manifest included
    bool installed = false;
    bool offered = false;
    bool builtIn = false;      // one of Basecamp's own apps (BuiltInApps.h), whoever signed it: Invalid
    bool reserved = false;     // ships with Basecamp, and not signed by Logos: Invalid
    bool backendTaken = false; // an app whose `<name>_backend` another installed package has: Invalid
    QString replacesSignedBy;  // the key that signed the copy it replaces, when another signs it
    bool replacesUnverified = false;  // replaces a copy no checkable key signed, and isn't Logos's
};

// Whether `did` is an Ed25519 key's did:jwk exactly as liblgx writes it. Any
// other spelling of a key is refused: liblgx reads the key out of it leniently,
// and its text can be made to look like another key's.
bool isCanonicalDid(const QString& did);

// What the dialogs show for a key: the first 64 bits of the SHA-256 of the raw
// public key, as "7F3A 91C2 0B4D E8A6". Empty unless `did` is canonical.
QString fingerprint(const QString& did);

// A publisher's or package's own text, made fit to show: line breaks, and
// characters that are invisible or turn the text around, become spaces, and a
// long one is cut so that what follows it stays in view.
QString shown(const QString& text);

// The DIDs package_manager's listTrustedKeys() trusts. A Logos key in it, of
// any age, stands for every key Logos signs with; a revoked one for none.
QStringList trustedDids(const QVariantList& trustedKeys, const LogosKeys& logos = logosKeys());

// A planned package, judged by the release the downloader would fetch for it:
// the one among `versions` (one repository's entries for the package) with
// this version and rootHash, that the download source can serve. That is the
// release whose signer the downloader holds the file to. Unknown when there is
// none, or when such releases disagree about who signed it.
Package fromCatalog(const QString& name, const QString& version, const QString& rootHash,
                    const QVariantList& versions, const QStringList& trusted);

// A package on disk, judged by package_manager's record of the installed copy
// ({installType, signerDid}): shipped with Basecamp, or signed (or not) by the
// key the copy names. Unknown without a record. Marked `installed`.
Package fromInstalled(const QString& name, const QString& version, const QVariantMap& record,
                      const QStringList& trusted);

// Every package of a resolved plan: what it would bring, judged by
// fromCatalog() against `versionsOf(row)`, and what is on disk already and
// stays, judged by that copy -- `installedOf(row)`, package_manager's record
// of it ({installType, signerDid}) -- and marked `installed`. The optional
// packages offered but not chosen (a row's `optionalDependencies`) are judged
// the same way and marked `offered`. A row the resolver could not resolve, or
// no rows at all, is Unknown, never left out.
QList<Package> judgePlan(const QString& name, const QVariantList& resolved,
                         const std::function<bool(const QVariantMap&)>& onDisk,
                         const std::function<QVariantList(const QVariantMap&)>& versionsOf,
                         const std::function<QVariantMap(const QVariantMap&)>& installedOf,
                         const QStringList& trusted, const QStringList& bundled = {},
                         const LogosKeys& logos = logosKeys());

// `package`, as it would replace what is installed as `record`
// (package_manager's, or empty): one of Basecamp's own apps, or a name it
// calls modules under (identityNames()), never installs (Invalid, `builtIn`),
// a reserved name (`bundled`: reservedNames(), or
// reservedFor() for one install) may only come from Logos
// (otherwise Invalid, `reserved`), and one
// signed by another publisher than the copy it replaces says so
// (`replacesSignedBy`; Basecamp's own copy is Logos's), as does one
// replacing a copy no checkable key signed,
// unless Logos signed it (`replacesUnverified`). Logos's keys are one
// publisher; names compare case-insensitively.
Package asReplacement(Package package, const QVariantMap& record, const QStringList& bundled,
                      const LogosKeys& logos = logosKeys());

// package_manager's record of `name` among the `installed` packages: the one
// by exactly that name, else one a case-insensitive file system finds under it
// (Wallet_UI installs over wallet_ui). Empty if none.
QVariantMap installedRecordOf(const QVariantList& installed, const QString& name);

// Who may call package_manager and package_downloader, which hold the keyring
// and install packages: Basecamp (`core`) and the backends of the default-app
// slots allowed to (DefaultApps.h; main.cpp's restrictPackageModules()).
QStringList packageModuleCallers();

// `package`, as installing it would put it on disk: asReplacement(), and an
// app (`type` ui_qml) whose backend's name (`<name>_backend`) an installed
// package has (`backendRecord`, its record; empty if none) is Invalid
// (`backendTaken`). Every way in is judged by this: a catalogue plan's rows
// (the package, its dependencies, its upgrades) and a local file.
Package asInstall(Package package, const QString& type, const QVariantMap& record,
                  const QVariantMap& backendRecord, const QStringList& bundled,
                  const LogosKeys& logos = logosKeys());

// The names Basecamp and the runtime call modules under, and intents_module
// knows them by: Basecamp (`core`), the runtime's `core_service`, which may
// call any module under an enforce policy, the shell (`main_ui`) and the link
// requester (`logos_link`). A package under one would act as them, so none
// installs, Logos's either.
QStringList identityNames();

// The modules Basecamp ships that the person may update themselves, judged
// like any package: Logos Storage. Nothing Basecamp checks relies on it; what
// it downloads is checked after.
QStringList updatableBundledModules();

// The names only Logos may install under (asReplacement()'s `bundled`): the
// `modules` Basecamp ships but updatableBundledModules(), the `apps` it ships
// and the backend each of them loads, every packageModuleCallers() name, and
// intents_module.
QStringList reservedNames(const QStringList& modules, const QStringList& apps);

// The names reserved for one install: `bundled` (reservedNames()), plus the
// backend (`<app>_backend`) of each ui_qml app `installed` or in the `plan`.
// It ships inside its app's package, and a package by that name would make
// the app's backend unregistrable. A name that only ends in _backend, with no
// such app, is a package like any other: the catalogue has eth_wallet_backend.
QStringList reservedFor(const QStringList& bundled, const QVariantList& installed,
                        const QVariantList& plan);

// A name in `reserved`, case-insensitively.
bool isReservedName(const QString& name, const QStringList& reserved);

// The file a confirm request names in `localPath`, if Basecamp may open it at
// all: only the Package Manager installs from a file, and only an absolute,
// local path to a .lgx -- never a network share, which opening alone would
// contact. Empty otherwise: the file is then judged Unknown, unopened.
QString localPackagePath(const QString& path, bool fromPackageManager);

// A local .lgx file, judged by package_manager's verifyPackage() of it.
Package fromVerifiedFile(const QString& name, const QString& version,
                         const QVariantMap& verified, const QStringList& trusted);

// What approving a plan lets package_manager_ui install: each row of
// `resolved`, as a resolver request pinned to that release -- {name, version,
// rootHash, repositoryUrl, signer, sha256, optional}. `signer` is the key
// `judged` (judgePlan()) says signed it; an unsigned one has `sha256`
// instead, the catalogue's digest of the file, which pins its manifest too.
// A row already on disk is left out, except `name` itself.
QVariantList approvedPlan(const QString& name, const QVariantList& resolved,
                          const QList<Package>& judged);

// The local file as Basecamp read it -- {rootHash, signer, sha256} -- for
// package_manager_ui to find again before installing it. Empty when it names
// no root hash, its SHA-256 could not be read, or package_manager's
// inspectPackage() and verifyPackage() of it disagree about what it is or who
// signed it.
QVariantMap fileIdentity(const QString& name, const QString& version,
                         const QVariantMap& verified, const QVariantMap& inspected,
                         const QString& sha256);

// The SHA-256 of the file at `path`, in hex; empty if it can't be read.
QString sha256Of(const QString& path);

// Every key of the publisher `did` is: all of Logos's, old and revoked too,
// for a Logos key; otherwise just `did`.
QStringList publisherKeys(const QString& did, const LogosKeys& logos = logosKeys());

// Whether a confirm request is for a local .lgx: it names the file. Anything
// else is a catalogue release, with or without a repository.
bool requestsLocalFile(const QVariantMap& params);

// A trusted key's name as Basecamp shows it: Logos's for a Logos key, and
// nothing for any other key whose name reads as Logos, whoever put it in the
// keyring.
QString publisherName(const QString& displayName, const QString& did, const LogosKeys& logos = logosKeys());

// A trusted key's address as Basecamp shows it: Logos's for a Logos key, and
// nothing for any other key whose address claims Logos.
QString publisherUrl(const QString& url, const QString& did, const LogosKeys& logos = logosKeys());

// What the install dialogs show, `about` being the package the dialog is
// about (without one, nothing is offered for trust):
//   needsPrompt     something is not signed by a trusted publisher
//   blocked         something is damaged or wrongly signed: Cancel only
//   canTrust        `about` is signed by a publisher not trusted yet, which
//                   may be: "Trust & install" is offered, and trusts that
//                   publisher (`trust`) and nobody else
//   trustCoversAll  trusting it would leave nothing to ask about, so the
//                   other answer is a plain "Install", not "Install anyway"
//   trust        that publisher, as in `publishers`, or empty
//   packages     [{name, version, status, did, fingerprint, signerName,
//                signerUrl}], the ones not trusted
//   publishers   [{did, fingerprint, name, url, packages, impersonates}], the
//                signers not trusted yet. `impersonates` is the name of a
//                trusted publisher (or Logos) whose name this one uses with
//                another key; such a publisher is not offered for trust.
//   byName       {<package>: {status, publisher, impersonates, installed}}
//                for every package judged, keyed by its name as planned: what
//                its row says. `publisher` is the name a trusted publisher was
//                trusted by, or the name an untrusted one claims. A package
//                `installed` already, or only `offered`, is shown here and
//                nowhere else: it asks nothing.
// Every name and URL in it has been through shown(). A package signed with a
// revoked Logos key is Invalid.
QVariantMap summary(const QList<Package>& packages, const QVariantList& trustedKeys,
                    const QString& about = {}, const LogosKeys& logos = logosKeys());

// `packages` as judged without a keyring to check their signers against:
// trusted or not, a signed one is Unknown.
QList<Package> unchecked(QList<Package> packages);

QString keyNameFor(const QString& did);
QString statusName(Status status);

} // namespace logos::trust
