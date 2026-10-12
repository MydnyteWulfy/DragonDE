# Protocol code generation.
#
#   dragon_add_protocol(<target> <xml> [CORE] [SKIP iface...])
#       Server side: C server header (wayland-scanner) + C++ classes
#       (dragon-scanner). Include as "dragon-protocols/<name>.h".
#
#   dragon_add_client_protocol(<target> <xml>)
#       Client side (the nested backend talks to its parent compositor):
#       C client header only. Include as <name>-client-protocol.h.
#
# Both sides share one copy of each protocol's wl_interface tables, compiled
# into `dragon_protocol_tables` (created by the caller before first use).
#
# CORE marks wayland.xml itself: its headers and tables ship with libwayland.

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
pkg_get_variable(WAYLAND_XML_DIR wayland-scanner pkgdatadir)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

set(DRAGON_PROTOCOL_ROOT ${CMAKE_BINARY_DIR}/generated)
set(DRAGON_PROTOCOL_DIR ${DRAGON_PROTOCOL_ROOT}/dragon-protocols)
file(MAKE_DIRECTORY ${DRAGON_PROTOCOL_DIR})

# Adds <xml>'s interface tables to dragon_protocol_tables, once.
function(_dragon_protocol_tables xml base)
    get_property(done GLOBAL PROPERTY _DRAGON_TABLES_${base})
    if(done)
        return()
    endif()
    set_property(GLOBAL PROPERTY _DRAGON_TABLES_${base} TRUE)

    set(out ${DRAGON_PROTOCOL_DIR}/${base}-protocol.c)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${out}
        DEPENDS ${xml}
        COMMENT "wayland-scanner tables ${base}"
        VERBATIM)
    target_sources(dragon_protocol_tables PRIVATE ${out})
endfunction()

function(dragon_add_protocol target xml)
    cmake_parse_arguments(ARG "CORE" "" "SKIP" ${ARGN})
    get_filename_component(base ${xml} NAME_WE)
    set(out ${DRAGON_PROTOCOL_DIR})

    if(ARG_CORE)
        set(c_header wayland-server-protocol.h)
    else()
        set(c_header ${base}-server-protocol.h)
        add_custom_command(
            OUTPUT ${out}/${c_header}
            COMMAND ${WAYLAND_SCANNER} server-header ${xml} ${out}/${c_header}
            DEPENDS ${xml}
            COMMENT "wayland-scanner server-header ${base}"
            VERBATIM)
        target_sources(${target} PRIVATE ${out}/${c_header})
        _dragon_protocol_tables(${xml} ${base})
    endif()

    set(skip_args "")
    if(ARG_SKIP)
        list(JOIN ARG_SKIP "," skip_list)
        set(skip_args --skip ${skip_list})
    endif()

    add_custom_command(
        OUTPUT ${out}/${base}.h ${out}/${base}.cpp
        COMMAND dragon-scanner ${xml} ${out}/${base}.h ${out}/${base}.cpp --c-header ${c_header} ${skip_args}
        DEPENDS dragon-scanner ${xml}
        COMMENT "dragon-scanner ${base}"
        VERBATIM)
    target_sources(${target} PRIVATE ${out}/${base}.h ${out}/${base}.cpp)
endfunction()

function(dragon_add_client_protocol target xml)
    get_filename_component(base ${xml} NAME_WE)
    set(header ${DRAGON_PROTOCOL_DIR}/${base}-client-protocol.h)
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${header}
        DEPENDS ${xml}
        COMMENT "wayland-scanner client-header ${base}"
        VERBATIM)
    target_sources(${target} PRIVATE ${header})
    _dragon_protocol_tables(${xml} ${base})
endfunction()
