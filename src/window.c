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
#include <string.h>

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

/* ------------------------------------------------------------- window states */

bool w3ld_window_is_tiled (struct w3ld_window *window) {
	return !window->floating && !window->fullscreen && !window->maximized
		&& !window->fake_fullscreen;
}

/* Move the window's scene trees to the layer its state calls for. */
void w3ld_window_update_layer (struct w3ld_window *window) {
	struct w3ld_server *server = window->server;
	enum w3ld_scene_layer layer = window->fullscreen ? W3LD_LAYER_FULLSCREEN
		: window->floating ? W3LD_LAYER_FLOATING
		: W3LD_LAYER_TILED;
	wlr_scene_node_reparent(&window->tree->node, server->layers[layer]);
	wlr_scene_node_reparent(&window->surface_tree->node, server->layers[layer]);
	wlr_scene_node_raise_to_top(&window->tree->node);
	wlr_scene_node_raise_to_top(&window->surface_tree->node);
}

/* Tell the client which fullscreen/maximized state it should render for. */
void w3ld_window_inform_states (struct w3ld_window *window) {
	bool fullscreen = window->fullscreen || window->fake_fullscreen;
	if (window->type == W3LD_WINDOW_X11) {
		wlr_xwayland_surface_set_fullscreen(window->xwayland_surface,
				fullscreen);
		wlr_xwayland_surface_set_maximized(window->xwayland_surface,
				window->maximized, window->maximized);
	} else {
		wlr_xdg_toplevel_set_fullscreen(window->xdg_toplevel, fullscreen);
		wlr_xdg_toplevel_set_maximized(window->xdg_toplevel, window->maximized);
	}
}

void w3ld_window_clear_states (struct w3ld_window *window) {
	window->floating = false;
	window->fullscreen = false;
	window->maximized = false;
	window->fake_fullscreen = false;
	w3ld_window_inform_states(window);
	w3ld_window_update_layer(window);
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

/* --------------------------------------------------------------------- rules */

static bool rule_matches (
	struct w3ld_rule *rule,
	struct w3ld_window *window
) {
	const char *value = rule->field == W3LD_RULE_APP_ID
		? w3ld_window_app_id(window)
		: rule->field == W3LD_RULE_TITLE
		? w3ld_window_title(window)
		: (window->initial_title ? window->initial_title : "");
	if (rule->regex)
		return regexec(&rule->re, value, 0, NULL, 0) == 0;
	return strcasestr(value, rule->pattern) != NULL;
}

/* Applied once, at map, after the workspace default is assigned. Returns
 * whether a no-focus rule matched. */
static bool apply_rules (struct w3ld_window *window) {
	struct w3ld_server *server = window->server;
	bool no_focus = false;

	struct w3ld_rule *rule;
	wl_list_for_each(rule, &server->rules, link) {
		if (!rule_matches(rule, window))
			continue;
		switch (rule->action) {
		case W3LD_RULE_WORKSPACE: {
			struct w3ld_workspace *workspace;
			if (w3ld_parse_ws_addr(server, rule->ws_addr, &workspace))
				window->workspace = workspace;
			break;
		}
		case W3LD_RULE_FLOAT: {
			window->floating = true;
			w3ld_float_seed(window);
			struct wlr_box *usable = &window->workspace->output->usable;
			int width = rule->float_w_px ? rule->float_w_px
				: rule->float_w > 0 ? (int)(usable->width * rule->float_w) : 0;
			int height = rule->float_h_px ? rule->float_h_px
				: rule->float_h > 0 ? (int)(usable->height * rule->float_h) : 0;
			if (width > 0 && height > 0) {
				window->float_pending_app_size = false;
				window->float_geom.width = width;
				window->float_geom.height = height;
				window->float_geom.x = usable->x + (usable->width - width) / 2;
				window->float_geom.y = usable->y
					+ (usable->height - height) / 2;
			}
			break;
		}
		case W3LD_RULE_TILE:
			window->floating = false;
			window->maximized = false;
			window->fullscreen = false;
			window->fake_fullscreen = false;
			break;
		case W3LD_RULE_SUPPRESS_MAXIMIZE:
			window->suppress_maximize = true;
			window->maximized = false;
			break;
		case W3LD_RULE_NO_FOCUS:
			no_focus = true;
			break;
		}
	}
	w3ld_window_inform_states(window);
	w3ld_window_update_layer(window);
	return no_focus;
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
	free(window->initial_title);
	window->initial_title = strdup(w3ld_window_title(window));
	bool no_focus = apply_rules(window);

	/* A fullscreen/maximized window on this workspace either yields to the
	 * new window or keeps covering it (and keeps focus). Fake-fullscreen is
	 * a rendering preference, not exclusivity — never affected. */
	bool covered = false;
	struct w3ld_window *other;
	wl_list_for_each(other, &server->windows, link) {
		if (!other->mapped || other->workspace != window->workspace)
			continue;
		if (!other->fullscreen && !other->maximized)
			continue;
		if (server->config.exit_fullscreen_on_new) {
			w3ld_window_clear_states(other);
		} else {
			covered = true;
		}
	}

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
	if (server->config.focus_new && !covered && !no_focus)
		w3ld_focus_window(window);
	if (server->config.mouse_focus_new && !covered && !no_focus)
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
	if (window->xdg_toplevel->base->initial_commit) {
		wlr_xdg_toplevel_set_size(window->xdg_toplevel, 0, 0);
		return;
	}

	/* float-app-size: adopt the size the app chose, re-centred. */
	if (window->float_pending_app_size && window->floating) {
		struct wlr_box *geometry = &window->xdg_toplevel->base->geometry;
		if (geometry->width > 0 && geometry->height > 0) {
			window->float_pending_app_size = false;
			struct wlr_box *usable = &window->workspace->output->usable;
			window->float_geom.width = geometry->width;
			window->float_geom.height = geometry->height;
			window->float_geom.x = usable->x
				+ (usable->width - geometry->width) / 2;
			window->float_geom.y = usable->y
				+ (usable->height - geometry->height) / 2;
			w3ld_arrange(window->server);
		}
	}
}

/* Client fullscreen requests are honored (games); maximize requests are
 * honored only for floating windows — the layout owns tiled geometry (apps
 * that remember being maximized would otherwise fill the screen on open). */
static void window_request_fullscreen (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window =
		wl_container_of(listener, window, request_fullscreen);
	if (window->mapped) {
		window->fullscreen = window->xdg_toplevel->requested.fullscreen;
		if (window->fullscreen) {
			window->maximized = false;
			window->fake_fullscreen = false;
		}
		w3ld_window_inform_states(window);
		w3ld_window_update_layer(window);
		w3ld_arrange(window->server);
	} else if (window->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(window->xdg_toplevel->base);
	}
}

static void window_request_maximize (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window =
		wl_container_of(listener, window, request_maximize);
	if (window->mapped && window->floating && !window->suppress_maximize) {
		window->maximized = window->xdg_toplevel->requested.maximized;
		w3ld_window_inform_states(window);
		w3ld_window_update_layer(window);
		w3ld_arrange(window->server);
	} else if (window->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(window->xdg_toplevel->base);
	}
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
	wl_list_remove(&window->request_fullscreen.link);
	wl_list_remove(&window->request_maximize.link);
	free(window->initial_title);
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
	window->request_fullscreen.notify = window_request_fullscreen;
	wl_signal_add(&toplevel->events.request_fullscreen,
			&window->request_fullscreen);
	window->request_maximize.notify = window_request_maximize;
	wl_signal_add(&toplevel->events.request_maximize,
			&window->request_maximize);
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
