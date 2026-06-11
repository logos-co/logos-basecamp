cmake_minimum_required(VERSION 3.16)
project(LogosBasecampUnitTests LANGUAGES CXX)

# Standalone unit-test project for basecamp's pure-C++ logic. Same shape
# as tests/sandbox/CMakeLists.txt — plain Qt Test, no logos-test-framework
# (that's for Logos modules; basecamp is a Qt app and uses Qt's native
# test runner). Driven by nix/unit-tests.nix; can also be built locally:
#
#   cmake -S tests -B build-unit-test
#   cmake --build build-unit-test
#   ctest --test-dir build-unit-test --output-on-failure

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Qml Test)

enable_testing()

# Path to the production basecamp sources under test. The test links the
# real AppsModel + InstallStage so it exercises exactly the same code
# the running app uses — no re-implementation, no mocks.
set(BASECAMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_executable(apps_model_test
    apps_model_test.cpp
    ${BASECAMP_SRC}/AppsModel.cpp
    ${BASECAMP_SRC}/InstallEnums.cpp
)
target_include_directories(apps_model_test PRIVATE ${BASECAMP_SRC})
target_link_libraries(apps_model_test PRIVATE
    Qt6::Core
    Qt6::Qml     # InstallEnums.h pulls in <QtQml/qqml.h>
    Qt6::Test
)

add_test(NAME apps_model_test COMMAND apps_model_test)

# Filter-proxy tests — same target shape as apps_model_test but exercises
# AppsFilterProxy's filter chain against a real AppsModel. Sibling test
# binary so failures attribute cleanly to the proxy vs the model.
add_executable(apps_filter_proxy_test
    apps_filter_proxy_test.cpp
    ${BASECAMP_SRC}/AppsModel.cpp
    ${BASECAMP_SRC}/AppsFilterProxy.cpp
    ${BASECAMP_SRC}/InstallEnums.cpp
)
target_include_directories(apps_filter_proxy_test PRIVATE ${BASECAMP_SRC})
target_link_libraries(apps_filter_proxy_test PRIVATE
    Qt6::Core
    Qt6::Qml
    Qt6::Test
)
add_test(NAME apps_filter_proxy_test COMMAND apps_filter_proxy_test)

# Pins the package_manager.installPlugin IPC-response success contract.
# The helper duplicates PackageCoordinator::installPluginSucceeded so the
# test target stays free of logos-cpp-sdk + Qt Remote Objects. See the
# file's top comment for the rationale.
add_executable(install_plugin_success_test
    install_plugin_success_test.cpp
)
target_link_libraries(install_plugin_success_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME install_plugin_success_test COMMAND install_plugin_success_test)
