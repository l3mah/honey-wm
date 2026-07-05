/* Tiling layouts.
 *
 * M2 arranges every output's active workspace with a stateless master-stack
 * layout, then sets each window's scene-node visibility (shown only when its
 * workspace is the active one on its output). Gaps, smart-gaps, the layout
 * registry, and per-workspace parameters arrive in later milestones.
 */
#include "w3ld.h"

/* M1/M2 fixed parameters; replaced by config resolution in M3. */
static const double master_fraction = 0.5;
static const int master_count = 1;

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

/* --------------------------------------------------------------- master-stack */

static void arrange_output (struct w3ld_output *output) {
	int window_count = count_tiled(output->active);
	if (window_count == 0)
		return;

	struct wlr_box area = output->usable;
	int masters = master_count < window_count ? master_count : window_count;
	int stacked = window_count - masters;
	int master_width = stacked > 0 ? (int)(area.width * master_fraction)
		: area.width;

	int index = 0;
	struct w3ld_window *window;
	wl_list_for_each(window, &output->server->windows, link) {
		if (!window->mapped || window->workspace != output->active)
			continue;

		if (index < masters) {
			int height = area.height / masters;
			place_window(window, area.x, area.y + index * height,
					master_width, height);
		} else {
			int stack_index = index - masters;
			int height = area.height / stacked;
			place_window(window, area.x + master_width,
					area.y + stack_index * height,
					area.width - master_width, height);
		}
		DBG("tile %s ws %s:%d -> %d,%d %dx%d",
				window->xdg_toplevel->app_id ?
					window->xdg_toplevel->app_id : "?",
				output->wlr_output->name, output->active->number,
				window->geom.x, window->geom.y,
				window->geom.width, window->geom.height);
		index++;
	}
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
