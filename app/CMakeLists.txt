cmake_minimum_required(VERSION 3.16)
project(LogosBasecamp LANGUAGES CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)  # Enable Qt's Meta-Object Compiler
set(CMAKE_AUTORCC ON)  # Enable automatic compilation of resource files
set(CMAKE_AUTOUIC ON)  # carried over from the folded main_ui plugin
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# RPATH settings for macOS
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@executable_path/../lib")
else()
    set(CMAKE_INSTALL_RPATH "\$ORIGIN/../lib:\$ORIGIN/lib")
endif()

find_package(Qt6 COMPONENTS Widgets RemoteObjects Quick Qml QuickWidgets QuickControls2 Network REQUIRED)

# QML Inspector (Qt DevTools-like inspection over TCP)
# Enabled by default in Debug builds, disabled in Release.
# Override with -DENABLE_QML_INSPECTOR=ON/OFF
if(NOT DEFINED ENABLE_QML_INSPECTOR)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(ENABLE_QML_INSPECTOR OFF)
    else()
        set(ENABLE_QML_INSPECTOR ON)
    endif()
endif()

if(ENABLE_QML_INSPECTOR)
    if(NOT DEFINED LOGOS_QT_MCP_ROOT)
        # Default: workspace layout (repos/logos-qt-mcp alongside repos/logos-basecamp)
        get_filename_component(LOGOS_QT_MCP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../logos-qt-mcp" REALPATH)
    endif()
    if(NOT EXISTS "${LOGOS_QT_MCP_ROOT}/qt-plugin/CMakeLists.txt")
        message(FATAL_ERROR "logos-qt-mcp not found at ${LOGOS_QT_MCP_ROOT}. "
                            "Set LOGOS_QT_MCP_ROOT or clone https://github.com/logos-co/logos-qt-mcp")
    endif()
    add_subdirectory(${LOGOS_QT_MCP_ROOT}/qt-plugin ${CMAKE_CURRENT_BINARY_DIR}/qml-inspector)
endif()

# Add interfaces, utils, and macos directories to include path
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/interfaces)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/utils)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/macos)
# The ONE generated-header directory. nix/app.nix drops all of it in here:
# the build-info header, the module-generated *_api.h wrappers, the shared
# semver headers and logos-cpp-generator's --general-only output. Absent in
# non-nix builds — BuildInfo.h guards its include with __has_include.
#
# Deliberately a single directory. The plugin build had its own
# src/generated_code alongside this one, so once both targets are the same
# target BuildInfo.h's __has_include would be decided by -I ORDER, silently.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/generated)

# Allow override from environment or command line (for nix builds)
get_filename_component(REAL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)

# ── logos-module (provides interface.h) ──────────────────────────────────────
if(NOT DEFINED LOGOS_MODULE_ROOT)
    set(LOGOS_MODULE_ROOT "${REAL_SOURCE_DIR}/../../logos-module")
endif()

set(_logos_module_found FALSE)
if(EXISTS "${LOGOS_MODULE_ROOT}/src/interface.h")
    set(_logos_module_found TRUE)
    set(_logos_module_is_source TRUE)
elseif(EXISTS "${LOGOS_MODULE_ROOT}/include/module_lib/interface.h")
    set(_logos_module_found TRUE)
    set(_logos_module_is_source FALSE)
endif()

if(NOT _logos_module_found)
    message(FATAL_ERROR "logos-module not found at ${LOGOS_MODULE_ROOT}. "
                        "Set LOGOS_MODULE_ROOT or run git submodule update --init --recursive.")
endif()

message(STATUS "Using logos-module at: ${LOGOS_MODULE_ROOT}")

# logos-module include directories
if(_logos_module_is_source)
    include_directories(${LOGOS_MODULE_ROOT}/src)
else()
    include_directories(${LOGOS_MODULE_ROOT}/include/module_lib)
endif()

# ── logos-qt-sdk — the Qt<->lp SEAM HEADERS, headers only ─────────────────────
# app/generated/ holds the dependency wrappers nix/app.nix copies out of the
# module outputs (package_manager, package_downloader). Since B5 the builder
# emits the Qt-typed wrapper as a VENEER over the lp path, so
# package_manager_api.cpp opens with `#include "logos_qt_lp_bridge.h"`.
#
# That header is NOT part of the host runtime: logos-qt-host does not ship it,
# and logos-qt-sdk is where it stayed after the B1/B2a split. Nothing is LINKED
# from here — after B2a logos-qt-sdk is an INTERFACE library and this is purely
# an include directory.
#
# Probed in both layouts, like every other root above: the source checkout has
# cpp/, the nix install has include/cpp/.
if(NOT DEFINED LOGOS_QT_SDK_ROOT)
    set(LOGOS_QT_SDK_ROOT "${REAL_SOURCE_DIR}/../../logos-qt-sdk")
endif()

if(EXISTS "${LOGOS_QT_SDK_ROOT}/cpp/logos_qt_lp_bridge.h")
    include_directories(${LOGOS_QT_SDK_ROOT}/cpp)
elseif(EXISTS "${LOGOS_QT_SDK_ROOT}/include/cpp/logos_qt_lp_bridge.h")
    include_directories(${LOGOS_QT_SDK_ROOT}/include/cpp)
else()
    # Deliberately fatal rather than a silent skip. A missing seam header does
    # not fail here, it fails 250 objects later as
    # "logos_qt_lp_bridge.h: No such file or directory" while compiling a
    # GENERATED file, which reads as a codegen bug rather than a missing input.
    message(FATAL_ERROR
        "logos_qt_lp_bridge.h not found under ${LOGOS_QT_SDK_ROOT} (tried "
        "cpp/ and include/cpp/). The generated dependency wrappers in "
        "app/generated/ include it by name. Set LOGOS_QT_SDK_ROOT.")
endif()

message(STATUS "Using logos-qt-sdk (seam headers) at: ${LOGOS_QT_SDK_ROOT}")

# ── logos-liblogos (provides liblogos_core library and logos_host binary) ─────
if(NOT DEFINED LOGOS_LIBLOGOS_ROOT)
    set(LOGOS_LIBLOGOS_ROOT "${REAL_SOURCE_DIR}/../../logos-liblogos")
endif()

set(_liblogos_found FALSE)
if(EXISTS "${LOGOS_LIBLOGOS_ROOT}/src/logos_core/logos_core.h")
    set(_liblogos_found TRUE)
    set(_liblogos_is_source TRUE)
elseif(EXISTS "${LOGOS_LIBLOGOS_ROOT}/include/logos_core.h")
    set(_liblogos_found TRUE)
    set(_liblogos_is_source FALSE)
endif()

if(NOT _liblogos_found)
    message(FATAL_ERROR "logos-liblogos not found at ${LOGOS_LIBLOGOS_ROOT}. "
                        "Set LOGOS_LIBLOGOS_ROOT or run git submodule update --init --recursive.")
endif()

message(STATUS "Using logos-liblogos at: ${LOGOS_LIBLOGOS_ROOT}")

# logos-liblogos library paths
if(_liblogos_is_source)
    link_directories(${LOGOS_LIBLOGOS_ROOT}/build/lib)
else()
    link_directories(${LOGOS_LIBLOGOS_ROOT}/lib)
endif()

# ── logos-view-module-runtime (provides ViewModuleHost.h + the ui-host lib) ───
# Folded in from the former src/CMakeLists.txt. -DLOGOS_VIEW_MODULE_RUNTIME_ROOT
# was already being passed to this target by nix/app.nix and was a NO-OP; it is
# load-bearing now that UIPluginManager/PluginLoader compile into the exe.
if(NOT DEFINED LOGOS_VIEW_MODULE_RUNTIME_ROOT)
    set(_candidate "${REAL_SOURCE_DIR}/../../logos-view-module-runtime")
    if(EXISTS "${_candidate}/include/ViewModuleHost.h")
        set(LOGOS_VIEW_MODULE_RUNTIME_ROOT "${_candidate}")
    endif()
endif()
if(NOT DEFINED LOGOS_VIEW_MODULE_RUNTIME_ROOT OR NOT EXISTS "${LOGOS_VIEW_MODULE_RUNTIME_ROOT}/include/ViewModuleHost.h")
    message(FATAL_ERROR "LOGOS_VIEW_MODULE_RUNTIME_ROOT not found. Set it explicitly or provide logos-view-module-runtime as a sibling checkout.")
endif()
message(STATUS "Using logos-view-module-runtime at: ${LOGOS_VIEW_MODULE_RUNTIME_ROOT}")
find_library(LOGOS_VIEW_MODULE_RUNTIME_LIB
    NAMES logos_view_module_runtime
    PATHS ${LOGOS_VIEW_MODULE_RUNTIME_ROOT}/lib
    NO_DEFAULT_PATH
    REQUIRED)

# ── logos-design-system ──────────────────────────────────────────────────────
# Logos.Theme / .Icons / .Controls come from logos-design-system as STATIC
# qt_add_qml_module targets exposed via the Logos::DesignSystem umbrella
# INTERFACE library. Nix passes -DLogosDesignSystem_DIR=<store>/lib/cmake/
# LogosDesignSystem so this find_package succeeds without polluting the
# global CMAKE_PREFIX_PATH.
find_package(LogosDesignSystem CONFIG REQUIRED)

# Umbrella source that includes every generated wrapper (avoids duplicate
# symbols). Do NOT add the individual *_api.cpp files — logos_sdk.cpp includes
# them. Generated into ${CMAKE_CURRENT_SOURCE_DIR}/generated by nix/app.nix;
# absent in a plain non-nix configure, hence the EXISTS guard.
set(GENERATED_LOGOS_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/generated)
set(GENERATED_LOGOS_SDK_CPP ${GENERATED_LOGOS_SDK_DIR}/logos_sdk.cpp)
set(GENERATED_LOGOS_SDK_H   ${GENERATED_LOGOS_SDK_DIR}/logos_sdk.h)

# Add logos-cpp-sdk include directories (for nix builds)
if(DEFINED LOGOS_CPP_SDK_ROOT)
    include_directories(
        ${LOGOS_CPP_SDK_ROOT}/include
        ${LOGOS_CPP_SDK_ROOT}/include/cpp
        ${LOGOS_QT_HOST_ROOT}/include
        ${LOGOS_QT_HOST_ROOT}/include/cpp
        ${LOGOS_QT_HOST_ROOT}/include/core
        ${LOGOS_PROTOCOL_ROOT}/include
        ${LOGOS_PROTOCOL_ROOT}/include/cpp
    )
    link_directories(${LOGOS_CPP_SDK_ROOT}/lib ${LOGOS_QT_HOST_ROOT}/lib ${LOGOS_PROTOCOL_ROOT}/lib)
    # Link the Qt host runtime via its exported target so the app inherits the
    # full transitive link interface (logos-protocol, OpenSSL, Boost::system,
    # nlohmann_json) — a bare archive on the link line would leave Boost.Asio
    # TLS symbols undefined.
    find_package(logos-protocol REQUIRED CONFIG
        PATHS ${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol
        NO_DEFAULT_PATH)
    find_package(logos-qt-host REQUIRED CONFIG
        PATHS ${LOGOS_QT_HOST_ROOT}/lib/cmake/logos-qt-host
        NO_DEFAULT_PATH)
    # Fail loudly rather than silently dropping the host runtime off the link
    # line: the app's LogosAPI/LogosProviderObject symbols all come from here.
    if(NOT TARGET logos-qt-host::logos_qt_host)
        message(FATAL_ERROR
            "find_package(logos-qt-host) succeeded but did not define "
            "logos-qt-host::logos_qt_host. LOGOS_QT_HOST_ROOT="
            "${LOGOS_QT_HOST_ROOT}")
    endif()
    # Windows: the host is the image that WRITES the capability token, so if it
    # keeps a static copy of TokenManager the plugin never sees the token no
    # matter what the plugin links. Empty out the archives here too and let
    # liblogos_core.dll be the one provider — cmake/LogosSharedFromDll.cmake.
    # The link interface above is deliberately left intact.
    #
    # Retargeted from logos-qt-sdk::logos_qt_sdk to logos-qt-host::logos_qt_host
    # by the B4 host split: the archive that carries LogosAPI/TokenManager moved
    # repos, so the archive that must be emptied moved with it. Naming a target
    # that does not exist is now a FATAL_ERROR rather than the silent no-op it
    # used to be — this comment described that hazard for a while before the
    # foreach() enforced it. Getting the name wrong would otherwise regress
    # Windows to the 29 "rejecting unauthorized call" lines this shim prevents.
    include("${CMAKE_CURRENT_LIST_DIR}/../cmake/LogosSharedFromDll.cmake")
    logos_use_shared_runtime_from_dll(
        logos-qt-host::logos_qt_host
        logos-protocol::logos_protocol)
endif()

set(PROJECT_SOURCES
    main.cpp
    window.h
    window.cpp
    utils/LogRedirector.h
    utils/LogRedirector.cpp
    utils/AccessPolicyOption.h
    utils/AccessPolicyOption.cpp
    macos/trafficLightsTitleBar.h
    macos/trafficLightsTitleBar.cpp
    macos/macWindowStyle.h
    interfaces/IComponent.h
    resources.qrc

    # The UI shell, folded in from the former main_ui plugin. It was never a
    # module: it needs a QWidget* handoff, the logos_core_* lifecycle and
    # TokenManager access, and two of those three are host privileges — so the
    # plugin boundary bought nothing and cost a second image.
    MainContainer.cpp
    MainUIBackend.cpp
    CoreModuleManager.cpp
    UIPluginManager.cpp
    PackageCoordinator.cpp
    UninstallPlan.cpp
    InstallRegistry.cpp
    AppsModel.cpp
    ModuleInstanceModel.cpp
    PluginLoader.cpp
    WorkspaceArea.cpp
    ShortcutBridge.cpp
    restricted/DenyAllReply.cpp
    restricted/DenyAllNetworkAccessManager.cpp
    restricted/DenyAllNAMFactory.cpp
    restricted/RestrictedUrlInterceptor.cpp
    restricted/QmlSandbox.cpp
    restricted/SandboxLogging.cpp
)

if(EXISTS ${GENERATED_LOGOS_SDK_CPP})
    set_source_files_properties(${GENERATED_LOGOS_SDK_CPP} PROPERTIES
        GENERATED TRUE
        SKIP_AUTOMOC ON
        SKIP_AUTOUIC ON
        SKIP_AUTORCC ON
    )
    list(APPEND PROJECT_SOURCES ${GENERATED_LOGOS_SDK_CPP})
endif()

if(APPLE)
    list(APPEND PROJECT_SOURCES macos/macWindowStyle.mm)
endif()

option(LOGOS_PORTABLE_BUILD "Build as a portable/distributed app (no /nix/store dependencies at runtime)" OFF)

qt_add_executable(LogosBasecamp
    ${PROJECT_SOURCES}
)

# ── QML modules (folded in verbatim from the former src/CMakeLists.txt) ──────
# These paths are relative to CMAKE_CURRENT_SOURCE_DIR, which is why Basecamp/
# was moved PHYSICALLY into app/ rather than referenced as ../src/Basecamp:
# qt_add_qml_module's QML_FILES/RESOURCES are resolved against this directory
# and a `../` prefix changes where each file lands in the resource tree.
set_source_files_properties(Basecamp/Icons/BasecampIcons.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(Basecamp/AppManager/AppColors.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(basecamp_backend_qml
    URI Basecamp.Backend
    VERSION 1.0
    STATIC
    RESOURCE_PREFIX /qt/qml
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Basecamp/Backend
    SOURCES
        InstallEnums.h
        InstallEnums.cpp
        AppsFilterProxy.h
        AppsFilterProxy.cpp
        ModulesFilterProxy.h
        ModulesFilterProxy.cpp
)

qt_add_qml_module(basecamp_common_qml
    URI Basecamp.Common
    VERSION 1.0
    STATIC
    RESOURCE_PREFIX /qt/qml
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Basecamp/Common
    QML_FILES
        Basecamp/Common/LoadingOverlay.qml
        Basecamp/Common/EmptyView.qml
)

qt_add_qml_module(basecamp_sidebar_qml
    URI Basecamp.Sidebar
    VERSION 1.0
    STATIC
    RESOURCE_PREFIX /qt/qml
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Basecamp/Sidebar
    QML_FILES
        Basecamp/Sidebar/SidebarPanel.qml
        Basecamp/Sidebar/SidebarIconButton.qml
        Basecamp/Sidebar/SidebarCircleButton.qml
        Basecamp/Sidebar/SidebarAppDelegate.qml
)

qt_add_qml_module(basecamp_appmanager_qml
    URI Basecamp.AppManager
    VERSION 1.0
    STATIC
    RESOURCE_PREFIX /qt/qml
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Basecamp/AppManager
    QML_FILES
        Basecamp/AppManager/AppManagerView.qml
        Basecamp/AppManager/AppManagerPanelHeader.qml
        Basecamp/AppManager/AppRepoSection.qml
        Basecamp/AppManager/AppContextMenu.qml
        Basecamp/AppManager/AppGrid.qml
        Basecamp/AppManager/AppGridDelegate.qml
        Basecamp/AppManager/AppListDelegate.qml
        Basecamp/AppManager/PackageRowDelegate.qml
        Basecamp/AppManager/AddApplicationDialog.qml
        Basecamp/AppManager/AppColors.qml
        Basecamp/AppManager/AppTile.qml
)

qt_add_qml_module(basecamp_settings_qml
    URI Basecamp.Settings
    VERSION 1.0
    STATIC
    RESOURCE_PREFIX /qt/qml
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Basecamp/Settings
    QML_FILES
        Basecamp/Settings/SettingsView.qml
        Basecamp/Settings/DashboardView.qml
        Basecamp/Settings/AppsInspectorView.qml
        Basecamp/Settings/ModuleInspectorView.qml
        Basecamp/Settings/InspectorPanelHeader.qml
        Basecamp/Settings/ModuleStatusBadge.qml
        Basecamp/Settings/ModuleRowActions.qml
        Basecamp/Settings/PluginInterfaceView.qml
        Basecamp/Settings/RepositoriesView.qml
)

qt_add_qml_module(basecamp_shell_qml
    URI Basecamp.Shell
    VERSION 1.0
    STATIC
    RESOURCE_PREFIX /qt/qml
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Basecamp/Shell
    QML_FILES
        Basecamp/Shell/ContentViews.qml
        Basecamp/Shell/OverlayDialogs.qml
        Basecamp/Shell/ConfirmationDialog.qml
        Basecamp/Shell/UninstallDialog.qml
        Basecamp/Shell/WelcomePage.qml
)

qt_add_qml_module(basecamp_icons_qml
    URI Basecamp.Icons
    VERSION 1.0
    STATIC
    RESOURCE_PREFIX /qt/qml
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Basecamp/Icons
    QML_FILES
        Basecamp/Icons/BasecampIcons.qml
    RESOURCES
        Basecamp/Icons/basecamp.svg
        Basecamp/Icons/dashboard.svg
        Basecamp/Icons/module.svg
        Basecamp/Icons/settings.svg
        Basecamp/Icons/tent.png
        Basecamp/Icons/workspace.svg
)

qt_import_qml_plugins(LogosBasecamp)

target_include_directories(LogosBasecamp PRIVATE
    ${LOGOS_VIEW_MODULE_RUNTIME_ROOT}/include
    ${GENERATED_LOGOS_SDK_DIR}
)

if(LOGOS_PORTABLE_BUILD)
    target_compile_definitions(LogosBasecamp PRIVATE LOGOS_PORTABLE_BUILD)
    message(STATUS "Building LogosBasecamp as portable (LOGOS_PORTABLE_BUILD enabled)")
endif()

option(LOGOS_DISTRIBUTED_BUILD "Build for distribution (DMG/AppImage)" OFF)
if(LOGOS_DISTRIBUTED_BUILD)
    target_compile_definitions(LogosBasecamp PRIVATE LOGOS_DISTRIBUTED_BUILD)
    message(STATUS "Building LogosBasecamp for distribution (LOGOS_DISTRIBUTED_BUILD enabled)")
endif()

if(ENABLE_QML_INSPECTOR)
    target_compile_definitions(LogosBasecamp PRIVATE ENABLE_QML_INSPECTOR)
    message(STATUS "QML Inspector enabled")
endif()

# Link with logos_core, the Qt host runtime, and all Qt modules the UI shell and
# the out-of-process UI plugins need. Out-of-process UI plugins are loaded at
# runtime and depend on these Qt libraries; linking them here ensures the
# portable bundler includes them in the app bundle.
# logos-qt-host::logos_qt_host is the host runtime the qt split first put in
# logos-qt-sdk and the host split moved to logos-plugin-qt; it carries the
# logos-protocol library transitively.
#
# Qt6::Network is named EXPLICITLY. find_package() has always listed it, but
# nothing linked it: it arrived transitively through QuickWidgets/Quick, and
# restricted/DenyAllNetworkAccessManager.cpp subclasses QNetworkAccessManager.
#
# The seven $<LINK_LIBRARY:WHOLE_ARCHIVE,...qmlplugin> entries are load-bearing
# and must not be trimmed: a STATIC qt_add_qml_module registers its types from a
# static initializer inside the *_qmlplugin archive, so dropping one lets the
# linker garbage-collect it — no build error, blank pane at runtime.
target_link_libraries(LogosBasecamp PRIVATE
    Qt6::Widgets
    Qt6::RemoteObjects
    Qt6::Quick
    Qt6::Qml
    Qt6::QuickWidgets
    Qt6::QuickControls2
    Qt6::Network
    logos-qt-host::logos_qt_host
    ${LOGOS_VIEW_MODULE_RUNTIME_LIB}
    Logos::DesignSystem
    basecamp_backend_qml
    basecamp_common_qml
    basecamp_sidebar_qml
    basecamp_appmanager_qml
    basecamp_settings_qml
    basecamp_shell_qml
    basecamp_icons_qml
    $<LINK_LIBRARY:WHOLE_ARCHIVE,basecamp_backend_qmlplugin>
    $<LINK_LIBRARY:WHOLE_ARCHIVE,basecamp_common_qmlplugin>
    $<LINK_LIBRARY:WHOLE_ARCHIVE,basecamp_sidebar_qmlplugin>
    $<LINK_LIBRARY:WHOLE_ARCHIVE,basecamp_appmanager_qmlplugin>
    $<LINK_LIBRARY:WHOLE_ARCHIVE,basecamp_settings_qmlplugin>
    $<LINK_LIBRARY:WHOLE_ARCHIVE,basecamp_shell_qmlplugin>
    $<LINK_LIBRARY:WHOLE_ARCHIVE,basecamp_icons_qmlplugin>
    $<$<BOOL:${ENABLE_QML_INSPECTOR}>:qml-inspector>
    # logos_core is LAST, and that position is load-bearing on Windows.
    #
    # On Windows liblogos_core.dll is the single provider of the shared C++
    # runtime, so the static archives that define those types are emptied out
    # (LogosSharedFromDll.cmake) and every reference to them has to be satisfied
    # by liblogos_core's IMPORT LIBRARY. GNU ld resolves an archive left to
    # right: it only takes members that satisfy references that are already
    # undefined when it reaches them. With logos_core listed before
    # LOGOS_VIEW_MODULE_RUNTIME_LIB, the view runtime's references to
    # LogosAPIClient::whenObjectAvailable / ::eventSubscriptionState /
    # ::pendingEventSubscriptions and logos::qvariantToNlohmann came up AFTER
    # the import library had already been passed, and the mingw link failed with
    # four undefined references — even though liblogos_core.dll exports all four
    # (measured: they are in its export table and its .dll.a).
    #
    # This did not bite before the main_ui fold only because of where the two
    # sat: the plugin's CMakeLists linked LOGOS_VIEW_MODULE_RUNTIME_LIB in its
    # main target_link_libraries() and logos_core in a separate, LATER call, so
    # the generated order was already correct. Folding the UI shell into this
    # executable brought the view runtime onto a link line whose logos_core came
    # eighth. Nothing else on this list defines symbols logos_core needs — it is
    # an import library for a DLL — so last is both correct and the position
    # that cannot be broken by adding another archive above it.
    logos_core
)

if(APPLE)
    target_link_libraries(LogosBasecamp PRIVATE "-framework Cocoa" "-framework QuartzCore")
endif()

# Windows: import the shared runtime types from liblogos_core instead of taking
# a second static copy from liblogos_qt_host.a. This image WRITES the capability
# token (module_manager.cpp calls TokenManager::instance().saveToken), and PE has
# no symbol interposition, so a second static copy here would be a second token
# store — the failure this whole scheme exists to prevent.
#
# The fold makes this MORE important, not less: the UI shell used to be a second
# image (main_ui.dll) that carried the same opt-in, and now compiles into this
# one. See logos-protocol/cpp/logos_shared_api.h.
if(WIN32)
    target_compile_definitions(LogosBasecamp PRIVATE LOGOS_SHARED_USE_DLL)
endif()

# Copy the core library to the app's lib directory and fix its install name
if(APPLE)
    if(_liblogos_is_source)
        add_custom_command(TARGET LogosBasecamp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/lib
            COMMAND ${CMAKE_COMMAND} -E copy ${LOGOS_LIBLOGOS_ROOT}/build/lib/liblogos_core.dylib ${CMAKE_CURRENT_BINARY_DIR}/lib/
            COMMAND install_name_tool -id @rpath/liblogos_core.dylib ${CMAKE_CURRENT_BINARY_DIR}/lib/liblogos_core.dylib
            # Add additional rpath entries for flexibility when moving the binary
            COMMAND install_name_tool -add_rpath "@executable_path/lib" ${CMAKE_CURRENT_BINARY_DIR}/LogosBasecamp
            COMMAND install_name_tool -add_rpath "@loader_path/lib" ${CMAKE_CURRENT_BINARY_DIR}/LogosBasecamp
            COMMENT "Copying logos_core library and fixing installation paths"
        )
    else()
        add_custom_command(TARGET LogosBasecamp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/lib
            COMMAND ${CMAKE_COMMAND} -E copy ${LOGOS_LIBLOGOS_ROOT}/lib/liblogos_core.dylib ${CMAKE_CURRENT_BINARY_DIR}/lib/
            COMMAND install_name_tool -id @rpath/liblogos_core.dylib ${CMAKE_CURRENT_BINARY_DIR}/lib/liblogos_core.dylib
            # Add additional rpath entries for flexibility when moving the binary
            COMMAND install_name_tool -add_rpath "@executable_path/lib" ${CMAKE_CURRENT_BINARY_DIR}/LogosBasecamp
            COMMAND install_name_tool -add_rpath "@loader_path/lib" ${CMAKE_CURRENT_BINARY_DIR}/LogosBasecamp
            COMMENT "Copying logos_core library and fixing installation paths"
        )
    endif()
elseif(UNIX AND NOT APPLE)
    if(_liblogos_is_source)
        add_custom_command(TARGET LogosBasecamp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/lib
            COMMAND ${CMAKE_COMMAND} -E copy ${LOGOS_LIBLOGOS_ROOT}/build/lib/liblogos_core.so ${CMAKE_CURRENT_BINARY_DIR}/lib/
        )
    else()
        add_custom_command(TARGET LogosBasecamp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/lib
            COMMAND ${CMAKE_COMMAND} -E copy ${LOGOS_LIBLOGOS_ROOT}/lib/liblogos_core.so ${CMAKE_CURRENT_BINARY_DIR}/lib/
        )
    endif()

    # For Linux, CMake's RPATH settings above should handle most needs
    # Check if patchelf is available as an alternative for more complex cases
    find_program(PATCHELF_PROGRAM patchelf)
    if(PATCHELF_PROGRAM)
        add_custom_command(TARGET LogosBasecamp POST_BUILD
            # Properly escape $ORIGIN for shell command
            COMMAND ${PATCHELF_PROGRAM} --set-rpath "\$ORIGIN/lib:\$ORIGIN/../lib:${CMAKE_CURRENT_BINARY_DIR}/lib" ${CMAKE_CURRENT_BINARY_DIR}/LogosBasecamp
            COMMENT "Setting RPATH with patchelf for Linux"
        )
    else()
        message(STATUS "patchelf not found - using CMake's built-in RPATH settings for Linux")
    endif()
endif()

# Copy the logos_host executable from the core build directory.
# CMAKE_EXECUTABLE_SUFFIX is "" natively and ".exe" under mingw; naming the bare
# binary made this POST_BUILD step fail the LINK of LogosBasecamp itself, so the
# error read "FAILED: LogosBasecamp.exe" with no undefined references anywhere --
# the real message was the "Error copying file ... logos_host" line above it.
set(_logos_host_name "logos_host${CMAKE_EXECUTABLE_SUFFIX}")
if(_liblogos_is_source)
    set(_logos_host_src "${LOGOS_LIBLOGOS_ROOT}/build/bin/${_logos_host_name}")
else()
    set(_logos_host_src "${LOGOS_LIBLOGOS_ROOT}/bin/${_logos_host_name}")
endif()
add_custom_command(TARGET LogosBasecamp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "${_logos_host_src}" "${CMAKE_CURRENT_BINARY_DIR}/${_logos_host_name}"
    COMMENT "Copying ${_logos_host_name} to app directory"
)

# Create bin/plugins directory in build folder
add_custom_command(TARGET LogosBasecamp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/bin/plugins
    COMMAND ${CMAKE_COMMAND} -E echo "Creating plugins directory at ${CMAKE_CURRENT_BINARY_DIR}/bin/plugins"

    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/plugins
    COMMAND ${CMAKE_COMMAND} -E echo "Creating app plugins directory at ${CMAKE_CURRENT_BINARY_DIR}/bin/plugins"
)