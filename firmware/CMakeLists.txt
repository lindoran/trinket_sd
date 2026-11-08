# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

cmake_minimum_required(VERSION 3.13)

# Pull in the SDK -- this must happen before project()
include(pico_sdk_import.cmake)

project(trinket_sd C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Change this to match the actual board you're targeting.
set(PICO_BOARD pico CACHE STRING "Board type")

pico_sdk_init()

add_executable(trinket_sd
    src/main.c
    src/debug.c
    src/activity.c
    src/wire.c
    src/dispatch.c
    src/subsys_testharness.c
    src/subsys_spi.c
    src/subsys_sd.c
    src/subsys_dac.c
    src/dss_model.c
    src/dss_cmd.c
    src/subsys_dss.c
)

pico_generate_pio_header(trinket_sd ${CMAKE_CURRENT_LIST_DIR}/src/audio_i2s.pio)
pico_generate_pio_header(trinket_sd ${CMAKE_CURRENT_LIST_DIR}/src/dss_capture.pio)

# hardware_spi drives the H3 header's real SPI0 peripheral for subsys_spi/sd.
# hardware_pio + hardware_dma + hardware_pwm drive the I2S PCM5102 audio DAC on GPIO16,17,27,26.
target_link_libraries(trinket_sd
    pico_stdlib
    hardware_spi
    hardware_pio
    hardware_dma
    hardware_pwm
    hardware_clocks
)

pico_add_extra_outputs(trinket_sd)

# USB CDC console carries the WRITE/READ trace printf's -- see main.c.
pico_enable_stdio_usb(trinket_sd 1)
pico_enable_stdio_uart(trinket_sd 0)
