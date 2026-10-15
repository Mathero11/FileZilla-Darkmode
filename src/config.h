#ifndef DM_CONFIG_H
#define DM_CONFIG_H

#include <windows.h>

typedef enum {
    DM_MODE_AUTO,  /* follow Windows (dark app mode) */
    DM_MODE_DARK,  /* always dark */
    DM_MODE_LIGHT  /* always light: dark mode off */
} dm_mode;

/* What happens to the app's dark mode while a Windows file dialog (Open,
   Save, choose folder) is open. The dialog itself is never subclassed. */
typedef enum {
    DM_FILE_DIALOG_AUTO,  /* dark on Windows 11, light on Windows 10 */
    DM_FILE_DIALOG_DARK,  /* keep the dark app mode: Windows themes the dialog */
    DM_FILE_DIALOG_LIGHT  /* app mode "default" while it is open: fully light */
} dm_file_dialog;

typedef struct {
    dm_mode mode;
    BOOL log;
    dm_file_dialog file_dialog;
} dm_config;

extern dm_config g_cfg;

/* Reads fz-darkmode.ini next to the DLL; missing file = defaults. */
void dm_config_load(HINSTANCE dll);

/* Is Windows set to "dark app mode"? */
BOOL dm_windows_prefers_dark(void);

/* Should dark mode be on now (ini and Windows setting combined)? */
BOOL dm_is_dark(void);

const wchar_t *dm_mode_name(dm_mode mode);
const wchar_t *dm_file_dialog_name(dm_file_dialog fd);

#endif
