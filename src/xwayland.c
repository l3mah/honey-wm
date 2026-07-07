/* XWayland: run X11 apps.
 *
 * The Xwayland server starts lazily on the first X11 client. Managed X11
 * windows become regular w3ld windows (tiled, focused, closed through the
 * type-agnostic accessors) via the shared lifecycle in window.c. Override-
 * redirect surfaces (menus, tooltips, drag icons) are unmanaged: shown at their
 * own coordinates in the TOP layer, never tiled or focused.
 */
#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/interfaces/wlr_buffer.h>
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
	struct wl_listener unmanaged_commit;
	struct wl_listener set_geometry;
};

/* ------------------------------------------------ scaling (removable feature) */
/* xwayland-scale (removable): the whole section below, through
 * descale_surface_tree, is the XWayland-scale feature. Removal manifest in
 * xdg-output.c. XWayland-only — no regular-Wayland path reaches this. */

/* The X11 coordinate space (the Hyprland technique, per monitor): outputs are
 * packed contiguously in a single row (y = 0) in output-list order, each region
 * sized by its OWN render scale — the exact physical resolution under auto. X11
 * windows are configured inside their output's region at that output's scale
 * and their buffers displayed back at logical size: 1:1 crisp per monitor.
 * Contiguous packing leaves no gaps, so the X screen exactly covers the windows
 * (MonitorPositionController.cpp:103). */

/* The render scale of one output: off = 1, a number = that everywhere,
 * auto = the output's own scale. */
double w3ld_output_xwayland_scale (struct w3ld_output *output) {
	struct w3ld_config *config = &output->server->config;
	if (config->xwayland_scale_auto)
		return output->wlr_output->scale;
	return config->xwayland_scale > 0 ? config->xwayland_scale : 1.0;
}

/* An output's X11-space region size (physical resolution under auto). */
static void output_xwayland_size (
	struct w3ld_output *output,
	int *width,
	int *height
) {
	double scale = w3ld_output_xwayland_scale(output);
	if (scale == output->wlr_output->scale) {
		wlr_output_transformed_resolution(output->wlr_output, width, height);
	} else {
		struct wlr_box logical;
		wlr_output_layout_get_box(output->server->output_layout,
				output->wlr_output, &logical);
		*width = (int)round(logical.width * scale);
		*height = (int)round(logical.height * scale);
	}
}

/* An output's region, packed contiguously left-to-right in output-list order. */
void w3ld_output_xwayland_geometry (
	struct w3ld_output *output,
	struct wlr_box *box
) {
	int offset = 0;
	struct w3ld_output *other;
	wl_list_for_each(other, &output->server->outputs, link) {
		int width, height;
		output_xwayland_size(other, &width, &height);
		if (other == output) {
			*box = (struct wlr_box){ .x = offset, .y = 0, .width = width,
				.height = height };
			return;
		}
		offset += width;
	}
	*box = (struct wlr_box){0};
}

/* The output containing a layout point, or the focused one. */
static struct w3ld_output *output_for_point (
	struct w3ld_server *server,
	double lx,
	double ly
) {
	struct w3ld_output *output = w3ld_output_at(server, lx, ly);
	return output ? output : server->focused_output;
}

double w3ld_xwayland_scale_at (
	struct w3ld_server *server,
	double lx,
	double ly
) {
	struct w3ld_output *output = output_for_point(server, lx, ly);
	return output ? w3ld_output_xwayland_scale(output) : 1.0;
}

void w3ld_to_xwayland (
	struct w3ld_server *server,
	double lx,
	double ly,
	int *x,
	int *y
) {
	struct w3ld_output *output = output_for_point(server, lx, ly);
	if (!output) {
		*x = (int)round(lx);
		*y = (int)round(ly);
		return;
	}
	struct wlr_box region;
	w3ld_output_xwayland_geometry(output, &region);
	struct wlr_box logical;
	wlr_output_layout_get_box(server->output_layout, output->wlr_output,
			&logical);
	double scale = w3ld_output_xwayland_scale(output);
	*x = region.x + (int)round((lx - logical.x) * scale);
	*y = region.y + (int)round((ly - logical.y) * scale);
}

void w3ld_from_xwayland (
	struct w3ld_server *server,
	int x,
	int y,
	double *lx,
	double *ly
) {
	struct w3ld_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_box region;
		w3ld_output_xwayland_geometry(output, &region);
		if (x < region.x || x >= region.x + region.width
				|| y < region.y || y >= region.y + region.height)
			continue;
		struct wlr_box logical;
		wlr_output_layout_get_box(server->output_layout, output->wlr_output,
				&logical);
		double scale = w3ld_output_xwayland_scale(output);
		*lx = logical.x + (x - region.x) / scale;
		*ly = logical.y + (y - region.y) / scale;
		return;
	}
	*lx = x;
	*ly = y;
}

static void buffer_scale_x11 (
	struct wlr_scene_buffer *buffer,
	int sx,
	int sy,
	void *data
) {
	double scale = *(double *)data;
	if (!buffer->buffer)
		return;
	if (scale != 1.0) {
		/* Approach A: the buffer is oversized; display it at logical size (a
		 * clean downscale — bilinear keeps it smooth). */
		wlr_scene_buffer_set_dest_size(buffer,
				(int)round(buffer->buffer->width / scale),
				(int)round(buffer->buffer->height / scale));
		wlr_scene_buffer_set_filter_mode(buffer, WLR_SCALE_FILTER_BILINEAR);
	} else {
		/* Passthrough (xwayland-scale off): a 1x buffer the output will
		 * upscale — nearest-neighbour keeps it pixel-sharp instead of smeared,
		 * matching Hyprland's use_nearest_neighbor. */
		wlr_scene_buffer_set_filter_mode(buffer, WLR_SCALE_FILTER_NEAREST);
	}
}

/* Runs after the scene's own commit handler (registered earlier), so these
 * per-buffer overrides win each commit. */
static void descale_surface_tree (
	struct wlr_scene_tree *tree,
	double scale
) {
	wlr_scene_node_for_each_buffer(&tree->node, buffer_scale_x11, &scale);
}

/* ---------------------------------------------------------------- unmanaged */

/* Override-redirect coordinates are in the X11 coordinate space. */
static void unmanaged_position (struct w3ld_xwayland_surface *xw) {
	double lx, ly;
	w3ld_from_xwayland(xw->server, xw->xwayland_surface->x,
			xw->xwayland_surface->y, &lx, &ly);
	wlr_scene_node_set_position(&xw->unmanaged_tree->node,
			(int)round(lx), (int)round(ly));
}

static void unmanaged_commit (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_xwayland_surface *xw =
		wl_container_of(listener, xw, unmanaged_commit);
	descale_surface_tree(xw->unmanaged_tree,
			w3ld_xwayland_scale_at(xw->server, xw->unmanaged_tree->node.x,
				xw->unmanaged_tree->node.y));
}

static void handle_set_geometry (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_xwayland_surface *xw =
		wl_container_of(listener, xw, set_geometry);
	if (xw->unmanaged_tree)
		unmanaged_position(xw);
}

static void unmanaged_show (struct w3ld_xwayland_surface *xw) {
	struct wlr_xwayland_surface *xsurface = xw->xwayland_surface;
	xw->unmanaged_tree = wlr_scene_tree_create(
			xw->server->layers[W3LD_LAYER_TOP]);
	wlr_scene_subsurface_tree_create(xw->unmanaged_tree, xsurface->surface);
	unmanaged_position(xw);

	xw->unmanaged_commit.notify = unmanaged_commit;
	wl_signal_add(&xsurface->surface->events.commit, &xw->unmanaged_commit);
	xw->set_geometry.notify = handle_set_geometry;
	wl_signal_add(&xsurface->events.set_geometry, &xw->set_geometry);
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

static void x11_commit (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_window *window = wl_container_of(listener, window, commit);
	struct w3ld_output *output = window->workspace
		? window->workspace->output : window->server->focused_output;
	descale_surface_tree(window->surface_tree,
			output ? w3ld_output_xwayland_scale(output) : 1.0);
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
	window->commit.notify = x11_commit;
	wl_signal_add(&xsurface->surface->events.commit, &window->commit);
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
		wl_list_remove(&window->commit.link);
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
		wl_list_remove(&xw->unmanaged_commit.link);
		wl_list_remove(&xw->set_geometry.link);
		wlr_scene_node_destroy(&xw->unmanaged_tree->node);
		xw->unmanaged_tree = NULL;
	}
}

/* X11 apps resize themselves (restoring remembered bounds, dialogs sizing to
 * content). Before the layout owns the window the request is honored; a mapped
 * floating window gets its request (descaled into the float geometry); a mapped
 * tiled window is answered by re-asserting the layout's geometry — otherwise
 * the app's size sticks and the window shrinks inside its tile. */
static void handle_request_configure (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_xwayland_surface *xw =
		wl_container_of(listener, xw, request_configure);
	struct wlr_xwayland_surface_configure_event *event = data;
	struct w3ld_window *window = xw->window;

	if (!window || !window->mapped) {
		wlr_xwayland_surface_configure(xw->xwayland_surface, event->x,
				event->y, event->width, event->height);
		return;
	}

	if (window->floating) {
		double lx, ly;
		w3ld_from_xwayland(xw->server, event->x, event->y, &lx, &ly);
		double scale = w3ld_xwayland_scale_at(xw->server, lx, ly);
		window->float_geom.x = (int)round(lx);
		window->float_geom.y = (int)round(ly);
		window->float_geom.width = (int)round(event->width / scale);
		window->float_geom.height = (int)round(event->height / scale);
	}
	w3ld_arrange(xw->server); /* re-places: float request applied, tile re-asserted */
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

/* A minimal wlr_buffer wrapping xcursor pixels, for wlr_xwayland_set_cursor
 * (without a cursor Xwayland shows the X11 root fallback, the X shape). */
struct w3ld_pixel_buffer {
	struct wlr_buffer base;
	uint8_t *data;
	size_t stride;
};

static void pixel_buffer_destroy (struct wlr_buffer *buffer) {
	struct w3ld_pixel_buffer *pixel =
		wl_container_of(buffer, pixel, base);
	free(pixel->data);
	free(pixel);
}

static bool pixel_buffer_begin_access (
	struct wlr_buffer *buffer,
	uint32_t flags,
	void **data,
	uint32_t *format,
	size_t *stride
) {
	struct w3ld_pixel_buffer *pixel =
		wl_container_of(buffer, pixel, base);
	*data = pixel->data;
	*format = DRM_FORMAT_ARGB8888;
	*stride = pixel->stride;
	return true;
}

static void pixel_buffer_end_access (struct wlr_buffer *buffer) {
}

static const struct wlr_buffer_impl pixel_buffer_impl = {
	.destroy = pixel_buffer_destroy,
	.begin_data_ptr_access = pixel_buffer_begin_access,
	.end_data_ptr_access = pixel_buffer_end_access,
};

static void handle_ready (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, xwayland_ready);
	wlr_xwayland_set_seat(server->xwayland, server->seat);

	struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(
			server->xcursor_manager, "default", 1);
	if (!xcursor)
		return;
	struct wlr_xcursor_image *image = xcursor->images[0];

	struct w3ld_pixel_buffer *pixel = calloc(1, sizeof *pixel);
	pixel->stride = image->width * 4;
	pixel->data = malloc(pixel->stride * image->height);
	memcpy(pixel->data, image->buffer, pixel->stride * image->height);
	wlr_buffer_init(&pixel->base, &pixel_buffer_impl, image->width,
			image->height);
	wlr_xwayland_set_cursor(server->xwayland, &pixel->base,
			image->hotspot_x, image->hotspot_y);
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
