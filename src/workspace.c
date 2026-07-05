/* Workspaces: per-output, unlimited, created on demand, kept sorted by number.
 *
 * Workspaces are pure bookkeeping — a window carries a workspace pointer and is
 * shown only when its workspace is the active one on its output. There are no
 * dwm-style global tags.
 */
#include <stdlib.h>

#include "w3ld.h"

/* Find workspace `number` on `output`, creating it (in sorted position) if it
 * does not exist yet. */
struct w3ld_workspace *w3ld_workspace_get (
	struct w3ld_output *output,
	int number
) {
	struct w3ld_workspace *workspace;
	wl_list_for_each(workspace, &output->workspaces, link) {
		if (workspace->number == number)
			return workspace;
	}

	struct w3ld_workspace *created = calloc(1, sizeof *created);
	created->output = output;
	created->number = number;

	struct w3ld_workspace *after;
	wl_list_for_each(after, &output->workspaces, link) {
		if (after->number > number)
			break;
	}
	wl_list_insert(after->link.prev, &created->link);
	return created;
}

/* First mapped window on a workspace in tiling order, or NULL. */
struct w3ld_window *w3ld_workspace_first_window (struct w3ld_workspace *workspace) {
	struct w3ld_window *window;
	wl_list_for_each(window, &workspace->output->server->windows, link) {
		if (window->mapped && window->workspace == workspace)
			return window;
	}
	return NULL;
}

/* The output immediately left (direction < 0) or right (direction > 0) of
 * `from`, ordered by horizontal position, or NULL if there is none. */
struct w3ld_output *w3ld_output_adjacent (
	struct w3ld_output *from,
	int direction
) {
	struct w3ld_output *output;
	struct w3ld_output *best = NULL;
	wl_list_for_each(output, &from->server->outputs, link) {
		if (output == from)
			continue;
		if (direction > 0 && output->usable.x > from->usable.x) {
			if (!best || output->usable.x < best->usable.x)
				best = output;
		} else if (direction < 0 && output->usable.x < from->usable.x) {
			if (!best || output->usable.x > best->usable.x)
				best = output;
		}
	}
	return best;
}
