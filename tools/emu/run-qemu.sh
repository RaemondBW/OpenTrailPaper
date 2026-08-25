#!/bin/sh
# Boot the t5s3-emu firmware in Espressif QEMU (esp32s3 machine).
#
#   tools/emu/run-qemu.sh              # console on stdio, frames on tcp:5556
#   QEMU=/path/to/qemu-system-xtensa   # override the binary
#
# Get QEMU (once):
#   gh release download -R espressif/qemu esp-develop-9.2.2-20260417 \
#     -p 'qemu-xtensa-softmmu-*-aarch64-apple-darwin.tar.xz' && tar -xf qemu-*.tar.xz
#
# serial0 = UART0: boot log + console (ARDUINO_USB_CDC_ON_BOOT=0 in this env)
# serial1 = UART1: the frame/event wire (web/ or tools/emu/frame2png.py)
# serial2 = UART2: the GPS — pipe NMEA in to simulate a ride
set -e
cd "$(dirname "$0")/../.."

BUILD=.pio/build/t5s3-emu
QEMU="${QEMU:-qemu-system-xtensa}"
ESPTOOL=~/.platformio/packages/tool-esptoolpy/esptool.py
BOOT_APP0=~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin

[ -f "$BUILD/firmware.bin" ] || pio run -e t5s3-emu

python3 "$ESPTOOL" --chip esp32s3 merge_bin -o "$BUILD/flash.bin" \
    --flash_mode keep --flash_size 16MB --fill-flash-size 16MB \
    0x0 "$BUILD/bootloader.bin" \
    0x8000 "$BUILD/partitions.bin" \
    0xe000 "$BOOT_APP0" \
    0x10000 "$BUILD/firmware.bin"

# -gdb: the input path. QEMU's esp32s3 UART model delivers no RX, so the
# bridge pokes events into the firmware's mailbox ring through the gdbstub.
exec "$QEMU" -M esp32s3 -gdb tcp::3333 \
    -drive file="$BUILD/flash.bin",if=mtd,format=raw \
    -serial mon:stdio \
    -serial tcp::5556,server,nowait \
    -serial tcp::5557,server,nowait \
    -display none
