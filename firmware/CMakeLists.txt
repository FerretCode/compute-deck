cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(q10_keyboard_project C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

pico_sdk_init()

add_executable(q10_keyboard
    main.c
    i2c_multi.c
)

pico_generate_pio_header(q10_keyboard ${CMAKE_CURRENT_LIST_DIR}/i2c_multi.pio)

target_link_libraries(q10_keyboard
    pico_stdlib
    hardware_i2c
    hardware_pio
    hardware_irq
)

pico_enable_stdio_usb(q10_keyboard 1)
pico_enable_stdio_uart(q10_keyboard 1)

pico_add_extra_outputs(q10_keyboard)
