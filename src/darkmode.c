/*
 * Dark mode core.
 *
 * A per-thread WH_CBT hook sees every window the app creates. Each window is
 * subclassed (SetWindowSubclass) and gets the native Windows dark themes once
 * it has been created:
 *   - dark title bar (DWM) for top-level windows
 *   - dark scroll bars (DarkMode_Explorer) for windows with scroll bars
 *   - dark popup menus for the whole app (SetPreferredAppMode)
 *
 * On top of that, every color the app hands to Windows through WM_CTLCOLOR*,
 * list/tree view custom draw and window class brushes is mapped to dark, and
 * controls get their own dark handling (controls.c). The colors wxWidgets
 * picks itself come from the hooks in wx_hooks.c.
 */
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include "colors.h"
#include "config.h"
#include "controls.h"
#include "dark_api.h"
#include "darkmode.h"
#include "iat.h"
#include "log.h"
#include "menus.h"
#include "wx_hooks.h"

#define SUBCLASS_ID 0x444D465A /* "DMFZ" */

/* DWM attribute ids: 20 since Windows 10 20H1, 19 on 1809..1909. */
#define DWMWA_DARK_TITLE_BAR 20
#define DWMWA_DARK_TITLE_BAR_OLD 19

static DWORD g_hook_tls = TLS_OUT_OF_INDEXES;
static INIT_ONCE g_api_once = INIT_ONCE_STATIC_INIT;
static BOOL g_api_ok;

/* ---- process filter ---------------------------------------------------- */

BOOL dm_is_target_process(void)
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(NULL, path, MAX_PATH))
        return FALSE;
    const wchar_t *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return lstrcmpiW(name, L"filezilla.exe") == 0;
}

/* ---- one-time app-wide setup ------------------------------------------- */

static BOOL CALLBACK init_app_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;

    dm_api_load();
    /* Already done in DllMain; repeated in case the loader rebound the
       imports in between. Hooked entries are skipped. */
    dm_wx_hooks_install();
    g_api_ok = g_api.SetWindowSubclass && g_api.DefSubclassProc;
    if (!g_api_ok) {
        dm_log(L"ERROR: SetWindowSubclass not available, dark mode disabled");
        return TRUE;
    }

    /* Dark popup and context menus. ForceDark, because the ini can ask for
       dark mode while Windows itself is set to light. */
    if (g_api.SetPreferredAppMode) {
        g_api.SetPreferredAppMode(DM_APPMODE_FORCE_DARK);
        dm_log(L"app mode: SetPreferredAppMode(ForceDark)");
    } else if (g_api.AllowDarkModeForApp) {
        g_api.AllowDarkModeForApp(TRUE);
        dm_log(L"app mode: AllowDarkModeForApp(TRUE)");
    } else {
        dm_log(L"app mode: not supported on this Windows version");
    }
    if (g_api.RefreshImmersiveColorPolicyState)
        g_api.RefreshImmersiveColorPolicyState();
    if (g_api.FlushMenuThemes)
        g_api.FlushMenuThemes();
    return TRUE;
}

/* ---- per window -------------------------------------------------------- */

static void set_dark_title_bar(HWND hwnd)
{
    if (!g_api.DwmSetWindowAttribute)
        return;
    BOOL on = TRUE;
    if (FAILED(g_api.DwmSetWindowAttribute(hwnd, DWMWA_DARK_TITLE_BAR, &on, sizeof(on))))
        g_api.DwmSetWindowAttribute(hwnd, DWMWA_DARK_TITLE_BAR_OLD, &on, sizeof(on));
}

/* Applies the native dark themes. Safe to call more than once. */
static void apply_native_dark(HWND hwnd)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);

    if (g_api.AllowDarkModeForWindow)
        g_api.AllowDarkModeForWindow(hwnd, TRUE);

    if (!(style & WS_CHILD))
        set_dark_title_bar(hwnd);

    if ((style & (WS_VSCROLL | WS_HSCROLL)) && g_api.SetWindowTheme)
        g_api.SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL);
}

/* Let the app (or Windows) pick its colors, then map them to dark. */
static LRESULT map_ctlcolor(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HDC hdc = (HDC)wp;
    HBRUSH br = (HBRUSH)g_api.DefSubclassProc(hwnd, msg, wp, lp);
    SetTextColor(hdc, dm_map_fg(GetTextColor(hdc)));
    SetBkColor(hdc, dm_map_bg(GetBkColor(hdc)));
    return (LRESULT)dm_map_brush(br);
}

/* NM_CUSTOMDRAW from list views, tree views and headers. */
static LRESULT map_custom_draw(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)lp;
    dm_kind from = dm_classify(cd->hdr.hwndFrom);
    LRESULT result = g_api.DefSubclassProc(hwnd, msg, wp, lp);

    if (result & CDRF_SKIPDEFAULT)
        return result; /* the app paints this itself */

    switch (from) {
    case DM_KIND_HEADER:
        /* Header text color only applies per item. */
        if (cd->dwDrawStage == CDDS_PREPAINT)
            return result | CDRF_NOTIFYITEMDRAW;
        if (cd->dwDrawStage == CDDS_ITEMPREPAINT)
            SetTextColor(cd->hdc, DM_TEXT);
        break;
    case DM_KIND_LISTVIEW:
        if (cd->dwDrawStage & CDDS_ITEM) {
            NMLVCUSTOMDRAW *lv = (NMLVCUSTOMDRAW *)lp;
            lv->clrText = dm_map_fg(lv->clrText);
            lv->clrTextBk = dm_map_bg(lv->clrTextBk);
        }
        break;
    case DM_KIND_TREEVIEW:
        if (cd->dwDrawStage & CDDS_ITEM) {
            NMTVCUSTOMDRAW *tv = (NMTVCUSTOMDRAW *)lp;
            tv->clrText = dm_map_fg(tv->clrText);
            tv->clrTextBk = dm_map_bg(tv->clrTextBk);
        }
        break;
    default:
        break;
    }
    return result;
}

/* MessageBox (user32) paints its light button strip in WM_PAINT, inside
   user32 where our hooks do not reach. */
static BOOL is_message_box(HWND hwnd)
{
    return (HMODULE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE) == GetModuleHandleW(L"user32.dll") &&
           GetDlgItem(hwnd, 0xFFFF) != NULL; /* the message text static */
}

static BOOL CALLBACK find_top_button(HWND child, LPARAM lp)
{
    wchar_t cls[16];
    if (IsWindowVisible(child) && GetClassNameW(child, cls, 16) && lstrcmpiW(cls, L"Button") == 0) {
        RECT rc;
        GetWindowRect(child, &rc);
        MapWindowPoints(NULL, GetParent(child), (POINT *)&rc, 2);
        LONG *top = (LONG *)lp;
        if (rc.top < *top)
            *top = rc.top;
    }
    return TRUE;
}

static void paint_message_box(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, dm_brush(dm_map_bg(GetSysColor(COLOR_WINDOW))));

    LONG top = rc.bottom;
    EnumChildWindows(hwnd, find_top_button, (LPARAM)&top);
    if (top < rc.bottom) {
        RECT strip = rc;
        strip.top = top - MulDiv(14, GetDeviceCaps(hdc, LOGPIXELSY), 96);
        FillRect(hdc, &strip, dm_brush(dm_dark_sys_color(COLOR_3DFACE)));
    }
    EndPaint(hwnd, &ps);
}

/* Sunken (WS_EX_CLIENTEDGE), static and plain borders of child windows are
   drawn by Windows with the real 3D system colors: a white line around every
   list, tree and input field. Painted over after Windows has drawn them:
   a gray outer line, the rest in the control's background. */
static void paint_child_border(HWND hwnd)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    int width = 0;
    if (ex & WS_EX_CLIENTEDGE)
        width += GetSystemMetrics(SM_CXEDGE);
    if (ex & WS_EX_STATICEDGE)
        width += GetSystemMetrics(SM_CXBORDER);
    if (style & WS_BORDER)
        width += GetSystemMetrics(SM_CXBORDER);
    if (width == 0)
        return;

    HDC hdc = GetWindowDC(hwnd);
    if (!hdc)
        return;
    RECT rc;
    GetWindowRect(hwnd, &rc);
    OffsetRect(&rc, -rc.left, -rc.top);
    RECT inner = rc;
    InflateRect(&inner, -width, -width);
    ExcludeClipRect(hdc, inner.left, inner.top, inner.right, inner.bottom);
    FillRect(hdc, &rc, dm_brush(dm_dark_sys_color(COLOR_WINDOW)));
    FrameRect(hdc, &rc, dm_brush(DM_BORDER));
    ReleaseDC(hwnd, hdc);
}

static LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR id, DWORD_PTR ref)
{
    (void)id;
    dm_kind kind = (dm_kind)ref;
    LRESULT result;

    if (dm_control_proc(hwnd, msg, wp, lp, kind, &result))
        return result;
    if (!(GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD) && dm_menu_proc(hwnd, msg, wp, lp, &result))
        return result;

    switch (msg) {
    case WM_CREATE:
    case WM_INITDIALOG:
        result = g_api.DefSubclassProc(hwnd, msg, wp, lp);
        apply_native_dark(hwnd);
        dm_control_created(hwnd, kind);
        return result;

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORMSGBOX:
    case WM_CTLCOLORSCROLLBAR:
        return map_ctlcolor(hwnd, msg, wp, lp);

    case WM_NOTIFY:
        if (((NMHDR *)lp)->code == NM_CUSTOMDRAW)
            return map_custom_draw(hwnd, msg, wp, lp);
        break;

    case WM_NCPAINT:
        if (!(GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD))
            break;
        result = g_api.DefSubclassProc(hwnd, msg, wp, lp);
        paint_child_border(hwnd);
        return result;

    case WM_PAINT:
        if (kind == DM_KIND_DIALOG && is_message_box(hwnd)) {
            paint_message_box(hwnd);
            return 0;
        }
        break;

    case WM_NCDESTROY:
        g_api.RemoveWindowSubclass(hwnd, subclass_proc, SUBCLASS_ID);
        break;
    }
    return g_api.DefSubclassProc(hwnd, msg, wp, lp);
}

/* Returns TRUE the first time a window class is seen (and logs it). */
static BOOL first_of_class(HWND hwnd)
{
    static ATOM seen[128];
    static int seen_count;

    ATOM atom = (ATOM)GetClassLongPtrW(hwnd, GCW_ATOM);
    for (int i = 0; i < seen_count; i++)
        if (seen[i] == atom)
            return FALSE;
    if (seen_count < (int)(sizeof(seen) / sizeof(seen[0])))
        seen[seen_count++] = atom;

    wchar_t name[128];
    if (GetClassNameW(hwnd, name, 128))
        dm_log(L"new window class: %ls", name);
    return TRUE;
}

/* Window classes registered by the app (wxWidgets registers them for the
   exe or for its own DLL) erase their background with the class brush; swap
   it for a dark one. System classes are left alone. */
static void darken_class_background(HWND hwnd)
{
    HMODULE owner = (HMODULE)GetClassLongPtrW(hwnd, GCLP_HMODULE);
    if (!dm_is_app_module(owner))
        return;

    HBRUSH br = (HBRUSH)GetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND);
    COLORREF color;
    if (!br)
        return;
    if ((UINT_PTR)br <= COLOR_MENUBAR + 1) {
        color = GetSysColor((int)(UINT_PTR)br - 1); /* COLOR_xxx + 1 */
    } else {
        LOGBRUSH lb;
        if (GetObjectW(br, sizeof(lb), &lb) != sizeof(lb) || lb.lbStyle != BS_SOLID)
            return;
        color = lb.lbColor;
    }
    SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)dm_brush(dm_map_bg(color)));
}

/* ---- windows left alone ------------------------------------------------ */

/* The Windows file and folder dialogs (Explorer/DirectUI inside) can only be
   darkened partially, which looks broken. They, everything inside them and
   every window they own are left to Windows. */
static ATOM g_skip_prop;

static BOOL created_by_shell(HWND hwnd)
{
    static const wchar_t *const modules[] = {
        L"comdlg32.dll", L"shell32.dll", L"ExplorerFrame.dll", L"windows.storage.dll", L"DUI70.dll",
    };
    HMODULE inst = (HMODULE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    if (!inst)
        return FALSE;
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
        HMODULE m = GetModuleHandleW(modules[i]);
        if (m && m == inst)
            return TRUE;
    }
    return FALSE;
}

/* At HCBT_CREATEWND the new window may not be linked to its parent yet
   (documented, and so it is on Windows; Wine links it earlier), so the search
   starts at the parent or owner passed to CreateWindow. */
static BOOL inside_skipped_window(HWND hwnd, HWND parent)
{
    HWND start = parent && parent != HWND_MESSAGE ? parent : hwnd;
    HWND root = GetAncestor(start, GA_ROOT);
    for (int depth = 0; root && depth < 8; depth++) {
        if (GetPropW(root, MAKEINTATOM(g_skip_prop)))
            return TRUE;
        HWND owner = GetWindow(root, GW_OWNER);
        root = owner ? GetAncestor(owner, GA_ROOT) : NULL;
    }
    return FALSE;
}

/* With the app mode on ForceDark, Windows may darken parts of the file dialog
   by itself, and Windows 10 cannot darken all of it (the file list stays
   light). While such a dialog is open, the app mode goes back to default, so
   Windows draws it completely light; when it closes, dark mode (dark menus)
   comes back. Only dialogs are tracked: the shell also creates helper windows
   that live on after the dialog is gone.

   Windows 11 can draw the whole file dialog dark, but it decides part of that
   before the dialog window exists (title bar, dialog background, file name
   field), while the app mode is still ForceDark. Switching to default at
   HCBT_CREATEWND then leaves a mix: those parts dark, the Explorer parts that
   are created afterwards (address bar, navigation pane, file list) light. So
   on Windows 11 the app mode stays dark and Windows themes the dialog in one
   go. The ini (FileDialog=dark|light) overrides this choice. */
#define BUILD_WINDOWS_11 22000
#define MAX_SHELL_DIALOGS 8
static HWND g_shell_dialogs[MAX_SHELL_DIALOGS];
static int g_shell_dialog_count;
static SRWLOCK g_shell_dialog_lock = SRWLOCK_INIT;

static void set_app_dark(BOOL dark, const wchar_t *why)
{
    if (g_api.SetPreferredAppMode)
        g_api.SetPreferredAppMode(dark ? DM_APPMODE_FORCE_DARK : DM_APPMODE_DEFAULT);
    else if (g_api.AllowDarkModeForApp)
        g_api.AllowDarkModeForApp(dark);
    else
        return;
    if (g_api.FlushMenuThemes)
        g_api.FlushMenuThemes();
    dm_log(L"app mode: %ls (%ls)", dark ? L"dark" : L"default", why);
}

/* File dialogs opened through one of the hooked calls below (> 0: the app
   mode is already default). */
static LONG g_file_dialog_depth;

static BOOL file_dialog_goes_light(void)
{
    switch (g_cfg.file_dialog) {
    case DM_FILE_DIALOG_DARK:
        return FALSE;
    case DM_FILE_DIALOG_LIGHT:
        return TRUE;
    default:
        return g_api.build < BUILD_WINDOWS_11;
    }
}

static void shell_dialog_opened(HWND hwnd)
{
    wchar_t cls[16];
    if (!GetClassNameW(hwnd, cls, 16) || lstrcmpW(cls, L"#32770") != 0)
        return;
    if (!file_dialog_goes_light()) {
        dm_log(L"app mode: stays dark (Windows dialog opened, FileDialog=%ls, build %lu)",
               g_cfg.file_dialog == DM_FILE_DIALOG_DARK ? L"dark" : L"auto", g_api.build);
        return;
    }
    if (g_file_dialog_depth > 0) {
        dm_log(L"app mode: already default (switched before the dialog was created)");
        return;
    }
    AcquireSRWLockExclusive(&g_shell_dialog_lock);
    BOOL first = FALSE;
    if (g_shell_dialog_count < MAX_SHELL_DIALOGS) {
        g_shell_dialogs[g_shell_dialog_count++] = hwnd;
        first = g_shell_dialog_count == 1;
    }
    ReleaseSRWLockExclusive(&g_shell_dialog_lock);
    if (first)
        set_app_dark(FALSE, L"Windows dialog opened");
}

static void on_window_destroyed(HWND hwnd)
{
    if (!g_skip_prop || !GetPropW(hwnd, MAKEINTATOM(g_skip_prop)))
        return;
    RemovePropW(hwnd, MAKEINTATOM(g_skip_prop));

    AcquireSRWLockExclusive(&g_shell_dialog_lock);
    BOOL last = FALSE;
    for (int i = 0; i < g_shell_dialog_count; i++) {
        if (g_shell_dialogs[i] == hwnd) {
            g_shell_dialogs[i] = g_shell_dialogs[--g_shell_dialog_count];
            last = g_shell_dialog_count == 0;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_shell_dialog_lock);
    if (last)
        set_app_dark(TRUE, L"Windows dialog closed");
}

/* ---- file dialogs: switch the app mode before they exist ---------------- */

/* Windows 10 decides part of the file dialog's colors before the dialog
   window is created (title bar, address bar, buttons follow the app mode at
   that moment, the file list and navigation pane can never be dark there).
   Switching at HCBT_CREATEWND therefore gave a mix of dark and white. So
   the app mode is switched in the calls that show a file dialog, before
   anything of it exists, and switched back when they return (they are
   modal). Only on the "light" path (Windows 10, or FileDialog=light): on
   Windows 11 the app mode stays dark and Windows draws the whole dialog dark,
   exactly as before.
     - GetOpenFileNameW / GetSaveFileNameW: wxFileDialog
     - SHBrowseForFolderW: old folder dialog
     - IFileDialog::Show: wxDirDialog (and MFC's CFileDialog), created with
       CoCreateInstance(CLSID_FileOpenDialog / CLSID_FileSaveDialog)
     - SHFileOperationW: deleting local files (to the recycle bin). The shell
       asks "Are you sure ...?" in a task dialog (white page, gray button
       strip, drawn by DirectUI); with the app mode still dark at that moment
       Windows 10 gave a black title bar and black buttons around a white page
       with white text. */
static void file_dialog_begin(const wchar_t *api)
{
    if (!file_dialog_goes_light())
        return;
    if (InterlockedIncrement(&g_file_dialog_depth) == 1)
        set_app_dark(FALSE, api);
}

static void file_dialog_end(const wchar_t *api)
{
    if (!file_dialog_goes_light())
        return;
    if (InterlockedDecrement(&g_file_dialog_depth) == 0)
        set_app_dark(TRUE, api);
}

/* The real functions, from the import tables (our DLL does not import them
   itself, so FileZilla's load order stays the same). */
typedef BOOL(WINAPI *GetFileName_t)(LPOPENFILENAMEW);
typedef PIDLIST_ABSOLUTE(WINAPI *SHBrowseForFolderW_t)(LPBROWSEINFOW);
typedef HRESULT(WINAPI *CoCreateInstance_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
typedef int(WINAPI *SHFileOperationW_t)(LPSHFILEOPSTRUCTW);
static GetFileName_t real_GetOpenFileNameW, real_GetSaveFileNameW;
static SHBrowseForFolderW_t real_SHBrowseForFolderW;
static CoCreateInstance_t real_CoCreateInstance;
static SHFileOperationW_t real_SHFileOperationW;

static BOOL WINAPI hook_GetOpenFileNameW(LPOPENFILENAMEW ofn)
{
    file_dialog_begin(L"GetOpenFileNameW");
    BOOL ok = real_GetOpenFileNameW(ofn);
    file_dialog_end(L"GetOpenFileNameW returned");
    return ok;
}

static BOOL WINAPI hook_GetSaveFileNameW(LPOPENFILENAMEW ofn)
{
    file_dialog_begin(L"GetSaveFileNameW");
    BOOL ok = real_GetSaveFileNameW(ofn);
    file_dialog_end(L"GetSaveFileNameW returned");
    return ok;
}

static PIDLIST_ABSOLUTE WINAPI hook_SHBrowseForFolderW(LPBROWSEINFOW bi)
{
    file_dialog_begin(L"SHBrowseForFolderW");
    PIDLIST_ABSOLUTE pidl = real_SHBrowseForFolderW(bi);
    file_dialog_end(L"SHBrowseForFolderW returned");
    return pidl;
}

/* Modal as well: it returns when the confirmation and the progress are done. */
static int WINAPI hook_SHFileOperationW(LPSHFILEOPSTRUCTW op)
{
    file_dialog_begin(L"SHFileOperationW");
    int result = real_SHFileOperationW(op);
    file_dialog_end(L"SHFileOperationW returned");
    return result;
}

/* IFileDialog objects. Windows creates part of the dialog (a hidden
   container window) already in CoCreateInstance, so the app mode is switched
   there, and back when Show returns. A dialog that is created but never
   shown is switched back by a thread timer: it fires as soon as the app
   pumps messages again (Show is modal and pumps messages too, but then the
   object is marked as showing and left alone).

   IModalWindow::Show is slot 3 of the IFileDialog vtable (after the three
   IUnknown methods). The vtable belongs to the shell and is shared by all
   dialogs of that class, so it is patched once and the original kept. */
typedef HRESULT(STDMETHODCALLTYPE *Show_t)(void *self, HWND owner);
#define MAX_DIALOG_VTABLES 4
#define MAX_DIALOG_OBJECTS 8
static struct {
    void **vtable;
    Show_t show;
} g_dialog_vtables[MAX_DIALOG_VTABLES];
static int g_dialog_vtable_count;
static struct {
    void *self;
    BOOL begun;   /* counted in g_file_dialog_depth */
    BOOL showing; /* inside Show */
} g_dialog_objects[MAX_DIALOG_OBJECTS];
static SRWLOCK g_dialog_lock = SRWLOCK_INIT;

static Show_t original_show(void *self)
{
    void **vtable = *(void ***)self;
    Show_t show = NULL;
    AcquireSRWLockShared(&g_dialog_lock);
    for (int i = 0; i < g_dialog_vtable_count; i++)
        if (g_dialog_vtables[i].vtable == vtable)
            show = g_dialog_vtables[i].show;
    ReleaseSRWLockShared(&g_dialog_lock);
    return show;
}

/* Index of self in g_dialog_objects, added if new (-1 if full). Lock held. */
static int dialog_object(void *self)
{
    int free_slot = -1;
    for (int i = 0; i < MAX_DIALOG_OBJECTS; i++) {
        if (g_dialog_objects[i].self == self)
            return i;
        if (!g_dialog_objects[i].self && free_slot < 0)
            free_slot = i;
    }
    if (free_slot >= 0) {
        g_dialog_objects[free_slot].self = self;
        g_dialog_objects[free_slot].begun = FALSE;
        g_dialog_objects[free_slot].showing = FALSE;
    }
    return free_slot;
}

/* Marks the object as begun; returns TRUE if begin must be called. */
static BOOL dialog_mark_begun(void *self)
{
    AcquireSRWLockExclusive(&g_dialog_lock);
    int i = dialog_object(self);
    BOOL now = i >= 0 && !g_dialog_objects[i].begun;
    if (now)
        g_dialog_objects[i].begun = TRUE;
    ReleaseSRWLockExclusive(&g_dialog_lock);
    return now;
}

static void CALLBACK end_unshown_dialogs(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
    (void)hwnd; (void)msg; (void)time;
    KillTimer(NULL, id);
    int ends = 0;
    AcquireSRWLockExclusive(&g_dialog_lock);
    for (int i = 0; i < MAX_DIALOG_OBJECTS; i++) {
        if (g_dialog_objects[i].self && g_dialog_objects[i].begun && !g_dialog_objects[i].showing) {
            g_dialog_objects[i].self = NULL;
            g_dialog_objects[i].begun = FALSE;
            ends++;
        }
    }
    ReleaseSRWLockExclusive(&g_dialog_lock);
    while (ends-- > 0)
        file_dialog_end(L"file dialog created but not shown");
}

static HRESULT STDMETHODCALLTYPE hook_IFileDialog_Show(void *self, HWND owner)
{
    Show_t show = original_show(self);
    if (!show)
        return E_FAIL; /* cannot happen: only patched vtables lead here */

    /* Normally begun in CoCreateInstance already; a second Show of the same
       object begins again. */
    if (dialog_mark_begun(self))
        file_dialog_begin(L"IFileDialog::Show");
    AcquireSRWLockExclusive(&g_dialog_lock);
    int i = dialog_object(self);
    if (i >= 0)
        g_dialog_objects[i].showing = TRUE;
    ReleaseSRWLockExclusive(&g_dialog_lock);

    HRESULT hr = show(self, owner);

    BOOL end = FALSE;
    AcquireSRWLockExclusive(&g_dialog_lock);
    i = dialog_object(self);
    if (i >= 0) {
        end = g_dialog_objects[i].begun;
        g_dialog_objects[i].self = NULL;
        g_dialog_objects[i].begun = FALSE;
        g_dialog_objects[i].showing = FALSE;
    }
    ReleaseSRWLockExclusive(&g_dialog_lock);
    if (end)
        file_dialog_end(L"IFileDialog::Show returned");
    return hr;
}

/* Returns the object's IFileDialog pointer (not AddRef'ed: it is only used
   as a key) after patching its vtable. */
static void *patch_dialog_vtable(IUnknown *unk)
{
    static const IID iid_file_dialog = { 0x42f85136, 0xdb7e, 0x439c, { 0x85, 0xf1, 0xe4, 0x07, 0x5d, 0x13, 0x5f, 0xc8 } };
    void *dialog = NULL;
    if (FAILED(unk->lpVtbl->QueryInterface(unk, &iid_file_dialog, &dialog)) || !dialog)
        return NULL;
    void **vtable = *(void ***)dialog;

    AcquireSRWLockExclusive(&g_dialog_lock);
    BOOL known = FALSE;
    for (int i = 0; i < g_dialog_vtable_count; i++)
        known = known || g_dialog_vtables[i].vtable == vtable;
    DWORD old;
    if (!known && g_dialog_vtable_count < MAX_DIALOG_VTABLES && vtable[3] != (void *)hook_IFileDialog_Show &&
        VirtualProtect(&vtable[3], sizeof(void *), PAGE_READWRITE, &old)) {
        g_dialog_vtables[g_dialog_vtable_count].vtable = vtable;
        g_dialog_vtables[g_dialog_vtable_count].show = (Show_t)vtable[3];
        g_dialog_vtable_count++;
        vtable[3] = (void *)hook_IFileDialog_Show;
        VirtualProtect(&vtable[3], sizeof(void *), old, &old);
        dm_log(L"file dialog: IFileDialog::Show hooked (vtable %p)", (void *)vtable);
    }
    ReleaseSRWLockExclusive(&g_dialog_lock);
    ((IUnknown *)dialog)->lpVtbl->Release((IUnknown *)dialog);
    return dialog;
}

static HRESULT WINAPI hook_CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID *out)
{
    static const CLSID clsid_open = { 0xdc1c5a9c, 0xe88a, 0x4dde, { 0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7 } };
    static const CLSID clsid_save = { 0xc0b4e2f3, 0xba21, 0x4773, { 0x8d, 0xba, 0x33, 0x5e, 0xc9, 0x46, 0xeb, 0x8b } };
    BOOL file_dialog = clsid && (IsEqualCLSID(clsid, &clsid_open) || IsEqualCLSID(clsid, &clsid_save));
    if (!file_dialog)
        return real_CoCreateInstance(clsid, outer, ctx, iid, out);

    /* Switch first: the object creates windows of its own right away. */
    file_dialog_begin(L"CoCreateInstance(file dialog)");
    HRESULT hr = real_CoCreateInstance(clsid, outer, ctx, iid, out);
    void *self = SUCCEEDED(hr) && out && *out ? patch_dialog_vtable((IUnknown *)*out) : NULL;
    if (!self || !dialog_mark_begun(self)) {
        file_dialog_end(L"CoCreateInstance(file dialog) failed");
        return hr;
    }
    if (file_dialog_goes_light())
        SetTimer(NULL, 0, 0, end_unshown_dialogs);
    return hr;
}

static int hook_import(HMODULE module, const char *dll, const char *func, void *hook, void **real)
{
    void *original = NULL;
    if (!dm_iat_hook(module, dll, func, hook, &original))
        return 0;
    if (!*real && original)
        *real = original;
    return 1;
}

static void install_file_dialog_hooks(void)
{
    HMODULE modules[8];
    int count = dm_app_modules(modules, 8);
    int ok = 0;
    for (int i = 0; i < count; i++) {
        ok += hook_import(modules[i], "comdlg32.dll", "GetOpenFileNameW", (void *)hook_GetOpenFileNameW, (void **)&real_GetOpenFileNameW);
        ok += hook_import(modules[i], "comdlg32.dll", "GetSaveFileNameW", (void *)hook_GetSaveFileNameW, (void **)&real_GetSaveFileNameW);
        ok += hook_import(modules[i], "SHELL32.dll", "SHBrowseForFolderW", (void *)hook_SHBrowseForFolderW, (void **)&real_SHBrowseForFolderW);
        ok += hook_import(modules[i], "ole32.dll", "CoCreateInstance", (void *)hook_CoCreateInstance, (void **)&real_CoCreateInstance);
        ok += hook_import(modules[i], "SHELL32.dll", "SHFileOperationW", (void *)hook_SHFileOperationW, (void **)&real_SHFileOperationW);
    }
    dm_log(L"file dialog hooks: %d installed", ok);
}

/* Returns TRUE if hwnd must not be darkened. */
static BOOL skip_window(HWND hwnd, HWND parent)
{
    if (!g_skip_prop)
        g_skip_prop = GlobalAddAtomW(L"FileZillaDarkMode.skip");
    if (inside_skipped_window(hwnd, parent))
        return TRUE;
    if (!(GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD) && created_by_shell(hwnd)) {
        wchar_t cls[64] = L"";
        GetClassNameW(hwnd, cls, 64);
        SetPropW(hwnd, MAKEINTATOM(g_skip_prop), (HANDLE)1);
        dm_log(L"left to Windows: shell window %p (%ls)", (void *)hwnd, cls);
        shell_dialog_opened(hwnd);
        return TRUE;
    }
    return FALSE;
}

static void on_window_created(HWND hwnd, HWND parent)
{
    InitOnceExecuteOnce(&g_api_once, init_app_once, NULL, NULL);
    if (!g_api_ok)
        return;

    dm_kind kind = dm_classify(hwnd);
    if (kind == DM_KIND_SKIP || skip_window(hwnd, parent))
        return;
    if (first_of_class(hwnd) && kind == DM_KIND_WINDOW)
        darken_class_background(hwnd);
    g_api.SetWindowSubclass(hwnd, subclass_proc, SUBCLASS_ID, (DWORD_PTR)kind);
}

/* ---- hooks ------------------------------------------------------------- */

static LRESULT CALLBACK cbt_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HCBT_CREATEWND)
        on_window_created((HWND)wp, ((CBT_CREATEWNDW *)lp)->lpcs->hwndParent);
    else if (code == HCBT_DESTROYWND)
        on_window_destroyed((HWND)wp);
    return CallNextHookEx(NULL, code, wp, lp);
}

void dm_dark_process_attach(void)
{
    g_hook_tls = TlsAlloc();
    /* Before wxWidgets initializes, so colors it caches are the dark ones. */
    dm_wx_hooks_install();
    install_file_dialog_hooks();
    dm_dark_thread_attach();
    dm_log(L"dark mode active, CBT hook on thread %lu", GetCurrentThreadId());
}

void dm_dark_thread_attach(void)
{
    if (g_hook_tls == TLS_OUT_OF_INDEXES)
        return;
    HHOOK hook = SetWindowsHookExW(WH_CBT, cbt_proc, NULL, GetCurrentThreadId());
    TlsSetValue(g_hook_tls, hook);
}

void dm_dark_thread_detach(void)
{
    if (g_hook_tls == TLS_OUT_OF_INDEXES)
        return;
    HHOOK hook = (HHOOK)TlsGetValue(g_hook_tls);
    if (hook)
        UnhookWindowsHookEx(hook);
    TlsSetValue(g_hook_tls, NULL);
}
