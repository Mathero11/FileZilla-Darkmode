#ifndef DM_IAT_H
#define DM_IAT_H

#include <windows.h>

/*
 * Replaces one entry in the import address table of 'module'. On success the
 * previous target is stored in *original (if not NULL). Hooking the same entry
 * twice is harmless: an entry that already points to 'hook' is left alone.
 */
BOOL dm_iat_hook(HMODULE module, const char *dll, const char *func, void *hook, void **original);

/* Calls 'fn' with the name of every DLL that 'module' imports. Stops when
   'fn' returns FALSE. */
typedef BOOL (*dm_import_fn)(const char *dll, void *ctx);
void dm_for_each_import(HMODULE module, dm_import_fn fn, void *ctx);

#endif
