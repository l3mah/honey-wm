/* Window lifecycle: creation, map/unmap, focus, teardown.
 *
 * A window is an XDG toplevel or an X11 (XWayland) surface; the type-agnostic
 * accessors hide the difference from layout, actions, and status. Each window
 * is a border tree plus a surface tree in the TILED scene layer. On map a
 * window joins its output's active workspace and the server window list (list
 * order is tiling order). The XDG path lives here; the X11 path in xwayland.c
 * calls the shared lifecycle.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/util/edges.h>
#include <wlr/xwayland/xwayland.h>

#include "honey.h"

/* ---------------------------------------------------------------- accessors */

struct wlr_surface *honey_window_surface (struct honey_window *window) {
	if (window->type == HONEY_WINDOW_X11)
		return window->xwayland_surface->surface;
	return window->xdg_toplevel->base->surface;
}

const char *honey_window_title (struct honey_window *window) {
	const char *title = window->type == HONEY_WINDOW_X11
		? window->xwayland_surface->title
		: window->xdg_toplevel->title;
	return title ? title : "";
}

const char *honey_window_app_id (struct honey_window *window) {
	const char *app_id = window->type == HONEY_WINDOW_X11
		? window->xwayland_surface->class
		: window->xdg_toplevel->app_id;
	return app_id ? app_id : "";
}

/* Request the window's content box (position handled by the scene except for
 * X11, whose configure carries coordinates too). */
void honey_window_configure (
	struct honey_window *window,
	int x,
	int y,
	int width,
	int height
) {
	wlr_scene_node_set_position(&window->surface_tree->node, x, y);
	wlr_scene_rect_set_size(window->dim, width, height);

	/* Skip duplicate requests: wlroots schedules a configure per call, so
	 * re-sending an unchanged box on every arrange amplifies any
	 * commit-driven arrange into an event flood. */
	bool same_size = width == window->requested.width
			&& height == window->requested.height;
	bool same_position = x == window->requested.x && y == window->requested.y;
	window->requested = (struct wlr_box){ x, y, width, height };

	if (window->type == HONEY_WINDOW_X11) {
		if (same_size && same_position)
			return;
		/* xwayland-scale (removable): X11 lives in the scaled xwayland
		 * coordinate space — renders at its output's physical pixels, shown at
		 * logical size. Remove -> plain wlr_xwayland_surface_configure. */
		struct honey_server *server = window->server;
		int xw_x, xw_y;
		honey_to_xwayland(server, x, y, &xw_x, &xw_y);
		double scale = honey_xwayland_scale_at(server, x, y);
		wlr_xwayland_surface_configure(window->xwayland_surface, xw_x, xw_y,
				(int)round(width * scale), (int)round(height * scale));
	} else if (window->float_pending_app_size) {
		/* Awaiting the client's own size: keep asking it to choose (0,0) rather
		 * than pinning the seeded placeholder, so arrange cannot clobber the
		 * pending choice. window_commit adopts the committed size and re-places. */
		wlr_xdg_toplevel_set_size(window->xdg_toplevel, 0, 0);
	} else {
		if (!same_size)
			window->resize_serial = wlr_xdg_toplevel_set_size(
					window->xdg_toplevel, width, height);
	}
}

void honey_window_set_activated (
	struct honey_window *window,
	bool activated
) {
	if (window->type == HONEY_WINDOW_X11)
		wlr_xwayland_surface_activate(window->xwayland_surface, activated);
	else
		wlr_xdg_toplevel_set_activated(window->xdg_toplevel, activated);
}

void honey_window_close (struct honey_window *window) {
	if (window->type == HONEY_WINDOW_X11)
		wlr_xwayland_surface_close(window->xwayland_surface);
	else
		wlr_xdg_toplevel_send_close(window->xdg_toplevel);
}

/* ------------------------------------------------------------- window states */

bool honey_window_is_tiled (struct honey_window *window) {
	return !window->floating && !window->fullscreen && !window->maximized
		&& !window->fake_fullscreen;
}

/* Move the window's scene trees to the layer its state calls for. */
void honey_window_update_layer (struct honey_window *window) {
	struct honey_server *server = window->server;
	enum honey_scene_layer layer = window->fullscreen ? HONEY_LAYER_FULLSCREEN
		: window->floating ? HONEY_LAYER_FLOATING
		: HONEY_LAYER_TILED;
	wlr_scene_node_reparent(&window->tree->node, server->layers[layer]);
	wlr_scene_node_reparent(&window->surface_tree->node, server->layers[layer]);
	wlr_scene_node_raise_to_top(&window->tree->node);
	wlr_scene_node_raise_to_top(&window->surface_tree->node);
}

/* Tell the client which fullscreen/maximized state it should render for. */
void honey_window_inform_states (struct honey_window *window) {
	bool fullscreen = window->fullscreen || window->fake_fullscreen;
	if (window->type == HONEY_WINDOW_X11) {
		wlr_xwayland_surface_set_fullscreen(window->xwayland_surface,
				fullscreen);
		wlr_xwayland_surface_set_maximized(window->xwayland_surface,
				window->maximized, window->maximized);
	} else {
		wlr_xdg_toplevel_set_fullscreen(window->xdg_toplevel, fullscreen);
		wlr_xdg_toplevel_set_maximized(window->xdg_toplevel, window->maximized);
		/* Tiled states make the configured size binding: mpv letterboxes
		 * instead of shrinking to the video's aspect, GTK squares its
		 * corners. Cleared when floating so apps size freely again.
		 * (xdg-shell v2 introduced the states; older clients get nothing.) */
		if (wl_resource_get_version(window->xdg_toplevel->resource) >= 2) {
			wlr_xdg_toplevel_set_tiled(window->xdg_toplevel,
					honey_window_is_tiled(window)
					? WLR_EDGE_LEFT | WLR_EDGE_RIGHT
						| WLR_EDGE_TOP | WLR_EDGE_BOTTOM
					: WLR_EDGE_NONE);
		}
	}
}

void honey_window_clear_states (struct honey_window *window) {
	window->floating = false;
	window->fullscreen = false;
	window->maximized = false;
	window->fake_fullscreen = false;
	honey_window_inform_states(window);
	honey_window_update_layer(window);
}

void honey_window_apply_state (struct honey_window *window) {
	honey_window_inform_states(window);
	honey_window_update_layer(window);
	honey_arrange(window->server);
}

void honey_window_set_float_geom (
	struct honey_window *window,
	int width,
	int height
) {
	struct wlr_box *usable = &window->workspace->output->usable;
	window->float_geom = (struct wlr_box){
		.x = usable->x + (usable->width - width) / 2,
		.y = usable->y + (usable->height - height) / 2,
		.width = width,
		.height = height,
	};
}

/* Fullscreen requests are honored (games). Maximize requests are honored only
 * for floating windows — the layout owns tiled geometry (apps that remember
 * being maximized would otherwise fill the screen on open) — and never with a
 * suppress-maximize rule. */
void honey_window_handle_request_fullscreen (struct honey_window *window) {
	bool wanted = window->type == HONEY_WINDOW_X11
		? window->xwayland_surface->fullscreen
		: window->xdg_toplevel->requested.fullscreen;
	if (window->mapped) {
		window->fullscreen = wanted;
		if (wanted) {
			window->maximized = false;
			window->fake_fullscreen = false;
		}
		honey_window_apply_state(window);
	} else if (window->type == HONEY_WINDOW_XDG
			&& window->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(window->xdg_toplevel->base);
	}
}

void honey_window_handle_request_maximize (struct honey_window *window) {
	bool wanted = window->type == HONEY_WINDOW_X11
		? (window->xwayland_surface->maximized_horz
				|| window->xwayland_surface->maximized_vert)
		: window->xdg_toplevel->requested.maximized;
	if (window->mapped && window->floating && !window->suppress_maximize) {
		window->maximized = wanted;
		honey_window_apply_state(window);
	} else if (window->type == HONEY_WINDOW_XDG
			&& window->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(window->xdg_toplevel->base);
	}
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
	struct honey_window *window,
	uint32_t color
) {
	float rgba[4];
	color_to_float(color, rgba);
	for (int i = 0; i < 4; i++)
		wlr_scene_rect_set_color(window->border[i], rgba);
}

/* ------------------------------------------------------------------- effects */

static void buffer_set_opacity (
	struct wlr_scene_buffer *buffer,
	int sx,
	int sy,
	void *data
) {
	wlr_scene_buffer_set_opacity(buffer, *(float *)data);
}

/* Apply opacity and the dim overlay for the window's focus state. */
static void apply_effects (
	struct honey_window *window,
	bool focused
) {
	struct honey_config *config = &window->server->config;

	float opacity = focused ? (float)config->active_opacity
		: (float)config->inactive_opacity;
	wlr_scene_node_for_each_buffer(&window->surface_tree->node,
			buffer_set_opacity, &opacity);

	bool dim = !focused && config->dim_inactive > 0;
	wlr_scene_node_set_enabled(&window->dim->node, dim);
	if (dim) {
		float shade[4] = { 0, 0, 0, (float)config->dim_inactive };
		wlr_scene_rect_set_color(window->dim, shade);
	}
}

/* ------------------------------------------------------- foreign toplevel */

static void foreign_activate (
	struct wl_listener *listener,
	void *data
) {
	struct honey_window *window =
		wl_container_of(listener, window, foreign_activate);
	honey_focus_window(window);
}

static void foreign_close (
	struct wl_listener *listener,
	void *data
) {
	struct honey_window *window =
		wl_container_of(listener, window, foreign_close);
	honey_window_close(window);
}

static void foreign_update (struct honey_window *window) {
	if (!window->foreign)
		return;
	wlr_foreign_toplevel_handle_v1_set_title(window->foreign,
			honey_window_title(window));
	wlr_foreign_toplevel_handle_v1_set_app_id(window->foreign,
			honey_window_app_id(window));
}

/* --------------------------------------------------------------------- focus */

void honey_focus_window (struct honey_window *window) {
	if (!window || !window->mapped)
		return;

	struct honey_server *server = window->server;
	if (server->focused_layer)
		return; /* a keyboard-interactive layer surface holds focus */
	server->focused_output = window->workspace->output;
	if (server->focused == window)
		return;

	if (server->focused) {
		honey_window_set_activated(server->focused, false);
		set_border_color(server->focused, server->config.border_color_inactive);
		apply_effects(server->focused, false);
		if (server->focused->foreign)
			wlr_foreign_toplevel_handle_v1_set_activated(
					server->focused->foreign, false);
	}

	wlr_scene_node_raise_to_top(&window->tree->node);
	wlr_scene_node_raise_to_top(&window->surface_tree->node);
	honey_window_set_activated(window, true);
	set_border_color(window, server->config.border_color_active);
	apply_effects(window, true);
	if (window->foreign)
		wlr_foreign_toplevel_handle_v1_set_activated(window->foreign, true);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (keyboard) {
		wlr_seat_keyboard_notify_enter(server->seat,
				honey_window_surface(window),
				keyboard->keycodes, keyboard->num_keycodes,
				&keyboard->modifiers);
	}

	server->focused = window;
	honey_status_broadcast(server);
}

/* Focus the first window on an output's active workspace, or clear focus. */
void honey_focus_output_active (struct honey_output *output) {
	struct honey_server *server = output->server;
	server->focused_output = output;

	struct honey_window *window = honey_workspace_first_window(output->active);
	if (window) {
		server->focused = NULL; /* force re-activation */
		honey_focus_window(window);
		return;
	}

	if (server->focused) {
		honey_window_set_activated(server->focused, false);
		server->focused = NULL;
	}
	wlr_seat_keyboard_notify_clear_focus(server->seat);
	honey_status_broadcast(server);
}

/* --------------------------------------------------------------------- rules */

static bool rule_matches (
	struct honey_rule *rule,
	struct honey_window *window
) {
	const char *value = rule->field == HONEY_RULE_APP_ID
		? honey_window_app_id(window)
		: rule->field == HONEY_RULE_TITLE
		? honey_window_title(window)
		: (window->initial_title ? window->initial_title : "");
	if (rule->regex)
		return regexec(&rule->re, value, 0, NULL, 0) == 0;
	return strcasestr(value, rule->pattern) != NULL;
}

/* Applied once, at map, after the workspace default is assigned. Returns
 * whether a no-focus rule matched. */
static bool apply_rules (struct honey_window *window) {
	struct honey_server *server = window->server;
	bool no_focus = false;

	struct honey_rule *rule;
	wl_list_for_each(rule, &server->rules, link) {
		if (!rule_matches(rule, window))
			continue;
		switch (rule->action) {
		case HONEY_RULE_WORKSPACE: {
			struct honey_workspace *workspace;
			if (honey_parse_ws_addr(server, rule->ws_addr, &workspace))
				window->workspace = workspace;
			break;
		}
		case HONEY_RULE_FLOAT: {
			window->floating = true;
			honey_float_seed(window);
			struct wlr_box *usable = &window->workspace->output->usable;
			int width = rule->float_w_px ? rule->float_w_px
				: rule->float_w > 0 ? (int)(usable->width * rule->float_w) : 0;
			int height = rule->float_h_px ? rule->float_h_px
				: rule->float_h > 0 ? (int)(usable->height * rule->float_h) : 0;
			if (width > 0 && height > 0) {
				window->float_pending_app_size = false;
				honey_window_set_float_geom(window, width, height);
			}
			break;
		}
		case HONEY_RULE_TILE:
			window->floating = false;
			window->maximized = false;
			window->fullscreen = false;
			window->fake_fullscreen = false;
			break;
		case HONEY_RULE_SUPPRESS_MAXIMIZE:
			window->suppress_maximize = true;
			window->maximized = false;
			break;
		case HONEY_RULE_NO_FOCUS:
			no_focus = true;
			break;
		}
	}
	honey_window_inform_states(window);
	honey_window_update_layer(window);
	return no_focus;
}

/* ---------------------------------------------------------- shared lifecycle */

/* Create the border tree; the caller has already created surface_tree. */
void honey_window_finish_setup (struct honey_window *window) {
	struct honey_server *server = window->server;
	float inactive[4];
	color_to_float(server->config.border_color_inactive, inactive);
	window->tree = wlr_scene_tree_create(server->layers[HONEY_LAYER_TILED]);
	for (int i = 0; i < 4; i++)
		window->border[i] = wlr_scene_rect_create(window->tree, 1, 1, inactive);
	window->surface_tree->node.data = window;
	window->tree->node.data = window; /* border-rect clicks resolve here */

	/* Dim overlay: created last in the surface tree so it draws above the
	 * content; sized in honey_window_configure, enabled while unfocused. */
	float shade[4] = { 0, 0, 0, 0 };
	window->dim = wlr_scene_rect_create(window->surface_tree, 1, 1, shade);
	wlr_scene_node_set_enabled(&window->dim->node, false);
}

/* Dialogs, utilities, menus, tooltips, transients, modals, and fixed-size
 * windows float rather than tile — the Hyprland shouldBeFloated policy, for
 * both XDG (parent set, or fixed min==max size) and X11 (window-type atoms,
 * transient/modal/parent, or fixed size). */
static bool window_wants_floating (struct honey_window *window) {
	if (window->type == HONEY_WINDOW_X11) {
		struct wlr_xwayland_surface *xsurface = window->xwayland_surface;
		static const enum wlr_xwayland_net_wm_window_type float_types[] = {
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU,
		};
		for (size_t i = 0; i < sizeof float_types / sizeof float_types[0]; i++)
			if (wlr_xwayland_surface_has_window_type(xsurface, float_types[i]))
				return true;
		if (xsurface->modal || xsurface->parent)
			return true;
		xcb_size_hints_t *hints = xsurface->size_hints;
		if (hints && hints->min_width > 0 && hints->min_height > 0
				&& hints->min_width == hints->max_width
				&& hints->min_height == hints->max_height)
			return true;
		return false;
	}

	/* A transient (has a parent) floats. So does a fixed-size toplevel
	 * (min == max): non-resizable windows are dialogs, pickers, and progress
	 * windows (e.g. a GTK file-copy dialog that sets no parent) — tiling them
	 * stretches them to a full column. Xdg-shell has no window-type atoms, so
	 * this size signal is the portable equivalent of the X11 types above. */
	if (window->xdg_toplevel->parent)
		return true;
	struct wlr_xdg_toplevel_state *state = &window->xdg_toplevel->current;
	return state->min_width > 0 && state->min_height > 0
		&& state->min_width == state->max_width
		&& state->min_height == state->max_height;
}

/* Float an XDG window at its own natural size, centred. Sends 0,0 so the
 * client re-picks its preferred size and adopts it on the next commit — this
 * recovers the size if a tile-size configure had stretched the window while it
 * was reserved (a dialog whose parent arrived after its first commit would
 * otherwise float at a full column's height). The seeded size is clamped to
 * the output, so it can never exceed the screen in the meantime. This is the
 * dialog path; forced floats (rules, toggle-float) size via honey_float_seed. */
static void xdg_float_natural (struct honey_window *window) {
	struct wlr_box *usable = &window->workspace->output->usable;
	int width = window->xdg_toplevel->base->geometry.width;
	int height = window->xdg_toplevel->base->geometry.height;
	if (width <= 0 || width > usable->width)
		width = (int)(usable->width * 0.4);
	if (height <= 0 || height > usable->height)
		height = (int)(usable->height * 0.4);
	honey_window_set_float_geom(window, width, height);
	window->float_pending_app_size = true;
	window->resize_serial = 0;
	wlr_xdg_toplevel_set_size(window->xdg_toplevel, 0, 0);
}

void honey_window_handle_map (struct honey_window *window) {
	struct honey_server *server = window->server;

	window->mapped = true;
	if (!window->placed) /* XDG windows reserved their slot at first commit */
		window->workspace = server->focused_output->active;
	free(window->initial_title);
	window->initial_title = strdup(honey_window_title(window));

	/* Dialog-class windows float centred at their own size rather than tiling. */
	if (window_wants_floating(window)) {
		window->floating = true;
		honey_window_update_layer(window);
		if (window->type == HONEY_WINDOW_X11) {
			/* xwayland-scale (removable): X11 reports its size in the scaled
			 * coordinate space; float_geom is logical and configure re-applies
			 * the scale — divide it back
			 * or the window is configured scale-times too big (splash paints
			 * its real size in the corner of a black, oversized buffer). */
			double scale =
				honey_output_xwayland_scale(window->workspace->output);
			int width = (int)round(window->xwayland_surface->width / scale);
			int height = (int)round(window->xwayland_surface->height / scale);
			struct wlr_box *usable = &window->workspace->output->usable;
			if (width <= 0 || width > usable->width)
				width = (int)(usable->width * 0.4);
			if (height <= 0 || height > usable->height)
				height = (int)(usable->height * 0.4);
			honey_window_set_float_geom(window, width, height);
		} else if (window->placed) {
			/* Reserved as a tile before its dialog nature was known: its
			 * geometry is the stretched tile size, so recover the client's own
			 * size (0,0 adopt). Stays hidden until it commits that size. */
			xdg_float_natural(window);
		} else {
			/* Parented from the start, never reserved: the geometry is already
			 * the client's own size — use it directly, centred, shown at once. */
			struct wlr_box *g = &window->xdg_toplevel->base->geometry;
			int width = g->width, height = g->height;
			struct wlr_box *usable = &window->workspace->output->usable;
			if (width <= 0 || width > usable->width)
				width = (int)(usable->width * 0.4);
			if (height <= 0 || height > usable->height)
				height = (int)(usable->height * 0.4);
			honey_window_set_float_geom(window, width, height);
		}
	}

	bool no_focus = apply_rules(window);
	/* Rules have settled tiled-vs-floating; tell the client (tiled states). */
	honey_window_inform_states(window);

	/* A fullscreen/maximized window on this workspace either yields to the
	 * new window or keeps covering it (and keeps focus). Fake-fullscreen is
	 * a rendering preference, not exclusivity — never affected. */
	bool covered = false;
	struct honey_window *other;
	wl_list_for_each(other, &server->windows, link) {
		if (!other->mapped || other->workspace != window->workspace)
			continue;
		if (!other->fullscreen && !other->maximized)
			continue;
		if (server->config.exit_fullscreen_on_new) {
			honey_window_clear_states(other);
		} else {
			covered = true;
		}
	}

	window->foreign = wlr_foreign_toplevel_handle_v1_create(
			server->foreign_toplevel_manager);
	foreign_update(window);
	/* Output association is maintained by honey_arrange (tracks visibility). */
	window->foreign_activate.notify = foreign_activate;
	wl_signal_add(&window->foreign->events.request_activate,
			&window->foreign_activate);
	window->foreign_close.notify = foreign_close;
	wl_signal_add(&window->foreign->events.request_close,
			&window->foreign_close);

	if (!window->placed) {
		if (server->config.new_window_master)
			wl_list_insert(&server->windows, &window->link);     /* master */
		else
			wl_list_insert(server->windows.prev, &window->link); /* tail */
		window->placed = true;
	}

	honey_arrange(server);
	if (server->config.focus_new && !covered && !no_focus)
		honey_focus_window(window);
	if (server->config.mouse_focus_new && !covered && !no_focus)
		wlr_cursor_warp(server->cursor, NULL,
				window->geom.x + window->geom.width / 2,
				window->geom.y + window->geom.height / 2);
}

void honey_window_handle_unmap (struct honey_window *window) {
	struct honey_server *server = window->server;
	struct honey_output *output =
		window->workspace ? window->workspace->output : server->focused_output;

	if (window->foreign) {
		wl_list_remove(&window->foreign_activate.link);
		wl_list_remove(&window->foreign_close.link);
		wlr_foreign_toplevel_handle_v1_destroy(window->foreign);
		window->foreign = NULL;
	}

	window->mapped = false;
	wl_list_remove(&window->link);
	window->placed = false;
	if (server->focused == window)
		server->focused = NULL;
	if (server->op_window == window) { /* grabbed window went away */
		server->op = HONEY_OP_NONE;
		server->op_window = NULL;
	}

	honey_arrange(server);
	if (output)
		honey_focus_output_active(output);
}

/* ----------------------------------------------------------- XDG listeners */

static void window_map (
	struct wl_listener *listener,
	void *data
) {
	struct honey_window *window = wl_container_of(listener, window, map);
	honey_window_handle_map(window);
}

static void window_unmap (
	struct wl_listener *listener,
	void *data
) {
	struct honey_window *window = wl_container_of(listener, window, unmap);
	honey_window_handle_unmap(window);
}

/* Reserve the tile at the first commit, before the window maps: assign the
 * workspace, join the window list, and arrange, so the initial configure
 * already carries the exact slot size (plus tiled states) and the first
 * buffer tiles perfectly instead of flashing at the app's own size. */
static void window_reserve (struct honey_window *window) {
	struct honey_server *server = window->server;
	window->workspace = server->focused_output->active;
	window->placed = true;
	if (server->config.new_window_master)
		wl_list_insert(&server->windows, &window->link);
	else
		wl_list_insert(server->windows.prev, &window->link);
	honey_window_inform_states(window);
	honey_arrange(server);
}

static void window_commit (
	struct wl_listener *listener,
	void *data
) {
	struct honey_window *window = wl_container_of(listener, window, commit);
	/* Reply to the initial configure so the client can proceed to map.
	 * Dialogs and float-bound windows pick their own size (0,0); tiling-bound
	 * windows are placed now, sized by the layout before they ever draw. */
	if (window->xdg_toplevel->base->initial_commit) {
		if (!window_wants_floating(window) && window->server->focused_output)
			window_reserve(window);
		else
			wlr_xdg_toplevel_set_size(window->xdg_toplevel, 0, 0);
		return;
	}

	if (!window->floating)
		return;
	struct wlr_box *geometry = &window->xdg_toplevel->base->geometry;
	if (geometry->width <= 0 || geometry->height <= 0)
		return;

	/* While our own resize is in flight, commits still carry the previous
	 * geometry; adopting it as a "self-resize" would fight the request. */
	if (window->resize_serial != 0) {
		if (window->xdg_toplevel->base->current.configure_serial
				< window->resize_serial)
			return;
		window->resize_serial = 0;
	}

	/* float-app-size: adopt the app's chosen size, re-centred, once. */
	if (window->float_pending_app_size) {
		window->float_pending_app_size = false;
		honey_window_set_float_geom(window, geometry->width, geometry->height);
		honey_arrange(window->server);
		return;
	}

	/* Track a client that resizes itself (e.g. a dialog changing tabs) so the
	 * border keeps framing the actual window instead of the previous size. */
	int border = window->server->config.border_size;
	if (window->float_geom.width != geometry->width + 2 * border
			|| window->float_geom.height != geometry->height + 2 * border) {
		window->float_geom.width = geometry->width + 2 * border;
		window->float_geom.height = geometry->height + 2 * border;
		honey_arrange(window->server);
	}
}

static void window_request_fullscreen (
	struct wl_listener *listener,
	void *data
) {
	struct honey_window *window =
		wl_container_of(listener, window, request_fullscreen);
	honey_window_handle_request_fullscreen(window);
}

static void window_request_maximize (
	struct wl_listener *listener,
	void *data
) {
	struct honey_window *window =
		wl_container_of(listener, window, request_maximize);
	honey_window_handle_request_maximize(window);
}

static void window_status_changed (
	struct wl_listener *listener,
	void *data
) {
	struct honey_window *window = wl_container_of(listener, window, set_title);
	foreign_update(window);
	honey_status_broadcast(window->server);
}

static void window_app_id_changed (
	struct wl_listener *listener,
	void *data
) {
	struct honey_window *window = wl_container_of(listener, window, set_app_id);
	foreign_update(window);
	honey_status_broadcast(window->server);
}

/* A window that gains a parent after mapping (a dialog whose transient-for
 * lands late) becomes a float at its natural size — otherwise it stays a
 * full-height tiled column. */
static void window_parent_changed (
	struct wl_listener *listener,
	void *data
) {
	struct honey_window *window = wl_container_of(listener, window, set_parent);
	if (!window->mapped || !honey_window_is_tiled(window)
			|| !window->xdg_toplevel->parent)
		return;
	window->floating = true;
	honey_window_update_layer(window);
	honey_window_inform_states(window);
	xdg_float_natural(window);
	honey_arrange(window->server);
}

static void window_destroy (
	struct wl_listener *listener,
	void *data
) {
	struct honey_window *window = wl_container_of(listener, window, destroy);
	if (window->placed) { /* reserved at first commit but never mapped */
		wl_list_remove(&window->link);
		honey_arrange(window->server);
	}
	wl_list_remove(&window->map.link);
	wl_list_remove(&window->unmap.link);
	wl_list_remove(&window->commit.link);
	wl_list_remove(&window->destroy.link);
	wl_list_remove(&window->set_title.link);
	wl_list_remove(&window->set_app_id.link);
	wl_list_remove(&window->set_parent.link);
	wl_list_remove(&window->request_fullscreen.link);
	wl_list_remove(&window->request_maximize.link);
	free(window->initial_title);
	wlr_scene_node_destroy(&window->tree->node); /* borders; surface self-frees */
	free(window);
}

/* --------------------------------------------------------------------- popups */

/* wlr_scene_xdg_surface_create renders a surface and its sub-surfaces but NOT
 * its popups; each popup needs its own scene node parented to its parent's
 * scene tree (stored in xdg_surface->data). Without this, menus are invisible
 * even though the client's keyboard grab still works. */
struct honey_popup {
	struct wlr_xdg_popup *popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static void popup_commit (
	struct wl_listener *listener,
	void *data
) {
	struct honey_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->popup->base->initial_commit)
		wlr_xdg_surface_schedule_configure(popup->popup->base);
}

static void popup_destroy (
	struct wl_listener *listener,
	void *data
) {
	struct honey_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void new_xdg_popup (
	struct wl_listener *listener,
	void *data
) {
	struct wlr_xdg_popup *xdg_popup = data;
	struct wlr_xdg_surface *parent =
		wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	if (!parent || !parent->data)
		return; /* parent scene tree unknown (e.g. a layer surface) */

	struct wlr_scene_tree *parent_tree = parent->data;
	xdg_popup->base->data =
		wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	struct honey_popup *popup = calloc(1, sizeof *popup);
	popup->popup = xdg_popup;
	popup->commit.notify = popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* --------------------------------------------------------------- new toplevel */

static void new_xdg_toplevel (
	struct wl_listener *listener,
	void *data
) {
	struct honey_server *server =
		wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;

	struct honey_window *window = calloc(1, sizeof *window);
	window->server = server;
	window->type = HONEY_WINDOW_XDG;
	window->xdg_toplevel = toplevel;
	window->surface_tree = wlr_scene_xdg_surface_create(
			server->layers[HONEY_LAYER_TILED], toplevel->base);
	toplevel->base->data = window->surface_tree; /* for child popups */
	honey_window_finish_setup(window);

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
	window->set_parent.notify = window_parent_changed;
	wl_signal_add(&toplevel->events.set_parent, &window->set_parent);
	window->request_fullscreen.notify = window_request_fullscreen;
	wl_signal_add(&toplevel->events.request_fullscreen,
			&window->request_fullscreen);
	window->request_maximize.notify = window_request_maximize;
	wl_signal_add(&toplevel->events.request_maximize,
			&window->request_maximize);
}

/* -------------------------------------------------------------------- setup */

void honey_window_setup (struct honey_server *server) {
	wl_list_init(&server->windows);
	server->foreign_toplevel_manager =
		wlr_foreign_toplevel_manager_v1_create(server->display);
	server->xdg_shell = wlr_xdg_shell_create(server->display, 3);
	server->new_xdg_toplevel.notify = new_xdg_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel,
			&server->new_xdg_toplevel);
	server->new_xdg_popup.notify = new_xdg_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup,
			&server->new_xdg_popup);
}
