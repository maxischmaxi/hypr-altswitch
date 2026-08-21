/* Lua entry points and the shared switcher state — plain C.
 *
 * Hyprland's plugin API takes Lua C callbacks (int(*)(lua_State*)), so this is
 * the layer the config actually talks to: hl.plugin.altswitch.next() and
 * friends. Anything that needs Hyprland internals goes back out through the
 * hal_* functions, which plugin.cpp implements.
 */
#ifndef HYPR_ALTSWITCH_LUA_H
#define HYPR_ALTSWITCH_LUA_H

#include "switcher.h"

struct lua_State;

/* Shared with the renderer in plugin.cpp. */
extern sw_state g_switcher;

/* Lua C callbacks, registered from plugin.cpp. */
int altswitch_lua_next(struct lua_State *L);
int altswitch_lua_prev(struct lua_State *L);
int altswitch_lua_commit(struct lua_State *L);
int altswitch_lua_cancel(struct lua_State *L);
int altswitch_lua_active(struct lua_State *L);
int altswitch_lua_healthy(struct lua_State *L);
int altswitch_lua_stats(struct lua_State *L);

/* Implemented in plugin.cpp (C++ side), declared here so C can call it. */
int  hal_collect_windows(sw_window *out, int max);
void hal_focus_window(uint64_t id);
void hal_request_redraw(void);
void hal_end_switch(void);        /* release per-cycle resources (thumbnails) */
int  hal_is_healthy(void);        /* 0 once the plugin disabled itself */
void hal_report_unavailable(void);/* notify the user why nothing happens */
int  hal_thumb_count(void);       /* cached window pictures */
int  hal_label_count(void);

#endif /* HYPR_ALTSWITCH_LUA_H */
