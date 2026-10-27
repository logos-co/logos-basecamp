import QtQuick

// Deliberately broken ui_qml view used by tests/ui-tests.mjs. It compiles, so
// PluginLoader's async pre-compile passes, but nothing sets the root's
// required property, so creation in finishUiQmlLoad (setSource) fails with
// "Required property ... was not initialized". Do not "fix" this file.
Item {
    required property string fixtureRequiredValue
}
