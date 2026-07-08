/* Status stream (schema v1).
 *
 * `subscribe` clients receive newline-delimited JSON: a full-snapshot line per
 * output for workspaces and the focused window, emitted on connect and whenever
 * that output's slice changes. Lines are diffed against the last sent so nothing
 * is emitted for unchanged state. Extra v1 keys (layout, count, names) are
 * additive — consumers ignore what they don't use.
 *
 * A global `gamma` event (temperature Kelvin, brightness percent) is sent on
 * connect and whenever gamma changes by any route, so a bar reflects the live
 * state however it was driven (scroll, hotkey, direct `honeyctl gamma`).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "honey.h"

/* ------------------------------------------------------------------- helpers */

static void json_escape (
	const char *in,
	char *out,
	size_t cap
) {
	size_t o = 0;
	for (const char *p = in; *p && o + 2 < cap; p++) {
		unsigned char c = *p;
		if (c == '"' || c == '\\') {
			out[o++] = '\\';
			out[o++] = c;
		} else if (c == '\n') {
			out[o++] = '\\';
			out[o++] = 'n';
		} else if (c >= 0x20) {
			out[o++] = c;
		}
	}
	out[o] = '\0';
}

static int count_windows (struct honey_workspace *workspace) {
	int count = 0;
	struct honey_window *window;
	wl_list_for_each(window, &workspace->output->server->windows, link) {
		if (window->mapped && window->workspace == workspace)
			count++;
	}
	return count;
}

/* --------------------------------------------------------------- formatting */

static void format_workspaces (
	struct honey_output *output,
	char *buffer,
	size_t cap
) {
	struct honey_server *server = output->server;
	int active = output->active ? output->active->number : 0;
	bool focused = server->focused_output == output;

	char occupied[256] = {0};
	char names[512] = {0};
	size_t oo = 0, no = 0;
	int active_count = 0;

	struct honey_workspace *workspace;
	wl_list_for_each(workspace, &output->workspaces, link) {
		int windows = count_windows(workspace);
		if (workspace == output->active)
			active_count = windows;
		if (windows > 0)
			oo += snprintf(occupied + oo, sizeof occupied - oo, "%s%d",
					oo ? "," : "", workspace->number);
		if (workspace->name) {
			char escaped[128];
			json_escape(workspace->name, escaped, sizeof escaped);
			no += snprintf(names + no, sizeof names - no, "%s\"%d\":\"%s\"",
					no ? "," : "", workspace->number, escaped);
		}
	}

	const struct honey_layout *layout = output->active && output->active->layout
			? output->active->layout : server->config.layout;
	snprintf(buffer, cap,
			"{\"ev\":\"workspaces\",\"v\":1,\"output\":\"%s\",\"active\":%d,"
			"\"occupied\":[%s],\"focused\":%s,\"layout\":\"%s\","
			"\"count\":%d,\"names\":{%s}}\n",
			output->wlr_output->name, active, occupied,
			focused ? "true" : "false", layout->name, active_count, names);
}

static void format_window (
	struct honey_output *output,
	char *buffer,
	size_t cap
) {
	struct honey_server *server = output->server;
	bool focused = server->focused_output == output;

	struct honey_window *window = NULL;
	if (focused && server->focused)
		window = server->focused;
	else if (output->active)
		window = honey_workspace_first_window(output->active);

	const char *app_id = window ? honey_window_app_id(window) : "";
	const char *title = window ? honey_window_title(window) : "";
	char escaped_app[256], escaped_title[512];
	json_escape(app_id, escaped_app, sizeof escaped_app);
	json_escape(title, escaped_title, sizeof escaped_title);

	snprintf(buffer, cap,
			"{\"ev\":\"window\",\"v\":1,\"output\":\"%s\",\"focused\":%s,"
			"\"app_id\":\"%s\",\"title\":\"%s\"}\n",
			output->wlr_output->name, focused ? "true" : "false",
			escaped_app, escaped_title);
}

/* Gamma is global (not per-output): one line reporting the current temperature
 * (Kelvin, 0 = off) and brightness (percent, matching the command interface). */
static void format_gamma (
	struct honey_server *server,
	char *buffer,
	size_t cap
) {
	snprintf(buffer, cap,
			"{\"ev\":\"gamma\",\"v\":1,\"temperature\":%d,\"brightness\":%d}\n",
			(int)(server->gamma_temperature + 0.5),
			(int)(server->gamma_brightness * 100.0 + 0.5));
}

/* -------------------------------------------------------------- broadcasting */

static void send_line (
	int fd,
	const char *line
) {
	send(fd, line, strlen(line), MSG_NOSIGNAL);
}

static void send_to_subscribers (
	struct honey_server *server,
	const char *line
) {
	struct honey_ipc_client *client;
	wl_list_for_each(client, &server->ipc_clients, link) {
		if (client->subscriber)
			send_line(client->fd, line);
	}
}

/* Whether any connected client is a status subscriber. */
static bool has_subscribers (struct honey_server *server) {
	struct honey_ipc_client *client;
	wl_list_for_each(client, &server->ipc_clients, link) {
		if (client->subscriber)
			return true;
	}
	return false;
}

void honey_status_broadcast (struct honey_server *server) {
	/* Zero-cost when nobody is listening: skip the per-arrange JSON format +
	 * diff entirely. The standard ext-workspace / foreign-toplevel protocols
	 * cover generic bars; this stream is an optional hook for custom tooling. */
	if (!has_subscribers(server))
		return;

	struct honey_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		char buffer[1024];

		format_workspaces(output, buffer, sizeof buffer);
		if (!output->status_workspaces
				|| strcmp(output->status_workspaces, buffer) != 0) {
			free(output->status_workspaces);
			output->status_workspaces = strdup(buffer);
			send_to_subscribers(server, buffer);
		}

		format_window(output, buffer, sizeof buffer);
		if (!output->status_window
				|| strcmp(output->status_window, buffer) != 0) {
			free(output->status_window);
			output->status_window = strdup(buffer);
			send_to_subscribers(server, buffer);
		}
	}
}

void honey_status_broadcast_gamma (struct honey_server *server) {
	if (!has_subscribers(server))
		return;
	char buffer[128];
	format_gamma(server, buffer, sizeof buffer);
	send_to_subscribers(server, buffer);
}

void honey_status_snapshot (
	struct honey_server *server,
	struct honey_ipc_client *client
) {
	struct honey_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		char buffer[1024];
		format_workspaces(output, buffer, sizeof buffer);
		send_line(client->fd, buffer);
		format_window(output, buffer, sizeof buffer);
		send_line(client->fd, buffer);
	}

	char gamma[128];
	format_gamma(server, gamma, sizeof gamma);
	send_line(client->fd, gamma);
}
