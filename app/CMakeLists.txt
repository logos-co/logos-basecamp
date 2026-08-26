cmake_minimum_required(VERSION 3.16)
project(LogosBasecamp LANGUAGES CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)  # Enable Qt's Meta-Object Compiler
set(CMAKE_AUTORCC ON)  # Enable automatic compilation of resource files
set(CMAKE_AUTOUIC ON)  # Enable Qt's UI compiler
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
# Keep it a SINGLE directory: a second generated dir on the include path would
# make that __has_include depend on -I ORDER, silently.
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
# Load-bearing: UIPluginManager/PluginLoader compile into the exe.
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
    if(NOT TARGET logos-qt-host::logos_qt_host_shared)
        message(FATAL_ERROR
            "find_package(logos-qt-host) succeeded but did not define "
            "logos-qt-host::logos_qt_host_shared. That target arrives with "
            "logos-plugin-qt#22; a build against an older Qt host runtime would "
            "silently fall back to the static archive and give this image its "
            "own TokenManager. LOGOS_QT_HOST_ROOT=${LOGOS_QT_HOST_ROOT}")
    endif()
    # No shim any more. This image used to empty the static archives and take the
    # shared runtime from liblogos_core, which had absorbed both of them; it now
    # links the shared libraries that OWN those types directly
    # (logos-protocol#65, logos-plugin-qt#22, logos-liblogos#182), so there is no
    # second static copy for a shim to suppress.
endif()

set(PROJECT_SOURCES
    main.cpp
    window.h
    window.cpp
    utils/LogRedirector.h
    utils/LogRedirector.cpp
    utils/AccessPolicyOption.h
    utils/AccessPolicyOption.cpp
    utils/DependencyBlocker.h
    macos/trafficLightsTitleBar.h
    macos/trafficLightsTitleBar.cpp
    macos/macWindowStyle.h
    interfaces/IComponent.h
    interfaces/IShellHost.h
    interfaces/IShellView.h
    interfaces/InstallEnums.h
    interfaces/BasecampModelRoles.h
    resources.qrc

    # The host side of the shell boundary. The shell itself lives in ../src and
    # builds as the main_ui plugin; this image never compiles a line of it.
    ShellHostAdapter.cpp

    MainUIBackend.cpp
    CoreModuleManager.cpp
    UIPluginManager.cpp
    PackageCoordinator.cpp
    UninstallPlan.cpp
    InstallRegistry.cpp
    AppsModel.cpp
    ModuleInstanceModel.cpp
    PluginLoader.cpp
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

# The UI shell's seven qt_add_qml_module targets live in ../src. They are STATIC
# modules registering their types from a static initializer, so they link into
# the main_ui plugin — the image that loads the QML — not this executable.
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
# Qt6::Network is named EXPLICITLY rather than left to arrive transitively
# through Quick/QuickWidgets: restricted/DenyAllNetworkAccessManager.cpp
# subclasses QNetworkAccessManager.
#
# This executable owns no QML. Qt6::Quick / Qml / QuickWidgets / QuickControls2
# are linked for the out-of-process UI plugins, and so the portable bundler
# picks them up.
target_link_libraries(LogosBasecamp PRIVATE
    Qt6::Widgets
    Qt6::RemoteObjects
    Qt6::Quick
    Qt6::Qml
    Qt6::QuickWidgets
    Qt6::QuickControls2
    Qt6::Network
    # LOGOS_VIEW_MODULE_RUNTIME_LIB comes BEFORE logos_qt_host_shared for the
    # same GNU ld reason spelled out for logos_core below, and it is load-bearing
    # on Windows for exactly the same mechanism. That archive references
    # LogosAPI::forIdentity, and liblogos_qt_host.dll.a is an IMPORT library, so
    # ld treats it like any archive: it takes only the members satisfying
    # references already undefined when it reaches them. With the host ahead of
    # this archive, nothing has demanded forIdentity yet, the member is skipped,
    # and the mingw link dies with
    #   liblogos_view_module_runtime.a(LogosQmlBridge.cpp.obj): undefined
    #   reference to `LogosAPI::forIdentity(QString const&, QObject*)'
    # despite the symbol being present, correctly mangled, in both the .dll.a
    # (as T and __imp_) and the DLL's export table.
    #
    # This was LATENT until now rather than new. While app/PluginLoader.cpp
    # called LogosAPI::forIdentity itself, basecamp's own objects demanded the
    # symbol before ld reached the host, so the member was pulled early and this
    # archive's later reference resolved against it. Admitting consumers through
    # logos::admitConsumer removed that call — the last first-party reference —
    # and with it the accident that was holding the old order up.
    #
    # Linux and macOS cannot see any of this: there the host is a real shared
    # object, and shared libraries satisfy undefined references regardless of
    # position. A green Linux build proves nothing about this line.
    ${LOGOS_VIEW_MODULE_RUNTIME_LIB}
    logos-qt-host::logos_qt_host_shared
    $<$<BOOL:${ENABLE_QML_INSPECTOR}>:qml-inspector>
    # logos_core is LAST, and that position is load-bearing on Windows: it is the
    # IMPORT LIBRARY for liblogos_core.dll, which re-exports the shared C++
    # runtime defined by liblogos_protocol.dll and liblogos_qt_host.dll. (This
    # comment used to explain the ordering by LogosSharedFromDll.cmake emptying
    # the static archives; that shim was deleted in #348 when the runtime became
    # real shared libraries. The ordering rule below still holds -- only that
    # justification is gone, so do not read its absence as licence to reorder.)
    # GNU ld takes only archive members satisfying references already
    # undefined when it reaches them, so placing it ahead of
    # LOGOS_VIEW_MODULE_RUNTIME_LIB fails the mingw link with four undefined
    # references — LogosAPIClient::whenObjectAvailable / ::eventSubscriptionState
    # / ::pendingEventSubscriptions and logos::qvariantToNlohmann — despite all
    # four being in liblogos_core.dll's export table and its .dll.a.
    logos_core
)

if(APPLE)
    target_link_libraries(LogosBasecamp PRIVATE "-framework Cocoa" "-framework QuartzCore")
endif()

# LOGOS_SHARED_USE_DLL is gone with the shim. It made this image declare the
# runtime types __declspec(dllimport) so ld would not pull the archive member
# that redefines them — necessary while the types lived in a static archive that
# this image also linked. They now come from liblogos_protocol.dll and
# liblogos_qt_host.dll, which are the only definitions in the process, so the
# import is what the link naturally produces rather than something to opt into.
#
# This image still WRITES the capability token (module_manager.cpp calls
# TokenManager::instance().saveToken), so the invariant it protected is
# unchanged — see nix/symbol-gate.nix, which asserts it directly rather than
# trusting a compile flag.

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