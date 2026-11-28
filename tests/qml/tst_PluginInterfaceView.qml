import QtQuick
import QtTest

import Logos.Controls
import Logos.Theme

// Directory import: see tst_IntentChooserDialog.qml.
import "../../src/Basecamp/Settings"

// basecamp#562: a module's Interface called every method with no arguments,
// so a method that takes any could not be called. Each parameter gets a field,
// and Call sends what was typed with the method's parameters.
TestCase {
    id: testCase
    name: "PluginInterfaceView"
    visible: true
    when: windowShown
    width: 800
    height: 600

    QtObject {
        id: stub
        property var calls: []
        function getCoreModuleMethods(name) {
            return JSON.stringify([
                { name: "get_blocks", signature: "get_blocks(qulonglong,qulonglong)",
                  parameters: [{ name: "from_slot", type: "qulonglong" },
                               { name: "to_slot", type: "qulonglong" }] },
                { name: "get_chain_id", signature: "get_chain_id()" }
            ])
        }
        function getCoreModuleEvents(name) { return "[]" }
        function callCoreModuleMethod(module, method, parametersJson, argsJson) {
            calls.push({ module: module, method: method,
                         parameters: JSON.parse(parametersJson), args: JSON.parse(argsJson) })
            return JSON.stringify({ result: "ok" })
        }
    }

    Component {
        id: viewComp
        PluginInterfaceView { width: testCase.width; height: testCase.height; api: stub }
    }

    function init() { stub.calls = [] }

    function test_each_parameter_gets_a_field_and_call_sends_them() {
        const view = createTemporaryObject(viewComp, testCase, { pluginName: "blockchain_module" })
        waitForRendering(view)
        const from = findChild(view, "pluginInterface.arg.get_blocks.0")
        const to = findChild(view, "pluginInterface.arg.get_blocks.1")
        verify(from && from.visible, "a field for from_slot")
        verify(to && to.visible, "a field for to_slot")
        verify(!findChild(view, "pluginInterface.arg.get_chain_id.0"), "none for a method without")

        from.text = "10"
        to.text = "20"
        findChild(view, "pluginInterface.call.get_blocks").clicked()
        compare(stub.calls.length, 1)
        compare(stub.calls[0].module, "blockchain_module")
        compare(stub.calls[0].method, "get_blocks")
        compare(stub.calls[0].args, ["10", "20"])
        compare(stub.calls[0].parameters.map(p => p.type), ["qulonglong", "qulonglong"])
        compare(view.resultText, JSON.stringify({ result: "ok" }))
    }

    function test_a_method_without_parameters_sends_none() {
        const view = createTemporaryObject(viewComp, testCase, { pluginName: "blockchain_module" })
        waitForRendering(view)
        findChild(view, "pluginInterface.call.get_chain_id").clicked()
        compare(stub.calls.length, 1)
        compare(stub.calls[0].args, [])
        compare(stub.calls[0].parameters, [])
    }
}
