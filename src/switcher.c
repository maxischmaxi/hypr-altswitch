#include "switcher.h"

#include <string.h>

void sw_reset(sw_state *s) {
	if (!s)
		return;
	s->count         = 0;
	s->index         = 0;
	s->focused_index = 0;
	s->active        = 0;
}

/* Order: workspace ascending, then left to right on screen, then top to bottom.
 * That way the tiles sit the way the windows do — which is what you look for
 * when picking one — while the *selection* still starts on the most recently
 * used window further down.
 *
 * Insertion sort: the list is one screen worth of windows, and it keeps equal
 * entries in their original order. */
static int before(const sw_window *a, const sw_window *b) {
	if (a->workspace != b->workspace)
		return a->workspace < b->workspace;
	if (a->pos_x != b->pos_x)
		return a->pos_x < b->pos_x;
	return a->pos_y < b->pos_y;
}

static void sort_spatially(sw_window *items, int count) {
	for (int i = 1; i < count; ++i) {
		sw_window key = items[i];
		int       j   = i - 1;
		while (j >= 0 && !before(&items[j], &key)) {
			items[j + 1] = items[j];
			--j;
		}
		items[j + 1] = key;
	}
}

void sw_begin(sw_state *s, const sw_window *windows, int count) {
	if (!s || !windows)
		return;
	if (count > SW_MAX_WINDOWS)
		count = SW_MAX_WINDOWS;
	if (count < 0)
		count = 0;

	memcpy(s->items, windows, (size_t)count * sizeof(sw_window));
	s->count = count;
	sort_spatially(s->items, s->count);

	/* Where the focused window ended up after sorting. Prefer the explicit flag
	 * over history == 0: the flag comes straight from the compositor. */
	s->focused_index = 0;
	for (int i = 0; i < s->count; ++i) {
		if (s->items[i].focused || (s->items[i].history == 0 && !s->items[s->focused_index].focused)) {
			s->focused_index = i;
			if (s->items[i].focused)
				break;
		}
	}

	/* Preselect the most recently used window that is not the focused one, so a
	 * single alt+tab still toggles even though the tiles are in screen order. */
	int best = -1;
	for (int i = 0; i < s->count; ++i) {
		if (i == s->focused_index)
			continue;
		if (best < 0 || s->items[i].history < s->items[best].history)
			best = i;
	}

	s->index  = best >= 0 ? best : s->focused_index;
	s->active = s->count > 1;
}

void sw_step(sw_state *s, int delta) {
	if (!s || !s->active || s->count <= 0)
		return;
	int n = s->count;
	/* C's % keeps the sign of the dividend, so normalise into [0, n). */
	int next = (s->index + delta) % n;
	if (next < 0)
		next += n;
	s->index = next;
}

int sw_is_active(const sw_state *s) {
	return s && s->active && s->count > 0;
}

int sw_index(const sw_state *s) {
	return s ? s->index : 0;
}

uint64_t sw_selected(const sw_state *s) {
	if (!sw_is_active(s) || s->index < 0 || s->index >= s->count)
		return 0;
	return s->items[s->index].id;
}

int sw_select(sw_state *s, int index) {
	if (!sw_is_active(s) || index < 0 || index >= s->count || index == s->index)
		return 0;
	s->index = index;
	return 1;
}

static int inside(const sw_rect *r, float x, float y) {
	return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

int sw_hit_test(const sw_layout_result *l, float x, float y) {
	if (!l || l->tile_count <= 0)
		return SW_HIT_NONE;
	if (!inside(&l->panel, x, y))
		return SW_HIT_NONE;

	for (int i = 0; i < l->tile_count && i < SW_MAX_WINDOWS; ++i) {
		if (inside(&l->tiles[i], x, y))
			return i;
	}
	return SW_HIT_PANEL;
}

sw_layout_cfg sw_default_cfg(void) {
	sw_layout_cfg cfg;
	cfg.tile_w      = 300.0f;
	cfg.tile_h      = 190.0f;
	cfg.tile_gap    = 10.0f;
	cfg.group_gap   = 22.0f;
	cfg.group_pad   = 10.0f;
	/* Just the text — the label texture is filled to the brim, so this height is
	 * what you actually see. */
	cfg.label_h     = 11.0f;
	cfg.label_gap   = 9.0f;
	cfg.panel_pad   = 15.0f;
	cfg.panel_max_w = 0.92f;
	cfg.panel_max_h = 0.80f;
	cfg.tile_min_w  = 0.45f;
	return cfg;
}

void sw_layout(const sw_state *s, float screen_w, float screen_h, const sw_layout_cfg *cfg, sw_layout_result *out) {
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	if (!sw_is_active(s) || !cfg || screen_w <= 0 || screen_h <= 0)
		return;

	int n = s->count;
	if (n > SW_MAX_WINDOWS)
		n = SW_MAX_WINDOWS;

	/* Walk the (already sorted) list and cut it into workspace groups. */
	for (int i = 0; i < n; ++i) {
		sw_group *g = out->group_count > 0 ? &out->groups[out->group_count - 1] : 0;
		if (!g || g->workspace != s->items[i].workspace) {
			if (out->group_count >= SW_MAX_GROUPS)
				break;
			g            = &out->groups[out->group_count++];
			g->workspace = s->items[i].workspace;
			g->first     = i;
			g->count     = 0;
			memcpy(g->label, s->items[i].ws_label, SW_LABEL_LEN);
		}
		g->count++;
		out->tile_count = i + 1;
	}

	out->grouped = out->group_count > 1;

	/* Everything is sized against a 1080p reference, so the overlay keeps its
	 * proportions on a 4K screen instead of turning into a row of stamps. */
	float ui = screen_h / 1080.0f;
	if (ui < 0.75f)
		ui = 0.75f;
	if (ui > 2.5f)
		ui = 2.5f;

	const float tile_gap  = cfg->tile_gap * ui;
	const float panel_pad = cfg->panel_pad * ui;
	const float group_pad = out->grouped ? cfg->group_pad * ui : 0.0f;
	const float label_h   = out->grouped ? cfg->label_h * ui : 0.0f;
	const float label_gap = out->grouped ? cfg->label_gap * ui : 0.0f;
	const float group_gap = out->grouped ? cfg->group_gap * ui : tile_gap;

	const float max_w = screen_w * cfg->panel_max_w;
	const float max_h = screen_h * cfg->panel_max_h;
	const float inner = max_w - 2.0f * panel_pad;

	float tile_w = cfg->tile_w * ui;
	float tile_h = cfg->tile_h * ui;

	/* Per-tile width (letterboxed), which row of its group it lands on, and the
	 * measured size of every group. Recomputed each attempt. */
	float widths[SW_MAX_WINDOWS];
	int   tile_row[SW_MAX_WINDOWS];
	float gw[SW_MAX_GROUPS], gh[SW_MAX_GROUPS];
	int   group_row[SW_MAX_GROUPS];
	float row_height[SW_MAX_GROUPS];
	int   row_count  = 1;
	float panel_body = 0.0f;
	float widest_row = 0.0f;

	/* Tiles wrap onto further rows rather than shrinking — like the Windows
	 * switcher. Only when the whole block would grow past panel_max_h do the
	 * tiles get smaller, and then we measure again. */
	for (int attempt = 0; attempt < 12; ++attempt) {
		for (int i = 0; i < out->tile_count; ++i) {
			const sw_window *win = &s->items[i];
			float            w   = tile_w;
			if (win->src_w > 0.0f && win->src_h > 0.0f) {
				const float src_aspect  = win->src_w / win->src_h;
				const float tile_aspect = tile_w / tile_h;
				if (src_aspect < tile_aspect)
					w = tile_h * src_aspect;
			}
			/* A very tall or very flat window would otherwise collapse into a
			 * sliver you cannot recognise. The cell keeps a floor; the picture
			 * inside it keeps the real aspect and gets letterboxed. */
			const float floor_w = tile_w * cfg->tile_min_w;
			widths[i] = w < floor_w ? floor_w : w;
		}

		/* Wrap inside each group. */
		for (int gi = 0; gi < out->group_count; ++gi) {
			sw_group   *g     = &out->groups[gi];
			float       avail = inner - 2.0f * group_pad;
			if (avail < tile_w)
				avail = tile_w;

			int   rows   = 1;
			float cur    = 0.0f;
			float widest = 0.0f;

			for (int k = 0; k < g->count; ++k) {
				const int   i    = g->first + k;
				const float need = (cur > 0.0f ? tile_gap : 0.0f) + widths[i];
				if (cur > 0.0f && cur + need > avail) {
					++rows;
					cur = 0.0f;
				}
				tile_row[i] = rows - 1;
				cur += (cur > 0.0f ? tile_gap : 0.0f) + widths[i];
				if (cur > widest)
					widest = cur;
			}

			gw[gi] = widest + 2.0f * group_pad;
			gh[gi] = label_h + label_gap + (float)rows * tile_h + (float)(rows - 1) * tile_gap;
		}

		/* Wrap the group cards themselves. */
		row_count  = 0;
		panel_body = 0.0f;
		widest_row = 0.0f;
		float cur  = 0.0f;
		float rowh = 0.0f;

		for (int gi = 0; gi < out->group_count; ++gi) {
			const float need = (cur > 0.0f ? group_gap : 0.0f) + gw[gi];
			if (cur > 0.0f && cur + need > inner) {
				row_height[row_count++] = rowh;
				panel_body += rowh + group_gap;
				cur  = 0.0f;
				rowh = 0.0f;
			}
			group_row[gi] = row_count;
			out->groups[gi].first_in_row = (cur == 0.0f);
			cur += (cur > 0.0f ? group_gap : 0.0f) + gw[gi];
			if (gh[gi] > rowh)
				rowh = gh[gi];
			if (cur > widest_row)
				widest_row = cur;
		}
		row_height[row_count++] = rowh;
		panel_body += rowh;

		out->panel.w = widest_row + 2.0f * panel_pad;
		out->panel.h = panel_body + 2.0f * panel_pad;

		if (out->panel.h <= max_h || tile_h < 60.0f * ui)
			break;

		tile_w *= 0.85f;
		tile_h *= 0.85f;
	}

	out->panel.x = (screen_w - out->panel.w) * 0.5f;
	out->panel.y = (screen_h - out->panel.h) * 0.5f;

	/* Place: rows of cards, centred; inside a card, rows of tiles, centred. */
	float row_y = out->panel.y + panel_pad;

	for (int r = 0; r < row_count; ++r) {
		float row_w = 0.0f;
		for (int gi = 0; gi < out->group_count; ++gi) {
			if (group_row[gi] != r)
				continue;
			row_w += (row_w > 0.0f ? group_gap : 0.0f) + gw[gi];
		}

		float x = out->panel.x + (out->panel.w - row_w) * 0.5f;

		for (int gi = 0; gi < out->group_count; ++gi) {
			sw_group *g = &out->groups[gi];
			if (group_row[gi] != r)
				continue;

			g->box.x = x;
			g->box.y = row_y;
			g->box.w = gw[gi];
			g->box.h = gh[gi];

			g->label_box.x = g->box.x + group_pad;
			g->label_box.y = g->box.y;
			g->label_box.w = gw[gi] - 2.0f * group_pad;
			g->label_box.h = label_h;

			/* Centre each tile row inside the card, so a half-filled last row
			 * does not hang to the left. */
			int tr = 0;
			for (int k = 0; k < g->count; ++k) {
				const int i = g->first + k;
				if (k > 0 && tile_row[i] == tr)
					continue;
				tr = tile_row[i];

				float line_w = 0.0f;
				for (int m = k; m < g->count; ++m) {
					const int j = g->first + m;
					if (tile_row[j] != tr)
						break;
					line_w += (line_w > 0.0f ? tile_gap : 0.0f) + widths[j];
				}

				float tx = g->box.x + (gw[gi] - line_w) * 0.5f;
				for (int m = k; m < g->count; ++m) {
					const int        j   = g->first + m;
					const sw_window *win = &s->items[j];
					if (tile_row[j] != tr)
						break;

					const float cell_y = g->box.y + label_h + label_gap + (float)tr * (tile_h + tile_gap);

					out->tiles[j].x = tx;
					out->tiles[j].y = cell_y;
					out->tiles[j].w = widths[j];
					out->tiles[j].h = tile_h;

					/* Fit the picture into the cell without distorting it. */
					float pw = widths[j];
					float ph = tile_h;
					if (win->src_w > 0.0f && win->src_h > 0.0f) {
						const float src_aspect  = win->src_w / win->src_h;
						const float cell_aspect = widths[j] / tile_h;
						if (src_aspect > cell_aspect)
							ph = pw / src_aspect;
						else
							pw = ph * src_aspect;
					}

					out->thumbs[j].x = tx + (widths[j] - pw) * 0.5f;
					out->thumbs[j].y = cell_y + (tile_h - ph) * 0.5f;
					out->thumbs[j].w = pw;
					out->thumbs[j].h = ph;

					tx += widths[j] + tile_gap;
				}
			}

			x += gw[gi] + group_gap;
		}

		row_y += row_height[r] + group_gap;
	}
}
