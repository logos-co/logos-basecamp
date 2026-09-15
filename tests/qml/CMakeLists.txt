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

    # The Shell tests reach their components by directory import, which works
    # because nothing under Basecamp/Shell/ is a singleton. AddApplicationDialog
    # is not so lucky: it uses the AppColors and DownloadFormat singletons,
    # registered by a CMake source-file property with no on-disk qmldir behind
    # them, so a directory import cannot resolve either. Rebuilding just the
    # four files it needs as a real Basecamp.AppManager module is what makes
    # `import Basecamp.AppManager` work from a test.
    #
    # Basecamp.Backend, which they also import, is registered from main.cpp.
    set(APPMANAGER_QML_DIR "${BASECAMP_SHELL}/Basecamp/AppManager")
    set(APPMANAGER_QML_FILES
        ${APPMANAGER_QML_DIR}/AddApplicationDialog.qml
        ${APPMANAGER_QML_DIR}/PackageRowDelegate.qml
        ${APPMANAGER_QML_DIR}/AppColors.qml
        ${APPMANAGER_QML_DIR}/DownloadFormat.qml
    )
    # Resource paths are computed relative to CMAKE_CURRENT_SOURCE_DIR, and
    # these sit two directories up — without an alias each lands under a
    # ../../ prefix the engine will not find.
    foreach(qml_file IN LISTS APPMANAGER_QML_FILES)
        get_filename_component(qml_name "${qml_file}" NAME)
        set_source_files_properties("${qml_file}"
            PROPERTIES QT_RESOURCE_ALIAS "${qml_name}")
    endforeach()
    set_source_files_properties(
        ${APPMANAGER_QML_DIR}/AppColors.qml
        ${APPMANAGER_QML_DIR}/DownloadFormat.qml
        PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

    qt_add_qml_module(basecamp_appmanager_test_qml
        URI Basecamp.AppManager
        VERSION 1.0
        STATIC
        RESOURCE_PREFIX /qt/qml
        OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Basecamp/AppManager
        QML_FILES ${APPMANAGER_QML_FILES}
    )
    target_link_libraries(basecamp_appmanager_test_qml PRIVATE Logos::DesignSystem)
    # WHOLE_ARCHIVE: a STATIC qt_add_qml_module registers its types from a
    # static initializer inside the *_qmlplugin archive. Let the linker collect
    # it and there is no error, just an unresolvable import.
    target_link_libraries(qml_tests PRIVATE
        basecamp_appmanager_test_qml
        $<LINK_LIBRARY:WHOLE_ARCHIVE,basecamp_appmanager_test_qmlplugin>
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
