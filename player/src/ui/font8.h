/* 8x8 bitmap font for the picker UI. ASCII core derived from the
 * public-domain font8x8 (IBM VGA lineage); UI glyphs ours. Glyph row =
 * one byte, bit 0 = leftmost pixel. Rendered with each row doubled or
 * better — a 1-scanline stroke flickers on interlace (docs/UI.md §1). */
#ifndef PIDVD_UI_FONT8_H
#define PIDVD_UI_FONT8_H

#include <stdint.h>

extern const unsigned char pidvd_font8[96][8]; /* 0x20..0x7F */

/* Glyph for a Unicode codepoint: ASCII from the table above, the UI's
 * special glyphs (arrows, disc, gauge, ...), Latin-1 letters folded to
 * their base letter. NULL if unmapped (caller skips or draws 0x7F). */
const unsigned char *pidvd_font8_glyph(uint32_t cp);

/* Decode one UTF-8 codepoint, advancing *s. Invalid bytes yield U+FFFD
 * and advance by one. */
uint32_t pidvd_utf8_next(const char **s);

#endif
