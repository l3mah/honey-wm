/* Window lifecycle: creation, map/unmap, focus, teardown.
 *
 * A window is an XDG toplevel or an X11 (XWayland) surface; the type-agnostic
 * accessors hide the difference from layout, actions, and status. Each window
 * is a border tree plus a surface tree in the TILED scene layer. On map a
 * window joins its output's active workspace and the server window list (list
 * order is tiling order). The XDG path lives here; the X11 path in xwayland.c
 * calls the shared lifecycle.
 */
#include <stdlib.h>

#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/xwayland/xwayland.h>

#include "w3ld.h"

/* ---------------------------------------------------------------- accessors */

struct wlr_surface *w3ld_window_surface (struct w3ld_window *window) {
	if (window->type == W3LD_WINDOW_X11)
		return window->xwayland_surface->surface;
	return window->xdg_toplevel->base->surface;
}

const char *w3ld_window_title (struct w3ld_window *window) {
	const char *title = window->type == W3LD_WINDOW_X11
		? window->xwayland_surface->title
		: window->xdg_toplevel->title;
	return title ? title : "";
}

const char *w3ld_window_app_id (struct w3ld_window *window) {
	const char *app_id = window->type == W3LD_WINDOW_X11
		? window->xwayland_surface->class
		: window->xdg_toplevel->app_id;
	return app_id ? app_id : "";
}

/* Request the window's content box (position handled by the scene except for
 * X11, whose configure carries coordinates too). */
void w3ld_window_configure (
	struct w3ld_window *window,
	int x,
	int y,
	int width,
	int height
) {
	wlr_scene_node_set_position(&window->surface_tree->node, x, y);
	if (window->type == W3LD_WINDOW_X11)
		wlr_xwayland_surface_configure(window->xwayland_surface, x, y, width,
				height);
	else
		wlr_xdg_toplevel_set_size(window->xdg_toplevel, width, height);
}

void w3ld_window_set_activated (
	struct w3ld_window *window,
	bool activated
) {
	if (window->type == W3LD_WINDOW_X11)
		wlr_xwayland_surface_activate(window->xwayland_surface, activated);
	else
		wlr_xdg_toplevel_set_activated(window->xdg_toplevel, activated);
}

void w3ld_window_close (struct w3ld_window *window) {
	if (window->type == W3LD_WINDOW_X11)
		wlr_xwayland_surface_close(window->xwayland_surface);
	else
		wlr_xdg_toplevel_send_close(window->xdg_toplevel);
}

/* -------------------------------------------------------------------- borders */

static void color_to_float (
	uint32_t color,
	float out[4]
) {
	out[0] = ((color >> 24) & 0xff) / 255.0f;
	out[1] = ((color >> 16) & 0xff) / 255.0f;
	out[2] = ((color >> 8) & 0xff) / 255.0f;
	out[3] = (color & 0xff) / 255.0f;
}

static void set_border_color (
	struct w3ld_window *window,
	uint32_t color
) {
	float rgba[4];
	color_to_float(color, rgba);
	for (int i = 0; i < 4; i++)
		wlr_scene_rect_set_color(window->border[i], rgba);
}

/* ------------------------------------------------------- foreign toplevel */

static void foreign_activate (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window =
		wl_container_of(listener, window, foreign_activate);
	w3ld_focus_window(window);
}

static void foreign_close (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window =
		wl_container_of(listener, window, foreign_close);
	w3ld_window_close(window);
}

static void foreign_update (struct w3ld_window *window) {
	if (!window->foreign)
		return;
	wlr_foreign_toplevel_handle_v1_set_title(window->foreign,
			w3ld_window_title(window));
	wlr_foreign_toplevel_handle_v1_set_app_id(window->foreign,
			w3ld_window_app_id(window));
}

/* --------------------------------------------------------------------- focus */

void w3ld_focus_window (struct w3ld_window *window) {
	if (!window || !window->mapped)
		return;

	struct w3ld_server *server = window->server;
	if (server->focused_layer)
		return; /* a keyboard-interactive layer surface holds focus */
	server->focused_output = window->workspace->output;
	if (server->focused == window)
		return;

	if (server->focused) {
		w3ld_window_set_activated(server->focused, false);
		set_border_color(server->focused, server->config.border_color_inactive);
		if (server->focused->foreign)
			wlr_foreign_toplevel_handle_v1_set_activated(
					server->focused->foreign, false);
	}

	wlr_scene_node_raise_to_top(&window->tree->node);
	wlr_scene_node_raise_to_top(&window->surface_tree->node);
	w3ld_window_set_activated(window, true);
	set_border_color(window, server->config.border_color_active);
	if (window->foreign)
		wlr_foreign_toplevel_handle_v1_set_activated(window->foreign, true);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (keyboard) {
		wlr_seat_keyboard_notify_enter(server->seat,
				w3ld_window_surface(window),
				keyboard->keycodes, keyboard->num_keycodes,
				&keyboard->modifiers);
	}

	server->focused = window;
	w3ld_status_broadcast(server);
}

/* Focus the first window on an output's active workspace, or clear focus. */
void w3ld_focus_output_active (struct w3ld_output *output) {
	struct w3ld_server *server = output->server;
	server->focused_output = output;

	struct w3ld_window *window = w3ld_workspace_first_window(output->active);
	if (window) {
		server->focused = NULL; /* force re-activation */
		w3ld_focus_window(window);
		return;
	}

	if (server->focused) {
		w3ld_window_set_activated(server->focused, false);
		server->focused = NULL;
	}
	wlr_seat_keyboard_notify_clear_focus(server->seat);
	w3ld_status_broadcast(server);
}

/* ---------------------------------------------------------- shared lifecycle */

/* Create the border tree; the caller has already created surface_tree. */
void w3ld_window_finish_setup (struct w3ld_window *window) {
	struct w3ld_server *server = window->server;
	float inactive[4];
	color_to_float(server->config.border_color_inactive, inactive);
	window->tree = wlr_scene_tree_create(server->layers[W3LD_LAYER_TILED]);
	for (int i = 0; i < 4; i++)
		window->border[i] = wlr_scene_rect_create(window->tree, 1, 1, inactive);
	window->surface_tree->node.data = window;
}

void w3ld_window_handle_map (struct w3ld_window *window) {
	struct w3ld_server *server = window->server;

	window->mapped = true;
	window->workspace = server->focused_output->active;

	window->foreign = wlr_foreign_toplevel_handle_v1_create(
			server->foreign_toplevel_manager);
	foreign_update(window);
	wlr_foreign_toplevel_handle_v1_output_enter(window->foreign,
			window->workspace->output->wlr_output);
	window->foreign_activate.notify = foreign_activate;
	wl_signal_add(&window->foreign->events.request_activate,
			&window->foreign_activate);
	window->foreign_close.notify = foreign_close;
	wl_signal_add(&window->foreign->events.request_close,
			&window->foreign_close);

	if (server->config.new_window_master)
		wl_list_insert(&server->windows, &window->link);       /* master */
	else
		wl_list_insert(server->windows.prev, &window->link);   /* stack tail */

	w3ld_arrange(server);
	if (server->config.focus_new)
		w3ld_focus_window(window);
	if (server->config.mouse_focus_new)
		wlr_cursor_warp(server->cursor, NULL,
				window->geom.x + window->geom.width / 2,
				window->geom.y + window->geom.height / 2);
}

void w3ld_window_handle_unmap (struct w3ld_window *window) {
	struct w3ld_server *server = window->server;
	struct w3ld_output *output =
		window->workspace ? window->workspace->output : server->focused_output;

	if (window->foreign) {
		wl_list_remove(&window->foreign_activate.link);
		wl_list_remove(&window->foreign_close.link);
		wlr_foreign_toplevel_handle_v1_destroy(window->foreign);
		window->foreign = NULL;
	}

	window->mapped = false;
	wl_list_remove(&window->link);
	if (server->focused == window)
		server->focused = NULL;

	w3ld_arrange(server);
	if (output)
		w3ld_focus_output_active(output);
}

/* ----------------------------------------------------------- XDG listeners */

static void window_map (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window = wl_container_of(listener, window, map);
	w3ld_window_handle_map(window);
}

static void window_unmap (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window = wl_container_of(listener, window, unmap);
	w3ld_window_handle_unmap(window);
}

static void window_commit (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window = wl_container_of(listener, window, commit);
	/* Reply to the initial configure so the client can proceed to map; 0,0
	 * lets it pick its own initial size before the layout sizes it. */
	if (window->xdg_toplevel->base->initial_commit)
		wlr_xdg_toplevel_set_size(window->xdg_toplevel, 0, 0);
}

static void window_status_changed (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window = wl_container_of(listener, window, set_title);
	foreign_update(window);
	w3ld_status_broadcast(window->server);
}

static void window_app_id_changed (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window = wl_container_of(listener, window, set_app_id);
	foreign_update(window);
	w3ld_status_broadcast(window->server);
}

static void window_destroy (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window = wl_container_of(listener, window, destroy);
	wl_list_remove(&window->map.link);
	wl_list_remove(&window->unmap.link);
	wl_list_remove(&window->commit.link);
	wl_list_remove(&window->destroy.link);
	wl_list_remove(&window->set_title.link);
	wl_list_remove(&window->set_app_id.link);
	wlr_scene_node_destroy(&window->tree->node); /* borders; surface self-frees */
	free(window);
}

/* --------------------------------------------------------------- new toplevel */

static void new_xdg_toplevel (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;

	struct w3ld_window *window = calloc(1, sizeof *window);
	window->server = server;
	window->type = W3LD_WINDOW_XDG;
	window->xdg_toplevel = toplevel;
	window->surface_tree = wlr_scene_xdg_surface_create(
			server->layers[W3LD_LAYER_TILED], toplevel->base);
	w3ld_window_finish_setup(window);

	window->map.notify = window_map;
	wl_signal_add(&toplevel->base->surface->events.map, &window->map);
	window->unmap.notify = window_unmap;
	wl_signal_add(&toplevel->base->surface->events.unmap, &window->unmap);
	window->commit.notify = window_commit;
	wl_signal_add(&toplevel->base->surface->events.commit, &window->commit);
	window->destroy.notify = window_destroy;
	wl_signal_add(&toplevel->events.destroy, &window->destroy);
	window->set_title.notify = window_status_changed;
	wl_signal_add(&toplevel->events.set_title, &window->set_title);
	window->set_app_id.notify = window_app_id_changed;
	wl_signal_add(&toplevel->events.set_app_id, &window->set_app_id);
}

/* -------------------------------------------------------------------- setup */

void w3ld_window_setup (struct w3ld_server *server) {
	wl_list_init(&server->windows);
	server->foreign_toplevel_manager =
		wlr_foreign_toplevel_manager_v1_create(server->display);
	server->xdg_shell = wlr_xdg_shell_create(server->display, 3);
	server->new_xdg_toplevel.notify = new_xdg_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel,
			&server->new_xdg_toplevel);
}
