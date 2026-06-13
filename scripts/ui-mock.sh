#!/usr/bin/env bash
# Terminal preview of the PiDVD picker (docs/UI.md) — 24-bit color mockup
# for vibe checks without a Pi. Needs a truecolor terminal; layout assumes
# single-width rendering of the UI glyph set.
#
#   scripts/ui-mock.sh                          the full tour
#   scripts/ui-mock.sh <theme>                  console browse, one theme
#   scripts/ui-mock.sh <theme> <screen>         one theme, one screen
#
#   themes:  amber-ice phosphor vfd midnight
#   screens: console marquee ledger settings

X=$'\e[0m'
fg() { printf '\e[38;2;%d;%d;%dm' "$((16#${1:0:2}))" "$((16#${1:2:2}))" "$((16#${1:4:2}))"; }
bg() { printf '\e[48;2;%d;%d;%dm' "$((16#${1:0:2}))" "$((16#${1:2:2}))" "$((16#${1:4:2}))"; }

# theme <DIM> <TEXT> <BRIGHT> <HOT> <BAR> <BARTXT> <PANEL>   (docs/UI.md §2)
theme() {
    D=$(fg "$1"); T=$(fg "$2"); B=$(fg "$3"); H=$(fg "$4")
    SB=$(bg "$5"); SF=$(fg "$6"); PB=$(bg "$7")
}
set_theme() {
    case "$1" in
    amber-ice) theme 4E6A86 D98E00 F4EFE2 8FC6FF FFA000 1A0E00 161310 ;;
    phosphor)  theme 6E4400 D98E00 FFB000 FFDE9C FFA000 140A00 1C1000 ;;
    vfd)       theme 1F5A50 63D6BE D9FFF4 FFB000 49E0C2 03201A 07201B ;;
    midnight)  theme 32436B 8FB0E8 EEF2FA FFB000 5B86DC 060D1E 0D1426 ;;
    *) echo "unknown theme: $1 (amber-ice|phosphor|vfd|midnight)" >&2; exit 1 ;;
    esac
}

H68=$(printf '─%.0s' {1..68})
H38=$(printf '─%.0s' {1..38})
H29=$(printf '─%.0s' {1..29})

say() { printf '  %s\n' "$*"; }
row() { say "${D}│$1${X}${D}│${PB}$2${X}${D}│${X}"; }

logo() {
    echo
    say "               ${B}██████▖ ${H}▝█▘${B} ██████▖ ██▖  ▗██ ██████▖"
    say "               ${B}██  ▝██ ▗▄▖ ██  ▝██ ▝██  ██▘ ██  ▝██"
    say "               ${B}██▄▄██▘ ▐█▌ ██   ██  ▐█▙▟█▌  ██   ██"
    say "               ${B}██▀▀▀   ▐█▌ ██   ██  ▝████▘  ██   ██"
    say "               ${B}██      ▐█▌ ██  ▗██   ▝██▘   ██  ▗██"
    say "               ${B}██      ▝█▘ ██████▘    ▝▘    ██████▘${X}"
    say "          ${D}F I E L D   A C C U R A T E   ·   1 5 k H z${X}"
    echo
}

render_console() {
    logo
    say "${D}┌${H68}┐${X}"
    say "${D}│${H} ◉ ${B}PiDVD${T}                   ${D}USB · ${T}/Action                   ${D}39 DISCS │${X}"
    say "${D}├${H68}┤${X}"
    say "${D}│ ▸ NOW PLAYING  ${B}DIE HARD 2${T}          II 01:12:33  ${B}▮▮▮▮▮${D}▯▯▯▯▯         │${X}"
    say "${D}├${H38}┬${H29}┤${X}"
    row "${T}  ◂ ..                                "  '                             '
    row "${T}  ▸ Box Sets                      ${D}12  "  "${B}  DIE HARD                   "
    row "${SB}${SF}  ◉ Die Hard                    2:08  "  "${D}  DIE_HARD_SE_PAL            "
    row "${T}  ◉ Die Hard 2                  ${D}1:58  "  '                             '
    row "${T}  ◉ Goldeneye                   ${D}2:10  "  "${B}  PAL ${D}· ${B}576i ${D}· ${B}16:9          "
    row "${T}  ◉ Heat                        ${D}2:45  "  "${B}  REGION 2 ${D}· ${B}7.6 GB          "
    row "${T}  ◉ Léon                        ${D}1:50  "  '                             '
    row "${T}  ◉ Ronin                       ${D}2:01  "  "${B}  4 TITLES ${D}· ${B}28 CHAPTERS     "
    row "${T}  ◉ Speed                       ${D}1:56  "  "${D}  LONGEST  ${B}1:52:47           "
    row "${T}  ◉ The Long Kiss Goodnight     ${D}2:00  "  '                             '
    row '                                      '  "${D}  AUDIO  ${B}AC-3 5.1  EN        "
    row '                                      '  "${B}         AC-3 2.0  DE        "
    row '                                      '  "${D}  SUBS   ${B}EN DE FR NL         "
    row '                                      '  "${H}  ⟳ RESUME AT 00:41:07       "
    say "${D}├${H38}┴${H29}┤${X}"
    say "${D}│${B} ▴▾ ${D}SELECT   ${B}↵ ${D}PLAY   ${B}◂ ${D}BACK   ${B}« » ${D}PAGE   ${B}■ ${D}EJECT                   │${X}"
    say "${D}└${H68}┘${X}"
    echo
}

render_marquee() {
    logo
    say "                  ${H}◉ ${B}PiDVD ${D}· USB · ${T}/Action ${D}· 39 DISCS${X}"
    echo
    say "                             ${D}Goldeneye${X}"
    say "                               ${T}Heat${X}"
    echo
    say "                      ${B}▸  D I E   H A R D  ◂${X}"
    echo
    say "                               ${T}Léon${X}"
    say "                              ${D}Ronin${X}"
    echo
    say "          ${B}PAL ${D}· ${B}576i ${D}· ${B}16:9 ${D}· ${B}2:08 ${D}· ${B}AC-3 5.1 ${D}· ${B}REGION 2 ${D}· ${H}⟳${X}"
    echo
    say "              ${D}II ${B}DIE HARD 2 ${D}· ${T}01:12:33  ${B}▮▮▮▮▮${D}▯▯▯▯▯${X}"
    say "                  ${B}▴▾ ${D}SELECT   ${B}↵ ${D}PLAY   ${B}◂ ${D}BACK${X}"
    echo
}

lrow() { say "${T} ◉ $1${X}"; }
render_ledger() {
    echo
    say "${H} ◉ ${B}PiDVD   ${D}USB · ${T}/Action                                    ${D}39 DISCS${X}"
    say "${D} NAME                               STD    ASPECT   LENGTH     SIZE  ${X}"
    say "${T} ▸ Box Sets                          ${D}·      ·     12 ITEMS     18.2 GB${X}"
    say "${SB}${SF} ◉ Die Hard                         PAL    16:9       2:08     7.6 GB${X}"
    say "${T} ◉ Die Hard 2                       ${D}PAL    16:9       1:58     6.8 GB${X}"
    say "${T} ◉ Goldeneye                        ${D}PAL    16:9       2:10     7.1 GB${X}"
    say "${T} ◉ Heat                             ${D}PAL    16:9       2:45     7.9 GB${X}"
    say "${T} ◉ Léon                             ${D}PAL    16:9       1:50     7.4 GB${X}"
    say "${T} ◉ Ronin                            ${D}PAL    16:9       2:01     6.9 GB${X}"
    say "${T} ◉ Speed                            ${D}NTSC   16:9       1:56     7.0 GB${X}"
    say "${T} ◉ The Long Kiss Goodnight          ${D}PAL    16:9       2:00     7.2 GB${X}"
    say "${T} ◉ True Lies                        ${D}PAL    16:9       2:21     7.8 GB${X}"
    say "${D}   ⋮${X}"
    say "${B} DIE HARD ${D}· ${B}REGION 2 ${D}· ${B}4 TITLES ${D}· ${B}AC-3 5.1 EN DE ${D}· ${B}SUBS EN DE FR NL${X}"
    say "${B} ▴▾ ${D}SELECT   ${B}↵ ${D}PLAY   ${B}◂ ${D}BACK   ${B}« » ${D}PAGE   ${B}■ ${D}EJECT${X}"
    echo
}

render_settings() {
    echo
    say "   ${H}◉ ${B}SETTINGS${X}"
    echo
    say "   ${D}THEME             ${SB}${SF} ◂ AMBER & ICE ▸ ${X}"
    say "   ${D}LAYOUT             ${B}◂ CONSOLE ▸${X}"
    say "   ${D}PICKER MODE        ${B}◂ AUTO · LAST DISC ▸${X}"
    say "   ${D}AUDIO OUTPUT       ${B}◂ STEREO DOWNMIX ▸${X}"
    say "   ${D}ATTRACT DIM        ${B}◂ AFTER 15 MIN ▸${X}"
    say "   ${D}RESCAN CATALOG     ${B}↵${X}"
    echo
    say "   ${D}PIDVD 0.4 · 2025.02 · CRT NEVER LIES${X}"
    echo
    say "   ${B}▴▾ ${D}SELECT   ${B}◂▸ ${D}CHANGE   ${B}■ ${D}EXIT${X}"
    echo
}

render() {
    case "$1" in
    console)  render_console ;;
    marquee)  render_marquee ;;
    ledger)   render_ledger ;;
    settings) render_settings ;;
    *) echo "unknown screen: $1 (console|marquee|ledger|settings)" >&2; exit 1 ;;
    esac
}

banner() { printf '\n  \e[1m── %s ──\e[0m\n' "$*"; }

if [ $# -ge 2 ]; then
    set_theme "$1"; render "$2"
elif [ $# -eq 1 ]; then
    set_theme "$1"; render_console
else
    for t in amber-ice phosphor vfd midnight; do
        set_theme "$t"
        banner "CONSOLE · $(echo "$t" | tr 'a-z-' 'A-Z ')"
        render_console
    done
    set_theme amber-ice
    banner "MARQUEE · AMBER ICE";  render_marquee
    banner "LEDGER · AMBER ICE";   render_ledger
    banner "SETTINGS · AMBER ICE"; render_settings
fi
