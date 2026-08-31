# shell-preview — the Basecamp UI shell against a fixture, with no Logos code.
#
# Depends on Qt and app/interfaces only. Deliberately does NOT link liblogos,
# logos-protocol, logos-qt-host or logos-view-module-runtime; the whole point is
# that a core rework cannot reach this binary.
#
# It loads the real, shipped main_ui.so at runtime — the same artifact the real
# host loads, unmodified.

cmake_minimum_required(VERSION 3.21)
project(BasecampShellPreview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

qt_add_executable(basecamp-shell-preview
    src/main.cpp
    src/FixtureBackend.cpp
    src/FixtureBackend.h
    src/FixtureShellHost.cpp
    src/FixtureShellHost.h
    src/FixtureModels.cpp
    src/FixtureModels.h
)

qt_add_resources(basecamp-shell-preview "shell_preview_fixture"
    PREFIX "/shell-preview"
    BASE "fixtures"
    FILES fixtures/shell-fixture.json
)

target_include_directories(basecamp-shell-preview PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # IShellHost.h, IShellView.h, BasecampModelRoles.h — the boundary the shell
    # is compiled against. Headers only; nothing is linked from the app.
    ${CMAKE_CURRENT_SOURCE_DIR}/../app/interfaces
)

target_link_libraries(basecamp-shell-preview PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
