/* XWayland: run X11 apps.
 *
 * The Xwayland server starts lazily on the first X11 client. Managed X11
 * windows become regular w3ld windows (tiled, focused, closed through the
 * type-agnostic accessors) via the shared lifecycle in window.c. Override-
 * redirect surfaces (menus, tooltips, drag icons) are unmanaged: shown at their
 * own coordinates in the TOP layer, never tiled or focused.
 */
#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/xwayland/xwayland.h>

#include "w3ld.h"

struct w3ld_xwayland_surface {
	struct w3ld_server *server;
	struct wlr_xwayland_surface *xwayland_surface;
	struct w3ld_window *window;             /* managed windows, or NULL */
	struct wlr_scene_tree *unmanaged_tree;  /* override-redirect, or NULL */

	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener request_configure;
	struct wl_listener destroy;
};

/* ---------------------------------------------------------------- unmanaged */

static void unmanaged_show (struct w3ld_xwayland_surface *xw) {
	struct wlr_xwayland_surface *xsurface = xw->xwayland_surface;
	xw->unmanaged_tree = wlr_scene_tree_create(
			xw->server->layers[W3LD_LAYER_TOP]);
	wlr_scene_subsurface_tree_create(xw->unmanaged_tree, xsurface->surface);
	wlr_scene_node_set_position(&xw->unmanaged_tree->node, xsurface->x,
			xsurface->y);
}

/* ----------------------------------------------------------------- managed */

static void x11_map (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window = wl_container_of(listener, window, map);
	w3ld_window_handle_map(window);
}

static void x11_unmap (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window = wl_container_of(listener, window, unmap);
	w3ld_window_handle_unmap(window);
}

static void x11_title (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window = wl_container_of(listener, window, set_title);
	w3ld_status_broadcast(window->server);
}

static void x11_class (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window = wl_container_of(listener, window, set_app_id);
	w3ld_status_broadcast(window->server);
}

static void x11_request_fullscreen (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window =
		wl_container_of(listener, window, request_fullscreen);
	w3ld_window_handle_request_fullscreen(window);
}

static void x11_request_maximize (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window =
		wl_container_of(listener, window, request_maximize);
	w3ld_window_handle_request_maximize(window);
}

static void managed_show (struct w3ld_xwayland_surface *xw) {
	struct w3ld_server *server = xw->server;
	struct wlr_xwayland_surface *xsurface = xw->xwayland_surface;

	struct w3ld_window *window = calloc(1, sizeof *window);
	window->server = server;
	window->type = W3LD_WINDOW_X11;
	window->xwayland_surface = xsurface;
	window->surface_tree = wlr_scene_tree_create(
			server->layers[W3LD_LAYER_TILED]);
	wlr_scene_subsurface_tree_create(window->surface_tree, xsurface->surface);
	w3ld_window_finish_setup(window);
	xw->window = window;

	window->map.notify = x11_map;
	wl_signal_add(&xsurface->surface->events.map, &window->map);
	window->unmap.notify = x11_unmap;
	wl_signal_add(&xsurface->surface->events.unmap, &window->unmap);
	window->set_title.notify = x11_title;
	wl_signal_add(&xsurface->events.set_title, &window->set_title);
	window->set_app_id.notify = x11_class;
	wl_signal_add(&xsurface->events.set_class, &window->set_app_id);
	window->request_fullscreen.notify = x11_request_fullscreen;
	wl_signal_add(&xsurface->events.request_fullscreen,
			&window->request_fullscreen);
	window->request_maximize.notify = x11_request_maximize;
	wl_signal_add(&xsurface->events.request_maximize,
			&window->request_maximize);
}

/* ---------------------------------------------------------------- listeners */

/* The X11 window got a wlr_surface: build the managed or unmanaged scene. */
static void handle_associate (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_xwayland_surface *xw =
		wl_container_of(listener, xw, associate);
	if (xw->xwayland_surface->override_redirect)
		unmanaged_show(xw);
	else
		managed_show(xw);
}

static void handle_dissociate (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_xwayland_surface *xw =
		wl_container_of(listener, xw, dissociate);
	if (xw->window) {
		struct w3ld_window *window = xw->window;
		if (window->mapped)
			w3ld_window_handle_unmap(window);
		wl_list_remove(&window->map.link);
		wl_list_remove(&window->unmap.link);
		wl_list_remove(&window->set_title.link);
		wl_list_remove(&window->set_app_id.link);
		wl_list_remove(&window->request_fullscreen.link);
		wl_list_remove(&window->request_maximize.link);
		free(window->initial_title);
		wlr_scene_node_destroy(&window->surface_tree->node);
		wlr_scene_node_destroy(&window->tree->node);
		free(window);
		xw->window = NULL;
	}
	if (xw->unmanaged_tree) {
		wlr_scene_node_destroy(&xw->unmanaged_tree->node);
		xw->unmanaged_tree = NULL;
	}
}

/* Honor configure requests for windows the layout doesn't own yet. */
static void handle_request_configure (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_xwayland_surface *xw =
		wl_container_of(listener, xw, request_configure);
	struct wlr_xwayland_surface_configure_event *event = data;
	if (!xw->window || !xw->window->mapped)
		wlr_xwayland_surface_configure(xw->xwayland_surface, event->x,
				event->y, event->width, event->height);
}

static void handle_destroy (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_xwayland_surface *xw = wl_container_of(listener, xw, destroy);
	wl_list_remove(&xw->associate.link);
	wl_list_remove(&xw->dissociate.link);
	wl_list_remove(&xw->request_configure.link);
	wl_list_remove(&xw->destroy.link);
	free(xw);
}

static void handle_new_surface (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *xsurface = data;

	struct w3ld_xwayland_surface *xw = calloc(1, sizeof *xw);
	xw->server = server;
	xw->xwayland_surface = xsurface;

	xw->associate.notify = handle_associate;
	wl_signal_add(&xsurface->events.associate, &xw->associate);
	xw->dissociate.notify = handle_dissociate;
	wl_signal_add(&xsurface->events.dissociate, &xw->dissociate);
	xw->request_configure.notify = handle_request_configure;
	wl_signal_add(&xsurface->events.request_configure, &xw->request_configure);
	xw->destroy.notify = handle_destroy;
	wl_signal_add(&xsurface->events.destroy, &xw->destroy);
}

static void handle_ready (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, xwayland_ready);
	wlr_xwayland_set_seat(server->xwayland, server->seat);
}

/* -------------------------------------------------------------------- setup */

void w3ld_xwayland_setup (struct w3ld_server *server) {
	server->xwayland = wlr_xwayland_create(server->display, server->compositor,
			true /* lazy: start on first X11 client */);
	if (!server->xwayland) {
		LOG("XWayland unavailable (Xwayland binary not found?)");
		return;
	}

	server->xwayland_ready.notify = handle_ready;
	wl_signal_add(&server->xwayland->events.ready, &server->xwayland_ready);
	server->new_xwayland_surface.notify = handle_new_surface;
	wl_signal_add(&server->xwayland->events.new_surface,
			&server->new_xwayland_surface);

	setenv("DISPLAY", server->xwayland->display_name, true);
	LOG("XWayland on DISPLAY=%s", server->xwayland->display_name);
}
