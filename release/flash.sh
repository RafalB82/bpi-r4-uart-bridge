#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-/dev/ttyUSB0}"

# Verify esptool.py is available
if ! command -v esptool.py &>/dev/null; then
    echo "ERROR: esptool.py not found. Install with: pip install esptool"
    exit 1
fi

# Verify port exists
if [ ! -e "$PORT" ]; then
    echo "ERROR: Serial port '$PORT' not found."
    echo "Usage: $0 [port]  (default: /dev/ttyUSB0)"
    echo "Available ports:"
    ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || echo "  (none found)"
    exit 1
fi

# Verify firmware files exist
for f in bootloader.bin partitions.bin firmware.bin; do
    if [ ! -f "$SCRIPT_DIR/$f" ]; then
        echo "ERROR: Missing $f in $SCRIPT_DIR"
        exit 1
    fi
done

echo "Flashing BPI-R4 UART Bridge firmware to $PORT..."
esptool.py --port "$PORT" --baud 460800 write_flash \
    0x1000  "$SCRIPT_DIR/bootloader.bin" \
    0x8000  "$SCRIPT_DIR/partitions.bin" \
    0x10000 "$SCRIPT_DIR/firmware.bin"

echo ""
echo "Flash complete! Reset the ESP32 to start the bridge."
