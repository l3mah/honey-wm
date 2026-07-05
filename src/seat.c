/* Seat: wlr_seat, cursor, keyboards, and the input event handlers.
 *
 * Keyboards build a default xkb keymap and feed the seat. A small hardcoded
 * keybinding table drives M1 (spawn / close / focus / exit); dynamic bindings
 * arrive with the IPC layer in M3. The pointer moves the cursor and forwards
 * enter/motion/button/axis to the surface under it.
 */
#include <stdlib.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon.h>

#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>

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
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
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

static void new_keyboard (
	struct w3ld_server *server,
	struct wlr_input_device *device
) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
	struct w3ld_keyboard *keyboard = calloc(1, sizeof *keyboard);
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap =
		xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

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
	if (!server->mouse_follows_focus)
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

	if (server->follow_mouse) {
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
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
	} else {
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
				"default");
		wlr_seat_pointer_notify_clear_focus(server->seat);
	}
}

static void cursor_motion (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
			event->delta_x, event->delta_y);
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
	pointer_focus(server, event->time_msec);
}

static void cursor_button (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	wlr_seat_pointer_notify_button(server->seat, event->time_msec,
			event->button, event->state);
}

static void cursor_axis (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
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
		new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		wlr_cursor_attach_input_device(server->cursor, device);
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
	server->xcursor_manager = wlr_xcursor_manager_create(NULL, 24);

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
