# Windows: take the shared C++ runtime from liblogos_core.dll, not from the
# static archives.
#
# LogosBasecamp.exe and main_ui.dll run in ONE process, and both link
# liblogos_qt_host.a / liblogos_protocol.a (the Qt archive was liblogos_qt_sdk.a
# until the host runtime moved into logos-qt-host). On ELF and Mach-O that is
# harmless: liblogos_core exports the symbols and the loader interposes them, so
# there is one LogosAPI, one TokenManager, one of everything. PE has no
# interposition, so each image binds its own static copy and gets its own
# function-local statics.
# The visible symptom was 29 "ModuleProxy: rejecting unauthorized call" lines and
# no package_manager_ui in the sidebar: the host wrote the capability token into
# its TokenManager and the plugin read a different, empty one.
#
# liblogos_core.dll now exports those symbols explicitly (see
# logos-liblogos/src/CMakeLists.txt) and the consumers declare them
# __declspec(dllimport) via LOGOS_SHARED_USE_DLL. That alone is NOT enough,
# because ld picks archive members by object file for reasons that have nothing
# to do with our symbols: measured here, main_ui's AppsModel.cpp.obj referenced
# std::string's move constructor, ld satisfied it out of the Qt archive's
# logos_api.cpp.obj, and that one object dragged in LogosAPI, LogosAPIClient and
# TokenManager behind it -- each then colliding with the DLL's export
# ("multiple definition of `LogosAPI::LogosAPI'"). No export list can prevent
# that; the archive simply must not be a candidate.
#
# So the archives are replaced by an EMPTY one. Everything else the imported
# targets carry -- include directories, Qt/OpenSSL/Boost/nlohmann link
# interfaces, compile features -- is untouched, which is why this is done by
# repointing IMPORTED_LOCATION rather than by dropping the
# target_link_libraries() call and re-listing the usage requirements by hand.
#
# Off Windows this file is never included: ELF/Mach-O already resolve to the one
# provider, and there is nothing to fix.

function(logos_use_shared_runtime_from_dll)
    if(NOT WIN32)
        return()
    endif()

    # One empty archive per build tree, shared by every target that needs it.
    set(_stub "${CMAKE_BINARY_DIR}/logos_shared_from_dll_stub.a")
    if(NOT EXISTS "${_stub}")
        execute_process(
            COMMAND "${CMAKE_AR}" crs "${_stub}"
            RESULT_VARIABLE _ar_rc
            OUTPUT_QUIET ERROR_VARIABLE _ar_err)
        if(NOT _ar_rc EQUAL 0 OR NOT EXISTS "${_stub}")
            message(FATAL_ERROR
                "Could not create the empty archive that stands in for the "
                "logos static libraries on Windows: ${_ar_err}")
        endif()
    endif()

    # A target that is not there is a HARD ERROR, not a skip.
    #
    # This loop used to be `if(TARGET ...)`, which meant the one failure this
    # function has -- being handed a name that no longer resolves -- produced no
    # output at all. The build then succeeded, the archive stayed in the link,
    # and the result was the silent split-brain this whole file exists to
    # prevent: two TokenManagers in one process, a token written into one and
    # read from the other, and a UI that simply never appears. It cost a long
    # debugging session to find the first time. Every caller passes targets it
    # has just find_package'd, so there is no legitimate absent case.
    #
    # This is exactly the shape the qt-sdk -> qt-host move would have tripped:
    # `logos-qt-sdk::logos_qt_sdk` still exists, but it is an INTERFACE library
    # with no archive now, and the archive to empty out is
    # `logos-qt-host::logos_qt_host`.
    foreach(_tgt IN LISTS ARGN)
        if(NOT TARGET ${_tgt})
            message(FATAL_ERROR
                "logos_use_shared_runtime_from_dll: ${_tgt} is not a target. It must be "
                "an IMPORTED target whose archive can be replaced by the empty stand-in; "
                "skipping it would leave the static copy of the shared runtime in this "
                "image, which on Windows means a second TokenManager and refused "
                "cross-module calls at runtime, with nothing failing at build time.")
        endif()
        set_target_properties(${_tgt} PROPERTIES IMPORTED_LOCATION "${_stub}")
        message(STATUS "Windows: ${_tgt} provided by liblogos_core.dll, static archive suppressed")
    endforeach()
endfunction()
