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
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wlr/xwayland/server.h>
#include <wlr/xwayland/xwayland.h>

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

/* ------------------------------------------------------------------ children */

static struct w3ld_server *signal_server; /* for the SIGCHLD handler */

/* Reap exited children, except the Xwayland process (wlroots waits on it). */
static void reap_children (int signo) {
	siginfo_t info;
	pid_t xwayland_pid = signal_server && signal_server->xwayland
		? signal_server->xwayland->server->pid : -1;
	info.si_pid = 0;
	while (waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT) == 0
			&& info.si_pid) {
		if (info.si_pid == xwayland_pid)
			break;
		waitpid(info.si_pid, NULL, 0);
		info.si_pid = 0;
	}
}

/* ---------------------------------------------------------------------- main */

int main (
	int argc,
	char *argv[]
) {
	wlr_log_init(WLR_INFO, NULL);

	/* Reap spawned children with a handler, not SIG_IGN: an ignored SIGCHLD
	 * survives exec, and Xwayland inheriting it breaks its xkbcomp Popen
	 * (keymap compile reads back ECHILD and fails). The Xwayland child itself
	 * is skipped — wlroots waits on it. */
	struct sigaction child_action = { .sa_handler = reap_children };
	sigaction(SIGCHLD, &child_action, NULL);

	struct w3ld_server server = {0};
	signal_server = &server;
	w3ld_config_defaults(&server.config);

	/* -c <path> overrides the config file; remembered so reload uses it too.
	 * argv lives for the process, so the pointer is safe to keep. */
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-c") && i + 1 < argc)
			server.config_path = argv[++i];
	}
	/* Ready before any arrange (which broadcasts) runs during backend start. */
	wl_list_init(&server.ipc_clients);
	wl_list_init(&server.rules);
	server.ipc_fd = -1;

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

	server.compositor = wlr_compositor_create(server.display, 5,
			server.renderer);
	wlr_subcompositor_create(server.display);
	wlr_data_device_manager_create(server.display);

	server.output_layout = wlr_output_layout_create(server.display);
	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene,
			server.output_layout);

	w3ld_layer_setup(&server);
	w3ld_output_setup(&server);
	w3ld_output_manager_setup(&server);
	w3ld_window_setup(&server);
	w3ld_decoration_setup(&server);
	w3ld_seat_setup(&server);
	w3ld_input_setup(&server);
	w3ld_binding_setup(&server);
	w3ld_protocols_setup(&server);
	w3ld_handlers_setup(&server);
	w3ld_gamma_setup(&server);
	w3ld_ext_workspace_setup(&server);
	w3ld_xwayland_setup(&server);

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
	/* Declare the session so toolkits and portals identify it (only if a
	 * session manager hasn't already — hence overwrite = 0). */
	setenv("XDG_CURRENT_DESKTOP", "w3ld", 0);
	setenv("XDG_SESSION_DESKTOP", "w3ld", 0);
	setenv("XDG_SESSION_TYPE", "wayland", 0);
	LOG("running on WAYLAND_DISPLAY=%s", socket);

	w3ld_ipc_setup(&server);
	w3ld_config_run(&server);

	/* Load a cursor image up front; without this the cursor is invisible
	 * until the first motion event sets one. */
	wlr_cursor_set_xcursor(server.cursor, server.xcursor_manager, "default");

	wl_display_run(server.display);

	wl_display_destroy_clients(server.display);
	wl_display_destroy(server.display);
	return 0;
}
