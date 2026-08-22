cmake_minimum_required(VERSION 3.16)
project(LogosBasecampQmlTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickTest Test)

enable_testing()

# AppsModel and InstallRegistry are host-side; AppsFilterProxy and InstallEnums
# live in the main_ui shell. This suite covers the seam, so it builds from both.
set(BASECAMP_SRC   "${CMAKE_CURRENT_SOURCE_DIR}/../../app")
set(BASECAMP_SHELL "${CMAKE_CURRENT_SOURCE_DIR}/../../src")

if(DEFINED LOGOS_PACKAGE_HEADERS)
    set(LOGOS_PACKAGE_HEADERS_INCLUDE "${LOGOS_PACKAGE_HEADERS}")
endif()

add_executable(qml_tests
    main.cpp
    ${BASECAMP_SRC}/AppsModel.cpp
    ${BASECAMP_SRC}/InstallRegistry.cpp   # AppsModel depends on InstallRegistry
    ${BASECAMP_SHELL}/AppsFilterProxy.cpp
    ${BASECAMP_SHELL}/InstallEnums.cpp
    # AUTOMOC pairs a header with a .cpp of the same basename in the SAME
    # directory; InstallEnums.cpp is in src/ and its header in app/interfaces/,
    # so it must be listed or the link fails on InstallStage/InstallStatus'
    # staticMetaObject.
    ${BASECAMP_SRC}/interfaces/InstallEnums.h
)
target_include_directories(qml_tests PRIVATE
    ${BASECAMP_SRC}
    ${BASECAMP_SRC}/interfaces
    ${BASECAMP_SHELL}
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
