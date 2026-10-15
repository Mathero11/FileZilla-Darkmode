#include "dark_api.h"
#include "log.h"

dm_api g_api;

/* Undocumented uxtheme exports only exist from Windows 10 1809 on; on older
   builds the same ordinals are different functions, so never touch them. */
#define BUILD_1809 17763
#define BUILD_1903 18362

typedef LONG(WINAPI *RtlGetVersion_t)(OSVERSIONINFOW *);

DWORD dm_windows_build(void)
{
    /* GetVersionEx lies without a manifest; RtlGetVersion does not. */
    OSVERSIONINFOW vi = { 0 };
    vi.dwOSVersionInfoSize = sizeof(vi);
    RtlGetVersion_t rtl_get_version =
        (RtlGetVersion_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rtl_get_version && rtl_get_version(&vi) == 0)
        return vi.dwBuildNumber;
    return 0;
}

static void *by_ordinal(HMODULE mod, WORD ordinal)
{
    return mod ? (void *)GetProcAddress(mod, MAKEINTRESOURCEA(ordinal)) : NULL;
}

/* Via void *: a direct cast between function pointer types is warned about. */
#define LOAD(mod, field, name) \
    (g_api.field = (__typeof__(g_api.field))((mod) ? (void *)GetProcAddress((mod), (name)) : NULL))
#define LOAD_ORD(mod, field, ord) (g_api.field = (__typeof__(g_api.field))by_ordinal((mod), (ord)))

void dm_api_load(void)
{
    g_api.build = dm_windows_build();

    HMODULE comctl = GetModuleHandleW(L"comctl32.dll");
    if (!comctl)
        comctl = LoadLibraryW(L"comctl32.dll");
    LOAD(comctl, SetWindowSubclass, "SetWindowSubclass");
    LOAD(comctl, RemoveWindowSubclass, "RemoveWindowSubclass");
    LOAD(comctl, DefSubclassProc, "DefSubclassProc");

    HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
    LOAD(uxtheme, SetWindowTheme, "SetWindowTheme");
    LOAD(uxtheme, OpenThemeData, "OpenThemeData");
    LOAD(uxtheme, CloseThemeData, "CloseThemeData");
    LOAD(uxtheme, DrawThemeBackground, "DrawThemeBackground");
    LOAD(uxtheme, GetThemePartSize, "GetThemePartSize");
    LOAD(uxtheme, DrawThemeParentBackground, "DrawThemeParentBackground");
    if (g_api.build >= BUILD_1809) {
        LOAD_ORD(uxtheme, RefreshImmersiveColorPolicyState, 104);
        LOAD_ORD(uxtheme, AllowDarkModeForWindow, 133);
        if (g_api.build >= BUILD_1903)
            LOAD_ORD(uxtheme, SetPreferredAppMode, 135);
        else
            LOAD_ORD(uxtheme, AllowDarkModeForApp, 135);
        LOAD_ORD(uxtheme, FlushMenuThemes, 136);
    }

    HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");
    LOAD(dwmapi, DwmSetWindowAttribute, "DwmSetWindowAttribute");

    dm_log(L"api: build %lu, subclass %ls, SetWindowTheme %ls, dark mode ordinals %ls, dwm %ls",
           g_api.build,
           g_api.SetWindowSubclass && g_api.DefSubclassProc ? L"ok" : L"MISSING",
           g_api.SetWindowTheme ? L"ok" : L"MISSING",
           g_api.AllowDarkModeForWindow ? L"ok" : L"missing",
           g_api.DwmSetWindowAttribute ? L"ok" : L"missing");
}
