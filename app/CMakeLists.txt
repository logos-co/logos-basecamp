cmake_minimum_required(VERSION 3.16)
project(LogosApp LANGUAGES CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)  # Enable Qt's Meta-Object Compiler
set(CMAKE_AUTORCC ON)  # Enable automatic compilation of resource files
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

find_package(Qt6 COMPONENTS Widgets RemoteObjects Quick Qml QuickWidgets QuickControls2 REQUIRED)

# Add interfaces, utils, and macos directories to include path
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/interfaces)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/utils)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/macos)

# Allow override from environment or command line (for nix builds)
if(NOT DEFINED LOGOS_LIBLOGOS_ROOT)
    # Get the real path of current source directory to handle symlinks
    get_filename_component(REAL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
    set(_core_poc_liblogos "${REAL_SOURCE_DIR}/../../logos-liblogos")
    if(EXISTS "${_core_poc_liblogos}/interface.h")
        set(LOGOS_LIBLOGOS_ROOT "${_core_poc_liblogos}")
    else()
        # Fallback to relative path
        set(LOGOS_LIBLOGOS_ROOT "${REAL_SOURCE_DIR}/../../logos-liblogos")
    endif()
endif()

# Check if dependencies are available (support both source and installed layouts)
set(_liblogos_found FALSE)
if(EXISTS "${LOGOS_LIBLOGOS_ROOT}/interface.h")
    set(_liblogos_found TRUE)
    set(_liblogos_is_source TRUE)
elseif(EXISTS "${LOGOS_LIBLOGOS_ROOT}/include/interface.h")
    set(_liblogos_found TRUE)
    set(_liblogos_is_source FALSE)
endif()

if(NOT _liblogos_found)
    message(FATAL_ERROR "logos-liblogos not found at ${LOGOS_LIBLOGOS_ROOT}. "
                        "Set LOGOS_LIBLOGOS_ROOT or run git submodule update --init --recursive.")
endif()

message(STATUS "Using logos-liblogos at: ${LOGOS_LIBLOGOS_ROOT}")
message(STATUS "logos-liblogos layout: ${_liblogos_is_source}")

# Include directories based on layout type
if(_liblogos_is_source)
    include_directories(
        ${LOGOS_LIBLOGOS_ROOT}/host
        ${LOGOS_LIBLOGOS_ROOT}
    )
    # Set logos_core library path for source layout
    link_directories(${LOGOS_LIBLOGOS_ROOT}/build/lib)
else()
    include_directories(
        ${LOGOS_LIBLOGOS_ROOT}/include
    )
    # Set logos_core library path for installed layout
    link_directories(${LOGOS_LIBLOGOS_ROOT}/lib)
endif()

# Add logos-cpp-sdk include directories (for nix builds)
if(DEFINED LOGOS_CPP_SDK_ROOT)
    include_directories(
        ${LOGOS_CPP_SDK_ROOT}/include
        ${LOGOS_CPP_SDK_ROOT}/include/cpp
        ${LOGOS_CPP_SDK_ROOT}/include/core
    )
    link_directories(${LOGOS_CPP_SDK_ROOT}/lib)
endif()

set(PROJECT_SOURCES
    main.cpp
    window.h
    window.cpp
    macos/trafficLightsTitleBar.h
    macos/trafficLightsTitleBar.cpp
    macos/macWindowStyle.h
    interfaces/IComponent.h
    resources.qrc
)

if(APPLE)
    list(APPEND PROJECT_SOURCES macos/macWindowStyle.mm)
endif()

option(LOGOS_PORTABLE_BUILD "Build as a portable/distributed app (no /nix/store dependencies at runtime)" OFF)

qt_add_executable(LogosApp
    ${PROJECT_SOURCES}
)

if(LOGOS_PORTABLE_BUILD)
    target_compile_definitions(LogosApp PRIVATE LOGOS_PORTABLE_BUILD)
    message(STATUS "Building LogosApp as portable (LOGOS_PORTABLE_BUILD enabled)")
endif()

# Link with logos_core, logos_sdk, and all Qt modules needed by plugins.
# Plugins (main_ui, package_manager_ui) are loaded at runtime and depend on
# these Qt libraries. Linking them here ensures the portable bundler includes
# them in the app bundle.
target_link_libraries(LogosApp PRIVATE
    Qt6::Widgets
    Qt6::RemoteObjects
    Qt6::Quick
    Qt6::Qml
    Qt6::QuickWidgets
    Qt6::QuickControls2
    logos_core
    logos_sdk
)

if(APPLE)
    target_link_libraries(LogosApp PRIVATE "-framework Cocoa" "-framework QuartzCore")
endif()

# Copy the core library to the app's lib directory and fix its install name
if(APPLE)
    if(_liblogos_is_source)
        add_custom_command(TARGET LogosApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/lib
            COMMAND ${CMAKE_COMMAND} -E copy ${LOGOS_LIBLOGOS_ROOT}/build/lib/liblogos_core.dylib ${CMAKE_CURRENT_BINARY_DIR}/lib/
            COMMAND install_name_tool -id @rpath/liblogos_core.dylib ${CMAKE_CURRENT_BINARY_DIR}/lib/liblogos_core.dylib
            # Add additional rpath entries for flexibility when moving the binary
            COMMAND install_name_tool -add_rpath "@executable_path/lib" ${CMAKE_CURRENT_BINARY_DIR}/LogosApp
            COMMAND install_name_tool -add_rpath "@loader_path/lib" ${CMAKE_CURRENT_BINARY_DIR}/LogosApp
            COMMENT "Copying logos_core library and fixing installation paths"
        )
    else()
        add_custom_command(TARGET LogosApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/lib
            COMMAND ${CMAKE_COMMAND} -E copy ${LOGOS_LIBLOGOS_ROOT}/lib/liblogos_core.dylib ${CMAKE_CURRENT_BINARY_DIR}/lib/
            COMMAND install_name_tool -id @rpath/liblogos_core.dylib ${CMAKE_CURRENT_BINARY_DIR}/lib/liblogos_core.dylib
            # Add additional rpath entries for flexibility when moving the binary
            COMMAND install_name_tool -add_rpath "@executable_path/lib" ${CMAKE_CURRENT_BINARY_DIR}/LogosApp
            COMMAND install_name_tool -add_rpath "@loader_path/lib" ${CMAKE_CURRENT_BINARY_DIR}/LogosApp
            COMMENT "Copying logos_core library and fixing installation paths"
        )
    endif()
elseif(UNIX AND NOT APPLE)
    if(_liblogos_is_source)
        add_custom_command(TARGET LogosApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/lib
            COMMAND ${CMAKE_COMMAND} -E copy ${LOGOS_LIBLOGOS_ROOT}/build/lib/liblogos_core.so ${CMAKE_CURRENT_BINARY_DIR}/lib/
        )
    else()
        add_custom_command(TARGET LogosApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/lib
            COMMAND ${CMAKE_COMMAND} -E copy ${LOGOS_LIBLOGOS_ROOT}/lib/liblogos_core.so ${CMAKE_CURRENT_BINARY_DIR}/lib/
        )
    endif()

    # For Linux, CMake's RPATH settings above should handle most needs
    # Check if patchelf is available as an alternative for more complex cases
    find_program(PATCHELF_PROGRAM patchelf)
    if(PATCHELF_PROGRAM)
        add_custom_command(TARGET LogosApp POST_BUILD
            # Properly escape $ORIGIN for shell command
            COMMAND ${PATCHELF_PROGRAM} --set-rpath "\$ORIGIN/lib:\$ORIGIN/../lib:${CMAKE_CURRENT_BINARY_DIR}/lib" ${CMAKE_CURRENT_BINARY_DIR}/LogosApp
            COMMENT "Setting RPATH with patchelf for Linux"
        )
    else()
        message(STATUS "patchelf not found - using CMake's built-in RPATH settings for Linux")
    endif()
endif()

# Copy the logos_host executable from the core build directory
if(_liblogos_is_source)
    add_custom_command(TARGET LogosApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${LOGOS_LIBLOGOS_ROOT}/build/bin/logos_host ${CMAKE_CURRENT_BINARY_DIR}/logos_host
        COMMENT "Copying logos_host executable to app directory"
    )
else()
    add_custom_command(TARGET LogosApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${LOGOS_LIBLOGOS_ROOT}/bin/logos_host ${CMAKE_CURRENT_BINARY_DIR}/logos_host
        COMMENT "Copying logos_host executable to app directory"
    )
endif()

# Create bin/plugins directory in build folder
add_custom_command(TARGET LogosApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/bin/plugins
    COMMAND ${CMAKE_COMMAND} -E echo "Creating plugins directory at ${CMAKE_CURRENT_BINARY_DIR}/bin/plugins"

    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/plugins
    COMMAND ${CMAKE_COMMAND} -E echo "Creating app plugins directory at ${CMAKE_CURRENT_BINARY_DIR}/bin/plugins"
)

# Add src/main_ui plugin
# add_subdirectory(${CMAKE_SOURCE_DIR}/src/main_ui)