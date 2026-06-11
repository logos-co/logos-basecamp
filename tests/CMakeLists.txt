cmake_minimum_required(VERSION 3.16)
project(LogosBasecampUnitTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Qml Test)

enable_testing()

set(BASECAMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_executable(apps_model_test
    apps_model_test.cpp
    ${BASECAMP_SRC}/AppsModel.cpp
    ${BASECAMP_SRC}/InstallEnums.cpp
)
target_include_directories(apps_model_test PRIVATE ${BASECAMP_SRC})
target_link_libraries(apps_model_test PRIVATE
    Qt6::Core
    Qt6::Qml     # InstallEnums.h pulls in <QtQml/qqml.h>
    Qt6::Test
)

add_test(NAME apps_model_test COMMAND apps_model_test)

add_executable(apps_filter_proxy_test
    apps_filter_proxy_test.cpp
    ${BASECAMP_SRC}/AppsModel.cpp
    ${BASECAMP_SRC}/AppsFilterProxy.cpp
    ${BASECAMP_SRC}/InstallEnums.cpp
)
target_include_directories(apps_filter_proxy_test PRIVATE ${BASECAMP_SRC})
target_link_libraries(apps_filter_proxy_test PRIVATE
    Qt6::Core
    Qt6::Qml
    Qt6::Test
)
add_test(NAME apps_filter_proxy_test COMMAND apps_filter_proxy_test)

add_executable(install_plugin_success_test
    install_plugin_success_test.cpp
)
target_link_libraries(install_plugin_success_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME install_plugin_success_test COMMAND install_plugin_success_test)
