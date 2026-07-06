/* Workspaces: per-output, unlimited, created on demand, kept sorted by number.
 *
 * Workspaces are pure bookkeeping — a window carries a workspace pointer and is
 * shown only when its workspace is the active one on its output. There are no
 * dwm-style global tags.
 */
#include <stdlib.h>
#include <string.h>

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

/* The nearest output in a direction from `from`, scored by primary-axis
 * distance plus a perpendicular-offset penalty, or NULL if there is none. */
struct w3ld_output *w3ld_output_in_direction (
	struct w3ld_output *from,
	enum w3ld_direction direction
) {
	int from_x = from->usable.x + from->usable.width / 2;
	int from_y = from->usable.y + from->usable.height / 2;

	struct w3ld_output *output;
	struct w3ld_output *best = NULL;
	long best_score = -1;
	wl_list_for_each(output, &from->server->outputs, link) {
		if (output == from)
			continue;
		int dx = (output->usable.x + output->usable.width / 2) - from_x;
		int dy = (output->usable.y + output->usable.height / 2) - from_y;

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
			best = output;
		}
	}
	return best;
}

/* The output with this connector name (e.g. "DP-1"), or NULL. */
struct w3ld_output *w3ld_output_by_name (
	struct w3ld_server *server,
	const char *name
) {
	struct w3ld_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (!strcmp(output->wlr_output->name, name))
			return output;
	}
	return NULL;
}

/* The output whose usable box contains the point (x, y), or NULL. */
struct w3ld_output *w3ld_output_at (
	struct w3ld_server *server,
	double x,
	double y
) {
	struct w3ld_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_box *box = &output->usable;
		if (x >= box->x && x < box->x + box->width
				&& y >= box->y && y < box->y + box->height)
			return output;
	}
	return NULL;
}
