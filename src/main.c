/* w3ld — a tiling Wayland compositor on wlroots.
 *
 * Bootstrap: create the wlroots backend, renderer, and allocator, build the
 * scene graph, advertise the essential protocol globals, then run the Wayland
 * event loop. Output, window, and input policy live in the other modules.
 */
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>

#include "w3ld.h"

/* ------------------------------------------------------------------- logging */

void w3ld_log (const char *format, ...) {
	va_list args;
	fputs("w3ld: ", stderr);
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

void w3ld_dbg (const char *format, ...) {
	static int enabled = -1;
	if (enabled < 0)
		enabled = getenv("W3LD_DEBUG") != NULL;
	if (!enabled)
		return;

	va_list args;
	fputs("w3ld[dbg]: ", stderr);
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

/* ---------------------------------------------------------------------- main */

int main (
	int argc,
	char *argv[]
) {
	wlr_log_init(WLR_INFO, NULL);
	signal(SIGCHLD, SIG_IGN); /* reap spawned children automatically */

	struct w3ld_server server = {0};
	w3ld_config_defaults(&server.config);

	server.display = wl_display_create();
	server.event_loop = wl_display_get_event_loop(server.display);

	server.backend = wlr_backend_autocreate(server.event_loop, NULL);
	if (!server.backend) {
		LOG("failed to create wlr_backend");
		return 1;
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (!server.renderer) {
		LOG("failed to create wlr_renderer");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.display);

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (!server.allocator) {
		LOG("failed to create wlr_allocator");
		return 1;
	}

	wlr_compositor_create(server.display, 5, server.renderer);
	wlr_subcompositor_create(server.display);
	wlr_data_device_manager_create(server.display);

	server.output_layout = wlr_output_layout_create(server.display);
	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene,
			server.output_layout);

	w3ld_output_setup(&server);
	w3ld_output_manager_setup(&server);
	w3ld_window_setup(&server);
	w3ld_decoration_setup(&server);
	w3ld_seat_setup(&server);
	w3ld_binding_setup(&server);

	const char *socket = wl_display_add_socket_auto(server.display);
	if (!socket) {
		LOG("failed to create wayland socket");
		wlr_backend_destroy(server.backend);
		return 1;
	}

	if (!wlr_backend_start(server.backend)) {
		LOG("failed to start backend");
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	LOG("running on WAYLAND_DISPLAY=%s", socket);

	w3ld_ipc_setup(&server);
	w3ld_config_run(&server);

	wl_display_run(server.display);

	wl_display_destroy_clients(server.display);
	wl_display_destroy(server.display);
	return 0;
}
