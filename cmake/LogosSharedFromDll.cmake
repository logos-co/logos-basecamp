# Take the shared C++ runtime from liblogos_core, not from the static archives.
#
# LogosBasecamp and every plugin it loads in-process share ONE process, and all
# of them link liblogos_qt_host.a / liblogos_protocol.a. Each image that links
# an archive gets its own copy of the code in it, and therefore its own copy of
# every function-local static inside it: TokenManager::instance, the per-identity
# StoreRegistry, the host-services grant, the deferred event-subscription
# registry. The host writes a capability token into its copy, another image reads
# its own empty one, and every cross-module call is refused at runtime -- with no
# build diagnostic anywhere.
#
# THIS IS NOT A WINDOWS-ONLY PROBLEM. An earlier version of this file asserted
# "ELF and Mach-O already resolve to the one provider, and there is nothing to
# fix" and returned early off Windows. That claim is false for Mach-O and was
# falsified by measurement:
#
#   * PE   -- no interposition at all. A second archive is always a second
#             singleton. Symptom measured here: 29 "ModuleProxy: rejecting
#             unauthorized call" lines and no package_manager_ui in the sidebar.
#   * Mach-O -- two-level namespace, so it behaves like PE, not like ELF. It
#             appears to work only while the consumer image has NO definition of
#             its own, so ld binds the undefined symbol to liblogos_core.dylib.
#             The moment any reference drags an archive member in, that image
#             gets its own copy silently. Measured when main_ui was folded into
#             the executable: ONE reference to LogosAPI::forIdentity pulled
#             logos_api.cpp.o and token_manager.cpp.o into the exe, which then
#             defined nine runtime entry points liblogos_core.dylib did not
#             export, and the app produced 31 refusals against a pre-fold
#             baseline of 0.
#   * ELF  -- flat namespace, first definition wins process-wide, so it really
#             does collapse. We empty the archives here anyway, so the invariant
#             is ONE rule on three platforms rather than three rules -- and so
#             the symbol gate can assert the same thing everywhere.
#
# Declaring the symbols __declspec(dllimport) (LOGOS_SHARED_USE_DLL, Windows
# only) is NOT sufficient on its own, because ld picks archive members by object
# file for reasons that have nothing to do with our symbols: measured here,
# main_ui's AppsModel.cpp.obj referenced std::string's move constructor, ld
# satisfied it out of liblogos_qt_sdk.a's logos_api.cpp.obj, and that one object
# dragged in LogosAPI, LogosAPIClient and TokenManager behind it. No export list
# can prevent that; the archive simply must not be a candidate.
#
# So the archives are replaced by an EMPTY one. Everything else the imported
# targets carry -- include directories, Qt/OpenSSL/Boost/nlohmann link
# interfaces, compile features -- is untouched, which is why this is done by
# repointing IMPORTED_LOCATION rather than by dropping the
# target_link_libraries() call and re-listing the usage requirements by hand.
#
# Once a consumer has no archive to fall back on, anything liblogos_core failed
# to include becomes an UNDEFINED REFERENCE at link time. That is the intended
# trade: a loud link error instead of a silent split-brain. The provider side
# guarantees the coverage -- see logos-liblogos/src/CMakeLists.txt.

function(logos_use_shared_runtime_from_dll)
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
                "logos static libraries: ${_ar_err}")
        endif()
    endif()

    foreach(_tgt IN LISTS ARGN)
        if(NOT TARGET ${_tgt})
            message(FATAL_ERROR
                "logos_use_shared_runtime_from_dll: ${_tgt} is not a target. It must be "
                "an IMPORTED target whose archive can be replaced by the empty stand-in; "
                "skipping it would leave a static copy of the shared runtime in this "
                "image alongside liblogos_core's exported one.")
        endif()
        set_target_properties(${_tgt} PROPERTIES IMPORTED_LOCATION "${_stub}")
        message(STATUS "${_tgt} provided by liblogos_core, static archive suppressed")
    endforeach()
endfunction()
