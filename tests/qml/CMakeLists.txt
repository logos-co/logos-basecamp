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

# AppsModel is host-side; AppsFilterProxy and InstallEnums live in the main_ui
# shell. This suite covers the seam, so it builds from both.
set(BASECAMP_SRC   "${CMAKE_CURRENT_SOURCE_DIR}/../../app")
set(BASECAMP_SHELL "${CMAKE_CURRENT_SOURCE_DIR}/../../src")

if(DEFINED LOGOS_PACKAGE_HEADERS)
    set(LOGOS_PACKAGE_HEADERS_INCLUDE "${LOGOS_PACKAGE_HEADERS}")
endif()

add_executable(qml_tests
    main.cpp
    ${BASECAMP_SRC}/AppsModel.cpp
    ${BASECAMP_SHELL}/AppsFilterProxy.cpp
    ${BASECAMP_SHELL}/ModulesFilterProxy.cpp   # the Settings inspectors' tables
    ${BASECAMP_SHELL}/InstallEnums.cpp
    # AUTOMOC pairs a header with a .cpp of the same basename in the SAME
    # directory; InstallEnums.cpp is in src/ and its header in app/interfaces/,
    # so it must be listed or the link fails on InstallStatus'
    # staticMetaObject.
    ${BASECAMP_SRC}/interfaces/InstallEnums.h
)
target_include_directories(qml_tests PRIVATE
    ${BASECAMP_SRC}
    ${BASECAMP_SRC}/interfaces
    ${BASECAMP_SHELL}
    ${LOGOS_PACKAGE_HEADERS_INCLUDE}
)
# The shared installer files (Basecamp.Installer): plain JavaScript the shell
# and the enum contract tests import, so built with or without the design system.
set(INSTALLER_QML_FILES
    ${BASECAMP_SHELL}/Basecamp/Installer/Availability.js
    ${BASECAMP_SHELL}/Basecamp/Installer/Downloads.js
    ${BASECAMP_SHELL}/Basecamp/Installer/Stages.js
    ${BASECAMP_SHELL}/Basecamp/Installer/Types.js
)
foreach(js_file IN LISTS INSTALLER_QML_FILES)
    get_filename_component(js_name "${js_file}" NAME)
    set_source_files_properties("${js_file}" PROPERTIES QT_RESOURCE_ALIAS "${js_name}")
endforeach()
qt_add_qml_module(basecamp_installer_test_qml
    URI Basecamp.Installer
    VERSION 1.0
    STATIC
    RESOURCE_PREFIX /qt/qml
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Basecamp/Installer
    QML_FILES ${INSTALLER_QML_FILES}
)
target_link_libraries(qml_tests PRIVATE
    basecamp_installer_test_qml
    $<LINK_LIBRARY:WHOLE_ARCHIVE,basecamp_installer_test_qmlplugin>
)

if(LogosDesignSystem_FOUND)
    target_link_libraries(qml_tests PRIVATE Logos::DesignSystem)
    target_compile_definitions(qml_tests PRIVATE HAVE_LOGOS_DESIGN_SYSTEM=1)
    message(STATUS "qml_tests: design system found — shell component tests enabled")

    # The Shell tests reach their components by directory import, which works
    # because nothing under Basecamp/Shell/ is a singleton. Basecamp.Backend,
    # which they also import, is registered from main.cpp.

    # Basecamp.Common, for the install dialogs' trust notice and badges and the
    # Settings inspectors' loading overlay, imported as the module it is in
    # production. The alias keeps each file at the module's root (they sit two
    # directories up); WHOLE_ARCHIVE keeps the static plugin's registration,
    # without which the import fails with no error.
    set(COMMON_QML_FILES
        ${BASECAMP_SHELL}/Basecamp/Common/EmptyView.qml
        ${BASECAMP_SHELL}/Basecamp/Common/InstallTrustNotice.qml
        ${BASECAMP_SHELL}/Basecamp/Common/InstallTrustLine.qml
        ${BASECAMP_SHELL}/Basecamp/Common/InstallTrustMark.qml
        ${BASECAMP_SHELL}/Basecamp/Common/LoadingOverlay.qml
    )
    foreach(qml_file IN LISTS COMMON_QML_FILES)
        get_filename_component(qml_name "${qml_file}" NAME)
        set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qml_name}")
    endforeach()
    qt_add_qml_module(basecamp_common_test_qml
        URI Basecamp.Common
        VERSION 1.0
        STATIC
        RESOURCE_PREFIX /qt/qml
        OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Basecamp/Common
        QML_FILES ${COMMON_QML_FILES}
    )
    target_link_libraries(basecamp_common_test_qml PRIVATE Logos::DesignSystem)
    target_link_libraries(qml_tests PRIVATE
        basecamp_common_test_qml
        $<LINK_LIBRARY:WHOLE_ARCHIVE,basecamp_common_test_qmlplugin>
    )
    # Basecamp.Icons, the shell's own icons, and Basecamp.Common's EmptyView:
    # what the welcome page imports.
    set(ICONS_DIR "${BASECAMP_SHELL}/Basecamp/Icons")
    set(ICONS_RESOURCES
        ${ICONS_DIR}/logos-lambda.svg
        ${ICONS_DIR}/dashboard.svg
        ${ICONS_DIR}/filter.svg
        ${ICONS_DIR}/module.svg
        ${ICONS_DIR}/settings.svg
        ${ICONS_DIR}/tent.png
        ${ICONS_DIR}/workspace.svg
    )
    foreach(icon_file IN LISTS ICONS_RESOURCES ITEMS ${ICONS_DIR}/BasecampIcons.qml)
        get_filename_component(icon_name "${icon_file}" NAME)
        set_source_files_properties("${icon_file}" PROPERTIES QT_RESOURCE_ALIAS "${icon_name}")
    endforeach()
    set_source_files_properties(${ICONS_DIR}/BasecampIcons.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
    qt_add_qml_module(basecamp_icons_test_qml
        URI Basecamp.Icons
        VERSION 1.0
        STATIC
        RESOURCE_PREFIX /qt/qml
        OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Basecamp/Icons
        QML_FILES ${ICONS_DIR}/BasecampIcons.qml
        RESOURCES ${ICONS_RESOURCES}
    )
    target_link_libraries(qml_tests PRIVATE
        basecamp_icons_test_qml
        $<LINK_LIBRARY:WHOLE_ARCHIVE,basecamp_icons_test_qmlplugin>
    )
    qt_import_qml_plugins(qml_tests)
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
