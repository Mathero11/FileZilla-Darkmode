#include <stdarg.h>
#include <stdio.h>
#include "log.h"

#define LOG_NAME L"fz-darkmode.log"
#define LOG_MAX_SIZE (1024 * 1024)

static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_lock;

static HANDLE open_log(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return h;

    /* Grown too large: start over. */
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size) && size.QuadPart > LOG_MAX_SIZE) {
        CloseHandle(h);
        h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    return h;
}

void dm_log_init(HINSTANCE dll, BOOL enabled)
{
    InitializeCriticalSection(&g_log_lock);
    if (!enabled)
        return;

    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(dll, path, MAX_PATH);
    wchar_t *slash = len ? wcsrchr(path, L'\\') : NULL;
    if (slash && (slash - path) + 1 + wcslen(LOG_NAME) < MAX_PATH) {
        lstrcpyW(slash + 1, LOG_NAME);
        g_log = open_log(path);
    }

    /* E.g. installed in Program Files without write access. */
    if (g_log == INVALID_HANDLE_VALUE) {
        len = GetTempPathW(MAX_PATH, path);
        if (len && len + wcslen(LOG_NAME) < MAX_PATH) {
            lstrcatW(path, LOG_NAME);
            g_log = open_log(path);
        }
    }

    if (g_log != INVALID_HANDLE_VALUE)
        dm_log(L"log file: %ls", path);
}

void dm_log(const wchar_t *fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE)
        return;

    wchar_t line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = _snwprintf(line, 64, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] ",
                       t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                       t.wMilliseconds, GetCurrentProcessId());
    if (n < 0)
        n = 0;

    va_list ap;
    va_start(ap, fmt);
    int m = _vsnwprintf(line + n, 1024 - n - 3, fmt, ap);
    va_end(ap);
    n = (m < 0) ? 1024 - 3 : n + m;
    line[n++] = L'\r';
    line[n++] = L'\n';
    line[n] = 0;

    char utf8[3 * 1024];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, n, utf8, sizeof(utf8), NULL, NULL);

    EnterCriticalSection(&g_log_lock);
    DWORD written;
    WriteFile(g_log, utf8, bytes, &written, NULL);
    LeaveCriticalSection(&g_log_lock);
}

void dm_log_close(void)
{
    if (g_log != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
}
