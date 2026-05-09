# SPDX-License-Identifier: MIT

idf_component_register(
    SRCS "esp_now_serial_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_timer nvs_flash log esp_event esp_netif esp_driver_gpio esp_driver_uart esp_driver_usb_serial_jtag led_strip
)
