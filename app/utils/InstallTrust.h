#pragma once

#include <logos/installer/Trust.h>

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

// Trust at install: basecamp-installer-common's judge (Trust.h: who signed a
// package, and what a dialog shows), and what only Basecamp can judge on top
// of it -- its own apps and modules, the names it calls modules under, an
// app's backend name -- and the approval it answers an install with.
namespace logos::trust {

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

// Whether a confirm request is for a local .lgx: it names the file. Anything
// else is a catalogue release, with or without a repository.
bool requestsLocalFile(const QVariantMap& params);

} // namespace logos::trust
