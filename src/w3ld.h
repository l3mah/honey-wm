/* w3ld — a tiling Wayland compositor on wlroots 0.20.
 *
 * Shared state and declarations.
 */
#ifndef W3LD_H
#define W3LD_H

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>

struct w3ld_workspace;

/* ------------------------------------------------------------------- server */

struct w3ld_server {
	struct wl_display *display;
	struct wl_event_loop *event_loop;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_scene *scene;
	struct wlr_output_layout *output_layout;
	struct wlr_scene_output_layout *scene_layout;

	struct wl_list outputs; /* w3ld_output.link */
	struct wl_listener new_output;
	struct w3ld_output *focused_output;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_list windows; /* w3ld_window.link — tiling/stack order */
	struct w3ld_window *focused;

	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_manager;
	struct wl_list keyboards; /* w3ld_keyboard.link */
	struct wl_listener new_input;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
};

/* ------------------------------------------------------------------- output */

struct w3ld_output {
	struct wl_list link;
	struct w3ld_server *server;
	struct wlr_output *wlr_output;
	struct wlr_box usable; /* layout geometry; layer-shell reserves in M3 */

	struct wl_list workspaces; /* w3ld_workspace.link, sorted by number */
	struct w3ld_workspace *active;
	int previous_number; /* for workspace-back */

	struct wl_listener frame;
	struct wl_listener destroy;
};

/* ---------------------------------------------------------------- workspace */

struct w3ld_workspace {
	struct wl_list link; /* w3ld_output.workspaces */
	struct w3ld_output *output;
	int number;
};

/* ------------------------------------------------------------------- window */

struct w3ld_window {
	struct wl_list link;
	struct w3ld_server *server;
	struct w3ld_workspace *workspace;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	struct wlr_box geom; /* current tiled geometry */
	bool mapped;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
};

/* ----------------------------------------------------------------- keyboard */

struct w3ld_keyboard {
	struct wl_list link;
	struct w3ld_server *server;
	struct wlr_keyboard *wlr_keyboard;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* ------------------------------------------------------------------- logging */

void w3ld_log (const char *format, ...);
void w3ld_dbg (const char *format, ...);
#define LOG(...) w3ld_log(__VA_ARGS__)
#define DBG(...) w3ld_dbg(__VA_ARGS__)

/* ------------------------------------------------------------------- modules */

void w3ld_output_setup (struct w3ld_server *server);
void w3ld_window_setup (struct w3ld_server *server);
void w3ld_seat_setup (struct w3ld_server *server);

void w3ld_arrange (struct w3ld_server *server);
void w3ld_spawn (const char *command);

/* focus */
void w3ld_focus_window (struct w3ld_window *window);
void w3ld_focus_output_active (struct w3ld_output *output);

/* workspaces */
struct w3ld_workspace *w3ld_workspace_get (
	struct w3ld_output *output,
	int number
);
struct w3ld_window *w3ld_workspace_first_window (struct w3ld_workspace *workspace);
struct w3ld_output *w3ld_output_adjacent (
	struct w3ld_output *from,
	int direction
);

/* actions (called by keybinds now, by IPC in M3) */
void w3ld_action_close (struct w3ld_server *server);
void w3ld_action_focus (
	struct w3ld_server *server,
	int direction
);
void w3ld_action_workspace (
	struct w3ld_server *server,
	int number
);
void w3ld_action_move_to_workspace (
	struct w3ld_server *server,
	int number
);
void w3ld_action_workspace_back (struct w3ld_server *server);
void w3ld_action_workspace_cycle (
	struct w3ld_server *server,
	int direction
);
void w3ld_action_focus_output (
	struct w3ld_server *server,
	int direction
);
void w3ld_action_move_to_output (
	struct w3ld_server *server,
	int direction
);

/* Stubs filled in later milestones. */
void w3ld_input_setup (struct w3ld_server *server);
void w3ld_decoration_setup (struct w3ld_server *server);
void w3ld_layer_setup (struct w3ld_server *server);
void w3ld_xwayland_setup (struct w3ld_server *server);

#endif
