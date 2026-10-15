#ifndef DM_CONTROLS_H
#define DM_CONTROLS_H

#include <windows.h>

typedef enum {
    DM_KIND_SKIP,      /* never subclassed (IME, OLE helper windows) */
    DM_KIND_WINDOW,    /* wxWidgets window without special handling */
    DM_KIND_DIALOG,    /* #32770 (wxDialog is one too) */
    DM_KIND_BUTTON,
    DM_KIND_EDIT,
    DM_KIND_STATIC,
    DM_KIND_COMBOBOX,
    DM_KIND_COMBOLBOX, /* drop-down list of a combo box */
    DM_KIND_LISTBOX,
    DM_KIND_LISTVIEW,
    DM_KIND_HEADER,
    DM_KIND_TREEVIEW,
    DM_KIND_UPDOWN,
    DM_KIND_TOOLTIP,
    DM_KIND_SCROLLBAR,
    DM_KIND_RICHEDIT,  /* message log */
    DM_KIND_TAB,       /* wxNotebook (Site Manager, Settings pages) */
    DM_KIND_STATUSBAR
} dm_kind;

dm_kind dm_classify(HWND hwnd);

/* Applies dark themes and colors to a control right after it is created. */
void dm_control_created(HWND hwnd, dm_kind kind);

/* Control specific message handling. Returns TRUE if the message was fully
   handled and *result must be returned. */
BOOL dm_control_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, dm_kind kind, LRESULT *result);

/* Dark check box / radio button glyph in 'box'. check: BST_UNCHECKED,
   BST_CHECKED or BST_INDETERMINATE; sub: 1 normal, 2 hot, 3 pressed,
   4 disabled. Also used for check boxes that wxWidgets draws with the theme. */
void dm_draw_check_glyph(HDC hdc, const RECT *box, BOOL radio, int check, int sub);

/* Filled, anti-aliased triangle centered in r. dir: 0 up, 1 down, 2 left,
   3 right. */
void dm_paint_arrow(HDC hdc, const RECT *r, int dir, COLORREF color);

/* px at 96 DPI scaled to the session's DPI (FileZilla is system DPI aware). */
int dm_scale(int px);

#endif
