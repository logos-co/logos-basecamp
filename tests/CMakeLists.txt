cmake_minimum_required(VERSION 3.16)
project(LogosBasecampUnitTests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Qml Test Widgets Quick QuickWidgets)

# Third-party libraries a test may declare with `// libdeps: <cmake targets>`.
# Probed rather than REQUIRED so the tests that need neither still configure
# without them; a test that DECLARES one and cannot get it is a hard error
# below, never a silent skip.
find_package(spdlog QUIET)
find_package(yaml-cpp QUIET)

# view_surface_test drives the nested compositor (app/surface/*, RFC-4) with a
# real Wayland client; it exists only where Qt6::WaylandCompositor does.
set(LOGOS_HAVE_WAYLAND_COMPOSITOR OFF)
if(UNIX AND NOT APPLE)
    find_package(Qt6 COMPONENTS WaylandCompositor QUIET)
    if(TARGET Qt6::WaylandCompositor)
        set(LOGOS_HAVE_WAYLAND_COMPOSITOR ON)
    endif()
endif()

# view_surface_test's raw libwayland client (RFC-4 rev 2 §Tests) speaks
# xdg-shell by hand, from protocol code generated here with wayland-scanner.
# Qt6::WaylandCompositor already brings the Wayland::Client target; the XML
# comes from wayland-protocols. Missing either: the raw tests are QSKIPped
# (LOGOS_HAVE_RAW_WAYLAND_CLIENT undefined), and configure says so.
set(LOGOS_HAVE_RAW_WAYLAND_CLIENT OFF)
if(LOGOS_HAVE_WAYLAND_COMPOSITOR)
    find_program(WAYLAND_SCANNER wayland-scanner)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND_PROTOCOLS QUIET wayland-protocols)
        if(WAYLAND_PROTOCOLS_FOUND)
            pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
        endif()
    endif()
    set(_xdg_xml "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
    if(WAYLAND_SCANNER AND EXISTS "${_xdg_xml}" AND TARGET Wayland::Client)
        set(_gen "${CMAKE_CURRENT_BINARY_DIR}/wayland-gen")
        file(MAKE_DIRECTORY "${_gen}")
        add_custom_command(
            OUTPUT "${_gen}/xdg-shell-client-protocol.h" "${_gen}/xdg-shell-protocol.c"
            COMMAND "${WAYLAND_SCANNER}" client-header "${_xdg_xml}" "${_gen}/xdg-shell-client-protocol.h"
            COMMAND "${WAYLAND_SCANNER}" private-code "${_xdg_xml}" "${_gen}/xdg-shell-protocol.c"
            DEPENDS "${_xdg_xml}"
            COMMENT "Generating xdg-shell client protocol for the raw surface client")
        add_library(xdg_shell_client STATIC "${_gen}/xdg-shell-protocol.c")
        target_include_directories(xdg_shell_client PUBLIC "${_gen}")
        target_link_libraries(xdg_shell_client PUBLIC Wayland::Client)
        set_target_properties(xdg_shell_client PROPERTIES LINKER_LANGUAGE C)
        set(LOGOS_HAVE_RAW_WAYLAND_CLIENT ON)
    else()
        message(STATUS "wayland-scanner/wayland-protocols not found: raw Wayland client tests skipped")
    endif()
endif()

enable_testing()

# Two source roots: the host in ../app, the main_ui shell in ../src. The unit
# tests cover both; ../app/interfaces holds the contract headers they share.
set(BASECAMP_SRC   "${CMAKE_CURRENT_SOURCE_DIR}/../app")
set(BASECAMP_SHELL "${CMAKE_CURRENT_SOURCE_DIR}/../src")

if(DEFINED LOGOS_PACKAGE_HEADERS)
    set(LOGOS_PACKAGE_HEADERS_INCLUDE "${LOGOS_PACKAGE_HEADERS}")
endif()

# The intent classes include LogosIntent.h — the frozen vocabulary shared with
# the view-module runtime. It is header-only and pulls in nothing but QtCore, so
# adding the path costs the dependency-free harness nothing.
if(NOT DEFINED LOGOS_VIEW_MODULE_RUNTIME_ROOT)
    foreach(_candidate
            "${CMAKE_CURRENT_SOURCE_DIR}/../../logos-view-module-runtime"
            "$ENV{LOGOS_VIEW_MODULE_RUNTIME_ROOT}")
        if(EXISTS "${_candidate}/include/LogosIntent.h")
            set(LOGOS_VIEW_MODULE_RUNTIME_ROOT "${_candidate}")
            break()
        endif()
    endforeach()
endif()
if(DEFINED LOGOS_VIEW_MODULE_RUNTIME_ROOT)
    set(LOGOS_INTENT_INCLUDE "${LOGOS_VIEW_MODULE_RUNTIME_ROOT}/include")
endif()

# The channel SurfaceTransport (RFC-7) needs the runtime's BUILT
# LogosViewSurface library, which the source checkout above does not have.
# LOGOS_VIEW_SURFACE_ROOT points at a built runtime (nix passes the package);
# without it the Logos.Surface tests run with the Wayland backend only and
# surface_channel_test is skipped. Not a silent skip: configure says so.
set(LOGOS_HAVE_SURFACE_CHANNEL OFF)
if(NOT DEFINED LOGOS_VIEW_SURFACE_ROOT AND DEFINED ENV{LOGOS_VIEW_SURFACE_ROOT})
    set(LOGOS_VIEW_SURFACE_ROOT "$ENV{LOGOS_VIEW_SURFACE_ROOT}")
endif()
if(DEFINED LOGOS_VIEW_SURFACE_ROOT)
    find_library(LOGOS_VIEW_SURFACE_LIB
        NAMES LogosViewSurface
        PATHS "${LOGOS_VIEW_SURFACE_ROOT}/lib" "${LOGOS_VIEW_SURFACE_ROOT}/bin"
        NO_DEFAULT_PATH)
    if(LOGOS_VIEW_SURFACE_LIB AND EXISTS "${LOGOS_VIEW_SURFACE_ROOT}/include/logos/surface/SurfaceHostSession.h")
        set(LOGOS_HAVE_SURFACE_CHANNEL ON)
        set(LOGOS_VIEW_SURFACE_INCLUDE
            "${LOGOS_VIEW_SURFACE_ROOT}/include"
            "${LOGOS_VIEW_SURFACE_ROOT}/include/logos/surface")
        message(STATUS "Channel transport tests enabled: ${LOGOS_VIEW_SURFACE_LIB}")
    endif()
endif()
if(NOT LOGOS_HAVE_SURFACE_CHANNEL)
    message(STATUS "LOGOS_VIEW_SURFACE_ROOT not set or has no LogosViewSurface: channel transport tests skipped")
endif()

# -----------------------------------------------------------------------
# Auto-discovery: any tests/*_test.cpp file is picked up automatically.
# -----------------------------------------------------------------------

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
if(NOT LOGOS_HAVE_WAYLAND_COMPOSITOR)
    list(FILTER TEST_SOURCES EXCLUDE REGEX "view_surface_test\\.cpp$")
    message(STATUS "Qt6::WaylandCompositor not available: skipping view_surface_test")
endif()
if(NOT LOGOS_HAVE_SURFACE_CHANNEL)
    list(FILTER TEST_SOURCES EXCLUDE REGEX "surface_channel_test\\.cpp$")
endif()

foreach(test_src IN LISTS TEST_SOURCES)
    get_filename_component(test_name "${test_src}" NAME_WE)

    # Read the top of the file and look for `// srcdeps: A.cpp B.cpp`.
    file(READ "${test_src}" _head LIMIT 1024)
    set(_extra_srcs "")
    if(_head MATCHES "//[ \t]*srcdeps:[ \t]*([^\n]*)")
        string(STRIP "${CMAKE_MATCH_1}" _deps_line)
        string(REGEX REPLACE "[ \t]+" ";" _dep_list "${_deps_line}")
        # Resolve a bare srcdep path against the host root, then the shell
        # root. Missing in BOTH is fatal: dropping it silently surfaces much
        # later as an undefined reference from the test's own object file.
        foreach(dep IN LISTS _dep_list)
            if(dep)
                if(EXISTS "${BASECAMP_SRC}/${dep}")
                    list(APPEND _extra_srcs "${BASECAMP_SRC}/${dep}")
                elseif(EXISTS "${BASECAMP_SHELL}/${dep}")
                    list(APPEND _extra_srcs "${BASECAMP_SHELL}/${dep}")
                else()
                    message(FATAL_ERROR
                        "srcdep '${dep}' for ${test_name} not found under "
                        "${BASECAMP_SRC} or ${BASECAMP_SHELL}")
                endif()
            endif()
        endforeach()
    endif()

    # Third-party links, same header-comment convention as srcdeps.
    set(_extra_libs "")
    if(_head MATCHES "//[ \t]*libdeps:[ \t]*([^\n]*)")
        string(STRIP "${CMAKE_MATCH_1}" _libs_line)
        string(REGEX REPLACE "[ \t]+" ";" _lib_list "${_libs_line}")
        foreach(lib IN LISTS _lib_list)
            if(lib)
                if(NOT TARGET ${lib})
                    message(FATAL_ERROR
                        "libdep '${lib}' for ${test_name} is not available. "
                        "Run the suite through nix build .#unit-tests, or install it.")
                endif()
                list(APPEND _extra_libs ${lib})
            endif()
        endforeach()
    endif()

    # AUTOMOC pairs a header with a .cpp of the same basename in the SAME
    # directory, and InstallEnums.cpp (src/) and its header (app/interfaces/)
    # are in different ones -- so the header rides along on every target to get
    # InstallStage/InstallStatus' staticMetaObject. Elsewhere: a spare moc TU.
    # A test that builds the Logos.Surface item gets every backend this host
    # compiles in: the item's façade selects them behind
    # LOGOS_HAVE_WAYLAND_COMPOSITOR / LOGOS_HAVE_SURFACE_CHANNEL (both set
    # for all tests), so their sources and libraries must follow.
    set(_wants_channel OFF)
    set(_wants_wayland OFF)
    set(_has_item OFF)
    foreach(_s IN LISTS _extra_srcs)
        if(_s MATCHES "surface/LogosSurfaceItem\\.cpp$")
            set(_has_item ON)
        endif()
        if(_s MATCHES "surface/SurfaceChannelHost\\.cpp$")
            set(_wants_channel ON)
        endif()
    endforeach()
    if(_has_item AND LOGOS_HAVE_SURFACE_CHANNEL)
        set(_wants_channel ON)
    endif()
    if(_has_item AND LOGOS_HAVE_WAYLAND_COMPOSITOR)
        set(_wants_wayland ON)
    endif()
    if(_wants_channel AND LOGOS_HAVE_SURFACE_CHANNEL)
        foreach(_s surface/SurfaceChannelHost.cpp surface/ChannelSurfaceBackend.cpp)
            if(NOT "${BASECAMP_SRC}/${_s}" IN_LIST _extra_srcs)
                list(APPEND _extra_srcs "${BASECAMP_SRC}/${_s}")
            endif()
        endforeach()
    else()
        set(_wants_channel OFF)
    endif()
    if(_wants_wayland)
        foreach(_s surface/SurfaceCompositor.cpp surface/WaylandSurfaceBackend.cpp
                   surface/SurfaceTransportSelector.cpp surface/SurfaceBackend.cpp)
            if(NOT "${BASECAMP_SRC}/${_s}" IN_LIST _extra_srcs)
                list(APPEND _extra_srcs "${BASECAMP_SRC}/${_s}")
            endif()
        endforeach()
        if(NOT "Qt6::WaylandCompositor" IN_LIST _extra_libs)
            list(APPEND _extra_libs Qt6::WaylandCompositor Qt6::WaylandCompositorPrivate)
        endif()
    endif()

    add_executable(${test_name} ${test_src} ${_extra_srcs}
        ${BASECAMP_SRC}/interfaces/InstallEnums.h)
    target_include_directories(${test_name} PRIVATE
        ${BASECAMP_SRC}
        ${BASECAMP_SRC}/interfaces
        ${BASECAMP_SHELL}
        ${LOGOS_PACKAGE_HEADERS_INCLUDE}
        ${LOGOS_INTENT_INCLUDE}
    )
    target_link_libraries(${test_name} PRIVATE
        Qt6::Core
        Qt6::Qml
        Qt6::Widgets
        Qt6::Quick
        Qt6::QuickWidgets
        Qt6::Test
        ${_extra_libs}
    )
    if(LOGOS_HAVE_WAYLAND_COMPOSITOR)
        target_compile_definitions(${test_name} PRIVATE LOGOS_HAVE_WAYLAND_COMPOSITOR)
    endif()
    if(LOGOS_HAVE_RAW_WAYLAND_CLIENT AND test_name STREQUAL "view_surface_test")
        target_compile_definitions(${test_name} PRIVATE LOGOS_HAVE_RAW_WAYLAND_CLIENT)
        target_link_libraries(${test_name} PRIVATE xdg_shell_client)
    endif()
    if(_wants_channel)
        target_compile_definitions(${test_name} PRIVATE LOGOS_HAVE_SURFACE_CHANNEL)
        target_include_directories(${test_name} PRIVATE ${LOGOS_VIEW_SURFACE_INCLUDE})
        target_link_libraries(${test_name} PRIVATE ${LOGOS_VIEW_SURFACE_LIB} Qt6::Network Qt6::Gui)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
