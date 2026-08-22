cmake_minimum_required(VERSION 3.21)

project(main_ui LANGUAGES CXX)

# ─────────────────────────────────────────────────────────────────────────────
# main_ui — Basecamp's UI shell, as a Qt plugin.
#
# This target links Qt and NOTHING ELSE from this workspace. No logos runtime,
# no logos-qt-host, no liblogos, no generated module wrappers. That is the whole
# point of the split and it is mechanically enforced: nix/symbol-gate.nix
# measures every image loaded into the app's process and fails if a runtime type
# is defined in more than one of them.
#
# Everything the shell needs from the host arrives through IShellHost, and the
# only host headers it may see are the contract ones in app/interfaces/ —
# IShellHost.h, IShellView.h, InstallEnums.h and BasecampModelRoles.h. That
# directory, and not app/ itself, is on the include path below, so including
# MainUIBackend.h or logos_api.h from here does not compile.
#
# Built FORWARD from app/CMakeLists.txt minus the runtime, deliberately, rather
# than restored from the pre-fold revision: that version still lists
# Basecamp/AppManager/AppTile.qml in QML_FILES, a file master deleted, and
# fails at configure.
# ─────────────────────────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 COMPONENTS Core Gui Widgets Quick Qml QuickWidgets QuickControls2 REQUIRED)
qt_standard_project_setup(REQUIRES 6.5)

# The design system ships Logos.Theme / .Icons / .Controls as STATIC
# qt_add_qml_module targets behind the Logos::DesignSystem umbrella.
find_package(LogosDesignSystem CONFIG REQUIRED)

# The ONLY host directory this target may see. Widening this to ${CMAKE_SOURCE_DIR}/app
# would put every host header — MainUIBackend.h, logos_api.h, token_manager.h —
# one #include away, and the boundary would hold only by convention.
set(BASECAMP_INTERFACES ${CMAKE_CURRENT_SOURCE_DIR}/../app/interfaces)

# Global, not per-target: each qt_add_qml_module below creates its OWN target,
# and those targets do not inherit main_ui's include directories. Without this
# basecamp_backend_qml fails to find BasecampModelRoles.h.
include_directories(${BASECAMP_INTERFACES})

set(SOURCES
    MainShellView.h
    MainShellView.cpp
    MainContainer.h
    MainContainer.cpp
    WorkspaceArea.h
    WorkspaceArea.cpp
    ShortcutBridge.h
    ShortcutBridge.cpp
    ${BASECAMP_INTERFACES}/IShellHost.h
    ${BASECAMP_INTERFACES}/IShellView.h
    ${BASECAMP_INTERFACES}/BasecampModelRoles.h
)

# SHARED, not MODULE: QPluginLoader is happy with either, and SHARED is what the
# pre-fold target used — keeping it identical means the PREFIX/SUFFIX block
# below produces byte-identically named artefacts to what app/window.cpp and the
# bundler already look for.
add_library(main_ui SHARED ${SOURCES})

# ── QML modules ─────────────────────────────────────────────────────────────
# Paths are relative to CMAKE_CURRENT_SOURCE_DIR, which is why Basecamp/ lives
# physically beside this file rather than being referenced as ../app/Basecamp:
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
        ../app/interfaces/InstallEnums.h
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


# Registers the STATIC qml module plugins with this target. Without it the
# resource roots exist but nothing imports them.
qt_import_qml_plugins(main_ui)

target_include_directories(main_ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${BASECAMP_INTERFACES}
)

# The seven $<LINK_LIBRARY:WHOLE_ARCHIVE,...qmlplugin> entries are load-bearing
# and must not be trimmed: a STATIC qt_add_qml_module registers its types from a
# static initializer inside the *_qmlplugin archive, so dropping one lets the
# linker garbage-collect it — no build error, blank pane at runtime.
target_link_libraries(main_ui
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Quick
        Qt6::Qml
        Qt6::QuickWidgets
        Qt6::QuickControls2
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
)

# Kept verbatim from the pre-fold target, including the branch order.
if(APPLE)
    set_target_properties(main_ui PROPERTIES
        BUNDLE FALSE
        FRAMEWORK FALSE
        PREFIX ""
        SUFFIX ".dylib"
    )
elseif(WIN32)
    # Windows MUST be tested before the else-branch. Without it the plugin was
    # emitted as main_ui.so -- a genuine PE carrying a Unix extension -- and
    # app/window.cpp looks for ".dll" on Q_OS_WIN, so it would never be found
    # and the app would report "No main UI" with nothing obviously wrong.
    set_target_properties(main_ui PROPERTIES
        PREFIX ""
        SUFFIX ".dll"
    )
else()
    set_target_properties(main_ui PROPERTIES
        PREFIX ""
        SUFFIX ".so"
    )
endif()

install(TARGETS main_ui LIBRARY DESTINATION lib RUNTIME DESTINATION lib)
