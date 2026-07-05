/* Actions: focus, workspace switching, and moving windows across workspaces and
 * outputs. Invoked by keybinds now and by the IPC layer in M3.
 *
 * Actions operate on the focused output and focused window. They mutate window
 * workspace assignments and the active workspace, then re-arrange and re-focus.
 */
#include <stdlib.h>

#include "w3ld.h"

/* ------------------------------------------------------------------- helpers */

static void switch_workspace (
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
		wlr_xdg_toplevel_send_close(server->focused->xdg_toplevel);
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
		switch_workspace(server->focused_output, number);
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
		switch_workspace(server->focused_output,
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
	switch_workspace(output, target->number);
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
