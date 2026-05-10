# ESP-Serial-Bridge

ESP32 ESP-NOW serial bridge for transmitting serial data between two boards.

## Build and flash

Set up ESP-IDF first, then build from the repository root:

```sh
source ~/.espressif/tools/activate_idf_v6.0.1.sh
idf.py set-target esp32c3
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

If `idf.py` is not available, repair/install the ESP-IDF Python environment for v6.0.1 before building.

### Waveshare ESP32-C6-Dev-Kit-N8 / 8 MB OTA layout

For the Waveshare ESP32-C6-Dev-Kit-N8, use the dedicated 8 MB OTA profile. It uses `partitions_8mb_ota.csv` with two 3 MiB OTA app slots so a future web updater can write firmware directly to the inactive slot, plus a 1.875 MiB `storage` SPIFFS partition for diagnostics web assets.

Use a separate build directory and sdkconfig file for this board:

```sh
source ~/.espressif/tools/activate_idf_v6.0.1.sh
idf.py -B build-c6-n8 \
    -DSDKCONFIG=sdkconfig.esp32c6_n8 \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6_8mb_ota" \
    set-target esp32c6 reconfigure build
idf.py -B build-c6-n8 -p /dev/ttyACM0 flash monitor
```

If an existing generated `sdkconfig` keeps old 2 MB flash or single-app partition settings, create a fresh `SDKCONFIG` file as shown above or rerun `reconfigure`/`menuconfig` after changing defaults.

## Configuration

Run `idf.py menuconfig` and open **ESP-NOW Serial Bridge**.

Important options:

- **Peer WiFi STA MAC address**: required; set this to the opposite board's WiFi STA MAC. Default: `94:B9:7E:D9:DD:D4`.
- **WiFi / ESP-NOW channel**: default `13`.
- **Bridge UART baud rate**: default `115200`.
- **UART number / TX / RX pins**: defaults match the sketch (`UART1`, TX GPIO `1`, RX GPIO `3`). These pins may be shared with USB-UART/boot logs on some boards; change them if needed.
- **Activity LED**: GPIO, polarity, pulse length, and blink-on-send/recv behavior. By default it blinks on receive and successful send.
- **WS2812/NeoPixel activity LED**: optional `led_strip` support for boards with addressable RGB LEDs. Configure GPIO and wire color order (`GRB` for most WS2812 LEDs, `RGB` for boards whose colors appear swapped). NeoPixel mode uses fixed activity colors: send = blue, receive = green, overlapping send/receive = yellow.
- **Debug telemetry logs**: optional ESP-IDF log output with packet/drop/send counters.

Hardware UART is the default bridge interface. On ESP32-C3/S3/C6-class targets, **Use USB Serial/JTAG for bridge traffic** can use the native `/dev/ttyACM*` CDC ACM port instead. If you use that port for payload data, monitor/debug logs share the same stream and can interfere with binary protocols.

## NMEA-0183 GPS/AIS

Recommended options:

- Enable **Flush UART packets on line feed** (`CONFIG_BRIDGE_PACKET_SPLIT_ON_LF=y`) so NMEA sentences ending in `\r\n` are forwarded as complete ESP-NOW packets.
- Keep **UART to ESP-NOW transmit queue depth** (`CONFIG_BRIDGE_TX_QUEUE_DEPTH`) at the default `16` or increase it if AIS bursts and RF retries cause TX queue drops.
- Increase **ESP-NOW RX byte queue size** (`CONFIG_BRIDGE_RX_QUEUE_SIZE`) if reverse-direction bursts matter.
- Disable debug telemetry logs on payload interfaces.
- Prefer hardware UART over USB Serial/JTAG CDC for NMEA payloads, because USB CDC may share the same stream with logs/monitor traffic.

## Optional diagnostic web interface

An optional read-only diagnostic web page can be enabled in `idf.py menuconfig` under **ESP-NOW Serial Bridge → Enable diagnostic web interface**.

When enabled, the board starts a WPA3-only SoftAP alongside ESP-NOW. Configure the SoftAP SSID, passphrase, and gateway IP in the same menu. Open:

```text
http://<configured-ap-ip>/
```

The page is read-only and build-time configured only. It exposes bridge status such as uptime, STA/peer MACs, channel, ESP-NOW RX/send counters, and UART-to-ESP-NOW drop counters. The JSON endpoint is available at `/api/status`.

The static diagnostics assets live in `main/web` and are packed into the `storage` SPIFFS partition. Because the SPIFFS image is marked `FLASH_IN_PROJECT`, `idf.py flash` writes the web assets together with the firmware.

Notes:

- WPA3 SoftAP/SAE support is required.
- The SoftAP uses the same channel as ESP-NOW (`CONFIG_BRIDGE_WIFI_CHANNEL`).
- The AP netmask is fixed to `/24`; DHCP leases are derived automatically from the configured AP IP.
- The `storage` partition is 1.875 MiB; keep diagnostics assets reasonably small.

## Optional ESP-NOW encryption

ESP-NOW encryption is disabled by default. Enable **ESP-NOW Serial Bridge → Enable ESP-NOW encryption** in menuconfig on both boards.

Both boards must use identical 16-byte PMK and LMK values, configured as 32 hex characters:

```sh
openssl rand -hex 16
```

Keep real keys secret and do not commit production keys. The defaults are example values only.

## Notes

- ESP-NOW payload size is capped at 250 bytes.
- The UART-to-ESP-NOW packet timeout follows the original sketch: about 20 bit-times at the configured baud rate.
- WiFi sleep is disabled for lower latency.
- Country defaults to `DE`; verify channel and transmit power settings are legal in your region.

## Acknowledgements

This project is an ESP-IDF reimplementation based on and inspired by the original Arduino implementation [ESP-Now-Serial-Bridge](https://github.com/yuri-rage/ESP-Now-Serial-Bridge) by yuri-rage. The original project's license notice is preserved in substance by using the same permissive MIT license terms.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
