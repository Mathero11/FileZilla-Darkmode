#ifndef DM_DARKMODE_H
#define DM_DARKMODE_H

#include <windows.h>

/* Should this process get dark mode at all (is it filezilla.exe)? */
BOOL dm_is_target_process(void);

/* Called from DllMain. They only install/remove per-thread CBT hooks; the real
   work starts when the first window is created, outside the loader lock. */
void dm_dark_process_attach(void);
void dm_dark_thread_attach(void);
void dm_dark_thread_detach(void);

#endif
