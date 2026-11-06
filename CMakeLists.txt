cmake_minimum_required(VERSION 3.13)

add_compile_definitions(PICO_DEFAULT_UART_BAUD_RATE=921600)

set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/pico-sdk")
set(PICO_TINYUSB_PATH "${CMAKE_CURRENT_LIST_DIR}/tinyusb")
set(PICO_PIO_USB_PATH "${CMAKE_CURRENT_LIST_DIR}/Pico-PIO-USB")

include(pico_sdk_import.cmake)

# RP2040 runs at 192 MHz: a multiple of 48 MHz for Pico-PIO-USB, within the
# chip's 200 MHz rating with the core at 1.15 V. The flash clock divider goes
# up to 4 (48 MHz) so that flash chips on all sorts of boards keep up. Set
# before pico_sdk_init() so that the SDK's boot2 gets the divider too.
if(PICO_PLATFORM STREQUAL "rp2040")
    add_compile_definitions(
        SYS_CLK_HZ=192000000
        PLL_SYS_VCO_FREQ_HZ=1536000000
        PLL_SYS_POSTDIV1=4
        PLL_SYS_POSTDIV2=2
        SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST=1
        SYS_CLK_VREG_VOLTAGE_MIN=VREG_VOLTAGE_1_15
        PICO_FLASH_SPI_CLKDIV=4
    )
endif()

if((PICO_BOARD STREQUAL "pico") OR (PICO_BOARD STREQUAL "pico2"))
    add_compile_definitions(
        PICO_DEFAULT_UART_TX_PIN=16
        PICO_DEFAULT_UART_RX_PIN=17
        PICO_DEFAULT_PIO_USB_DP_PIN=0
        PICO_DEFAULT_PIO_USB_DP_PIN2=2
    )
elseif(PICO_BOARD STREQUAL "adafruit_feather_rp2040_usb_host")
    # USB host port on GPIO 16/17, 5V enable on GPIO 18, UART stays on board defaults (GPIO 0/1)
    add_compile_definitions(
        PICO_DEFAULT_PIO_USB_DP_PIN=16
        PICO_DEFAULT_PIO_USB_VBUSEN_PIN=18
        PICO_DEFAULT_PIO_USB_VBUSEN_STATE=1
    )
endif()

project(adapter)

pico_sdk_init()

add_executable(adapter
    src/adapter.c
    src/descriptors.c
)
target_include_directories(adapter PRIVATE src)
target_link_libraries(adapter
    pico_stdlib
    tinyusb_host
    tinyusb_device
    tinyusb_board
    tinyusb_pico_pio_usb
)
pico_add_extra_outputs(adapter)
