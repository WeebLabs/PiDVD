#!/bin/sh
# Runs after the rootfs is assembled, before image creation.
set -eu

BOARD_DIR="$(dirname "$0")"

# Boot config + kernel cmdline live on the boot partition. Install BOTH
# here (post-build runs every build) rather than relying on the
# rpi-firmware package's BR2_PACKAGE_RPI_FIRMWARE_CONFIG_FILE install:
# that step is stamp-cached, so config.txt edits silently never reach
# the image after the first build (cost us a full day of boot debugging
# when a stale gpu_mem=16 kept shipping).
# Default boot = composite. The OUTPUT setting copies in a *-hdmi or
# *-composite variant and reboots, so ship all of them on the boot partition.
install -m 0644 "${BOARD_DIR}/config.txt"  "${BINARIES_DIR}/rpi-firmware/config.txt"
install -m 0644 "${BOARD_DIR}/cmdline.txt" "${BINARIES_DIR}/rpi-firmware/cmdline.txt"
install -m 0644 "${BOARD_DIR}/config.txt"  "${BINARIES_DIR}/rpi-firmware/config-composite.txt"
install -m 0644 "${BOARD_DIR}/cmdline.txt" "${BINARIES_DIR}/rpi-firmware/cmdline-composite.txt"
install -m 0644 "${BOARD_DIR}/config-hdmi.txt"  "${BINARIES_DIR}/rpi-firmware/config-hdmi.txt"
install -m 0644 "${BOARD_DIR}/cmdline-hdmi.txt" "${BINARIES_DIR}/rpi-firmware/cmdline-hdmi.txt"

# Mount point for USB disc drives, and for the boot partition (deploys)
mkdir -p "${TARGET_DIR}/media/usb" "${TARGET_DIR}/boot"

# No syslog/klog/cron on an appliance; the player logs to console
rm -f "${TARGET_DIR}/etc/init.d/S01syslogd" \
      "${TARGET_DIR}/etc/init.d/S02klogd" \
      "${TARGET_DIR}/etc/init.d/S50crond" \
      "${TARGET_DIR}/etc/init.d/S02sysctl"
