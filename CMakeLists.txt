cmake_minimum_required(VERSION 3.13)

add_compile_definitions(PICO_DEFAULT_UART_BAUD_RATE=921600)
add_compile_definitions(PICO_DEFAULT_UART_TX_PIN=16)
add_compile_definitions(PICO_DEFAULT_UART_RX_PIN=17)
add_compile_definitions(PICO_DEFAULT_PIO_USB_DP_PIN=0)

set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/pico-sdk")
set(PICO_TINYUSB_PATH "${CMAKE_CURRENT_LIST_DIR}/tinyusb")
set(PICO_PIO_USB_PATH "${CMAKE_CURRENT_LIST_DIR}/Pico-PIO-USB")

include(pico_sdk_import.cmake)

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
