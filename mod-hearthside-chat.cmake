if(TARGET modules)
    # Include nlohmann/json library (bundled, namespaced as hs_nlohmann via src/hs_json.h)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/deps/nlohmann/json.hpp")
        target_include_directories(modules BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/deps)
        message(STATUS "[mod-hearthside-chat] Using bundled nlohmann/json (namespaced as hs_nlohmann)")
    else()
        message(FATAL_ERROR "[mod-hearthside-chat] nlohmann/json not found.\n"
                          "  Please place json.hpp in deps/nlohmann/json.hpp")
    endif()

    # Include fmt library
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

    # Include cpp-httplib (header-only, bundled, namespaced as hs_httplib at each call site)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/deps/yhirose/cpp-httplib/httplib.h")
        target_include_directories(modules BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/deps/yhirose/cpp-httplib)
        message(STATUS "[mod-hearthside-chat] Using bundled cpp-httplib")
    else()
        message(FATAL_ERROR "[mod-hearthside-chat] cpp-httplib not found.\n"
                          "  Please place httplib.h in deps/yhirose/cpp-httplib/httplib.h")
    endif()

    # Enable SSL/TLS support (needed if the LLM endpoint or a future provider is HTTPS)
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND OR OPENSSL_FOUND)
        target_compile_definitions(modules PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
        target_link_libraries(modules PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        message(STATUS "[mod-hearthside-chat] OpenSSL found - HTTPS support enabled")
    else()
        message(WARNING "[mod-hearthside-chat] OpenSSL not found - only HTTP (no HTTPS) available")
    endif()

    # Platform-specific threading and networking
    if(WIN32)
        target_link_libraries(modules PRIVATE ws2_32 crypt32)
    else()
        target_link_libraries(modules PRIVATE pthread)
    endif()
endif()
