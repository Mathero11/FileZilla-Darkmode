#ifndef DM_LOG_H
#define DM_LOG_H

#include <windows.h>

/* Opens the log file next to the DLL, or in %TEMP% if that is not writable. */
void dm_log_init(HINSTANCE dll, BOOL enabled);
void dm_log(const wchar_t *fmt, ...);
void dm_log_close(void);

#endif
