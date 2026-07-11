/* honey — a tiling Wayland compositor on wlroots 0.20.
 *
 * Shared state and declarations.
 */
#ifndef HONEY_H
#define HONEY_H

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
#include <regex.h>
#include <xkbcommon/xkbcommon.h>

struct honey_workspace;
struct honey_layer_surface;
struct wlr_xwayland_surface;

enum honey_direction {
	HONEY_DIR_LEFT,
	HONEY_DIR_RIGHT,
	HONEY_DIR_UP,
	HONEY_DIR_DOWN,
};

enum honey_orientation {
	HONEY_ORIENT_LEFT,   /* master column on the left, stack on the right */
	HONEY_ORIENT_RIGHT,
	HONEY_ORIENT_TOP,    /* master row on top, stack below */
	HONEY_ORIENT_BOTTOM,
};

/* Scene-graph z-order: children of the scene root, lowest first. */
enum honey_scene_layer {
	HONEY_LAYER_BACKGROUND,
	HONEY_LAYER_BOTTOM,
	HONEY_LAYER_TILED,      /* tiled + maximized + fake-fullscreen windows */
	HONEY_LAYER_FLOATING,   /* floats: above tiled/maximized, below bars */
	HONEY_LAYER_TOP,        /* bars */
	HONEY_LAYER_FULLSCREEN, /* exclusive fullscreen: covers bars */
	HONEY_LAYER_OVERLAY,    /* lock screens, notifications */
	HONEY_NUM_LAYERS,
};

/* ------------------------------------------------------------------- layouts */

/* A layout is a pure function of the tiled-window array and its parameters.
 * Adding a layout = one arrange() plus a registry entry in layout.c. */
struct honey_layout_ctx {
	struct honey_window **windows;
	int count;
	struct wlr_box area; /* usable area minus outer gaps */
	int gap;             /* inner gap between windows */
	int border;
	/* master params (effective: workspace override or global) */
	double mfact;
	int nmaster;
	enum honey_orientation orientation;
	/* spiral / grid params */
	double spiral_ratio;
	bool spiral_horizontal;
	int grid_columns; /* 0 = auto */
};

struct honey_layout {
	const char *name;
	void (*arrange) (struct honey_layout_ctx *ctx);
};

const struct honey_layout *honey_layout_by_name (const char *name);

/* ------------------------------------------------------------------- config */

struct honey_config {
	/* layout */
	const struct honey_layout *layout; /* global default */
	double master_mfact;
	int master_nmaster;
	enum honey_orientation master_orientation;
	double spiral_ratio;
	bool spiral_horizontal;
	int grid_columns;
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
	bool float_app_size; /* floats adopt the app's own preferred size */
	/* behavior */
	bool follow_mouse;
	bool mouse_follows_focus;
	bool warp_on_workspace_switch; /* mouse-follows-focus also warps on ws switch */
	bool new_window_master;
	bool focus_new;
	bool mouse_focus_new;
	bool exit_fullscreen_on_new;
	bool allow_tearing;
	bool drop_at_cursor;   /* dropping a dragged tiled window re-tiles it there */
	bool resize_on_border; /* dragging a border resizes without a modifier */
	bool scroll_workspace; /* super+scroll cycles workspaces */
	double active_opacity;
	double inactive_opacity;
	double dim_inactive; /* 0 = off, else dim strength 0..1 */
	bool error_window;   /* show config errors in a floating terminal */
	bool suspend_hidden; /* tell windows on hidden workspaces to throttle */
	double xwayland_scale; /* 0 = off, else the X11 render scale */
	bool xwayland_scale_auto; /* follow the highest output scale */
};

/* ------------------------------------------------------------------- server */

struct honey_server {
	struct wl_display *display;
	struct wl_event_loop *event_loop;
	struct wlr_backend *backend;
	struct wlr_session *session; /* NULL when nested or headless */
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_compositor *compositor;
	struct wlr_xwayland *xwayland;
	struct wl_listener xwayland_ready;
	struct wl_listener new_xwayland_surface;

	struct wlr_scene *scene;
	struct wlr_scene_tree *layers[HONEY_NUM_LAYERS];
	struct wlr_output_layout *output_layout;
	struct wlr_scene_output_layout *scene_layout;

	struct wl_listener new_layer_surface;
	struct honey_layer_surface *focused_layer;

	struct wl_list outputs; /* honey_output.link */
	struct wl_listener new_output;
	struct honey_output *focused_output;

	struct wlr_output_manager_v1 *output_manager;
	struct wl_listener output_manager_apply;
	struct wl_listener output_manager_test;
	struct wl_listener output_layout_change;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_listener new_toplevel_decoration;
	struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;
	struct wlr_ext_workspace_manager_v1 *ext_workspace_manager;
	struct wl_listener ext_workspace_commit;
	struct wl_list windows; /* honey_window.link — tiling/stack order */
	struct honey_window *focused;

	struct honey_config config;
	const char *config_path; /* -c override; NULL = default ~/.config/honey/init */
	double gamma_temperature; /* night-light target; 0 = neutral/off */
	double gamma_brightness;
	double gamma_brightness_min; /* clamp floor for brightness ops (0-1) */
	double gamma_brightness_max; /* clamp ceiling for brightness ops (0-1) */

	struct wl_list keybinds;  /* honey_keybind.link */
	struct wl_list rules;     /* honey_rule.link */
	struct wl_list exec_once; /* honey_exec_once.link */

	int ipc_fd; /* listening control socket, or -1 */
	struct wl_event_source *ipc_source;
	char ipc_path[108]; /* sun_path max */
	struct wl_list ipc_clients; /* honey_ipc_client.link */
	struct wl_list xdg_output_resources; /* per-client xdg-output resources */

	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_manager;
	char *cursor_theme; /* NULL = XCURSOR_THEME env / default */
	int cursor_size;
	bool cursor_is_default; /* honey set the default image (not a client's) */
	struct wl_list keyboards; /* honey_keyboard.link */
	struct wl_listener new_input;

	/* keyboard config (RMLVO; NULL = xkb default) + repeat */
	char *kb_layout, *kb_variant, *kb_model, *kb_options, *kb_rules;
	int kb_repeat_rate, kb_repeat_delay;
	struct wl_list input_rules;   /* honey_input_rule.link */
	struct wl_list input_devices; /* honey_input_device.link (libinput config) */
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_listener drag_destroy;
	struct wlr_scene_tree *drag_icon; /* active drag-and-drop icon, or NULL */
	bool dragging;                    /* a data-device drag is in progress */

	/* interactive pointer operation (super+drag move/resize) */
	enum { HONEY_OP_NONE, HONEY_OP_MOVE, HONEY_OP_RESIZE } op;
	struct honey_window *op_window;
	bool op_was_tiled;             /* re-tile on drop (drop-at-cursor) */
	double op_start_x, op_start_y; /* cursor at grab time */
	struct wlr_box op_geom;        /* window geometry at grab time */
	uint32_t op_edges;             /* WLR_EDGE_* for resize */

	/* extra protocol handlers */
	struct wlr_tearing_control_manager_v1 *tearing_control;
	struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;
	struct wlr_pointer_constraints_v1 *pointer_constraints;
	struct wlr_pointer_constraint_v1 *active_constraint;
	struct wl_listener new_constraint;
	struct wl_listener constraint_destroy;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wl_list idle_inhibitors;      /* honey_idle_inhibitor.link */
	struct wl_list shortcuts_inhibitors; /* honey_shortcuts_inhibitor.link */
	struct wl_listener request_cursor_shape;
	struct wl_listener new_virtual_keyboard;
	struct wl_listener new_virtual_pointer;
	struct wl_listener request_activate;
	struct wl_listener new_idle_inhibitor;
	struct wl_listener new_shortcuts_inhibitor;
};

struct honey_idle_inhibitor {
	struct wl_list link; /* honey_server.idle_inhibitors */
	struct honey_server *server;
	struct wl_listener destroy;
};

struct honey_shortcuts_inhibitor {
	struct wl_list link; /* honey_server.shortcuts_inhibitors */
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
	struct wl_listener destroy;
};

/* ------------------------------------------------------------------- output */

struct honey_output {
	struct wl_list link;
	struct honey_server *server;
	struct wlr_output *wlr_output;
	struct wlr_box usable; /* layout geometry minus layer-shell exclusive zones */

	struct wl_list workspaces; /* honey_workspace.link, sorted by number */
	struct honey_workspace *active;
	int previous_number; /* for workspace-back */

	struct wl_list layer_surfaces; /* honey_layer_surface.link */
	struct wlr_ext_workspace_group_handle_v1 *ext_group;
	struct wlr_color_transform *gamma_transform; /* sized to this CRTC, or NULL */

	char *status_workspaces; /* last broadcast line, for diffing */
	char *status_window;

	struct wl_listener frame;
	struct wl_listener destroy;
};

/* ---------------------------------------------------------------- workspace */

struct honey_workspace {
	struct wl_list link; /* honey_output.workspaces */
	struct honey_output *output;
	int number;
	char *name; /* optional label, or NULL */
	struct wlr_ext_workspace_handle_v1 *ext; /* ext-workspace handle, or NULL */

	/* per-workspace overrides; has_* false = use the global default */
	const struct honey_layout *layout; /* NULL = global */
	double mfact;
	bool has_mfact;
	int nmaster;
	bool has_nmaster;
	enum honey_orientation orientation;
	bool has_orientation;
};

/* ------------------------------------------------------------------- window */

enum honey_window_type {
	HONEY_WINDOW_XDG,
	HONEY_WINDOW_X11,
};

struct honey_window {
	struct wl_list link;
	struct honey_server *server;
	struct honey_workspace *workspace;
	enum honey_window_type type;
	struct wlr_xdg_toplevel *xdg_toplevel;         /* XDG windows */
	struct wlr_xwayland_surface *xwayland_surface; /* X11 windows */
	struct wlr_scene_tree *tree;         /* parent, positioned at the tile origin */
	struct wlr_scene_tree *surface_tree; /* xdg surface, inset by the border */
	struct wlr_scene_rect *border[4];    /* top, bottom, left, right */
	struct wlr_scene_rect *dim;          /* dim-inactive overlay */
	struct wlr_box geom; /* current geometry */
	struct wlr_box requested; /* last configure request (duplicate dedupe) */
	uint32_t resize_serial; /* pending size request not yet acked (XDG) */
	bool placed; /* in server.windows with a reserved slot (may predate map) */
	bool mapped;
	bool suspended; /* XDG: last suspended state sent (hidden-workspace throttle) */

	/* window states (mutually exclusive) */
	bool floating;
	struct wlr_box float_geom; /* geometry while floating */
	bool float_pending_app_size; /* adopt the app's own size on next commit */
	bool auto_centered; /* auto-floated dialog: keep centred as its size settles,
	                     * until the user moves or resizes it */
	bool fullscreen;
	bool maximized;
	bool fake_fullscreen; /* fill output + tell the client, not exclusive */
	char *initial_title;  /* title at map time, for initial-title rules */
	bool suppress_maximize; /* rule: ignore client maximize requests */

	struct wlr_foreign_toplevel_handle_v1 *foreign; /* taskbar handle, or NULL */
	struct wlr_output *foreign_output; /* output the foreign handle reports, or NULL */

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
	struct wl_listener set_parent;
	struct wl_listener request_fullscreen;
	struct wl_listener request_maximize;
	struct wl_listener foreign_activate;
	struct wl_listener foreign_close;
};

/* ------------------------------------------------------------------ keybind */

struct honey_keybind {
	struct wl_list link; /* honey_server.keybinds */
	uint32_t modifiers;
	xkb_keysym_t sym;
	char *action; /* owned */
};

/* One exec-once command already run this session (keyed by the exact line). */
struct honey_exec_once {
	struct wl_list link; /* honey_server.exec_once */
	char *command; /* owned */
};

struct honey_ipc_client {
	struct wl_list link; /* honey_server.ipc_clients */
	struct honey_server *server;
	int fd;
	struct wl_event_source *source;
	bool subscriber; /* receives the status stream instead of one reply */
};

/* ------------------------------------------------------------------- rules */

enum honey_rule_field {
	HONEY_RULE_APP_ID,
	HONEY_RULE_TITLE,
	HONEY_RULE_INITIAL_TITLE,
};

enum honey_rule_action {
	HONEY_RULE_WORKSPACE,
	HONEY_RULE_FLOAT,
	HONEY_RULE_TILE,
	HONEY_RULE_SUPPRESS_MAXIMIZE,
	HONEY_RULE_NO_FOCUS,
};

struct honey_rule {
	struct wl_list link; /* honey_server.rules */
	enum honey_rule_field field;
	bool regex;
	regex_t re; /* compiled when regex */
	char *pattern;
	enum honey_rule_action action;
	char *ws_addr;               /* WORKSPACE */
	double float_w, float_h;     /* FLOAT fractions, 0 = default */
	int float_w_px, float_h_px;  /* FLOAT pixels, 0 = unused */
	bool float_default;          /* FLOAT "default": use the app's own size */
};

/* -------------------------------------------------------------- layer surface */

struct honey_layer_surface {
	struct wl_list link; /* honey_output.layer_surfaces */
	struct honey_server *server;
	struct honey_output *output;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
};

/* --------------------------------------------------------------------- input */

struct honey_input_rule {
	struct wl_list link; /* honey_server.input_rules */
	char *device; /* substring match against device name, or "*" */
	char *option;
	char *value;
};

struct honey_input_device {
	struct wl_list link; /* honey_server.input_devices */
	struct honey_server *server;
	struct wlr_input_device *device;
	struct wl_listener destroy;
};

/* ----------------------------------------------------------------- keyboard */

struct honey_keyboard {
	struct wl_list link;
	struct honey_server *server;
	struct wlr_keyboard *wlr_keyboard;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* ------------------------------------------------------------------- logging */

void honey_log (const char *format, ...);
void honey_dbg (const char *format, ...);
#define LOG(...) honey_log(__VA_ARGS__)
#define DBG(...) honey_dbg(__VA_ARGS__)

/* ------------------------------------------------------------------- modules */

void honey_output_setup (struct honey_server *server);
void honey_output_manager_setup (struct honey_server *server);
bool honey_output_command (
	struct honey_server *server,
	char *args,
	char *error,
	size_t error_size
);
void honey_window_setup (struct honey_server *server);
void honey_seat_setup (struct honey_server *server);
void honey_seat_new_keyboard (
	struct honey_server *server,
	struct wlr_input_device *device
);

/* extra protocol handlers (cursor-shape, virtual input, activation, idle/
 * shortcuts inhibit) */
void honey_handlers_setup (struct honey_server *server);
bool honey_shortcuts_inhibited (struct honey_server *server);
void honey_constraint_check (
	struct honey_server *server,
	struct wlr_surface *surface
);

void honey_arrange (struct honey_server *server);
void honey_exec (const char *command);
void honey_exec_once (
	struct honey_server *server,
	const char *command
);

/* type-agnostic window accessors (XDG / X11) */
struct wlr_surface *honey_window_surface (struct honey_window *window);
const char *honey_window_title (struct honey_window *window);
const char *honey_window_app_id (struct honey_window *window);
void honey_window_configure (
	struct honey_window *window,
	int x,
	int y,
	int width,
	int height
);
void honey_window_set_activated (
	struct honey_window *window,
	bool activated
);
void honey_window_close (struct honey_window *window);

/* shared window lifecycle (called by the XDG and X11 paths) */
void honey_window_finish_setup (struct honey_window *window);
void honey_window_handle_map (struct honey_window *window);
void honey_window_handle_unmap (struct honey_window *window);

/* window states */
bool honey_window_is_tiled (struct honey_window *window);
void honey_window_update_layer (struct honey_window *window);
void honey_window_inform_states (struct honey_window *window);
void honey_window_clear_states (struct honey_window *window);
/* inform + relayer + rearrange after a state change */
void honey_window_apply_state (struct honey_window *window);
/* centre a floating geometry of the given size on the window's output */
void honey_window_set_float_geom (
	struct honey_window *window,
	int width,
	int height
);
/* client state requests, shared by the XDG and X11 paths */
void honey_window_handle_request_fullscreen (struct honey_window *window);
void honey_window_handle_request_maximize (struct honey_window *window);
void honey_float_seed (struct honey_window *window);
void honey_action_toggle_float (struct honey_server *server);
void honey_action_fullscreen (struct honey_server *server);
void honey_action_maximize (struct honey_server *server);
void honey_action_fake_fullscreen (struct honey_server *server);

/* stack order + live layout tweaks */
void honey_action_swap (
	struct honey_server *server,
	int direction
);
void honey_action_swap_master (struct honey_server *server);
void honey_action_mfact (
	struct honey_server *server,
	double delta
);
void honey_action_nmaster (
	struct honey_server *server,
	int delta
);
void honey_action_orientation_cycle (struct honey_server *server);
void honey_action_swap_dir (
	struct honey_server *server,
	enum honey_direction direction
);

/* config */
void honey_config_defaults (struct honey_config *config);
bool honey_config_set (
	struct honey_server *server,
	const char *key,
	const char *value
);
bool honey_parse_orientation (
	const char *value,
	enum honey_orientation *out
);
bool honey_config_get (
	struct honey_server *server,
	const char *key,
	char *reply,
	size_t reply_size
);

/* bindings + IPC */
void honey_binding_setup (struct honey_server *server);
bool honey_binding_add (
	struct honey_server *server,
	const char *combo,
	const char *action
);
bool honey_binding_remove (
	struct honey_server *server,
	const char *combo
);
bool honey_binding_run (
	struct honey_server *server,
	uint32_t modifiers,
	xkb_keysym_t sym
);
bool honey_action_run (
	struct honey_server *server,
	const char *action
);
bool honey_action_known (const char *verb);
bool honey_command_known (const char *verb);
bool honey_command_run (
	struct honey_server *server,
	const char *line
);
void honey_ipc_setup (struct honey_server *server);
void honey_config_run (struct honey_server *server);

/* focus */
void honey_focus_window (struct honey_window *window);
void honey_focus_output_active (struct honey_output *output);

/* workspaces */
struct honey_workspace *honey_workspace_get (
	struct honey_output *output,
	int number
);
/* Parse "output:N" or bare "N" (focused output). */
bool honey_parse_ws_addr (
	struct honey_server *server,
	const char *addr,
	struct honey_workspace **out
);
struct honey_window *honey_workspace_first_window (struct honey_workspace *workspace);
struct honey_output *honey_output_in_direction (
	struct honey_output *from,
	enum honey_direction direction
);
struct honey_output *honey_output_at (
	struct honey_server *server,
	double x,
	double y
);
struct honey_output *honey_output_by_name (
	struct honey_server *server,
	const char *name
);
void honey_warp_to_focus (struct honey_server *server);

/* actions (invoked by keybindings and IPC commands) */
void honey_action_close (struct honey_server *server);
void honey_action_focus (
	struct honey_server *server,
	int direction
);
void honey_action_workspace (
	struct honey_server *server,
	int number
);
void honey_action_move_to_workspace (
	struct honey_server *server,
	int number
);
void honey_action_workspace_back (struct honey_server *server);
void honey_action_workspace_cycle (
	struct honey_server *server,
	int direction
);
void honey_action_focus_dir (
	struct honey_server *server,
	enum honey_direction direction
);
void honey_action_move_to_output (
	struct honey_server *server,
	enum honey_direction direction
);

/* layer shell */
void honey_layer_setup (struct honey_server *server);
void honey_layer_arrange (struct honey_output *output);

/* good-citizen protocol managers */
void honey_protocols_setup (struct honey_server *server);

/* gamma / night-light */
void honey_gamma_setup (struct honey_server *server);
void honey_gamma_set (
	struct honey_server *server,
	double temperature,
	double brightness
);
void honey_gamma_update_output (struct honey_output *output);

/* ext-workspace protocol */
void honey_ext_workspace_setup (struct honey_server *server);
void honey_ext_workspace_sync (struct honey_server *server);

/* actions helper shared with protocol handlers */
void honey_switch_workspace (
	struct honey_output *output,
	int number
);

/* status stream */
void honey_status_broadcast (struct honey_server *server);
void honey_status_broadcast_gamma (struct honey_server *server);
void honey_status_snapshot (
	struct honey_server *server,
	struct honey_ipc_client *client
);

/* input */
void honey_input_setup (struct honey_server *server);
void honey_input_apply_keyboard (
	struct honey_server *server,
	struct wlr_keyboard *keyboard
);
void honey_input_add_device (
	struct honey_server *server,
	struct wlr_input_device *device
);
bool honey_kb_layout (
	struct honey_server *server,
	const char *layout,
	const char *variant,
	const char *model,
	const char *options,
	const char *rules
);
bool honey_kb_repeat (
	struct honey_server *server,
	int rate,
	int delay
);
bool honey_input_rule_add (
	struct honey_server *server,
	const char *device,
	const char *option,
	const char *value
);

/* X11 coordinate space: each output occupies a region anchored at its logical
 * position times the anchor scale, sized by its own render scale. */
double honey_output_xwayland_scale (struct honey_output *output);
void honey_output_xwayland_geometry (
	struct honey_output *output,
	struct wlr_box *box
);
void honey_to_xwayland (
	struct honey_server *server,
	double lx,
	double ly,
	int *x,
	int *y
);
void honey_from_xwayland (
	struct honey_server *server,
	int x,
	int y,
	double *lx,
	double *ly
);
/* the render scale that applies at a layout point (its output's scale) */
double honey_xwayland_scale_at (
	struct honey_server *server,
	double lx,
	double ly
);

/* xdg-output (in-tree: the Xwayland client sees scaled geometry) */
void honey_xdg_output_setup (struct honey_server *server);
void honey_xdg_output_update (struct honey_server *server);
void honey_xdg_output_output_destroyed (
	struct honey_server *server,
	struct wlr_output *wlr_output
);

void honey_decoration_setup (struct honey_server *server);
void honey_xwayland_setup (struct honey_server *server);

#endif
