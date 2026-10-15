#ifndef DM_DARK_API_H
#define DM_DARK_API_H

#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>

/*
 * Functions loaded at runtime. Nothing here is imported statically, so the
 * proxy DLL still loads in processes (or Windows versions) that lack them.
 * Every pointer may be NULL.
 */
typedef struct {
    DWORD build; /* Windows build number, e.g. 19045 */

    /* comctl32 (v6, already loaded by FileZilla) */
    BOOL(WINAPI *SetWindowSubclass)(HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR);
    BOOL(WINAPI *RemoveWindowSubclass)(HWND, SUBCLASSPROC, UINT_PTR);
    LRESULT(WINAPI *DefSubclassProc)(HWND, UINT, WPARAM, LPARAM);

    /* uxtheme, documented */
    HRESULT(WINAPI *SetWindowTheme)(HWND, LPCWSTR, LPCWSTR);
    HTHEME(WINAPI *OpenThemeData)(HWND, LPCWSTR);
    HRESULT(WINAPI *CloseThemeData)(HTHEME);
    HRESULT(WINAPI *DrawThemeBackground)(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
    HRESULT(WINAPI *GetThemePartSize)(HTHEME, HDC, int, int, LPCRECT, THEMESIZE, SIZE *);
    HRESULT(WINAPI *DrawThemeParentBackground)(HWND, HDC, const RECT *);

    /* uxtheme, undocumented, by ordinal (Windows 10 1809+ only) */
    void(WINAPI *RefreshImmersiveColorPolicyState)(void); /* #104 */
    BOOLEAN(WINAPI *AllowDarkModeForWindow)(HWND, BOOL);  /* #133 */
    BOOLEAN(WINAPI *AllowDarkModeForApp)(BOOL);           /* #135 before 1903 */
    int(WINAPI *SetPreferredAppMode)(int);                /* #135 since 1903 */
    void(WINAPI *FlushMenuThemes)(void);                  /* #136 */

    /* dwmapi */
    HRESULT(WINAPI *DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
} dm_api;

extern dm_api g_api;

/* Values for SetPreferredAppMode. */
enum { DM_APPMODE_DEFAULT = 0, DM_APPMODE_ALLOW_DARK = 1, DM_APPMODE_FORCE_DARK = 2 };

/* Reads the Windows build number; safe to call from DllMain. */
DWORD dm_windows_build(void);

/* Loads all pointers above. Must NOT be called from DllMain (uses LoadLibrary). */
void dm_api_load(void);

#endif
