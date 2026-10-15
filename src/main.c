/*
 * Dark mode for FileZilla (unofficial) - entry point.
 *
 * DllMain deliberately does little: read settings, log, hook the color
 * functions in FileZilla's wxWidgets DLLs and install a CBT hook. Everything
 * else happens when the app creates its first window.
 */
#include <windows.h>
#include "config.h"
#include "dark_api.h"
#include "darkmode.h"
#include "log.h"

#define DM_VERSION L"0.1.1 (test version)"

static BOOL g_dark_active;

static void log_startup(HINSTANCE dll)
{
    wchar_t path[MAX_PATH];

    dm_log(L"=== Dark mode for FileZilla " DM_VERSION L" started ===");
    if (GetModuleFileNameW(NULL, path, MAX_PATH))
        dm_log(L"process: %ls", path);
    if (GetModuleFileNameW(dll, path, MAX_PATH))
        dm_log(L"dark mode dll: %ls", path);
    dm_log(L"Windows build %lu", dm_windows_build());

    BOOL windows_dark = dm_windows_prefers_dark();
    dm_log(L"ini Mode: %ls", dm_mode_name(g_cfg.mode));
    dm_log(L"ini FileDialog: %ls", dm_file_dialog_name(g_cfg.file_dialog));    dm_log(L"Windows setting: %ls", windows_dark ? L"dark" : L"light");
}

BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID reserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        /* Only log for FileZilla itself; other programs in the folder (if any)
           get nothing but the version.dll forwarding. */
        if (!dm_is_target_process()) {
            DisableThreadLibraryCalls(dll);
            break;
        }
        dm_config_load(dll);
        dm_log_init(dll, g_cfg.log);
        log_startup(dll);

        if (!dm_is_dark()) {
            dm_log(L"dark mode OFF");
        } else {
            g_dark_active = TRUE;
            dm_dark_process_attach();
        }
        /* Thread notifications are only needed to hook new UI threads. */
        if (!g_dark_active)
            DisableThreadLibraryCalls(dll);
        break;

    case DLL_THREAD_ATTACH:
        dm_dark_thread_attach();
        break;

    case DLL_THREAD_DETACH:
        dm_dark_thread_detach();
        break;

    case DLL_PROCESS_DETACH:
        /* reserved != NULL: the process is exiting, hooks die with it. */
        if (reserved == NULL && g_dark_active)
            dm_dark_thread_detach();
        dm_log(L"shut down");
        dm_log_close();
        break;
    }
    return TRUE;
}
