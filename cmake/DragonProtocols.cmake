# Protocol code generation.
#
#   dragon_add_protocol(<target> <xml> [CORE] [SKIP iface...])
#
# For each XML, wayland-scanner produces the C server header and interface
# tables, then dragon-scanner produces C++ classes on top. Generated files land
# in ${DRAGON_PROTOCOL_DIR}; include them as "dragon-protocols/<name>.h".
#
# CORE marks wayland.xml itself: its C header and interface tables ship with
# libwayland-server, so only the C++ layer is generated.

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
pkg_get_variable(WAYLAND_XML_DIR wayland-scanner pkgdatadir)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

set(DRAGON_PROTOCOL_ROOT ${CMAKE_BINARY_DIR}/generated)
set(DRAGON_PROTOCOL_DIR ${DRAGON_PROTOCOL_ROOT}/dragon-protocols)
file(MAKE_DIRECTORY ${DRAGON_PROTOCOL_DIR})

function(dragon_add_protocol target xml)
    cmake_parse_arguments(ARG "CORE" "" "SKIP" ${ARGN})
    get_filename_component(base ${xml} NAME_WE)
    set(out ${DRAGON_PROTOCOL_DIR})

    if(ARG_CORE)
        set(c_header wayland-server-protocol.h)
    else()
        set(c_header ${base}-server-protocol.h)
        add_custom_command(
            OUTPUT ${out}/${c_header} ${out}/${base}-protocol.c
            COMMAND ${WAYLAND_SCANNER} server-header ${xml} ${out}/${c_header}
            COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${out}/${base}-protocol.c
            DEPENDS ${xml}
            COMMENT "wayland-scanner ${base}"
            VERBATIM)
        target_sources(${target} PRIVATE ${out}/${c_header} ${out}/${base}-protocol.c)
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
