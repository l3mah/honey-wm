/* Tiling layouts.
 *
 * A stateless master-stack layout over each output's active workspace, driven by
 * the config (master-mfact/nmaster/orientation, gaps, smart-gaps, border-size). A
 * layout is a pure function of the window-list order and the config. Each window
 * is a border tree framing an inset surface. Windows on inactive workspaces are
 * hidden.
 */
#include "w3ld.h"

/* ------------------------------------------------------------------- helpers */

/* Frame the tile (width x height) with a border of the given width. */
static void set_border_rects (
	struct w3ld_window *window,
	int width,
	int height,
	int border
) {
	wlr_scene_rect_set_size(window->border[0], width, border);        /* top */
	wlr_scene_node_set_position(&window->border[0]->node, 0, 0);
	wlr_scene_rect_set_size(window->border[1], width, border);        /* bottom */
	wlr_scene_node_set_position(&window->border[1]->node, 0, height - border);
	wlr_scene_rect_set_size(window->border[2], border, height - 2 * border); /* left */
	wlr_scene_node_set_position(&window->border[2]->node, 0, border);
	wlr_scene_rect_set_size(window->border[3], border, height - 2 * border); /* right */
	wlr_scene_node_set_position(&window->border[3]->node, width - border, border);
}

static void place_window (
	struct w3ld_window *window,
	int x,
	int y,
	int width,
	int height,
	int border
) {
	window->geom = (struct wlr_box){ .x = x, .y = y, .width = width,
		.height = height };
	wlr_scene_node_set_position(&window->tree->node, x, y);
	set_border_rects(window, width, height, border);
	w3ld_window_configure(window, x + border, y + border,
			width - 2 * border, height - 2 * border);
	DBG("tile %s -> %d,%d %dx%d bw %d", w3ld_window_app_id(window),
			x, y, width, height, border);
}

static bool tiled_on (
	struct w3ld_window *window,
	struct w3ld_workspace *workspace
) {
	return window->mapped && window->workspace == workspace
		&& w3ld_window_is_tiled(window);
}

static int count_tiled (struct w3ld_workspace *workspace) {
	int count = 0;
	struct w3ld_window *window;
	wl_list_for_each(window, &workspace->output->server->windows, link) {
		if (tiled_on(window, workspace))
			count++;
	}
	return count;
}

/* Place a visible non-tiled window according to its state. */
static void place_untiled (
	struct w3ld_window *window,
	struct w3ld_output *output
) {
	struct w3ld_config *config = &output->server->config;

	if (window->fullscreen || window->fake_fullscreen) {
		struct wlr_box full;
		wlr_output_layout_get_box(output->server->output_layout,
				output->wlr_output, &full);
		place_window(window, full.x, full.y, full.width, full.height, 0);
	} else if (window->maximized) {
		int border = config->smart_gaps ? 0 : config->border_size;
		place_window(window, output->usable.x, output->usable.y,
				output->usable.width, output->usable.height, border);
	} else if (window->floating) {
		place_window(window, window->float_geom.x, window->float_geom.y,
				window->float_geom.width, window->float_geom.height,
				config->border_size);
	}
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
	int gap,
	int border
) {
	int index = 0;
	int placed = 0;
	struct w3ld_window *window;
	wl_list_for_each(window, &output->server->windows, link) {
		if (!tiled_on(window, output->active))
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
			place_window(window, x, y, width, height, border);
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

	struct w3ld_window *state_window;
	wl_list_for_each(state_window, &output->server->windows, link) {
		if (state_window->mapped
				&& state_window->workspace == output->active
				&& !w3ld_window_is_tiled(state_window))
			place_untiled(state_window, output);
	}

	int window_count = count_tiled(output->active);
	if (window_count == 0)
		return;

	bool smart = config->smart_gaps && window_count == 1;
	int gap_out = smart ? 0 : config->gaps_out;
	int gap_in = smart ? 0 : config->gaps_in;
	int border = smart ? 0 : config->border_size;

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

	place_region(output, 0, masters, master_box, split_horizontal, gap_in,
			border);
	if (stacked > 0)
		place_region(output, masters, stacked, stack_box, split_horizontal,
				gap_in, border);
}

void w3ld_arrange (struct w3ld_server *server) {
	struct w3ld_output *output;
	wl_list_for_each(output, &server->outputs, link)
		arrange_output(output);

	struct w3ld_window *window;
	wl_list_for_each(window, &server->windows, link) {
		bool visible = window->mapped && window->workspace
			&& window->workspace->output->active == window->workspace;
		wlr_scene_node_set_enabled(&window->tree->node, visible);
		wlr_scene_node_set_enabled(&window->surface_tree->node, visible);
	}

	w3ld_ext_workspace_sync(server);
	w3ld_status_broadcast(server);
}
