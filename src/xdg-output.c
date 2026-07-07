/* xdg-output, implemented in-tree instead of wlroots' manager for one reason:
 * the Xwayland client must see a different picture than everyone else.
 *
 * Xwayland sizes its X11 screen and RANDR outputs from xdg-output. Regular
 * clients get the logical layout. The Xwayland client gets the layout scaled
 * by the X11 render scale (xwayland-scale), so the X screen exactly covers the
 * physically-sized windows the compositor configures — without this, windows
 * overflow the X screen and X drops input past the logical boundary (the
 * dead-click zone). On an output whose scale equals the render scale the
 * reported size is the physical resolution — the Hyprland technique.
 *
 * The global is advertised at version 2: version 3 deprecates xdg_output.done
 * in favour of wl_output.done, which wlroots owns and this module cannot send.
 */
#include <stdlib.h>

#include <wlr/xwayland/server.h>
#include <wlr/xwayland/xwayland.h>

#include "xdg-output-unstable-v1-protocol.h"

#include "w3ld.h"

#define XDG_OUTPUT_VERSION 2

/* ------------------------------------------------------------------- events */

static bool for_xwayland (
	struct w3ld_server *server,
	struct wl_resource *resource
) {
	return server->xwayland && server->xwayland->server
		&& wl_resource_get_client(resource)
			== server->xwayland->server->client;
}

static void send_geometry (
	struct w3ld_server *server,
	struct wl_resource *resource
) {
	struct wlr_output *wlr_output = wl_resource_get_user_data(resource);
	if (!wlr_output)
		return; /* inert: the output is gone */

	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, wlr_output, &box);
	if (wlr_box_empty(&box))
		return; /* not in the layout */

	double scale = for_xwayland(server, resource)
		? w3ld_xwayland_effective_scale(server) : 1.0;
	zxdg_output_v1_send_logical_position(resource,
			(int)(box.x * scale), (int)(box.y * scale));
	zxdg_output_v1_send_logical_size(resource,
			(int)(box.width * scale), (int)(box.height * scale));
	zxdg_output_v1_send_done(resource);
}

/* Resend after anything that moves geometry: layout changes, mode/scale
 * changes, xwayland-scale changes. Clients (Xwayland's RANDR included) apply
 * output state on wl_output.done, so one is scheduled for every output. */
void w3ld_xdg_output_update (struct w3ld_server *server) {
	struct wl_resource *resource;
	wl_resource_for_each(resource, &server->xdg_output_resources)
		send_geometry(server, resource);

	struct w3ld_output *output;
	wl_list_for_each(output, &server->outputs, link)
		wlr_output_schedule_done(output->wlr_output);
}

/* Called from output teardown so stale resources go inert, not dangling. */
void w3ld_xdg_output_output_destroyed (
	struct w3ld_server *server,
	struct wlr_output *wlr_output
) {
	struct wl_resource *resource;
	wl_resource_for_each(resource, &server->xdg_output_resources) {
		if (wl_resource_get_user_data(resource) == wlr_output)
			wl_resource_set_user_data(resource, NULL);
	}
}

/* ---------------------------------------------------------------- interfaces */

static void output_handle_destroy (
	struct wl_client *client,
	struct wl_resource *resource
) {
	wl_resource_destroy(resource);
}

static const struct zxdg_output_v1_interface output_impl = {
	.destroy = output_handle_destroy,
};

static void output_resource_destroy (struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void manager_handle_destroy (
	struct wl_client *client,
	struct wl_resource *resource
) {
	wl_resource_destroy(resource);
}

static void manager_handle_get_xdg_output (
	struct wl_client *client,
	struct wl_resource *manager_resource,
	uint32_t id,
	struct wl_resource *output_resource
) {
	struct w3ld_server *server = wl_resource_get_user_data(manager_resource);
	struct wlr_output *wlr_output = wlr_output_from_resource(output_resource);

	struct wl_resource *resource = wl_resource_create(client,
			&zxdg_output_v1_interface,
			wl_resource_get_version(manager_resource), id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &output_impl, wlr_output,
			output_resource_destroy);
	wl_list_insert(&server->xdg_output_resources,
			wl_resource_get_link(resource));

	if (wlr_output
			&& wl_resource_get_version(resource)
				>= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION) {
		zxdg_output_v1_send_name(resource, wlr_output->name);
		if (wlr_output->description)
			zxdg_output_v1_send_description(resource,
					wlr_output->description);
	}
	send_geometry(server, resource);
}

static const struct zxdg_output_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.get_xdg_output = manager_handle_get_xdg_output,
};

static void manager_bind (
	struct wl_client *client,
	void *data,
	uint32_t version,
	uint32_t id
) {
	struct wl_resource *resource = wl_resource_create(client,
			&zxdg_output_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

/* -------------------------------------------------------------------- setup */

void w3ld_xdg_output_setup (struct w3ld_server *server) {
	wl_list_init(&server->xdg_output_resources);
	wl_global_create(server->display, &zxdg_output_manager_v1_interface,
			XDG_OUTPUT_VERSION, server, manager_bind);
}
