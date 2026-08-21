/* Pure C core of the alt-tab switcher.
 *
 * Nothing in here knows about Hyprland: the C++ glue in plugin.cpp collects the
 * windows into sw_window records, calls into this file for ordering, selection
 * and geometry, and renders whatever comes back. Keeping it that way means the
 * part that has to track Hyprland's moving internals stays as small as possible.
 */
#ifndef HYPR_ALTSWITCH_SWITCHER_H
#define HYPR_ALTSWITCH_SWITCHER_H

#include <stdint.h>

#define SW_MAX_WINDOWS 64
#define SW_MAX_GROUPS  16
#define SW_TITLE_LEN   128
#define SW_CLASS_LEN   64
#define SW_LABEL_LEN   32

typedef struct {
	float x, y, w, h;
} sw_rect;

typedef struct {
	uint64_t id;      /* opaque window handle, never dereferenced on the C side */
	int32_t  history; /* 0 = currently focused, 1 = the one before it, ... */
	int32_t  focused; /* 1 for the window that has focus right now */
	int32_t  workspace;
	char     ws_label[SW_LABEL_LEN]; /* workspace name, drawn above its group */
	float    pos_x;   /* screen position, decides the left-to-right order */
	float    pos_y;
	float    src_w;   /* real window size, used to keep the thumbnail aspect */
	float    src_h;
	char     class_name[SW_CLASS_LEN];
	char     title[SW_TITLE_LEN];
} sw_window;

typedef struct {
	int32_t workspace;
	char    label[SW_LABEL_LEN];
	int     first;        /* index of its first tile */
	int     count;
	int     first_in_row; /* 1 when no divider should be drawn to its left */
	sw_rect box;          /* area the group occupies */
	sw_rect label_box;    /* where to draw the workspace name */
} sw_group;

typedef struct {
	sw_window items[SW_MAX_WINDOWS];
	int       count;
	int       index;         /* selected entry */
	int       focused_index; /* where the focused window sits in the list */
	int       active;        /* 0 = overlay hidden */
} sw_state;

/* Tunables for the overlay geometry, all in logical pixels. */
typedef struct {
	float tile_w;
	float tile_h;
	float tile_gap;
	float group_gap;   /* extra space between workspaces */
	float group_pad;   /* breathing room between a divider and the tiles */
	float label_h;     /* height of the workspace name */
	float label_gap;   /* air between the name and the tiles below it */
	float panel_pad;
	float panel_max_w; /* fraction of the screen the panel may take */
	float panel_max_h; /* … before tiles start shrinking */
	float tile_min_w;  /* fraction of tile_w a cell may shrink to */
} sw_layout_cfg;

typedef struct {
	sw_rect  panel;
	sw_rect  tiles[SW_MAX_WINDOWS];  /* the cell: plate, border, selection ring */
	sw_rect  thumbs[SW_MAX_WINDOWS]; /* the picture inside it, aspect preserved */
	int      tile_count;
	sw_group groups[SW_MAX_GROUPS];
	int      group_count;
	int      grouped; /* 0 when everything sits on one workspace */
} sw_layout_result;

void          sw_reset(sw_state *s);
/* Takes a snapshot, orders it by workspace and screen position, and preselects
 * the most recently used window that is not the current one. */
void          sw_begin(sw_state *s, const sw_window *windows, int count);
void          sw_step(sw_state *s, int delta);
int           sw_is_active(const sw_state *s);
int           sw_index(const sw_state *s);
uint64_t      sw_selected(const sw_state *s);
sw_layout_cfg sw_default_cfg(void);
void          sw_layout(const sw_state *s, float screen_w, float screen_h, const sw_layout_cfg *cfg, sw_layout_result *out);

#endif /* HYPR_ALTSWITCH_SWITCHER_H */
