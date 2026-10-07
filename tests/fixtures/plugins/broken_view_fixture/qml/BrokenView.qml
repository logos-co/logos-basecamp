import QtQuick

// Deliberately broken ui_qml view used by tests/ui-tests.mjs: references a
// type that does not exist, so PluginLoader's async pre-compile fails the
// same way a ui_qml app built against a newer Logos.Controls does on an
// older host ("X is not a type"). Do not "fix" this file.
Item {
    NonExistentFixtureType {}
}
