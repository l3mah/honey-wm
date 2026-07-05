/* Layer shell (wlr_layer_shell_v1): bars, wallpapers, and launchers.
 *
 * Each layer surface is placed in the scene tree matching its layer (background/
 * bottom/top/overlay) and positioned by the scene helper. Exclusive zones shrink
 * the output's usable area so tiling avoids bars. Keyboard-interactive surfaces
 * (launchers) take keyboard focus while mapped.
 */
#include <stdlib.h>

#include <wlr/types/wlr_layer_shell_v1.h>

#include "w3ld.h"

static struct wlr_scene_tree *layer_tree (
	struct w3ld_server *server,
	enum zwlr_layer_shell_v1_layer layer
) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return server->layers[W3LD_LAYER_BACKGROUND];
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return server->layers[W3LD_LAYER_BOTTOM];
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return server->layers[W3LD_LAYER_TOP];
	default:
		return server->layers[W3LD_LAYER_OVERLAY];
	}
}

/* Recompute the output's usable area: the full box minus each layer surface's
 * exclusive zone (exclusive surfaces first), configuring every surface. */
void w3ld_layer_arrange (struct w3ld_output *output) {
	struct wlr_box full;
	wlr_output_layout_get_box(output->server->output_layout,
			output->wlr_output, &full);
	struct wlr_box usable = full;

	for (int exclusive = 1; exclusive >= 0; exclusive--) {
		struct w3ld_layer_surface *layer;
		wl_list_for_each(layer, &output->layer_surfaces, link) {
			bool has_exclusive =
				layer->layer_surface->current.exclusive_zone > 0;
			if (has_exclusive != (exclusive == 1))
				continue;
			wlr_scene_layer_surface_v1_configure(layer->scene, &full, &usable);
		}
	}
	output->usable = usable;
	DBG("layer arrange %s: usable %d,%d %dx%d", output->wlr_output->name,
			usable.x, usable.y, usable.width, usable.height);
}

static void layer_rearrange (struct w3ld_layer_surface *layer) {
	w3ld_layer_arrange(layer->output);
	w3ld_arrange(layer->server);
}

/* Give keyboard focus to an interactive layer surface (a launcher). */
static void layer_focus (struct w3ld_layer_surface *layer) {
	struct w3ld_server *server = layer->server;
	if (layer->layer_surface->current.keyboard_interactive
			== ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)
		return;
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (keyboard) {
		wlr_seat_keyboard_notify_enter(server->seat,
				layer->layer_surface->surface, keyboard->keycodes,
				keyboard->num_keycodes, &keyboard->modifiers);
	}
	server->focused_layer = layer;
}

/* Restore focus to the active window when a focused layer surface goes away. */
static void layer_unfocus (struct w3ld_layer_surface *layer) {
	struct w3ld_server *server = layer->server;
	if (server->focused_layer != layer)
		return;
	server->focused_layer = NULL;
	if (server->focused_output)
		w3ld_focus_output_active(server->focused_output);
}

/* ----------------------------------------------------------------- listeners */

static void layer_map (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_layer_surface *layer = wl_container_of(listener, layer, map);
	DBG("layer surface mapped on %s", layer->output->wlr_output->name);
	layer_rearrange(layer);
	layer_focus(layer);
}

static void layer_unmap (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_layer_surface *layer = wl_container_of(listener, layer, unmap);
	layer_unfocus(layer);
	layer_rearrange(layer);
}

static void layer_commit (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_layer_surface *layer = wl_container_of(listener, layer, commit);
	wlr_scene_node_reparent(&layer->scene->tree->node,
			layer_tree(layer->server, layer->layer_surface->current.layer));
	layer_rearrange(layer);
}

static void layer_destroy (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_layer_surface *layer = wl_container_of(listener, layer, destroy);
	struct w3ld_server *server = layer->server;
	struct w3ld_output *output = layer->output;

	wl_list_remove(&layer->map.link);
	wl_list_remove(&layer->unmap.link);
	wl_list_remove(&layer->commit.link);
	wl_list_remove(&layer->destroy.link);
	wl_list_remove(&layer->link);
	layer_unfocus(layer);
	free(layer);

	if (output)
		w3ld_layer_arrange(output);
	w3ld_arrange(server);
}

static void new_layer_surface (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *layer_surface = data;

	if (!layer_surface->output) {
		if (!server->focused_output) {
			wlr_layer_surface_v1_destroy(layer_surface);
			return;
		}
		layer_surface->output = server->focused_output->wlr_output;
	}
	struct w3ld_output *output = layer_surface->output->data;

	struct w3ld_layer_surface *layer = calloc(1, sizeof *layer);
	layer->server = server;
	layer->output = output;
	layer->layer_surface = layer_surface;
	layer->scene = wlr_scene_layer_surface_v1_create(
			layer_tree(server, layer_surface->pending.layer), layer_surface);

	layer->map.notify = layer_map;
	wl_signal_add(&layer_surface->surface->events.map, &layer->map);
	layer->unmap.notify = layer_unmap;
	wl_signal_add(&layer_surface->surface->events.unmap, &layer->unmap);
	layer->commit.notify = layer_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &layer->commit);
	layer->destroy.notify = layer_destroy;
	wl_signal_add(&layer_surface->events.destroy, &layer->destroy);

	wl_list_insert(&output->layer_surfaces, &layer->link);
}

/* -------------------------------------------------------------------- setup */

void w3ld_layer_setup (struct w3ld_server *server) {
	for (int i = 0; i < W3LD_NUM_LAYERS; i++)
		server->layers[i] = wlr_scene_tree_create(&server->scene->tree);

	struct wlr_layer_shell_v1 *shell =
		wlr_layer_shell_v1_create(server->display, 5);
	server->new_layer_surface.notify = new_layer_surface;
	wl_signal_add(&shell->events.new_surface, &server->new_layer_surface);
}
