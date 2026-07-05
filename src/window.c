/* Window (wlr_xdg_toplevel) lifecycle: creation, map/unmap, focus, teardown.
 *
 * Each toplevel gets a wlr_scene_tree. Membership of the server window list is
 * managed on map/unmap; the list order is the tiling order. Focus is a separate
 * pointer plus keyboard/activation state — it never reorders the tiling list.
 */
#include <stdlib.h>

#include "w3ld.h"

/* --------------------------------------------------------------------- focus */

void w3ld_focus_window (struct w3ld_window *window) {
	if (!window || !window->mapped)
		return;

	struct w3ld_server *server = window->server;
	if (server->focused == window)
		return;

	if (server->focused)
		wlr_xdg_toplevel_set_activated(server->focused->xdg_toplevel, false);

	wlr_scene_node_raise_to_top(&window->scene_tree->node);
	wlr_xdg_toplevel_set_activated(window->xdg_toplevel, true);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (keyboard) {
		wlr_seat_keyboard_notify_enter(server->seat,
				window->xdg_toplevel->base->surface,
				keyboard->keycodes, keyboard->num_keycodes,
				&keyboard->modifiers);
	}

	server->focused = window;
}

/* Focus the first mapped window, or clear focus if none remain. */
static void focus_any (struct w3ld_server *server) {
	struct w3ld_window *window;
	wl_list_for_each(window, &server->windows, link) {
		if (window->mapped) {
			server->focused = NULL; /* force re-activation */
			w3ld_focus_window(window);
			return;
		}
	}
	server->focused = NULL;
}

/* ----------------------------------------------------------------- listeners */

static void window_map (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window = wl_container_of(listener, window, map);
	window->mapped = true;
	wl_list_insert(&window->server->windows, &window->link);
	w3ld_arrange(window->server);
	w3ld_focus_window(window);
}

static void window_unmap (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window = wl_container_of(listener, window, unmap);
	struct w3ld_server *server = window->server;

	window->mapped = false;
	wl_list_remove(&window->link);
	if (server->focused == window)
		server->focused = NULL;

	w3ld_arrange(server);
	if (!server->focused)
		focus_any(server);
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

static void window_destroy (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window = wl_container_of(listener, window, destroy);
	wl_list_remove(&window->map.link);
	wl_list_remove(&window->unmap.link);
	wl_list_remove(&window->commit.link);
	wl_list_remove(&window->destroy.link);
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
	window->xdg_toplevel = toplevel;
	window->scene_tree =
		wlr_scene_xdg_surface_create(&server->scene->tree, toplevel->base);
	window->scene_tree->node.data = window;

	window->map.notify = window_map;
	wl_signal_add(&toplevel->base->surface->events.map, &window->map);
	window->unmap.notify = window_unmap;
	wl_signal_add(&toplevel->base->surface->events.unmap, &window->unmap);
	window->commit.notify = window_commit;
	wl_signal_add(&toplevel->base->surface->events.commit, &window->commit);
	window->destroy.notify = window_destroy;
	wl_signal_add(&toplevel->events.destroy, &window->destroy);
}

/* -------------------------------------------------------------------- setup */

void w3ld_window_setup (struct w3ld_server *server) {
	wl_list_init(&server->windows);
	server->xdg_shell = wlr_xdg_shell_create(server->display, 3);
	server->new_xdg_toplevel.notify = new_xdg_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel,
			&server->new_xdg_toplevel);
}
