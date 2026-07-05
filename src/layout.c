/* Tiling layouts.
 *
 * M1 ships a single stateless master-stack layout over one output. A layout is
 * a pure function of the mapped-window list order and its parameters; it sets
 * each window's scene-node position and toplevel size. Gaps, smart-gaps, the
 * layout registry, and per-workspace parameters arrive in later milestones.
 */
#include "w3ld.h"

/* M1 fixed parameters; replaced by config resolution in M3. */
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
	DBG("tile %s -> %d,%d %dx%d",
			window->xdg_toplevel->app_id ? window->xdg_toplevel->app_id : "?",
			x, y, width, height);
}

/* --------------------------------------------------------------- master-stack */

void w3ld_arrange (struct w3ld_server *server) {
	if (wl_list_empty(&server->outputs))
		return;

	struct w3ld_output *output =
		wl_container_of(server->outputs.next, output, link);
	struct wlr_box area;
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &area);

	int window_count = 0;
	struct w3ld_window *window;
	wl_list_for_each(window, &server->windows, link) {
		if (window->mapped)
			window_count++;
	}
	if (window_count == 0)
		return;

	int masters = master_count < window_count ? master_count : window_count;
	int stacked = window_count - masters;
	int master_width = stacked > 0 ? (int)(area.width * master_fraction)
		: area.width;

	int index = 0;
	wl_list_for_each(window, &server->windows, link) {
		if (!window->mapped)
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
		index++;
	}
}
