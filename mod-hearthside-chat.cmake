if(TARGET modules)
    # ------------------------------------------------------------------
    # Review E1 (2026-09-03): everything this module needs to compile is
    # scoped to *this module's own translation units*, not to the shared
    # `modules` target that every AzerothCore module compiles into.
    #
    # What this file used to do, and why it was wrong:
    #
    #   target_include_directories(modules BEFORE PRIVATE .../deps)
    #   target_include_directories(modules BEFORE PRIVATE .../deps/yhirose/cpp-httplib)
    #   target_compile_definitions(modules PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
    #
    # BEFORE put this module's deps/ ahead of every other module's include
    # path, so any other module writing `#include <nlohmann/json.hpp>` or
    # `#include <httplib.h>` resolved to *this* module's bundled copy -- and
    # critically without the renaming, because `#define nlohmann hs_nlohmann`
    # lives inside src/hs_json.h and `#define httplib hs_httplib` at each
    # call site. That is the same ODR/version-skew problem the namespacing
    # exists to prevent, arriving through the include path instead.
    #
    # And it was not hypothetical: mod-game-state-api vendors its own
    # nlohmann (3.12.0, same as ours) and its own cpp-httplib (0.22.0, ours
    # is 0.43.1). The target-wide CPPHTTPLIB_OPENSSL_SUPPORT was silently
    # compiling *that* module's 0.22.0 httplib with OpenSSL support it never
    # asked for, and the target-wide OpenSSL link meant it happily linked
    # rather than failing loudly.
    #
    # Source-file properties are the fix: they apply to exactly the files
    # listed and nothing else. This .cmake is include()d from
    # modules/CMakeLists.txt after both add_library(modules STATIC ...) and
    # the per-module add_library(... SHARED ...) calls, in the same
    # directory scope, so the properties reach this module's sources under
    # either the static or the dynamic linkage path.
    # ------------------------------------------------------------------
    file(GLOB HEARTHSIDE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")
    if(NOT HEARTHSIDE_SOURCES)
        message(FATAL_ERROR "[mod-hearthside-chat] no sources found under ${CMAKE_CURRENT_LIST_DIR}/src")
    endif()

    set(HEARTHSIDE_INCLUDES "")
    set(HEARTHSIDE_DEFINES "")

    # Bundled nlohmann/json, namespaced as hs_nlohmann via src/hs_json.h.
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/deps/nlohmann/json.hpp")
        list(APPEND HEARTHSIDE_INCLUDES "${CMAKE_CURRENT_LIST_DIR}/deps")
        message(STATUS "[mod-hearthside-chat] Using bundled nlohmann/json (namespaced as hs_nlohmann, scoped to this module)")
    else()
        message(FATAL_ERROR "[mod-hearthside-chat] nlohmann/json not found.\n"
                          "  Please place json.hpp in deps/nlohmann/json.hpp")
    endif()

    # Bundled cpp-httplib, namespaced as hs_httplib at each call site.
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/deps/yhirose/cpp-httplib/httplib.h")
        list(APPEND HEARTHSIDE_INCLUDES "${CMAKE_CURRENT_LIST_DIR}/deps/yhirose/cpp-httplib")
        message(STATUS "[mod-hearthside-chat] Using bundled cpp-httplib (scoped to this module)")
    else()
        message(FATAL_ERROR "[mod-hearthside-chat] cpp-httplib not found.\n"
                          "  Please place httplib.h in deps/yhirose/cpp-httplib/httplib.h")
    endif()

    # fmt: a link dependency, not an include-path or macro one, so it stays
    # target-wide. AzerothCore's own `common` already links fmt publicly, so
    # this adds nothing another module could observe.
    if(TARGET fmt)
        target_link_libraries(modules PRIVATE fmt)
        message(STATUS "[mod-hearthside-chat] Using AzerothCore fmt library")
    else()
        find_package(fmt CONFIG QUIET)
        if(fmt_FOUND)
            target_link_libraries(modules PRIVATE fmt::fmt)
            message(STATUS "[mod-hearthside-chat] Using system fmt library")
        else()
            message(FATAL_ERROR "[mod-hearthside-chat] fmt library not found.\n"
                              "  Ubuntu/Debian: sudo apt install libfmt-dev\n"
                              "  Or build AzerothCore with full deps")
        endif()
    endif()

    # SSL/TLS support (needed if the LLM endpoint or a future provider is
    # HTTPS). The *definition* is per-source -- it changes how httplib.h
    # compiles, and another module's vendored copy must not inherit it. The
    # *link* stays target-wide: link libraries cannot be scoped per source
    # file, and an extra library on the link line changes no other module's
    # code, unlike a macro that rewrites its headers.
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND OR OPENSSL_FOUND)
        list(APPEND HEARTHSIDE_DEFINES CPPHTTPLIB_OPENSSL_SUPPORT)
        target_link_libraries(modules PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        message(STATUS "[mod-hearthside-chat] OpenSSL found - HTTPS support enabled (scoped to this module)")
    else()
        message(WARNING "[mod-hearthside-chat] OpenSSL not found - only HTTP (no HTTPS) available")
    endif()

    set_source_files_properties(${HEARTHSIDE_SOURCES}
      PROPERTIES
        INCLUDE_DIRECTORIES "${HEARTHSIDE_INCLUDES}"
        COMPILE_DEFINITIONS "${HEARTHSIDE_DEFINES}")

    # Platform-specific threading and networking. Link-only, same reasoning
    # as the OpenSSL link above.
    if(WIN32)
        target_link_libraries(modules PRIVATE ws2_32 crypt32)
    else()
        target_link_libraries(modules PRIVATE pthread)
    endif()
endif()
