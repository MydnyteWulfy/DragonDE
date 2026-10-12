# Shared compile settings. Link `dragon::options` privately into every DragonDE target.

add_library(dragon_options INTERFACE)
add_library(dragon::options ALIAS dragon_options)

target_compile_options(dragon_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnull-dereference -Wimplicit-fallthrough
    # Designated initializers that leave fields zeroed are the intended style
    # for Vulkan/libwayland structs; -Wextra would flag every one of them.
    -Wno-missing-field-initializers
    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor -Woverloaded-virtual>
    $<$<CONFIG:Debug>:-g3 -fno-omit-frame-pointer>
)

if(DRAGON_SANITIZE)
    set(_dragon_san -fsanitize=address,undefined -fno-sanitize-recover=undefined)
    target_compile_options(dragon_options INTERFACE $<$<CONFIG:Debug>:${_dragon_san}>)
    target_link_options(dragon_options INTERFACE $<$<CONFIG:Debug>:${_dragon_san}>)
endif()
