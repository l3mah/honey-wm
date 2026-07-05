/* Actions: focus, workspace switching, and moving windows across workspaces and
 * outputs. Invoked by keybinds now and by the IPC layer in M3.
 *
 * Actions operate on the focused output and focused window. They mutate window
 * workspace assignments and the active workspace, then re-arrange and re-focus.
 */
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

/* ------------------------------------------------------------------- outputs */

void w3ld_action_focus_output (
	struct w3ld_server *server,
	int direction
) {
	if (!server->focused_output)
		return;
	struct w3ld_output *target =
		w3ld_output_adjacent(server->focused_output, direction);
	if (!target)
		return;
	server->focused_output = target;
	w3ld_focus_output_active(target);
}

void w3ld_action_move_to_output (
	struct w3ld_server *server,
	int direction
) {
	if (!server->focused)
		return;
	struct w3ld_window *window = server->focused;
	struct w3ld_output *source = window->workspace->output;
	struct w3ld_output *target = w3ld_output_adjacent(source, direction);
	if (!target)
		return;

	window->workspace = target->active;
	server->focused_output = target;
	w3ld_arrange(server);
	w3ld_focus_window(window);
}
