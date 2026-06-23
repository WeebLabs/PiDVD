# PiDVD UI — The Amber Picker

The on-screen interface for browsing and selecting DVD ISOs. Design doctrine:
this is not a media center. It is the front panel of a single-purpose hi-fi
appliance, rendered by a real CRT. The aesthetic is **warm amber phosphor** —
a vintage terminal that happens to play DVDs. Hierarchy comes from
*brightness* first; hue is an accent, spent sparingly (themeable, §2 — from
pure monochrome to amber-with-ice). No icons-from-a-set, no cover art, no
gradients. Text, rules, and glow.

A second doctrine: **the CRT is the renderer**. We don't fake scanlines,
bloom, or curvature — the display provides them. Our job is to feed it
pixel-honest 720-wide fields and respect interlace physics (below).

## 1. Canvas and interlace rules

- **Menu mode is fixed: 240p NTSC, always.** The picker is *always* a
  progressive 240p NTSC mode — one rock-steady, flicker-free menu that
  never changes while you browse. Playback is the only thing that
  modesets: when a disc starts, the nav engine switches to the disc's own
  native **480i / 576i** (chosen from the IFO video standard, not the
  region code); on STOP the picker re-opens 240p NTSC. The menu never
  follows the disc's standard — it's a constant.
- **How 240p is rendered.** The layout is authored once at 720×480 (the
  NTSC interlaced geometry) and the video backend decimates it 2:1 into
  the real 720×240 progressive scanout. So all UI code stays
  resolution-agnostic, and — because the menu is interlace-safe (below) —
  the 2:1 decimation is lossless: crisp native 240p. If the VEC can't
  offer 240p, the backend falls back to 480i rather than going dark.
- **Safe area**: all UI lives inside title-safe — 48 px horizontal
  margins, 32 px vertical → a working canvas of **624×416** at the 720×480
  authoring height. One layout, vertical regions flex.
- **Law #1 — no 1-line horizontals.** Any horizontal stroke, rule, or
  glyph row must span **≥ 2 authoring lines**. Originally an interlace
  rule (a 1-line feature would strobe between fields); it now *also*
  guarantees the 2:1 decimation to 240p loses nothing. Every font is
  rendered with each glyph row doubled (1 font pixel = 2 lines), box rules
  are 2 px.
- **Law #2 — vertical motion moves in 2-line steps.** The selection bar
  slide and any vertical animation step by even line counts (so they
  survive decimation cleanly). Horizontal motion (marquee) is
  unconstrained.
- **Brightness law**: no large fields of peak white/amber (CRT bloom +
  phosphor wear on an appliance that may sit on a menu for hours). The
  selection bar uses mid-amber fill; peak brightness is reserved for small
  text. The attract screen slowly pulses for the same reason.

## 2. Palette & themes

Every screen is drawn through eight **roles**; a theme is nothing but a
value table for them (a config option — no layout or code differences):

| Role     | Use |
|----------|-----|
| `BG`     | screen background |
| `PANEL`  | info-pane / header fill |
| `DIM`    | box rules, scrollbar track, labels, runtimes, disabled text |
| `TEXT`   | body text — list names, the workhorse tone |
| `BRIGHT` | metadata values, selected-disc name, peaks |
| `HOT`    | tiny accents only: logo glint, resume marker, spinner |
| `BAR`    | selection bar fill |
| `BARTXT` | selection bar text (inverse video) |

Hierarchy is always brightness-driven; accent hues are spent on *small*
things. Four themes (XRGB8888):

| Role     | **AMBER & ICE** (default) | **PHOSPHOR** (mono) | **VFD** | **MIDNIGHT** |
|----------|-----------|-----------|-----------|-----------|
| `BG`     | `#0D0A06` | `#0E0600` | `#03100D` | `#070B14` |
| `PANEL`  | `#161310` | `#1C0E00` | `#07201B` | `#0D1426` |
| `DIM`    | `#4E6A86` | `#6E3C00` | `#1F5A50` | `#32436B` |
| `TEXT`   | `#D98E00` | `#D97C00` | `#63D6BE` | `#8FB0E8` |
| `BRIGHT` | `#F4EFE2` | `#FF9A00` | `#D9FFF4` | `#EEF2FA` |
| `HOT`    | `#8FC6FF` | `#FFC29C` | `#FFB000` | `#FFB000` |
| `BAR`    | `#FFA000` | `#FF8C00` | `#49E0C2` | `#5B86DC` |
| `BARTXT` | `#1A0E00` | `#140900` | `#03201A` | `#060D1E` |

- **AMBER & ICE** — amber body text, warm-white values and peaks, steel-blue
  chrome and labels (amber's complement), ice-blue accents. Warm where it
  matters, cool where it recedes.
- **PHOSPHOR** — the pure-mono phosphor terminal, nudged slightly red of P3 amber. One phosphor, six
  intensities.
- **VFD** — vacuum-fluorescent hi-fi front panel: icy cyan body, white
  peaks, amber reserved for the accent pops (resume, glint) like an LED on
  a tuner.
- **MIDNIGHT** — the inverse pairing: blue-dominant with amber accents;
  closest to a '90s player OSD.

Compare them live: `scripts/ui-mock.sh` renders all four (or one by name).
Dithering to RGB666 for the VGA666 path is a non-issue at these levels —
all ramps survive 6-bit cleanly.

## 3. Typography

One bitmap font family, three sizes, all with row-doubling baked in:

| Style   | Cell (px) | Source glyph | Grid on 624 px | Use |
|---------|-----------|--------------|----------------|-----|
| `SMALL` | 8×16      | 8×8 @1×, rows ×2 | 78 cols | metadata values, key hints, footers |
| `LIST`  | 16×16     | 8×8 @2×2     | 39 cols | list rows, headers, body |
| `TITLE` | 32×32     | 8×8 @4×4     | 19 cols | selected-disc name, splash |

The 8×8 source font is custom-drawn (CP437-flavored, full ASCII + the few
glyphs we need: `▸ ◂ ▴ ▾ ◉ ⏎ ▮ ▯`), stored as a C array. At 2× the doubled
rows read as deliberate chunky scan-texture on the CRT — that's the look,
not a compromise.

## 4. The logo

Splash/attract form (drawn as pixels from the block font, shown here in its
ASCII source — `█` maps to a 4×4 px block, so the logo is 168×56 px):

```
 ██████▖ ▝█▘ ██████▖ ██▖  ▗██ ██████▖
 ██  ▝██ ▗▄▖ ██  ▝██ ▝██  ██▘ ██  ▝██
 ██▄▄██▘ ▐█▌ ██   ██  ▐█▙▟█▌  ██   ██
 ██▀▀▀   ▐█▌ ██   ██  ▝████▘  ██   ██
 ██      ▐█▌ ██  ▗██   ▝██▘   ██  ▗██
 ██      ▝█▘ ██████▘    ▝▘    ██████▘
─────────────────────────────────────
   F I E L D   A C C U R A T E   ·   1 5 k H z
```

The dot of the `i` is drawn in `HOT` — the single brightest pixel cluster on
the splash, like a power LED. Header form is just `◉ PiDVD` in `LIST` size,
where `◉` is the disc glyph that doubles as the activity spinner
(`◌ ◍ ◉ ◍` frames while scanning/loading).

## 5. Screens

### 5.1 ATTRACT — no source mounted

Logo centered, tagline under it, and below, pulsing at ~0.4 Hz between
`DIM` and `TEXT` (sinusoidal, never to black):

```
                I N S E R T   D I S C
              ▸ USB DRIVE  ·  DVD VIDEO ISO
```

USB hot-plug is the disc tray. The instant a drive mounts, a one-second
"tray" beat plays — the spinner runs `◌ ◍ ◉` while the catalog scan kicks
off — then cut to BROWSE.

### 5.2 BROWSE — the main screen (CONSOLE layout)

BROWSE has three **layouts** (§5.5) — like themes, a config swap with no
behavioral difference. CONSOLE is the reference layout; the others reuse
every rule defined here.

```
┌────────────────────────────────────────────────────────────────────┐
│ ◉ PiDVD            USB · /Action                          39 DISCS │
├────────────────────────────────────────────────────────────────────┤
│ ▸ NOW PLAYING  DIE HARD 2              ⏸ 01:12:33  ▮▮▮▮▮▯▯▯▯▯      │
├──────────────────────────────────────┬─────────────────────────────┤
│   ◂ ..                               │                             │
│   ▸ Box Sets                    12 ▸ │   DIE HARD                  │
│ ▌ ◉ Die Hard              2:08    ▐  │   DIE_HARD_SE_PAL           │
│   ◉ Die Hard 2            1:58       │                             │
│   ◉ Goldeneye             2:10       │   PAL · 576i · 16:9         │
│   ◉ Heat                  2:45       │   REGION 2 · 7.6 GB         │
│   ◉ Léon                  1:50       │                             │
│   ◉ Ronin                 2:01       │   4 TITLES · 28 CHAPTERS    │
│   ◉ Speed                 1:56      ░│   LONGEST   1:52:47         │
│                                     ░│                             │
│                                     ░│   AUDIO   AC-3 5.1  EN     │
│                                     ░│           AC-3 2.0  DE     │
│                                      │   SUBS    EN DE FR NL      │
│                                      │                             │
│                                      │   ⟳ RESUME AT 00:41:07      │
├──────────────────────────────────────┴─────────────────────────────┤
│ ▴▾ SELECT   ⏎ PLAY   ◂ BACK   ⏮ ⏭ PAGE   ■ EJECT                   │
└────────────────────────────────────────────────────────────────────┘
```

(Each `│ ─` rule is a 2 px line in `DIM`; the outer frame is the safe-area
boundary and is **not** drawn — shown here for layout only.)

- **Header**: logo glyph + name (`TEXT`), source + current directory path
  (center, `DIM`, path breadcrumbs), disc count right.
- **NOW PLAYING shelf** (only when a playback is suspended): pinned row,
  `BRIGHT` name, pause glyph, timecode, and an 10-cell `▮▯` progress gauge.
  `TITLE` key jumps focus to it from anywhere; ENTER resumes instantly.
- **List pane** (left, ~37 cols of `LIST`): directories first (`▸`, item
  count right-aligned), then ISOs (`◉`, runtime of the longest title
  right-aligned once scanned — until then a dim shimmer `░▒░` placeholder).
  Filenames are the display names (archivists name their files): strip
  `.iso`, `_`→space. Names wider than the column marquee-scroll **only on
  the selected row**, 2 px/field, with a 1 s hold at each end.
- **Selection bar**: full-row `BAR` fill, text in `BG` (inverse video),
  with `▌ ▐` end caps. Moving the selection slides the bar to the next row
  over 4 fields (eased, 2-line steps) — fast enough to never lag held-key
  repeat, alive enough to feel mechanical.
- **Scrollbar**: 4 px `DIM` track flush right of the list, `TEXT` thumb;
  appears only when the list overflows.
- **Info pane** (right, `PANEL` fill): the selected disc's card —
  - name (`TITLE` size, wrapped to 2 lines max),
  - volume ID under it in `SMALL` `DIM`,
  - badge line: standard + lines + aspect (`PAL · 576i · 16:9`, plus `LB`
    when letterboxed), then region + size,
  - structure: title count, chapter total, longest-title runtime,
  - audio streams (format, channel layout, language) and subtitle langs,
  - `⟳ RESUME AT hh:mm:ss` in `HOT` when a resume point exists.
  For a directory: item counts and total GB. While unscanned: the card
  shows name + size and `◍ READING DISC…` with the spinner.
- **Footer**: key hints in `SMALL`, `DIM` glyphs + `TEXT` labels. Always
  exactly one line; contextual (e.g. `⏎ OPEN` on a directory).

### 5.3 LAUNCH transition

ENTER on a disc: the list dims to `DIM`, the info card stays lit, the
spinner spins beside the name for the open/modeset beat (~1 s), then video.
It reads as "tray closed, disc spun up". No fades — brightness cuts, like
hardware.

### 5.4 Back from playback

`STOP` during playback suspends it and returns to BROWSE: position is
bookmarked, the NOW PLAYING shelf appears, selection lands on the suspended
disc. ENTER on the shelf (or the disc itself) reopens at the bookmark via
dvdnav title+time seek — "instant" to the user without keeping a paused
decode pipeline alive across a possible modeset. Starting a different disc
abandons the suspended one (its resume point persists). `STOP` pressed in
BROWSE ejects: unmounts the drive cleanly and returns to ATTRACT.

Resume points are persisted per-disc in `.pidvd/resume` at the drive root
(keyed by relpath+size, plain text). Read-only media: RAM only, lost at
power-off — acceptable.

### 5.5 Alternate BROWSE layouts

**MARQUEE** — the couch layout. A vertical carousel: the selected disc in
`TITLE` size dead-center with `▸ ◂` flanks, neighbors above/below in
`LIST` size fading `TEXT`→`DIM` with distance (two each side). One
`SMALL` info line below — the essentials only. Directories render as
`▸ BOX SETS · 12`. UP/DOWN slides the stack (2-line steps, eased);
selection wraps. Gorgeous at 3 m, best under ~50 discs per folder.

```
              ◉ PiDVD · USB · /Action · 39 DISCS

                         Goldeneye
                           Heat

                  ▸  D I E   H A R D  ◂

                           Léon
                          Ronin

      PAL · 576i · 16:9 · 2:08 · AC-3 5.1 · REGION 2 · ⟳

          II DIE HARD 2 · 01:12:33  ▮▮▮▮▮▯▯▯▯▯
            ▴▾ SELECT   ⏎ PLAY   ◂ BACK
```

(`⟳` marks a resume point; the `II` line is the NOW PLAYING shelf,
present only while something is suspended.)

**LEDGER** — the archivist layout. Full-width `SMALL`-font table, 17 rows
on PAL: name plus STD / ASPECT / LENGTH / SIZE columns, header row in
`DIM`. The selected disc's remaining card data (region, titles, audio,
subs) condenses into a single detail strip above the footer. Built for
hundred-disc collections; pairs with the catalog cache doing its job.

```
 ◉ PiDVD   USB · /Action                                  39 DISCS
 NAME                               STD    ASPECT   LENGTH    SIZE
 ▸ Box Sets                          ·       ·     12 ITEMS  18.2 GB
▌◉ Die Hard                         PAL    16:9      2:08     7.6 GB▐
 ◉ Die Hard 2                       PAL    16:9      1:58     6.8 GB
 ◉ Goldeneye                        PAL    16:9      2:10     7.1 GB
   ⋮
 DIE HARD · REGION 2 · 4 TITLES · AC-3 5.1 EN DE · SUBS EN DE FR NL
 ▴▾ SELECT   ⏎ PLAY   ◂ BACK   « » PAGE   ■ EJECT
```

Layout choice lives in SETTINGS, applies live, and is persisted. ATTRACT
and SETTINGS are layout-independent.

### 5.6 SETTINGS

Entered with `MENU` from the BROWSE root (where `MENU`/`LEFT` would
otherwise be a no-op — the footer hints `◂ SETTINGS` only at root) or
from ATTRACT. Same chrome as BROWSE; one centered panel:

```
   ⚙ SETTINGS

   THEME             ◂ AMBER & ICE ▸
   LAYOUT            ◂ CONSOLE ▸
   AUDIO OUTPUT      ◂ DISPLAY-SYNC PCM ▸
   ATTRACT DIM       ◂ AFTER 15 MIN ▸
   SCREENSAVER       ◂ WARP STARFIELD ▸
   COMP FILTER       ◂ 5 ▸
   RESCAN CATALOG    ⏎

   PIDVD 0.4 · 2025.02 · CRT NEVER LIES

   ▴▾ SELECT   ◂▸ CHANGE   ■ EXIT
```

- Labels `DIM`, values `BRIGHT`, selected row gets the bar treatment on
  the value only. LEFT/RIGHT cycle a value and it applies **instantly** —
  flipping themes/layouts live on the CRT is half the fun. ENTER fires
  action rows (rescan). MENU or STOP exits.
- Values: THEME (§2 four), LAYOUT (CONSOLE/MARQUEE/LEDGER), AUDIO DEVICE
  (AUTO — USB preferred, bcm2835 PWM fallback, never HDMI — or any connected
  card), VOLUME (0..100, the output card's UAC mixer), ATTRACT DIM
  (OFF / 5 / 15 / 30 MIN — blanks to the drifting logo bug, CRT burn-in
  kindness), SCREENSAVER (OFF / WARP STARFIELD / DVD LOGO — §5.7), COMP FILTER
  (OFF / 1..8, the menu's composite low-pass). There is *no* menu-mode setting:
  the menu is always 240p NTSC (§1). During playback, VOL± adjusts volume and
  SUB cycles subtitle tracks, each with a brief on-screen indicator.
- **Persistence**: `pidvd.cfg` (`key=value`) on the SD boot partition —
  the appliance's NVRAM, independent of whatever drive is inserted.
  Write path: remount rw → write → sync → remount ro; failure degrades
  to RAM for the session. The same file carries `last_standard` and the
  last-played disc for the shelf.

### 5.7 SCREENSAVER

After `PIDVD_SAVER_IDLE_SECONDS` (90 s, a code knob in `ui/saver.h` — not the
SETTINGS "ATTRACT DIM") with no input on any picker screen (ATTRACT, BROWSE
or SETTINGS), the selected screensaver paints over the screen. The first
keypress only wakes it (consumed); input resumes normally after. The
underlying screen keeps running while it's up, so waking lands exactly where
you left off. It never appears during playback (a separate path).

Both effects are pure, *stateless* pixel code in `ui/saver.c` — a frame is a
function of `(tick, theme, canvas)` only, so they slot into picker.c's
view-model-in/pixels-out render path with no per-frame state. Each has its
own block of tunables at the top of the file. The selection persists in
`pidvd.cfg` as `saver=`; the enum leaves room for more.

- **WARP STARFIELD** — a deep, perspective-correct (`1/z`) star tunnel: faint
  stars massed at the vanishing point that brighten, fatten and streak as
  they tear past, over a faint twinkling dust backdrop. Active-theme colours
  on black; "hero" stars burn `hot`. Tunables: density, speed, depth range,
  streak length, lens/spread, dust, roll/drift, speed "breathing".
- **DVD LOGO** — the classic bouncing "DVD Video" logo (the corner-hit
  ritual). The artwork is the public-domain Wikimedia Commons logo baked to
  an 8-bit alpha mask (`ui/dvdlogo_data.h`, regenerated by
  `tools/gen_dvdlogo.py`) and blitted tinted. Position is a triangle wave of
  the field counter and the colour is a function of the reflection count, so
  the bounce and per-bounce recolour are exact and stateless. Tunables:
  scale, per-axis speed, bounce-box inset, colour mode (theme cycle / classic
  vivid palette / rainbow / fixed), corner-hit background flash, drop shadow,
  and multiple simultaneous logos.

## 6. Input map (BROWSE context)

| `pidvd_key_t` | Action |
|---|---|
| `UP` / `DOWN` | move selection (held = repeat) |
| `RIGHT` / `ENTER` | open directory / play disc / resume shelf |
| `LEFT` / `MENU` | parent directory (at root: SETTINGS, §5.6) |
| `PREV/NEXT_CHAPTER` | page up / page down |
| `TITLE` | jump to NOW PLAYING shelf; press again: back to prior position |
| `PLAY_PAUSE` | same as ENTER on a disc |
| `STOP` | eject (unmount → ATTRACT) |
| `AUDIO` | cycle sort: NAME / RUNTIME / RECENT (toast in header) |
| `SUBTITLE`, `ANGLE` | unused (reserved) |

Five keys (arrows + enter) are sufficient; everything else is convenience.
The existing `/tmp/pidvd-ctl` FIFO drives all of this over ssh unchanged.

## 7. Catalog: how metadata gets there

Listing must be instant; IFO parsing must never block navigation.

- **Enumerate**: `readdir` for `*.iso` (case-insensitive) + directories.
  Sort, display immediately. Zero ISOs on the whole drive → ATTRACT with a
  `NO DVD ISOS FOUND` notice; exactly one → autoplay (locked UX decision);
  else BROWSE.
- **Scan thread** (core 0, low priority): walks visible-first, then the
  rest. Each ISO: `pidvd_disc_open()` → volume ID, titles, standard,
  aspect, coded size, audio/subs, longest runtime — IFO reads only, a few
  hundred KB per disc over USB 2, so ~5–10 discs/s typical.
- **Cache**: `.pidvd/catalog` at the drive root — one text record per ISO
  keyed by `(relpath, size, mtime)`. Re-plug of an unchanged drive = whole
  catalog hot in <100 ms with zero ISO opens. Unwritable drive → cache in
  RAM for the session.
- **Core addition needed**: region mask. It lives in the VMG IFO
  (`vmgi_mat->vmg_category`, bits 16–23, inverted prohibition mask) —
  expose as `uint8_t pidvd_disc_region_mask(d)`; the UI renders
  `REGION 2`, `REGION 1,2,4`, or `ALL`. Also worth surfacing then:
  `provider_id` (sometimes a nice studio string) as a `DIM` footnote.

## 8. Sources (SMB-ready, not SMB-yet)

The browser walks **sources**, each just a mounted root:
`/media/usb0`, `/media/usb1`, … later `/media/smb-<name>` (kernel CIFS
mount from a config file). One source: header shows `USB`. Multiple: the
BROWSE root level lists sources as rows (`▸ USB DRIVE 1`, `▸ NAS · dvds`)
and `LEFT` from a source root returns there. Nothing in the UI or catalog
code knows the difference — SMB later is a mount script, not a UI change.

## 9. Implementation shape

```
src/ui/font8.c        the 8×8 font + glyphs (generated C array)
src/ui/draw.c|h       fb primitives: rect, hline2, text at 1×/2×/4×,
                      marquee clip — all interlace-rule-enforcing
src/ui/catalog.c|h    enumerate, scan thread, sidecar cache, resume store
src/ui/settings.c|h   settings model + pidvd.cfg persistence (boot part.)
src/ui/picker.c|h     state machine (ATTRACT/BROWSE/SETTINGS/LAUNCH),
                      the three layouts, animation tick, input handling
```

- Owns `pidvd_video` while active: immediate-mode full redraw into
  `begin_frame()` per dirty event; a 50/60 Hz field tick runs only while an
  animation (pulse, spinner, marquee, bar slide) is live — otherwise the
  same frame is re-presented and the CPU sleeps. Redraw budget is trivial
  (one 720×576 fill + text).
- `main.c` becomes: mount-watch → picker → player → picker loop. The
  player keeps its existing in-play key handling; only `STOP` routes back.
- This is milestone-4 scope, but `draw.c` + `font8.c` are also exactly
  what the in-play OSD (via the presenter `blend_cb`) will use — build
  once, use twice.

A terminal preview of the BROWSE screen (24-bit color, any modern
terminal): `scripts/ui-mock.sh [amber-ice|phosphor|vfd|midnight]` — no
argument renders all four themes for comparison. For vibe checks without
a Pi.

## 10. Open questions

- Attract screen long-idle: blank to a drifting `◉ PiDVD` bug after N min
  (burn-in kindness on a menu-parked CRT)? Cheap, probably yes.
- `AUDIO`-key sort cycling: useful or clutter? Drop if unused in practice.
- NTSC canvas loses 84 lines — current layout flexes (list shows 8 rows
  instead of 11); revisit if info card ever grows.
