/* Tiling layouts.
 *
 * A stateless master-stack layout over each output's active workspace, driven by
 * the config (master-mfact/nmaster/orientation, gaps, smart-gaps). A layout is a
 * pure function of the window-list order and the config. Windows on inactive
 * workspaces are hidden. Borders arrive in a follow-up; content fills the tile.
 */
#include "w3ld.h"

/* ------------------------------------------------------------------- helpers */

static void place_window (
	struct w3ld_window *window,
	int x,
	int y,
	int width,
	int height
) {
	window->geom = (struct wlr_box){ .x = x, .y = y, .width = width,
		.height = height };
	wlr_scene_node_set_position(&window->scene_tree->node, x, y);
	wlr_xdg_toplevel_set_size(window->xdg_toplevel, width, height);
	DBG("tile %s -> %d,%d %dx%d",
			window->xdg_toplevel->app_id ? window->xdg_toplevel->app_id : "?",
			x, y, width, height);
}

static int count_tiled (struct w3ld_workspace *workspace) {
	int count = 0;
	struct w3ld_window *window;
	wl_list_for_each(window, &workspace->output->server->windows, link) {
		if (window->mapped && window->workspace == workspace)
			count++;
	}
	return count;
}

/* Place windows [start, start+count) of the active workspace into `box`, stacked
 * along one axis with `gap` between them; the last window fills the remainder to
 * absorb integer rounding. */
static void place_region (
	struct w3ld_output *output,
	int start,
	int count,
	struct wlr_box box,
	bool vertical,
	int gap
) {
	int index = 0;
	int placed = 0;
	struct w3ld_window *window;
	wl_list_for_each(window, &output->server->windows, link) {
		if (!window->mapped || window->workspace != output->active)
			continue;
		if (index >= start && index < start + count) {
			int x, y, width, height;
			if (vertical) {
				int each = (box.height - gap * (count - 1)) / count;
				x = box.x;
				width = box.width;
				y = box.y + placed * (each + gap);
				height = placed == count - 1 ? box.y + box.height - y : each;
			} else {
				int each = (box.width - gap * (count - 1)) / count;
				y = box.y;
				height = box.height;
				x = box.x + placed * (each + gap);
				width = placed == count - 1 ? box.x + box.width - x : each;
			}
			place_window(window, x, y, width, height);
			placed++;
		}
		index++;
	}
}

/* --------------------------------------------------------------- master-stack */

static void arrange_output (struct w3ld_output *output) {
	if (!output->active)
		return;
	struct w3ld_config *config = &output->server->config;
	int window_count = count_tiled(output->active);
	if (window_count == 0)
		return;

	bool smart = config->smart_gaps && window_count == 1;
	int gap_out = smart ? 0 : config->gaps_out;
	int gap_in = smart ? 0 : config->gaps_in;

	struct wlr_box area = output->usable;
	area.x += gap_out;
	area.y += gap_out;
	area.width -= 2 * gap_out;
	area.height -= 2 * gap_out;

	int masters = config->master_nmaster < 1 ? 1 : config->master_nmaster;
	if (masters > window_count)
		masters = window_count;
	int stacked = window_count - masters;

	bool split_horizontal = config->master_orientation == W3LD_ORIENT_LEFT
		|| config->master_orientation == W3LD_ORIENT_RIGHT;
	bool master_first = config->master_orientation == W3LD_ORIENT_LEFT
		|| config->master_orientation == W3LD_ORIENT_TOP;

	struct wlr_box master_box = area;
	struct wlr_box stack_box = area;
	if (stacked > 0) {
		if (split_horizontal) {
			int available = area.width - gap_in;
			int master_width = (int)(available * config->master_mfact);
			int stack_width = available - master_width;
			if (master_first) {
				master_box.width = master_width;
				stack_box.x = area.x + master_width + gap_in;
				stack_box.width = stack_width;
			} else {
				stack_box.width = stack_width;
				master_box.x = area.x + stack_width + gap_in;
				master_box.width = master_width;
			}
		} else {
			int available = area.height - gap_in;
			int master_height = (int)(available * config->master_mfact);
			int stack_height = available - master_height;
			if (master_first) {
				master_box.height = master_height;
				stack_box.y = area.y + master_height + gap_in;
				stack_box.height = stack_height;
			} else {
				stack_box.height = stack_height;
				master_box.y = area.y + stack_height + gap_in;
				master_box.height = master_height;
			}
		}
	}

	place_region(output, 0, masters, master_box, split_horizontal, gap_in);
	if (stacked > 0)
		place_region(output, masters, stacked, stack_box, split_horizontal,
				gap_in);
}

void w3ld_arrange (struct w3ld_server *server) {
	struct w3ld_output *output;
	wl_list_for_each(output, &server->outputs, link)
		arrange_output(output);

	struct w3ld_window *window;
	wl_list_for_each(window, &server->windows, link) {
		bool visible = window->mapped && window->workspace
			&& window->workspace->output->active == window->workspace;
		wlr_scene_node_set_enabled(&window->scene_tree->node, visible);
	}
}
