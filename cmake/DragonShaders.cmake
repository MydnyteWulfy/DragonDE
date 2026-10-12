# GLSL -> SPIR-V at build time, embedded as uint32 arrays.
#
#   dragon_add_shaders(<target> <shader>...)
#
# Each shader `foo.vert` becomes `${DRAGON_SHADER_DIR}/foo.vert.spv.inc`, a
# comma-separated word list to include inside an array initializer:
#
#   static const uint32_t kFooVert[] = {
#   #include "shaders/foo.vert.spv.inc"
#   };

find_program(GLSLC glslc REQUIRED)

set(DRAGON_SHADER_ROOT ${CMAKE_BINARY_DIR}/generated)
set(DRAGON_SHADER_DIR ${DRAGON_SHADER_ROOT}/shaders)
file(MAKE_DIRECTORY ${DRAGON_SHADER_DIR})

function(dragon_add_shaders target)
    foreach(src ${ARGN})
        get_filename_component(abs ${src} ABSOLUTE)
        get_filename_component(name ${src} NAME)
        set(out ${DRAGON_SHADER_DIR}/${name}.spv.inc)
        add_custom_command(
            OUTPUT ${out}
            COMMAND ${GLSLC} --target-env=vulkan1.3 -O -Werror -mfmt=num -o ${out} ${abs}
            DEPENDS ${abs}
            COMMENT "glslc ${name}"
            VERBATIM)
        target_sources(${target} PRIVATE ${out})
    endforeach()
    target_include_directories(${target} PRIVATE ${DRAGON_SHADER_ROOT})
endfunction()
