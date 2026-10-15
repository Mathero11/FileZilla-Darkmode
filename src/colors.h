#ifndef DM_COLORS_H
#define DM_COLORS_H

#include <windows.h>

/*
 * Color mapping from the app's light colors to dark ones. Hue and saturation
 * are kept, only lightness changes. Both functions are idempotent, so a color
 * that passes through them twice does not change any further.
 */

/* Backgrounds: light colors become dark, dark ones stay (capped). */
COLORREF dm_map_bg(COLORREF c);

/* Text and lines: dark colors become light, light ones stay (floored), and
   every result has at least 4.5:1 contrast on the dark backgrounds. */
COLORREF dm_map_fg(COLORREF c);

/* Same as dm_map_fg, without logging (for bitmap pixels). */
COLORREF dm_map_fg_quiet(COLORREF c);

/* For images with dark symbols on a light background: light pixels are mapped
   as background, dark pixels as foreground, so symbols stay visible. */
COLORREF dm_map_invert(COLORREF c);

/* For icons: only "ink" (dark, colorless pixels such as black text, outlines
   and arrows) becomes light. Colored and light pixels stay as they are. */
COLORREF dm_map_ink(COLORREF c);

/* WCAG contrast ratio between two colors, 1..21. */
float dm_contrast(COLORREF a, COLORREF b);

/* 0xRRGGBB for "#%06X" in log lines; CLR_INVALID stays 0xFFFFFFFF. */
unsigned dm_rgb_hex(COLORREF c);

/* Solid brush for a color. Cached and never deleted. */
HBRUSH dm_brush(COLORREF c);

/* Solid brush with its color mapped through dm_map_bg; other brushes
   (hollow, pattern, NULL) are returned unchanged. */
HBRUSH dm_map_brush(HBRUSH br);

/* NULL or hollow brush: the control wants a transparent background. */
BOOL dm_is_hollow_brush(HBRUSH br);

/* Fixed colors for things the DLL draws itself. */
#define DM_TEXT            RGB(0xE6, 0xE6, 0xE6) /* = dm_map_fg(black) */
#define DM_TEXT_DISABLED   RGB(0x80, 0x80, 0x80)
#define DM_BORDER          RGB(0x5A, 0x5A, 0x5A)
#define DM_CONTROL         RGB(0x33, 0x33, 0x33)
#define DM_CONTROL_PRESSED RGB(0x4A, 0x4A, 0x4A)
#define DM_HEADER          RGB(0x2B, 0x2B, 0x2B)

/* Cut Box frame of Find Image (screen capture). It is drawn over arbitrary
   screen content, so it needs a color that stands out on white and on black
   alike: relative luminance ~0.179 gives 4.6:1 against both. */
#define DM_CAPTURE_FRAME   RGB(0xE5, 0x00, 0x72)

#endif
