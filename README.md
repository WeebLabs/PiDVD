# PiDVD

A field-accurate DVD player appliance for the Raspberry Pi 3.

PiDVD boots a minimal Buildroot Linux image straight into a custom KMS player that
plays DVD-Video ISOs (decrypted) with full menu support, outputting **native
interlaced 15 kHz video** — 480i59.94 or 576i50, auto-selected from the disc's IFO
video attributes. No scaling, no deinterlacing, no 480p. Scanout is pixel-exact
BT.601: 720 active samples per line at a 13.5 MHz dot clock.

## Signal chain

```
DVD ISO (USB drive)
  └─ player core: libdvdread/libdvdnav → libmpeg2 + a52dec → SPU overlay
       └─ field-accurate presenter (honors TFF/RFF, native 3:2 cadence)
            ├─ Milestone 1: composite (VEC)            → CRT
            ├─ Milestone 3: VGA666 (18-bit DPI) → UMSA → SCART CRT
            └─ display-locked PCM → USB S/PDIF (adaptive resampling)
```

## Repository layout

```
player/      Player core + tools. OS-agnostic C against a thin platform layer.
             Builds on the macOS host too (tools/dvdinfo) for fast development.
buildroot/   BR2_EXTERNAL tree: defconfig, board files, rootfs overlay,
             pidvd-player package.
docker/      Linux build container (Buildroot does not build on macOS).
scripts/     Build entry points.
docs/        Architecture, decisions, risk register.
```

## Quick start

### Host tool (macOS) — inspect an ISO

```sh
brew install libdvdread
cmake -S player -B player/build && cmake --build player/build
player/build/dvdinfo /path/to/movie.iso
```

### Pi image (via Docker)

```sh
./scripts/build-image.sh        # downloads Buildroot, builds sdcard.img
# flash output/images/sdcard.img to an SD card
```

## Hardware

- Raspberry Pi 3B / 3B+ (Pi Zero 2 W compatible — same SoC family)
- VGA666 (Gert van Loo's passive DPI DAC, GPIO 0–21) → UMSA → SCART
- USB S/PDIF adapter (class-compliant). **S/PDIF/I2S HATs are incompatible:**
  DPI consumes GPIO 18–21 (the I2S pins).
- IR receiver (TSOP-style) on a free GPIO (22–27) — optional, later milestone
- USB drive holding decrypted `.iso` files; USB keyboard / 2.4 GHz media remote

See `docs/ARCHITECTURE.md` for the full design and milestone plan.
