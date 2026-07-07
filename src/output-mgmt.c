/* Native output configuration.
 *
 * Implements the server side of wlr-output-management-v1 so external tools
 * (wlr-randr, kanshi) can set mode/scale/position/transform, plus xdg-output for
 * bars and screenshot tools. The current layout is advertised to clients on
 * every change; client apply/test requests are validated and committed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>

#include "w3ld.h"

/* --------------------------------------------------------------------- helpers */

/* Refresh each output's usable box (full box minus layer-shell exclusive zones)
 * and re-tile. */
static void refresh_layout (struct w3ld_server *server) {
	w3ld_xdg_output_update(server);
	struct w3ld_output *output;
	wl_list_for_each(output, &server->outputs, link)
		w3ld_layer_arrange(output);
	w3ld_arrange(server);
}

/* Advertise the current output configuration to management clients. */
static void advertise_configuration (struct w3ld_server *server) {
	struct wlr_output_configuration_v1 *config =
		wlr_output_configuration_v1_create();

	struct w3ld_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output_configuration_head_v1 *head =
			wlr_output_configuration_head_v1_create(config, output->wlr_output);
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, output->wlr_output,
				&box);
		head->state.x = box.x;
		head->state.y = box.y;
	}

	wlr_output_manager_v1_set_configuration(server->output_manager, config);
}

/* Apply (or just test) a client-requested configuration. */
static bool apply_configuration (
	struct w3ld_server *server,
	struct wlr_output_configuration_v1 *config,
	bool test_only
) {
	struct wlr_output_configuration_head_v1 *head;

	wl_list_for_each(head, &config->heads, link) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_head_v1_state_apply(&head->state, &state);
		bool ok = wlr_output_test_state(head->state.output, &state);
		wlr_output_state_finish(&state);
		if (!ok)
			return false;
	}
	if (test_only)
		return true;

	wl_list_for_each(head, &config->heads, link) {
		struct wlr_output *wlr_output = head->state.output;
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_head_v1_state_apply(&head->state, &state);
		wlr_output_commit_state(wlr_output, &state);
		wlr_output_state_finish(&state);

		if (head->state.enabled) {
			wlr_output_layout_add(server->output_layout, wlr_output,
					head->state.x, head->state.y);
		} else {
			wlr_output_layout_remove(server->output_layout, wlr_output);
		}
	}

	refresh_layout(server);
	return true;
}

/* ----------------------------------------------------------------- listeners */

static void output_manager_apply (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, output_manager_apply);
	struct wlr_output_configuration_v1 *config = data;

	if (apply_configuration(server, config, false))
		wlr_output_configuration_v1_send_succeeded(config);
	else
		wlr_output_configuration_v1_send_failed(config);
	wlr_output_configuration_v1_destroy(config);

	advertise_configuration(server);
}

static void output_manager_test (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, output_manager_test);
	struct wlr_output_configuration_v1 *config = data;

	if (apply_configuration(server, config, true))
		wlr_output_configuration_v1_send_succeeded(config);
	else
		wlr_output_configuration_v1_send_failed(config);
	wlr_output_configuration_v1_destroy(config);
}

static void output_layout_change (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server =
		wl_container_of(listener, server, output_layout_change);
	refresh_layout(server);
	advertise_configuration(server);
}

/* -------------------------------------------------------------------- setup */

/* ---------------------------------------------------------- w3ldctl command */

static struct w3ld_output *output_by_name (
	struct w3ld_server *server,
	const char *name
) {
	struct w3ld_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (!strcmp(output->wlr_output->name, name))
			return output;
	}
	return NULL;
}

/* `output <name> [mode WxH[@R]] [scale F] [pos X,Y] [transform 0-7]
 *  [adaptive-sync on|off] [on|off]` — apply native output config. */
bool w3ld_output_command (
	struct w3ld_server *server,
	char *args,
	char *error,
	size_t error_size
) {
	char *save = NULL;
	char *name = strtok_r(args, " \t", &save);
	if (!name) {
		snprintf(error, error_size, "usage: output <name> [mode WxH[@R]]"
				" [scale F] [pos X,Y] [transform 0-7] [on|off]");
		return false;
	}
	struct w3ld_output *output = output_by_name(server, name);
	if (!output) {
		snprintf(error, error_size, "unknown output '%s'", name);
		return false;
	}
	struct wlr_output *wlr_output = output->wlr_output;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	int pos_x = 0, pos_y = 0;
	bool set_pos = false;
	bool enabled = true;

	char *key;
	while ((key = strtok_r(NULL, " \t", &save))) {
		if (!strcmp(key, "off")) {
			wlr_output_state_set_enabled(&state, false);
			enabled = false;
			continue;
		}
		if (!strcmp(key, "on")) {
			wlr_output_state_set_enabled(&state, true);
			continue;
		}
		char *value = strtok_r(NULL, " \t", &save);
		if (!value) {
			snprintf(error, error_size, "missing value for '%s'", key);
			wlr_output_state_finish(&state);
			return false;
		}
		if (!strcmp(key, "mode")) {
			int width, height, refresh = 0;
			if (sscanf(value, "%dx%d@%d", &width, &height, &refresh) < 2) {
				snprintf(error, error_size, "bad mode '%s'", value);
				wlr_output_state_finish(&state);
				return false;
			}
			struct wlr_output_mode *mode, *match = NULL;
			wl_list_for_each(mode, &wlr_output->modes, link) {
				if (mode->width == width && mode->height == height
						&& (refresh == 0 || mode->refresh / 1000 == refresh))
					match = mode;
			}
			if (match)
				wlr_output_state_set_mode(&state, match);
			else
				wlr_output_state_set_custom_mode(&state, width, height,
						refresh * 1000);
		} else if (!strcmp(key, "scale")) {
			wlr_output_state_set_scale(&state, atof(value));
		} else if (!strcmp(key, "pos") || !strcmp(key, "position")) {
			if (sscanf(value, "%d,%d", &pos_x, &pos_y) != 2) {
				snprintf(error, error_size, "bad position '%s'", value);
				wlr_output_state_finish(&state);
				return false;
			}
			set_pos = true;
		} else if (!strcmp(key, "transform")) {
			wlr_output_state_set_transform(&state, atoi(value));
		} else if (!strcmp(key, "adaptive-sync")) {
			wlr_output_state_set_adaptive_sync_enabled(&state,
					!strcasecmp(value, "on") || !strcasecmp(value, "true"));
		} else {
			snprintf(error, error_size, "unknown output key '%s'", key);
			wlr_output_state_finish(&state);
			return false;
		}
	}

	if (!wlr_output_commit_state(wlr_output, &state)) {
		snprintf(error, error_size, "commit failed");
		wlr_output_state_finish(&state);
		return false;
	}
	wlr_output_state_finish(&state);

	if (!enabled)
		wlr_output_layout_remove(server->output_layout, wlr_output);
	else if (set_pos)
		wlr_output_layout_add(server->output_layout, wlr_output, pos_x, pos_y);
	else
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	return true;
}

void w3ld_output_manager_setup (struct w3ld_server *server) {
	server->output_manager = wlr_output_manager_v1_create(server->display);
	w3ld_xdg_output_setup(server); /* xwayland-scale (removable): stock = wlr_xdg_output_manager_v1_create */

	server->output_manager_apply.notify = output_manager_apply;
	wl_signal_add(&server->output_manager->events.apply,
			&server->output_manager_apply);
	server->output_manager_test.notify = output_manager_test;
	wl_signal_add(&server->output_manager->events.test,
			&server->output_manager_test);

	server->output_layout_change.notify = output_layout_change;
	wl_signal_add(&server->output_layout->events.change,
			&server->output_layout_change);
}
