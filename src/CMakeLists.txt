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

# Allow override for logos-module (provides interface.h)
if(NOT DEFINED LOGOS_MODULE_ROOT)
    get_filename_component(REAL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
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

# Allow override for logos-liblogos (provides liblogos_core library)
if(NOT DEFINED LOGOS_LIBLOGOS_ROOT)
    get_filename_component(REAL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
    set(LOGOS_LIBLOGOS_ROOT "${REAL_SOURCE_DIR}/../../logos-liblogos")
endif()

if(NOT DEFINED LOGOS_VIEW_MODULE_RUNTIME_ROOT)
    get_filename_component(REAL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
    set(_candidate "${REAL_SOURCE_DIR}/../../logos-view-module-runtime")
    if(EXISTS "${_candidate}/include/ViewModuleHost.h")
        set(LOGOS_VIEW_MODULE_RUNTIME_ROOT "${_candidate}")
    endif()
endif()
if(NOT DEFINED LOGOS_VIEW_MODULE_RUNTIME_ROOT OR NOT EXISTS "${LOGOS_VIEW_MODULE_RUNTIME_ROOT}/include/ViewModuleHost.h")
    message(FATAL_ERROR "LOGOS_VIEW_MODULE_RUNTIME_ROOT not found. Set it explicitly or provide logos-view-module-runtime as a sibling checkout.")
endif()
message(STATUS "Using logos-view-module-runtime at: ${LOGOS_VIEW_MODULE_RUNTIME_ROOT}")

set(_liblogos_found FALSE)
if(EXISTS "${LOGOS_LIBLOGOS_ROOT}/src/logos_core/logos_core.h")
    set(_liblogos_found TRUE)
    set(_liblogos_is_source TRUE)
elseif(EXISTS "${LOGOS_LIBLOGOS_ROOT}/include/logos_core.h")
    set(_liblogos_found TRUE)
    set(_liblogos_is_source FALSE)
endif()

if(_liblogos_found)
    message(STATUS "Using logos-liblogos at: ${LOGOS_LIBLOGOS_ROOT}")
endif()

# The SDK stack since the Qt split: logos-qt-sdk carries the Qt developer
# layer (LogosAPI etc.) this app uses; logos-protocol the transports; the
# Qt-free logos-cpp-sdk only std headers.
if(NOT DEFINED LOGOS_PROTOCOL_ROOT AND DEFINED ENV{LOGOS_PROTOCOL_ROOT})
    set(LOGOS_PROTOCOL_ROOT "$ENV{LOGOS_PROTOCOL_ROOT}")
endif()
if(NOT DEFINED LOGOS_QT_SDK_ROOT AND DEFINED ENV{LOGOS_QT_SDK_ROOT})
    set(LOGOS_QT_SDK_ROOT "$ENV{LOGOS_QT_SDK_ROOT}")
endif()
set(_cpp_sdk_found FALSE)
if(EXISTS "${LOGOS_QT_SDK_ROOT}/cpp/logos_api.h")
    set(_cpp_sdk_found TRUE)
    set(_cpp_sdk_is_source TRUE)
elseif(EXISTS "${LOGOS_QT_SDK_ROOT}/include/cpp/logos_api.h")
    set(_cpp_sdk_found TRUE)
    set(_cpp_sdk_is_source FALSE)
endif()

if(NOT _cpp_sdk_found)
    message(FATAL_ERROR "logos-qt-sdk not found at ${LOGOS_QT_SDK_ROOT}. "
                        "Set LOGOS_QT_SDK_ROOT (and LOGOS_PROTOCOL_ROOT).")
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

# Shared utilities (LogosBasecampPaths, etc.)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../app/utils)

set(SOURCES
    main_ui_plugin.cpp
    MainContainer.cpp
    MainUIBackend.cpp
    CoreModuleManager.cpp
    UIPluginManager.cpp
    PackageCoordinator.cpp
    AppsModel.cpp
    AppsFilterProxy.cpp
    InstallStage.cpp
    PluginLoader.cpp
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
        ${LOGOS_QT_SDK_ROOT}/cpp/logos_api.cpp
        ${LOGOS_QT_SDK_ROOT}/cpp/logos_api.h
        ${LOGOS_QT_SDK_ROOT}/cpp/logos_api_provider.cpp
        ${LOGOS_QT_SDK_ROOT}/cpp/logos_api_provider.h
        ${LOGOS_QT_SDK_ROOT}/cpp/logos_provider_object.cpp
        ${LOGOS_QT_SDK_ROOT}/cpp/logos_provider_object.h
        ${LOGOS_QT_SDK_ROOT}/cpp/qt_provider_object.cpp
        ${LOGOS_QT_SDK_ROOT}/cpp/qt_provider_object.h
    )
endif()

# Use umbrella source that includes all generated wrappers to avoid duplicate symbols
if(_cpp_sdk_is_source)
    set(GENERATED_LOGOS_SDK_DIR ${LOGOS_QT_SDK_ROOT}/cpp/generated)
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
        ${LOGOS_QT_SDK_ROOT}/cpp
        ${LOGOS_QT_SDK_ROOT}/core
        ${LOGOS_PROTOCOL_ROOT}/cpp
    )
else()
    target_include_directories(main_ui PRIVATE 
        ${LOGOS_CPP_SDK_ROOT}/include
        ${LOGOS_CPP_SDK_ROOT}/include/cpp
        ${LOGOS_QT_SDK_ROOT}/include
        ${LOGOS_QT_SDK_ROOT}/include/cpp
        ${LOGOS_QT_SDK_ROOT}/include/core
        ${LOGOS_PROTOCOL_ROOT}/include
        ${LOGOS_PROTOCOL_ROOT}/include/cpp
        ${GENERATED_LOGOS_SDK_DIR}
    )
endif()

# Add logos-module include directories (for interface.h)
if(_logos_module_found)
    if(_logos_module_is_source)
        target_include_directories(main_ui PRIVATE ${LOGOS_MODULE_ROOT}/src)
    else()
        target_include_directories(main_ui PRIVATE ${LOGOS_MODULE_ROOT}/include/module_lib)
    endif()
endif()

# Add logos-liblogos link directories if available (for logos_core library)
if(_liblogos_found)
    if(_liblogos_is_source)
        target_link_directories(main_ui PRIVATE ${LOGOS_LIBLOGOS_ROOT}/build/lib)
    else()
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

target_include_directories(main_ui PRIVATE ${LOGOS_VIEW_MODULE_RUNTIME_ROOT}/include)
find_library(LOGOS_VIEW_MODULE_RUNTIME_LIB
    NAMES logos_view_module_runtime
    PATHS ${LOGOS_VIEW_MODULE_RUNTIME_ROOT}/lib
    NO_DEFAULT_PATH
    REQUIRED)

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
        ${LOGOS_VIEW_MODULE_RUNTIME_LIB}
)


# Link logos_core library if available (for logos_core_get_module_stats)
if(_liblogos_found)
    target_link_libraries(main_ui PRIVATE logos_core)
endif()

# Link the SDK via its exported CMake target so the consumer inherits
# INTERFACE_LINK_LIBRARIES (OpenSSL::SSL, OpenSSL::Crypto, Boost::system,
# nlohmann_json). Bare `find_library` only puts liblogos_sdk.a on the
# link line — every Boost.Asio TLS symbol it pulls in (X509_check_host,
# SSL_*, ...) ends up undefined at link time on Linux as soon as the
# SDK adds any code path that references the ssl transports.
if(NOT _cpp_sdk_is_source)
    find_package(logos-protocol REQUIRED CONFIG
        PATHS ${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol
        NO_DEFAULT_PATH)
    find_package(logos-qt-sdk REQUIRED CONFIG
        PATHS ${LOGOS_QT_SDK_ROOT}/lib/cmake/logos-qt-sdk
        NO_DEFAULT_PATH)
    target_link_libraries(main_ui PRIVATE logos-qt-sdk::logos_qt_sdk)
else()
    if(EXISTS "${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol")
        find_package(logos-protocol REQUIRED CONFIG
            PATHS ${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol
            NO_DEFAULT_PATH)
        target_link_libraries(main_ui PRIVATE logos-protocol::logos_protocol)
    elseif(EXISTS "${LOGOS_PROTOCOL_ROOT}/cpp/CMakeLists.txt")
        if(NOT TARGET logos_protocol)
            add_subdirectory("${LOGOS_PROTOCOL_ROOT}/cpp"
                             "${CMAKE_BINARY_DIR}/logos-protocol-build")
        endif()
        target_link_libraries(main_ui PRIVATE logos_protocol)
    endif()
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
