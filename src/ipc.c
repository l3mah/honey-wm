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

/* --------------------------------------------------------------------- rules */

/* Parse a float size token: "40%" (fraction of the output), "600p"/"600px"
 * (pixels), bare numbers as fraction (<=1) or percent (>1). */
static bool parse_dim (
	const char *token,
	double *fraction,
	int *pixels
) {
	char *end;
	double value = strtod(token, &end);
	if (end == token || value <= 0)
		return false;
	*fraction = 0;
	*pixels = 0;
	if (!strcmp(end, "%"))
		*fraction = value / 100.0;
	else if (!strcmp(end, "p") || !strcmp(end, "px"))
		*pixels = (int)value;
	else if (*end == '\0')
		*fraction = value > 1.0 ? value / 100.0 : value;
	else
		return false;
	return true;
}

static void free_rule (struct w3ld_rule *rule) {
	if (rule->regex)
		regfree(&rule->re);
	free(rule->pattern);
	free(rule->ws_addr);
	free(rule);
}

/* rule <app-id|title|initial-title>[-re] <pattern...> <action>
 * action (parsed off the end): workspace <output:N|N> | float [default|W H] |
 * tile | suppress-maximize | no-focus. Same field+pattern+action replaces. */
static void cmd_rule (
	struct w3ld_server *server,
	char *args,
	char *reply,
	size_t reply_size
) {
	char *tokens[32];
	int count = 0;
	char *save = NULL;
	for (char *token = strtok_r(args, " \t", &save);
			token && count < 32; token = strtok_r(NULL, " \t", &save))
		tokens[count++] = token;
	if (count < 3) {
		snprintf(reply, reply_size,
				"error: usage: rule <field>[-re] <pattern...> <action>");
		return;
	}

	enum w3ld_rule_field field;
	bool regex = false;
	char *field_token = tokens[0];
	size_t field_length = strlen(field_token);
	if (field_length > 3 && !strcmp(field_token + field_length - 3, "-re")) {
		regex = true;
		field_token[field_length - 3] = '\0';
	}
	if (!strcmp(field_token, "app-id"))
		field = W3LD_RULE_APP_ID;
	else if (!strcmp(field_token, "title"))
		field = W3LD_RULE_TITLE;
	else if (!strcmp(field_token, "initial-title"))
		field = W3LD_RULE_INITIAL_TITLE;
	else {
		snprintf(reply, reply_size,
				"error: field must be app-id, title or initial-title");
		return;
	}

	/* Parse the action off the end. */
	enum w3ld_rule_action action;
	char *ws_addr = NULL;
	double float_w = 0, float_h = 0;
	int float_w_px = 0, float_h_px = 0;
	int pattern_end; /* exclusive index into tokens */
	char *last = tokens[count - 1];

	if (!strcmp(last, "tile")) {
		action = W3LD_RULE_TILE;
		pattern_end = count - 1;
	} else if (!strcmp(last, "suppress-maximize")) {
		action = W3LD_RULE_SUPPRESS_MAXIMIZE;
		pattern_end = count - 1;
	} else if (!strcmp(last, "no-focus")) {
		action = W3LD_RULE_NO_FOCUS;
		pattern_end = count - 1;
	} else if (count >= 3 && !strcmp(tokens[count - 2], "workspace")) {
		action = W3LD_RULE_WORKSPACE;
		ws_addr = last;
		pattern_end = count - 2;
	} else if (!strcmp(last, "float")) {
		action = W3LD_RULE_FLOAT;
		pattern_end = count - 1;
	} else if (count >= 3 && !strcmp(tokens[count - 2], "float")
			&& !strcmp(last, "default")) {
		action = W3LD_RULE_FLOAT;
		pattern_end = count - 2;
	} else if (count >= 4 && !strcmp(tokens[count - 3], "float")) {
		action = W3LD_RULE_FLOAT;
		if (!parse_dim(tokens[count - 2], &float_w, &float_w_px)
				|| !parse_dim(last, &float_h, &float_h_px)) {
			snprintf(reply, reply_size, "error: bad float size");
			return;
		}
		pattern_end = count - 3;
	} else {
		snprintf(reply, reply_size, "error: action must be workspace <addr>,"
				" float [default|W H], tile, suppress-maximize or no-focus");
		return;
	}
	if (pattern_end < 2) {
		snprintf(reply, reply_size, "error: missing pattern");
		return;
	}

	/* Re-join the pattern tokens. */
	char pattern[256] = {0};
	size_t offset = 0;
	for (int i = 1; i < pattern_end; i++)
		offset += snprintf(pattern + offset, sizeof pattern - offset, "%s%s",
				i > 1 ? " " : "", tokens[i]);

	struct w3ld_rule *rule = calloc(1, sizeof *rule);
	rule->field = field;
	rule->regex = regex;
	rule->pattern = strdup(pattern);
	rule->action = action;
	rule->ws_addr = ws_addr ? strdup(ws_addr) : NULL;
	rule->float_w = float_w;
	rule->float_h = float_h;
	rule->float_w_px = float_w_px;
	rule->float_h_px = float_h_px;
	if (regex && regcomp(&rule->re, pattern,
			REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0) {
		free(rule->pattern);
		free(rule->ws_addr);
		free(rule);
		snprintf(reply, reply_size, "error: bad regex '%s'", pattern);
		return;
	}

	/* Replace an existing rule with the same field + pattern + action. */
	struct w3ld_rule *existing;
	wl_list_for_each(existing, &server->rules, link) {
		if (existing->field == rule->field && existing->action == rule->action
				&& existing->regex == rule->regex
				&& !strcmp(existing->pattern, rule->pattern)) {
			wl_list_remove(&existing->link);
			free_rule(existing);
			break;
		}
	}
	wl_list_insert(server->rules.prev, &rule->link);
	snprintf(reply, reply_size, "ok");
}

static void cmd_windows (
	struct w3ld_server *server,
	char *reply,
	size_t reply_size
) {
	size_t offset = 0;
	struct w3ld_window *window;
	wl_list_for_each(window, &server->windows, link) {
		if (!window->mapped)
			continue;
		offset += snprintf(reply + offset, reply_size - offset,
				"app-id=\"%s\" title=\"%s\" ws=%s:%d%s%s%s%s%s\n",
				w3ld_window_app_id(window), w3ld_window_title(window),
				window->workspace->output->wlr_output->name,
				window->workspace->number,
				window == server->focused ? " [focused]" : "",
				window->floating ? " [float]" : "",
				window->fullscreen ? " [fs]" : "",
				window->maximized ? " [max]" : "",
				window->fake_fullscreen ? " [fake-fs]" : "");
		if (offset >= reply_size - 1)
			break;
	}
	if (offset == 0)
		snprintf(reply, reply_size, "(no windows)");
}

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

	if (!strncmp(line, "set ", 4)) {
		char *rest = line + 4;
		while (*rest == ' ')
			rest++;
		char *space = strchr(rest, ' ');
		if (!space) {
			snprintf(reply, reply_size, "error: usage: set <key> <value>");
			return;
		}
		*space = '\0';
		char *value = space + 1;
		while (*value == ' ')
			value++;
		if (w3ld_config_set(server, rest, value))
			snprintf(reply, reply_size, "ok");
		else
			snprintf(reply, reply_size, "error: bad set '%s'", rest);
		return;
	}

	if (!strncmp(line, "kb-layout ", 10)) {
		char *save = NULL;
		char *layout = strtok_r(line + 10, " \t", &save);
		char *variant = strtok_r(NULL, " \t", &save);
		char *model = strtok_r(NULL, " \t", &save);
		char *options = strtok_r(NULL, " \t", &save);
		char *rules = strtok_r(NULL, " \t", &save);
		if (!layout) {
			snprintf(reply, reply_size, "error: usage: kb-layout <layout>"
					" [variant] [model] [options] [rules]");
			return;
		}
		if (w3ld_kb_layout(server, layout, variant, model, options, rules))
			snprintf(reply, reply_size, "ok");
		else
			snprintf(reply, reply_size, "error: bad keyboard layout");
		return;
	}

	if (!strncmp(line, "kb-repeat ", 10)) {
		int rate, delay;
		if (sscanf(line + 10, "%d %d", &rate, &delay) != 2) {
			snprintf(reply, reply_size, "error: usage: kb-repeat <rate> <delay>");
			return;
		}
		if (w3ld_kb_repeat(server, rate, delay))
			snprintf(reply, reply_size, "ok");
		else
			snprintf(reply, reply_size, "error: bad repeat values");
		return;
	}

	if (!strncmp(line, "input ", 6)) {
		char *save = NULL;
		char *device = strtok_r(line + 6, " \t", &save);
		char *option = strtok_r(NULL, " \t", &save);
		char *value = strtok_r(NULL, " \t", &save);
		if (!device || !option || !value) {
			snprintf(reply, reply_size,
					"error: usage: input <device|*> <option> <value>");
			return;
		}
		if (w3ld_input_rule_add(server, device, option, value))
			snprintf(reply, reply_size, "ok");
		else
			snprintf(reply, reply_size, "error: unknown input option '%s'",
					option);
		return;
	}

	if (!strncmp(line, "set-ws ", 7)) {
		char *save = NULL;
		char *addr = strtok_r(line + 7, " \t", &save);
		char *key = strtok_r(NULL, " \t", &save);
		char *value = strtok_r(NULL, " \t", &save);
		if (!addr || !key || !value) {
			snprintf(reply, reply_size,
					"error: usage: set-ws <output:N|N> <key> <value>");
			return;
		}
		struct w3ld_workspace *workspace;
		if (!w3ld_parse_ws_addr(server, addr, &workspace)) {
			snprintf(reply, reply_size, "error: unknown workspace '%s'", addr);
			return;
		}
		if (!strcmp(key, "layout")) {
			const struct w3ld_layout *layout = w3ld_layout_by_name(value);
			if (!layout) {
				snprintf(reply, reply_size, "error: unknown layout '%s'",
						value);
				return;
			}
			workspace->layout = layout;
		} else if (!strcmp(key, "master-mfact")) {
			workspace->mfact = atof(value);
			workspace->has_mfact = true;
		} else if (!strcmp(key, "master-nmaster")) {
			workspace->nmaster = atoi(value);
			workspace->has_nmaster = true;
		} else if (!strcmp(key, "master-orientation")) {
			if (!w3ld_parse_orientation(value, &workspace->orientation)) {
				snprintf(reply, reply_size, "error: bad orientation '%s'",
						value);
				return;
			}
			workspace->has_orientation = true;
		} else {
			snprintf(reply, reply_size, "error: '%s' is not overridable"
					" per-workspace (layout, master-mfact, master-nmaster,"
					" master-orientation)", key);
			return;
		}
		w3ld_arrange(server);
		snprintf(reply, reply_size, "ok");
		return;
	}

	if (!strncmp(line, "workspace-name ", 15)) {
		char *rest = line + 15;
		while (*rest == ' ')
			rest++;
		int number = atoi(rest);
		if (number < 1 || !server->focused_output) {
			snprintf(reply, reply_size,
					"error: usage: workspace-name <N> [name]");
			return;
		}
		char *space = strchr(rest, ' ');
		char *name = space ? space + 1 : NULL;
		struct w3ld_workspace *workspace =
			w3ld_workspace_get(server->focused_output, number);
		free(workspace->name);
		workspace->name = (name && *name) ? strdup(name) : NULL;
		w3ld_status_broadcast(server);
		snprintf(reply, reply_size, "ok");
		return;
	}

	if (!strncmp(line, "output ", 7)) {
		char error[256];
		if (w3ld_output_command(server, line + 7, error, sizeof error))
			snprintf(reply, reply_size, "ok");
		else
			snprintf(reply, reply_size, "error: %s", error);
		return;
	}

	if (!strncmp(line, "gamma", 5) && (line[5] == ' ' || line[5] == '\0')) {
		char *arg = line + 5;
		while (*arg == ' ')
			arg++;
		if (!*arg || !strcmp(arg, "off") || !strcmp(arg, "reset")) {
			w3ld_gamma_set(server, 0, 1.0);
			snprintf(reply, reply_size, "ok");
			return;
		}
		double temperature = 0, brightness = 1.0;
		int fields = sscanf(arg, "%lf %lf", &temperature, &brightness);
		if (fields < 1 || temperature < 1000 || temperature > 10000) {
			snprintf(reply, reply_size,
					"error: usage: gamma <1000-10000> [brightness] | off");
			return;
		}
		w3ld_gamma_set(server, temperature, brightness);
		snprintf(reply, reply_size, "ok");
		return;
	}

	if (!strncmp(line, "rule ", 5)) {
		cmd_rule(server, line + 5, reply, reply_size);
		return;
	}
	if (!strcmp(line, "windows")) {
		cmd_windows(server, reply, reply_size);
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
	if (w3ld_action_run(server, line))
		snprintf(reply, reply_size, "ok");
	else
		snprintf(reply, reply_size, "error: unknown command '%s'", line);
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

	/* `subscribe` holds the connection open as a status-stream client. */
	if (!strcmp(buffer, "subscribe")) {
		client->subscriber = true;
		w3ld_status_snapshot(client->server, client);
		return 0;
	}

	char reply[4096];
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
	/* ipc_clients + ipc_fd are initialized in main() before backend start. */
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
	w3ld_binding_add(server, "super+shift+f", "toggle-float");
	w3ld_binding_add(server, "super+e", "fullscreen");
	w3ld_binding_add(server, "super+m", "maximize");
	w3ld_binding_add(server, "super+shift+Return", "swap-master");
	w3ld_binding_add(server, "super+shift+j", "swap-next");
	w3ld_binding_add(server, "super+shift+k", "swap-prev");
	w3ld_binding_add(server, "super+h", "master-mfact 0.05");
	w3ld_binding_add(server, "super+l", "master-mfact -0.05");
	w3ld_binding_add(server, "super+shift+Escape", "exit");

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
