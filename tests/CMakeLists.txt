cmake_minimum_required(VERSION 3.16)
project(LogosBasecampUnitTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Qml Test Widgets Quick QuickWidgets)

enable_testing()

set(BASECAMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")

if(DEFINED LOGOS_PACKAGE_HEADERS)
    set(LOGOS_PACKAGE_HEADERS_INCLUDE "${LOGOS_PACKAGE_HEADERS}")
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
        foreach(dep IN LISTS _dep_list)
            if(dep)
                list(APPEND _extra_srcs "${BASECAMP_SRC}/${dep}")
            endif()
        endforeach()
    endif()

    add_executable(${test_name} ${test_src} ${_extra_srcs})
    target_include_directories(${test_name} PRIVATE
        ${BASECAMP_SRC}
        ${LOGOS_PACKAGE_HEADERS_INCLUDE}
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
