cmake_minimum_required(VERSION 3.16)
project(MainUIPlugin VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find Qt packages
find_package(Qt6 REQUIRED COMPONENTS Core Widgets RemoteObjects Quick Qml QuickWidgets QuickControls2)

# Allow override from environment or command line (for nix builds)
if(NOT DEFINED LOGOS_CPP_SDK_ROOT)
    # Get the real path to handle symlinks correctly
    get_filename_component(REAL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
    set(_parent_cpp_sdk "${REAL_SOURCE_DIR}/../../logos-cpp-sdk")
    if(EXISTS "${_parent_cpp_sdk}/cpp/logos_api.h")
        set(LOGOS_CPP_SDK_ROOT "${_parent_cpp_sdk}")
    else()
        # Fallback to relative path
        set(LOGOS_CPP_SDK_ROOT "${REAL_SOURCE_DIR}/../../logos-cpp-sdk")
    endif()
endif()

# Allow override for logos-liblogos (for nix builds)
if(NOT DEFINED LOGOS_LIBLOGOS_ROOT)
    get_filename_component(REAL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
    set(_parent_liblogos "${REAL_SOURCE_DIR}/../../logos-liblogos")
    if(EXISTS "${_parent_liblogos}/interface.h")
        set(LOGOS_LIBLOGOS_ROOT "${_parent_liblogos}")
    else()
        set(LOGOS_LIBLOGOS_ROOT "${REAL_SOURCE_DIR}/../../logos-liblogos")
    endif()
endif()

# Check if logos-liblogos is available
set(_liblogos_found FALSE)
if(EXISTS "${LOGOS_LIBLOGOS_ROOT}/interface.h")
    set(_liblogos_found TRUE)
    set(_liblogos_is_source TRUE)
elseif(EXISTS "${LOGOS_LIBLOGOS_ROOT}/include/interface.h")
    set(_liblogos_found TRUE)
    set(_liblogos_is_source FALSE)
endif()

if(_liblogos_found)
    message(STATUS "Using logos-liblogos at: ${LOGOS_LIBLOGOS_ROOT}")
    message(STATUS "logos-liblogos layout: ${_liblogos_is_source}")
endif()

# Check if dependencies are available (support both source and installed layouts)
set(_cpp_sdk_found FALSE)
if(EXISTS "${LOGOS_CPP_SDK_ROOT}/cpp/logos_api.h")
    set(_cpp_sdk_found TRUE)
    set(_cpp_sdk_is_source TRUE)
elseif(EXISTS "${LOGOS_CPP_SDK_ROOT}/include/cpp/logos_api.h")
    set(_cpp_sdk_found TRUE)
    set(_cpp_sdk_is_source FALSE)
elseif(EXISTS "${LOGOS_CPP_SDK_ROOT}/lib/liblogos_sdk.a" OR EXISTS "${LOGOS_CPP_SDK_ROOT}/lib/liblogos_sdk.dylib" OR EXISTS "${LOGOS_CPP_SDK_ROOT}/lib/liblogos_sdk.so")
    # If we have the SDK library but no source files, it's definitely installed layout
    set(_cpp_sdk_found TRUE)
    set(_cpp_sdk_is_source FALSE)
endif()

if(NOT _cpp_sdk_found)
    message(FATAL_ERROR "logos-cpp-sdk not found at ${LOGOS_CPP_SDK_ROOT}. "
                        "Set LOGOS_CPP_SDK_ROOT or run git submodule update --init --recursive.")
endif()

message(STATUS "Using logos-cpp-sdk at: ${LOGOS_CPP_SDK_ROOT}")
message(STATUS "logos-cpp-sdk layout: ${_cpp_sdk_is_source}")

# Try to find the component-interfaces package first
find_package(component-interfaces QUIET)

# If not found, use the app/interfaces folder
if(NOT component-interfaces_FOUND)
    # Include the app/interfaces directory
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../app/interfaces)
    
    # Create a component-interfaces library
    add_library(component-interfaces INTERFACE)
    target_include_directories(component-interfaces INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../app/interfaces)
endif()

# Shared utilities (LogosAppPaths, etc.)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../app/utils)

set(SOURCES
    main_ui_plugin.cpp
    MainContainer.cpp
    MainUIBackend.cpp
    LogosQmlBridge.cpp
    restricted/DenyAllReply.cpp
    restricted/DenyAllNetworkAccessManager.cpp
    restricted/DenyAllNAMFactory.cpp
    restricted/RestrictedUrlInterceptor.cpp
    mdichild.cpp
    mdiview.cpp
    main_ui_resources.qrc
)

# Add SDK sources based on layout type
if(_cpp_sdk_is_source)
    list(APPEND SOURCES
        ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api.cpp
        ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api.h
        ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api_client.cpp
        ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api_client.h
        ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api_consumer.cpp
        ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api_consumer.h
        ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api_provider.cpp
        ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api_provider.h
        ${LOGOS_CPP_SDK_ROOT}/cpp/token_manager.cpp
        ${LOGOS_CPP_SDK_ROOT}/cpp/token_manager.h
        ${LOGOS_CPP_SDK_ROOT}/cpp/module_proxy.cpp
        ${LOGOS_CPP_SDK_ROOT}/cpp/module_proxy.h
    )
endif()

# Use umbrella source that includes all generated wrappers to avoid duplicate symbols
if(_cpp_sdk_is_source)
    set(GENERATED_LOGOS_SDK_DIR ${LOGOS_CPP_SDK_ROOT}/cpp/generated)
else()
    # For nix builds, generated files are in the source directory
    set(GENERATED_LOGOS_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/generated_code)
endif()
set(GENERATED_LOGOS_SDK_CPP ${GENERATED_LOGOS_SDK_DIR}/logos_sdk.cpp)
set(GENERATED_LOGOS_SDK_H   ${GENERATED_LOGOS_SDK_DIR}/logos_sdk.h)

# Only add logos_sdk.cpp (umbrella that includes all module implementations)
# Do not add individual *_api.cpp files as they are included by logos_sdk.cpp
if(EXISTS ${GENERATED_LOGOS_SDK_CPP})
    set_source_files_properties(${GENERATED_LOGOS_SDK_CPP} PROPERTIES 
        GENERATED TRUE
        SKIP_AUTOMOC ON
        SKIP_AUTOUIC ON
        SKIP_AUTORCC ON
    )
    list(APPEND SOURCES ${GENERATED_LOGOS_SDK_CPP})
endif()

# Run Logos C++ generator on metadata before compilation (only for source layout)
set(METADATA_JSON "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json")

# Only run generator for source layout - nix builds already have generated files
if(_cpp_sdk_is_source)
    # Source layout: build and run the generator
    set(REPO_ROOT "${REAL_SOURCE_DIR}/../..")
    set(CPP_GENERATOR "${REPO_ROOT}/build/cpp-generator/bin/logos-cpp-generator")
    set(PLUGINS_OUTPUT_DIR "${REPO_ROOT}/modules/build/modules")
    
    if(APPLE)
        set(PLUGIN_SUFFIX ".dylib")
    elseif(WIN32)
        set(PLUGIN_SUFFIX ".dll")
    else()
        set(PLUGIN_SUFFIX ".so")
    endif()

    # Always run the generator before building this plugin so new wrappers (e.g. package_manager) are produced
    add_custom_target(run_cpp_generator_main_ui
        COMMAND "${CPP_GENERATOR}" --metadata "${METADATA_JSON}" --module-dir "${PLUGINS_OUTPUT_DIR}"
        WORKING_DIRECTORY "${REPO_ROOT}"
        DEPENDS ${METADATA_JSON}
        COMMENT "Running logos-cpp-generator for main_ui"
        VERBATIM
    )
endif()

add_library(main_ui SHARED ${SOURCES})

# Ensure generator runs before building the plugin (only for source layout)
if(_cpp_sdk_is_source)
    add_dependencies(main_ui run_cpp_generator_main_ui)
endif()

# Add include directories before target_link_libraries
target_include_directories(main_ui PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Add SDK include directories based on layout type
if(_cpp_sdk_is_source)
    target_include_directories(main_ui PRIVATE 
        ${LOGOS_CPP_SDK_ROOT}/cpp
        ${LOGOS_CPP_SDK_ROOT}/cpp/generated
    )
else()
    target_include_directories(main_ui PRIVATE 
        ${LOGOS_CPP_SDK_ROOT}/include
        ${LOGOS_CPP_SDK_ROOT}/include/cpp
        ${LOGOS_CPP_SDK_ROOT}/include/core
        ${GENERATED_LOGOS_SDK_DIR}
    )
endif()

# Add logos-liblogos include directories if available
if(_liblogos_found)
    if(_liblogos_is_source)
        target_include_directories(main_ui PRIVATE 
            ${LOGOS_LIBLOGOS_ROOT}
            ${LOGOS_LIBLOGOS_ROOT}/host
        )
        target_link_directories(main_ui PRIVATE ${LOGOS_LIBLOGOS_ROOT}/build/lib)
    else()
        target_include_directories(main_ui PRIVATE ${LOGOS_LIBLOGOS_ROOT}/include)
        target_link_directories(main_ui PRIVATE ${LOGOS_LIBLOGOS_ROOT}/lib)
    endif()
endif()

# Option for distributed builds (DMG/AppImage)
option(LOGOS_DISTRIBUTED_BUILD "Build for distribution (DMG/AppImage)" OFF)
option(LOGOS_PORTABLE_BUILD "Build as a portable/distributed app" OFF)

# Add Qt definitions
target_compile_definitions(main_ui PRIVATE
    QT_PLUGIN
    QT_DEPRECATED_WARNINGS
)

# Add distributed build flag if enabled
if(LOGOS_DISTRIBUTED_BUILD)
    target_compile_definitions(main_ui PRIVATE LOGOS_DISTRIBUTED_BUILD)
    message(STATUS "Building main_ui for distribution (LOGOS_DISTRIBUTED_BUILD enabled)")
endif()

if(LOGOS_PORTABLE_BUILD)
    target_compile_definitions(main_ui PRIVATE LOGOS_PORTABLE_BUILD)
    message(STATUS "Building main_ui as portable (LOGOS_PORTABLE_BUILD enabled)")
endif()

target_link_libraries(main_ui 
    PRIVATE 
        Qt6::Core 
        Qt6::Widgets
        Qt6::RemoteObjects
        Qt6::Quick
        Qt6::Qml
        Qt6::QuickWidgets
        Qt6::QuickControls2
        component-interfaces
)

# Link logos_core library if available (for logos_core_get_module_stats)
if(_liblogos_found)
    target_link_libraries(main_ui PRIVATE logos_core)
endif()

# Link SDK library if using installed layout
if(NOT _cpp_sdk_is_source)
    find_library(LOGOS_SDK_LIB logos_sdk PATHS ${LOGOS_CPP_SDK_ROOT}/lib NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(main_ui PRIVATE ${LOGOS_SDK_LIB})
endif()

# For macOS, we need to set the bundle properties
if(APPLE)
    set_target_properties(main_ui PROPERTIES
        BUNDLE FALSE
        FRAMEWORK FALSE
        PREFIX ""
        SUFFIX ".dylib"
    )
else()
    set_target_properties(main_ui PROPERTIES
        PREFIX ""
        SUFFIX ".so"
    )
endif() 
