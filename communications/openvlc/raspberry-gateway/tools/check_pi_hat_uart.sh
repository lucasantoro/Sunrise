#!/usr/bin/env bash
# Preflight checks for the STM32 transceiver connected to Raspberry GPIO UART.
set -u

failed=0

fail() {
    echo "ERROR: $*" >&2
    failed=1
}

warn() {
    echo "WARNING: $*" >&2
}

uart_device=""
if [ -e /dev/ttyAMA0 ]; then
    # Prefer the GPIO header UART device directly: on Raspberry Pi 5,
    # /dev/serial0 can keep pointing at the dedicated debug UART
    # (ttyAMA10) even after uart0 is enabled via a dtoverlay.
    uart_target=/dev/ttyAMA0
    uart_device=ttyAMA0
    echo "GPIO UART: /dev/ttyAMA0 present"
elif [ ! -e /dev/serial0 ]; then
    fail "neither /dev/ttyAMA0 nor /dev/serial0 exists; enable the hardware serial port (dtoverlay=uart0 on Pi 5, raspi-config on earlier Pis) and reboot"
else
    uart_target=$(readlink -f /dev/serial0)
    uart_device=$(basename "$uart_target")
    echo "GPIO UART: /dev/serial0 -> $uart_target"
    case "$uart_target" in
        /dev/ttyAMA10)
            fail "$uart_target is the Pi 5 dedicated debug UART, not the GPIO header UART; add 'dtoverlay=uart0' to config.txt and reboot"
            ;;
        /dev/ttyAMA*) ;;
        /dev/ttyS*)
            fail "$uart_target is the mini-UART; use a PL011 ttyAMA device for reliable 2 Mbaud operation"
            ;;
        *)
            warn "unrecognized UART target $uart_target; verify that it is a hardware UART"
            ;;
    esac
fi

cmdline_file=""
for candidate in /boot/firmware/cmdline.txt /boot/cmdline.txt; do
    if [ -r "$candidate" ]; then
        cmdline_file="$candidate"
        break
    fi
done
if [ -n "$cmdline_file" ] && grep -Eq '(^|[[:space:]])console=(serial0|ttyAMA[0-9]*|ttyS[0-9]*),' "$cmdline_file"; then
    fail "serial console is still enabled in $cmdline_file"
fi

config_file=""
for candidate in /boot/firmware/config.txt /boot/config.txt; do
    if [ -r "$candidate" ]; then
        config_file="$candidate"
        break
    fi
done
if [ -n "$config_file" ] \
    && ! grep -Eq '^[[:space:]]*enable_uart=1([[:space:]]*(#.*)?)?$' "$config_file" \
    && ! grep -Eq '^[[:space:]]*dtparam=uart0=on([[:space:]]*(#.*)?)?$' "$config_file"; then
    warn "neither enable_uart=1 nor dtparam=uart0=on was found in $config_file"
fi

for unit in serial-getty@serial0.service "serial-getty@${uart_device}.service"; do
    if [ -n "$uart_device" ] && systemctl is-active --quiet "$unit" 2>/dev/null; then
        fail "$unit is active; disable the serial login console"
    fi
done

if [ -r /etc/default/openvlc-transceiver ]; then
    if ! grep -Eqx 'OPENVLC_SERIAL_PORT=/dev/(ttyAMA0|serial0)' /etc/default/openvlc-transceiver; then
        fail "/etc/default/openvlc-transceiver does not select /dev/ttyAMA0 or /dev/serial0"
    fi
    if ! grep -qx 'OPENVLC_SERIAL_BAUD=2000000' /etc/default/openvlc-transceiver; then
        fail "/etc/default/openvlc-transceiver does not select 2000000 baud"
    fi
else
    warn "/etc/default/openvlc-transceiver is not installed yet"
fi

if ! python3 -c 'import serial' >/dev/null 2>&1; then
    fail "python3-serial is not installed"
fi

if [ "$failed" -ne 0 ]; then
    echo "Pi HAT UART preflight failed." >&2
    exit 1
fi

echo "Pi HAT UART preflight passed."
