/*
 * Color and theme hooks for FileZilla's wxWidgets DLLs.
 *
 * wxWidgets 3.2 has no dark mode on Windows, but it takes nearly all of its
 * colors from GetSysColor (window, button face, text, ...), and FileZilla
 * switches its own colors (message log, directory comparison, icons) to dark
 * variants when the window background is dark. So the first and biggest step
 * is to give the wxmsw DLLs dark system colors: they are hooked in their
 * import tables only, the rest of the process (Windows' own dialogs, comctl32)
 * keeps the real ones.
 *
 * What wxWidgets draws with the visual styles (uxtheme) does not use system
 * colors: the page background of notebooks (Site Manager), themed borders,
 * check boxes in custom drawn controls. Those parts are painted dark here.
 *
 * Text that wxWidgets draws is brightened to at least 4.5:1 contrast (the
 * WCAG rule, see colors.c). This is done at the text
 * output calls, not in SetTextColor/SetBkColor: wxWidgets also uses those two
 * to build bitmap masks, which must keep their exact colors.
 */
#include <windows.h>
#include <uxtheme.h>
#include <vssym32.h>
#include "colors.h"
#include "controls.h"
#include "iat.h"
#include "log.h"
#include "wx_hooks.h"

/* ---- system colors ----------------------------------------------------- */

/* Borders get their own range: not as dark as backgrounds, not as bright as
   text, so frames stay visible without shouting. */
static COLORREF map_border(COLORREF c)
{
    COLORREF bg = dm_map_bg(c);
    BYTE lo = (BYTE)(GetRValue(bg) < GetGValue(bg) ? GetRValue(bg) : GetGValue(bg));
    if (GetBValue(bg) < lo)
        lo = GetBValue(bg);
    return lo >= 0x50 ? bg : DM_BORDER;
}

COLORREF dm_dark_sys_color(int index)
{
    COLORREF c = GetSysColor(index);
    switch (index) {
    /* Text */
    case COLOR_WINDOWTEXT:
    case COLOR_MENUTEXT:
    case COLOR_CAPTIONTEXT:
    case COLOR_BTNTEXT:
    case COLOR_INACTIVECAPTIONTEXT:
    case COLOR_INFOTEXT:
    case COLOR_HOTLIGHT:
    case COLOR_HIGHLIGHTTEXT:
        return dm_map_fg(c);
    case COLOR_GRAYTEXT:
        return DM_TEXT_DISABLED;

    /* 3D edges and frames: flat dark look */
    case COLOR_BTNHIGHLIGHT:
        return RGB(0x3C, 0x3C, 0x3C);
    case COLOR_3DLIGHT:
        return RGB(0x2E, 0x2E, 0x2E);
    case COLOR_BTNSHADOW:
    case COLOR_3DDKSHADOW:
    case COLOR_WINDOWFRAME:
    case COLOR_ACTIVEBORDER:
    case COLOR_INACTIVEBORDER:
        return map_border(c);

    /* The desktop color is not ours to change. */
    case COLOR_BACKGROUND:
        return c;

    /* Everything else is a background. */
    default:
        return dm_map_bg(c);
    }
}

static DWORD WINAPI hook_GetSysColor(int index) { return dm_dark_sys_color(index); }
static HBRUSH WINAPI hook_GetSysColorBrush(int index) { return dm_brush(dm_dark_sys_color(index)); }

/* ---- text: at least 4.5:1 ------------------------------------------------ */

/* Map whatever text color is current right before drawing; colors that were
   mapped already (e.g. from the dark system colors) stay the same. */
static void map_current_text_color(HDC hdc)
{
    COLORREF c = GetTextColor(hdc);
    COLORREF m = dm_map_fg(c);
    if (m != c)
        SetTextColor(hdc, m);
}

static int WINAPI hook_DrawTextW(HDC hdc, LPCWSTR s, int n, LPRECT rc, UINT fmt)
{
    if (!(fmt & DT_CALCRECT))
        map_current_text_color(hdc);
    return DrawTextW(hdc, s, n, rc, fmt);
}

static BOOL WINAPI hook_ExtTextOutW(HDC hdc, int x, int y, UINT opt, const RECT *rc, LPCWSTR s, UINT n, const INT *dx)
{
    if (s && n)
        map_current_text_color(hdc);
    return ExtTextOutW(hdc, x, y, opt, rc, s, n, dx);
}

/* ---- visual styles --------------------------------------------------------- */

typedef HTHEME(WINAPI *OpenThemeData_t)(HWND, LPCWSTR);
typedef HRESULT(WINAPI *DrawThemeBackground_t)(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
typedef HRESULT(WINAPI *GetThemeColor_t)(HTHEME, int, int, int, COLORREF *);
static OpenThemeData_t real_OpenThemeData;
static DrawThemeBackground_t real_DrawThemeBackground;
static GetThemeColor_t real_GetThemeColor;

typedef enum { TC_OTHER, TC_TAB, TC_EDIT, TC_BUTTON, TC_HEADER, TC_COMBOBOX } theme_class;

/* Which class each theme handle was opened for. wxWidgets opens and closes
   themes all the time, and a closed handle may come back for another class,
   so every OpenThemeData overwrites the entry of its handle. */
#define THEME_SLOTS 64
static struct {
    HTHEME theme;
    theme_class cls;
} g_themes[THEME_SLOTS];
static int g_theme_next;
static SRWLOCK g_theme_lock = SRWLOCK_INIT;

/* "Explorer::TreeView;TreeView" -> the first class without its prefix. */
static theme_class parse_theme_class(LPCWSTR classes)
{
    static const struct {
        const wchar_t *name;
        theme_class cls;
    } known[] = {
        { L"TAB", TC_TAB }, { L"EDIT", TC_EDIT }, { L"BUTTON", TC_BUTTON },
        { L"HEADER", TC_HEADER }, { L"COMBOBOX", TC_COMBOBOX },
    };
    wchar_t first[64];
    int n = 0;
    if (!classes)
        return TC_OTHER;
    for (const wchar_t *p = classes; *p && *p != L';' && n < 63; p++) {
        if (p[0] == L':' && p[1] == L':') {
            n = 0; /* drop the "Explorer::" prefix */
            p++;
            continue;
        }
        first[n++] = *p;
    }
    first[n] = 0;
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (lstrcmpiW(first, known[i].name) == 0)
            return known[i].cls;
    return TC_OTHER;
}

static void remember_theme(HTHEME theme, theme_class cls)
{
    AcquireSRWLockExclusive(&g_theme_lock);
    int slot = -1;
    for (int i = 0; i < THEME_SLOTS && slot < 0; i++)
        if (g_themes[i].theme == theme)
            slot = i;
    if (slot < 0) {
        slot = g_theme_next;
        g_theme_next = (g_theme_next + 1) % THEME_SLOTS;
    }
    g_themes[slot].theme = theme;
    g_themes[slot].cls = cls;
    ReleaseSRWLockExclusive(&g_theme_lock);
}

static theme_class theme_class_of(HTHEME theme)
{
    theme_class cls = TC_OTHER;
    AcquireSRWLockShared(&g_theme_lock);
    for (int i = 0; i < THEME_SLOTS; i++) {
        if (g_themes[i].theme == theme) {
            cls = g_themes[i].cls;
            break;
        }
    }
    ReleaseSRWLockShared(&g_theme_lock);
    return cls;
}

static HTHEME WINAPI hook_OpenThemeData(HWND hwnd, LPCWSTR classes)
{
    HTHEME theme = real_OpenThemeData(hwnd, classes);
    if (theme)
        remember_theme(theme, parse_theme_class(classes));
    return theme;
}

static void frame_rect(HDC hdc, const RECT *rc, COLORREF color)
{
    FrameRect(hdc, rc, dm_brush(color));
}

/* Paints a themed part dark. Returns FALSE to let the theme draw it. */
static BOOL draw_dark_part(theme_class cls, HDC hdc, int part, int state, const RECT *rc)
{
    switch (cls) {
    case TC_TAB:
        /* Page background of wxNotebook (it makes a pattern brush of it for
           the pages and their controls). Same color as dialogs. */
        if (part == TABP_PANE || part == TABP_BODY) {
            FillRect(hdc, rc, dm_brush(dm_dark_sys_color(COLOR_BTNFACE)));
            return TRUE;
        }
        return FALSE;

    case TC_EDIT:
        /* Themed borders that wxWidgets draws around its windows. */
        if (part >= EP_BACKGROUNDWITHBORDER && part <= EP_EDITBORDER_HVSCROLL) {
            BOOL hot = state == ETS_HOT || state == ETS_FOCUSED;
            FillRect(hdc, rc, dm_brush(dm_dark_sys_color(COLOR_WINDOW)));
            frame_rect(hdc, rc, hot ? RGB(0x80, 0x80, 0x80) : DM_BORDER);
            return TRUE;
        }
        return FALSE;

    case TC_BUTTON:
        if (part == BP_CHECKBOX || part == BP_RADIOBUTTON) {
            /* States: 4 per check state (normal, hot, pressed, disabled). */
            int check = (state - 1) / 4, sub = (state - 1) % 4 + 1;
            if (part == BP_RADIOBUTTON || check > BST_INDETERMINATE)
                check = check == 1 ? BST_CHECKED : BST_UNCHECKED;
            dm_draw_check_glyph(hdc, rc, part == BP_RADIOBUTTON, check, sub);
            return TRUE;
        }
        if (part == BP_GROUPBOX) {
            frame_rect(hdc, rc, DM_BORDER);
            return TRUE;
        }
        return FALSE;

    case TC_HEADER:
        if (part == HP_HEADERITEM) {
            RECT sep = { rc->right - 1, rc->top + 3, rc->right, rc->bottom - 4 };
            RECT bottom = { rc->left, rc->bottom - 1, rc->right, rc->bottom };
            FillRect(hdc, rc, dm_brush(state == HIS_PRESSED ? DM_CONTROL_PRESSED
                                       : state == HIS_HOT ? DM_CONTROL : DM_HEADER));
            FillRect(hdc, &sep, dm_brush(DM_BORDER));
            FillRect(hdc, &bottom, dm_brush(DM_BORDER));
            return TRUE;
        }
        return FALSE;

    case TC_COMBOBOX:
        if (part == CP_DROPDOWNBUTTON) {
            FillRect(hdc, rc, dm_brush(state == CBXS_PRESSED ? DM_CONTROL_PRESSED : DM_CONTROL));
            dm_paint_arrow(hdc, rc, 1, state == CBXS_DISABLED ? DM_TEXT_DISABLED : DM_TEXT);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

static HRESULT WINAPI hook_DrawThemeBackground(HTHEME theme, HDC hdc, int part, int state, LPCRECT rc, LPCRECT clip)
{
    theme_class cls = rc ? theme_class_of(theme) : TC_OTHER;
    if (cls != TC_OTHER) {
        int saved = clip ? SaveDC(hdc) : 0;
        if (clip)
            IntersectClipRect(hdc, clip->left, clip->top, clip->right, clip->bottom);
        BOOL done = draw_dark_part(cls, hdc, part, state, rc);
        if (saved)
            RestoreDC(hdc, saved);
        if (done)
            return S_OK;
    }
    return real_DrawThemeBackground(theme, hdc, part, state, rc, clip);
}

static HRESULT WINAPI hook_GetThemeColor(HTHEME theme, int part, int state, int prop, COLORREF *out)
{
    HRESULT hr = real_GetThemeColor(theme, part, state, prop, out);
    if (FAILED(hr) || !out)
        return hr;
    switch (prop) {
    case TMT_TEXTCOLOR:
        *out = dm_map_fg(*out);
        break;
    case TMT_FILLCOLOR:
    case TMT_FILLCOLORHINT:
        *out = dm_map_bg(*out);
        break;
    case TMT_BORDERCOLOR:
    case TMT_BORDERCOLORHINT:
        *out = map_border(*out);
        break;
    }
    return hr;
}

/* ---- message dialogs ------------------------------------------------------ */

/* wxMessageDialog (every Yes/No question and error box of FileZilla, e.g.
   "Do you really want to delete 1 file from the server?") shows a Windows task
   dialog when it finds TaskDialogIndirect in comctl32
   (wxMSWMessageDialog::GetTaskDialogIndirectFunc, through wxDynamicLibrary,
   so through GetProcAddress in wxbase). Windows draws that dialog in DirectUI:
   a white page and a gray button strip we cannot reach, between a title bar and
   buttons that do follow the dark app mode. Without the function wx falls back
   to MessageBox (same buttons and labels), which darkmode.c paints dark.
   wxProgressDialog then uses its own dialog as well. */
static LONG g_task_dialog_logged;

static FARPROC WINAPI hook_GetProcAddress(HMODULE module, LPCSTR name)
{
    if ((ULONG_PTR)name > 0xFFFF && lstrcmpA(name, "TaskDialogIndirect") == 0) {
        if (InterlockedExchange(&g_task_dialog_logged, 1) == 0)
            dm_log(L"message dialogs: TaskDialogIndirect hidden from wxWidgets (classic MessageBox)");
        SetLastError(ERROR_PROC_NOT_FOUND);
        return NULL;
    }
    return GetProcAddress(module, name);
}

/* ---- install ------------------------------------------------------------- */

#define MAX_APP_MODULES 8
static HMODULE g_app_modules[MAX_APP_MODULES];
static int g_app_module_count;

/* wxbase*: no colors or themes in there, so not an "app module" (class
   brushes), but its dynamic library loader is hooked. */
static HMODULE g_base_modules[MAX_APP_MODULES];
static int g_base_module_count;

BOOL dm_is_app_module(HMODULE module)
{
    for (int i = 0; i < g_app_module_count; i++)
        if (g_app_modules[i] == module)
            return TRUE;
    return FALSE;
}

int dm_app_modules(HMODULE *out, int max)
{
    int n = g_app_module_count < max ? g_app_module_count : max;
    for (int i = 0; i < n; i++)
        out[i] = g_app_modules[i];
    return n;
}

static void add_app_module(HMODULE module)
{
    if (module && !dm_is_app_module(module) && g_app_module_count < MAX_APP_MODULES)
        g_app_modules[g_app_module_count++] = module;
}

/* The exe imports wxmsw32u_core_gcc_custom.dll, wxmsw32u_aui_..., etc. The
   version in the name changes with wxWidgets, so match on the prefix. */
static BOOL collect_wx_module(const char *dll, void *ctx)
{
    (void)ctx;
    if (CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, dll, 5, "wxmsw", 5) == CSTR_EQUAL) {
        add_app_module(GetModuleHandleA(dll));
    } else if (CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, dll, 6, "wxbase", 6) == CSTR_EQUAL) {
        HMODULE module = GetModuleHandleA(dll);
        if (module && g_base_module_count < MAX_APP_MODULES) {
            for (int i = 0; i < g_base_module_count; i++)
                if (g_base_modules[i] == module)
                    return TRUE;
            g_base_modules[g_base_module_count++] = module;
        }
    }
    return TRUE;
}

static void module_name(HMODULE module, wchar_t *name, int len)
{
    wchar_t path[MAX_PATH];
    lstrcpynW(name, L"?", len);
    if (GetModuleFileNameW(module, path, MAX_PATH)) {
        const wchar_t *p = wcsrchr(path, L'\\');
        lstrcpynW(name, p ? p + 1 : path, len);
    }
}

static void set_original(void **real, void *original)
{
    if (!*real && original)
        *real = original;
}

void dm_wx_hooks_install(void)
{
    HMODULE exe = GetModuleHandleW(NULL);
    add_app_module(exe);
    dm_for_each_import(exe, collect_wx_module, NULL);

    struct hook {
        const char *dll, *func;
        void *hook;
        void **real;
    };
    static const struct hook hooks[] = {
        { "USER32.dll", "GetSysColor", (void *)hook_GetSysColor, NULL },
        { "USER32.dll", "GetSysColorBrush", (void *)hook_GetSysColorBrush, NULL },
        { "USER32.dll", "DrawTextW", (void *)hook_DrawTextW, NULL },
        { "GDI32.dll", "ExtTextOutW", (void *)hook_ExtTextOutW, NULL },
        { "UxTheme.dll", "OpenThemeData", (void *)hook_OpenThemeData, (void **)&real_OpenThemeData },
        { "UxTheme.dll", "DrawThemeBackground", (void *)hook_DrawThemeBackground, (void **)&real_DrawThemeBackground },
        { "UxTheme.dll", "GetThemeColor", (void *)hook_GetThemeColor, (void **)&real_GetThemeColor },
    };
    const int total = (int)(sizeof(hooks) / sizeof(hooks[0]));

    for (int m = 0; m < g_app_module_count; m++) {
        int ok = 0;
        wchar_t list[160] = L"";
        for (int i = 0; i < total; i++) {
            void *original = NULL;
            if (!dm_iat_hook(g_app_modules[m], hooks[i].dll, hooks[i].func, hooks[i].hook, &original))
                continue;
            if (hooks[i].real)
                set_original(hooks[i].real, original);
            ok++;
            if (lstrlenW(list) + 24 < 160) {
                wchar_t name[32];
                MultiByteToWideChar(CP_ACP, 0, hooks[i].func, -1, name, 32);
                if (*list)
                    lstrcatW(list, L" ");
                lstrcatW(list, name);
            }
        }
        wchar_t name[64];
        module_name(g_app_modules[m], name, 64);
        dm_log(L"wx hooks: %d in %ls%ls%ls", ok, name, ok ? L": " : L"", list);
    }

    for (int m = 0; m < g_base_module_count; m++) {
        BOOL ok = dm_iat_hook(g_base_modules[m], "KERNEL32.dll", "GetProcAddress",
                              (void *)hook_GetProcAddress, NULL);
        wchar_t name[64];
        module_name(g_base_modules[m], name, 64);
        dm_log(L"wx hooks: %d in %ls%ls", ok ? 1 : 0, name, ok ? L": GetProcAddress" : L"");
    }
}
