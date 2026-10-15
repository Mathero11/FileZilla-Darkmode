/*
 * Standard Win32 / common controls.
 *
 * Most controls only need a dark theme (DarkMode_Explorer / DarkMode_CFD) and
 * their colors mapped. Check boxes, radio buttons, group boxes, up-down
 * arrows, column headers, tab controls and status bars are painted here,
 * because Windows 10 has no dark theme for them (or their text ignores the
 * colors from WM_CTLCOLOR*). The message log (rich edit) gets dark colors.
 *
 * wxWidgets subclasses its controls after they are created, so its window
 * procedure runs before ours. Some wx controls paint into their own memory DC
 * and pass it in WM_PAINT's wParam (wxStaticBox, wxNotebook); painting must
 * then go into that DC.
 */
#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <uxtheme.h>
#include <vssym32.h>
#include "colors.h"
#include "controls.h"
#include "dark_api.h"
#include "log.h"
#include "wx_hooks.h"

/* ---- classification ---------------------------------------------------- */

static const struct {
    const wchar_t *name;
    dm_kind kind;
} g_classes[] = {
    { L"#32770", DM_KIND_DIALOG },
    { L"Button", DM_KIND_BUTTON },
    { L"Edit", DM_KIND_EDIT },
    { L"Static", DM_KIND_STATIC },
    { L"ComboBox", DM_KIND_COMBOBOX },
    { L"ComboLBox", DM_KIND_COMBOLBOX },
    { L"ListBox", DM_KIND_LISTBOX },
    { WC_LISTVIEWW, DM_KIND_LISTVIEW },
    { WC_HEADERW, DM_KIND_HEADER },
    { WC_TREEVIEWW, DM_KIND_TREEVIEW },
    { UPDOWN_CLASSW, DM_KIND_UPDOWN },
    { TOOLTIPS_CLASSW, DM_KIND_TOOLTIP },
    { L"ScrollBar", DM_KIND_SCROLLBAR },
    { L"RICHEDIT50W", DM_KIND_RICHEDIT },
    { L"RichEdit20W", DM_KIND_RICHEDIT },
    { WC_TABCONTROLW, DM_KIND_TAB },
    { L"_wx_SysTabCtl32", DM_KIND_TAB }, /* wxNotebook's copy of the tab control class */
    { STATUSCLASSNAMEW, DM_KIND_STATUSBAR },
    { L"IME", DM_KIND_SKIP },
    { L"Wine IME", DM_KIND_SKIP },
    { L"MSCTFIME UI", DM_KIND_SKIP },
    { L"OleMainThreadWndClass", DM_KIND_SKIP },
};

dm_kind dm_classify(HWND hwnd)
{
    wchar_t name[64];
    if (!GetClassNameW(hwnd, name, 64))
        return DM_KIND_SKIP;
    for (size_t i = 0; i < sizeof(g_classes) / sizeof(g_classes[0]); i++)
        if (lstrcmpiW(name, g_classes[i].name) == 0)
            return g_classes[i].kind;
    return DM_KIND_WINDOW;
}

/* ---- helpers ----------------------------------------------------------- */

static void set_theme(HWND hwnd, const wchar_t *app_name)
{
    if (g_api.SetWindowTheme)
        g_api.SetWindowTheme(hwnd, app_name, NULL);
}

/* Double buffer for flicker-free painting. */
typedef struct {
    HDC target, dc;
    HBITMAP bitmap, old_bitmap;
    RECT rc;
} buffer;

static HDC buffer_begin(buffer *b, HDC target, const RECT *rc)
{
    b->target = target;
    b->rc = *rc;
    b->dc = CreateCompatibleDC(target);
    b->bitmap = CreateCompatibleBitmap(target, rc->right - rc->left, rc->bottom - rc->top);
    if (!b->dc || !b->bitmap) {
        if (b->dc)
            DeleteDC(b->dc);
        if (b->bitmap)
            DeleteObject(b->bitmap);
        b->dc = NULL;
        return target;
    }
    b->old_bitmap = SelectObject(b->dc, b->bitmap);
    SetViewportOrgEx(b->dc, -rc->left, -rc->top, NULL);
    return b->dc;
}

static void buffer_end(buffer *b)
{
    if (!b->dc)
        return;
    SetViewportOrgEx(b->dc, 0, 0, NULL);
    BitBlt(b->target, b->rc.left, b->rc.top, b->rc.right - b->rc.left, b->rc.bottom - b->rc.top,
           b->dc, 0, 0, SRCCOPY);
    SelectObject(b->dc, b->old_bitmap);
    DeleteObject(b->bitmap);
    DeleteDC(b->dc);
}

/* Child windows paint themselves; without WS_CLIPCHILDREN our full repaint
   would wipe them until their next repaint. */
static BOOL CALLBACK exclude_child(HWND child, LPARAM lp)
{
    HDC hdc = (HDC)lp;
    HWND parent = GetParent(child);
    if (IsWindowVisible(child)) {
        RECT rc;
        GetWindowRect(child, &rc);
        MapWindowPoints(NULL, parent, (POINT *)&rc, 2);
        ExcludeClipRect(hdc, rc.left, rc.top, rc.right, rc.bottom);
    }
    return TRUE;
}

static void exclude_children(HWND hwnd, HDC hdc)
{
    for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        exclude_child(c, (LPARAM)hdc);
}

/* WM_PAINT for a control painted by 'paint'. With a DC in wParam (wx paints
   into its own memory DC) that DC is used, otherwise BeginPaint. */
static LRESULT handle_paint(HWND hwnd, WPARAM wp, void (*paint)(HWND, HDC), BOOL clip_children)
{
    if (wp) {
        paint(hwnd, (HDC)wp);
        return 0;
    }
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (clip_children)
        exclude_children(hwnd, hdc);
    paint(hwnd, hdc);
    EndPaint(hwnd, &ps);
    return 0;
}

/* Background brush the parent wants for a static-like control, mapped dark.
   Also leaves the parent's (mapped) text color in hdc. */
static HBRUSH parent_static_brush(HWND hwnd, HDC hdc)
{
    HBRUSH br = (HBRUSH)SendMessageW(GetParent(hwnd), WM_CTLCOLORSTATIC, (WPARAM)hdc, (LPARAM)hwnd);
    SetTextColor(hdc, dm_map_fg(GetTextColor(hdc)));
    return dm_map_brush(br);
}

static void fill_parent_background(HWND hwnd, HDC hdc, HBRUSH br, const RECT *rc)
{
    if (!dm_is_hollow_brush(br))
        FillRect(hdc, rc, br);
    else if (g_api.DrawThemeParentBackground)
        g_api.DrawThemeParentBackground(hwnd, hdc, rc);
    else
        FillRect(hdc, rc, dm_brush(dm_dark_sys_color(COLOR_BTNFACE)));
}

static wchar_t *get_text(HWND hwnd, wchar_t *stack_buf, int stack_len)
{
    int len = GetWindowTextLengthW(hwnd);
    wchar_t *buf = len < stack_len ? stack_buf : HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(wchar_t));
    if (!buf)
        return NULL;
    GetWindowTextW(hwnd, buf, len + 1);
    return buf;
}

static void free_text(wchar_t *buf, wchar_t *stack_buf)
{
    if (buf && buf != stack_buf)
        HeapFree(GetProcessHeap(), 0, buf);
}

static HGDIOBJ select_control_font(HWND hwnd, HDC hdc)
{
    HFONT font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
    return font ? SelectObject(hdc, font) : NULL;
}

/* Image lists belong to the comctl32 that created them (v6 here), so draw
   with that comctl32: the module of the control's class window procedure.
   (Not the class's module: wxWidgets registers copies of the comctl32
   classes under its own name, e.g. _wx_SysTabCtl32.) */
typedef BOOL(WINAPI *ImageList_Draw_t)(HIMAGELIST, int, HDC, int, int, UINT);
typedef BOOL(WINAPI *ImageList_GetIconSize_t)(HIMAGELIST, int *, int *);

static void *comctl_proc(HWND control, const char *name)
{
    HMODULE comctl = NULL;
    void *wndproc = (void *)GetClassLongPtrW(control, GCLP_WNDPROC);
    if (!wndproc || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        (LPCWSTR)wndproc, &comctl))
        return NULL;
    return (void *)GetProcAddress(comctl, name);
}

static BOOL draw_image(HWND control, HIMAGELIST il, int index, HDC hdc, int x, int y)
{
    ImageList_Draw_t draw = (ImageList_Draw_t)comctl_proc(control, "ImageList_Draw");
    return il && index >= 0 && draw && draw(il, index, hdc, x, y, ILD_TRANSPARENT);
}

static SIZE image_size(HWND control, HIMAGELIST il)
{
    SIZE sz = { 0, 0 };
    ImageList_GetIconSize_t get = (ImageList_GetIconSize_t)comctl_proc(control, "ImageList_GetIconSize");
    int cx, cy;
    if (il && get && get(il, &cx, &cy)) {
        sz.cx = cx;
        sz.cy = cy;
    }
    return sz;
}

/* FileZilla is "system DPI aware": everything runs at the DPI of the
   session (125%, 150%, ...), Windows bitmap-scales it on other monitors.
   Our own fixed sizes (margins, arrows) scale with that DPI. */
int dm_scale(int px)
{
    static int dpi;
    if (!dpi) {
        HDC screen = GetDC(NULL);
        dpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
        if (screen)
            ReleaseDC(NULL, screen);
        if (dpi <= 0)
            dpi = 96;
    }
    return MulDiv(px, dpi, 96);
}

/* Filled polygon with smooth (anti-aliased) edges: drawn 4x larger over a
   copy of what is below it and scaled down with HALFTONE, like the check
   box glyph. Points are in logical units of hdc. */
static void fill_polygon_aa(HDC hdc, const POINT *p, int n, COLORREF color)
{
    enum { SCALE = 4, MAX_POINTS = 8 };
    if (n < 3 || n > MAX_POINTS)
        return;
    RECT box = { p[0].x, p[0].y, p[0].x, p[0].y };
    for (int i = 1; i < n; i++) {
        if (p[i].x < box.left) box.left = p[i].x;
        if (p[i].y < box.top) box.top = p[i].y;
        if (p[i].x > box.right) box.right = p[i].x;
        if (p[i].y > box.bottom) box.bottom = p[i].y;
    }
    InflateRect(&box, 1, 1);
    int w = box.right - box.left, h = box.bottom - box.top;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = mem ? CreateCompatibleBitmap(hdc, w * SCALE, h * SCALE) : NULL;
    if (!bmp) {
        if (mem)
            DeleteDC(mem);
        return;
    }
    HGDIOBJ old_bmp = SelectObject(mem, bmp);
    SetStretchBltMode(mem, COLORONCOLOR);
    StretchBlt(mem, 0, 0, w * SCALE, h * SCALE, hdc, box.left, box.top, w, h, SRCCOPY);

    /* Pixel centers: point (x, y) is the middle of pixel x, y. */
    POINT big[MAX_POINTS];
    for (int i = 0; i < n; i++) {
        big[i].x = (p[i].x - box.left) * SCALE + SCALE / 2;
        big[i].y = (p[i].y - box.top) * SCALE + SCALE / 2;
    }
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ old_brush = SelectObject(mem, brush);
    HGDIOBJ old_pen = SelectObject(mem, GetStockObject(NULL_PEN));
    Polygon(mem, big, n);
    SelectObject(mem, old_pen);
    SelectObject(mem, old_brush);
    DeleteObject(brush);

    int old_mode = SetStretchBltMode(hdc, HALFTONE);
    POINT old_org;
    SetBrushOrgEx(hdc, 0, 0, &old_org); /* required after switching to HALFTONE */
    StretchBlt(hdc, box.left, box.top, w, h, mem, 0, 0, w * SCALE, h * SCALE, SRCCOPY);
    SetBrushOrgEx(hdc, old_org.x, old_org.y, NULL);
    SetStretchBltMode(hdc, old_mode);

    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

/* Triangle pointing in direction dir (0 up, 1 down, 2 left, 3 right),
   'size' = half its width, centered on cx, cy. */
static void paint_triangle(HDC hdc, int cx, int cy, int size, int dir, COLORREF color)
{
    int half = size / 2 > 0 ? size / 2 : 1;
    POINT p[3];
    switch (dir) {
    case 0: p[0].x = cx - size; p[0].y = cy + half; p[1].x = cx + size; p[1].y = cy + half; p[2].x = cx; p[2].y = cy - half; break;
    case 1: p[0].x = cx - size; p[0].y = cy - half; p[1].x = cx + size; p[1].y = cy - half; p[2].x = cx; p[2].y = cy + half; break;
    case 2: p[0].x = cx + half; p[0].y = cy - size; p[1].x = cx + half; p[1].y = cy + size; p[2].x = cx - half; p[2].y = cy; break;
    default: p[0].x = cx - half; p[0].y = cy - size; p[1].x = cx - half; p[1].y = cy + size; p[2].x = cx + half; p[2].y = cy; break;
    }
    fill_polygon_aa(hdc, p, 3, color);
}

void dm_paint_arrow(HDC hdc, const RECT *r, int dir, COLORREF color)
{
    int w = r->right - r->left, h = r->bottom - r->top;
    int size = (w < h ? w : h) / 4;
    if (size < 2)
        size = 2;
    paint_triangle(hdc, r->left + w / 2, r->top + h / 2, size, dir, color);
}

/* ---- buttons ----------------------------------------------------------- */

static BOOL is_check_or_radio(LONG style)
{
    if (style & (BS_PUSHLIKE | BS_BITMAP | BS_ICON))
        return FALSE;
    switch (style & BS_TYPEMASK) {
    case BS_CHECKBOX:
    case BS_AUTOCHECKBOX:
    case BS_RADIOBUTTON:
    case BS_AUTORADIOBUTTON:
    case BS_3STATE:
    case BS_AUTO3STATE:
        return TRUE;
    }
    return FALSE;
}

static BOOL is_groupbox(LONG style) { return (style & BS_TYPEMASK) == BS_GROUPBOX; }

static BOOL is_custom_painted_button(HWND hwnd)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    return is_check_or_radio(style) || is_groupbox(style);
}

static UINT text_format(HWND hwnd, LONG style)
{
    UINT fmt = (style & BS_MULTILINE) ? DT_WORDBREAK : DT_SINGLELINE | DT_VCENTER;
    switch (style & BS_CENTER) {
    case BS_CENTER: fmt |= DT_CENTER; break;
    case BS_RIGHT: fmt |= DT_RIGHT; break;
    default: fmt |= DT_LEFT; break;
    }
    if (SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL)
        fmt |= DT_HIDEPREFIX;
    return fmt;
}

/* Drawn 4x larger and scaled down with HALFTONE, which gives smooth
   (anti-aliased) edges with plain GDI. */
void dm_draw_check_glyph(HDC hdc, const RECT *box, BOOL radio, int check, int sub)
{
    enum { SCALE = 4 };
    int w = box->right - box->left, h = box->bottom - box->top;
    if (w <= 0 || h <= 0)
        return;
    int big_w = w * SCALE, big_h = h * SCALE;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = mem ? CreateCompatibleBitmap(hdc, big_w, big_h) : NULL;
    if (!bmp) {
        if (mem)
            DeleteDC(mem);
        return;
    }
    HGDIOBJ old_bmp = SelectObject(mem, bmp);

    /* Start from what is already painted below the glyph, so the smoothed
       edge blends into it. */
    SetStretchBltMode(mem, COLORONCOLOR);
    StretchBlt(mem, 0, 0, big_w, big_h, hdc, box->left, box->top, w, h, SRCCOPY);

    COLORREF fill, border, mark;
    switch (sub) {
    case 2: fill = RGB(0x33, 0x33, 0x33); border = RGB(0xB0, 0xB0, 0xB0); mark = DM_TEXT; break;
    case 3: fill = RGB(0x3C, 0x3C, 0x3C); border = RGB(0xB0, 0xB0, 0xB0); mark = DM_TEXT; break;
    case 4: fill = RGB(0x26, 0x26, 0x26); border = RGB(0x4F, 0x4F, 0x4F); mark = RGB(0x6E, 0x6E, 0x6E); break;
    default: fill = RGB(0x2B, 0x2B, 0x2B); border = RGB(0x8A, 0x8A, 0x8A); mark = DM_TEXT; break;
    }

    HPEN pen = CreatePen(PS_SOLID, SCALE, border); /* 1px after scaling */
    HBRUSH brush = CreateSolidBrush(fill);
    HGDIOBJ old_pen = SelectObject(mem, pen);
    HGDIOBJ old_brush = SelectObject(mem, brush);
    int inset = SCALE / 2;
    if (radio)
        Ellipse(mem, inset, inset, big_w - inset, big_h - inset);
    else
        RoundRect(mem, inset, inset, big_w - inset, big_h - inset, 3 * SCALE, 3 * SCALE);

    if (check == BST_CHECKED && radio) {
        HBRUSH dot = CreateSolidBrush(mark);
        SelectObject(mem, GetStockObject(NULL_PEN));
        SelectObject(mem, dot);
        int cx = big_w / 2, cy = big_h / 2, r = big_w / 5;
        Ellipse(mem, cx - r, cy - r, cx + r + 1, cy + r + 1);
        SelectObject(mem, brush);
        DeleteObject(dot);
    } else if (check == BST_CHECKED) {
        HPEN tick = CreatePen(PS_SOLID, SCALE * 3 / 2 + 1, mark);
        SelectObject(mem, tick);
        POINT p[3] = { { big_w * 22 / 100, big_h * 52 / 100 },
                       { big_w * 42 / 100, big_h * 72 / 100 },
                       { big_w * 78 / 100, big_h * 30 / 100 } };
        Polyline(mem, p, 3);
        SelectObject(mem, pen);
        DeleteObject(tick);
    } else if (check == BST_INDETERMINATE && !radio) {
        RECT square = { big_w * 28 / 100, big_h * 28 / 100, big_w * 72 / 100, big_h * 72 / 100 };
        FillRect(mem, &square, dm_brush(mark));
    }

    SelectObject(mem, old_pen);
    SelectObject(mem, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);

    int old_mode = SetStretchBltMode(hdc, HALFTONE);
    POINT old_org;
    SetBrushOrgEx(hdc, 0, 0, &old_org); /* required after switching to HALFTONE */
    StretchBlt(hdc, box->left, box->top, w, h, mem, 0, 0, big_w, big_h, SRCCOPY);
    SetBrushOrgEx(hdc, old_org.x, old_org.y, NULL);
    SetStretchBltMode(hdc, old_mode);

    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static void paint_check_or_radio(HWND hwnd, HDC hdc, LONG style)
{
    RECT rc;
    GetClientRect(hwnd, &rc);

    HBRUSH br = parent_static_brush(hwnd, hdc);
    COLORREF text = GetTextColor(hdc);
    fill_parent_background(hwnd, hdc, br, &rc);

    HGDIOBJ old_font = select_control_font(hwnd, hdc);

    LRESULT state = SendMessageW(hwnd, BM_GETSTATE, 0, 0);
    BOOL enabled = IsWindowEnabled(hwnd);
    UINT type = style & BS_TYPEMASK;
    BOOL radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    int check = (int)(state & 3);

    /* Theme state ids: 1..4 unchecked, 5..8 checked, 9..12 mixed (checkbox
       only), each as normal/hot/pressed/disabled. */
    int base = check == BST_CHECKED ? 4 : (check == BST_INDETERMINATE && !radio) ? 8 : 0;
    int sub = !enabled ? 4 : (state & BST_PUSHED) ? 3 : (state & BST_HOT) ? 2 : 1;
    int part = radio ? BP_RADIOBUTTON : BP_CHECKBOX;

    HTHEME theme = g_api.OpenThemeData ? g_api.OpenThemeData(hwnd, L"Button") : NULL;
    SIZE glyph = { dm_scale(13), dm_scale(13) };
    if (theme && g_api.GetThemePartSize)
        g_api.GetThemePartSize(theme, hdc, part, base + sub, NULL, TS_DRAW, &glyph);

    BOOL glyph_right = (style & BS_RIGHTBUTTON) != 0;
    RECT box;
    box.left = glyph_right ? rc.right - glyph.cx : rc.left;
    switch (style & BS_VCENTER) {
    case BS_TOP: box.top = rc.top; break;
    case BS_BOTTOM: box.top = rc.bottom - glyph.cy; break;
    default: box.top = (rc.top + rc.bottom - glyph.cy) / 2; break;
    }
    box.right = box.left + glyph.cx;
    box.bottom = box.top + glyph.cy;

    /* The theme only provides the size: Windows 10 has no dark glyphs. */
    if (theme)
        g_api.CloseThemeData(theme);
    dm_draw_check_glyph(hdc, &box, radio, check, sub);

    RECT tr = rc;
    int gap = glyph.cx / 3;
    if (glyph_right)
        tr.right = box.left - gap;
    else
        tr.left = box.right + gap;

    wchar_t stack_buf[256];
    wchar_t *label = get_text(hwnd, stack_buf, 256);
    if (label && *label) {
        UINT fmt = text_format(hwnd, style);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, enabled ? text : DM_TEXT_DISABLED);

        RECT measured = tr;
        DrawTextW(hdc, label, -1, &measured, fmt | DT_CALCRECT);
        if (style & BS_MULTILINE) {
            /* DT_VCENTER only works for single lines: center by hand. */
            int h = measured.bottom - measured.top;
            tr.top = (tr.top + tr.bottom - h) / 2;
            tr.bottom = tr.top + h;
        }
        DrawTextW(hdc, label, -1, &tr, fmt);

        if ((state & BST_FOCUS) && !(SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            int h = measured.bottom - measured.top;
            RECT focus = { tr.left - 1, (tr.top + tr.bottom - h) / 2 - 1,
                           tr.left + (measured.right - measured.left) + 1, 0 };
            focus.bottom = focus.top + h + 2;
            SetTextColor(hdc, DM_TEXT);
            DrawFocusRect(hdc, &focus);
        }
    }
    free_text(label, stack_buf);

    if (old_font)
        SelectObject(hdc, old_font);
}

static void paint_groupbox(HWND hwnd, HDC hdc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);

    HBRUSH br = parent_static_brush(hwnd, hdc);
    COLORREF text = GetTextColor(hdc);

    HGDIOBJ old_font = select_control_font(hwnd, hdc);

    TEXTMETRICW tm;
    GetTextMetricsW(hdc, &tm);

    /* Frame through the middle of the caption line. */
    RECT frame = rc;
    frame.top += tm.tmHeight / 2;
    FrameRect(hdc, &frame, dm_brush(DM_BORDER));

    wchar_t stack_buf[256];
    wchar_t *label = get_text(hwnd, stack_buf, 256);
    if (label && *label) {
        UINT fmt = DT_SINGLELINE | DT_LEFT | DT_TOP;
        if (SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL)
            fmt |= DT_HIDEPREFIX;
        RECT tr = { rc.left + tm.tmAveCharWidth, rc.top, rc.right - tm.tmAveCharWidth, rc.top + tm.tmHeight };
        RECT measured = tr;
        DrawTextW(hdc, label, -1, &measured, fmt | DT_CALCRECT);
        measured.left -= 2;
        measured.right += 2;
        fill_parent_background(hwnd, hdc, br, &measured);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, IsWindowEnabled(hwnd) ? text : DM_TEXT_DISABLED);
        DrawTextW(hdc, label, -1, &tr, fmt);
    }
    free_text(label, stack_buf);

    if (old_font)
        SelectObject(hdc, old_font);
}

static void paint_button(HWND hwnd, HDC hdc)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (is_groupbox(style)) {
        /* Group boxes are transparent inside: paint directly, no buffer. */
        paint_groupbox(hwnd, hdc);
        return;
    }
    RECT rc;
    GetClientRect(hwnd, &rc);
    buffer b;
    HDC dc = buffer_begin(&b, hdc, &rc);
    paint_check_or_radio(hwnd, dc, style);
    buffer_end(&b);
}

/* The button control also paints itself outside WM_PAINT (on state changes,
   e.g. in Wine); repaint right after those messages. */
static BOOL button_may_self_paint(UINT msg)
{
    switch (msg) {
    case BM_SETCHECK: case BM_SETSTATE: case BM_CLICK: case BM_SETSTYLE:
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE: case WM_SETTEXT:
    case WM_UPDATEUISTATE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_MOUSEMOVE: case WM_MOUSELEAVE: case WM_KEYDOWN: case WM_KEYUP:
    case WM_CAPTURECHANGED:
        return TRUE;
    }
    return FALSE;
}

static BOOL button_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    if (!is_custom_painted_button(hwnd))
        return FALSE;

    switch (msg) {
    case WM_PAINT:
        *result = handle_paint(hwnd, wp, paint_button, FALSE);
        return TRUE;
    case WM_PRINTCLIENT:
        paint_button(hwnd, (HDC)wp);
        *result = 0;
        return TRUE;
    case WM_ERASEBKGND:
        *result = 1;
        return TRUE;
    }

    if (button_may_self_paint(msg)) {
        LRESULT before = SendMessageW(hwnd, BM_GETSTATE, 0, 0);
        *result = g_api.DefSubclassProc(hwnd, msg, wp, lp);
        LRESULT after = SendMessageW(hwnd, BM_GETSTATE, 0, 0);
        BOOL mouse = msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE;
        if (!mouse || before != after)
            RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        return TRUE;
    }
    return FALSE;
}

/* ---- up-down (spin) ---------------------------------------------------- */

static void paint_updown(HWND hwnd, HDC target)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    buffer b;
    HDC hdc = buffer_begin(&b, target, &rc);

    BOOL horz = (GetWindowLongW(hwnd, GWL_STYLE) & UDS_HORZ) != 0;
    BOOL enabled = IsWindowEnabled(hwnd);
    RECT first = rc, second = rc;
    if (horz)
        first.right = second.left = (rc.left + rc.right) / 2;
    else
        first.bottom = second.top = (rc.top + rc.bottom) / 2;

    /* Pressed half: the control captures the mouse while a button is held. */
    int pressed = -1;
    if (GetCapture() == hwnd && (GetKeyState(VK_LBUTTON) & 0x8000)) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        pressed = PtInRect(&first, pt) ? 0 : PtInRect(&second, pt) ? 1 : -1;
    }

    FillRect(hdc, &rc, dm_brush(DM_CONTROL));
    if (pressed == 0)
        FillRect(hdc, &first, dm_brush(DM_CONTROL_PRESSED));
    if (pressed == 1)
        FillRect(hdc, &second, dm_brush(DM_CONTROL_PRESSED));
    FrameRect(hdc, &rc, dm_brush(DM_BORDER));

    COLORREF arrow = enabled ? DM_TEXT : DM_TEXT_DISABLED;
    /* Vertical: top half increments (up arrow). Horizontal: left half decrements. */
    dm_paint_arrow(hdc, &first, horz ? 2 : 0, arrow);
    dm_paint_arrow(hdc, &second, horz ? 3 : 1, arrow);

    buffer_end(&b);
}

static BOOL updown_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    switch (msg) {
    case WM_PAINT:
        *result = handle_paint(hwnd, wp, paint_updown, FALSE);
        return TRUE;
    case WM_PRINTCLIENT:
        paint_updown(hwnd, (HDC)wp);
        *result = 0;
        return TRUE;
    case WM_ERASEBKGND:
        *result = 1;
        return TRUE;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_MOUSEMOVE:
    case WM_CAPTURECHANGED: case WM_ENABLE: case WM_TIMER:
        *result = g_api.DefSubclassProc(hwnd, msg, wp, lp);
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        return TRUE;
    }
    return FALSE;
}

/* ---- header (list view column titles) ---------------------------------- */

/* Owner-drawn or bitmap columns are left to the control (and its dark theme). */
static BOOL header_is_plain(HWND hwnd)
{
    int count = (int)SendMessageW(hwnd, HDM_GETITEMCOUNT, 0, 0);
    for (int i = 0; i < count; i++) {
        HDITEMW item = { 0 };
        item.mask = HDI_FORMAT;
        if (SendMessageW(hwnd, HDM_GETITEMW, i, (LPARAM)&item) && (item.fmt & (HDF_OWNERDRAW | HDF_BITMAP)))
            return FALSE;
    }
    return TRUE;
}

/* Sort triangle at the right end of the column, vertically centered (at the
   top, as the light theme does, it runs into the text of low headers).
   Returns the space it takes. */
static int paint_sort_arrow(HDC hdc, const RECT *tr, BOOL up)
{
    int size = dm_scale(4);
    paint_triangle(hdc, tr->right - size, (tr->top + tr->bottom) / 2, size, up ? 0 : 1, RGB(0xA0, 0xA0, 0xA0));
    return 2 * size + dm_scale(4);
}

static void paint_header(HWND hwnd, HDC target)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    buffer b;
    HDC hdc = buffer_begin(&b, target, &rc);

    FillRect(hdc, &rc, dm_brush(DM_HEADER));
    RECT bottom = { rc.left, rc.bottom - 1, rc.right, rc.bottom };
    FillRect(hdc, &bottom, dm_brush(DM_BORDER));

    HGDIOBJ old_font = select_control_font(hwnd, hdc);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, IsWindowEnabled(hwnd) ? DM_TEXT : DM_TEXT_DISABLED);
    HIMAGELIST il = (HIMAGELIST)SendMessageW(hwnd, HDM_GETIMAGELIST, 0, 0);

    int count = (int)SendMessageW(hwnd, HDM_GETITEMCOUNT, 0, 0);
    for (int i = 0; i < count; i++) {
        RECT ir;
        if (!SendMessageW(hwnd, HDM_GETITEMRECT, i, (LPARAM)&ir))
            continue;

        wchar_t text[256] = L"";
        HDITEMW item = { 0 };
        item.mask = HDI_TEXT | HDI_FORMAT | HDI_IMAGE;
        item.pszText = text;
        item.cchTextMax = 256;
        SendMessageW(hwnd, HDM_GETITEMW, i, (LPARAM)&item);

        RECT tr = ir;
        InflateRect(&tr, -dm_scale(6), 0);

        if ((item.fmt & HDF_IMAGE) && il && item.iImage >= 0) {
            SIZE sz = image_size(hwnd, il);
            int y = (ir.top + ir.bottom - sz.cy) / 2, x;
            if (item.fmt & HDF_BITMAP_ON_RIGHT) {
                x = tr.right - sz.cx;
                tr.right = x - dm_scale(4);
            } else {
                x = tr.left;
                tr.left += sz.cx + dm_scale(4);
            }
            draw_image(hwnd, il, item.iImage, hdc, x, y);
        }
        if (item.fmt & (HDF_SORTUP | HDF_SORTDOWN))
            tr.right -= paint_sort_arrow(hdc, &tr, (item.fmt & HDF_SORTUP) != 0);

        UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
        fmt |= (item.fmt & HDF_JUSTIFYMASK) == HDF_RIGHT    ? DT_RIGHT
               : (item.fmt & HDF_JUSTIFYMASK) == HDF_CENTER ? DT_CENTER
                                                            : DT_LEFT;
        DrawTextW(hdc, text, -1, &tr, fmt);

        RECT sep = { ir.right - 1, ir.top + dm_scale(3), ir.right, ir.bottom - dm_scale(4) };
        FillRect(hdc, &sep, dm_brush(DM_BORDER));
    }

    if (old_font)
        SelectObject(hdc, old_font);
    buffer_end(&b);
}

static BOOL header_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    (void)lp;
    switch (msg) {
    case WM_PAINT:
        if (!header_is_plain(hwnd))
            return FALSE;
        *result = handle_paint(hwnd, wp, paint_header, FALSE);
        return TRUE;
    case WM_PRINTCLIENT:
        if (!header_is_plain(hwnd))
            return FALSE;
        paint_header(hwnd, (HDC)wp);
        *result = 0;
        return TRUE;
    case WM_ERASEBKGND:
        *result = 1;
        return TRUE;
    }
    return FALSE;
}

/* ---- tab control (wxNotebook) -------------------------------------------- */

/* Only the plain layout is painted: tabs on top, no buttons, not owner-drawn. */
static BOOL tab_is_plain(HWND hwnd)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    return !(style & (TCS_BOTTOM | TCS_VERTICAL | TCS_BUTTONS | TCS_OWNERDRAWFIXED));
}

static void paint_tab(HWND hwnd, HDC target)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    buffer b;
    HDC hdc = buffer_begin(&b, target, &rc);

    COLORREF face = dm_dark_sys_color(COLOR_BTNFACE);
    FillRect(hdc, &rc, dm_brush(face));

    int count = (int)SendMessageW(hwnd, TCM_GETITEMCOUNT, 0, 0);
    int sel = (int)SendMessageW(hwnd, TCM_GETCURSEL, 0, 0);
    HIMAGELIST il = (HIMAGELIST)SendMessageW(hwnd, TCM_GETIMAGELIST, 0, 0);

    /* The page frame starts below the lowest tab row. */
    LONG pane_top = rc.top;
    for (int i = 0; i < count; i++) {
        RECT ir;
        if (SendMessageW(hwnd, TCM_GETITEMRECT, i, (LPARAM)&ir) && ir.bottom > pane_top)
            pane_top = ir.bottom;
    }
    RECT pane = { rc.left, pane_top, rc.right, rc.bottom };
    FrameRect(hdc, &pane, dm_brush(DM_BORDER));

    HGDIOBJ old_font = select_control_font(hwnd, hdc);
    SetBkMode(hdc, TRANSPARENT);
    BOOL enabled = IsWindowEnabled(hwnd);

    for (int i = 0; i < count; i++) {
        RECT ir;
        if (!SendMessageW(hwnd, TCM_GETITEMRECT, i, (LPARAM)&ir))
            continue;
        BOOL selected = i == sel;
        if (selected) {
            /* The selected tab is a bit larger and merges into the page. */
            InflateRect(&ir, dm_scale(2), 0);
            ir.top -= dm_scale(2);
            ir.bottom += 1;
        }
        FillRect(hdc, &ir, dm_brush(selected ? face : DM_HEADER));
        RECT left = { ir.left, ir.top, ir.left + 1, ir.bottom };
        RECT top = { ir.left, ir.top, ir.right, ir.top + 1 };
        RECT right = { ir.right - 1, ir.top, ir.right, ir.bottom };
        FillRect(hdc, &left, dm_brush(DM_BORDER));
        FillRect(hdc, &top, dm_brush(DM_BORDER));
        FillRect(hdc, &right, dm_brush(DM_BORDER));

        wchar_t text[256] = L"";
        TCITEMW item = { 0 };
        item.mask = TCIF_TEXT | TCIF_IMAGE;
        item.pszText = text;
        item.cchTextMax = 256;
        SendMessageW(hwnd, TCM_GETITEMW, i, (LPARAM)&item);

        RECT tr = ir;
        if (il && item.iImage >= 0) {
            SIZE sz = image_size(hwnd, il);
            draw_image(hwnd, il, item.iImage, hdc, tr.left + dm_scale(6), (tr.top + tr.bottom - sz.cy) / 2);
            tr.left += sz.cx + dm_scale(6);
        }
        SetTextColor(hdc, !enabled ? DM_TEXT_DISABLED : DM_TEXT);
        UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS;
        if (SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL)
            fmt |= DT_HIDEPREFIX;
        DrawTextW(hdc, text, -1, &tr, fmt);
    }

    if (old_font)
        SelectObject(hdc, old_font);
    buffer_end(&b);
}

static BOOL tab_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    (void)lp;
    if (!tab_is_plain(hwnd))
        return FALSE;
    switch (msg) {
    case WM_PAINT:
        *result = handle_paint(hwnd, wp, paint_tab, TRUE);
        return TRUE;
    case WM_PRINTCLIENT:
        paint_tab(hwnd, (HDC)wp);
        *result = 0;
        return TRUE;
    case WM_ERASEBKGND:
        *result = 1;
        return TRUE;
    }
    return FALSE;
}

/* ---- status bar ------------------------------------------------------------ */

/* A part's text may hold up to three tab-separated pieces: left, centered
   and right aligned. */
static void draw_status_text(HDC hdc, const wchar_t *text, RECT tr)
{
    static const UINT align[3] = { DT_LEFT, DT_CENTER, DT_RIGHT };
    const wchar_t *piece = text;
    for (int n = 0; n < 3 && piece; n++) {
        const wchar_t *tab = wcschr(piece, L'\t');
        int len = tab ? (int)(tab - piece) : -1;
        if (len != 0)
            DrawTextW(hdc, piece, len, &tr, align[n] | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        piece = tab ? tab + 1 : NULL;
    }
}

static void paint_statusbar(HWND hwnd, HDC target)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    buffer b;
    HDC hdc = buffer_begin(&b, target, &rc);

    FillRect(hdc, &rc, dm_brush(dm_dark_sys_color(COLOR_BTNFACE)));
    RECT line = { rc.left, rc.top, rc.right, rc.top + 1 };
    FillRect(hdc, &line, dm_brush(DM_BORDER));

    HGDIOBJ old_font = select_control_font(hwnd, hdc);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, DM_TEXT);

    int parts = (int)SendMessageW(hwnd, SB_GETPARTS, 0, 0);
    if (parts > 64)
        parts = 64;
    for (int i = 0; i < parts; i++) {
        RECT pr;
        if (!SendMessageW(hwnd, SB_GETRECT, i, (LPARAM)&pr))
            continue;
        LRESULT lt = SendMessageW(hwnd, SB_GETTEXTLENGTHW, i, 0);
        int len = LOWORD(lt);
        if (!(HIWORD(lt) & SBT_OWNERDRAW) && len > 0) {
            wchar_t stack_buf[256];
            wchar_t *text = len < 256 ? stack_buf : HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(wchar_t));
            if (text) {
                text[0] = 0;
                SendMessageW(hwnd, SB_GETTEXTW, i, (LPARAM)text);
                RECT tr = pr;
                InflateRect(&tr, -dm_scale(4), 0);
                draw_status_text(hdc, text, tr);
                if (text != stack_buf)
                    HeapFree(GetProcessHeap(), 0, text);
            }
        }
        if (i < parts - 1) {
            RECT sep = { pr.right - 1, pr.top + dm_scale(3), pr.right, pr.bottom - dm_scale(3) };
            FillRect(hdc, &sep, dm_brush(DM_BORDER));
        }
    }

    /* Size grip: a triangle of dots in the corner. */
    if ((GetWindowLongW(hwnd, GWL_STYLE) & SBARS_SIZEGRIP) && !IsZoomed(GetAncestor(hwnd, GA_ROOT))) {
        int dot = dm_scale(2);
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col <= row; col++) {
                int x = rc.right - (col + 1) * dot * 2, y = rc.bottom - (3 - row) * dot * 2;
                RECT d = { x, y, x + dot, y + dot };
                FillRect(hdc, &d, dm_brush(RGB(0x80, 0x80, 0x80)));
            }
        }
    }

    if (old_font)
        SelectObject(hdc, old_font);
    buffer_end(&b);
}

static BOOL statusbar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    switch (msg) {
    case WM_PAINT:
        *result = handle_paint(hwnd, wp, paint_statusbar, TRUE);
        return TRUE;
    case WM_PRINTCLIENT:
        paint_statusbar(hwnd, (HDC)wp);
        *result = 0;
        return TRUE;
    case WM_ERASEBKGND:
        *result = 1;
        return TRUE;
    case SB_SETTEXTW:
    case SB_SETTEXTA:
    case SB_SETPARTS:
    case WM_SIZE:
        /* The control repaints only the changed part, inside its own code. */
        *result = g_api.DefSubclassProc(hwnd, msg, wp, lp);
        InvalidateRect(hwnd, NULL, FALSE);
        return TRUE;
    }
    return FALSE;
}

/* ---- message log (rich edit) ---------------------------------------------- */

/* A rich edit control ignores WM_CTLCOLOR*: it paints with the system colors
   unless told otherwise. FileZilla sets its log colors per message type and
   leaves "Status:" lines on the default (automatic) color. */
static void darken_richedit(HWND hwnd)
{
    g_api.DefSubclassProc(hwnd, EM_SETBKGNDCOLOR, 0, dm_dark_sys_color(COLOR_WINDOW));
    CHARFORMAT2W cf = { 0 };
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = dm_dark_sys_color(COLOR_WINDOWTEXT);
    g_api.DefSubclassProc(hwnd, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
    g_api.DefSubclassProc(hwnd, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
}

static BOOL richedit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    switch (msg) {
    case EM_SETBKGNDCOLOR:
        /* wParam != 0: "use the system color", which is the light one here. */
        *result = g_api.DefSubclassProc(hwnd, msg, 0,
                                        wp ? dm_dark_sys_color(COLOR_WINDOW) : dm_map_bg((COLORREF)lp));
        return TRUE;

    case EM_SETCHARFORMAT: {
        const CHARFORMAT2W *in = (const CHARFORMAT2W *)lp;
        if (!in || (in->cbSize != sizeof(CHARFORMATW) && in->cbSize != sizeof(CHARFORMAT2W)) ||
            !(in->dwMask & (CFM_COLOR | CFM_BACKCOLOR)))
            return FALSE;
        CHARFORMAT2W cf = { 0 };
        CopyMemory(&cf, in, in->cbSize);
        if (cf.dwMask & CFM_COLOR) {
            /* Text colors get at least 4.5:1, like everything else. */
            if (cf.dwEffects & CFE_AUTOCOLOR) {
                cf.dwEffects &= ~CFE_AUTOCOLOR;
                cf.crTextColor = dm_dark_sys_color(COLOR_WINDOWTEXT);
            } else {
                cf.crTextColor = dm_map_fg(cf.crTextColor);
            }
        }
        if (cf.cbSize == sizeof(CHARFORMAT2W) && (cf.dwMask & CFM_BACKCOLOR) && !(cf.dwEffects & CFE_AUTOBACKCOLOR))
            cf.crBackColor = dm_map_bg(cf.crBackColor);
        *result = g_api.DefSubclassProc(hwnd, msg, wp, (LPARAM)&cf);
        return TRUE;
    }
    }
    return FALSE;
}

/* ---- list view / tree view colors ---------------------------------------- */

/* A list view paints CLR_DEFAULT text and text background with the system
   colors (white rows, black text) inside comctl32, where our hooks do not
   reach. So CLR_DEFAULT is replaced by the dark system color. */
static COLORREF map_lv_text_bk(COLORREF c)
{
    return c == CLR_DEFAULT ? dm_dark_sys_color(COLOR_WINDOW) : dm_map_bg(c);
}

static COLORREF map_lv_text(COLORREF c)
{
    return c == CLR_DEFAULT ? dm_dark_sys_color(COLOR_WINDOWTEXT) : dm_map_fg(c);
}

static void darken_listview(HWND hwnd)
{
    g_api.DefSubclassProc(hwnd, LVM_SETBKCOLOR, 0,
                          dm_map_bg((COLORREF)SendMessageW(hwnd, LVM_GETBKCOLOR, 0, 0)));
    g_api.DefSubclassProc(hwnd, LVM_SETTEXTBKCOLOR, 0,
                          map_lv_text_bk((COLORREF)SendMessageW(hwnd, LVM_GETTEXTBKCOLOR, 0, 0)));
    g_api.DefSubclassProc(hwnd, LVM_SETTEXTCOLOR, 0,
                          map_lv_text((COLORREF)SendMessageW(hwnd, LVM_GETTEXTCOLOR, 0, 0)));
}

static void darken_treeview(HWND hwnd)
{
    COLORREF bk = (COLORREF)SendMessageW(hwnd, TVM_GETBKCOLOR, 0, 0);
    COLORREF text = (COLORREF)SendMessageW(hwnd, TVM_GETTEXTCOLOR, 0, 0);
    if (bk == (COLORREF)-1)
        bk = GetSysColor(COLOR_WINDOW);
    if (text == (COLORREF)-1)
        text = GetSysColor(COLOR_WINDOWTEXT);
    g_api.DefSubclassProc(hwnd, TVM_SETBKCOLOR, 0, dm_map_bg(bk));
    g_api.DefSubclassProc(hwnd, TVM_SETTEXTCOLOR, 0, dm_map_fg(text));
    g_api.DefSubclassProc(hwnd, TVM_SETLINECOLOR, 0, DM_BORDER);
}

/* Colors the app sets later are mapped on the way in. */
static BOOL color_message_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, dm_kind kind, LRESULT *result)
{
    COLORREF (*map)(COLORREF) = NULL;

    if (kind == DM_KIND_LISTVIEW) {
        if (msg == LVM_SETBKCOLOR)
            map = dm_map_bg;
        else if (msg == LVM_SETTEXTBKCOLOR)
            map = map_lv_text_bk;
        else if (msg == LVM_SETTEXTCOLOR)
            map = map_lv_text;
    } else if (kind == DM_KIND_TREEVIEW) {
        if (msg == TVM_SETBKCOLOR)
            map = dm_map_bg;
        else if (msg == TVM_SETTEXTCOLOR || msg == TVM_SETLINECOLOR)
            map = dm_map_fg;
    }
    if (!map || (COLORREF)lp == (COLORREF)-1)
        return FALSE;
    *result = g_api.DefSubclassProc(hwnd, msg, wp, (LPARAM)map((COLORREF)lp));
    return TRUE;
}

/* ---- public ------------------------------------------------------------ */

void dm_control_created(HWND hwnd, dm_kind kind)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);

    switch (kind) {
    case DM_KIND_BUTTON:
        /* Push buttons: native dark theme. Check boxes etc. are painted here,
           but still use the dark theme for their glyphs. */
        set_theme(hwnd, L"DarkMode_Explorer");
        if (is_check_or_radio(style) || is_groupbox(style))
            InvalidateRect(hwnd, NULL, TRUE);
        break;
    case DM_KIND_EDIT:
    case DM_KIND_COMBOBOX:
        set_theme(hwnd, L"DarkMode_CFD");
        break;
    case DM_KIND_LISTVIEW:
        set_theme(hwnd, L"DarkMode_Explorer");
        darken_listview(hwnd);
        break;
    case DM_KIND_HEADER:
        set_theme(hwnd, L"DarkMode_ItemsView");
        break;
    case DM_KIND_TREEVIEW:
        set_theme(hwnd, L"DarkMode_Explorer");
        darken_treeview(hwnd);
        break;
    case DM_KIND_RICHEDIT:
        set_theme(hwnd, L"DarkMode_Explorer"); /* scroll bars */
        darken_richedit(hwnd);
        break;
    case DM_KIND_COMBOLBOX:
    case DM_KIND_LISTBOX:
    case DM_KIND_TOOLTIP:
    case DM_KIND_SCROLLBAR:
        set_theme(hwnd, L"DarkMode_Explorer");
        break;
    case DM_KIND_TAB:
    case DM_KIND_STATUSBAR:
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    default:
        break;
    }
}

BOOL dm_control_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, dm_kind kind, LRESULT *result)
{
    switch (kind) {
    case DM_KIND_BUTTON:
        return button_proc(hwnd, msg, wp, lp, result);
    case DM_KIND_UPDOWN:
        return updown_proc(hwnd, msg, wp, lp, result);
    case DM_KIND_HEADER:
        return header_proc(hwnd, msg, wp, lp, result);
    case DM_KIND_TAB:
        return tab_proc(hwnd, msg, wp, lp, result);
    case DM_KIND_STATUSBAR:
        return statusbar_proc(hwnd, msg, wp, lp, result);
    case DM_KIND_RICHEDIT:
        return richedit_proc(hwnd, msg, wp, lp, result);
    case DM_KIND_LISTVIEW:
    case DM_KIND_TREEVIEW:
        return color_message_proc(hwnd, msg, wp, lp, kind, result);
    default:
        return FALSE;
    }
}
