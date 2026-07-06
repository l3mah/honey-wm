/* Actions: focus, workspace switching, and moving windows across workspaces and
 * outputs. Invoked by keybindings and by IPC commands alike.
 *
 * Actions operate on the focused output and focused window. They mutate window
 * workspace assignments and the active workspace, then re-arrange and re-focus.
 */
#include <stdlib.h>

#include <wlr/xwayland/xwayland.h>

#include "w3ld.h"

/* ------------------------------------------------------------------- helpers */

void w3ld_switch_workspace (
	struct w3ld_output *output,
	int number
) {
	struct w3ld_workspace *target = w3ld_workspace_get(output, number);
	if (target == output->active)
		return;
	output->previous_number = output->active->number;
	output->active = target;

	struct w3ld_server *server = output->server;
	server->focused_output = output;
	w3ld_arrange(server);
	w3ld_focus_output_active(output);
	w3ld_warp_to_focus(server);
}

/* --------------------------------------------------------------------- focus */

void w3ld_action_close (struct w3ld_server *server) {
	if (server->focused)
		w3ld_window_close(server->focused);
}

void w3ld_action_focus (
	struct w3ld_server *server,
	int direction
) {
	if (!server->focused)
		return;
	struct wl_list *link = &server->focused->link;
	link = direction > 0 ? link->next : link->prev;
	if (link == &server->windows)
		link = direction > 0 ? link->next : link->prev;

	struct w3ld_window *start = server->focused;
	while (link != &start->link) {
		if (link == &server->windows) {
			link = direction > 0 ? link->next : link->prev;
			continue;
		}
		struct w3ld_window *window = wl_container_of(link, window, link);
		if (window->mapped && window->workspace == start->workspace) {
			w3ld_focus_window(window);
			w3ld_warp_to_focus(server);
			return;
		}
		link = direction > 0 ? link->next : link->prev;
	}
}

/* ---------------------------------------------------------------- workspaces */

void w3ld_action_workspace (
	struct w3ld_server *server,
	int number
) {
	if (server->focused_output)
		w3ld_switch_workspace(server->focused_output, number);
}

void w3ld_action_move_to_workspace (
	struct w3ld_server *server,
	int number
) {
	if (!server->focused)
		return;
	struct w3ld_window *window = server->focused;
	struct w3ld_output *output = window->workspace->output;
	window->workspace = w3ld_workspace_get(output, number);

	w3ld_arrange(server);
	w3ld_focus_output_active(output);
	w3ld_warp_to_focus(server);
}

void w3ld_action_workspace_back (struct w3ld_server *server) {
	if (server->focused_output)
		w3ld_switch_workspace(server->focused_output,
				server->focused_output->previous_number);
}

/* Cycle to the next/previous *existing* workspace on the focused output. */
void w3ld_action_workspace_cycle (
	struct w3ld_server *server,
	int direction
) {
	struct w3ld_output *output = server->focused_output;
	if (!output)
		return;
	struct wl_list *link = &output->active->link;
	link = direction > 0 ? link->next : link->prev;
	if (link == &output->workspaces)
		link = direction > 0 ? link->next : link->prev;
	if (link == &output->active->link)
		return;
	struct w3ld_workspace *target = wl_container_of(link, target, link);
	w3ld_switch_workspace(output, target->number);
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
static struct w3ld_window *tiled_neighbor (
	struct w3ld_window *from,
	int direction
) {
	struct wl_list *link = &from->link;
	struct w3ld_server *server = from->server;
	for (;;) {
		link = direction > 0 ? link->next : link->prev;
		if (link == &from->link)
			return NULL;
		if (link == &server->windows)
			continue; /* the sentinel; the next step wraps past it */
		struct w3ld_window *window = wl_container_of(link, window, link);
		if (window->mapped && window->workspace == from->workspace
				&& w3ld_window_is_tiled(window))
			return window;
	}
}

void w3ld_action_swap (
	struct w3ld_server *server,
	int direction
) {
	struct w3ld_window *window = server->focused;
	if (!window || !w3ld_window_is_tiled(window))
		return;
	struct w3ld_window *other = tiled_neighbor(window, direction);
	if (!other)
		return;
	list_swap(&window->link, &other->link);
	w3ld_arrange(server);
	w3ld_warp_to_focus(server);
}

void w3ld_action_swap_master (struct w3ld_server *server) {
	struct w3ld_window *window = server->focused;
	if (!window || !w3ld_window_is_tiled(window))
		return;
	struct w3ld_window *master = w3ld_workspace_first_window(window->workspace);
	while (master && !w3ld_window_is_tiled(master))
		master = tiled_neighbor(master, +1);
	if (!master)
		return;
	if (master == window) /* focused is the master: swap with the next */
		master = tiled_neighbor(window, +1);
	if (!master || master == window)
		return;
	list_swap(&window->link, &master->link);
	w3ld_arrange(server);
	w3ld_warp_to_focus(server);
}

/* ---------------------------------------------------------- live layout tweaks */

/* Live tweaks write the ACTIVE workspace's override (each workspace remembers
 * its own values); `set master-*` remains the global default. */

void w3ld_action_mfact (
	struct w3ld_server *server,
	double delta
) {
	if (!server->focused_output)
		return;
	struct w3ld_workspace *workspace = server->focused_output->active;
	double value = (workspace->has_mfact ? workspace->mfact
			: server->config.master_mfact) + delta;
	workspace->mfact = value < 0.05 ? 0.05 : value > 0.95 ? 0.95 : value;
	workspace->has_mfact = true;
	w3ld_arrange(server);
}

void w3ld_action_nmaster (
	struct w3ld_server *server,
	int delta
) {
	if (!server->focused_output)
		return;
	struct w3ld_workspace *workspace = server->focused_output->active;
	int value = (workspace->has_nmaster ? workspace->nmaster
			: server->config.master_nmaster) + delta;
	workspace->nmaster = value < 1 ? 1 : value;
	workspace->has_nmaster = true;
	w3ld_arrange(server);
}

void w3ld_action_orientation_cycle (struct w3ld_server *server) {
	if (!server->focused_output)
		return;
	struct w3ld_workspace *workspace = server->focused_output->active;
	enum w3ld_orientation current = workspace->has_orientation
		? workspace->orientation : server->config.master_orientation;
	workspace->orientation = (current + 1) % 4;
	workspace->has_orientation = true;
	w3ld_arrange(server);
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
void w3ld_float_seed (struct w3ld_window *window) {
	struct w3ld_server *server = window->server;
	struct wlr_box *usable = &window->workspace->output->usable;

	int width, height;
	if (server->config.float_app_size && window->type == W3LD_WINDOW_X11) {
		width = window->xwayland_surface->width;
		height = window->xwayland_surface->height;
	} else {
		width = float_size(server->config.float_width, usable->width);
		height = float_size(server->config.float_height, usable->height);
		if (server->config.float_app_size) {
			/* xdg: size 0 lets the app choose; adopted on its next commit. */
			window->float_pending_app_size = true;
		}
	}
	w3ld_window_set_float_geom(window, width, height);
}

void w3ld_action_toggle_float (struct w3ld_server *server) {
	struct w3ld_window *window = server->focused;
	if (!window)
		return;
	bool enable = !window->floating;
	w3ld_window_clear_states(window);
	window->floating = enable;
	if (enable)
		w3ld_float_seed(window);
	w3ld_window_apply_state(window);
}

void w3ld_action_fullscreen (struct w3ld_server *server) {
	struct w3ld_window *window = server->focused;
	if (!window)
		return;
	bool enable = !window->fullscreen;
	w3ld_window_clear_states(window);
	window->fullscreen = enable;
	w3ld_window_apply_state(window);
}

void w3ld_action_maximize (struct w3ld_server *server) {
	struct w3ld_window *window = server->focused;
	if (!window)
		return;
	bool enable = !window->maximized;
	w3ld_window_clear_states(window);
	window->maximized = enable;
	w3ld_window_apply_state(window);
}

void w3ld_action_fake_fullscreen (struct w3ld_server *server) {
	struct w3ld_window *window = server->focused;
	if (!window)
		return;
	bool enable = !window->fake_fullscreen;
	w3ld_window_clear_states(window);
	window->fake_fullscreen = enable;
	w3ld_window_apply_state(window);
}

/* ---------------------------------------------------------------- directional */

/* Focus the nearest visible window in a direction (crossing monitors); if there
 * is none, step focus to the adjacent output that way (which may be empty). */
void w3ld_action_focus_dir (
	struct w3ld_server *server,
	enum w3ld_direction direction
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

	struct w3ld_window *best = NULL;
	long best_score = -1;
	struct w3ld_window *window;
	wl_list_for_each(window, &server->windows, link) {
		if (!window->mapped || window == server->focused)
			continue;
		if (window->workspace != window->workspace->output->active)
			continue;
		int dx = (window->geom.x + window->geom.width / 2) - from_x;
		int dy = (window->geom.y + window->geom.height / 2) - from_y;

		long primary, secondary;
		switch (direction) {
		case W3LD_DIR_LEFT:  if (dx >= 0) continue; primary = -dx; secondary = labs(dy); break;
		case W3LD_DIR_RIGHT: if (dx <= 0) continue; primary =  dx; secondary = labs(dy); break;
		case W3LD_DIR_UP:    if (dy >= 0) continue; primary = -dy; secondary = labs(dx); break;
		default:             if (dy <= 0) continue; primary =  dy; secondary = labs(dx); break;
		}
		long score = primary + secondary * 2;
		if (best_score < 0 || score < best_score) {
			best_score = score;
			best = window;
		}
	}

	if (best) {
		w3ld_focus_window(best);
	} else {
		struct w3ld_output *output =
			w3ld_output_in_direction(server->focused_output, direction);
		if (output)
			w3ld_focus_output_active(output);
	}
	w3ld_warp_to_focus(server);
}

void w3ld_action_move_to_output (
	struct w3ld_server *server,
	enum w3ld_direction direction
) {
	if (!server->focused)
		return;
	struct w3ld_window *window = server->focused;
	struct w3ld_output *target =
		w3ld_output_in_direction(window->workspace->output, direction);
	if (!target)
		return;

	window->workspace = target->active;
	server->focused_output = target;
	w3ld_arrange(server);
	w3ld_focus_window(window);
	w3ld_warp_to_focus(server);
}
