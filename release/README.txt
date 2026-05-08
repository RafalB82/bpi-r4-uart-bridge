BPI-R4 UART Bridge - ESP32 Firmware
====================================

This firmware turns an ESP32 into a WiFi-to-serial bridge for debugging 
the Banana Pi BPI-R4 via UART console.

FEATURES
- WiFi connection: REDACTED_WIFI / REDACTED_PASSWORD (configurable via web)
- Web UI at http://esp32-ip/ with serial terminal in browser
- TCP serial bridge on port 8888 (for remote access)
- Configuration saved in flash

CONNECTIONS
  BPI-R4 (26pin header)   ->   ESP32 Dev Board
  Pin 8 (UART0 TX)        ->   GPIO16 (RX2)
  Pin 10 (UART0 RX)       ->   GPIO17 (TX2)
  Pin 9 (GND)             ->   GND

Both are 3.3V logic. Connect directly.

FLASHING (using esptool.py)
  esptool.py --port /dev/ttyUSB0 write_flash \
    0x1000 bootloader.bin \
    0x8000 partitions.bin \
    0x10000 firmware.bin

Or using PlatformIO:
  pio run --target upload

USAGE
1. Power on ESP32 - it connects to WiFi REDACTED_WIFI
2. Find its IP from router DHCP list
3. Open http://esp32-ip/ in browser
4. Connect via TCP: nc esp32-ip 8888
5. BPI-R4 bootlog appears in terminal

DEFAULT CREDENTIALS
  WiFi: REDACTED_WIFI / REDACTED_PASSWORD
  Config AP (if WiFi fails): BPI-R4-Bridge / config1234
