cmake_minimum_required(VERSION 3.13)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

include(pico_sdk_import.cmake)
project(squarewave C CXX ASM)
pico_sdk_init()

add_executable(squarewave squarewave.c)
pico_generate_pio_header(squarewave ${CMAKE_CURRENT_LIST_DIR}/squarewave.pio)
target_link_libraries(squarewave PRIVATE pico_stdlib hardware_pio hardware_clocks)
pico_enable_stdio_usb(squarewave 1)
pico_enable_stdio_uart(squarewave 0)
pico_add_extra_outputs(squarewave)
