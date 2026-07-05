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
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

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
};

/* ------------------------------------------------------------------- output */

struct w3ld_output {
	struct wl_list link;
	struct w3ld_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener destroy;
};

/* ------------------------------------------------------------------- logging */

void w3ld_log (const char *format, ...);
void w3ld_dbg (const char *format, ...);
#define LOG(...) w3ld_log(__VA_ARGS__)
#define DBG(...) w3ld_dbg(__VA_ARGS__)

/* ------------------------------------------------------------------- modules */

void w3ld_output_setup (struct w3ld_server *server);

/* Stubs filled in later milestones. */
void w3ld_window_setup (struct w3ld_server *server);
void w3ld_seat_setup (struct w3ld_server *server);
void w3ld_input_setup (struct w3ld_server *server);
void w3ld_decoration_setup (struct w3ld_server *server);
void w3ld_layer_setup (struct w3ld_server *server);
void w3ld_xwayland_setup (struct w3ld_server *server);

#endif
