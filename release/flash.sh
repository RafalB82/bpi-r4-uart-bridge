#!/bin/bash
PORT="${1:-/dev/ttyUSB0}"
esptool.py --port "$PORT" write_flash \
    0x1000 bootloader.bin \
    0x8000 partitions.bin \
    0x10000 firmware.bin
