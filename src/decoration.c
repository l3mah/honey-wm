/* Decorations.
 *
 * Advertises both decoration protocols so every toolkit gets server-side
 * decorations (honey draws only a border; clients drop their titlebars):
 *   - xdg-decoration (zxdg_decoration_manager_v1): each toplevel decoration is
 *     forced to server-side mode. Spoken by alacritty and newer Qt.
 *   - kde server-decoration (org_kde_kwin_server_decoration_manager): default
 *     server mode. This is the protocol GTK3/GTK4 actually speak — advertising
 *     it is the fix for GTK client-side headerbars that river could not provide.
 */
#include <stdlib.h>

#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>

#include "honey.h"

struct honey_decoration {
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct wl_listener request_mode;
	struct wl_listener surface_commit;
	struct wl_listener destroy;
};

/* Forcing server-side schedules a configure, which asserts if the xdg surface
 * has not had its initial commit yet — so it is guarded on `initialized`. */
static void apply_server_side (struct honey_decoration *decoration) {
	if (decoration->decoration->toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(decoration->decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void decoration_request_mode (
	struct wl_listener *listener,
	void *data
) {
	struct honey_decoration *decoration =
		wl_container_of(listener, decoration, request_mode);
	apply_server_side(decoration);
}

/* Apply once the surface is initialized (its first commit), then stop watching. */
static void decoration_surface_commit (
	struct wl_listener *listener,
	void *data
) {
	struct honey_decoration *decoration =
		wl_container_of(listener, decoration, surface_commit);
	if (!decoration->decoration->toplevel->base->initialized)
		return;
	wlr_xdg_toplevel_decoration_v1_set_mode(decoration->decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	wl_list_remove(&decoration->surface_commit.link);
	wl_list_init(&decoration->surface_commit.link);
}

static void decoration_destroy (
	struct wl_listener *listener,
	void *data
) {
	struct honey_decoration *decoration =
		wl_container_of(listener, decoration, destroy);
	wl_list_remove(&decoration->request_mode.link);
	wl_list_remove(&decoration->surface_commit.link);
	wl_list_remove(&decoration->destroy.link);
	free(decoration);
}

static void new_toplevel_decoration (
	struct wl_listener *listener,
	void *data
) {
	struct wlr_xdg_toplevel_decoration_v1 *xdg_decoration = data;
	struct honey_decoration *decoration = calloc(1, sizeof *decoration);
	decoration->decoration = xdg_decoration;

	decoration->request_mode.notify = decoration_request_mode;
	wl_signal_add(&xdg_decoration->events.request_mode,
			&decoration->request_mode);
	decoration->destroy.notify = decoration_destroy;
	wl_signal_add(&xdg_decoration->events.destroy, &decoration->destroy);
	decoration->surface_commit.notify = decoration_surface_commit;
	wl_signal_add(&xdg_decoration->toplevel->base->surface->events.commit,
			&decoration->surface_commit);

	apply_server_side(decoration);
}

void honey_decoration_setup (struct honey_server *server) {
	struct wlr_xdg_decoration_manager_v1 *xdg_manager =
		wlr_xdg_decoration_manager_v1_create(server->display);
	server->new_toplevel_decoration.notify = new_toplevel_decoration;
	wl_signal_add(&xdg_manager->events.new_toplevel_decoration,
			&server->new_toplevel_decoration);

	struct wlr_server_decoration_manager *kde_manager =
		wlr_server_decoration_manager_create(server->display);
	wlr_server_decoration_manager_set_default_mode(kde_manager,
			WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
}
