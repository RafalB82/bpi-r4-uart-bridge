BPI-R4 UART Bridge - ESP32 Firmware
====================================

This firmware turns an ESP32 into a WiFi-to-serial bridge for debugging 
the Banana Pi BPI-R4 via UART console.

FEATURES
- WiFi connection configured via web UI on first boot (AP mode)
- Web UI at http://bpi-r4-bridge.local/ with serial terminal in browser
- TCP serial bridge on port 8888 (for remote access)
- Configuration saved in flash
- mDNS: bpi-r4-bridge.local

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
1. Power on ESP32 - connects to configured WiFi (or starts AP on first boot)
2. Open http://bpi-r4-bridge.local/ in browser (or use router's DHCP list for IP)
3. Connect via TCP: nc bpi-r4-bridge.local 8888
4. BPI-R4 bootlog appears in terminal

DEFAULT CREDENTIALS
  AP mode (first boot / WiFi failure): BPI-R4-Bridge / config1234

CHANGELOG
  v1.4 - security: XSS fix in /status.json (proper JSON string escaping)
         security: SSID/password length validation (WPA2 limits: 32/63 chars)
         fix: safe restart via flag + loop drain (no more delay+restart race)
         fix: erase-remove idiom for TCP client pruning (cleaner, no O(n²))
         fix: static buffer for TCP->UART reads (stack safety, was 4KB on stack)
         fix: non-printable char filtering in WebSocket broadcast
         fix: ring buffer full warning logged (was silent data drop)
         fix: removed dead pendingRestart variable
         add: Task Watchdog (10s timeout, auto-resets ESP on loop hang)
         chore: flash.sh rewritten with proper error handling
         chore: release binaries removed from git (use GitHub Releases)
  v1.3 - fix: mDNS no longer restarted every loop iteration
         fix: UART read buffer enlarged from 256 to 4096 bytes
         fix: TCP client vector pre-reserved (no heap reallocs)
         fix: WiFiEvent logs AP client connect/disconnect
         fix: vTaskDelay(1) instead of delay(1) for proper FreeRTOS yield
  v1.2 - mDNS hostname (bpi-r4-bridge.local)
         NVS baud rate validation
         Increased UART RX buffer
  v1.1 - TCP bridge on port 8888
         Multi-client support
  v1.0 - Initial release
