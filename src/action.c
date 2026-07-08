/* Actions: focus, workspace switching, and moving windows across workspaces and
 * outputs. Invoked by keybindings and by IPC commands alike.
 *
 * Actions operate on the focused output and focused window. They mutate window
 * workspace assignments and the active workspace, then re-arrange and re-focus.
 */
#include <stdlib.h>

#include <wlr/xwayland/xwayland.h>

#include "honey.h"

/* ------------------------------------------------------------------- helpers */

void honey_switch_workspace (
	struct honey_output *output,
	int number
) {
	struct honey_workspace *target = honey_workspace_get(output, number);
	if (target == output->active)
		return;
	output->previous_number = output->active->number;
	output->active = target;

	struct honey_server *server = output->server;
	server->focused_output = output;
	honey_arrange(server);
	honey_focus_output_active(output);
	/* Switching workspace (keyboard or a bar click) shouldn't yank the cursor
	 * unless explicitly opted in — a click on the workspace module would
	 * otherwise teleport the pointer away from the bar. */
	if (server->config.warp_on_workspace_switch)
		honey_warp_to_focus(server);
}

/* --------------------------------------------------------------------- focus */

void honey_action_close (struct honey_server *server) {
	if (server->focused)
		honey_window_close(server->focused);
}

void honey_action_focus (
	struct honey_server *server,
	int direction
) {
	if (!server->focused)
		return;
	struct wl_list *link = &server->focused->link;
	link = direction > 0 ? link->next : link->prev;
	if (link == &server->windows)
		link = direction > 0 ? link->next : link->prev;

	struct honey_window *start = server->focused;
	while (link != &start->link) {
		if (link == &server->windows) {
			link = direction > 0 ? link->next : link->prev;
			continue;
		}
		struct honey_window *window = wl_container_of(link, window, link);
		if (window->mapped && window->workspace == start->workspace) {
			honey_focus_window(window);
			honey_warp_to_focus(server);
			return;
		}
		link = direction > 0 ? link->next : link->prev;
	}
}

/* ---------------------------------------------------------------- workspaces */

void honey_action_workspace (
	struct honey_server *server,
	int number
) {
	if (server->focused_output)
		honey_switch_workspace(server->focused_output, number);
}

void honey_action_move_to_workspace (
	struct honey_server *server,
	int number
) {
	if (!server->focused)
		return;
	struct honey_window *window = server->focused;
	struct honey_output *output = window->workspace->output;
	window->workspace = honey_workspace_get(output, number);

	honey_arrange(server);
	honey_focus_output_active(output);
	/* Sending a window away shifts focus to whatever remains here; don't let
	 * that yank the cursor unless workspace-switch warping is opted in. */
	if (server->config.warp_on_workspace_switch)
		honey_warp_to_focus(server);
}

void honey_action_workspace_back (struct honey_server *server) {
	if (server->focused_output)
		honey_switch_workspace(server->focused_output,
				server->focused_output->previous_number);
}

/* Cycle to the next/previous *existing* workspace on the focused output. */
void honey_action_workspace_cycle (
	struct honey_server *server,
	int direction
) {
	struct honey_output *output = server->focused_output;
	if (!output)
		return;
	struct wl_list *link = &output->active->link;
	link = direction > 0 ? link->next : link->prev;
	if (link == &output->workspaces)
		link = direction > 0 ? link->next : link->prev;
	if (link == &output->active->link)
		return;
	struct honey_workspace *target = wl_container_of(link, target, link);
	honey_switch_workspace(output, target->number);
}

/* ---------------------------------------------------------------- stack order */

/* Swap two list nodes, safe for adjacent nodes in either order. */
static void list_swap (
	struct wl_list *a,
	struct wl_list *b
) {
	if (a == b)
		return;
	if (a->prev == b) { /* b directly before a: move a in front of b */
		wl_list_remove(a);
		wl_list_insert(b->prev, a);
		return;
	}
	if (b->prev == a) { /* a directly before b */
		wl_list_remove(b);
		wl_list_insert(a->prev, b);
		return;
	}
	struct wl_list *a_prev = a->prev;
	struct wl_list *b_prev = b->prev;
	wl_list_remove(a);
	wl_list_insert(b_prev, a);
	wl_list_remove(b);
	wl_list_insert(a_prev, b);
}

/* The next/previous tiled window on the same workspace, wrapping, or NULL. */
static struct honey_window *tiled_neighbor (
	struct honey_window *from,
	int direction
) {
	struct wl_list *link = &from->link;
	struct honey_server *server = from->server;
	for (;;) {
		link = direction > 0 ? link->next : link->prev;
		if (link == &from->link)
			return NULL;
		if (link == &server->windows)
			continue; /* the sentinel; the next step wraps past it */
		struct honey_window *window = wl_container_of(link, window, link);
		if (window->mapped && window->workspace == from->workspace
				&& honey_window_is_tiled(window))
			return window;
	}
}

void honey_action_swap (
	struct honey_server *server,
	int direction
) {
	struct honey_window *window = server->focused;
	if (!window || !honey_window_is_tiled(window))
		return;
	struct honey_window *other = tiled_neighbor(window, direction);
	if (!other)
		return;
	list_swap(&window->link, &other->link);
	honey_arrange(server);
	honey_warp_to_focus(server);
}

void honey_action_swap_master (struct honey_server *server) {
	struct honey_window *window = server->focused;
	if (!window || !honey_window_is_tiled(window))
		return;
	struct honey_window *master = honey_workspace_first_window(window->workspace);
	while (master && !honey_window_is_tiled(master))
		master = tiled_neighbor(master, +1);
	if (!master)
		return;
	if (master == window) /* focused is the master: swap with the next */
		master = tiled_neighbor(window, +1);
	if (!master || master == window)
		return;
	list_swap(&window->link, &master->link);
	honey_arrange(server);
	honey_warp_to_focus(server);
}

/* ---------------------------------------------------------- live layout tweaks */

/* Live tweaks write the ACTIVE workspace's override (each workspace remembers
 * its own values); `set master-*` remains the global default. */

void honey_action_mfact (
	struct honey_server *server,
	double delta
) {
	if (!server->focused_output)
		return;
	struct honey_workspace *workspace = server->focused_output->active;
	double value = (workspace->has_mfact ? workspace->mfact
			: server->config.master_mfact) + delta;
	workspace->mfact = value < 0.05 ? 0.05 : value > 0.95 ? 0.95 : value;
	workspace->has_mfact = true;
	honey_arrange(server);
}

void honey_action_nmaster (
	struct honey_server *server,
	int delta
) {
	if (!server->focused_output)
		return;
	struct honey_workspace *workspace = server->focused_output->active;
	int value = (workspace->has_nmaster ? workspace->nmaster
			: server->config.master_nmaster) + delta;
	workspace->nmaster = value < 1 ? 1 : value;
	workspace->has_nmaster = true;
	honey_arrange(server);
}

void honey_action_orientation_cycle (struct honey_server *server) {
	if (!server->focused_output)
		return;
	struct honey_workspace *workspace = server->focused_output->active;
	enum honey_orientation current = workspace->has_orientation
		? workspace->orientation : server->config.master_orientation;
	workspace->orientation = (current + 1) % 4;
	workspace->has_orientation = true;
	honey_arrange(server);
}

/* ------------------------------------------------------------- window states */

/* Convert a float-size config value to pixels: a fraction of the usable axis,
 * with values above 1 read as a percent (lakewm compatibility). */
static int float_size (
	double value,
	int axis
) {
	if (value > 1.0)
		value /= 100.0;
	return (int)(axis * value);
}

/* Seed the floating geometry: the configured size (or the app's own choice),
 * centred on the window's output. */
void honey_float_seed (struct honey_window *window) {
	struct honey_server *server = window->server;
	struct wlr_box *usable = &window->workspace->output->usable;

	int width, height;
	if (server->config.float_app_size && window->type == HONEY_WINDOW_X11) {
		/* xwayland-scale (removable): X11 reports its size in the scaled
		 * xwayland coordinate space. */
		double scale = honey_output_xwayland_scale(window->workspace->output);
		width = (int)(window->xwayland_surface->width / scale);
		height = (int)(window->xwayland_surface->height / scale);
	} else {
		width = float_size(server->config.float_width, usable->width);
		height = float_size(server->config.float_height, usable->height);
		if (server->config.float_app_size) {
			/* xdg: size 0 lets the app choose; adopted on its next commit. */
			window->float_pending_app_size = true;
		}
	}
	honey_window_set_float_geom(window, width, height);
}

void honey_action_toggle_float (struct honey_server *server) {
	struct honey_window *window = server->focused;
	if (!window)
		return;
	bool enable = !window->floating;
	honey_window_clear_states(window);
	window->floating = enable;
	if (enable)
		honey_float_seed(window);
	honey_window_apply_state(window);
}

void honey_action_fullscreen (struct honey_server *server) {
	struct honey_window *window = server->focused;
	if (!window)
		return;
	bool enable = !window->fullscreen;
	honey_window_clear_states(window);
	window->fullscreen = enable;
	honey_window_apply_state(window);
}

void honey_action_maximize (struct honey_server *server) {
	struct honey_window *window = server->focused;
	if (!window)
		return;
	bool enable = !window->maximized;
	honey_window_clear_states(window);
	window->maximized = enable;
	honey_window_apply_state(window);
}

void honey_action_fake_fullscreen (struct honey_server *server) {
	struct honey_window *window = server->focused;
	if (!window)
		return;
	bool enable = !window->fake_fullscreen;
	honey_window_clear_states(window);
	window->fake_fullscreen = enable;
	honey_window_apply_state(window);
}

/* ---------------------------------------------------------------- directional */

/* The nearest window in a direction from the focused window's centre, scored
 * by primary-axis distance plus a perpendicular penalty. workspace limits the
 * search to tiled windows of that workspace; NULL searches every visible
 * window (crossing monitors). */
static struct honey_window *window_in_direction (
	struct honey_server *server,
	int from_x,
	int from_y,
	enum honey_direction direction,
	struct honey_workspace *workspace
) {
	struct honey_window *best = NULL;
	long best_score = -1;
	struct honey_window *window;
	wl_list_for_each(window, &server->windows, link) {
		if (!window->mapped || window == server->focused)
			continue;
		if (workspace) {
			if (window->workspace != workspace
					|| !honey_window_is_tiled(window))
				continue;
		} else if (window->workspace != window->workspace->output->active) {
			continue;
		}
		int dx = (window->geom.x + window->geom.width / 2) - from_x;
		int dy = (window->geom.y + window->geom.height / 2) - from_y;

		long primary, secondary;
		switch (direction) {
		case HONEY_DIR_LEFT:  if (dx >= 0) continue; primary = -dx; secondary = labs(dy); break;
		case HONEY_DIR_RIGHT: if (dx <= 0) continue; primary =  dx; secondary = labs(dy); break;
		case HONEY_DIR_UP:    if (dy >= 0) continue; primary = -dy; secondary = labs(dx); break;
		default:             if (dy <= 0) continue; primary =  dy; secondary = labs(dx); break;
		}
		long score = primary + secondary * 2;
		if (best_score < 0 || score < best_score) {
			best_score = score;
			best = window;
		}
	}
	return best;
}

/* Focus the nearest visible window in a direction (crossing monitors); if there
 * is none, step focus to the adjacent output that way (which may be empty). */
void honey_action_focus_dir (
	struct honey_server *server,
	enum honey_direction direction
) {
	if (!server->focused_output)
		return;

	int from_x, from_y;
	if (server->focused) {
		from_x = server->focused->geom.x + server->focused->geom.width / 2;
		from_y = server->focused->geom.y + server->focused->geom.height / 2;
	} else {
		from_x = server->focused_output->usable.x
			+ server->focused_output->usable.width / 2;
		from_y = server->focused_output->usable.y
			+ server->focused_output->usable.height / 2;
	}

	struct honey_window *best =
		window_in_direction(server, from_x, from_y, direction, NULL);
	if (best) {
		honey_focus_window(best);
	} else {
		struct honey_output *output =
			honey_output_in_direction(server->focused_output, direction);
		if (output)
			honey_focus_output_active(output);
	}
	honey_warp_to_focus(server);
}

/* Swap the focused window with the nearest tiled window in a direction on the
 * same workspace — spatial reordering, natural in the grid layout. */
void honey_action_swap_dir (
	struct honey_server *server,
	enum honey_direction direction
) {
	struct honey_window *window = server->focused;
	if (!window || !honey_window_is_tiled(window))
		return;
	struct honey_window *other = window_in_direction(server,
			window->geom.x + window->geom.width / 2,
			window->geom.y + window->geom.height / 2,
			direction, window->workspace);
	if (!other)
		return;
	list_swap(&window->link, &other->link);
	honey_arrange(server);
	honey_warp_to_focus(server);
}

void honey_action_move_to_output (
	struct honey_server *server,
	enum honey_direction direction
) {
	if (!server->focused)
		return;
	struct honey_window *window = server->focused;
	struct honey_output *target =
		honey_output_in_direction(window->workspace->output, direction);
	if (!target)
		return;

	window->workspace = target->active;
	server->focused_output = target;
	honey_arrange(server);
	honey_focus_window(window);
	honey_warp_to_focus(server);
}
