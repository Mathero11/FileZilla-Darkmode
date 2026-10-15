#include "config.h"

#define INI_NAME L"fz-darkmode.ini"
#define INI_SECTION L"DarkMode"

dm_config g_cfg = { DM_MODE_AUTO, TRUE, DM_FILE_DIALOG_AUTO };

void dm_config_load(HINSTANCE dll)
{
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(dll, path, MAX_PATH);
    wchar_t *slash = len ? wcsrchr(path, L'\\') : NULL;
    if (!slash || (slash - path) + 1 + wcslen(INI_NAME) >= MAX_PATH)
        return;
    lstrcpyW(slash + 1, INI_NAME);

    wchar_t mode[16];
    GetPrivateProfileStringW(INI_SECTION, L"Mode", L"auto", mode, 16, path);
    if (lstrcmpiW(mode, L"dark") == 0)
        g_cfg.mode = DM_MODE_DARK;
    else if (lstrcmpiW(mode, L"light") == 0)
        g_cfg.mode = DM_MODE_LIGHT;
    else
        g_cfg.mode = DM_MODE_AUTO;

    g_cfg.log = GetPrivateProfileIntW(INI_SECTION, L"Log", 1, path) != 0;

    wchar_t fd[16];
    GetPrivateProfileStringW(INI_SECTION, L"FileDialog", L"auto", fd, 16, path);
    if (lstrcmpiW(fd, L"dark") == 0)
        g_cfg.file_dialog = DM_FILE_DIALOG_DARK;
    else if (lstrcmpiW(fd, L"light") == 0)
        g_cfg.file_dialog = DM_FILE_DIALOG_LIGHT;
    else
        g_cfg.file_dialog = DM_FILE_DIALOG_AUTO;
}

BOOL dm_windows_prefers_dark(void)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return FALSE;

    DWORD value = 1, size = sizeof(value), type = 0;
    LONG rc = RegQueryValueExW(key, L"AppsUseLightTheme", NULL, &type, (BYTE *)&value, &size);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS && type == REG_DWORD && value == 0;
}

BOOL dm_is_dark(void)
{
    switch (g_cfg.mode) {
    case DM_MODE_DARK:
        return TRUE;
    case DM_MODE_LIGHT:
        return FALSE;
    default:
        return dm_windows_prefers_dark();
    }
}

const wchar_t *dm_mode_name(dm_mode mode)
{
    switch (mode) {
    case DM_MODE_DARK:
        return L"dark (always dark)";
    case DM_MODE_LIGHT:
        return L"light (dark mode off)";
    default:
        return L"auto (follow Windows)";
    }
}

const wchar_t *dm_file_dialog_name(dm_file_dialog fd)
{
    switch (fd) {
    case DM_FILE_DIALOG_DARK:
        return L"dark (keep dark app mode)";
    case DM_FILE_DIALOG_LIGHT:
        return L"light (default app mode while open)";
    default:
        return L"auto (dark on Windows 11, light on Windows 10)";
    }
}
