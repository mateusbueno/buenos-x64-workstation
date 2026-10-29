#!/usr/bin/env bash
# Story 9.1: ConfigFS composite USB gadget setup for Velora 73.
# Configures exactly three functions:
#   1. UAC2 Audio (functions/uac2.usb0)
#   2. MIDI 2.0 / 1.0 (functions/midi2.usb0 or functions/midi.usb0)
#   3. CDC-NCM Ethernet (functions/ncm.usb0)
#
# INVARIANT (NFR24, AR38):
# No mass-storage, ACM/serial or other function may be configured.
# Storage (NVMe/eMMC) is NEVER exposed as a block device over USB.
set -euo pipefail

GADGET_NAME="velora"
CONFIGFS_ROOT="${CONFIGFS_ROOT:-/sys/kernel/config}"
GADGET_DIR="$CONFIGFS_ROOT/usb_gadget/$GADGET_NAME"

# Fixed USB Identifiers (Story 9.1)
ID_VENDOR="0x1209"
ID_PRODUCT="0x5673"
BCD_DEVICE="0x0100"
BCD_USB="0x0200"

MANUFACTURER="Velora"
PRODUCT="Velora 73"

get_serial_number() {
    if [ -f /etc/machine-id ] && [ -s /etc/machine-id ]; then
        head -c 16 /etc/machine-id
    elif [ -f /proc/sys/kernel/random/boot_id ]; then
        tr -d '-' < /proc/sys/kernel/random/boot_id | head -c 16
    else
        echo "VELORA73-DEV001"
    fi
}

start_gadget() {
    SERIAL_NUMBER="$(get_serial_number)"

    # Ensure configfs is mounted
    if [ ! -d "$CONFIGFS_ROOT/usb_gadget" ]; then
        mkdir -p "$CONFIGFS_ROOT"
        mount -t configfs none "$CONFIGFS_ROOT" 2>/dev/null || true
    fi

    mkdir -p "$GADGET_DIR"
    cd "$GADGET_DIR"

    # Device Descriptors
    echo "$ID_VENDOR" > idVendor
    echo "$ID_PRODUCT" > idProduct
    echo "$BCD_DEVICE" > bcdDevice
    echo "$BCD_USB" > bcdUSB

    # Strings (English US 0x409)
    mkdir -p strings/0x409
    echo "$MANUFACTURER" > strings/0x409/manufacturer
    echo "$PRODUCT" > strings/0x409/product
    echo "$SERIAL_NUMBER" > strings/0x409/serialnumber

    # Configuration 1
    mkdir -p configs/c.1/strings/0x409
    echo "Velora Composite (Audio/MIDI/NCM)" > configs/c.1/strings/0x409/configuration
    echo 500 > configs/c.1/MaxPower

    # 1. Function: UAC2 Audio (Capture to host: 4 channels, 48 kHz, 24-bit, async with explicit feedback)
    mkdir -p functions/uac2.usb0
    echo 48000 > functions/uac2.usb0/c_srate
    echo 3 > functions/uac2.usb0/c_ssize
    echo 0x0F > functions/uac2.usb0/c_chmask
    echo "async" > functions/uac2.usb0/c_sync
    echo 0 > functions/uac2.usb0/p_chmask

    # 2. Function: MIDI (f_midi2 UMP + MIDI 1.0 alternate setting, fallback to f_midi)
    if [ -d functions/midi2.usb0 ] || mkdir -p functions/midi2.usb0 2>/dev/null; then
        MIDI_FUNC="midi2.usb0"
        echo "Velora MIDI" > functions/midi2.usb0/id || true
    else
        mkdir -p functions/midi.usb0
        MIDI_FUNC="midi.usb0"
        echo "Velora MIDI" > functions/midi.usb0/id || true
        echo 1 > functions/midi.usb0/in_ports || true
        echo 1 > functions/midi.usb0/out_ports || true
    fi

    # 3. Function: CDC-NCM Ethernet (Peer-to-peer link for Velora Studio on ws.v1)
    mkdir -p functions/ncm.usb0

    # Link EXACTLY these three functions into configuration 1
    ln -s "functions/uac2.usb0" configs/c.1/uac2.usb0
    ln -s "functions/$MIDI_FUNC" configs/c.1/midi.usb0
    ln -s "functions/ncm.usb0" configs/c.1/ncm.usb0

    # Bind to first available UDC
    UDC_DRIVER="$(ls /sys/class/udc 2>/dev/null | head -n 1 || true)"
    if [ -n "$UDC_DRIVER" ]; then
        echo "$UDC_DRIVER" > UDC
        echo "Velora USB gadget bound to $UDC_DRIVER"
    fi
}

stop_gadget() {
    if [ -d "$GADGET_DIR" ]; then
        cd "$GADGET_DIR"
        if [ -f UDC ] && [ -s UDC ]; then
            echo "" > UDC || true
        fi
        rm -f configs/c.1/uac2.usb0 configs/c.1/midi.usb0 configs/c.1/ncm.usb0 || true
        rmdir configs/c.1/strings/0x409 configs/c.1 2>/dev/null || true
        rmdir functions/uac2.usb0 functions/midi2.usb0 functions/midi.usb0 functions/ncm.usb0 2>/dev/null || true
        rmdir strings/0x409 2>/dev/null || true
        cd "$CONFIGFS_ROOT/usb_gadget"
        rmdir "$GADGET_NAME" 2>/dev/null || true
    fi
}

case "${1:-start}" in
    start)
        start_gadget
        ;;
    stop)
        stop_gadget
        ;;
    restart)
        stop_gadget
        start_gadget
        ;;
    *)
        echo "Usage: $0 {start|stop|restart}" >&2
        exit 1
        ;;
esac
