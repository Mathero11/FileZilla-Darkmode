#ifndef DM_WX_HOOKS_H
#define DM_WX_HOOKS_H

#include <windows.h>

/*
 * Hooks the color and theme functions in the import tables of FileZilla's
 * wxWidgets DLLs (wxmsw*_core, wxmsw*_aui, ...). wxWidgets takes nearly all
 * of its colors from GetSysColor and FileZilla already has code paths for a
 * dark background, so dark system colors alone darken most of the window.
 * Safe to call from DllMain and more than once.
 */
void dm_wx_hooks_install(void);

/* System color as the app should see it in dark mode. */
COLORREF dm_dark_sys_color(int index);

/* Is 'module' one of the modules whose imports are hooked (the exe and the
   wxmsw DLLs)? Their window classes get a dark class brush. */
BOOL dm_is_app_module(HMODULE module);

/* Copies those modules into out (at most max); returns how many. */
int dm_app_modules(HMODULE *out, int max);

#endif
