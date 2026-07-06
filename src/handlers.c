/* Extra protocol handlers that need a little compositor logic.
 *
 *   - cursor-shape: apps request named cursors (resize arrows, text bar, ...).
 *   - virtual keyboard/pointer: injected input (wtype, ydotool, wayvnc).
 *   - xdg-activation: raise/focus a surface that requested activation.
 *   - idle-inhibit: suppress idle while an inhibitor exists (video playback).
 *   - keyboard-shortcuts-inhibit: let a focused app (VM, remote desktop) grab
 *     the compositor's keybinds while active.
 */
#include <stdlib.h>

#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>

#include "w3ld.h"

/* --------------------------------------------------------------- cursor-shape */

static void handle_cursor_shape (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, request_cursor_shape);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (server->seat->pointer_state.focused_client == event->seat_client) {
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
				wlr_cursor_shape_v1_name(event->shape));
	}
}

/* ------------------------------------------------------------- virtual input */

static void handle_new_virtual_keyboard (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *keyboard = data;
	w3ld_seat_new_keyboard(server, &keyboard->keyboard.base);
}

static void handle_new_virtual_pointer (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	wlr_cursor_attach_input_device(server->cursor,
			&event->new_pointer->pointer.base);
}

/* -------------------------------------------------------------- xdg-activation */

static void handle_request_activate (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, request_activate);
	struct wlr_xdg_activation_v1_request_activate_event *event = data;

	struct w3ld_window *window;
	wl_list_for_each(window, &server->windows, link) {
		if (window->mapped
				&& window->xdg_toplevel->base->surface == event->surface) {
			w3ld_focus_window(window);
			return;
		}
	}
}

/* -------------------------------------------------------------- idle-inhibit */

static void update_idle_inhibit (struct w3ld_server *server) {
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier,
			!wl_list_empty(&server->idle_inhibitors));
}

static void idle_inhibitor_destroy (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_idle_inhibitor *inhibitor =
		wl_container_of(listener, inhibitor, destroy);
	struct w3ld_server *server = inhibitor->server;
	wl_list_remove(&inhibitor->destroy.link);
	wl_list_remove(&inhibitor->link);
	free(inhibitor);
	update_idle_inhibit(server);
}

static void handle_new_idle_inhibitor (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, new_idle_inhibitor);
	struct wlr_idle_inhibitor_v1 *wlr_inhibitor = data;

	struct w3ld_idle_inhibitor *inhibitor = calloc(1, sizeof *inhibitor);
	inhibitor->server = server;
	inhibitor->destroy.notify = idle_inhibitor_destroy;
	wl_signal_add(&wlr_inhibitor->events.destroy, &inhibitor->destroy);
	wl_list_insert(&server->idle_inhibitors, &inhibitor->link);
	update_idle_inhibit(server);
}

/* --------------------------------------------------------- pointer constraints */

static void constraint_destroy (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, constraint_destroy);
	wl_list_remove(&server->constraint_destroy.link);
	wl_list_init(&server->constraint_destroy.link);
	server->active_constraint = NULL;
}

/* Activate the constraint matching the pointer-focused surface (if any),
 * deactivating the previous one. */
void w3ld_constraint_check (
	struct w3ld_server *server,
	struct wlr_surface *surface
) {
	struct wlr_pointer_constraint_v1 *constraint = surface
		? wlr_pointer_constraints_v1_constraint_for_surface(
				server->pointer_constraints, surface, server->seat)
		: NULL;
	if (constraint == server->active_constraint)
		return;

	if (server->active_constraint) {
		wlr_pointer_constraint_v1_send_deactivated(server->active_constraint);
		wl_list_remove(&server->constraint_destroy.link);
		wl_list_init(&server->constraint_destroy.link);
	}
	server->active_constraint = constraint;
	if (constraint) {
		wlr_pointer_constraint_v1_send_activated(constraint);
		server->constraint_destroy.notify = constraint_destroy;
		wl_signal_add(&constraint->events.destroy,
				&server->constraint_destroy);
	}
}

static void handle_new_constraint (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, new_constraint);
	struct wlr_pointer_constraint_v1 *constraint = data;
	if (server->seat->pointer_state.focused_surface == constraint->surface)
		w3ld_constraint_check(server, constraint->surface);
}

/* ------------------------------------------------- keyboard-shortcuts-inhibit */

bool w3ld_shortcuts_inhibited (struct w3ld_server *server) {
	if (!server->focused)
		return false;
	struct wlr_surface *surface = server->focused->xdg_toplevel->base->surface;
	struct w3ld_shortcuts_inhibitor *shortcuts;
	wl_list_for_each(shortcuts, &server->shortcuts_inhibitors, link) {
		if (shortcuts->inhibitor->active
				&& shortcuts->inhibitor->surface == surface)
			return true;
	}
	return false;
}

static void shortcuts_inhibitor_destroy (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_shortcuts_inhibitor *shortcuts =
		wl_container_of(listener, shortcuts, destroy);
	wl_list_remove(&shortcuts->destroy.link);
	wl_list_remove(&shortcuts->link);
	free(shortcuts);
}

static void handle_new_shortcuts_inhibitor (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, new_shortcuts_inhibitor);
	struct wlr_keyboard_shortcuts_inhibitor_v1 *wlr_inhibitor = data;
	wlr_keyboard_shortcuts_inhibitor_v1_activate(wlr_inhibitor);

	struct w3ld_shortcuts_inhibitor *shortcuts = calloc(1, sizeof *shortcuts);
	shortcuts->inhibitor = wlr_inhibitor;
	shortcuts->destroy.notify = shortcuts_inhibitor_destroy;
	wl_signal_add(&wlr_inhibitor->events.destroy, &shortcuts->destroy);
	wl_list_insert(&server->shortcuts_inhibitors, &shortcuts->link);
}

/* -------------------------------------------------------------------- setup */

void w3ld_handlers_setup (struct w3ld_server *server) {
	wl_list_init(&server->idle_inhibitors);
	wl_list_init(&server->shortcuts_inhibitors);
	wl_list_init(&server->constraint_destroy.link);

	server->tearing_control =
		wlr_tearing_control_manager_v1_create(server->display, 1);
	server->relative_pointer_manager =
		wlr_relative_pointer_manager_v1_create(server->display);
	server->pointer_constraints =
		wlr_pointer_constraints_v1_create(server->display);
	server->new_constraint.notify = handle_new_constraint;
	wl_signal_add(&server->pointer_constraints->events.new_constraint,
			&server->new_constraint);

	struct wlr_cursor_shape_manager_v1 *cursor_shape =
		wlr_cursor_shape_manager_v1_create(server->display, 1);
	server->request_cursor_shape.notify = handle_cursor_shape;
	wl_signal_add(&cursor_shape->events.request_set_shape,
			&server->request_cursor_shape);

	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard =
		wlr_virtual_keyboard_manager_v1_create(server->display);
	server->new_virtual_keyboard.notify = handle_new_virtual_keyboard;
	wl_signal_add(&virtual_keyboard->events.new_virtual_keyboard,
			&server->new_virtual_keyboard);

	struct wlr_virtual_pointer_manager_v1 *virtual_pointer =
		wlr_virtual_pointer_manager_v1_create(server->display);
	server->new_virtual_pointer.notify = handle_new_virtual_pointer;
	wl_signal_add(&virtual_pointer->events.new_virtual_pointer,
			&server->new_virtual_pointer);

	struct wlr_xdg_activation_v1 *activation =
		wlr_xdg_activation_v1_create(server->display);
	server->request_activate.notify = handle_request_activate;
	wl_signal_add(&activation->events.request_activate,
			&server->request_activate);

	server->idle_notifier = wlr_idle_notifier_v1_create(server->display);
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit =
		wlr_idle_inhibit_v1_create(server->display);
	server->new_idle_inhibitor.notify = handle_new_idle_inhibitor;
	wl_signal_add(&idle_inhibit->events.new_inhibitor,
			&server->new_idle_inhibitor);

	struct wlr_keyboard_shortcuts_inhibit_manager_v1 *shortcuts_inhibit =
		wlr_keyboard_shortcuts_inhibit_v1_create(server->display);
	server->new_shortcuts_inhibitor.notify = handle_new_shortcuts_inhibitor;
	wl_signal_add(&shortcuts_inhibit->events.new_inhibitor,
			&server->new_shortcuts_inhibitor);
}
