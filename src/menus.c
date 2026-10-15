#include <windows.h>
#include "colors.h"
#include "dark_api.h"
#include "menus.h"
#include "wx_hooks.h"

/* Undocumented messages Windows 10/11 send to draw a themed menu bar. Wine
   does not send them; there the MIM_BACKGROUND brush does the work. */
#define WM_UAHDRAWMENU 0x0091
#define WM_UAHDRAWMENUITEM 0x0092

typedef struct {
    HMENU hmenu;
    HDC hdc;
    DWORD flags;
} uah_menu;

typedef union {
    struct { DWORD cx, cy; } bar[2];
    struct { DWORD cx, cy; } popup[4];
} uah_menu_item_metrics;

typedef struct {
    DWORD rgcx[4];
    DWORD update_max_widths : 2;
} uah_menu_popup_metrics;

typedef struct {
    int position;
    uah_menu_item_metrics metrics;
    uah_menu_popup_metrics popup_metrics;
} uah_menu_item;

typedef struct {
    DRAWITEMSTRUCT dis;
    uah_menu um;
    uah_menu_item umi;
} uah_draw_menu_item;

#define MENU_HOT     RGB(0x3E, 0x3E, 0x3E)
#define MENU_PRESSED RGB(0x4A, 0x4A, 0x4A)

static HBRUSH menu_brush(void)
{
    return dm_brush(dm_dark_sys_color(COLOR_MENU));
}

/* Background brush for the menu bar only. The popup menus are drawn by
   Windows (wxWidgets does not owner-draw them), dark through the app mode;
   a brush of our own there would only fight with that (and under Wine give
   black text on a dark background). */
static void set_menu_bar_background(HMENU menu)
{
    if (!menu)
        return;
    HBRUSH br = menu_brush();
    MENUINFO mi = { 0 };
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND;
    if (GetMenuInfo(menu, &mi) && mi.hbrBack == br)
        return;
    mi.hbrBack = br;
    SetMenuInfo(menu, &mi);
}

/* Menu bar rectangle in window coordinates. */
static BOOL menu_bar_rect(HWND hwnd, RECT *rc)
{
    MENUBARINFO mbi = { 0 };
    mbi.cbSize = sizeof(mbi);
    if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi))
        return FALSE;
    RECT win;
    GetWindowRect(hwnd, &win);
    *rc = mbi.rcBar;
    OffsetRect(rc, -win.left, -win.top);
    return TRUE;
}

/* Fallback for systems that ignore MIM_BACKGROUND on the menu bar (Wine) and
   send no WM_UAHDRAWMENU: paint the empty parts of the bar, i.e. everything
   except the items, after the non-client area was drawn. */
static void paint_menu_bar_gaps(HWND hwnd)
{
    HMENU menu = GetMenu(hwnd);
    RECT bar, win;
    if (!menu || !menu_bar_rect(hwnd, &bar))
        return;
    GetWindowRect(hwnd, &win);

    HDC hdc = GetWindowDC(hwnd);
    if (!hdc)
        return;
    int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; i++) {
        RECT item;
        if (GetMenuItemRect(hwnd, menu, i, &item)) {
            OffsetRect(&item, -win.left, -win.top);
            ExcludeClipRect(hdc, item.left, item.top, item.right, item.bottom);
        }
    }
    FillRect(hdc, &bar, menu_brush());
    ReleaseDC(hwnd, hdc);
}

/* Themed Windows draws a light 1px line between the menu bar and the client
   area, outside of any message we can answer. Paint over it afterwards. */
static void paint_menu_bar_line(HWND hwnd)
{
    RECT client, win;
    GetClientRect(hwnd, &client);
    MapWindowPoints(hwnd, NULL, (POINT *)&client, 2);
    GetWindowRect(hwnd, &win);
    OffsetRect(&client, -win.left, -win.top);

    RECT line = client;
    line.bottom = line.top;
    line.top--;

    HDC hdc = GetWindowDC(hwnd);
    if (hdc) {
        FillRect(hdc, &line, menu_brush());
        ReleaseDC(hwnd, hdc);
    }
}

/* One item of the menu bar ("File", "Edit", ...). Popup menus are drawn by
   Windows itself, dark through SetPreferredAppMode. */
static void draw_menu_bar_item(const uah_draw_menu_item *di)
{
    wchar_t text[256] = L"";
    MENUITEMINFOW mii = { 0 };
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = text;
    mii.cch = 255;
    GetMenuItemInfoW(di->um.hmenu, di->umi.position, TRUE, &mii);

    UINT state = di->dis.itemState;
    COLORREF bg = (state & ODS_SELECTED) ? MENU_PRESSED : (state & ODS_HOTLIGHT) ? MENU_HOT : dm_dark_sys_color(COLOR_MENU);
    BOOL disabled = (state & (ODS_INACTIVE | ODS_GRAYED | ODS_DISABLED)) != 0;
    RECT rc = di->dis.rcItem;

    FillRect(di->um.hdc, &rc, dm_brush(bg));
    UINT fmt = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
    if (state & ODS_NOACCEL)
        fmt |= DT_HIDEPREFIX;
    int old_mode = SetBkMode(di->um.hdc, TRANSPARENT);
    COLORREF old_color = SetTextColor(di->um.hdc, disabled ? DM_TEXT_DISABLED : DM_TEXT);
    DrawTextW(di->um.hdc, text, -1, &rc, fmt);
    SetTextColor(di->um.hdc, old_color);
    SetBkMode(di->um.hdc, old_mode);
}

BOOL dm_menu_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    switch (msg) {
    case WM_UAHDRAWMENU: {
        uah_menu *um = (uah_menu *)lp;
        RECT rc;
        if (!menu_bar_rect(hwnd, &rc))
            return FALSE;
        FillRect(um->hdc, &rc, menu_brush());
        *result = TRUE;
        return TRUE;
    }

    case WM_UAHDRAWMENUITEM:
        draw_menu_bar_item((const uah_draw_menu_item *)lp);
        *result = TRUE;
        return TRUE;

    case WM_NCPAINT:
    case WM_NCACTIVATE:
        if (!GetMenu(hwnd))
            return FALSE;
        /* The app may replace its menu at any time (SetMenu). */
        set_menu_bar_background(GetMenu(hwnd));
        *result = g_api.DefSubclassProc(hwnd, msg, wp, lp);
        paint_menu_bar_gaps(hwnd);
        paint_menu_bar_line(hwnd);
        return TRUE;
    }
    return FALSE;
}
