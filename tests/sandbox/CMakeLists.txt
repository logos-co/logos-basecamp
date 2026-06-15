cmake_minimum_required(VERSION 3.16)
project(LogosBasecampSandboxTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Qml Quick Network Test)

enable_testing()

# Path to the production basecamp sources under test. The test links the real
# sandbox code (QmlSandbox + the restricted/ helpers) so it exercises exactly
# what PluginLoader::loadQmlView runs — no re-implementation.
set(BASECAMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src")

# ---- The "evil" Qt QML plugin (attacker payload) -------------------------
# A genuine Qt plugin with valid metadata so Qt will dlopen() it when a qmldir
# 'plugin' directive resolves to it. Its constructor writes a sentinel file.
add_library(evil_plugin MODULE evil_plugin/evil_plugin.cpp)
target_link_libraries(evil_plugin PRIVATE Qt6::Core Qt6::Qml)
set_target_properties(evil_plugin PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/evil")
if(APPLE)
    set_target_properties(evil_plugin PROPERTIES SUFFIX ".so") # match Qt unix suffix probing
endif()

# Resolve the final plugin path for the test to copy into attacker dirs.
set(EVIL_PLUGIN_PATH "${CMAKE_CURRENT_BINARY_DIR}/evil/libevil_plugin.so")

# ---- The regression test -------------------------------------------------
add_executable(tst_qml_sandbox
    tst_qml_sandbox.cpp
    ${BASECAMP_SRC}/restricted/QmlSandbox.cpp
    ${BASECAMP_SRC}/restricted/RestrictedUrlInterceptor.cpp
    ${BASECAMP_SRC}/restricted/DenyAllNAMFactory.cpp
    ${BASECAMP_SRC}/restricted/DenyAllNetworkAccessManager.cpp
    ${BASECAMP_SRC}/restricted/DenyAllReply.cpp
)
target_include_directories(tst_qml_sandbox PRIVATE ${BASECAMP_SRC})
target_compile_definitions(tst_qml_sandbox PRIVATE
    EVIL_PLUGIN_PATH="${EVIL_PLUGIN_PATH}"
    EVIL_APP_DIR="${CMAKE_CURRENT_SOURCE_DIR}/evil_app")
target_link_libraries(tst_qml_sandbox PRIVATE
    Qt6::Core Qt6::Qml Qt6::Quick Qt6::Network Qt6::Test)
add_dependencies(tst_qml_sandbox evil_plugin)

# Register each test slot as its OWN ctest case (separate process). Qt caches
# loaded QML plugins process-wide (modulesForWhichPluginsHaveBeenLoaded), so
# running every slot in one process could let an earlier load mask a later
# escape. One process per slot guarantees each assertion stands alone. (Test
# modules also use unique names as a second layer of defense.)
foreach(slot
    evilPluginReallyExecutes_whenUnsandboxed
    sandboxBlocksNativePlugin_relativePath
    sandboxBlocksNativePlugin_absolutePath
    sandboxAllowsPureQmlModule
    sandboxDeniesNetworkAccess
    sandboxDeniesFileSchemeNetworkRequest
    sandboxBlocksRemoteSchemeUrl
    sandboxBlocksFileOutsideRoots
    sandboxAllowsFileUnderInstallDir
    sandboxPassesThroughNonExistentProbe
    sandboxAllowsQrcScheme
    evilApp_allProbesBlocked
    evilApp_qmldirPluginImportRejected)
  add_test(NAME qml_sandbox.${slot} COMMAND tst_qml_sandbox ${slot})
  # Headless: the test only compiles/instantiates QML types, no real window.
  set_tests_properties(qml_sandbox.${slot} PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()
