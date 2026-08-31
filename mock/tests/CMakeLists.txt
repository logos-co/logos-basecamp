# Tests for the fixture-resolution half of the mock.
#
# The logos_core_* ABI conformance test moved to logos-liblogos with the code
# it exercises. What is left checks the part that is still Basecamp's job:
# placeholder expansion against the real bundle layout, and writing a resolved
# fixture that other IMAGES (ui-host, plugins with their own static copies of
# logos-protocol) can read.

find_package(Qt6 REQUIRED COMPONENTS Core)

qt_add_executable(mock_fixture_test mock_fixture_test.cpp)
target_link_libraries(mock_fixture_test PRIVATE logos_mock_backend Qt6::Core)
add_test(NAME mock_fixture COMMAND mock_fixture_test)
