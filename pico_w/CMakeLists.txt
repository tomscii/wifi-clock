cmake_minimum_required (VERSION 3.13)

include (pico_sdk_import.cmake)

project (wifi_clock C CXX ASM)
set (CMAKE_C_STANDARD 11)
set (CMAKE_CXX_STANDARD 17)
pico_sdk_init ()

add_executable (wifi_clock
                main.c
                dht.c
                display.c
                ntp.c
                snmp.c
                wifi.c
               )

pico_enable_stdio_usb (wifi_clock 1)
pico_enable_stdio_uart (wifi_clock 1)

# create map/bin/hex file etc.
pico_add_extra_outputs (wifi_clock)

target_include_directories (wifi_clock PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_compile_definitions (wifi_clock PRIVATE
                            WIFI_SSID=\"${WIFI_SSID}\"
                            WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
                           )

target_link_libraries (wifi_clock
                       pico_cyw43_arch_lwip_poll
                       pico_stdlib
                       hardware_rtc
                       hardware_spi
                      )
