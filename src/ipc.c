/* Control socket and config execution.
 *
 * A unix socket at $XDG_RUNTIME_DIR/w3ld-$WAYLAND_DISPLAY.sock accepts one
 * command per connection from w3ldctl, integrated into the Wayland event loop.
 * The command grammar is `map`/`unmap`/`ping`/`exit` plus any action name as a
 * direct verb. The config init (a shell script of w3ldctl calls) is run at
 * startup; without one, a default set of bindings is loaded.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "w3ld.h"

/* ------------------------------------------------------------------ dispatch */

static void dispatch (
	struct w3ld_server *server,
	char *line,
	char *reply,
	size_t reply_size
) {
	if (!strncmp(line, "map ", 4)) {
		char *rest = line + 4;
		while (*rest == ' ')
			rest++;
		char *space = strchr(rest, ' ');
		if (!space) {
			snprintf(reply, reply_size, "error: usage: map <combo> <action>");
			return;
		}
		*space = '\0';
		char *action = space + 1;
		while (*action == ' ')
			action++;
		if (w3ld_binding_add(server, rest, action))
			snprintf(reply, reply_size, "ok");
		else
			snprintf(reply, reply_size, "error: bad combo '%s'", rest);
		return;
	}

	if (!strncmp(line, "unmap ", 6)) {
		char *combo = line + 6;
		while (*combo == ' ')
			combo++;
		if (w3ld_binding_remove(server, combo))
			snprintf(reply, reply_size, "ok");
		else
			snprintf(reply, reply_size, "error: not mapped");
		return;
	}

	if (!strcmp(line, "ping")) {
		snprintf(reply, reply_size, "pong");
		return;
	}
	if (!strcmp(line, "exit")) {
		wl_display_terminate(server->display);
		snprintf(reply, reply_size, "ok");
		return;
	}

	/* Any other line is treated as an action verb (spawn, workspace, ...). */
	w3ld_action_run(server, line);
	snprintf(reply, reply_size, "ok");
}

/* -------------------------------------------------------------------- client */

static void client_drop (struct w3ld_ipc_client *client) {
	wl_event_source_remove(client->source);
	close(client->fd);
	wl_list_remove(&client->link);
	free(client);
}

static int handle_client (
	int fd,
	uint32_t mask,
	void *data
) {
	struct w3ld_ipc_client *client = data;
	char buffer[1024];
	ssize_t count = recv(fd, buffer, sizeof buffer - 1, 0);
	if (count <= 0) {
		client_drop(client);
		return 0;
	}
	buffer[count] = '\0';
	char *newline = strchr(buffer, '\n');
	if (newline)
		*newline = '\0';

	char reply[512];
	dispatch(client->server, buffer, reply, sizeof reply);
	send(fd, reply, strlen(reply), MSG_NOSIGNAL);
	client_drop(client);
	return 0;
}

static int handle_listen (
	int fd,
	uint32_t mask,
	void *data
) {
	struct w3ld_server *server = data;
	int client_fd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (client_fd < 0)
		return 0;

	struct w3ld_ipc_client *client = calloc(1, sizeof *client);
	client->server = server;
	client->fd = client_fd;
	client->source = wl_event_loop_add_fd(server->event_loop, client_fd,
			WL_EVENT_READABLE, handle_client, client);
	wl_list_insert(&server->ipc_clients, &client->link);
	return 0;
}

/* -------------------------------------------------------------------- setup */

void w3ld_ipc_setup (struct w3ld_server *server) {
	wl_list_init(&server->ipc_clients);
	server->ipc_fd = -1;

	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	if (!runtime || !display) {
		LOG("no XDG_RUNTIME_DIR/WAYLAND_DISPLAY; control socket disabled");
		return;
	}
	snprintf(server->ipc_path, sizeof server->ipc_path, "%s/w3ld-%s.sock",
			runtime, display);

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		LOG("control socket: %s", strerror(errno));
		return;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	strncpy(addr.sun_path, server->ipc_path, sizeof addr.sun_path - 1);
	unlink(server->ipc_path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0
			|| listen(fd, 16) < 0) {
		LOG("control socket bind: %s", strerror(errno));
		close(fd);
		return;
	}

	server->ipc_fd = fd;
	server->ipc_source = wl_event_loop_add_fd(server->event_loop, fd,
			WL_EVENT_READABLE, handle_listen, server);
	LOG("control socket: %s", server->ipc_path);
}

/* ------------------------------------------------------------ config / init */

static void load_default_bindings (struct w3ld_server *server) {
	w3ld_binding_add(server, "super+Return", "spawn alacritty");
	w3ld_binding_add(server, "super+shift+q", "close");
	w3ld_binding_add(server, "super+j", "focus-next");
	w3ld_binding_add(server, "super+k", "focus-prev");
	w3ld_binding_add(server, "super+Tab", "workspace-back");
	w3ld_binding_add(server, "super+Left", "focus-dir left");
	w3ld_binding_add(server, "super+Right", "focus-dir right");
	w3ld_binding_add(server, "super+Up", "focus-dir up");
	w3ld_binding_add(server, "super+Down", "focus-dir down");
	w3ld_binding_add(server, "super+shift+Left", "move-to-output left");
	w3ld_binding_add(server, "super+shift+Right", "move-to-output right");
	w3ld_binding_add(server, "super+shift+e", "exit");

	for (int number = 1; number <= 9; number++) {
		char combo[32], action[32];
		snprintf(combo, sizeof combo, "super+%d", number);
		snprintf(action, sizeof action, "workspace %d", number);
		w3ld_binding_add(server, combo, action);
		snprintf(combo, sizeof combo, "super+shift+%d", number);
		snprintf(action, sizeof action, "move-to-workspace %d", number);
		w3ld_binding_add(server, combo, action);
	}
}

void w3ld_config_run (struct w3ld_server *server) {
	char path[512] = {0};
	const char *config_home = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (config_home && *config_home)
		snprintf(path, sizeof path, "%s/w3ld/init", config_home);
	else if (home)
		snprintf(path, sizeof path, "%s/.config/w3ld/init", home);

	if (path[0] && access(path, R_OK) == 0) {
		LOG("running config: %s", path);
		pid_t pid = fork();
		if (pid == 0) {
			setsid();
			execl("/bin/sh", "/bin/sh", path, (char *)NULL);
			_exit(127);
		}
		return;
	}

	LOG("no config init found; loading default bindings");
	load_default_bindings(server);
}
