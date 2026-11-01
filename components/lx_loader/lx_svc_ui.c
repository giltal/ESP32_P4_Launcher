/*
 * UI interface: launcher-drawn browser and message box (lx_ui). The calls run
 * on the app's task; lx_ui brackets each locked stretch with SVC_ENTER/LEAVE
 * (guard below), so a stop request ends them (CANCEL) and a forced stop never
 * catches them holding the display or a file.
 */
#include "lx_services.h"
#include "lx_ui.h"

static void guard_enter(void) { SVC_ENTER(); }
static void guard_leave(void) { SVC_LEAVE(); }

static int ui_pick_file(const papp_pick_cfg_t *cfg, papp_pick_result_t *out)
{
    lx_ui_set_guard(guard_enter, guard_leave);
    return lx_ui_pick_file(cfg, out, &lx_svc_exit_flag);
}

static void ui_message(const char *title, const char *text)
{
    lx_ui_set_guard(guard_enter, guard_leave);
    lx_ui_message(title, text, &lx_svc_exit_flag);
}

static int ui_menu(const char *title, const char *const *items, int n, int initial)
{
    lx_ui_set_guard(guard_enter, guard_leave);
    return lx_ui_menu(title, items, n, initial, &lx_svc_exit_flag);
}

static int ui_volume(void)
{
    lx_ui_set_guard(guard_enter, guard_leave);
    return lx_ui_volume(&lx_svc_exit_flag);
}

const papp_if_ui_t lx_if_ui = {
    .hdr = { PAPP_IF_UI, PAPP_IF_UI_VER, sizeof(papp_if_ui_t) },
    .pick_file = ui_pick_file,
    .message   = ui_message,
    .menu      = ui_menu,
    .volume    = ui_volume,
};
