#include "altswitch_lua.h"

#include <lua.h>

sw_state g_switcher;

/* Starts a cycle if none is running, otherwise moves the selection. */
static void step_or_begin(int delta) {
	/* Once the C++ side has disabled itself there is nothing sane to show, so
	 * say so instead of drawing a stale overlay. The config checks healthy()
	 * too and falls back to the pure-Lua switcher. */
	if (!hal_is_healthy()) {
		hal_report_unavailable();
		return;
	}

	if (!sw_is_active(&g_switcher)) {
		sw_window buf[SW_MAX_WINDOWS];
		int       n = hal_collect_windows(buf, SW_MAX_WINDOWS);
		sw_begin(&g_switcher, buf, n);

		/* Opening the switcher is already the first step: sw_begin preselects
		 * the most recently used other window, which is exactly where a single
		 * alt+tab should land — no matter where that window sits in the
		 * spatially ordered list, or on which workspace. Stepping again here
		 * was the old behaviour from when the list itself was MRU-ordered, and
		 * it made a tap land on the focused window whenever that one happened
		 * to sit first. Backwards starts from the focused window instead. */
		if (delta < 0) {
			g_switcher.index = g_switcher.focused_index;
			sw_step(&g_switcher, delta);
		}
	} else
		sw_step(&g_switcher, delta);

	hal_request_redraw();
}

int altswitch_lua_next(struct lua_State *L) {
	(void)L;
	step_or_begin(1);
	return 0;
}

int altswitch_lua_prev(struct lua_State *L) {
	(void)L;
	step_or_begin(-1);
	return 0;
}

int altswitch_lua_commit(struct lua_State *L) {
	(void)L;
	uint64_t id = sw_selected(&g_switcher);
	sw_reset(&g_switcher);
	hal_end_switch();
	hal_request_redraw();
	if (id)
		hal_focus_window(id);
	return 0;
}

int altswitch_lua_cancel(struct lua_State *L) {
	(void)L;
	sw_reset(&g_switcher);
	hal_end_switch();
	hal_request_redraw();
	return 0;
}

int altswitch_lua_healthy(struct lua_State *L) {
	lua_pushboolean(L, hal_is_healthy());
	return 1;
}

int altswitch_lua_active(struct lua_State *L) {
	lua_pushboolean(L, sw_is_active(&g_switcher));
	return 1;
}

/* Diagnostics: how much the plugin is actually holding. Mostly useful to see
 * whether the thumbnail cache is being kept warm. */
int altswitch_lua_stats(struct lua_State *L) {
	lua_newtable(L);
	lua_pushinteger(L, hal_thumb_count());
	lua_setfield(L, -2, "thumbs");
	lua_pushinteger(L, hal_label_count());
	lua_setfield(L, -2, "labels");
	lua_pushinteger(L, g_switcher.count);
	lua_setfield(L, -2, "windows");
	lua_pushboolean(L, hal_is_healthy());
	lua_setfield(L, -2, "healthy");
	return 1;
}
