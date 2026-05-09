// SPDX-License-Identifier: MIT

#include <ctype.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#if CONFIG_BRIDGE_USE_USB_CDC
#include "driver/usb_serial_jtag.h"
#endif
#include "nvs_flash.h"
#include "sdkconfig.h"

#define UART_RX_BUFFER_SIZE 2048
#define UART_TX_BUFFER_SIZE 2048
#define UART_READ_TIMEOUT_TICKS 1
#define LED_EVENT_PULSE 1
#define LED_EVENT_CANCEL 2
#define SEND_RESULT_NONE 0
#define SEND_RESULT_PENDING 1
#define SEND_RESULT_SUCCESS 2
#define SEND_RESULT_FAIL 3

typedef struct {
    size_t len;
    uint8_t data[CONFIG_BRIDGE_PACKET_SIZE];
} tx_packet_t;

static const char *TAG = "esp_now_bridge";

static uint8_t s_peer_addr[ESP_NOW_ETH_ALEN];
static StreamBufferHandle_t s_rx_stream;
static QueueHandle_t s_tx_queue;
static QueueHandle_t s_led_queue;
static atomic_int s_send_result = SEND_RESULT_NONE;

static atomic_uint s_tx_dropped_packets = 0;
static atomic_uint s_tx_dropped_bytes = 0;
static atomic_uint s_rx_dropped = 0;
static atomic_uint s_rx_received_packets = 0;
static atomic_uint s_rx_received_bytes = 0;
static atomic_uint s_rx_truncated_bytes = 0;
static atomic_uint s_send_success_count = 0;
static atomic_uint s_send_fail_count = 0;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static int64_t now_ms(void)
{
    return now_us() / 1000;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static esp_err_t parse_mac(const char *text, uint8_t out[ESP_NOW_ETH_ALEN])
{
    if (text == NULL || strlen(text) != 17) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < ESP_NOW_ETH_ALEN; ++i) {
        int hi = hex_nibble(text[i * 3]);
        int lo = hex_nibble(text[i * 3 + 1]);
        if (hi < 0 || lo < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
        if (i < ESP_NOW_ETH_ALEN - 1 && text[i * 3 + 2] != ':') {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

#if CONFIG_BRIDGE_ESPNOW_ENCRYPTION
static esp_err_t parse_hex_key(const char *text, uint8_t out[ESP_NOW_KEY_LEN])
{
    if (text == NULL || strlen(text) != ESP_NOW_KEY_LEN * 2) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < ESP_NOW_KEY_LEN; ++i) {
        int hi = hex_nibble(text[i * 2]);
        int lo = hex_nibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }

    return ESP_OK;
}
#endif

static void set_led(bool on)
{
    gpio_set_level(CONFIG_BRIDGE_LED_PIN, CONFIG_BRIDGE_LED_ACTIVE_LOW ? !on : on);
}

static void request_led_pulse(void)
{
    if (s_led_queue != NULL) {
        const uint8_t event = LED_EVENT_PULSE;
        (void)xQueueSend(s_led_queue, &event, 0);
    }
}

#if CONFIG_BRIDGE_BLINK_ON_SEND_SUCCESS
static void cancel_led_pulse(void)
{
    if (s_led_queue != NULL) {
        const uint8_t event = LED_EVENT_CANCEL;
        (void)xQueueSend(s_led_queue, &event, 0);
    }
}
#endif

static void led_task(void *arg)
{
    (void)arg;
    int64_t led_off_at = 0;
    set_led(false);

    while (true) {
        uint8_t event = 0;
        TickType_t wait_ticks = portMAX_DELAY;
        if (led_off_at != 0) {
            int64_t remaining_ms = led_off_at - now_ms();
            wait_ticks = remaining_ms > 0 ? pdMS_TO_TICKS(remaining_ms) : 0;
        }

        if (xQueueReceive(s_led_queue, &event, wait_ticks) == pdTRUE) {
            if (event == LED_EVENT_CANCEL) {
                led_off_at = 0;
                set_led(false);
            } else if (event == LED_EVENT_PULSE) {
                set_led(true);
                led_off_at = now_ms() + CONFIG_BRIDGE_LED_PULSE_MS;
            }
        }

        if (led_off_at != 0 && now_ms() >= led_off_at) {
            led_off_at = 0;
            set_led(false);
        }
    }
}

static void on_data_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;

    if (status == ESP_NOW_SEND_SUCCESS) {
        atomic_fetch_add(&s_send_success_count, 1);
        atomic_store(&s_send_result, SEND_RESULT_SUCCESS);
#if CONFIG_BRIDGE_BLINK_ON_SEND_SUCCESS
        request_led_pulse();
#endif
    } else {
        atomic_fetch_add(&s_send_fail_count, 1);
        atomic_store(&s_send_result, SEND_RESULT_FAIL);
#if CONFIG_BRIDGE_BLINK_ON_SEND_SUCCESS
        cancel_led_pulse();
#endif
    }
}

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (info == NULL || memcmp(info->src_addr, s_peer_addr, ESP_NOW_ETH_ALEN) != 0) {
        return;
    }
    if (data == NULL || len <= 0) {
        return;
    }

#if CONFIG_BRIDGE_BLINK_ON_RECV
    request_led_pulse();
#endif

    const size_t capped_len = len > CONFIG_BRIDGE_PACKET_SIZE ? CONFIG_BRIDGE_PACKET_SIZE : (size_t)len;
    const size_t written = xStreamBufferSend(s_rx_stream, data, capped_len, 0);

    atomic_fetch_add(&s_rx_received_packets, 1);
    atomic_fetch_add(&s_rx_received_bytes, (unsigned)written);
    atomic_fetch_add(&s_rx_dropped, (unsigned)(capped_len - written));
    atomic_fetch_add(&s_rx_truncated_bytes, (unsigned)(len - capped_len));
}

static esp_err_t init_led(void)
{
    ESP_RETURN_ON_ERROR(gpio_reset_pin(CONFIG_BRIDGE_LED_PIN), TAG, "reset LED GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_direction(CONFIG_BRIDGE_LED_PIN, GPIO_MODE_OUTPUT), TAG, "set LED GPIO direction");
    set_led(false);
    s_led_queue = xQueueCreate(8, sizeof(uint8_t));
    ESP_RETURN_ON_FALSE(s_led_queue != NULL, ESP_ERR_NO_MEM, TAG, "create LED queue");
    BaseType_t ok = xTaskCreate(led_task, "bridge_led", 2048, NULL, 4, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create LED task");
    return ESP_OK;
}

static esp_err_t init_bridge_io(void)
{
#if CONFIG_BRIDGE_USE_USB_CDC
    usb_serial_jtag_driver_config_t usb_config = {
        .tx_buffer_size = UART_TX_BUFFER_SIZE,
        .rx_buffer_size = UART_RX_BUFFER_SIZE,
    };
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&usb_config), TAG, "install USB Serial/JTAG driver");
    return ESP_OK;
#else
    uart_config_t uart_config = {
        .baud_rate = CONFIG_BRIDGE_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    const uart_port_t uart_num = (uart_port_t)CONFIG_BRIDGE_UART_NUM;
    ESP_RETURN_ON_ERROR(uart_driver_install(uart_num, UART_RX_BUFFER_SIZE, UART_TX_BUFFER_SIZE, 0, NULL, 0), TAG,
                        "install UART driver");
    ESP_RETURN_ON_ERROR(uart_param_config(uart_num, &uart_config), TAG, "configure UART");
    ESP_RETURN_ON_ERROR(
        uart_set_pin(uart_num, CONFIG_BRIDGE_TX_PIN, CONFIG_BRIDGE_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG,
        "set UART pins");
    return ESP_OK;
#endif
}

static int bridge_read_bytes(uint8_t *buffer, size_t len, TickType_t ticks_to_wait)
{
#if CONFIG_BRIDGE_USE_USB_CDC
    return usb_serial_jtag_read_bytes(buffer, len, ticks_to_wait);
#else
    return uart_read_bytes((uart_port_t)CONFIG_BRIDGE_UART_NUM, buffer, len, ticks_to_wait);
#endif
}

static int bridge_write_bytes(const uint8_t *buffer, size_t len)
{
#if CONFIG_BRIDGE_USE_USB_CDC
    return usb_serial_jtag_write_bytes(buffer, len, pdMS_TO_TICKS(20));
#else
    return uart_write_bytes((uart_port_t)CONFIG_BRIDGE_UART_NUM, buffer, len);
#endif
}

static esp_err_t init_wifi(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "init netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "create event loop");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "init wifi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable wifi sleep");

    wifi_country_t country = {
        .cc = CONFIG_BRIDGE_WIFI_COUNTRY,
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_country(&country), TAG, "set wifi country");
    ESP_RETURN_ON_ERROR(esp_wifi_set_max_tx_power(CONFIG_BRIDGE_WIFI_MAX_TX_POWER), TAG, "set wifi tx power");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(CONFIG_BRIDGE_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE), TAG,
                        "set wifi channel");

    return ESP_OK;
}

static esp_err_t init_espnow(void)
{
#if CONFIG_BRIDGE_ESPNOW_ENCRYPTION
    uint8_t pmk[ESP_NOW_KEY_LEN];
    uint8_t lmk[ESP_NOW_KEY_LEN];
    ESP_RETURN_ON_ERROR(parse_hex_key(CONFIG_BRIDGE_ESPNOW_PMK_HEX, pmk), TAG, "parse ESP-NOW PMK");
    ESP_RETURN_ON_ERROR(parse_hex_key(CONFIG_BRIDGE_ESPNOW_LMK_HEX, lmk), TAG, "parse ESP-NOW LMK");
#endif

    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "init esp-now");
#if CONFIG_BRIDGE_ESPNOW_ENCRYPTION
    ESP_RETURN_ON_ERROR(esp_now_set_pmk(pmk), TAG, "set ESP-NOW PMK");
#endif
    ESP_RETURN_ON_ERROR(esp_now_register_send_cb(on_data_sent), TAG, "register send callback");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(on_data_recv), TAG, "register receive callback");

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_peer_addr, sizeof(s_peer_addr));
    peer.channel = CONFIG_BRIDGE_WIFI_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
#if CONFIG_BRIDGE_ESPNOW_ENCRYPTION
    memcpy(peer.lmk, lmk, ESP_NOW_KEY_LEN);
    peer.encrypt = true;
#else
    peer.encrypt = false;
#endif

    ESP_RETURN_ON_ERROR(esp_now_add_peer(&peer), TAG, "add ESP-NOW peer");
    return ESP_OK;
}

static void espnow_to_uart_task(void *arg)
{
    (void)arg;
    uint8_t buffer[128];

    while (true) {
        const size_t len = xStreamBufferReceive(s_rx_stream, buffer, sizeof(buffer), portMAX_DELAY);
        if (len > 0) {
            int written = bridge_write_bytes(buffer, len);
            if (written < 0) {
                atomic_fetch_add(&s_rx_dropped, (unsigned)len);
            } else if ((size_t)written < len) {
                atomic_fetch_add(&s_rx_dropped, (unsigned)(len - (size_t)written));
            }
        }
    }
}

static void drop_tx_packet(const tx_packet_t *packet)
{
    atomic_fetch_add(&s_tx_dropped_packets, 1);
    atomic_fetch_add(&s_tx_dropped_bytes, (unsigned)packet->len);
}

static void enqueue_tx_packet(tx_packet_t *packet)
{
    if (packet->len == 0) {
        return;
    }

    if (xQueueSend(s_tx_queue, packet, 0) != pdTRUE) {
        drop_tx_packet(packet);
    }
    packet->len = 0;
}

static void uart_packetizer_task(void *arg)
{
    (void)arg;
    tx_packet_t packet = {0};
    int64_t send_timeout_us = 0;
    const int64_t inter_byte_timeout_us = ((1000000LL * 20) + CONFIG_BRIDGE_BAUD_RATE - 1) / CONFIG_BRIDGE_BAUD_RATE;

    while (true) {
        uint8_t byte = 0;
        const int read_len = bridge_read_bytes(&byte, 1, UART_READ_TIMEOUT_TICKS);

        if (read_len == 1) {
            packet.data[packet.len++] = byte;
            send_timeout_us = now_us() + inter_byte_timeout_us;
        }

        const bool full = packet.len == sizeof(packet.data);
        const bool line_complete = false;
        const bool timed_out = packet.len > 0 && now_us() >= send_timeout_us;
        const bool should_flush = packet.len > 0 && (full || line_complete || timed_out);
        if (should_flush) {
            enqueue_tx_packet(&packet);
        }
    }
}

static bool wait_for_send_result(void)
{
    while (true) {
        const int send_result = atomic_load(&s_send_result);
        if (send_result == SEND_RESULT_SUCCESS) {
            atomic_store(&s_send_result, SEND_RESULT_NONE);
            return true;
        }
        if (send_result == SEND_RESULT_FAIL) {
            atomic_store(&s_send_result, SEND_RESULT_NONE);
            return false;
        }
        vTaskDelay(1);
    }
}

static void espnow_sender_task(void *arg)
{
    (void)arg;
    tx_packet_t packet = {0};

    while (true) {
        if (xQueueReceive(s_tx_queue, &packet, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        while (true) {
#if CONFIG_BRIDGE_BLINK_ON_SEND
            request_led_pulse();
#endif
            atomic_store(&s_send_result, SEND_RESULT_PENDING);
            esp_err_t err = esp_now_send(s_peer_addr, packet.data, packet.len);
            if (err == ESP_OK && wait_for_send_result()) {
                break;
            }

            if (err != ESP_OK) {
                atomic_store(&s_send_result, SEND_RESULT_NONE);
#if CONFIG_BRIDGE_DEBUG
                ESP_LOGW(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
#endif
            }
            vTaskDelay(pdMS_TO_TICKS(CONFIG_BRIDGE_SEND_RETRY_DELAY_MS));
        }
    }
}

#if CONFIG_BRIDGE_DEBUG
static void telemetry_task(void *arg)
{
    (void)arg;
    unsigned last_rx_packets = 0;
    unsigned last_rx_bytes = 0;
    unsigned last_rx_dropped = 0;
    unsigned last_rx_truncated = 0;
    unsigned last_tx_dropped_packets = 0;
    unsigned last_tx_dropped_bytes = 0;
    unsigned last_send_success = 0;
    unsigned last_send_fail = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BRIDGE_DEBUG_TELEMETRY_INTERVAL_MS));

        unsigned rx_packets = atomic_load(&s_rx_received_packets);
        unsigned rx_bytes = atomic_load(&s_rx_received_bytes);
        unsigned rx_dropped = atomic_load(&s_rx_dropped);
        unsigned rx_truncated = atomic_load(&s_rx_truncated_bytes);
        unsigned tx_dropped_packets = atomic_load(&s_tx_dropped_packets);
        unsigned tx_dropped_bytes = atomic_load(&s_tx_dropped_bytes);
        unsigned send_success = atomic_load(&s_send_success_count);
        unsigned send_fail = atomic_load(&s_send_fail_count);

        if (rx_packets != last_rx_packets) {
            ESP_LOGI(TAG, "ESP-NOW RX packets: %u, bytes: %u", rx_packets - last_rx_packets, rx_bytes - last_rx_bytes);
            last_rx_packets = rx_packets;
            last_rx_bytes = rx_bytes;
        }
        if (rx_dropped != last_rx_dropped || rx_truncated != last_rx_truncated) {
            ESP_LOGW(TAG, "ESP-NOW RX dropped: %u, truncated: %u", rx_dropped - last_rx_dropped,
                     rx_truncated - last_rx_truncated);
            last_rx_dropped = rx_dropped;
            last_rx_truncated = rx_truncated;
        }
        if (tx_dropped_packets != last_tx_dropped_packets) {
            ESP_LOGW(TAG, "UART TX queue dropped packets: %u, bytes: %u", tx_dropped_packets - last_tx_dropped_packets,
                     tx_dropped_bytes - last_tx_dropped_bytes);
            last_tx_dropped_packets = tx_dropped_packets;
            last_tx_dropped_bytes = tx_dropped_bytes;
        }
        if (send_success != last_send_success) {
            ESP_LOGI(TAG, "ESP-NOW send success: %u", send_success - last_send_success);
            last_send_success = send_success;
        }
        if (send_fail != last_send_fail) {
            ESP_LOGW(TAG, "ESP-NOW send failed: %u", send_fail - last_send_fail);
            last_send_fail = send_fail;
        }
    }
}
#endif

static esp_err_t start_bridge_tasks(void)
{
    BaseType_t ok = xTaskCreate(uart_packetizer_task, "uart_packetizer", 4096, NULL, 10, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create UART packetizer task");

    ok = xTaskCreate(espnow_sender_task, "espnow_sender", 4096, NULL, 10, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create ESP-NOW sender task");

    ok = xTaskCreate(espnow_to_uart_task, "espnow_to_uart", 4096, NULL, 9, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create ESP-NOW-to-UART task");

#if CONFIG_BRIDGE_DEBUG
    ok = xTaskCreate(telemetry_task, "bridge_telemetry", 4096, NULL, 3, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create telemetry task");
#endif

    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(parse_mac(CONFIG_BRIDGE_PEER_MAC, s_peer_addr));

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_rx_stream = xStreamBufferCreate(CONFIG_BRIDGE_RX_QUEUE_SIZE, 1);
    ESP_ERROR_CHECK(s_rx_stream == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_tx_queue = xQueueCreate(CONFIG_BRIDGE_TX_QUEUE_DEPTH, sizeof(tx_packet_t));
    ESP_ERROR_CHECK(s_tx_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    ESP_ERROR_CHECK(init_led());
    ESP_ERROR_CHECK(init_bridge_io());
    ESP_ERROR_CHECK(init_wifi());
    ESP_ERROR_CHECK(init_espnow());

    uint8_t own_mac[ESP_NOW_ETH_ALEN] = {0};
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, own_mac));
    ESP_LOGI(TAG, "ESP32 MAC address: %02X:%02X:%02X:%02X:%02X:%02X", own_mac[0], own_mac[1], own_mac[2], own_mac[3],
             own_mac[4], own_mac[5]);
    ESP_LOGI(TAG, "Peer MAC address: %02X:%02X:%02X:%02X:%02X:%02X", s_peer_addr[0], s_peer_addr[1], s_peer_addr[2],
             s_peer_addr[3], s_peer_addr[4], s_peer_addr[5]);
#if CONFIG_BRIDGE_ESPNOW_ENCRYPTION
    ESP_LOGI(TAG, "ESP-NOW encryption: enabled");
#else
    ESP_LOGI(TAG, "ESP-NOW encryption: disabled");
#endif
#if CONFIG_BRIDGE_USE_USB_CDC
    ESP_LOGI(TAG, "Bridge I/O: USB Serial/JTAG CDC ACM, channel %d", CONFIG_BRIDGE_WIFI_CHANNEL);
#else
    ESP_LOGI(TAG, "Bridge I/O: UART%d %d baud, TX GPIO %d, RX GPIO %d, channel %d", CONFIG_BRIDGE_UART_NUM,
             CONFIG_BRIDGE_BAUD_RATE, CONFIG_BRIDGE_TX_PIN, CONFIG_BRIDGE_RX_PIN, CONFIG_BRIDGE_WIFI_CHANNEL);
#endif

    ESP_ERROR_CHECK(start_bridge_tasks());
}
