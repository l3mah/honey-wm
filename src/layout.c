/* Tiling layouts.
 *
 * Stateless layouts behind a name-keyed registry: each arrange() is a pure
 * function of the tiled-window array and its parameters (a layout can always be
 * recomputed from the window-list order — no per-layout state). The driver
 * collects each output's tiled windows, resolves effective parameters
 * (workspace override or global), and delegates. Non-tiled windows (floating /
 * fullscreen / maximized / fake-fullscreen) are placed by their state. Windows
 * on inactive workspaces are hidden.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_foreign_toplevel_management_v1.h>

#include "honey.h"

#define MAX_TILED 64

/* ------------------------------------------------------------------- helpers */

/* Frame the tile (width x height) with a border of the given width. */
static void set_border_rects (
	struct honey_window *window,
	int width,
	int height,
	int border
) {
	wlr_scene_rect_set_size(window->border[0], width, border);        /* top */
	wlr_scene_node_set_position(&window->border[0]->node, 0, 0);
	wlr_scene_rect_set_size(window->border[1], width, border);        /* bottom */
	wlr_scene_node_set_position(&window->border[1]->node, 0, height - border);
	wlr_scene_rect_set_size(window->border[2], border, height - 2 * border);
	wlr_scene_node_set_position(&window->border[2]->node, 0, border); /* left */
	wlr_scene_rect_set_size(window->border[3], border, height - 2 * border);
	wlr_scene_node_set_position(&window->border[3]->node, width - border,
			border);                                                  /* right */
}

static void place_window (
	struct honey_window *window,
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
	honey_window_configure(window, x + border, y + border,
			width - 2 * border, height - 2 * border);
	DBG("tile %s -> %d,%d %dx%d bw %d", honey_window_app_id(window),
			x, y, width, height, border);
}

static void place_box (
	struct honey_layout_ctx *ctx,
	struct honey_window *window,
	struct wlr_box box
) {
	place_window(window, box.x, box.y, box.width, box.height, ctx->border);
}

/* Split `count` windows along one axis of `box` with `gap` between them; the
 * last window absorbs integer-division remainder. */
static void arrange_line (
	struct honey_layout_ctx *ctx,
	struct honey_window **windows,
	int count,
	struct wlr_box box,
	bool vertical
) {
	for (int i = 0; i < count; i++) {
		struct wlr_box cell = box;
		if (vertical) {
			int each = (box.height - ctx->gap * (count - 1)) / count;
			cell.y = box.y + i * (each + ctx->gap);
			cell.height = i == count - 1
				? box.y + box.height - cell.y : each;
		} else {
			int each = (box.width - ctx->gap * (count - 1)) / count;
			cell.x = box.x + i * (each + ctx->gap);
			cell.width = i == count - 1
				? box.x + box.width - cell.x : each;
		}
		place_box(ctx, windows[i], cell);
	}
}

/* ------------------------------------------------------------------- layouts */

static void master_arrange (struct honey_layout_ctx *ctx) {
	int masters = ctx->nmaster < 1 ? 1 : ctx->nmaster;
	if (masters > ctx->count)
		masters = ctx->count;
	int stacked = ctx->count - masters;

	bool split_horizontal = ctx->orientation == HONEY_ORIENT_LEFT
		|| ctx->orientation == HONEY_ORIENT_RIGHT;
	bool master_first = ctx->orientation == HONEY_ORIENT_LEFT
		|| ctx->orientation == HONEY_ORIENT_TOP;

	struct wlr_box master_box = ctx->area;
	struct wlr_box stack_box = ctx->area;
	if (stacked > 0) {
		if (split_horizontal) {
			int available = ctx->area.width - ctx->gap;
			int master_width = (int)(available * ctx->mfact);
			if (master_first) {
				master_box.width = master_width;
				stack_box.x = ctx->area.x + master_width + ctx->gap;
				stack_box.width = available - master_width;
			} else {
				stack_box.width = available - master_width;
				master_box.x = ctx->area.x + stack_box.width + ctx->gap;
				master_box.width = master_width;
			}
		} else {
			int available = ctx->area.height - ctx->gap;
			int master_height = (int)(available * ctx->mfact);
			if (master_first) {
				master_box.height = master_height;
				stack_box.y = ctx->area.y + master_height + ctx->gap;
				stack_box.height = available - master_height;
			} else {
				stack_box.height = available - master_height;
				master_box.y = ctx->area.y + stack_box.height + ctx->gap;
				master_box.height = master_height;
			}
		}
	}

	arrange_line(ctx, ctx->windows, masters, master_box, split_horizontal);
	if (stacked > 0)
		arrange_line(ctx, ctx->windows + masters, stacked, stack_box,
				split_horizontal);
}

/* Fibonacci spiral: each window takes `spiral_ratio` of the remaining box, the
 * rest recurses with the split axis alternating. */
static void spiral_arrange (struct honey_layout_ctx *ctx) {
	struct wlr_box remaining = ctx->area;
	bool horizontal = ctx->spiral_horizontal;

	for (int i = 0; i < ctx->count; i++) {
		if (i == ctx->count - 1) {
			place_box(ctx, ctx->windows[i], remaining);
			break;
		}
		struct wlr_box cell = remaining;
		if (horizontal) {
			int take = (int)((remaining.width - ctx->gap) * ctx->spiral_ratio);
			cell.width = take;
			remaining.x += take + ctx->gap;
			remaining.width -= take + ctx->gap;
		} else {
			int take = (int)((remaining.height - ctx->gap) * ctx->spiral_ratio);
			cell.height = take;
			remaining.y += take + ctx->gap;
			remaining.height -= take + ctx->gap;
		}
		place_box(ctx, ctx->windows[i], cell);
		horizontal = !horizontal;
	}
}

/* Even rows x columns; a partial last row stretches to fill the width. */
static void grid_arrange (struct honey_layout_ctx *ctx) {
	int columns = ctx->grid_columns > 0 ? ctx->grid_columns
		: (int)ceil(sqrt(ctx->count));
	if (columns > ctx->count)
		columns = ctx->count;
	int rows = (ctx->count + columns - 1) / columns;

	int placed = 0;
	for (int row = 0; row < rows; row++) {
		int in_row = ctx->count - placed < columns
			? ctx->count - placed : columns;
		int each_height = (ctx->area.height - ctx->gap * (rows - 1)) / rows;
		struct wlr_box row_box = ctx->area;
		row_box.y = ctx->area.y + row * (each_height + ctx->gap);
		row_box.height = row == rows - 1
			? ctx->area.y + ctx->area.height - row_box.y : each_height;
		arrange_line(ctx, ctx->windows + placed, in_row, row_box, false);
		placed += in_row;
	}
}

/* ------------------------------------------------------------------ registry */

static const struct honey_layout layouts[] = {
	{ "master", master_arrange },
	{ "spiral", spiral_arrange },
	{ "grid", grid_arrange },
};

const struct honey_layout *honey_layout_by_name (const char *name) {
	for (size_t i = 0; i < sizeof layouts / sizeof layouts[0]; i++) {
		if (!strcmp(layouts[i].name, name))
			return &layouts[i];
	}
	return NULL;
}

/* -------------------------------------------------------------------- driver */

static bool tiled_on (
	struct honey_window *window,
	struct honey_workspace *workspace
) {
	return window->mapped && window->workspace == workspace
		&& honey_window_is_tiled(window);
}

/* Place a visible non-tiled window according to its state. */
static void place_untiled (
	struct honey_window *window,
	struct honey_output *output
) {
	struct honey_config *config = &output->server->config;

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

static void arrange_output (struct honey_output *output) {
	if (!output->active)
		return;
	struct honey_server *server = output->server;
	struct honey_config *config = &server->config;
	struct honey_workspace *workspace = output->active;

	struct honey_window *tiled[MAX_TILED];
	int count = 0;
	struct honey_window *window;
	wl_list_for_each(window, &server->windows, link) {
		if (window->mapped && window->workspace == workspace
				&& !honey_window_is_tiled(window))
			place_untiled(window, output);
		else if (tiled_on(window, workspace) && count < MAX_TILED)
			tiled[count++] = window;
	}
	if (count == 0)
		return;

	bool smart = config->smart_gaps && count == 1;
	int gap_out = smart ? 0 : config->gaps_out;

	struct honey_layout_ctx ctx = {
		.windows = tiled,
		.count = count,
		.area = {
			.x = output->usable.x + gap_out,
			.y = output->usable.y + gap_out,
			.width = output->usable.width - 2 * gap_out,
			.height = output->usable.height - 2 * gap_out,
		},
		.gap = smart ? 0 : config->gaps_in,
		.border = smart ? 0 : config->border_size,
		.mfact = workspace->has_mfact ? workspace->mfact
			: config->master_mfact,
		.nmaster = workspace->has_nmaster ? workspace->nmaster
			: config->master_nmaster,
		.orientation = workspace->has_orientation ? workspace->orientation
			: config->master_orientation,
		.spiral_ratio = config->spiral_ratio,
		.spiral_horizontal = config->spiral_horizontal,
		.grid_columns = config->grid_columns,
	};

	const struct honey_layout *layout =
		workspace->layout ? workspace->layout : config->layout;
	layout->arrange(&ctx);
}

void honey_arrange (struct honey_server *server) {
	struct honey_output *output;
	wl_list_for_each(output, &server->outputs, link)
		arrange_output(output);

	struct honey_window *window;
	wl_list_for_each(window, &server->windows, link) {
		bool visible = window->mapped && window->workspace
			&& window->workspace->output->active == window->workspace;
		wlr_scene_node_set_enabled(&window->tree->node, visible);
		wlr_scene_node_set_enabled(&window->surface_tree->node, visible);

		/* Keep the foreign-toplevel output association in sync with actual
		 * visibility: a window on a hidden workspace is on no output, a moved
		 * window changes output. Taskbars filter on this (all-outputs:false
		 * shows only the visible/current-workspace windows). */
		struct wlr_output *foreign_output = visible
			? window->workspace->output->wlr_output : NULL;
		if (window->foreign && foreign_output != window->foreign_output) {
			if (window->foreign_output)
				wlr_foreign_toplevel_handle_v1_output_leave(window->foreign,
						window->foreign_output);
			if (foreign_output)
				wlr_foreign_toplevel_handle_v1_output_enter(window->foreign,
						foreign_output);
			window->foreign_output = foreign_output;
		}
	}

	honey_ext_workspace_sync(server);
	honey_status_broadcast(server);
}
