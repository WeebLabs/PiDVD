# PiDVD

## Deploy

Target Pi (LAN): **192.168.1.23** — `root@` over SSH with the baked-in key
`~/.ssh/pidvd_deploy`. Both scripts build in the `pidvd-builder` Docker
toolchain, push over the network, and (re)start the process — no SD-card shuffle.

- **Player** (UI / playback changes): `scripts/deploy.sh 192.168.1.23`
  - rebuilds the player package, pushes the binary, restarts playback.
  - add `--kernel` to also push zImage/DTBs/config/cmdline and reboot
    (only for kernel/boot changes, not normal UI work).
- **Web remote**: `scripts/remote-deploy.sh 192.168.1.23`
  - then open `http://192.168.1.23:8080/` on any LAN device.

## UI / themes

On-device picker UI is C in `player/src/ui/`. Themes live in
`player/src/ui/render.c` (`pidvd_themes[]`) + `render.h` (`PIDVD_N_THEMES`);
the SETTINGS menu names them in `player/src/ui/settings.c` (`theme_v[]`, row
count, and load-time modulo all track the theme count). Preview every screen
on the host before the CRT: build the `uipreview` target in `player/build`
and run it — it writes per-screen/per-theme PPMs. See `docs/UI.md`.
