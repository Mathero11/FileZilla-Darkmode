#include <math.h>
#include "colors.h"
#include "log.h"

/* Lightness ranges, 0..1. Backgrounds end up in [BG_MIN, BG_MAX], text in
   [FG_MIN, FG_MAX]. White -> #1C1C1C, button face -> #232323, black -> #E6E6E6. */
#define BG_MIN 0.11f
#define BG_MAX 0.35f
#define FG_MIN 0.65f
#define FG_MAX 0.90f

/* Light tinted backgrounds (the app's light blue and beige menus) turn into
   strong navy/olive when only their lightness is flipped. Keep a hint of the
   tint only. */
#define BG_SATURATION_KEEP 0.3f

/* HSL lightness is not perceptual: pure blue at L=0.65 is still hard to read
   on a dark background. Text colors are therefore also brightened until they
   reach this WCAG contrast ratio against the lightest dark background we use
   (selected rows, pressed controls). */
#define FG_MIN_CONTRAST 4.5f
#define FG_CONTRAST_BG RGB(0x35, 0x35, 0x35)

typedef struct {
    float h, s, l;
} hsl;

static float max3(float a, float b, float c) { return a > b ? (a > c ? a : c) : (b > c ? b : c); }
static float min3(float a, float b, float c) { return a < b ? (a < c ? a : c) : (b < c ? b : c); }

static hsl rgb_to_hsl(COLORREF c)
{
    float r = GetRValue(c) / 255.0f, g = GetGValue(c) / 255.0f, b = GetBValue(c) / 255.0f;
    float mx = max3(r, g, b), mn = min3(r, g, b), d = mx - mn;
    hsl x = { 0.0f, 0.0f, (mx + mn) / 2.0f };

    if (d > 0.0f) {
        x.s = x.l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
        if (mx == r)
            x.h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        else if (mx == g)
            x.h = (b - r) / d + 2.0f;
        else
            x.h = (r - g) / d + 4.0f;
        x.h /= 6.0f;
    }
    return x;
}

static float hue_to_rgb(float p, float q, float t)
{
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

static BYTE to_byte(float v)
{
    int i = (int)(v * 255.0f + 0.5f);
    return (BYTE)(i < 0 ? 0 : i > 255 ? 255 : i);
}

static COLORREF hsl_to_rgb(hsl x)
{
    if (x.s <= 0.0f) {
        BYTE v = to_byte(x.l);
        return RGB(v, v, v);
    }
    float q = x.l < 0.5f ? x.l * (1.0f + x.s) : x.l + x.s - x.l * x.s;
    float p = 2.0f * x.l - q;
    return RGB(to_byte(hue_to_rgb(p, q, x.h + 1.0f / 3.0f)),
               to_byte(hue_to_rgb(p, q, x.h)),
               to_byte(hue_to_rgb(p, q, x.h - 1.0f / 3.0f)));
}

/* CLR_NONE, CLR_DEFAULT, palette indexes etc. must pass through untouched. */
static BOOL is_plain_rgb(COLORREF c) { return (c & 0xFF000000) == 0; }

COLORREF dm_map_bg(COLORREF c)
{
    if (!is_plain_rgb(c))
        return c;
    hsl x = rgb_to_hsl(c);
    if (x.l >= 0.5f) {
        x.l = BG_MIN + (BG_MAX - BG_MIN) * 2.0f * (1.0f - x.l);
        x.s *= BG_SATURATION_KEEP;
    } else if (x.l > BG_MAX)
        x.l = BG_MAX;
    else
        return c;
    return hsl_to_rgb(x);
}

/* WCAG relative luminance of an sRGB color, 0..1. */
static float channel_to_linear(BYTE v)
{
    float c = v / 255.0f;
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float luminance(COLORREF c)
{
    return 0.2126f * channel_to_linear(GetRValue(c)) +
           0.7152f * channel_to_linear(GetGValue(c)) +
           0.0722f * channel_to_linear(GetBValue(c));
}

float dm_contrast(COLORREF a, COLORREF b)
{
    float la = luminance(a), lb = luminance(b);
    return la > lb ? (la + 0.05f) / (lb + 0.05f) : (lb + 0.05f) / (la + 0.05f);
}

/* 0xRRGGBB for "#%06X" in log lines; CLR_INVALID stays 0xFFFFFFFF. */
unsigned dm_rgb_hex(COLORREF c)
{
    if (c == CLR_INVALID)
        return 0xFFFFFFFFu;
    return (unsigned)GetRValue(c) << 16 | (unsigned)GetGValue(c) << 8 | GetBValue(c);
}

/* Logs each distinct text color mapping once, so the result can be checked. */
static void log_fg_mapping_once(COLORREF from, COLORREF to)
{
    static COLORREF seen[128];
    static int seen_count;
    static SRWLOCK lock = SRWLOCK_INIT;
    BOOL is_new = TRUE;

    AcquireSRWLockExclusive(&lock);
    for (int i = 0; i < seen_count && is_new; i++)
        is_new = seen[i] != from;
    if (is_new && seen_count < (int)(sizeof(seen) / sizeof(seen[0])))
        seen[seen_count++] = from;
    ReleaseSRWLockExclusive(&lock);

    if (is_new)
        dm_log(L"text color #%02X%02X%02X -> #%02X%02X%02X (contrast %.1f:1 on #353535)",
               GetRValue(from), GetGValue(from), GetBValue(from),
               GetRValue(to), GetGValue(to), GetBValue(to),
               (double)dm_contrast(to, FG_CONTRAST_BG));
}

COLORREF dm_map_fg_quiet(COLORREF c)
{
    if (!is_plain_rgb(c))
        return c;
    hsl x = rgb_to_hsl(c);
    if (x.l <= 0.5f)
        x.l = FG_MAX - (FG_MAX - FG_MIN) * 2.0f * x.l;
    else if (x.l < FG_MIN)
        x.l = FG_MIN;

    COLORREF out = hsl_to_rgb(x);
    while (dm_contrast(out, FG_CONTRAST_BG) < FG_MIN_CONTRAST && x.l < 0.98f) {
        x.l += 0.01f;
        out = hsl_to_rgb(x);
    }
    /* Rounding through HSL can shift an already fine color by one step;
       keep the original then, so mapping twice changes nothing. */
    if (dm_contrast(c, FG_CONTRAST_BG) >= FG_MIN_CONTRAST && rgb_to_hsl(c).l >= FG_MIN)
        out = c;
    return out;
}

COLORREF dm_map_fg(COLORREF c)
{
    COLORREF out = dm_map_fg_quiet(c);
    if (is_plain_rgb(c))
        log_fg_mapping_once(c, out);
    return out;
}

/* WCAG relative luminance is perceptual, so it decides what is "dark" in an
   image better than HSL lightness. */
COLORREF dm_map_invert(COLORREF c)
{
    if (!is_plain_rgb(c))
        return c;
    return luminance(c) >= 0.18f ? dm_map_bg(c) : dm_map_fg_quiet(c);
}

/* Chroma (max - min channel) instead of HSL saturation: near-black pixels
   have a high HSL saturation from tiny channel differences, but are ink. */
#define INK_MAX_CHROMA 40
#define INK_MAX_LIGHTNESS 0.35f

COLORREF dm_map_ink(COLORREF c)
{
    if (!is_plain_rgb(c))
        return c;
    BYTE r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    BYTE mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    BYTE mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (mx - mn > INK_MAX_CHROMA || (mx + mn) / 510.0f > INK_MAX_LIGHTNESS)
        return c;
    return dm_map_fg_quiet(c);
}

/* ---- brushes ----------------------------------------------------------- */

#define BRUSH_CACHE_SIZE 256

static struct {
    COLORREF color;
    HBRUSH brush;
} g_brushes[BRUSH_CACHE_SIZE];
static int g_brush_count;
static SRWLOCK g_brush_lock = SRWLOCK_INIT;

HBRUSH dm_brush(COLORREF c)
{
    HBRUSH br = NULL;

    AcquireSRWLockExclusive(&g_brush_lock);
    for (int i = 0; i < g_brush_count; i++) {
        if (g_brushes[i].color == c) {
            br = g_brushes[i].brush;
            break;
        }
    }
    if (!br) {
        br = CreateSolidBrush(c);
        /* Brushes handed out must stay valid, so a full cache is never
           evicted; past that point brushes simply are not cached. */
        if (br && g_brush_count < BRUSH_CACHE_SIZE) {
            g_brushes[g_brush_count].color = c;
            g_brushes[g_brush_count].brush = br;
            g_brush_count++;
        }
    }
    ReleaseSRWLockExclusive(&g_brush_lock);
    return br;
}

BOOL dm_is_hollow_brush(HBRUSH br)
{
    LOGBRUSH lb;
    if (!br)
        return TRUE;
    return GetObjectW(br, sizeof(lb), &lb) == sizeof(lb) && lb.lbStyle == BS_HOLLOW;
}

HBRUSH dm_map_brush(HBRUSH br)
{
    LOGBRUSH lb;
    if (!br || GetObjectW(br, sizeof(lb), &lb) != sizeof(lb) || lb.lbStyle != BS_SOLID)
        return br;
    COLORREF mapped = dm_map_bg(lb.lbColor);
    return mapped == lb.lbColor ? br : dm_brush(mapped);
}
