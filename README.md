# BPI-R4 UART Bridge

ESP32 firmware that turns your ESP32 into a WiFi-to-UART bridge for debugging the Banana Pi BPI-R4.

## Features

- Web UI with serial terminal (WebSocket)
- TCP bridge on port 8888 (`nc <ip> 8888`)
- WiFi configuration via web (AP mode on first boot)
- Settings saved to flash (NVS)
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

```bash
esptool.py --port /dev/ttyUSB0 write_flash \
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
