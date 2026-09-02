cmake_minimum_required(VERSION 3.16)
project(LogosBasecampUnitTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Qml Test Widgets Quick QuickWidgets)

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

# -----------------------------------------------------------------------
# Auto-discovery: any tests/*_test.cpp file is picked up automatically.
# -----------------------------------------------------------------------

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

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

    # AUTOMOC pairs a header with a .cpp of the same basename in the SAME
    # directory, and InstallEnums.cpp (src/) and its header (app/interfaces/)
    # are in different ones -- so the header rides along on every target to get
    # InstallStage/InstallStatus' staticMetaObject. Elsewhere: a spare moc TU.
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
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
