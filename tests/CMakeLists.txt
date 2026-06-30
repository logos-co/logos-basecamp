cmake_minimum_required(VERSION 3.16)
project(LogosBasecampUnitTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Qml Test)

enable_testing()

set(BASECAMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_executable(module_model_test
    module_model_test.cpp
    ${BASECAMP_SRC}/ModuleModel.cpp
    ${BASECAMP_SRC}/InstallRegistry.cpp
    ${BASECAMP_SRC}/InstallEnums.cpp
)
target_include_directories(module_model_test PRIVATE ${BASECAMP_SRC})
target_link_libraries(module_model_test PRIVATE
    Qt6::Core
    Qt6::Qml     # InstallEnums.h pulls in <QtQml/qqml.h>
    Qt6::Test
)

add_test(NAME module_model_test COMMAND module_model_test)

add_executable(module_filter_proxy_test
    module_filter_proxy_test.cpp
    ${BASECAMP_SRC}/ModuleModel.cpp
    ${BASECAMP_SRC}/ModuleFilterProxy.cpp
    ${BASECAMP_SRC}/InstallRegistry.cpp
    ${BASECAMP_SRC}/InstallEnums.cpp
)
target_include_directories(module_filter_proxy_test PRIVATE ${BASECAMP_SRC})
target_link_libraries(module_filter_proxy_test PRIVATE
    Qt6::Core
    Qt6::Qml
    Qt6::Test
)
add_test(NAME module_filter_proxy_test COMMAND module_filter_proxy_test)

add_executable(repository_model_test
    repository_model_test.cpp
    ${BASECAMP_SRC}/RepositoryModel.cpp
)
target_include_directories(repository_model_test PRIVATE ${BASECAMP_SRC})
target_link_libraries(repository_model_test PRIVATE
    Qt6::Core
    Qt6::Test
)
add_test(NAME repository_model_test COMMAND repository_model_test)

add_executable(install_plugin_success_test
    install_plugin_success_test.cpp
)
target_link_libraries(install_plugin_success_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME install_plugin_success_test COMMAND install_plugin_success_test)
