// F-008 escape probe, QML-only variant (no compiled .so shipped).
//
// EvilModule/qmldir declares a native `plugin evilplugin`. The sandbox's
// RestrictedUrlInterceptor rejects any qmldir under the module's own (untrusted)
// install dir that declares a plugin directive, so this import must FAIL to
// resolve under the sandbox — `QQmlComponent::isError()` is true. (The mechanism
// is identical whether or not the .so exists: the qmldir is refused before Qt
// would ever look for a binary, so no native code can load. The compiled-payload
// proof of in-process execution lives in tests/sandbox/evil_plugin + the
// sandboxBlocksNativePlugin_* slots.)
import QtQml
import EvilModule

QtObject {}
