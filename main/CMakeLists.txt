# SPDX-License-Identifier: MIT

idf_component_register(
    SRCS "diagnostics_web.c" "esp_now_serial_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_timer nvs_flash log esp_event esp_netif esp_http_server spiffs esp_driver_gpio esp_driver_uart esp_driver_usb_serial_jtag led_strip
)

if(CONFIG_BRIDGE_DIAGNOSTICS_WEB)
    spiffs_create_partition_image(storage web FLASH_IN_PROJECT)
endif()
