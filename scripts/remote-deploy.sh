#!/bin/sh
# Build the web remote and push it to a running PiDVD, then start it — no SD
# card shuffle, no image rebuild. Same docker toolchain + deploy key as
# scripts/deploy.sh. After this, open http://<pi-ip>:8080/ on any phone or
# laptop on the LAN to drive the picker.
#
#   scripts/remote-deploy.sh <pi-ip>
set -eu

PI="${1:?usage: remote-deploy.sh <pi-ip>}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
SSH="ssh -i $HOME/.ssh/pidvd_deploy -o StrictHostKeyChecking=no root@$PI"

echo ">> building pidvd-remote"
docker run --rm -v pidvd-build:/br -v "$REPO":/work pidvd-builder sh -c "
    cd /br/buildroot-2025.02.3 &&
    make BR2_EXTERNAL=/work/buildroot O=/br/output BR2_DL_DIR=/br/dl \
         pidvd-player-dirclean >/dev/null 2>&1
    make BR2_EXTERNAL=/work/buildroot O=/br/output BR2_DL_DIR=/br/dl \
         pidvd-player 2>&1 | grep -iE \"error|no rule|stop\.|failed\" && exit 1
    cp /br/output/target/usr/bin/pidvd-remote /work/output/deploy-remote" \
    || { echo '!! BUILD FAILED'; exit 1; }

echo ">> pushing pidvd-remote to $PI"
$SSH "killall pidvd-remote 2>/dev/null; mount -o remount,rw /"
$SSH "cat > /usr/bin/pidvd-remote && chmod 755 /usr/bin/pidvd-remote" \
    < "$REPO/output/deploy-remote"
$SSH "mount -o remount,ro / 2>/dev/null || true"

echo ">> starting pidvd-remote"
$SSH "pidvd-remote 8080 /tmp/pidvd-ctl >/dev/null 2>&1 &" || true
echo ">> done — open http://$PI:8080/ in a browser"
