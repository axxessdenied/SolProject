add_library(sol_platform STATIC
    src/file_io.cpp
    src/input_bindings.cpp
)
add_library(sol::platform ALIAS sol_platform)

target_include_directories(sol_platform PUBLIC include)
target_link_libraries(sol_platform PUBLIC sol::core)

if(WIN32)
    target_sources(sol_platform PRIVATE
        src/win32/audio_win32.cpp
        src/win32/system_win32.cpp
        src/win32/window_win32.cpp
    )
    # WASAPI reaches the endpoint through COM (decisions/009 / Phase 8t).
    target_link_libraries(sol_platform PRIVATE ole32)
elseif(LINUX)
    # Phase 21. The directory is the OS and the file name is the backend, so an
    # X11 window could sit beside the Wayland one without either being renamed.
    #
    # libwayland, libxkbcommon and the cursor theme loader are SYSTEM libraries
    # in the sense AGENTS.md section 5 uses - the Linux counterparts of ole32,
    # with the same standing this project already gives the Vulkan loader. No
    # vendored code arrives with them and none is added to third_party/.
    enable_language(C)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET GLOBAL wayland-client)
    pkg_check_modules(WAYLAND_CURSOR REQUIRED IMPORTED_TARGET GLOBAL wayland-cursor)
    pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET GLOBAL xkbcommon)

    # ⚑ Wayland protocols ship as XML, not as headers. wayland-scanner turns
    # each one into a client header plus a C file of message dispatch tables,
    # and the C file must be COMPILED AND LINKED - the interface symbols
    # (`xdg_wm_base_interface` and friends) live there and nowhere else. A build
    # that generates only the headers links with undefined references naming
    # those symbols, which reads as a missing library rather than as a missing
    # codegen step.
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    if(NOT WAYLAND_PROTOCOLS_DIR)
        message(FATAL_ERROR "wayland-protocols is not installed (no pkgdatadir); install wayland-protocols")
    endif()

    set(waylandGeneratedDir ${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols)
    file(MAKE_DIRECTORY ${waylandGeneratedDir})
    set(waylandProtocolSources "")
    # xdg-shell is what makes a surface a WINDOW; the other two are what make
    # mouse-look possible (raw deltas, and a cursor that stays put).
    foreach(protocol IN ITEMS
        stable/xdg-shell/xdg-shell
        unstable/relative-pointer/relative-pointer-unstable-v1
        unstable/pointer-constraints/pointer-constraints-unstable-v1)
        get_filename_component(protocolName ${protocol} NAME)
        set(protocolXml ${WAYLAND_PROTOCOLS_DIR}/${protocol}.xml)
        if(NOT EXISTS ${protocolXml})
            message(FATAL_ERROR "Wayland protocol ${protocol}.xml not found under ${WAYLAND_PROTOCOLS_DIR}")
        endif()
        add_custom_command(
            OUTPUT ${waylandGeneratedDir}/${protocolName}-client-protocol.h
            COMMAND ${WAYLAND_SCANNER} client-header
                    ${protocolXml} ${waylandGeneratedDir}/${protocolName}-client-protocol.h
            DEPENDS ${protocolXml}
            COMMENT "wayland-scanner client-header ${protocolName}"
            VERBATIM)
        add_custom_command(
            OUTPUT ${waylandGeneratedDir}/${protocolName}-protocol.c
            COMMAND ${WAYLAND_SCANNER} private-code
                    ${protocolXml} ${waylandGeneratedDir}/${protocolName}-protocol.c
            DEPENDS ${protocolXml}
            COMMENT "wayland-scanner private-code ${protocolName}"
            VERBATIM)
        list(APPEND waylandProtocolSources
            ${waylandGeneratedDir}/${protocolName}-client-protocol.h
            ${waylandGeneratedDir}/${protocolName}-protocol.c)
    endforeach()

    # ⚑ Deliberately NOT linked to sol_options. This is machine output from
    # wayland-scanner and holds third_party's standing under AGENTS.md section
    # 5: it is not ours to keep warning-clean, and -Werror over generated C
    # would make a wayland-protocols upgrade able to break the build for
    # reasons no first-party file could be edited to fix.
    add_library(sol_wayland_protocols STATIC ${waylandProtocolSources})
    target_include_directories(sol_wayland_protocols PUBLIC ${waylandGeneratedDir})
    target_link_libraries(sol_wayland_protocols PUBLIC PkgConfig::WAYLAND_CLIENT)

    target_sources(sol_platform PRIVATE
        src/linux/audio_linux.cpp
        src/linux/system_linux.cpp
        src/linux/window_wayland.cpp
        src/linux/xkb_keys.cpp
    )
    target_link_libraries(sol_platform PRIVATE
        sol_wayland_protocols
        PkgConfig::WAYLAND_CLIENT
        PkgConfig::WAYLAND_CURSOR
        PkgConfig::XKBCOMMON
    )
else()
    message(FATAL_ERROR "Sol has no platform backend for this OS yet (Windows and Linux only).")
endif()
