# PiDVD Architecture

## Goal

A DVD player appliance whose entire point is **native field-accurate 15 kHz
interlaced output**. The player never produces 480p or scaled video. Each decoded
field is presented to the display at field rate, in the correct field order, with
MPEG-2 `top_field_first` / `repeat_first_field` flags honored — so NTSC film
content plays with its native 3:2 pulldown cadence and PAL is a clean 2 fields
per frame.

## Platform decisions (locked 2026-06)

| Decision | Choice | Why |
|---|---|---|
| Board | Pi 3B/3B+ | VC4 is the proven 15 kHz scanout silicon (VGA666/RGB-Pi era); Pi 4's BCM2711 has a history of broken interlace in KMS |
| OS | Buildroot minimal Linux | 3–5× less effort than bare metal at identical output fidelity; `vc4` KMS driver as the working base; real debugging |
| Bare metal | Deferred, not dropped | Player core is OS-agnostic C behind a platform layer; a Circle port stays possible |
| Video out | VEC composite first, then VGA666 (18-bit DPI) → UMSA → SCART | UMSA handles csync + audio onto SCART, we output plain RGBHV |
| Audio out | USB S/PDIF adapter | I2S/S/PDIF HATs impossible: DPI mode consumes GPIO 0–21 incl. I2S pins 18–21. USB is class-compliant, supports IEC 61937 AC-3 passthrough |
| Decode | Software (libmpeg2, a52dec, MP2, LPCM) | VC4 hardware MPEG-2 decoder is unreachable/unlicensed from our stack; SD MPEG-2 is comfortable on 4× A53 |
| CSS | Not implemented | ISOs must be pre-decrypted |

## Player core

OS-agnostic C, organized as:

```
src/core/       disc model: ISO open (libdvdread), IFO attributes, standard
                detection (NTSC/PAL from video_attr, NOT region code),
                title/chapter/audio/subpicture enumeration
src/nav/        libdvdnav integration: DVD VM, menus, NAV packet (PCI/DSI),
                button highlights, seamless branching
src/demux/      MPEG-PS demux of VOB sectors handed out by dvdnav
src/decode/     video (libmpeg2), audio (a52dec / MP2 / LPCM), SPU (custom
                RLE sub-picture decoder for subtitles + menu highlights)
src/present/    A/V clock, field scheduler: maps decoded fields to output
                fields by PTS, honors TFF/RFF, drives vsync-paced flips
src/platform/   thin interfaces: video_out, audio_out, input, storage
platform/linux/ KMS/DRM atomic implementation, ALSA (USB S/PDIF), evdev
```

### Threading (4× Cortex-A53)

- core 0: nav/demux + UI
- core 1: MPEG-2 video decode
- core 2: audio decode + SPU
- core 3: presenter (field pacing, page flips)

## Video output

- Modes: 720×480i @ 59.94 fields/s, 720×576i @ 50 fields/s. Pixel clock
  13.5 MHz — 1:1 with DVD's BT.601 sampling. No resampling anywhere.
- Standard auto-selected from the IFO `video_attr` of the VTS being played
  (`video_format`: 0=NTSC, 1=PAL). Region code is irrelevant to this.
- KMS atomic page flips on a true interlaced mode; one flip per frame, the
  CRTC scans alternating fields; RFF handled by repeating a flip interval.
- Widescreen: DVDs are anamorphic (always 720 wide); the 16:9 flag is
  signaled, not scaled. On PAL we render WSS into line 23 as picture content
  so 16:9-capable sets auto-switch.
- Color: YCbCr 4:2:0 → RGB (NEON), dithered to RGB666 for the VGA666.
- Milestone 1 uses the VEC (composite), which is natively interlaced and
  the lowest-risk first picture. DPI interlace (pixel valve PV0 interlace
  bit) is the one unproven assumption — proven with a test pattern before
  the player depends on it. The Linux `vc4` driver is the register reference;
  a small kernel patch is acceptable if DPI interlace needs enabling.

## Audio

- AC-3: decode+downmix to stereo PCM, or IEC 61937 passthrough (config)
- MP2, LPCM: decode to PCM
- Output: ALSA → USB S/PDIF. Optional build: stereo downmix to the 3.5 mm
  jack (PWM) for SCART TV speakers via the UMSA.
- A/V sync: audio clock is master; video fields scheduled against it.

## UX

- ISOs on a USB drive (FAT/exFAT/ext4). Hot-plug = disc insert.
- Exactly one ISO → autoplay. Several → big-text picker rendered in the
  same 15 kHz mode. Stop/eject returns to the picker.
- Input: single evdev layer — USB keyboard, 2.4 GHz USB media remotes
  (HID), IR via `gpio-ir-recv` on GPIO 22–27 (free in DPI mode).
- Read-only root, power-cut safe, boot-to-player ~3–5 s.

## Milestones

1. **Composite first picture** — Buildroot image boots, mounts USB, parses
   IFOs (structure + standard over serial/console), VEC shows decoded video.
2. **Full playback** — dvdnav menus, SPU/subtitles, A/V sync, S/PDIF audio,
   field-cadence presentation.
3. **VGA666** — interlaced 15 kHz DPI proven (test pattern, then player),
   pixel-exact RGB via UMSA.
4. **Polish** — picker UI, remotes/IR, WSS, resume points, boot trim.

## Risk register

- **PV0 DPI interlace** (milestone 3): unproven on bare KMS; mitigations:
  vc4 register-level programming documented by the Linux driver, firmware
  `dpi_timings` interlace flag as cross-check, RGB-Pi/Pi2SCART prior art.
- **HDMI 15 kHz**: out of scope (adapter lottery on interlaced input).
- **Mixed-standard discs** (NTSC titles + PAL menus or vice versa): rare but
  legal; the presenter must support mode switch at VTS boundaries.
- **Buildroot symbol drift**: defconfig written against Buildroot 2025.02 LTS;
  expect minor renames if bumped.
