#!/bin/sh
# Build the PiDVD SD card image inside Docker (Buildroot needs Linux).
#
# All heavy I/O (Buildroot source, downloads, build tree) lives in a named
# Docker volume — Buildroot's small-file load over a bind mount (virtiofs)
# is slow and has crashed the Docker Desktop VM. Only the final images are
# copied back to the repo. JOBS=n caps build parallelism.
#
# Output: output/images/sdcard.img
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BR_VERSION="2025.02.3"

docker build -t pidvd-builder "${REPO}/docker"
docker volume create pidvd-build >/dev/null
mkdir -p "${REPO}/output/images"

docker run --rm \
    -v pidvd-build:/br \
    -v "${REPO}:/work" \
    -e FORCE_UNSAFE_CONFIGURE=1 \
    pidvd-builder \
    sh -c "
        set -eu
        if [ ! -d /br/buildroot-${BR_VERSION} ]; then
            echo '>> Fetching Buildroot ${BR_VERSION}'
            wget -q https://buildroot.org/downloads/buildroot-${BR_VERSION}.tar.gz \
                 -O /br/br.tar.gz
            tar -xzf /br/br.tar.gz -C /br && rm /br/br.tar.gz
        fi
        cd /br/buildroot-${BR_VERSION}
        make BR2_EXTERNAL=/work/buildroot O=/br/output \
             BR2_DL_DIR=/br/dl pidvd_defconfig
        make -C /br/output -j${JOBS:-\$(nproc)}
        cp /br/output/images/sdcard.img /work/output/images/
    "

echo ">> Done: output/images/sdcard.img"
