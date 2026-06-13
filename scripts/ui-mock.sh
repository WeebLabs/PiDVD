#!/usr/bin/env bash
# Terminal preview of the PiDVD picker (docs/UI.md) — 24-bit color mockup
# of the BROWSE screen for vibe checks without a Pi. Needs a truecolor
# terminal; layout assumes single-width rendering of the UI glyph set.
#
#   scripts/ui-mock.sh              all four themes
#   scripts/ui-mock.sh amber-ice    one theme (amber-ice|phosphor|vfd|midnight)

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

render() {
    echo
    say "               ${B}██████▖ ${H}▝█▘${B} ██████▖ ██▖  ▗██ ██████▖"
    say "               ${B}██  ▝██ ▗▄▖ ██  ▝██ ▝██  ██▘ ██  ▝██"
    say "               ${B}██▄▄██▘ ▐█▌ ██   ██  ▐█▙▟█▌  ██   ██"
    say "               ${B}██▀▀▀   ▐█▌ ██   ██  ▝████▘  ██   ██"
    say "               ${B}██      ▐█▌ ██  ▗██   ▝██▘   ██  ▗██"
    say "               ${B}██      ▝█▘ ██████▘    ▝▘    ██████▘${X}"
    say "          ${D}F I E L D   A C C U R A T E   ·   1 5 k H z${X}"
    echo
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

if [ -n "$1" ]; then
    set_theme "$1"
    render
else
    for t in amber-ice phosphor vfd midnight; do
        set_theme "$t"
        printf '\n  \e[1m%s\e[0m\n' "── THEME: $(echo "$t" | tr 'a-z-' 'A-Z ') ──"
        render
    done
fi
