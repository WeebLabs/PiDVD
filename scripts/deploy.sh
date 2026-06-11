#!/bin/sh
# Push freshly built artifacts to a running PiDVD over the network —
# no SD card shuffle. Uses the baked-in deploy key.
#
#   scripts/deploy.sh <pi-ip>            rebuild player pkg + push binary,
#                                        restart playback
#   scripts/deploy.sh <pi-ip> --kernel   also push zImage/DTBs/config/cmdline
#                                        to the boot partition and reboot
set -eu

PI="${1:?usage: deploy.sh <pi-ip> [--kernel]}"
MODE="${2:-player}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
SSH="ssh -i $HOME/.ssh/pidvd_deploy -o StrictHostKeyChecking=no root@$PI"

echo ">> building player"
docker run --rm -v pidvd-build:/br -v "$REPO":/work pidvd-builder sh -c "
    cd /br/buildroot-2025.02.3 &&
    make BR2_EXTERNAL=/work/buildroot O=/br/output BR2_DL_DIR=/br/dl \
         pidvd-player-rebuild >/dev/null 2>&1 &&
    cp /br/output/target/usr/bin/pidvd-player /work/output/deploy-player"

echo ">> pushing player to $PI"
$SSH "killall pidvd-player 2>/dev/null; mount -o remount,rw /"
$SSH "cat > /usr/bin/pidvd-player && chmod 755 /usr/bin/pidvd-player" \
    < "$REPO/output/deploy-player"
$SSH "mount -o remount,ro / 2>/dev/null || true"

if [ "$MODE" = "--kernel" ]; then
    echo ">> pushing kernel + boot files"
    docker run --rm -v pidvd-build:/br -v "$REPO":/work pidvd-builder sh -c "
        cp /br/output/images/zImage /br/output/images/bcm2710-rpi-3-b.dtb \
           /br/output/images/bcm2710-rpi-3-b-plus.dtb /work/output/deploy-boot/ 2>/dev/null ||
        { mkdir -p /work/output/deploy-boot && \
          cp /br/output/images/zImage /br/output/images/bcm2710-rpi-3-b.dtb \
             /br/output/images/bcm2710-rpi-3-b-plus.dtb /work/output/deploy-boot/; }"
    $SSH "mount -t vfat /dev/mmcblk0p1 /boot 2>/dev/null || true"
    for f in zImage bcm2710-rpi-3-b.dtb bcm2710-rpi-3-b-plus.dtb; do
        $SSH "cat > /boot/$f" < "$REPO/output/deploy-boot/$f"
    done
    $SSH "cat > /boot/config.txt"  < "$REPO/buildroot/board/pidvd/config.txt"
    $SSH "cat > /boot/cmdline.txt" < "$REPO/buildroot/board/pidvd/cmdline.txt"
    $SSH "umount /boot && reboot" || true
    echo ">> rebooting with new kernel"
else
    echo ">> restarting playback"
    $SSH "/etc/init.d/S90pidvd stop; /etc/init.d/S90pidvd start" || true
fi
echo ">> done"
