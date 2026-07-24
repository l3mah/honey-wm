/* honey — a tiling Wayland compositor on wlroots.
 *
 * Bootstrap: create the wlroots backend, renderer, and allocator, build the
 * scene graph, advertise the essential protocol globals, then run the Wayland
 * event loop. Output, window, and input policy live in the other modules.
 */
#include <execinfo.h>
#include <fcntl.h>
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

#include "honey.h"

#ifndef HONEY_VERSION
#define HONEY_VERSION "unknown"
#endif

/* ------------------------------------------------------------------- logging */

void honey_log (const char *format, ...) {
	va_list args;
	fputs("honey: ", stderr);
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

void honey_dbg (const char *format, ...) {
	static int enabled = -1;
	if (enabled < 0)
		enabled = getenv("HONEY_DEBUG") != NULL;
	if (!enabled)
		return;

	va_list args;
	fputs("honey[dbg]: ", stderr);
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

/* ------------------------------------------------------------------ children */

static struct honey_server *signal_server; /* for the SIGCHLD handler */

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

/* -------------------------------------------------------------------- crashes */

/* $HOME/honey-crash.log, filled at startup; the handler appends here so a
 * crash is captured even when stdout is truncated on the next login. */
static char crash_log_path[4096];

/* On a fatal signal, dump a backtrace to stderr AND to the dedicated crash
 * log, then re-raise so the process still dies. -rdynamic makes the frames
 * name honey's own functions even in a stripped build. */
static void crash_handler (int sig) {
	void *frames[64];
	int n = backtrace(frames, 64);
	const char *name = strsignal(sig);
	char header[128];
	int len = snprintf(header, sizeof header,
			"\nhoney %s: FATAL signal %d (%s) — backtrace:\n", HONEY_VERSION,
			sig, name ? name : "?");

	if (write(STDERR_FILENO, header, len) < 0) { /* best effort */ }
	backtrace_symbols_fd(frames, n, STDERR_FILENO);

	if (crash_log_path[0]) {
		int fd = open(crash_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			if (write(fd, header, len) < 0) { /* best effort */ }
			backtrace_symbols_fd(frames, n, fd);
			close(fd);
		}
	}

	signal(sig, SIG_DFL);
	raise(sig);
}

/* ---------------------------------------------------------------------- main */

int main (
	int argc,
	char *argv[]
) {
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
			printf("honey %s\n", HONEY_VERSION);
			return 0;
		}
	}

	/* HONEY_DEBUG raises wlroots to DEBUG (cursor/DRM/render diagnostics). */
	wlr_log_init(getenv("HONEY_DEBUG") ? WLR_DEBUG : WLR_INFO, NULL);

	/* Backtrace on a crash → stderr and $HOME/honey-crash.log. Prime
	 * backtrace() once so its lazy libgcc load doesn't happen in the handler. */
	const char *home = getenv("HOME");
	if (home)
		snprintf(crash_log_path, sizeof crash_log_path, "%s/honey-crash.log",
				home);
	void *prime[1];
	backtrace(prime, 1);
	struct sigaction crash = { .sa_handler = crash_handler };
	sigemptyset(&crash.sa_mask);
	sigaction(SIGSEGV, &crash, NULL);
	sigaction(SIGABRT, &crash, NULL);
	sigaction(SIGBUS, &crash, NULL);
	sigaction(SIGFPE, &crash, NULL);

	/* Reap spawned children with a handler, not SIG_IGN: an ignored SIGCHLD
	 * survives exec, and Xwayland inheriting it breaks its xkbcomp Popen
	 * (keymap compile reads back ECHILD and fails). The Xwayland child itself
	 * is skipped — wlroots waits on it. */
	struct sigaction child_action = { .sa_handler = reap_children };
	sigaction(SIGCHLD, &child_action, NULL);

	struct honey_server server = {0};
	signal_server = &server;
	honey_config_defaults(&server.config);

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

	server.backend = wlr_backend_autocreate(server.event_loop, &server.session);
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

	honey_layer_setup(&server);
	honey_output_setup(&server);
	honey_output_manager_setup(&server);
	honey_window_setup(&server);
	honey_decoration_setup(&server);
	honey_seat_setup(&server);
	honey_input_setup(&server);
	honey_binding_setup(&server);
	honey_protocols_setup(&server);
	honey_handlers_setup(&server);
	honey_gamma_setup(&server);
	honey_ext_workspace_setup(&server);
	honey_xwayland_setup(&server);

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
	setenv("XDG_CURRENT_DESKTOP", "honey", 0);
	setenv("XDG_SESSION_DESKTOP", "honey", 0);
	setenv("XDG_SESSION_TYPE", "wayland", 0);
	LOG("running on WAYLAND_DISPLAY=%s", socket);

	honey_ipc_setup(&server);
	honey_config_run(&server);

	/* Load a cursor image up front; without this the cursor is invisible
	 * until the first motion event sets one. */
	wlr_cursor_set_xcursor(server.cursor, server.xcursor_manager, "default");

	wl_display_run(server.display);

	wl_display_destroy_clients(server.display);

	/* wlroots asserts that no listeners remain on its globals at destroy;
	 * unhook everything registered on managers that live until then. */
	wl_list_remove(&server.xwayland_ready.link);
	wl_list_remove(&server.new_xwayland_surface.link);
	wl_list_remove(&server.new_layer_surface.link);
	wl_list_remove(&server.new_output.link);
	wl_list_remove(&server.output_manager_apply.link);
	wl_list_remove(&server.output_manager_test.link);
	wl_list_remove(&server.output_layout_change.link);
	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);
	wl_list_remove(&server.new_toplevel_decoration.link);
	wl_list_remove(&server.ext_workspace_commit.link);
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.request_cursor.link);
	wl_list_remove(&server.request_set_selection.link);
	wl_list_remove(&server.request_start_drag.link);
	wl_list_remove(&server.start_drag.link);
	wl_list_remove(&server.new_constraint.link);
	wl_list_remove(&server.constraint_destroy.link);
	wl_list_remove(&server.request_cursor_shape.link);
	wl_list_remove(&server.new_virtual_keyboard.link);
	wl_list_remove(&server.new_virtual_pointer.link);
	wl_list_remove(&server.request_activate.link);
	wl_list_remove(&server.new_idle_inhibitor.link);
	wl_list_remove(&server.new_shortcuts_inhibitor.link);

	/* Tear the backend down while the display's protocol globals are still
	 * alive: output destruction frees each output's ext-workspace handles,
	 * which belong to a manager global that wl_display_destroy would otherwise
	 * free first (leaving the per-output destroy to touch freed memory). */
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.display);
	return 0;
}
