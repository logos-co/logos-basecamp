# mock/ — run Basecamp against a fixture instead of a live Logos runtime.
#
# Builds FixtureCoreRuntime (the ICoreRuntime implementation) and
# MockBackendFixture (resolves the fixture, exports LOGOS_MOCK_FIXTURE).
# See mock/README.md.

cmake_minimum_required(VERSION 3.21)

find_package(Qt6 REQUIRED COMPONENTS Core)

qt_add_library(logos_mock_backend STATIC
    src/MockBackendFixture.cpp
    src/MockBackendFixture.h
    src/FixtureCoreRuntime.cpp
    src/FixtureCoreRuntime.h
)

set_target_properties(logos_mock_backend PROPERTIES AUTOMOC ON)

# The fixture is compiled in as ":/mock/mock-backend.json". install() expands
# its {PLUGINS_DIR} / {LIB_EXT} placeholders and writes the RESOLVED result to
# the user data dir — plugin images read that file and have no idea where this
# application installed anything, so they cannot be handed placeholders.
qt_add_resources(logos_mock_backend "logos_mock_fixture"
    PREFIX "/mock"
    BASE "fixtures"
    FILES fixtures/mock-backend.json
)

target_include_directories(logos_mock_backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # ICoreRuntime.h — the interface this implements.
    ${CMAKE_CURRENT_SOURCE_DIR}/../app/interfaces
    # LogosBasecampPaths.h — placeholder expansion resolves through the same
    # helpers the app uses, so a fixture path cannot drift from where the
    # bundle actually stages things.
    ${CMAKE_CURRENT_SOURCE_DIR}/../app/utils
)

# Reaches app/main.cpp, which branches on it to call install().
target_compile_definitions(logos_mock_backend PUBLIC LOGOS_MOCK_BACKEND=1)

target_link_libraries(logos_mock_backend PUBLIC Qt6::Core)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
