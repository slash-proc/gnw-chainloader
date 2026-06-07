#ifndef SYSTEM_MOD_UI_H
#define SYSTEM_MOD_UI_H

/*
 * Module UI service — callbacks the host exposes to PIE modules so they can talk
 * to the user (confirm dialogs, error modals) without linking the core UI. The
 * host fills this once (mod_ui()) pointing at the core's ui_show_* functions; a
 * module receives a pointer to it through its own host-API struct.
 *
 * `confirm` is ASYNC: it pushes a modal and returns immediately; `on_yes` runs
 * later, when the user accepts (B/cancel just dismisses). The callback is module
 * code, which stays resident in the module pool, so the pointer is valid then.
 *
 * This is the reusable foundation for "modules that drive the UI": the language
 * module uses it to confirm an SD to LittleFS language-pack install before writing.
 */
typedef struct {
    void (*confirm)(const char *message, void (*on_yes)(void));
    void (*error)(const char *message);
} mod_ui_t;

/* The host's UI service vtable (points at ui_show_confirm / ui_show_error). */
const mod_ui_t *mod_ui(void);

#endif /* SYSTEM_MOD_UI_H */
