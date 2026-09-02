cmake_minimum_required(VERSION 3.16)
project(LogosBasecampQmlTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickTest Test)

# The design system ships its QML modules as STATIC qt_add_qml_module targets
# behind one umbrella, so a test that instantiates a real shell component has to
# link it — there is no filesystem import path to point at. Optional: the enum
# and proxy contract tests do not need it, and a design-system-less environment
# should still run those.
find_package(LogosDesignSystem CONFIG QUIET)

enable_testing()

# AppsModel and InstallRegistry are host-side; AppsFilterProxy and InstallEnums
# live in the main_ui shell. This suite covers the seam, so it builds from both.
set(BASECAMP_SRC   "${CMAKE_CURRENT_SOURCE_DIR}/../../app")
set(BASECAMP_SHELL "${CMAKE_CURRENT_SOURCE_DIR}/../../src")

if(DEFINED LOGOS_PACKAGE_HEADERS)
    set(LOGOS_PACKAGE_HEADERS_INCLUDE "${LOGOS_PACKAGE_HEADERS}")
endif()

add_executable(qml_tests
    main.cpp
    ${BASECAMP_SRC}/AppsModel.cpp
    ${BASECAMP_SRC}/InstallRegistry.cpp   # AppsModel depends on InstallRegistry
    ${BASECAMP_SHELL}/AppsFilterProxy.cpp
    ${BASECAMP_SHELL}/InstallEnums.cpp
    # AUTOMOC pairs a header with a .cpp of the same basename in the SAME
    # directory; InstallEnums.cpp is in src/ and its header in app/interfaces/,
    # so it must be listed or the link fails on InstallStage/InstallStatus'
    # staticMetaObject.
    ${BASECAMP_SRC}/interfaces/InstallEnums.h
)
target_include_directories(qml_tests PRIVATE
    ${BASECAMP_SRC}
    ${BASECAMP_SRC}/interfaces
    ${BASECAMP_SHELL}
    ${LOGOS_PACKAGE_HEADERS_INCLUDE}
)
if(LogosDesignSystem_FOUND)
    target_link_libraries(qml_tests PRIVATE Logos::DesignSystem)
    target_compile_definitions(qml_tests PRIVATE HAVE_LOGOS_DESIGN_SYSTEM=1)
    message(STATUS "qml_tests: design system found — shell component tests enabled")
else()
    message(STATUS "qml_tests: no design system — shell component tests skipped")
endif()

target_link_libraries(qml_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickTest
    Qt6::Test
)
target_compile_definitions(qml_tests PRIVATE
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

add_test(NAME qml_tests COMMAND qml_tests)
