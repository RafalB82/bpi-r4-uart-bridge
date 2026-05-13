# BPI-R4 UART Bridge

ESP32 firmware that turns your ESP32 into a WiFi-to-UART bridge for debugging the Banana Pi BPI-R4.

## Features

- Web UI with serial terminal (WebSocket)
- TCP bridge on port 8888 (`nc <ip> 8888`)
- WiFi configuration via web (AP mode on first boot)
- Settings saved to flash (NVS)
- Task Watchdog for reliability (auto-reset on hang)
- Input validation (SSID/password length, baud range)
- No hardcoded credentials

## First use

1. Flash the firmware (see below)
2. Connect to ESP32 AP: `BPI-R4-Bridge` (password: `config1234`)
3. Open http://192.168.4.1/
4. Go to WiFi Config, enter your network credentials
5. ESP32 reboots and connects to your WiFi

## Connections

```
BPI-R4 (26pin header)   ESP32 Dev Board
  pin 8  (UART0 TX)   -> GPIO16 (RX2)
  pin 10 (UART0 RX)   -> GPIO17 (TX2)
  pin 9  (GND)         -> GND
```

> **mDNS:** Once connected to WiFi, reach the bridge at **http://bpi-r4-bridge.local/**
>
> Or via TCP: `nc bpi-r4-bridge.local 8888`

## Flash

Download the latest release from [GitHub Releases](../../releases), then:

```bash
./flash.sh /dev/ttyUSB0
```

Or manually:

```bash
esptool.py --port /dev/ttyUSB0 --baud 460800 write_flash \
  0x1000 bootloader.bin \
  0x8000 partitions.bin \
  0x10000 firmware.bin
```

Or with PlatformIO: `pio run --target upload`

## Build

```bash
pio run
```

## Files

- `src/main.cpp` - firmware source
- `platformio.ini` - PlatformIO project config
- `release/flash.sh` - flashing helper script
- `release/README.txt` - release notes and changelog

## Changelog

### v1.5
- **Fix:** Non-blocking WiFi connect (no longer blocks loop for up to 20s — was risking WDT reset)
- **Fix:** `Serial2.setTimeout(5ms)` — `readBytes()` returns quickly instead of potentially blocking up to 1s
- **Fix:** Baud rate whitelist — only standard values accepted (9600–921600), no garbage like `12345`

### v1.4
- **Security:** XSS fix in `/status.json` (proper JSON escaping)
- **Security:** SSID/password length validation (WPA2 limits)
- **Fix:** Safe restart via flag + loop drain (no delay+restart race)
- **Fix:** Erase-remove idiom for TCP client pruning
- **Fix:** Static buffer for TCP→UART reads (stack safety)
- **Fix:** Non-printable char filtering in WebSocket broadcast
- **Fix:** Ring buffer full warning logged (was silent drop)
- **Add:** Task Watchdog (10s timeout, auto-resets on hang)
- **Chore:** `flash.sh` rewritten with error handling
- **Chore:** Release binaries moved to GitHub Releases

### v1.3
- mDNS no longer restarted every loop iteration
- UART RX buffer enlarged (256 → 4096)
- TCP client vector pre-reserved
- WiFiEvent logs AP client events
- `vTaskDelay(1)` for proper FreeRTOS yield

### v1.2
- mDNS hostname (`bpi-r4-bridge.local`)
- NVS baud rate validation
- Increased UART RX buffer

### v1.1
- TCP bridge on port 8888
- Multi-client support

### v1.0
- Initial release
