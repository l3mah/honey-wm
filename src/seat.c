/* Seat: wlr_seat, cursor, keyboards, and the input event handlers.
 *
 * Keyboards feed the seat and dispatch keybindings (matched at keysym level 0
 * so shifted binds resolve to the base symbol). The pointer moves the cursor,
 * drives focus (follow-mouse, click-to-focus), forwards events to the surface
 * under it, and runs the interactive move/resize grabs: super+drag, border
 * drags (resize-on-border), drop-at-cursor re-tiling, and super+scroll
 * workspace cycling.
 */
#include <stdlib.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon.h>

#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/util/edges.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/xwayland/xwayland.h>

#include "w3ld.h"

/* --------------------------------------------------------------------- spawn */

void w3ld_spawn (const char *command) {
	if (!command || !*command)
		return;
	pid_t pid = fork();
	if (pid < 0) {
		LOG("fork failed for: %s", command);
		return;
	}
	if (pid == 0) {
		setsid();
		execl("/bin/sh", "/bin/sh", "-c", command, (char *)NULL);
		_exit(127);
	}
}

/* ---------------------------------------------------------------- keyboards */

static void keyboard_modifiers (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
			&keyboard->wlr_keyboard->modifiers);
}

static void keyboard_key (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct w3ld_server *server = keyboard->server;
	struct wlr_keyboard *wlr_keyboard = keyboard->wlr_keyboard;
	struct wlr_keyboard_key_event *event = data;

	uint32_t keycode = event->keycode + 8; /* libinput -> xkb offset */
	uint32_t modifiers = wlr_keyboard_get_modifiers(wlr_keyboard);

	bool handled = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED
			&& !w3ld_shortcuts_inhibited(server)) {
		/* Match at level 0 so shifted binds resolve to the base symbol
		 * (Super+Shift+1 -> 1, not exclam). */
		xkb_layout_index_t layout =
			xkb_state_key_get_layout(wlr_keyboard->xkb_state, keycode);
		const xkb_keysym_t *syms;
		int count = xkb_keymap_key_get_syms_by_level(wlr_keyboard->keymap,
				keycode, layout, 0, &syms);
		for (int i = 0; i < count; i++) {
			if (w3ld_binding_run(server, modifiers, syms[i])) {
				handled = true;
				break;
			}
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(server->seat, wlr_keyboard);
		wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
				event->keycode, event->state);
	}
}

static void update_capabilities (struct w3ld_server *server) {
	uint32_t capabilities = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards))
		capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(server->seat, capabilities);
}

static void keyboard_destroy (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	update_capabilities(keyboard->server);
	free(keyboard);
}

void w3ld_seat_new_keyboard (
	struct w3ld_server *server,
	struct wlr_input_device *device
) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
	struct w3ld_keyboard *keyboard = calloc(1, sizeof *keyboard);
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	w3ld_input_apply_keyboard(server, wlr_keyboard);

	keyboard->modifiers.notify = keyboard_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, wlr_keyboard);
	wl_list_insert(&server->keyboards, &keyboard->link);
}

/* ------------------------------------------------------------------- cursor */

/* Warp the cursor to the focused window (or focused output) center on
 * keyboard-driven focus moves, when mouse-follows-focus is enabled. */
void w3ld_warp_to_focus (struct w3ld_server *server) {
	if (!server->config.mouse_follows_focus)
		return;
	int x, y;
	if (server->focused) {
		x = server->focused->geom.x + server->focused->geom.width / 2;
		y = server->focused->geom.y + server->focused->geom.height / 2;
	} else if (server->focused_output) {
		x = server->focused_output->usable.x
			+ server->focused_output->usable.width / 2;
		y = server->focused_output->usable.y
			+ server->focused_output->usable.height / 2;
	} else {
		return;
	}
	wlr_cursor_warp(server->cursor, NULL, x, y);
	DBG("warp to %d,%d (%s)", x, y,
			server->focused ? w3ld_window_app_id(server->focused) : "output");
}

/* Climb the scene tree to the window that owns a node, or NULL. */
static struct w3ld_window *window_from_node (struct wlr_scene_node *node) {
	while (node) {
		if (node->data)
			return node->data;
		node = node->parent ? &node->parent->node : NULL;
	}
	return NULL;
}

/* ------------------------------------------------------------- interactive op */

/* Held Logo modifier on any keyboard of the seat. */
static bool super_held (struct w3ld_server *server) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	return keyboard
		&& (wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_LOGO);
}

/* Begin a move/resize grab; a tiled window floats in place first. */
static void op_begin (
	struct w3ld_server *server,
	struct w3ld_window *window,
	int op,
	uint32_t edges
) {
	if (window->fullscreen || window->maximized)
		return;

	server->op_was_tiled = w3ld_window_is_tiled(window);
	if (server->op_was_tiled) {
		window->floating = true;
		window->float_geom = window->geom; /* float in place, no jump */
		w3ld_window_update_layer(window);
	}

	server->op = op;
	server->op_window = window;
	server->op_start_x = server->cursor->x;
	server->op_start_y = server->cursor->y;
	server->op_geom = window->float_geom;
	server->op_edges = edges;
	w3ld_focus_window(window);
	wlr_seat_pointer_notify_clear_focus(server->seat);
	wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			op == W3LD_OP_MOVE ? "grabbing" : "se-resize");
}

/* Re-tile a dropped window at the cursor: insert it next to the tiled window
 * under the cursor (or at the tail) on the output the cursor is over. */
static void drop_at_cursor (
	struct w3ld_server *server,
	struct w3ld_window *window
) {
	struct w3ld_output *output = w3ld_output_at(server,
			server->cursor->x, server->cursor->y);
	if (output)
		window->workspace = output->active;

	struct w3ld_window *target = NULL;
	struct w3ld_window *other;
	wl_list_for_each(other, &server->windows, link) {
		if (other == window || !other->mapped
				|| other->workspace != window->workspace
				|| !w3ld_window_is_tiled(other))
			continue;
		if (server->cursor->x >= other->geom.x
				&& server->cursor->x < other->geom.x + other->geom.width
				&& server->cursor->y >= other->geom.y
				&& server->cursor->y < other->geom.y + other->geom.height) {
			target = other;
			break;
		}
	}

	window->floating = false;
	w3ld_window_update_layer(window);
	wl_list_remove(&window->link);
	if (target) {
		/* before the target when the cursor is in its first half */
		bool before = server->cursor->y
			< target->geom.y + target->geom.height / 2;
		wl_list_insert(before ? target->link.prev : &target->link,
				&window->link);
	} else {
		wl_list_insert(server->windows.prev, &window->link);
	}
}

static void op_end (struct w3ld_server *server) {
	struct w3ld_window *window = server->op_window;
	bool was_move = server->op == W3LD_OP_MOVE;
	server->op = W3LD_OP_NONE;
	server->op_window = NULL;

	if (window && was_move && server->op_was_tiled
			&& server->config.drop_at_cursor)
		drop_at_cursor(server, window);

	wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, "default");
	w3ld_arrange(server);
}

static void op_motion (struct w3ld_server *server) {
	struct w3ld_window *window = server->op_window;
	int dx = (int)(server->cursor->x - server->op_start_x);
	int dy = (int)(server->cursor->y - server->op_start_y);

	if (server->op == W3LD_OP_MOVE) {
		window->float_geom.x = server->op_geom.x + dx;
		window->float_geom.y = server->op_geom.y + dy;
	} else {
		struct wlr_box geom = server->op_geom;
		if (server->op_edges & WLR_EDGE_RIGHT)
			geom.width += dx;
		if (server->op_edges & WLR_EDGE_BOTTOM)
			geom.height += dy;
		if (server->op_edges & WLR_EDGE_LEFT) {
			geom.x += dx;
			geom.width -= dx;
		}
		if (server->op_edges & WLR_EDGE_TOP) {
			geom.y += dy;
			geom.height -= dy;
		}
		if (geom.width < 50)
			geom.width = 50;
		if (geom.height < 50)
			geom.height = 50;
		window->float_geom = geom;
	}
	w3ld_arrange(server);
}

/* When a border rect is grabbed, which edge does it resize? */
static uint32_t border_edge (
	struct w3ld_window *window,
	struct wlr_scene_node *node
) {
	if (node == &window->border[0]->node)
		return WLR_EDGE_TOP;
	if (node == &window->border[1]->node)
		return WLR_EDGE_BOTTOM;
	if (node == &window->border[2]->node)
		return WLR_EDGE_LEFT;
	if (node == &window->border[3]->node)
		return WLR_EDGE_RIGHT;
	return 0;
}

/* Resize edges from the grab point's quadrant within the window. */
static uint32_t quadrant_edges (
	struct w3ld_server *server,
	struct w3ld_window *window
) {
	struct wlr_box *geom = window->floating ? &window->float_geom
		: &window->geom;
	uint32_t edges = 0;
	edges |= server->cursor->x < geom->x + geom->width / 2
		? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
	edges |= server->cursor->y < geom->y + geom->height / 2
		? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
	return edges;
}

/* Forward pointer focus to the surface under the cursor; with follow-mouse on,
 * also move keyboard focus to the window (or output) under the cursor. */
static void pointer_focus (
	struct w3ld_server *server,
	uint32_t time_msec
) {
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct w3ld_window *window = NULL;
	struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node,
			server->cursor->x, server->cursor->y, &sx, &sy);
	if (node && node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *scene_surface =
			wlr_scene_surface_try_from_buffer(buffer);
		if (scene_surface)
			surface = scene_surface->surface;
		window = window_from_node(node);
	}

	if (server->config.follow_mouse) {
		if (window) {
			w3ld_focus_window(window);
		} else {
			struct w3ld_output *output = w3ld_output_at(server,
					server->cursor->x, server->cursor->y);
			if (output)
				server->focused_output = output;
		}
	}

	if (surface) {
		/* xwayland-scale (removable): X11 surfaces live in the scaled xwayland
		 * coordinate space; the scene reports logical coords, Xwayland expects
		 * its own. Remove this block to revert. */
		if (wlr_xwayland_surface_try_from_wlr_surface(surface)) {
			double scale = w3ld_xwayland_scale_at(server, server->cursor->x,
					server->cursor->y);
			sx *= scale;
			sy *= scale;
		}
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
	} else {
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
				"default");
		wlr_seat_pointer_notify_clear_focus(server->seat);
	}
	w3ld_constraint_check(server, surface);
}

static void cursor_motion (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;

	wlr_relative_pointer_manager_v1_send_relative_motion(
			server->relative_pointer_manager, server->seat,
			(uint64_t)event->time_msec * 1000, event->delta_x, event->delta_y,
			event->unaccel_dx, event->unaccel_dy);

	/* A locked constraint pins the cursor: relative motion only. */
	if (server->active_constraint
			&& server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
		return;

	wlr_cursor_move(server->cursor, &event->pointer->base,
			event->delta_x, event->delta_y);
	if (server->op != W3LD_OP_NONE) {
		op_motion(server);
		return;
	}
	pointer_focus(server, event->time_msec);
}

static void cursor_motion_absolute (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
			event->x, event->y);
	if (server->op != W3LD_OP_NONE) {
		op_motion(server);
		return;
	}
	pointer_focus(server, event->time_msec);
}

static void cursor_button (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (server->op != W3LD_OP_NONE) {
			op_end(server);
			return;
		}
		wlr_seat_pointer_notify_button(server->seat, event->time_msec,
				event->button, event->state);
		return;
	}

	double sx, sy;
	struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node,
			server->cursor->x, server->cursor->y, &sx, &sy);
	struct w3ld_window *window = node ? window_from_node(node) : NULL;

	if (window && super_held(server)) {
		if (event->button == BTN_LEFT) {
			op_begin(server, window, W3LD_OP_MOVE, 0);
			return;
		}
		if (event->button == BTN_RIGHT) {
			op_begin(server, window, W3LD_OP_RESIZE,
					quadrant_edges(server, window));
			return;
		}
	}

	/* Dragging a border rect resizes without a modifier. */
	if (window && server->config.resize_on_border
			&& event->button == BTN_LEFT
			&& node->type == WLR_SCENE_NODE_RECT) {
		uint32_t edge = border_edge(window, node);
		if (edge) {
			op_begin(server, window, W3LD_OP_RESIZE, edge);
			return;
		}
	}

	if (window)
		w3ld_focus_window(window); /* click-to-focus */
	wlr_seat_pointer_notify_button(server->seat, event->time_msec,
			event->button, event->state);
}

static void cursor_axis (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;

	/* super+scroll cycles the focused output's workspaces. */
	if (server->config.scroll_workspace && super_held(server)
			&& event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL
			&& event->delta != 0) {
		w3ld_action_workspace_cycle(server, event->delta > 0 ? +1 : -1);
		return;
	}

	wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
			event->orientation, event->delta, event->delta_discrete,
			event->source, event->relative_direction);
}

static void cursor_frame (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

/* -------------------------------------------------------------------- input */

static void new_input (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		w3ld_seat_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		wlr_cursor_attach_input_device(server->cursor, device);
		w3ld_input_add_device(server, device);
		break;
	default:
		break;
	}
	update_capabilities(server);
}

/* --------------------------------------------------------------- seat events */

static void request_cursor (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	if (server->seat->pointer_state.focused_client == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
	}
}

static void request_set_selection (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

/* -------------------------------------------------------------------- setup */

void w3ld_seat_setup (struct w3ld_server *server) {
	wl_list_init(&server->keyboards);

	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
	server->cursor_size = 24;
	server->xcursor_manager = wlr_xcursor_manager_create(NULL,
			server->cursor_size);

	server->cursor_motion.notify = cursor_motion;
	wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
	server->cursor_motion_absolute.notify = cursor_motion_absolute;
	wl_signal_add(&server->cursor->events.motion_absolute,
			&server->cursor_motion_absolute);
	server->cursor_button.notify = cursor_button;
	wl_signal_add(&server->cursor->events.button, &server->cursor_button);
	server->cursor_axis.notify = cursor_axis;
	wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
	server->cursor_frame.notify = cursor_frame;
	wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

	server->new_input.notify = new_input;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);

	server->seat = wlr_seat_create(server->display, "seat0");
	server->request_cursor.notify = request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor,
			&server->request_cursor);
	server->request_set_selection.notify = request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection,
			&server->request_set_selection);
}
