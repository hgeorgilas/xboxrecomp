# Xbox Input → XInput Compatibility Layer

add_library(xbox_input STATIC
    xinput_device.c
    keyboard.c          # the key table and the one key map (every host)
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    # DualShock 4 / DualSense over HID.
    target_sources(xbox_input PRIVATE hid_pads.c)
    # Mouse+keyboard pad (RECOMP_MKB) reads Win32 raw input.
    target_sources(xbox_input PRIVATE mkb_input.c)
    target_link_libraries(xbox_input PUBLIC platform xinput hid setupapi)
else()
    find_package(SDL2 REQUIRED)
    # The SDL window's key and focus events into that table.
    target_sources(xbox_input PRIVATE keyboard_sdl.c)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()
