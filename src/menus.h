#ifndef DM_MENUS_H
#define DM_MENUS_H

#include <windows.h>

/*
 * Menu bar and popup menus. Popup menus are dark through the app mode
 * (SetPreferredAppMode); the menu bar, its items and the white line under it
 * are painted here.
 *
 * Handles messages for top-level windows. Returns TRUE if the message was
 * fully handled and *result must be returned.
 */
BOOL dm_menu_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result);

#endif
