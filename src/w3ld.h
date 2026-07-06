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
#include <xkbcommon/xkbcommon.h>

struct w3ld_workspace;
struct w3ld_layer_surface;
struct wlr_xwayland_surface;

enum w3ld_direction {
	W3LD_DIR_LEFT,
	W3LD_DIR_RIGHT,
	W3LD_DIR_UP,
	W3LD_DIR_DOWN,
};

enum w3ld_orientation {
	W3LD_ORIENT_LEFT,   /* master column on the left, stack on the right */
	W3LD_ORIENT_RIGHT,
	W3LD_ORIENT_TOP,    /* master row on top, stack below */
	W3LD_ORIENT_BOTTOM,
};

/* Scene-graph z-order: children of the scene root, lowest first. */
enum w3ld_scene_layer {
	W3LD_LAYER_BACKGROUND,
	W3LD_LAYER_BOTTOM,
	W3LD_LAYER_TILED,
	W3LD_LAYER_FLOATING,
	W3LD_LAYER_TOP,
	W3LD_LAYER_OVERLAY,
	W3LD_NUM_LAYERS,
};

/* ------------------------------------------------------------------- config */

struct w3ld_config {
	/* master layout */
	double master_mfact;
	int master_nmaster;
	enum w3ld_orientation master_orientation;
	/* gaps (global) */
	int gaps_in;
	int gaps_out;
	bool smart_gaps;
	/* appearance */
	int border_size;
	uint32_t border_color_active;   /* 0xRRGGBBAA */
	uint32_t border_color_inactive;
	double float_width;
	double float_height;
	/* behavior */
	bool follow_mouse;
	bool mouse_follows_focus;
	bool new_window_master;
	bool focus_new;
	bool mouse_focus_new;
	bool exit_fullscreen_on_new;
	bool allow_tearing;
};

/* ------------------------------------------------------------------- server */

struct w3ld_server {
	struct wl_display *display;
	struct wl_event_loop *event_loop;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_compositor *compositor;
	struct wlr_xwayland *xwayland;
	struct wl_listener xwayland_ready;
	struct wl_listener new_xwayland_surface;

	struct wlr_scene *scene;
	struct wlr_scene_tree *layers[W3LD_NUM_LAYERS];
	struct wlr_output_layout *output_layout;
	struct wlr_scene_output_layout *scene_layout;

	struct wl_listener new_layer_surface;
	struct w3ld_layer_surface *focused_layer;

	struct wl_list outputs; /* w3ld_output.link */
	struct wl_listener new_output;
	struct w3ld_output *focused_output;

	struct wlr_output_manager_v1 *output_manager;
	struct wl_listener output_manager_apply;
	struct wl_listener output_manager_test;
	struct wl_listener output_layout_change;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_toplevel_decoration;
	struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;
	struct wlr_ext_workspace_manager_v1 *ext_workspace_manager;
	struct wl_listener ext_workspace_commit;
	struct wl_list windows; /* w3ld_window.link — tiling/stack order */
	struct w3ld_window *focused;

	struct w3ld_config config;
	struct wlr_color_transform *gamma_transform; /* night-light LUT, or NULL */

	struct wl_list keybinds; /* w3ld_keybind.link */

	int ipc_fd; /* listening control socket, or -1 */
	struct wl_event_source *ipc_source;
	char ipc_path[108]; /* sun_path max */
	struct wl_list ipc_clients; /* w3ld_ipc_client.link */

	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_manager;
	struct wl_list keyboards; /* w3ld_keyboard.link */
	struct wl_listener new_input;

	/* keyboard config (RMLVO; NULL = xkb default) + repeat */
	char *kb_layout, *kb_variant, *kb_model, *kb_options, *kb_rules;
	int kb_repeat_rate, kb_repeat_delay;
	struct wl_list input_rules;   /* w3ld_input_rule.link */
	struct wl_list input_devices; /* w3ld_input_device.link (libinput config) */
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;

	/* extra protocol handlers */
	struct wlr_tearing_control_manager_v1 *tearing_control;
	struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;
	struct wlr_pointer_constraints_v1 *pointer_constraints;
	struct wlr_pointer_constraint_v1 *active_constraint;
	struct wl_listener new_constraint;
	struct wl_listener constraint_destroy;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wl_list idle_inhibitors;      /* w3ld_idle_inhibitor.link */
	struct wl_list shortcuts_inhibitors; /* w3ld_shortcuts_inhibitor.link */
	struct wl_listener request_cursor_shape;
	struct wl_listener new_virtual_keyboard;
	struct wl_listener new_virtual_pointer;
	struct wl_listener request_activate;
	struct wl_listener new_idle_inhibitor;
	struct wl_listener new_shortcuts_inhibitor;
};

struct w3ld_idle_inhibitor {
	struct wl_list link; /* w3ld_server.idle_inhibitors */
	struct w3ld_server *server;
	struct wl_listener destroy;
};

struct w3ld_shortcuts_inhibitor {
	struct wl_list link; /* w3ld_server.shortcuts_inhibitors */
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
	struct wl_listener destroy;
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

	struct wl_list layer_surfaces; /* w3ld_layer_surface.link */
	struct wlr_ext_workspace_group_handle_v1 *ext_group;

	char *status_workspaces; /* last broadcast line, for diffing */
	char *status_window;

	struct wl_listener frame;
	struct wl_listener destroy;
};

/* ---------------------------------------------------------------- workspace */

struct w3ld_workspace {
	struct wl_list link; /* w3ld_output.workspaces */
	struct w3ld_output *output;
	int number;
	char *name; /* optional label, or NULL */
	struct wlr_ext_workspace_handle_v1 *ext; /* ext-workspace handle, or NULL */
};

/* ------------------------------------------------------------------- window */

enum w3ld_window_type {
	W3LD_WINDOW_XDG,
	W3LD_WINDOW_X11,
};

struct w3ld_window {
	struct wl_list link;
	struct w3ld_server *server;
	struct w3ld_workspace *workspace;
	enum w3ld_window_type type;
	struct wlr_xdg_toplevel *xdg_toplevel;         /* XDG windows */
	struct wlr_xwayland_surface *xwayland_surface; /* X11 windows */
	struct wlr_scene_tree *tree;         /* parent, positioned at the tile origin */
	struct wlr_scene_tree *surface_tree; /* xdg surface, inset by the border */
	struct wlr_scene_rect *border[4];    /* top, bottom, left, right */
	struct wlr_box geom; /* current tiled geometry */
	bool mapped;

	struct wlr_foreign_toplevel_handle_v1 *foreign; /* taskbar handle, or NULL */

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
	struct wl_listener foreign_activate;
	struct wl_listener foreign_close;
};

/* ------------------------------------------------------------------ keybind */

struct w3ld_keybind {
	struct wl_list link; /* w3ld_server.keybinds */
	uint32_t modifiers;
	xkb_keysym_t sym;
	char *action; /* owned */
};

struct w3ld_ipc_client {
	struct wl_list link; /* w3ld_server.ipc_clients */
	struct w3ld_server *server;
	int fd;
	struct wl_event_source *source;
	bool subscriber; /* receives the status stream instead of one reply */
};

/* -------------------------------------------------------------- layer surface */

struct w3ld_layer_surface {
	struct wl_list link; /* w3ld_output.layer_surfaces */
	struct w3ld_server *server;
	struct w3ld_output *output;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
};

/* --------------------------------------------------------------------- input */

struct w3ld_input_rule {
	struct wl_list link; /* w3ld_server.input_rules */
	char *device; /* substring match against device name, or "*" */
	char *option;
	char *value;
};

struct w3ld_input_device {
	struct wl_list link; /* w3ld_server.input_devices */
	struct w3ld_server *server;
	struct wlr_input_device *device;
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
void w3ld_output_manager_setup (struct w3ld_server *server);
bool w3ld_output_command (
	struct w3ld_server *server,
	char *args,
	char *error,
	size_t error_size
);
void w3ld_window_setup (struct w3ld_server *server);
void w3ld_seat_setup (struct w3ld_server *server);
void w3ld_seat_new_keyboard (
	struct w3ld_server *server,
	struct wlr_input_device *device
);

/* extra protocol handlers (cursor-shape, virtual input, activation, idle/
 * shortcuts inhibit) */
void w3ld_handlers_setup (struct w3ld_server *server);
bool w3ld_shortcuts_inhibited (struct w3ld_server *server);
void w3ld_constraint_check (
	struct w3ld_server *server,
	struct wlr_surface *surface
);

void w3ld_arrange (struct w3ld_server *server);
void w3ld_spawn (const char *command);

/* type-agnostic window accessors (XDG / X11) */
struct wlr_surface *w3ld_window_surface (struct w3ld_window *window);
const char *w3ld_window_title (struct w3ld_window *window);
const char *w3ld_window_app_id (struct w3ld_window *window);
void w3ld_window_configure (
	struct w3ld_window *window,
	int x,
	int y,
	int width,
	int height
);
void w3ld_window_set_activated (
	struct w3ld_window *window,
	bool activated
);
void w3ld_window_close (struct w3ld_window *window);

/* shared window lifecycle (called by the XDG and X11 paths) */
void w3ld_window_finish_setup (struct w3ld_window *window);
void w3ld_window_handle_map (struct w3ld_window *window);
void w3ld_window_handle_unmap (struct w3ld_window *window);

/* config */
void w3ld_config_defaults (struct w3ld_config *config);
bool w3ld_config_set (
	struct w3ld_server *server,
	const char *key,
	const char *value
);

/* bindings + IPC */
void w3ld_binding_setup (struct w3ld_server *server);
bool w3ld_binding_add (
	struct w3ld_server *server,
	const char *combo,
	const char *action
);
bool w3ld_binding_remove (
	struct w3ld_server *server,
	const char *combo
);
bool w3ld_binding_run (
	struct w3ld_server *server,
	uint32_t modifiers,
	xkb_keysym_t sym
);
void w3ld_action_run (
	struct w3ld_server *server,
	const char *action
);
void w3ld_ipc_setup (struct w3ld_server *server);
void w3ld_config_run (struct w3ld_server *server);

/* focus */
void w3ld_focus_window (struct w3ld_window *window);
void w3ld_focus_output_active (struct w3ld_output *output);

/* workspaces */
struct w3ld_workspace *w3ld_workspace_get (
	struct w3ld_output *output,
	int number
);
struct w3ld_window *w3ld_workspace_first_window (struct w3ld_workspace *workspace);
struct w3ld_output *w3ld_output_in_direction (
	struct w3ld_output *from,
	enum w3ld_direction direction
);
struct w3ld_output *w3ld_output_at (
	struct w3ld_server *server,
	double x,
	double y
);
void w3ld_warp_to_focus (struct w3ld_server *server);

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
void w3ld_action_focus_dir (
	struct w3ld_server *server,
	enum w3ld_direction direction
);
void w3ld_action_move_to_output (
	struct w3ld_server *server,
	enum w3ld_direction direction
);

/* layer shell */
void w3ld_layer_setup (struct w3ld_server *server);
void w3ld_layer_arrange (struct w3ld_output *output);

/* good-citizen protocol managers */
void w3ld_protocols_setup (struct w3ld_server *server);

/* gamma / night-light */
void w3ld_gamma_setup (struct w3ld_server *server);
void w3ld_gamma_set (
	struct w3ld_server *server,
	double temperature,
	double brightness
);

/* ext-workspace protocol */
void w3ld_ext_workspace_setup (struct w3ld_server *server);
void w3ld_ext_workspace_sync (struct w3ld_server *server);

/* actions helper shared with protocol handlers */
void w3ld_switch_workspace (
	struct w3ld_output *output,
	int number
);

/* status stream */
void w3ld_status_broadcast (struct w3ld_server *server);
void w3ld_status_snapshot (
	struct w3ld_server *server,
	struct w3ld_ipc_client *client
);

/* input */
void w3ld_input_setup (struct w3ld_server *server);
void w3ld_input_apply_keyboard (
	struct w3ld_server *server,
	struct wlr_keyboard *keyboard
);
void w3ld_input_add_device (
	struct w3ld_server *server,
	struct wlr_input_device *device
);
bool w3ld_kb_layout (
	struct w3ld_server *server,
	const char *layout,
	const char *variant,
	const char *model,
	const char *options,
	const char *rules
);
bool w3ld_kb_repeat (
	struct w3ld_server *server,
	int rate,
	int delay
);
bool w3ld_input_rule_add (
	struct w3ld_server *server,
	const char *device,
	const char *option,
	const char *value
);

/* Stubs filled in later milestones. */
void w3ld_decoration_setup (struct w3ld_server *server);
void w3ld_xwayland_setup (struct w3ld_server *server);

#endif
