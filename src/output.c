/* Output management.
 *
 * Each wlr_output is enabled at its preferred mode, added to the output layout,
 * and given a wlr_scene_output that composites the scene graph onto it every
 * frame.
 */
#include <stdlib.h>
#include <time.h>

#include "w3ld.h"

/* ------------------------------------------------------------------- listeners */

static void output_frame (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *scene_output =
		wlr_scene_get_scene_output(output->server->scene, output->wlr_output);

	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_destroy (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_output *output = wl_container_of(listener, output, destroy);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

/* --------------------------------------------------------------- new outputs */

static void handle_new_output (
	struct wl_listener *listener,
	void *data
) {
	struct w3ld_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode)
		wlr_output_state_set_mode(&state, mode);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct w3ld_output *output = calloc(1, sizeof *output);
	output->server = server;
	output->wlr_output = wlr_output;

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *layout_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, layout_output,
			scene_output);

	LOG("new output %s", wlr_output->name);
}

/* -------------------------------------------------------------------- setup */

void w3ld_output_setup (struct w3ld_server *server) {
	wl_list_init(&server->outputs);
	server->new_output.notify = handle_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
}
