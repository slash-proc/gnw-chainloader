#ifndef UI_LIST_H
#define UI_LIST_H

#include "ui.h"

typedef struct {
    const char *title;
    int num_items;
    int selected;
    int scroll_y;
    int visible_lines;
    uint32_t selected_tick;
    uint32_t scroll_tick;     // last time scroll_y actually shifted (drives the scrollbar)

    // Callbacks
    const char* (*get_label)(int index);
    void (*on_action)(int index);
    void (*on_back)(void);
    /* Optional value-selector hook: LEFT/RIGHT on the selected row call this
     * with dir -1/+1 (e.g. the "< Theme >" item). NULL = LEFT/RIGHT ignored. */
    void (*on_adjust)(int index, int dir);
    /* Optional GAME-button hook: a GAME press calls this with the selected row
     * index (e.g. the Launch row flashing an OFW backup to preview its theme).
     * NULL = GAME ignored. */
    void (*on_game)(int index);
    /* Optional per-item enable predicate. Items returning false are shown greyed
     * out and skipped by navigation/action (like dividers). NULL = all enabled. */
    bool (*is_enabled)(int index);

    // Split pane support
    bool is_split;
    void (*draw_right_pane)(int selected_idx, uint32_t selected_tick);
} ui_list_t;

void ui_list_init(ui_list_t *list, const char *title, int num_items,
                  const char* (*get_label)(int), void (*on_action)(int));
void ui_list_update(ui_list_t *list);
void ui_list_draw(ui_list_t *list, int x, int y, int w, int h);

/* Configure a list as a split (two-pane) view: is_split=true, visible_lines=9, and
 * the right-pane drawer. The per-view on_back stays at the call site. */
void ui_list_set_split(ui_list_t *l, void (*draw_right_pane)(int, uint32_t));

/* Generic page forwarders: a ui_window_t whose user_data points at its ui_list uses these as its
 * update_content / draw_content, so a view needs no per-page wrapper. */
void ui_list_page_update(ui_window_t *self);
void ui_list_page_draw(ui_window_t *self);

/* Draw one label/value row of a split view's right detail pane (slot 0..3 -> label y 40/75/110/145,
 * value 15px below). marquee+tick scroll a long value. Shared by the File Browser, FS list, and
 * Partition Viewer right panes so the px/pw geometry + label/value pair live in one place. */
void ui_list_pane_row(int slot, const char *label, const char *value, bool marquee, uint32_t tick);

#endif // UI_LIST_H
