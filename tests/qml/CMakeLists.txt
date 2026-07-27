cmake_minimum_required(VERSION 3.16)
project(LogosBasecampQmlTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickTest Test)

enable_testing()

set(BASECAMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src")

if(DEFINED LOGOS_PACKAGE_HEADERS)
    set(LOGOS_PACKAGE_HEADERS_INCLUDE "${LOGOS_PACKAGE_HEADERS}")
endif()

add_executable(qml_tests
    main.cpp
    ${BASECAMP_SRC}/AppsModel.cpp
    ${BASECAMP_SRC}/AppsFilterProxy.cpp
    ${BASECAMP_SRC}/InstallEnums.cpp
    ${BASECAMP_SRC}/InstallRegistry.cpp   # AppsModel depends on InstallRegistry
)
target_include_directories(qml_tests PRIVATE
    ${BASECAMP_SRC}
    ${LOGOS_PACKAGE_HEADERS_INCLUDE}
)
target_link_libraries(qml_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickTest
    Qt6::Test
)
target_compile_definitions(qml_tests PRIVATE
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

add_test(NAME qml_tests COMMAND qml_tests)
