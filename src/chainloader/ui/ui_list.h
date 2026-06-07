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
    
    // Callbacks
    const char* (*get_label)(int index);
    void (*on_action)(int index);
    void (*on_back)(void);
    
    // Split pane support
    bool is_split;
    void (*draw_right_pane)(int selected_idx, uint32_t selected_tick);
} ui_list_t;

void ui_list_init(ui_list_t *list, const char *title, int num_items, 
                  const char* (*get_label)(int), void (*on_action)(int));
void ui_list_update(ui_list_t *list);
void ui_list_draw(ui_list_t *list, int x, int y, int w, int h);

#endif // UI_LIST_H
