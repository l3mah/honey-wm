/* Native output configuration.
 *
 * Implements the server side of wlr-output-management-v1 so external tools
 * (wlr-randr, kanshi) can set mode/scale/position/transform, plus xdg-output for
 * bars and screenshot tools. The current layout is advertised to clients on
 * every change; client apply/test requests are validated and committed.
 */
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>

#include "w3ld.h"

/* --------------------------------------------------------------------- helpers */

/* Refresh each output's usable box from the layout and re-tile. */
static void refresh_layout (struct w3ld_server *server) {
	struct w3ld_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		wlr_output_layout_get_box(server->output_layout, output->wlr_output,
				&output->usable);
	}
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

void w3ld_output_manager_setup (struct w3ld_server *server) {
	server->output_manager = wlr_output_manager_v1_create(server->display);
	wlr_xdg_output_manager_v1_create(server->display, server->output_layout);

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
